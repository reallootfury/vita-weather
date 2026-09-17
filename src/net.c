#include "net.h"
#include "map_math.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>
#include <curl/curl.h>
#include <png.h>
#include <psp2/appmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/location.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>

#define DATA_DIR "ux0:data/vita-weather"
#define CONFIG_PATH DATA_DIR "/config.txt"
#define CONFIG_TMP_PATH DATA_DIR "/config.tmp"
#define CACHE_PATH DATA_DIR "/forecast.json"
#define CACHE_TMP_PATH DATA_DIR "/forecast.tmp"
#define AIR_CACHE_PATH DATA_DIR "/air-quality.json"
#define AIR_CACHE_TMP_PATH DATA_DIR "/air-quality.tmp"
#define LOG_PATH DATA_DIR "/loader.log"
#define MAP_DIR DATA_DIR "/map"
#define HTTP_LIMIT (2u * 1024u * 1024u)

typedef enum WorkerJob {
    JOB_NONE = 0,
    JOB_REFRESH,
    JOB_SEARCH,
    JOB_SELECT,
    JOB_GPS
} WorkerJob;

typedef struct MemoryBuffer {
    char *data;
    size_t size;
    char content_type[64];
} MemoryBuffer;

static AppSnapshot g_snapshot;
static WeatherLocation g_location;
static SceKernelLwMutexWork g_state_mutex;
static int g_mutex_ready;
static SceUID g_worker = -1;
static volatile int g_worker_exit;
static WorkerJob g_job;
static char g_search_query[128];
static WeatherLocation g_job_location;
static int g_suggest_pending;
static char g_suggest_query[96];
static int g_map_pending;
static int g_map_zoom;
static int g_map_center_x;
static int g_map_center_y;
static int g_map_active_provider;
static int g_map_active_zoom;
static int g_map_active_base_x;
static int g_map_active_base_y;
static AppMapProvider g_map_provider;
static unsigned char g_net_memory[1024 * 1024] __attribute__((aligned(64)));
static int g_network_ready;

enum {
    MAP_STAGED_EMPTY = 0,
    MAP_STAGED_WRITING,
    MAP_STAGED_READY,
    MAP_STAGED_CACHED
};

typedef struct MapStagedTile {
    int state;
    AppMapProvider provider;
    int zoom;
    int tile_x;
    int tile_y;
    unsigned char pixels[MAP_TILE_ROW_BYTES * MAP_TILE_HEIGHT];
} MapStagedTile;

/* The worker owns file/network I/O and publishes a complete 5x3 window through
 * this bounded mailbox.  The render thread only performs a memory-to-texture
 * copy, avoiding the 50-320 ms synchronous file reads seen on physical Vita. */
static MapStagedTile g_map_staged[MAP_TILE_COUNT] __attribute__((aligned(64)));

typedef struct MapRequest {
    AppMapProvider provider;
    int zoom;
    int center_x;
    int center_y;
    int base_x;
    int base_y;
    int active_provider;
    int active_zoom;
    int active_base_x;
    int active_base_y;
} MapRequest;

static const MapRequest *g_transfer_map_request;
static int map_request_is_current(const MapRequest *request);

/* Vita's sceNetInetPton returns a negative SCE error for text that is not an
 * IP address. POSIX inet_pton must return zero in that case. The newlib shim
 * leaks the negative value, which curl interprets as "is an IP address" and
 * then omits TLS SNI. Normalize the result at the ABI boundary. */
int inet_pton(int address_family, const char *source, void *destination)
{
    if (address_family != AF_INET && address_family != AF_INET6)
        return -1;
    return sceNetInetPton(address_family, source, destination) > 0 ? 1 : 0;
}

static void state_lock(void)
{
    if (g_mutex_ready)
        sceKernelLockLwMutex(&g_state_mutex, 1, NULL);
}

static void state_unlock(void)
{
    if (g_mutex_ready)
        sceKernelUnlockLwMutex(&g_state_mutex, 1);
}

void app_log(const char *format, ...)
{
    FILE *file = fopen(LOG_PATH, "a");
    if (!file) return;
    fprintf(file, "[%7.3f] ", sceKernelGetProcessTimeWide() / 1000000.0);
    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    fputc('\n', file);
    fclose(file);
}

static void apply_location(WeatherData *weather, const WeatherLocation *location)
{
    snprintf(weather->location, sizeof(weather->location), "%s", location->name);
    snprintf(weather->region, sizeof(weather->region), "%s", location->region);
    if (location->timezone[0])
        snprintf(weather->timezone, sizeof(weather->timezone), "%s", location->timezone);
    weather->latitude = location->latitude;
    weather->longitude = location->longitude;
}

static void location_from_weather(WeatherLocation *location,
                                  const WeatherData *weather)
{
    memset(location, 0, sizeof(*location));
    snprintf(location->name, sizeof(location->name), "%s", weather->location);
    snprintf(location->region, sizeof(location->region), "%s", weather->region);
    snprintf(location->timezone, sizeof(location->timezone), "%s", weather->timezone);
    location->latitude = weather->latitude;
    location->longitude = weather->longitude;
}

static int write_file_atomic(const char *temporary, const char *path,
                             const void *data, size_t size)
{
    FILE *file = fopen(temporary, "wb");
    if (!file) return -1;
    int ok = fwrite(data, 1, size, file) == size && fflush(file) == 0;
    fclose(file);
    if (!ok) return -1;
    remove(path);
    return rename(temporary, path);
}

static char *read_file(const char *path, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fclose(file);
        return NULL;
    }
    long length = ftell(file);
    if (length < 1 || (unsigned long)length > HTTP_LIMIT || fseek(file, 0, SEEK_SET)) {
        fclose(file);
        return NULL;
    }
    char *data = malloc((size_t)length + 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    size_t read = fread(data, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) {
        free(data);
        return NULL;
    }
    data[read] = '\0';
    if (size_out) *size_out = read;
    return data;
}

static int same_location(const WeatherLocation *a, const WeatherLocation *b)
{
    return fabs(a->latitude - b->latitude) < 0.0001 &&
           fabs(a->longitude - b->longitude) < 0.0001;
}

static int add_saved_location_locked(const WeatherLocation *location)
{
    for (int i = 0; i < g_snapshot.saved_location_count; ++i) {
        if (same_location(&g_snapshot.saved_locations[i], location) ||
            (!strcmp(location->name, "Current Location") &&
             !strcmp(g_snapshot.saved_locations[i].name, "Current Location"))) {
            g_snapshot.saved_locations[i] = *location;
            g_snapshot.selected_location = i;
            return i;
        }
    }
    int index;
    if (g_snapshot.saved_location_count < APP_SAVED_LOCATIONS) {
        index = g_snapshot.saved_location_count++;
    } else {
        index = APP_SAVED_LOCATIONS - 1;
    }
    g_snapshot.saved_locations[index] = *location;
    g_snapshot.selected_location = index;
    return index;
}

static void config_append(char *config, size_t capacity, size_t *at,
                          const char *format, ...)
{
    if (*at >= capacity) return;
    va_list args;
    va_start(args, format);
    int written = vsnprintf(config + *at, capacity - *at, format, args);
    va_end(args);
    if (written > 0 && (size_t)written < capacity - *at)
        *at += (size_t)written;
    else
        *at = capacity;
}

static void save_config_locked(void)
{
    char config[4096];
    size_t at = 0;
    config_append(config, sizeof(config), &at,
                  "name=%s\nregion=%s\ntimezone=%s\nlatitude=%.7f\n"
                  "longitude=%.7f\nunit=%c\ntime24=%d\nlanguage=%d\n"
                  "theme=%d\nphoto_background=%d\nmotion=%d\nweather_sounds=%d\n"
                  "map_layer=%d\nmap_provider=%d\nauto_gps=%d\nselected=%d\n",
                  g_location.name, g_location.region, g_location.timezone,
                  g_location.latitude, g_location.longitude,
                  g_snapshot.use_fahrenheit ? 'F' : 'C',
                  g_snapshot.use_24_hour, (int)g_snapshot.language,
                  (int)g_snapshot.theme, g_snapshot.photo_background,
                  (int)g_snapshot.motion, g_snapshot.weather_sounds,
                  (int)g_snapshot.map_layer, (int)g_snapshot.map_provider,
                  g_snapshot.auto_gps, g_snapshot.selected_location);
    for (int i = 0; i < g_snapshot.saved_location_count; ++i) {
        const WeatherLocation *location = &g_snapshot.saved_locations[i];
        config_append(config, sizeof(config), &at,
                      "saved%d=%s|%s|%s|%.7f|%.7f\n", i,
                      location->name, location->region, location->timezone,
                      location->latitude, location->longitude);
    }
    if (at > 0 && at < sizeof(config))
        write_file_atomic(CONFIG_TMP_PATH, CONFIG_PATH, config, at);
}

static void trim_line(char *line)
{
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\r' || line[length - 1] == '\n' ||
                      line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';
}

static int load_config(void)
{
    FILE *file = fopen(CONFIG_PATH, "r");
    if (!file) return -1;
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        trim_line(line);
        char *equals = strchr(line, '=');
        if (!equals) continue;
        *equals++ = '\0';
        if (!strcmp(line, "name"))
            snprintf(g_location.name, sizeof(g_location.name), "%s", equals);
        else if (!strcmp(line, "region"))
            snprintf(g_location.region, sizeof(g_location.region), "%s", equals);
        else if (!strcmp(line, "timezone"))
            snprintf(g_location.timezone, sizeof(g_location.timezone), "%s", equals);
        else if (!strcmp(line, "latitude"))
            g_location.latitude = strtod(equals, NULL);
        else if (!strcmp(line, "longitude"))
            g_location.longitude = strtod(equals, NULL);
        else if (!strcmp(line, "unit"))
            g_snapshot.use_fahrenheit = equals[0] == 'F';
        else if (!strcmp(line, "time24"))
            g_snapshot.use_24_hour = atoi(equals) != 0;
        else if (!strcmp(line, "language")) {
            int value = atoi(equals);
            if (value >= 0 && value < APP_LANGUAGE_COUNT)
                g_snapshot.language = (AppLanguage)value;
        } else if (!strcmp(line, "theme")) {
            int value = atoi(equals);
            if (value >= 0 && value < APP_THEME_COUNT)
                g_snapshot.theme = (AppTheme)value;
        } else if (!strcmp(line, "photo_background")) {
            g_snapshot.photo_background = atoi(equals) != 0;
        } else if (!strcmp(line, "motion")) {
            int value = atoi(equals);
            if (value >= 0 && value < APP_MOTION_COUNT)
                g_snapshot.motion = (AppMotion)value;
        } else if (!strcmp(line, "weather_sounds")) {
            g_snapshot.weather_sounds = atoi(equals) != 0;
        } else if (!strcmp(line, "map_layer")) {
            int value = atoi(equals);
            if (value >= 0 && value < APP_MAP_LAYER_COUNT)
                g_snapshot.map_layer = (AppMapLayer)value;
        } else if (!strcmp(line, "map_provider")) {
            int value = atoi(equals);
            if (value >= 0 && value < APP_MAP_PROVIDER_COUNT)
                g_snapshot.map_provider = (AppMapProvider)value;
        } else if (!strcmp(line, "auto_gps"))
            g_snapshot.auto_gps = atoi(equals) != 0;
        else if (!strcmp(line, "selected"))
            g_snapshot.selected_location = atoi(equals);
        else if (!strncmp(line, "saved", 5)) {
            int index = atoi(line + 5);
            if (index < 0 || index >= APP_SAVED_LOCATIONS) continue;
            char *parts[5] = {equals, NULL, NULL, NULL, NULL};
            int valid = 1;
            for (int i = 1; i < 5; ++i) {
                char *separator = strchr(parts[i - 1], '|');
                if (!separator) {
                    valid = 0;
                    break;
                }
                *separator = '\0';
                parts[i] = separator + 1;
            }
            if (valid) {
                WeatherLocation *saved = &g_snapshot.saved_locations[index];
                memset(saved, 0, sizeof(*saved));
                snprintf(saved->name, sizeof(saved->name), "%s", parts[0]);
                snprintf(saved->region, sizeof(saved->region), "%s", parts[1]);
                snprintf(saved->timezone, sizeof(saved->timezone), "%s", parts[2]);
                saved->latitude = strtod(parts[3], NULL);
                saved->longitude = strtod(parts[4], NULL);
                if (saved->name[0] && index >= g_snapshot.saved_location_count)
                    g_snapshot.saved_location_count = index + 1;
            }
        }
    }
    fclose(file);
    return g_location.name[0] ? 0 : -1;
}

int app_load_initial_state(void)
{
    WeatherData demo;
    weather_load_demo(&demo);
    state_lock();
    g_snapshot.weather = demo;
    g_snapshot.use_fahrenheit = 1;
    g_snapshot.use_24_hour = 0;
    g_snapshot.language = APP_LANGUAGE_ENGLISH;
    g_snapshot.theme = APP_THEME_AUTO;
    g_snapshot.photo_background = 1;
    g_snapshot.motion = APP_MOTION_FULL;
    g_snapshot.weather_sounds = 0;
    g_snapshot.map_layer = APP_MAP_TEMPERATURE;
    g_snapshot.map_provider = APP_MAP_STANDARD;
    g_snapshot.busy = 0;
    snprintf(g_snapshot.activity, sizeof(g_snapshot.activity), "Ready");
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
             "Offline sample - updating automatically");
    location_from_weather(&g_location, &demo);
    load_config();
    if (g_snapshot.selected_location < 0 ||
        g_snapshot.selected_location >= g_snapshot.saved_location_count)
        g_snapshot.selected_location = 0;
    add_saved_location_locked(&g_location);
    apply_location(&g_snapshot.weather, &g_location);
    state_unlock();

    size_t cache_size = 0;
    char *cache = read_file(CACHE_PATH, &cache_size);
    if (!cache) return 0;
    WeatherData parsed;
    char error[96];
    int result = weather_parse_forecast(cache, cache_size, &parsed,
                                        error, sizeof(error));
    free(cache);
    if (result < 0) {
        app_log("cache rejected: %s", error);
        return 0;
    }
    size_t air_cache_size = 0;
    char *air_cache = read_file(AIR_CACHE_PATH, &air_cache_size);
    if (air_cache) {
        if (weather_parse_air_quality(air_cache, air_cache_size, &parsed,
                                      error, sizeof(error)) < 0)
            app_log("air-quality cache rejected: %s", error);
        else
            app_log("air-quality cache loaded: aqi=%.0f", parsed.air_quality);
        free(air_cache);
    }
    state_lock();
    apply_location(&parsed, &g_location);
    parsed.source = WEATHER_SOURCE_CACHE;
    g_snapshot.weather = parsed;
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "Showing saved forecast");
    state_unlock();
    app_log("cache loaded: location='%s' hours=%d first='%s' last='%s' days=%d",
            parsed.location, parsed.hour_count, parsed.hours[0].time,
            parsed.hours[parsed.hour_count - 1].time, parsed.day_count);
    return 1;
}

static size_t write_callback(void *contents, size_t size, size_t members,
                             void *user_data)
{
    MemoryBuffer *buffer = user_data;
    size_t add = size * members;
    if (add > HTTP_LIMIT || buffer->size > HTTP_LIMIT - add)
        return 0;
    char *grown = realloc(buffer->data, buffer->size + add + 1);
    if (!grown) return 0;
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, contents, add);
    buffer->size += add;
    buffer->data[buffer->size] = '\0';
    return add;
}

static int transfer_progress(void *user_data, curl_off_t download_total,
                             curl_off_t download_now, curl_off_t upload_total,
                             curl_off_t upload_now)
{
    (void)user_data;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    if (g_worker_exit) return 1;
    if (g_transfer_map_request) {
        state_lock();
        int current = map_request_is_current(g_transfer_map_request);
        int foreground_pending = g_job != JOB_NONE || g_suggest_pending;
        state_unlock();
        if (!current || foreground_pending) return 1;
    }
    return 0;
}

static int network_init(void)
{
    if (g_network_ready) return 0;
    int module = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (module < 0)
        app_log("SCE_SYSMODULE_NET load returned 0x%08x", module);
    if (sceNetShowNetstat() == (int)SCE_NET_ERROR_ENOTINIT) {
        SceNetInitParam parameters = {
            .memory = g_net_memory,
            .size = sizeof(g_net_memory),
            .flags = 0
        };
        int result = sceNetInit(&parameters);
        if (result < 0) {
            app_log("sceNetInit failed: 0x%08x", result);
            return result;
        }
    }
    int netctl = sceNetCtlInit();
    if (netctl < 0)
        app_log("sceNetCtlInit returned 0x%08x", netctl);
    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK) {
        app_log("curl_global_init failed: %d", result);
        return -(int)result;
    }
    curl_version_info_data *version = curl_version_info(CURLVERSION_NOW);
    app_log("network ready: curl=%s ssl=%s epoch=%lld", version->version,
            version->ssl_version ? version->ssl_version : "none",
            (long long)time(NULL));
    g_network_ready = 1;
    return 0;
}

static int http_get_with_handle(CURL *reusable, const char *url,
                                const char *accept, const char *user_agent,
                                const char *ca_file, MemoryBuffer *buffer,
                                long *status, char *error,
                                size_t error_size)
{
    memset(buffer, 0, sizeof(*buffer));
    *status = 0;
    if (network_init() < 0) {
        snprintf(error, error_size, "Network initialization failed");
        return -1;
    }
    CURL *curl = reusable ? reusable : curl_easy_init();
    if (!curl) {
        snprintf(error, error_size, "Unable to create HTTPS request");
        return -1;
    }
    if (reusable) curl_easy_reset(curl);
    struct curl_slist *headers = NULL;
    char curl_error[CURL_ERROR_SIZE] = "";
    char accept_header[80];
    snprintf(accept_header, sizeof(accept_header), "Accept: %s", accept);
    headers = curl_slist_append(headers, accept_header);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 18L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 128L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buffer);
    CURLcode result = curl_easy_perform(curl);
    long verify_result = 0;
    char *content_type = NULL;
    curl_easy_getinfo(curl, CURLINFO_SSL_VERIFYRESULT, &verify_result);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    if (content_type)
        snprintf(buffer->content_type, sizeof(buffer->content_type), "%s",
                 content_type);
    if (result != CURLE_OK) {
        if (result != CURLE_ABORTED_BY_CALLBACK)
            app_log("HTTPS failure: curl=%d verify=%ld http=%ld bytes=%u detail='%.160s'",
                    result, verify_result, *status, (unsigned int)buffer->size,
                    curl_error[0] ? curl_error : curl_easy_strerror(result));
        snprintf(error, error_size, "HTTPS error: %.72s", curl_easy_strerror(result));
    } else if (*status != 200) {
        snprintf(error, error_size, "Weather service returned HTTP %ld", *status);
    }
    curl_slist_free_all(headers);
    if (!reusable) curl_easy_cleanup(curl);
    if (result != CURLE_OK || *status != 200) {
        free(buffer->data);
        memset(buffer, 0, sizeof(*buffer));
        return -1;
    }
    return 0;
}

static int http_get(const char *url, const char *accept,
                    const char *user_agent, const char *ca_file,
                    MemoryBuffer *buffer, long *status,
                    char *error, size_t error_size)
{
    return http_get_with_handle(NULL, url, accept, user_agent, ca_file,
                                buffer, status, error, error_size);
}

static int http_get_json(const char *url, MemoryBuffer *buffer, long *status,
                         char *error, size_t error_size)
{
    return http_get(url, "application/json",
                    "VitaWeather/1.27 (PS Vita; native weather client)",
                    "app0:isrg-root-x1.pem",
                    buffer, status, error, error_size);
}

static void log_json_parse_failure(const char *endpoint,
                                   const MemoryBuffer *response,
                                   long status, const char *error)
{
    size_t first = 0;
    while (first < response->size &&
           isspace((unsigned char)response->data[first]))
        ++first;
    unsigned int first_byte = first < response->size
                                  ? (unsigned char)response->data[first]
                                  : 0u;
    app_log("JSON parse failed: endpoint=%s http=%ld content-type='%s' bytes=%u first=0x%02x error='%s'",
            endpoint, status,
            response->content_type[0] ? response->content_type : "unknown",
            (unsigned int)response->size, first_byte,
            error && error[0] ? error : "unknown");
}

static void url_encode(const char *input, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t at = 0;
    for (const unsigned char *p = (const unsigned char *)input; *p && at + 1 < out_size; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[at++] = (char)*p;
        } else if (at + 3 < out_size) {
            out[at++] = '%';
            out[at++] = hex[*p >> 4];
            out[at++] = hex[*p & 15];
        } else {
            break;
        }
    }
    out[at] = '\0';
}

static const char *geocode_language(void)
{
    static const char *const languages[] = {"en", "es", "fr", "de"};
    AppLanguage language;
    state_lock();
    language = g_snapshot.language;
    state_unlock();
    if ((unsigned int)language >= APP_LANGUAGE_COUNT)
        language = APP_LANGUAGE_ENGLISH;
    return languages[language];
}

static int fetch_locations(const char *query, WeatherLocation *locations,
                           int capacity, char *error, size_t error_size)
{
    char encoded[384];
    char url[640];
    url_encode(query, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=%s&format=json",
             encoded, capacity, geocode_language());
    MemoryBuffer response;
    long status;
    if (http_get_json(url, &response, &status, error, error_size) < 0)
        return -1;
    int result = weather_parse_geocodes(response.data, response.size, locations,
                                        capacity, error, error_size);
    if (result < 0)
        log_json_parse_failure("geocoding", &response, status, error);
    free(response.data);
    return result;
}

static int fetch_location(const char *query, WeatherLocation *location,
                          char *error, size_t error_size)
{
    int count = fetch_locations(query, location, 1, error, error_size);
    return count > 0 ? 0 : -1;
}

static int fetch_forecast(const WeatherLocation *location, WeatherData *weather,
                          char **raw_json, size_t *raw_size,
                          char *error, size_t error_size)
{
    char url[1800];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%.7f&longitude=%.7f"
        "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,is_day,wind_speed_10m,wind_direction_10m,surface_pressure,visibility,cloud_cover"
        "&hourly=temperature_2m,apparent_temperature,relative_humidity_2m,dew_point_2m,surface_pressure,visibility,uv_index,weather_code,precipitation_probability,precipitation,wind_speed_10m,wind_gusts_10m,wind_direction_10m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum,sunrise,sunset,uv_index_max"
        "&temperature_unit=celsius&wind_speed_unit=kmh&timezone=auto&forecast_days=10",
        location->latitude, location->longitude);
    MemoryBuffer response;
    long status;
    if (http_get_json(url, &response, &status, error, error_size) < 0)
        return -1;
    int result = weather_parse_forecast(response.data, response.size, weather,
                                        error, error_size);
    if (result < 0) {
        log_json_parse_failure("forecast", &response, status, error);
        free(response.data);
        return -1;
    }
    char air_url[640];
    snprintf(air_url, sizeof(air_url),
             "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.7f&longitude=%.7f"
             "&current=us_aqi,pm2_5,pm10&hourly=us_aqi&timezone=auto&forecast_days=1",
             location->latitude, location->longitude);
    MemoryBuffer air_response;
    long air_status;
    char air_error[96] = "";
    if (http_get_json(air_url, &air_response, &air_status,
                      air_error, sizeof(air_error)) == 0) {
        if (weather_parse_air_quality(air_response.data, air_response.size,
                                      weather, air_error,
                                      sizeof(air_error)) < 0) {
            log_json_parse_failure("air-quality", &air_response, air_status,
                                   air_error);
            app_log("air quality response ignored: %s", air_error);
        } else if (write_file_atomic(AIR_CACHE_TMP_PATH, AIR_CACHE_PATH,
                                   air_response.data,
                                   air_response.size) < 0)
            app_log("warning: failed to persist air-quality cache");
        free(air_response.data);
    } else {
        app_log("air quality unavailable: %s", air_error);
    }
    apply_location(weather, location);
    weather->source = WEATHER_SOURCE_LIVE;
    *raw_json = response.data;
    *raw_size = response.size;
    return 0;
}

static const char *map_provider_slug(AppMapProvider provider)
{
    switch (provider) {
    case APP_MAP_TOPOGRAPHIC: return "topo";
    case APP_MAP_CYCLOSM: return "cyclosm";
    case APP_MAP_OSM_FRANCE: return "osmfr";
    case APP_MAP_HUMANITARIAN: return "hot";
    default: return "standard";
    }
}

int app_map_provider_max_zoom(AppMapProvider provider)
{
    switch (provider) {
    case APP_MAP_TOPOGRAPHIC: return 17;
    case APP_MAP_HUMANITARIAN: return 18;
    case APP_MAP_CYCLOSM:
    case APP_MAP_OSM_FRANCE: return 20;
    default: return 19;
    }
}

const char *app_map_provider_name(AppMapProvider provider)
{
    switch (provider) {
    case APP_MAP_TOPOGRAPHIC: return "OpenTopoMap";
    case APP_MAP_CYCLOSM: return "CyclOSM";
    case APP_MAP_OSM_FRANCE: return "OSM France";
    case APP_MAP_HUMANITARIAN: return "Humanitarian";
    default: return "OSM Standard";
    }
}

const char *app_map_provider_attribution(AppMapProvider provider)
{
    switch (provider) {
    case APP_MAP_TOPOGRAPHIC:
        return "OpenTopoMap | (c) OpenStreetMap contributors";
    case APP_MAP_CYCLOSM:
        return "CyclOSM | (c) OpenStreetMap contributors";
    case APP_MAP_OSM_FRANCE:
        return "OSM France | (c) OpenStreetMap contributors";
    case APP_MAP_HUMANITARIAN:
        return "Humanitarian | (c) OpenStreetMap contributors";
    default:
        return "(c) OpenStreetMap contributors";
    }
}

void app_map_tile_path(char *out, size_t out_size, AppMapProvider provider,
                       int zoom,
                       int tile_x, int tile_y)
{
    snprintf(out, out_size, MAP_DIR "/%s-%d-%d-%d.png",
             map_provider_slug(provider), zoom,
             map_wrap_x(tile_x, zoom), map_clamp_y(tile_y, zoom));
}

void app_map_tile_raw_path(char *out, size_t out_size, AppMapProvider provider,
                           int zoom,
                           int tile_x, int tile_y)
{
    /* Version the decoded sidecar independently from the provider PNG. R20
     * uses byte-ordered RGBA for deterministic ABGR uploads on physical GXM;
     * the previous RGB565 files produced channel swaps and wedge corruption. */
    snprintf(out, out_size, MAP_DIR "/%s-%d-%d-%d.rgba8",
             map_provider_slug(provider), zoom,
             map_wrap_x(tile_x, zoom), map_clamp_y(tile_y, zoom));
}

static int map_tile_valid(const char *path)
{
    static const unsigned char signature[8] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
    };
    unsigned char header[sizeof(signature)];
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    size_t read = fread(header, 1, sizeof(header), file);
    fclose(file);
    return read == sizeof(header) && !memcmp(header, signature, sizeof(header));
}

static int map_raw_tile_valid(const char *path)
{
    enum { RAW_TILE_BYTES = MAP_TILE_WIDTH * MAP_TILE_HEIGHT * 4 };
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    int valid = !fseek(file, 0, SEEK_END) && ftell(file) == RAW_TILE_BYTES;
    fclose(file);
    return valid;
}

static int stage_map_cached_tile(const MapRequest *request, int target_slot,
                                 int zoom, int tile_x, int tile_y)
{
    if (target_slot < 0 || target_slot >= MAP_TILE_COUNT) return -1;
    tile_x = map_wrap_x(tile_x, zoom);
    tile_y = map_clamp_y(tile_y, zoom);
    int source_slot = -1;
    state_lock();
    if (!map_request_is_current(request)) {
        state_unlock();
        return -1;
    }
    for (int i = 0; i < MAP_TILE_COUNT; ++i) {
        const MapStagedTile *tile = &g_map_staged[i];
        if (tile->state == MAP_STAGED_CACHED &&
            tile->provider == request->provider && tile->zoom == zoom &&
            tile->tile_x == tile_x && tile->tile_y == tile_y) {
            source_slot = i;
            break;
        }
    }
    if (source_slot < 0) {
        state_unlock();
        return -1;
    }
    MapStagedTile *target = &g_map_staged[target_slot];
    if (source_slot == target_slot) {
        target->state = MAP_STAGED_READY;
        state_unlock();
        return 0;
    }
    target->state = MAP_STAGED_WRITING;
    target->provider = request->provider;
    target->zoom = zoom;
    target->tile_x = tile_x;
    target->tile_y = tile_y;
    state_unlock();

    /* Keep the cache bounded to the mailbox's existing 3.75 MiB. A worker-side
     * memory copy is far cheaper than the ~80 ms ux0: read measured per tile. */
    memcpy(target->pixels, g_map_staged[source_slot].pixels,
           sizeof(target->pixels));

    state_lock();
    int current = map_request_is_current(request) &&
                  target->provider == request->provider &&
                  target->zoom == zoom && target->tile_x == tile_x &&
                  target->tile_y == tile_y;
    target->state = current ? MAP_STAGED_READY : MAP_STAGED_EMPTY;
    state_unlock();
    return current ? 0 : -1;
}

static int stage_map_png_tile(const MapRequest *request, int slot,
                              int zoom, int tile_x, int tile_y,
                              const char *png_path, const char *raw_path)
{
    enum { WIDTH = 256, HEIGHT = 256, RAW_TILE_BYTES = WIDTH * HEIGHT * 4 };
    if (slot < 0 || slot >= MAP_TILE_COUNT) return -1;
    MapStagedTile *staged = &g_map_staged[slot];
    tile_x = map_wrap_x(tile_x, zoom);
    tile_y = map_clamp_y(tile_y, zoom);
    state_lock();
    if (!map_request_is_current(request)) {
        state_unlock();
        return -1;
    }
    staged->state = MAP_STAGED_WRITING;
    staged->provider = request->provider;
    staged->zoom = zoom;
    staged->tile_x = tile_x;
    staged->tile_y = tile_y;
    state_unlock();

    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    int valid = png_image_begin_read_from_file(&image, png_path);
    if (valid && (image.width != WIDTH || image.height != HEIGHT)) valid = 0;
    if (valid) {
        image.format = PNG_FORMAT_RGBA;
        valid = png_image_finish_read(&image, NULL, staged->pixels, 0, NULL);
    }
    if (valid) {
        char temporary[144];
        snprintf(temporary, sizeof(temporary), "%s.tmp", raw_path);
        if (write_file_atomic(temporary, raw_path, staged->pixels,
                              RAW_TILE_BYTES) < 0)
            app_log("map raw cache write failed: '%s'", raw_path);
    }
    png_image_free(&image);

    state_lock();
    int current = map_request_is_current(request) &&
                  staged->provider == request->provider &&
                  staged->zoom == zoom && staged->tile_x == tile_x &&
                  staged->tile_y == tile_y;
    staged->state = valid && current ? MAP_STAGED_READY : MAP_STAGED_EMPTY;
    state_unlock();
    return valid && current ? 0 : -1;
}

static int stage_map_raw_tile(const MapRequest *request, int slot,
                              int zoom, int tile_x, int tile_y,
                              const char *raw_path)
{
    if (slot < 0 || slot >= MAP_TILE_COUNT) return -1;
    MapStagedTile *staged = &g_map_staged[slot];
    tile_x = map_wrap_x(tile_x, zoom);
    tile_y = map_clamp_y(tile_y, zoom);
    state_lock();
    if (!map_request_is_current(request)) {
        state_unlock();
        return -1;
    }
    staged->state = MAP_STAGED_WRITING;
    staged->provider = request->provider;
    staged->zoom = zoom;
    staged->tile_x = tile_x;
    staged->tile_y = tile_y;
    state_unlock();

    FILE *file = fopen(raw_path, "rb");
    int valid = file != NULL;
    if (valid)
        valid = fread(staged->pixels, MAP_TILE_ROW_BYTES,
                      MAP_TILE_HEIGHT, file) == MAP_TILE_HEIGHT;
    if (valid) valid = fgetc(file) == EOF;
    if (file) fclose(file);

    state_lock();
    int current = map_request_is_current(request) &&
                  staged->provider == request->provider &&
                  staged->zoom == zoom && staged->tile_x == tile_x &&
                  staged->tile_y == tile_y;
    staged->state = valid && current ? MAP_STAGED_READY : MAP_STAGED_EMPTY;
    state_unlock();
    return valid && current ? 0 : -1;
}

static int map_request_is_current(const MapRequest *request)
{
    return g_snapshot.map.provider == request->provider &&
           g_snapshot.map.zoom == request->zoom &&
           g_snapshot.map.center_x == request->center_x &&
           g_snapshot.map.center_y == request->center_y;
}

static int map_request_is_current_safe(const MapRequest *request)
{
    state_lock();
    int current = map_request_is_current(request);
    state_unlock();
    return current;
}

static int foreground_job_pending_safe(void)
{
    state_lock();
    int pending = g_job != JOB_NONE || g_suggest_pending;
    state_unlock();
    return pending;
}

static void publish_map_progress(const MapRequest *request, int ready,
                                 const char *notice, int finished)
{
    state_lock();
    if (map_request_is_current(request)) {
        g_snapshot.map.ready_tiles = ready;
        g_snapshot.map.loading = !finished;
        ++g_snapshot.map.revision;
        snprintf(g_snapshot.map.notice, sizeof(g_snapshot.map.notice), "%s", notice);
    }
    state_unlock();
}

static int append_coordinate(char *out, size_t out_size, double value,
                             int first)
{
    size_t used = strlen(out);
    if (used >= out_size) return -1;
    int written = snprintf(out + used, out_size - used,
                           first ? "%.5f" : ",%.5f", value);
    return written > 0 && (size_t)written < out_size - used ? 0 : -1;
}

static void publish_map_field(const MapRequest *request,
                              const WeatherMapField *field,
                              const char *notice)
{
    state_lock();
    if (map_request_is_current(request)) {
        g_snapshot.map.field = *field;
        g_snapshot.map.field_zoom = request->zoom;
        g_snapshot.map.field_base_x = request->base_x;
        g_snapshot.map.field_base_y = request->base_y;
        g_snapshot.map.field_loading = 0;
        ++g_snapshot.map.field_revision;
        if (notice && notice[0])
            snprintf(g_snapshot.map.notice, sizeof(g_snapshot.map.notice),
                     "%s", notice);
    }
    state_unlock();
}

static void fetch_map_weather_field(const MapRequest *request)
{
    WeatherMapField empty_field;
    memset(&empty_field, 0, sizeof(empty_field));
    char latitudes[360] = "";
    char longitudes[360] = "";
    int point = 0;
    for (int row = 0; row < WEATHER_MAP_ROWS; ++row) {
        for (int column = 0; column < WEATHER_MAP_COLUMNS; ++column) {
            int tile_x = map_wrap_x(request->base_x + column,
                                    request->zoom);
            int tile_y = map_clamp_y(request->base_y + row,
                                     request->zoom);
            double longitude = map_tile_longitude(tile_x + 0.5,
                                                  request->zoom);
            double latitude = map_tile_latitude(tile_y + 0.5,
                                                request->zoom);
            if (append_coordinate(latitudes, sizeof(latitudes), latitude,
                                  point == 0) < 0 ||
                append_coordinate(longitudes, sizeof(longitudes), longitude,
                                  point == 0) < 0) {
                app_log("map field coordinate list overflow");
                publish_map_field(request, &empty_field,
                                  "Weather layer unavailable");
                return;
            }
            ++point;
        }
    }

    WeatherMapField field;
    memset(&field, 0, sizeof(field));
    if (network_init() < 0) {
        app_log("map field network unavailable");
        publish_map_field(request, &field, "Weather layer unavailable");
        return;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        app_log("map field HTTPS handle unavailable");
        publish_map_field(request, &field, "Weather layer unavailable");
        return;
    }
    uint64_t started_us = sceKernelGetProcessTimeWide();
    char url[1800];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
             "&hourly=temperature_2m,precipitation,wind_speed_10m,wind_direction_10m"
             "&wind_speed_unit=kmh&forecast_hours=24&timezone=GMT&timeformat=unixtime",
             latitudes, longitudes);
    MemoryBuffer response;
    long status = 0;
    char forecast_error[128] = "";
    g_transfer_map_request = request;
    int forecast_result = http_get_with_handle(
        curl, url, "application/json",
        "VitaWeather/1.27 (PS Vita homebrew weather map)",
        "app0:isrg-root-x1.pem", &response, &status, forecast_error,
        sizeof(forecast_error));
    g_transfer_map_request = NULL;
    if (!map_request_is_current_safe(request)) {
        free(response.data);
        curl_easy_cleanup(curl);
        app_log("map field cancelled after forecast: z=%d center=%d,%d",
                request->zoom, request->center_x, request->center_y);
        return;
    }
    if (forecast_result == 0) {
        forecast_result = weather_parse_map_forecast(
            response.data, response.size, &field, forecast_error,
            sizeof(forecast_error));
        if (forecast_result < 0)
            log_json_parse_failure("map-forecast", &response, status,
                                   forecast_error);
    }
    free(response.data);

    snprintf(url, sizeof(url),
             "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%s&longitude=%s"
             "&hourly=us_aqi&forecast_hours=24&timezone=GMT&timeformat=unixtime",
             latitudes, longitudes);
    MemoryBuffer air_response;
    long air_status = 0;
    char air_error[128] = "";
    g_transfer_map_request = request;
    int air_result = http_get_with_handle(
        curl, url, "application/json",
        "VitaWeather/1.27 (PS Vita homebrew weather map)",
        "app0:isrg-root-x1.pem", &air_response, &air_status, air_error,
        sizeof(air_error));
    g_transfer_map_request = NULL;
    if (!map_request_is_current_safe(request)) {
        free(air_response.data);
        curl_easy_cleanup(curl);
        app_log("map field cancelled after AQI: z=%d center=%d,%d",
                request->zoom, request->center_x, request->center_y);
        return;
    }
    if (air_result == 0) {
        air_result = weather_parse_map_air_quality(
            air_response.data, air_response.size, &field, air_error,
            sizeof(air_error));
        if (air_result < 0)
            log_json_parse_failure("map-air-quality", &air_response,
                                   air_status, air_error);
    }
    free(air_response.data);
    curl_easy_cleanup(curl);

    int forecast_points = 0;
    int aqi_points = 0;
    float minimum_aqi = 10000.0f;
    float maximum_aqi = -1.0f;
    for (int i = 0; i < field.point_count; ++i) {
        forecast_points += field.points[i].forecast_valid != 0;
        aqi_points += field.points[i].air_quality_valid != 0;
        if (field.points[i].air_quality_valid) {
            float value = field.points[i].air_quality[0];
            if (value < minimum_aqi) minimum_aqi = value;
            if (value > maximum_aqi) maximum_aqi = value;
        }
    }
    const char *notice = forecast_points || aqi_points
                             ? "Weather layers ready - Open-Meteo / CAMS"
                             : "Weather layers unavailable";
    publish_map_field(request, &field, notice);
    app_log("map field ready: z=%d base=%d,%d forecast=%d/%d aqi=%d/%d hours=%d aqi-range=%.0f..%.0f elapsed=%.1fms forecast='%s' aqi='%s'",
            request->zoom, request->base_x, request->base_y,
            forecast_points, WEATHER_MAP_POINTS, aqi_points,
            WEATHER_MAP_POINTS, field.hour_count,
            aqi_points ? minimum_aqi : 0.0f,
            aqi_points ? maximum_aqi : 0.0f,
            (sceKernelGetProcessTimeWide() - started_us) / 1000.0,
            forecast_result == 0 ? "ok" : forecast_error,
            air_result == 0 ? "ok" : air_error);
}

static void perform_map_job(const MapRequest *request)
{
    /* Center-first viewport order, followed by the visible neighbours and
     * finally the look-ahead edge.  A usable center tile can be presented
     * without waiting for the whole 5x3 cache window. */
    static const unsigned char tile_order[MAP_TILE_COUNT] = {
        7, 6, 8, 2, 12, 1, 3, 11, 13, 5, 9, 0, 4, 10, 14
    };
    int ready = 0;
    int failed = 0;
    int reused = 0;
    int memory_hits = 0;
    int raw_reads = 0;
    int decoded = 0;
    uint64_t raw_read_us = 0;
    uint64_t raw_read_max_us = 0;
    uint64_t decode_us = 0;
    uint64_t job_started_us = sceKernelGetProcessTimeWide();
    char error[128] = "";
    CURL *map_curl = NULL;
    for (int order = 0; order < MAP_TILE_COUNT && !g_worker_exit; ++order) {
            int tile_index = tile_order[order];
            int row = tile_index / MAP_TILE_COLUMNS;
            int column = tile_index % MAP_TILE_COLUMNS;
            if (!map_request_is_current_safe(request)) {
                app_log("map request superseded: zoom=%d center=%d,%d ready=%d",
                        request->zoom, request->center_x,
                        request->center_y, ready);
                if (map_curl) curl_easy_cleanup(map_curl);
                return;
            }
            int tile_x = map_wrap_x(request->base_x + column, request->zoom);
            int tile_y = map_clamp_y(request->base_y + row, request->zoom);
            int active_overlap = request->active_provider == request->provider &&
                                 request->active_zoom == request->zoom;
            if (active_overlap) {
                active_overlap = 0;
                for (int active_row = 0;
                     active_row < MAP_TILE_ROWS && !active_overlap;
                     ++active_row) {
                    int active_y = map_clamp_y(request->active_base_y +
                                               active_row, request->zoom);
                    if (active_y != tile_y) continue;
                    for (int active_column = 0;
                         active_column < MAP_TILE_COLUMNS; ++active_column) {
                        int active_x = map_wrap_x(request->active_base_x +
                                                 active_column,
                                                 request->zoom);
                        if (active_x == tile_x) {
                            active_overlap = 1;
                            break;
                        }
                    }
                }
            }
            if (active_overlap) {
                ++ready;
                ++reused;
                publish_map_progress(request, ready,
                                     "Reusing visible map tiles", 0);
                continue;
            }
            char path[128];
            char raw_path[128];
            error[0] = '\0';
            app_map_tile_path(path, sizeof(path), request->provider,
                              request->zoom, tile_x, tile_y);
            app_map_tile_raw_path(raw_path, sizeof(raw_path), request->provider,
                                  request->zoom,
                                  tile_x, tile_y);
            if (stage_map_cached_tile(request, tile_index, request->zoom,
                                      tile_x, tile_y) == 0) {
                ++ready;
                ++memory_hits;
                char progress[80];
                snprintf(progress, sizeof(progress), "Loading map %d/%d",
                         ready, MAP_TILE_COUNT);
                publish_map_progress(request, ready, progress, 0);
                continue;
            }
            int raw_valid = map_raw_tile_valid(raw_path);
            if (!raw_valid && !map_tile_valid(path)) {
                char url[192];
                char temporary[136];
                const char *url_base;
                switch (request->provider) {
                case APP_MAP_TOPOGRAPHIC:
                    url_base = "https://a.tile.opentopomap.org";
                    break;
                case APP_MAP_CYCLOSM:
                    url_base = "https://a.tile-cyclosm.openstreetmap.fr/cyclosm";
                    break;
                case APP_MAP_OSM_FRANCE:
                    url_base = "https://a.tile.openstreetmap.fr/osmfr";
                    break;
                case APP_MAP_HUMANITARIAN:
                    url_base = "https://a.tile.openstreetmap.fr/hot";
                    break;
                default:
                    url_base = "https://tile.openstreetmap.org";
                    break;
                }
                snprintf(url, sizeof(url), "%s/%d/%d/%d.png", url_base,
                         request->zoom, tile_x, tile_y);
                snprintf(temporary, sizeof(temporary), "%s.tmp", path);
                MemoryBuffer tile;
                long status;
                if (!map_curl) {
                    if (network_init() >= 0) map_curl = curl_easy_init();
                    if (!map_curl) {
                        ++failed;
                        app_log("map HTTPS handle unavailable");
                        continue;
                    }
                }
                g_transfer_map_request = request;
                int request_result = http_get_with_handle(
                    map_curl, url, "image/png",
                    "VitaWeather/1.27 (PS Vita homebrew map)",
                    request->provider == APP_MAP_STANDARD
                        ? "app0:globalsign-root-r3.pem"
                        : "app0:isrg-root-x1.pem",
                    &tile, &status, error, sizeof(error));
                g_transfer_map_request = NULL;
                if (request_result < 0 &&
                    !map_request_is_current_safe(request)) {
                    free(tile.data);
                    app_log("map transfer cancelled: zoom=%d center=%d,%d",
                            request->zoom, request->center_x,
                            request->center_y);
                    curl_easy_cleanup(map_curl);
                    return;
                }
                if (request_result < 0 ||
                    tile.size < 8 || memcmp(tile.data, "\x89PNG\r\n\x1a\n", 8)) {
                    free(tile.data);
                    ++failed;
                    app_log("map tile failed: z=%d x=%d y=%d error='%s'",
                            request->zoom, tile_x, tile_y, error);
                    continue;
                }
                if (write_file_atomic(temporary, path, tile.data, tile.size) < 0) {
                    ++failed;
                    app_log("map tile cache write failed: '%s'", path);
                    free(tile.data);
                    continue;
                }
                free(tile.data);
            }
            uint64_t stage_started_us = sceKernelGetProcessTimeWide();
            int stage_result;
            if (raw_valid) {
                stage_result = stage_map_raw_tile(
                    request, tile_index, request->zoom, tile_x, tile_y,
                    raw_path);
                uint64_t elapsed = sceKernelGetProcessTimeWide() -
                                   stage_started_us;
                ++raw_reads;
                raw_read_us += elapsed;
                if (elapsed > raw_read_max_us) raw_read_max_us = elapsed;
            } else {
                stage_result = stage_map_png_tile(
                    request, tile_index, request->zoom, tile_x, tile_y,
                    path, raw_path);
                ++decoded;
                decode_us += sceKernelGetProcessTimeWide() - stage_started_us;
            }
            if (stage_result < 0) {
                if (!map_request_is_current_safe(request)) {
                    if (map_curl) curl_easy_cleanup(map_curl);
                    return;
                }
                ++failed;
                app_log("map tile staging failed: '%s'", path);
                continue;
            }
            ++ready;
            char progress[80];
            snprintf(progress, sizeof(progress), "Loading map %d/%d",
                     ready, MAP_TILE_COUNT);
            publish_map_progress(request, ready, progress, 0);
    }

    if (map_curl) curl_easy_cleanup(map_curl);

    char notice[80];
    if (ready == MAP_TILE_COUNT)
        snprintf(notice, sizeof(notice), "Map ready - cached for repeat visits");
    else
        snprintf(notice, sizeof(notice), "%d tiles ready, %d unavailable",
                 ready, failed);
    publish_map_progress(request, ready, notice, 1);
    app_log("map ready: zoom=%d center=%d,%d tiles=%d gpu-reused=%d memory-hits=%d raw-reads=%d raw-total=%.1fms raw-max=%.1fms decoded=%d decode-total=%.1fms failed=%d elapsed=%.1fms",
            request->zoom, request->center_x, request->center_y, ready,
            reused, memory_hits, raw_reads, raw_read_us / 1000.0,
            raw_read_max_us / 1000.0, decoded, decode_us / 1000.0, failed,
            (sceKernelGetProcessTimeWide() - job_started_us) / 1000.0);
    if (ready > 0 && map_request_is_current_safe(request)) {
        /* Give held-stick movement time to queue its newest raster before
         * issuing lower-priority weather requests on the single worker. */
        int settled = 1;
        for (int wait = 0; wait < 13 && !g_worker_exit; ++wait) {
            sceKernelDelayThread(50 * 1000);
            if (!map_request_is_current_safe(request) ||
                foreground_job_pending_safe()) {
                settled = 0;
                break;
            }
        }
        if (settled && !foreground_job_pending_safe()) {
            fetch_map_weather_field(request);
        } else {
            WeatherMapField empty_field;
            memset(&empty_field, 0, sizeof(empty_field));
            publish_map_field(request, &empty_field,
                              "Weather layer deferred for location update");
            app_log("map field deferred: camera or foreground work pending");
        }
    }
}

static void publish_failure(const char *error)
{
    state_lock();
    g_snapshot.busy = g_job != JOB_NONE;
    if (g_snapshot.weather.source != WEATHER_SOURCE_DEMO) {
        /* Retain the last successful WeatherData and make its offline
         * provenance explicit until a later refresh succeeds. */
        g_snapshot.weather.source = WEATHER_SOURCE_CACHE;
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                 "Offline - saved forecast");
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                 "Offline: %.118s", error);
    } else {
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                 "Update failed");
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "%s", error);
    }
    state_unlock();
    app_log("request failed: %s", error);
}

static int location_fix_valid(const SceLocationLocationInfo *info)
{
    return info && isfinite(info->latitude) && isfinite(info->longitude) &&
           info->latitude >= -90.0 && info->latitude <= 90.0 &&
           info->longitude >= -180.0 && info->longitude <= 180.0 &&
           info->latitude != SCE_LOCATION_DATA_INVALID &&
           info->longitude != SCE_LOCATION_DATA_INVALID;
}

static unsigned int read_le16(const unsigned char *data)
{
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8);
}

static unsigned int read_le32(const unsigned char *data)
{
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8) |
           ((unsigned int)data[2] << 16) | ((unsigned int)data[3] << 24);
}

static void log_location_sfo(void)
{
    size_t size = 0;
    unsigned char *sfo = (unsigned char *)read_file(
        "app0:sce_sys/param.sfo", &size);
    if (!sfo) {
        app_log("GPS SFO: unable to read app0:sce_sys/param.sfo");
        return;
    }
    if (size < 20 || read_le32(sfo) != 0x46535000u) {
        app_log("GPS SFO: invalid header size=%u magic=0x%08x",
                (unsigned int)size, size >= 4 ? read_le32(sfo) : 0u);
        free(sfo);
        return;
    }
    unsigned int keys = read_le32(sfo + 8);
    unsigned int data = read_le32(sfo + 12);
    unsigned int entries = read_le32(sfo + 16);
    unsigned int attribute = 0;
    int attribute_found = 0;
    char title_id[20] = "";
    if (entries > 128) entries = 128;
    for (unsigned int i = 0; i < entries; ++i) {
        size_t entry_at = 20u + i * 16u;
        if (entry_at + 16u > size) break;
        unsigned int key_offset = read_le16(sfo + entry_at);
        unsigned int value_length = read_le32(sfo + entry_at + 4);
        unsigned int value_offset = read_le32(sfo + entry_at + 12);
        if ((size_t)keys + key_offset >= size ||
            (size_t)data + value_offset >= size)
            continue;
        const char *key = (const char *)sfo + keys + key_offset;
        size_t remaining = size - ((size_t)keys + key_offset);
        if (!memchr(key, '\0', remaining)) continue;
        const unsigned char *value = sfo + data + value_offset;
        size_t available = size - ((size_t)data + value_offset);
        if (!strcmp(key, "ATTRIBUTE") && available >= 4) {
            attribute = read_le32(value);
            attribute_found = 1;
        } else if (!strcmp(key, "TITLE_ID") && value_length > 0) {
            size_t copy = value_length;
            if (copy >= sizeof(title_id)) copy = sizeof(title_id) - 1;
            if (copy > available) copy = available;
            memcpy(title_id, value, copy);
            title_id[copy] = '\0';
        }
    }
    app_log("GPS SFO: size=%u entries=%u title='%s' ATTRIBUTE=%s0x%08x location-bit=%s",
            (unsigned int)size, entries, title_id[0] ? title_id : "?",
            attribute_found ? "" : "missing/", attribute,
            attribute_found && (attribute & 2u) ? "set" : "NOT SET");
    free(sfo);
}

static const char *location_error_name(unsigned int result)
{
    switch (result) {
    case SCE_LOCATION_ERROR_INVALID_ADDRESS: return "INVALID_ADDRESS";
    case SCE_LOCATION_ERROR_INVALID_HANDLE: return "INVALID_HANDLE";
    case SCE_LOCATION_ERROR_NO_MEMORY: return "NO_MEMORY";
    case SCE_LOCATION_ERROR_TOO_MANY_HANDLES: return "TOO_MANY_HANDLES";
    case SCE_LOCATION_ERROR_INVALID_LOCATION_METHOD:
        return "INVALID_LOCATION_METHOD";
    case SCE_LOCATION_ERROR_INVALID_HEADING_METHOD:
        return "INVALID_HEADING_METHOD";
    case SCE_LOCATION_ERROR_DISABLE_APPLICATION: return "DISABLE_APPLICATION";
    case SCE_LOCATION_ERROR_UNAUTHORIZED: return "UNAUTHORIZED";
    case SCE_LOCATION_ERROR_PROVIDER_UNAVAILABLE:
        return "PROVIDER_UNAVAILABLE";
    case SCE_LOCATION_ERROR_INVALID_TITLE_ID: return "INVALID_TITLE_ID";
    case SCE_LOCATION_ERROR_TIME_OUT: return "TIME_OUT";
    case SCE_LOCATION_ERROR_FATAL_ERROR: return "FATAL_ERROR";
    case 0x8010124fu: return "UNDOCUMENTED_OPEN_FAILURE_BEFORE_PERMISSION";
    default: return "unknown";
    }
}

static void log_location_environment(void)
{
    int pid = sceKernelGetCurrentProcess();
    int model = sceKernelGetModel();
    int dialog_model = sceKernelGetModelForCDialog();
    char title_id[20] = "";
    int title_result = sceAppMgrAppParamGetString(
        pid, 12, title_id, sizeof(title_id));
    SceKernelSystemSwVersion firmware;
    memset(&firmware, 0, sizeof(firmware));
    firmware.size = sizeof(firmware);
    int firmware_result = sceKernelGetSystemSwVersion(&firmware);
    app_log("GPS environment: pid=0x%08x model=%d/0x%x dialog-model=%d title-result=0x%08x title='%s' firmware-result=0x%08x firmware='%s' raw=0x%08x",
            pid, model, model, dialog_model, title_result,
            title_result >= 0 ? title_id : "?", firmware_result,
            firmware_result >= 0 ? firmware.versionString : "?",
            firmware_result >= 0 ? firmware.version : 0u);
    log_location_sfo();
}

static int obtain_gps_location(WeatherLocation *location,
                               char *error, size_t error_size)
{
    log_location_environment();
    int base_before = sceSysmoduleIsLoaded(SCE_SYSMODULE_LOCATION);
    int extension_before = sceSysmoduleIsLoaded(
        SCE_SYSMODULE_LOCATION_EXTENSION);
    int module = sceSysmoduleLoadModule(SCE_SYSMODULE_LOCATION);
    int extension = sceSysmoduleLoadModule(SCE_SYSMODULE_LOCATION_EXTENSION);
    int model = sceKernelGetModel();
    int base_after = sceSysmoduleIsLoaded(SCE_SYSMODULE_LOCATION);
    int extension_after = sceSysmoduleIsLoaded(
        SCE_SYSMODULE_LOCATION_EXTENSION);
    app_log("location module: base-before=0x%08x load=0x%08x after=0x%08x extension-before=0x%08x load=0x%08x after=0x%08x model=%d",
            base_before, module, base_after, extension_before, extension,
            extension_after, model);
    if (module < 0) {
        snprintf(error, error_size,
                 "Vita Location module failed to load (0x%08x)", module);
        return -1;
    }
    static const SceLocationLocationMethod methods[] = {
        SCE_LOCATION_LMETHOD_AGPS_AND_3G_AND_WIFI,
        SCE_LOCATION_LMETHOD_3G,
        SCE_LOCATION_LMETHOD_GPS_AND_WIFI,
        SCE_LOCATION_LMETHOD_GPS,
        SCE_LOCATION_LMETHOD_WIFI
    };
    static const char *const method_names[] = {
        "AGPS/3G/Wi-Fi", "3G", "GPS/Wi-Fi", "GPS", "Wi-Fi"
    };
    SceLocationHandle handle = 0;
    int result = -1;
    int method_index = 0;
    for (; method_index < (int)(sizeof(methods) / sizeof(methods[0]));
         ++method_index) {
        handle = 0;
        uint64_t open_started = sceKernelGetProcessTimeWide();
        result = sceLocationOpen(&handle, methods[method_index],
                                 SCE_LOCATION_HMETHOD_NONE);
        app_log("sceLocationOpen: attempt=%d/%d method=%s(%d) heading=NONE(%d) handle=%u result=0x%08x name=%s elapsed=%.2fms",
                method_index + 1,
                (int)(sizeof(methods) / sizeof(methods[0])),
                method_names[method_index], methods[method_index],
                SCE_LOCATION_HMETHOD_NONE, handle, result,
                result < 0 ? location_error_name((unsigned int)result) : "OK",
                (sceKernelGetProcessTimeWide() - open_started) / 1000.0);
        if (result >= 0) break;
    }
    if (result < 0) {
        if ((unsigned int)result == 0x8010124fu)
            snprintf(error, error_size,
                     "Native location failed before permission (0x%08x)",
                     (unsigned int)result);
        else
            snprintf(error, error_size,
                     "GPS providers are unavailable (0x%08x)", result);
        return -1;
    }

    SceLocationPermissionInfo permission;
    memset(&permission, 0, sizeof(permission));
    result = sceLocationGetPermission(handle, &permission);
    if (result < 0) {
        snprintf(error, error_size,
                 "Location permission check failed (0x%08x)", result);
        app_log("sceLocationGetPermission failed: 0x%08x", result);
        sceLocationClose(handle);
        return -1;
    }
    app_log("GPS permission: parental=%d system=%d application=%d",
            permission.parentalstatus, permission.mainstatus,
            permission.applicationstatus);
    if (permission.parentalstatus != SCE_LOCATION_PERMISSION_ALLOW) {
        snprintf(error, error_size,
                 "Location is blocked by parental controls");
        sceLocationClose(handle);
        return -1;
    }
    if (permission.mainstatus != SCE_LOCATION_PERMISSION_ALLOW) {
        snprintf(error, error_size,
                 "Enable Location Data in Vita System Settings");
        sceLocationClose(handle);
        return -1;
    }
    if (permission.applicationstatus !=
        SCE_LOCATION_PERMISSION_APPLICATION_ALLOW) {
        result = sceLocationConfirm(handle);
        if (result < 0) {
            snprintf(error, error_size,
                     "Location permission could not open (0x%08x)", result);
            sceLocationClose(handle);
            return -1;
        }
        uint64_t started = sceKernelGetProcessTimeWide();
        SceLocationDialogStatus status = SCE_LOCATION_DIALOG_STATUS_RUNNING;
        while (!g_worker_exit && status != SCE_LOCATION_DIALOG_STATUS_FINISHED &&
               sceKernelGetProcessTimeWide() - started < 45000000u) {
            sceKernelDelayThread(100 * 1000);
            sceLocationConfirmGetStatus(handle, &status);
        }
        SceLocationDialogResult dialog_result = SCE_LOCATION_DIALOG_RESULT_NONE;
        if (status != SCE_LOCATION_DIALOG_STATUS_FINISHED ||
            sceLocationConfirmGetResult(handle, &dialog_result) < 0 ||
            dialog_result != SCE_LOCATION_DIALOG_RESULT_ENABLE) {
            snprintf(error, error_size, "Location permission was not enabled");
            sceLocationClose(handle);
            return -1;
        }
        memset(&permission, 0, sizeof(permission));
        result = sceLocationGetPermission(handle, &permission);
        app_log("GPS permission after dialog: result=0x%08x parental=%d "
                "system=%d application=%d", result,
                permission.parentalstatus, permission.mainstatus,
                permission.applicationstatus);
        if (result < 0 ||
            permission.applicationstatus !=
                SCE_LOCATION_PERMISSION_APPLICATION_ALLOW) {
            snprintf(error, error_size,
                     "Location permission was not saved");
            sceLocationClose(handle);
            return -1;
        }
    }

    SceLocationLocationInfo info;
    uint64_t started = sceKernelGetProcessTimeWide();
    uint64_t last_progress = 0;
    int attempts = 0;
    result = -1;
    do {
        memset(&info, 0, sizeof(info));
        info.latitude = SCE_LOCATION_DATA_INVALID;
        info.longitude = SCE_LOCATION_DATA_INVALID;
        info.accuracy = SCE_LOCATION_DATA_INVALID;
        result = sceLocationGetLocation(handle, &info);
        ++attempts;
        if (result >= 0 && location_fix_valid(&info)) break;

        uint64_t elapsed = sceKernelGetProcessTimeWide() - started;
        if (!last_progress || elapsed - last_progress >= 5000000u) {
            app_log("GPS waiting: elapsed=%.1fs attempt=%d result=0x%08x",
                    elapsed / 1000000.0, attempts, result);
            state_lock();
            snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                     "Waiting for GPS fix... %.0f s", elapsed / 1000000.0);
            snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                     "Keep the Vita near a window or outdoors");
            state_unlock();
            last_progress = elapsed;
        }
        if (g_worker_exit || elapsed >= 30000000u) break;
        sceKernelDelayThread(500 * 1000);
    } while (!g_worker_exit);

    if (!location_fix_valid(&info)) sceLocationCancelGetLocation(handle);
    sceLocationClose(handle);
    if (result < 0 || !location_fix_valid(&info)) {
        snprintf(error, error_size,
                 "No GPS fix after 30 s (0x%08x); try outdoors", result);
        app_log("GPS fix failed: attempts=%d result=0x%08x lat=%.5f lon=%.5f",
                attempts, result, info.latitude, info.longitude);
        return -1;
    }
    memset(location, 0, sizeof(*location));
    snprintf(location->name, sizeof(location->name), "Current Location");
    snprintf(location->region, sizeof(location->region),
             "GPS fix - accuracy %.0f m", info.accuracy);
    location->latitude = info.latitude;
    location->longitude = info.longitude;
    app_log("GPS fix: attempts=%d lat=%.7f lon=%.7f accuracy=%.1f",
            attempts, info.latitude, info.longitude, info.accuracy);
    return 0;
}

static void perform_suggestion_job(const char *query)
{
    WeatherLocation results[APP_SEARCH_RESULTS];
    memset(results, 0, sizeof(results));
    char error[96] = "";
    int count = fetch_locations(query, results, APP_SEARCH_RESULTS,
                                error, sizeof(error));
    state_lock();
    if (!strcmp(query, g_suggest_query)) {
        g_snapshot.search_busy = 0;
        g_snapshot.search_result_count = count > 0 ? count : 0;
        if (count > 0)
            memcpy(g_snapshot.search_results, results,
                   sizeof(results[0]) * (size_t)count);
        snprintf(g_snapshot.search_notice, sizeof(g_snapshot.search_notice),
                 "%s", count > 0 ? "Choose a suggested location" : error);
    }
    state_unlock();
}

static void perform_job(WorkerJob job, const char *query,
                        WeatherLocation location)
{
    char error[128] = "Unknown weather error";
    int gps_fallback = 0;
    if (job == JOB_GPS) {
        state_lock();
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                 "Finding current location...");
        state_unlock();
        if (obtain_gps_location(&location, error, sizeof(error)) < 0) {
            app_log("GPS fallback: %s; refreshing saved location '%s'",
                    error, location.name);
            gps_fallback = 1;
            state_lock();
            snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                     "Updating saved location...");
            snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                     "GPS unavailable - using %s", location.name);
            snprintf(g_snapshot.gps_notice, sizeof(g_snapshot.gps_notice),
                     "%s", error);
            state_unlock();
        } else {
            state_lock();
            snprintf(g_snapshot.gps_notice, sizeof(g_snapshot.gps_notice),
                     "Native GPS fix acquired");
            state_unlock();
        }
    } else if (job == JOB_SEARCH) {
        state_lock();
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity), "Finding %s...", query);
        state_unlock();
        if (fetch_location(query, &location, error, sizeof(error)) < 0) {
            publish_failure(error);
            return;
        }
        state_lock();
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                 "Loading %s...", location.name);
        state_unlock();
    }

    WeatherData weather;
    char *raw = NULL;
    size_t raw_size = 0;
    if (fetch_forecast(&location, &weather, &raw, &raw_size,
                       error, sizeof(error)) < 0) {
        publish_failure(error);
        return;
    }
    if (write_file_atomic(CACHE_TMP_PATH, CACHE_PATH, raw, raw_size) < 0)
        app_log("warning: failed to persist forecast cache");
    free(raw);

    state_lock();
    g_location = location;
    add_saved_location_locked(&location);
    g_snapshot.weather = weather;
    if (job == JOB_GPS)
        g_snapshot.gps_active = !gps_fallback;
    else if (job == JOB_SEARCH || job == JOB_SELECT)
        g_snapshot.gps_active = 0;
    g_snapshot.busy = g_job != JOB_NONE;
    snprintf(g_snapshot.activity, sizeof(g_snapshot.activity), "Forecast updated");
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "%s",
             gps_fallback ? "GPS unavailable - saved location updated"
                          : "Live data from Open-Meteo");
    save_config_locked();
    state_unlock();
    app_log("forecast updated: location='%s' hours=%d first='%s' last='%s' days=%d",
            weather.location, weather.hour_count, weather.hours[0].time,
            weather.hours[weather.hour_count - 1].time, weather.day_count);
}

static int worker_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    while (!g_worker_exit) {
        WorkerJob job = JOB_NONE;
        int has_map_job = 0;
        int has_suggestion_job = 0;
        MapRequest map_request;
        memset(&map_request, 0, sizeof(map_request));
        char query[128] = "";
        char suggestion_query[96] = "";
        WeatherLocation location;
        memset(&location, 0, sizeof(location));
        state_lock();
        if (g_job != JOB_NONE) {
            job = g_job;
            g_job = JOB_NONE;
            snprintf(query, sizeof(query), "%s", g_search_query);
            location = job == JOB_SELECT ? g_job_location : g_location;
            g_snapshot.busy = 1;
        } else if (g_suggest_pending) {
            snprintf(suggestion_query, sizeof(suggestion_query), "%s",
                     g_suggest_query);
            g_suggest_pending = 0;
            has_suggestion_job = 1;
        } else if (g_map_pending) {
            map_request.provider = g_map_provider;
            map_request.zoom = g_map_zoom;
            map_request.center_x = g_map_center_x;
            map_request.center_y = g_map_center_y;
            map_request.base_x = g_map_center_x - MAP_TILE_COLUMNS / 2;
            map_request.base_y = g_map_center_y - MAP_TILE_ROWS / 2;
            map_request.active_provider = g_map_active_provider;
            map_request.active_zoom = g_map_active_zoom;
            map_request.active_base_x = g_map_active_base_x;
            map_request.active_base_y = g_map_active_base_y;
            g_map_pending = 0;
            has_map_job = 1;
        }
        state_unlock();
        if (job == JOB_NONE && !has_map_job && !has_suggestion_job) {
            sceKernelDelayThread(20 * 1000);
            continue;
        }
        if (has_suggestion_job) {
            perform_suggestion_job(suggestion_query);
        } else if (has_map_job) {
            perform_map_job(&map_request);
        } else {
            perform_job(job, query, location);
            state_lock();
            if (g_job == JOB_NONE)
                g_snapshot.busy = 0;
            state_unlock();
        }
    }
    return 0;
}

int app_services_init(void)
{
    sceIoMkdir(DATA_DIR, 0777);
    sceIoMkdir(MAP_DIR, 0777);
    FILE *log = fopen(LOG_PATH, "w");
    if (log) fclose(log);
    if (sceKernelCreateLwMutex(&g_state_mutex, "weather_state", 0, 0, NULL) < 0)
        return -1;
    g_mutex_ready = 1;
    app_load_initial_state();
    app_log("Vita Weather 1.27 start; data='%s'", DATA_DIR);
    g_worker = sceKernelCreateThread("weather_net", worker_main,
                                     0x10000100, 256 * 1024, 0, 0, NULL);
    if (g_worker < 0) {
        app_log("worker creation failed: 0x%08x", g_worker);
        return g_worker;
    }
    int result = sceKernelStartThread(g_worker, 0, NULL);
    if (result < 0) {
        app_log("worker start failed: 0x%08x", result);
        return result;
    }
    if (g_snapshot.auto_gps)
        app_queue_gps();
    else
        app_queue_refresh();
    return 0;
}

void app_services_shutdown(void)
{
    g_worker_exit = 1;
    if (g_worker >= 0) {
        sceKernelWaitThreadEnd(g_worker, NULL, NULL);
        sceKernelDeleteThread(g_worker);
        g_worker = -1;
    }
    if (g_network_ready) {
        curl_global_cleanup();
        sceNetCtlTerm();
        sceNetTerm();
        g_network_ready = 0;
    }
    if (g_mutex_ready) {
        sceKernelDeleteLwMutex(&g_state_mutex);
        g_mutex_ready = 0;
    }
}

void app_snapshot_read(AppSnapshot *out)
{
    state_lock();
    *out = g_snapshot;
    state_unlock();
}

void app_queue_refresh(void)
{
    state_lock();
    if (g_job == JOB_NONE && !g_snapshot.busy) {
        g_job = JOB_REFRESH;
        g_snapshot.busy = 1;
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity), "Updating forecast...");
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "Contacting Open-Meteo");
    } else {
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "An update is already queued");
    }
    state_unlock();
}

void app_queue_search(const char *query)
{
    if (!query || !query[0]) return;
    state_lock();
    snprintf(g_search_query, sizeof(g_search_query), "%s", query);
    g_job = JOB_SEARCH;
    g_snapshot.busy = 1;
    snprintf(g_snapshot.activity, sizeof(g_snapshot.activity), "City search queued");
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "Searching for %s", query);
    state_unlock();
    app_log("city search queued: '%s'", query);
}

void app_queue_suggestions(const char *query)
{
    if (!query) return;
    size_t length = strlen(query);
    state_lock();
    snprintf(g_snapshot.search_query, sizeof(g_snapshot.search_query), "%s", query);
    if (length < 2) {
        g_suggest_pending = 0;
        g_snapshot.search_busy = 0;
        g_snapshot.search_result_count = 0;
        snprintf(g_snapshot.search_notice, sizeof(g_snapshot.search_notice),
                 "Type at least two letters");
        state_unlock();
        return;
    }
    snprintf(g_suggest_query, sizeof(g_suggest_query), "%s", query);
    g_suggest_pending = 1;
    g_snapshot.search_busy = 1;
    g_snapshot.search_result_count = 0;
    snprintf(g_snapshot.search_notice, sizeof(g_snapshot.search_notice),
             "Finding nearby matches...");
    state_unlock();
}

static void queue_location_locked(const WeatherLocation *location,
                                  const char *activity)
{
    g_job_location = *location;
    g_job = JOB_SELECT;
    g_snapshot.busy = 1;
    g_snapshot.gps_active = 0;
    g_snapshot.search_busy = 0;
    snprintf(g_snapshot.activity, sizeof(g_snapshot.activity), "%s", activity);
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
             "Loading %s", location->name);
}

void app_select_suggestion(int index)
{
    state_lock();
    if (index >= 0 && index < g_snapshot.search_result_count) {
        queue_location_locked(&g_snapshot.search_results[index],
                              "Loading selected location...");
        app_log("suggestion selected: '%s'",
                g_snapshot.search_results[index].name);
    }
    state_unlock();
}

void app_select_saved_location(int index)
{
    state_lock();
    if (index >= 0 && index < g_snapshot.saved_location_count) {
        queue_location_locked(&g_snapshot.saved_locations[index],
                              "Loading saved location...");
    }
    state_unlock();
}

void app_remove_saved_location(int index)
{
    state_lock();
    if (g_snapshot.saved_location_count <= 1 || index < 0 ||
        index >= g_snapshot.saved_location_count) {
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                 "Keep at least one saved location");
        state_unlock();
        return;
    }
    if (index == g_snapshot.selected_location) {
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                 "Switch locations before removing this one");
        state_unlock();
        return;
    }
    for (int i = index; i + 1 < g_snapshot.saved_location_count; ++i)
        g_snapshot.saved_locations[i] = g_snapshot.saved_locations[i + 1];
    --g_snapshot.saved_location_count;
    if (g_snapshot.selected_location > index)
        --g_snapshot.selected_location;
    save_config_locked();
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "Location removed");
    state_unlock();
}

void app_queue_gps(void)
{
    int queued = 0;
    state_lock();
    if (g_job == JOB_NONE && !g_snapshot.busy) {
        g_job = JOB_GPS;
        g_snapshot.busy = 1;
        g_snapshot.gps_active = 0;
        snprintf(g_snapshot.activity, sizeof(g_snapshot.activity),
                 "Requesting GPS location...");
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                 "A location permission prompt may appear");
        queued = 1;
    } else {
        snprintf(g_snapshot.notice, sizeof(g_snapshot.notice),
                 "Wait for the current update to finish");
    }
    state_unlock();
    if (queued) app_log("GPS request queued");
}

void app_queue_map(int zoom, int center_x, int center_y, int active_provider,
                   int active_zoom, int active_base_x, int active_base_y)
{
    AppMapProvider provider;
    state_lock();
    provider = g_snapshot.map_provider;
    state_unlock();
    if (zoom < 3) zoom = 3;
    int max_zoom = app_map_provider_max_zoom(provider);
    if (zoom > max_zoom) zoom = max_zoom;
    center_x = map_wrap_x(center_x, zoom);
    center_y = map_clamp_y(center_y, zoom);

    state_lock();
    if (g_snapshot.map.provider == provider &&
        g_snapshot.map.zoom == zoom &&
        g_snapshot.map.center_x == center_x &&
        g_snapshot.map.center_y == center_y &&
        (g_snapshot.map.loading || g_snapshot.map.ready_tiles > 0)) {
        state_unlock();
        return;
    }
    g_map_provider = provider;
    g_map_zoom = zoom;
    g_map_center_x = center_x;
    g_map_center_y = center_y;
    g_map_active_provider = active_provider;
    g_map_active_zoom = active_zoom;
    g_map_active_base_x = active_base_x;
    g_map_active_base_y = active_base_y;
    g_map_pending = 1;
    g_snapshot.map.provider = provider;
    g_snapshot.map.zoom = zoom;
    g_snapshot.map.center_x = center_x;
    g_snapshot.map.center_y = center_y;
    g_snapshot.map.base_x = center_x - MAP_TILE_COLUMNS / 2;
    g_snapshot.map.base_y = center_y - MAP_TILE_ROWS / 2;
    g_snapshot.map.loading = 1;
    g_snapshot.map.ready_tiles = 0;
    memset(&g_snapshot.map.field, 0, sizeof(g_snapshot.map.field));
    g_snapshot.map.field_zoom = zoom;
    g_snapshot.map.field_base_x = g_snapshot.map.base_x;
    g_snapshot.map.field_base_y = g_snapshot.map.base_y;
    g_snapshot.map.field_loading = 1;
    ++g_snapshot.map.field_revision;
    ++g_snapshot.map.revision;
    snprintf(g_snapshot.map.notice, sizeof(g_snapshot.map.notice),
             "Loading nearby map tiles");
    state_unlock();
    app_log("map queued: provider=%s zoom=%d center=%d,%d",
            map_provider_slug(provider), zoom, center_x, center_y);
}

int app_map_tile_staged(AppMapProvider provider, int zoom,
                        int tile_x, int tile_y)
{
    tile_x = map_wrap_x(tile_x, zoom);
    tile_y = map_clamp_y(tile_y, zoom);
    int ready = 0;
    state_lock();
    for (int i = 0; i < MAP_TILE_COUNT; ++i) {
        const MapStagedTile *tile = &g_map_staged[i];
        if (tile->state == MAP_STAGED_READY && tile->provider == provider &&
            tile->zoom == zoom &&
            tile->tile_x == tile_x && tile->tile_y == tile_y) {
            ready = 1;
            break;
        }
    }
    state_unlock();
    return ready;
}

int app_map_copy_staged_tile(AppMapProvider provider, int zoom,
                             int tile_x, int tile_y,
                             void *pixels, size_t stride)
{
    if (!pixels || stride < MAP_TILE_ROW_BYTES) return -1;
    tile_x = map_wrap_x(tile_x, zoom);
    tile_y = map_clamp_y(tile_y, zoom);
    int result = -1;
    state_lock();
    for (int i = 0; i < MAP_TILE_COUNT; ++i) {
        MapStagedTile *tile = &g_map_staged[i];
        if (tile->state != MAP_STAGED_READY || tile->provider != provider ||
            tile->zoom != zoom ||
            tile->tile_x != tile_x || tile->tile_y != tile_y)
            continue;
        unsigned char *destination = pixels;
        for (int y = 0; y < MAP_TILE_HEIGHT; ++y)
            memcpy(destination + (size_t)y * stride,
                   tile->pixels + (size_t)y * MAP_TILE_ROW_BYTES,
                   MAP_TILE_ROW_BYTES);
        /* Retain the decoded pixels as a bounded worker-side cache. Immediate
         * reversals can republish them without another slow ux0: read. */
        tile->state = MAP_STAGED_CACHED;
        result = 0;
        break;
    }
    state_unlock();
    return result;
}

void app_toggle_units(void)
{
    state_lock();
    g_snapshot.use_fahrenheit = !g_snapshot.use_fahrenheit;
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "Unit system: %s",
             g_snapshot.use_fahrenheit ? "Imperial" : "Metric");
    save_config_locked();
    state_unlock();
}

void app_cycle_setting(AppSetting setting, int direction)
{
    if (!direction) direction = 1;
    int queue_gps = 0;
    state_lock();
    switch (setting) {
    case APP_SETTING_TEMPERATURE:
        g_snapshot.use_fahrenheit = !g_snapshot.use_fahrenheit;
        break;
    case APP_SETTING_TIME_FORMAT:
        g_snapshot.use_24_hour = !g_snapshot.use_24_hour;
        break;
    case APP_SETTING_LANGUAGE:
        g_snapshot.language = (AppLanguage)(((int)g_snapshot.language +
            (direction > 0 ? 1 : APP_LANGUAGE_COUNT - 1)) % APP_LANGUAGE_COUNT);
        break;
    case APP_SETTING_THEME:
        g_snapshot.theme = (AppTheme)(((int)g_snapshot.theme +
            (direction > 0 ? 1 : APP_THEME_COUNT - 1)) % APP_THEME_COUNT);
        break;
    case APP_SETTING_BACKGROUND:
        g_snapshot.photo_background = !g_snapshot.photo_background;
        break;
    case APP_SETTING_MOTION:
        g_snapshot.motion = (AppMotion)(((int)g_snapshot.motion +
            (direction > 0 ? 1 : APP_MOTION_COUNT - 1)) % APP_MOTION_COUNT);
        break;
    case APP_SETTING_WEATHER_SOUNDS:
        g_snapshot.weather_sounds = !g_snapshot.weather_sounds;
        break;
    case APP_SETTING_MAP_PROVIDER:
        g_snapshot.map_provider = (AppMapProvider)(
            ((int)g_snapshot.map_provider +
             (direction > 0 ? 1 : APP_MAP_PROVIDER_COUNT - 1)) %
            APP_MAP_PROVIDER_COUNT);
        g_map_pending = 0;
        for (int i = 0; i < MAP_TILE_COUNT; ++i)
            g_map_staged[i].state = MAP_STAGED_EMPTY;
        g_snapshot.map.zoom = 0;
        g_snapshot.map.loading = 0;
        g_snapshot.map.ready_tiles = 0;
        ++g_snapshot.map.revision;
        break;
    case APP_SETTING_AUTO_GPS:
        g_snapshot.auto_gps = !g_snapshot.auto_gps;
        queue_gps = g_snapshot.auto_gps;
        break;
    case APP_SETTING_COUNT:
        break;
    }
    save_config_locked();
    snprintf(g_snapshot.notice, sizeof(g_snapshot.notice), "Setting saved");
    state_unlock();
    app_log("setting changed: id=%d direction=%d", setting, direction);
    if (queue_gps) app_queue_gps();
}

void app_cycle_map_layer(int direction)
{
    if (!direction) direction = 1;
    state_lock();
    g_snapshot.map_layer = (AppMapLayer)(((int)g_snapshot.map_layer +
        (direction > 0 ? 1 : APP_MAP_LAYER_COUNT - 1)) % APP_MAP_LAYER_COUNT);
    save_config_locked();
    state_unlock();
}

void app_set_map_layer(AppMapLayer layer)
{
    if ((unsigned int)layer >= APP_MAP_LAYER_COUNT) return;
    state_lock();
    g_snapshot.map_layer = layer;
    save_config_locked();
    state_unlock();
}
