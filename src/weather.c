#include "weather.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSMN_STATIC
#define JSMN_PARENT_LINKS
#include "jsmn.h"

#define TOKEN_CAPACITY 8192

static int token_equals(const char *json, const jsmntok_t *token,
                        const char *value)
{
    size_t length = strlen(value);
    return token->type == JSMN_STRING &&
           token->end - token->start == (int)length &&
           !strncmp(json + token->start, value, length);
}

static int object_value(const char *json, const jsmntok_t *tokens,
                        int count, int object, const char *key)
{
    for (int i = object + 1; i + 1 < count; ++i) {
        if (tokens[i].parent == object && token_equals(json, &tokens[i], key))
            return i + 1;
    }
    return -1;
}

static int array_item(const jsmntok_t *tokens, int count, int array, int item)
{
    int seen = 0;
    for (int i = array + 1; i < count; ++i) {
        if (tokens[i].parent == array) {
            if (seen == item)
                return i;
            ++seen;
        }
    }
    return -1;
}

static int append_utf8(char *out, size_t out_size, size_t *at,
                       unsigned int codepoint)
{
    unsigned char bytes[3];
    int count;
    if (codepoint < 0x80) {
        bytes[0] = (unsigned char)codepoint;
        count = 1;
    } else if (codepoint < 0x800) {
        bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 2;
    } else {
        bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f));
        count = 3;
    }
    if (*at + (size_t)count >= out_size)
        return -1;
    for (int i = 0; i < count; ++i)
        out[(*at)++] = (char)bytes[i];
    return 0;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int token_string(const char *json, const jsmntok_t *token,
                        char *out, size_t out_size)
{
    if (token->type != JSMN_STRING || !out_size)
        return -1;
    size_t at = 0;
    for (int i = token->start; i < token->end && at + 1 < out_size; ++i) {
        unsigned char c = (unsigned char)json[i];
        if (c != '\\') {
            out[at++] = (char)c;
            continue;
        }
        if (++i >= token->end)
            break;
        c = (unsigned char)json[i];
        if (c == 'n') out[at++] = '\n';
        else if (c == 'r') out[at++] = '\r';
        else if (c == 't') out[at++] = '\t';
        else if (c == 'b') out[at++] = '\b';
        else if (c == 'f') out[at++] = '\f';
        else if (c == 'u' && i + 4 < token->end) {
            unsigned int cp = 0;
            int valid = 1;
            for (int n = 0; n < 4; ++n) {
                int h = hex_value(json[i + 1 + n]);
                if (h < 0) valid = 0;
                cp = (cp << 4) | (unsigned int)(h < 0 ? 0 : h);
            }
            if (valid && append_utf8(out, out_size, &at, cp) == 0)
                i += 4;
            else
                out[at++] = '?';
        } else {
            out[at++] = (char)c;
        }
    }
    out[at] = '\0';
    return 0;
}

static int token_number(const char *json, const jsmntok_t *token, double *out)
{
    char value[48];
    int length = token->end - token->start;
    if (length <= 0 || length >= (int)sizeof(value) ||
        token->type != JSMN_PRIMITIVE)
        return -1;
    memcpy(value, json + token->start, (size_t)length);
    value[length] = '\0';
    char *end = NULL;
    *out = strtod(value, &end);
    return end && *end == '\0' ? 0 : -1;
}

static int value_number(const char *json, const jsmntok_t *tokens, int count,
                        int object, const char *key, double *out)
{
    int token = object_value(json, tokens, count, object, key);
    return token >= 0 ? token_number(json, &tokens[token], out) : -1;
}

static int value_string(const char *json, const jsmntok_t *tokens, int count,
                        int object, const char *key, char *out, size_t out_size)
{
    int token = object_value(json, tokens, count, object, key);
    return token >= 0 ? token_string(json, &tokens[token], out, out_size) : -1;
}

static int array_number(const char *json, const jsmntok_t *tokens, int count,
                        int array, int item, double *out)
{
    int token = array_item(tokens, count, array, item);
    return token >= 0 ? token_number(json, &tokens[token], out) : -1;
}

static int array_string(const char *json, const jsmntok_t *tokens, int count,
                        int array, int item, char *out, size_t out_size)
{
    int token = array_item(tokens, count, array, item);
    return token >= 0 ? token_string(json, &tokens[token], out, out_size) : -1;
}

static int parse_json(const char *json, size_t len, jsmntok_t **out_tokens,
                      char *error, size_t error_size)
{
    jsmntok_t *tokens = calloc(TOKEN_CAPACITY, sizeof(*tokens));
    if (!tokens) {
        snprintf(error, error_size, "Not enough memory for forecast");
        return -1;
    }
    jsmn_parser parser;
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, json, len, tokens, TOKEN_CAPACITY);
    if (count < 1 || (tokens[0].type != JSMN_OBJECT &&
                      tokens[0].type != JSMN_ARRAY)) {
        snprintf(error, error_size, "Invalid JSON response (%d)", count);
        free(tokens);
        return -1;
    }
    *out_tokens = tokens;
    return count;
}

static void hour_label(const char *time, char *out, size_t out_size)
{
    const char *separator = strchr(time, 'T');
    if (separator && strlen(separator + 1) >= 5)
        snprintf(out, out_size, "%.5s", separator + 1);
    else
        snprintf(out, out_size, "%.7s", time);
}

static int weekday_for_date(const char *date)
{
    int year = 0, month = 0, day = 0;
    if (sscanf(date, "%d-%d-%d", &year, &month, &day) != 3)
        return -1;
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) --year;
    return (year + year / 4 - year / 100 + year / 400 +
            offsets[month - 1] + day) % 7;
}

static void day_label(const char *date, char *out, size_t out_size)
{
    static const char *names[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    int day = weekday_for_date(date);
    snprintf(out, out_size, "%s", day >= 0 ? names[day] : "DAY");
}

int weather_parse_forecast(const char *json, size_t len, WeatherData *out,
                           char *error, size_t error_size)
{
    if (!json || !out) return -1;
    jsmntok_t *tokens = NULL;
    int count = parse_json(json, len, &tokens, error, error_size);
    if (count < 0) return -1;

    int current = object_value(json, tokens, count, 0, "current");
    int hourly = object_value(json, tokens, count, 0, "hourly");
    int daily = object_value(json, tokens, count, 0, "daily");
    if (current < 0 || hourly < 0 || daily < 0 ||
        tokens[current].type != JSMN_OBJECT ||
        tokens[hourly].type != JSMN_OBJECT ||
        tokens[daily].type != JSMN_OBJECT) {
        snprintf(error, error_size, "Forecast response is missing sections");
        free(tokens);
        return -1;
    }

    WeatherData parsed;
    memset(&parsed, 0, sizeof(parsed));
    double number;
    if (value_number(json, tokens, count, current, "temperature_2m", &number) < 0) {
        snprintf(error, error_size, "Forecast has no current temperature");
        free(tokens);
        return -1;
    }
    parsed.temperature_c = (float)number;
    if (!value_number(json, tokens, count, current, "apparent_temperature", &number))
        parsed.apparent_c = (float)number;
    if (!value_number(json, tokens, count, current, "relative_humidity_2m", &number))
        parsed.humidity = (int)number;
    if (!value_number(json, tokens, count, current, "weather_code", &number))
        parsed.weather_code = (int)number;
    if (!value_number(json, tokens, count, current, "is_day", &number))
        parsed.is_day = (int)number;
    if (!value_number(json, tokens, count, current, "wind_speed_10m", &number))
        parsed.wind_kmh = (float)number;
    if (!value_number(json, tokens, count, current, "wind_direction_10m", &number))
        parsed.wind_direction = (float)number;
    if (!value_number(json, tokens, count, current, "surface_pressure", &number))
        parsed.pressure_hpa = (float)number;
    if (!value_number(json, tokens, count, current, "visibility", &number))
        parsed.visibility_km = (float)number / 1000.0f;
    if (!value_number(json, tokens, count, current, "cloud_cover", &number))
        parsed.cloud_cover = (float)number;
    value_string(json, tokens, count, current, "time", parsed.updated,
                 sizeof(parsed.updated));
    value_string(json, tokens, count, 0, "timezone", parsed.timezone,
                 sizeof(parsed.timezone));

    int h_time = object_value(json, tokens, count, hourly, "time");
    int h_temp = object_value(json, tokens, count, hourly, "temperature_2m");
    int h_apparent = object_value(json, tokens, count, hourly, "apparent_temperature");
    int h_code = object_value(json, tokens, count, hourly, "weather_code");
    int h_rain = object_value(json, tokens, count, hourly, "precipitation_probability");
    int h_precip = object_value(json, tokens, count, hourly, "precipitation");
    int h_wind = object_value(json, tokens, count, hourly, "wind_speed_10m");
    int h_gust = object_value(json, tokens, count, hourly, "wind_gusts_10m");
    int h_direction = object_value(json, tokens, count, hourly, "wind_direction_10m");
    int h_humidity = object_value(json, tokens, count, hourly, "relative_humidity_2m");
    int h_dew = object_value(json, tokens, count, hourly, "dew_point_2m");
    int h_pressure = object_value(json, tokens, count, hourly, "surface_pressure");
    int h_visibility = object_value(json, tokens, count, hourly, "visibility");
    int h_uv = object_value(json, tokens, count, hourly, "uv_index");
    if (h_time < 0 || h_temp < 0 || tokens[h_time].type != JSMN_ARRAY ||
        tokens[h_temp].type != JSMN_ARRAY) {
        snprintf(error, error_size, "Forecast has no hourly timeline");
        free(tokens);
        return -1;
    }
    int available_hours = tokens[h_time].size;
    int start = 0;
    char current_time[20] = "";
    value_string(json, tokens, count, current, "time", current_time,
                 sizeof(current_time));
    /* Open-Meteo's forecast timeline starts at midnight.  Keep the elapsed
     * part of today so full-day detail graphs can retain and shade
     * past observations, while current_hour_index keeps the 24-hour and map
     * timelines anchored at NOW. */
    char current_date[11] = "";
    if (strlen(current_time) >= 10)
        memcpy(current_date, current_time, 10);
    for (int i = 0; i < available_hours; ++i) {
        char candidate[20];
        if (!array_string(json, tokens, count, h_time, i, candidate, sizeof(candidate)) &&
            (!current_date[0] || strncmp(candidate, current_date, 10) >= 0)) {
            start = i;
            break;
        }
    }
    parsed.current_hour_index = -1;
    for (int i = start; i < available_hours && parsed.hour_count < WEATHER_HOURS; ++i) {
        WeatherHour *entry = &parsed.hours[parsed.hour_count];
        if (array_string(json, tokens, count, h_time, i, entry->time,
                         sizeof(entry->time)) < 0 ||
            array_number(json, tokens, count, h_temp, i, &number) < 0)
            continue;
        entry->temperature_c = (float)number;
        if (h_apparent >= 0 &&
            !array_number(json, tokens, count, h_apparent, i, &number))
            entry->apparent_c = (float)number;
        else
            entry->apparent_c = entry->temperature_c;
        hour_label(entry->time, entry->label, sizeof(entry->label));
        if (h_code >= 0 && !array_number(json, tokens, count, h_code, i, &number))
            entry->weather_code = (int)number;
        if (h_rain >= 0 && !array_number(json, tokens, count, h_rain, i, &number))
            entry->precipitation_percent = (int)number;
        if (h_precip >= 0 && !array_number(json, tokens, count, h_precip, i, &number))
            entry->precipitation_mm = (float)number;
        if (h_wind >= 0 && !array_number(json, tokens, count, h_wind, i, &number))
            entry->wind_kmh = (float)number;
        if (h_gust >= 0 && !array_number(json, tokens, count, h_gust, i, &number))
            entry->wind_gust_kmh = (float)number;
        else
            entry->wind_gust_kmh = entry->wind_kmh;
        if (h_direction >= 0 && !array_number(json, tokens, count, h_direction, i, &number))
            entry->wind_direction = (float)number;
        if (h_humidity >= 0 &&
            !array_number(json, tokens, count, h_humidity, i, &number))
            entry->humidity = (int)number;
        if (h_dew >= 0 && !array_number(json, tokens, count, h_dew, i, &number))
            entry->dew_point_c = (float)number;
        if (h_pressure >= 0 &&
            !array_number(json, tokens, count, h_pressure, i, &number))
            entry->pressure_hpa = (float)number;
        if (h_visibility >= 0 &&
            !array_number(json, tokens, count, h_visibility, i, &number))
            entry->visibility_km = (float)number / 1000.0f;
        if (h_uv >= 0 && !array_number(json, tokens, count, h_uv, i, &number))
            entry->uv_index = (float)number;
        if (parsed.current_hour_index < 0 &&
            (!current_time[0] || strcmp(entry->time, current_time) >= 0))
            parsed.current_hour_index = parsed.hour_count;
        ++parsed.hour_count;
    }
    if (parsed.current_hour_index < 0)
        parsed.current_hour_index = parsed.hour_count > 0
                                        ? parsed.hour_count - 1 : 0;

    int d_time = object_value(json, tokens, count, daily, "time");
    int d_high = object_value(json, tokens, count, daily, "temperature_2m_max");
    int d_low = object_value(json, tokens, count, daily, "temperature_2m_min");
    int d_code = object_value(json, tokens, count, daily, "weather_code");
    int d_rain = object_value(json, tokens, count, daily, "precipitation_probability_max");
    int d_precip = object_value(json, tokens, count, daily, "precipitation_sum");
    int d_sunrise = object_value(json, tokens, count, daily, "sunrise");
    int d_sunset = object_value(json, tokens, count, daily, "sunset");
    int d_uv = object_value(json, tokens, count, daily, "uv_index_max");
    if (d_time < 0 || d_high < 0 || d_low < 0 ||
        tokens[d_time].type != JSMN_ARRAY) {
        snprintf(error, error_size, "Forecast has no daily timeline");
        free(tokens);
        return -1;
    }
    int available_days = tokens[d_time].size;
    for (int i = 0; i < available_days && parsed.day_count < WEATHER_DAYS; ++i) {
        WeatherDay *entry = &parsed.days[parsed.day_count];
        if (array_string(json, tokens, count, d_time, i, entry->date,
                         sizeof(entry->date)) < 0 ||
            array_number(json, tokens, count, d_high, i, &number) < 0)
            continue;
        entry->high_c = (float)number;
        if (!array_number(json, tokens, count, d_low, i, &number))
            entry->low_c = (float)number;
        if (d_code >= 0 && !array_number(json, tokens, count, d_code, i, &number))
            entry->weather_code = (int)number;
        if (d_rain >= 0 && !array_number(json, tokens, count, d_rain, i, &number))
            entry->precipitation_percent = (int)number;
        if (d_precip >= 0 &&
            !array_number(json, tokens, count, d_precip, i, &number))
            entry->precipitation_mm = (float)number;
        char sun_time[24];
        if (d_sunrise >= 0 && !array_string(json, tokens, count, d_sunrise, i,
                                            sun_time, sizeof(sun_time))) {
            hour_label(sun_time, entry->sunrise, sizeof(entry->sunrise));
        }
        if (d_sunset >= 0 && !array_string(json, tokens, count, d_sunset, i,
                                           sun_time, sizeof(sun_time))) {
            hour_label(sun_time, entry->sunset, sizeof(entry->sunset));
        }
        if (d_uv >= 0 &&
            !array_number(json, tokens, count, d_uv, i, &number)) {
            entry->uv_index = (float)number;
            if (i == 0) parsed.uv_index = (float)number;
        }
        day_label(entry->date, entry->label, sizeof(entry->label));
        ++parsed.day_count;
    }

    if (!parsed.hour_count || !parsed.day_count) {
        snprintf(error, error_size, "Forecast timelines are empty");
        free(tokens);
        return -1;
    }
    *out = parsed;
    free(tokens);
    return 0;
}

static int parse_location_object(const char *json, const jsmntok_t *tokens,
                                 int count, int object, WeatherLocation *out)
{
    WeatherLocation location;
    memset(&location, 0, sizeof(location));
    if (value_string(json, tokens, count, object, "name", location.name,
                     sizeof(location.name)) < 0 ||
        value_number(json, tokens, count, object, "latitude", &location.latitude) < 0 ||
        value_number(json, tokens, count, object, "longitude", &location.longitude) < 0)
        return -1;
    value_string(json, tokens, count, object, "timezone", location.timezone,
                 sizeof(location.timezone));
    char admin[64] = "";
    char country[64] = "";
    value_string(json, tokens, count, object, "admin1", admin, sizeof(admin));
    value_string(json, tokens, count, object, "country", country, sizeof(country));
    if (admin[0] && country[0])
        snprintf(location.region, sizeof(location.region), "%.44s, %.47s",
                 admin, country);
    else
        snprintf(location.region, sizeof(location.region), "%.95s",
                 admin[0] ? admin : country);
    *out = location;
    return 0;
}

int weather_parse_geocodes(const char *json, size_t len, WeatherLocation *out,
                           int capacity, char *error, size_t error_size)
{
    if (!json || !out || capacity < 1) return -1;
    jsmntok_t *tokens = NULL;
    int count = parse_json(json, len, &tokens, error, error_size);
    if (count < 0) return -1;
    int results = object_value(json, tokens, count, 0, "results");
    if (results < 0 || tokens[results].type != JSMN_ARRAY) {
        snprintf(error, error_size, "No matching city found");
        free(tokens);
        return -1;
    }
    int parsed = 0;
    for (int i = 0; i < tokens[results].size && parsed < capacity; ++i) {
        int object = array_item(tokens, count, results, i);
        if (object >= 0 && tokens[object].type == JSMN_OBJECT &&
            parse_location_object(json, tokens, count, object, &out[parsed]) == 0)
            ++parsed;
    }
    free(tokens);
    if (!parsed) {
        snprintf(error, error_size, "No matching city found");
        return -1;
    }
    return parsed;
}

int weather_parse_geocode(const char *json, size_t len, WeatherLocation *out,
                          char *error, size_t error_size)
{
    int count = weather_parse_geocodes(json, len, out, 1, error, error_size);
    return count > 0 ? 0 : -1;
}

int weather_parse_air_quality(const char *json, size_t len, WeatherData *weather,
                              char *error, size_t error_size)
{
    if (!json || !weather) return -1;
    jsmntok_t *tokens = NULL;
    int count = parse_json(json, len, &tokens, error, error_size);
    if (count < 0) return -1;
    int current = object_value(json, tokens, count, 0, "current");
    int hourly = object_value(json, tokens, count, 0, "hourly");
    double number;
    if (current >= 0 && tokens[current].type == JSMN_OBJECT) {
        if (!value_number(json, tokens, count, current, "us_aqi", &number))
            weather->air_quality = (float)number;
        if (!value_number(json, tokens, count, current, "pm2_5", &number))
            weather->pm2_5 = (float)number;
        if (!value_number(json, tokens, count, current, "pm10", &number))
            weather->pm10 = (float)number;
    }
    if (hourly >= 0 && tokens[hourly].type == JSMN_OBJECT) {
        int times = object_value(json, tokens, count, hourly, "time");
        int values = object_value(json, tokens, count, hourly, "us_aqi");
        if (times >= 0 && values >= 0 && tokens[times].type == JSMN_ARRAY) {
            for (int h = 0; h < weather->hour_count; ++h) {
                for (int i = 0; i < tokens[times].size; ++i) {
                    char time[20];
                    if (!array_string(json, tokens, count, times, i, time, sizeof(time)) &&
                        !strcmp(time, weather->hours[h].time) &&
                        !array_number(json, tokens, count, values, i, &number)) {
                        weather->hours[h].air_quality = (float)number;
                        break;
                    }
                }
            }
        }
    }
    free(tokens);
    return weather->air_quality > 0.0f ? 0 : -1;
}

static int map_response_object(const jsmntok_t *tokens, int count, int item)
{
    if (tokens[0].type == JSMN_OBJECT)
        return item == 0 ? 0 : -1;
    return array_item(tokens, count, 0, item);
}

int weather_parse_map_forecast(const char *json, size_t len,
                               WeatherMapField *field, char *error,
                               size_t error_size)
{
    if (!json || !field) return -1;
    jsmntok_t *tokens = NULL;
    int count = parse_json(json, len, &tokens, error, error_size);
    if (count < 0) return -1;
    WeatherMapField parsed;
    memset(&parsed, 0, sizeof(parsed));
    int available = tokens[0].type == JSMN_ARRAY ? tokens[0].size : 1;
    if (available > WEATHER_MAP_POINTS) available = WEATHER_MAP_POINTS;
    for (int point = 0; point < available; ++point) {
        int object = map_response_object(tokens, count, point);
        if (object < 0 || tokens[object].type != JSMN_OBJECT) continue;
        int hourly = object_value(json, tokens, count, object, "hourly");
        if (hourly < 0 || tokens[hourly].type != JSMN_OBJECT) continue;
        int temperature = object_value(json, tokens, count, hourly,
                                       "temperature_2m");
        int precipitation = object_value(json, tokens, count, hourly,
                                         "precipitation");
        int wind = object_value(json, tokens, count, hourly,
                                "wind_speed_10m");
        int direction = object_value(json, tokens, count, hourly,
                                     "wind_direction_10m");
        if (temperature < 0 || precipitation < 0 || wind < 0 ||
            direction < 0 || tokens[temperature].type != JSMN_ARRAY ||
            tokens[precipitation].type != JSMN_ARRAY ||
            tokens[wind].type != JSMN_ARRAY ||
            tokens[direction].type != JSMN_ARRAY)
            continue;
        int hours = tokens[temperature].size;
        if (hours > tokens[precipitation].size)
            hours = tokens[precipitation].size;
        if (hours > tokens[wind].size) hours = tokens[wind].size;
        if (hours > tokens[direction].size) hours = tokens[direction].size;
        if (hours > WEATHER_MAP_HOURS) hours = WEATHER_MAP_HOURS;
        WeatherMapPoint *target = &parsed.points[point];
        int valid_hours = 0;
        for (int hour = 0; hour < hours; ++hour) {
            double values[4];
            if (array_number(json, tokens, count, temperature, hour,
                             &values[0]) < 0 ||
                array_number(json, tokens, count, precipitation, hour,
                             &values[1]) < 0 ||
                array_number(json, tokens, count, wind, hour,
                             &values[2]) < 0 ||
                array_number(json, tokens, count, direction, hour,
                             &values[3]) < 0)
                break;
            target->temperature_c[hour] = (float)values[0];
            target->precipitation_mm[hour] = (float)values[1];
            target->wind_kmh[hour] = (float)values[2];
            target->wind_direction[hour] = (float)values[3];
            ++valid_hours;
        }
        if (valid_hours > 0) {
            target->forecast_valid = 1;
            if (!parsed.hour_count || valid_hours < parsed.hour_count)
                parsed.hour_count = valid_hours;
        }
    }
    parsed.point_count = available;
    free(tokens);
    if (!parsed.hour_count) {
        snprintf(error, error_size, "Map forecast contains no usable grid");
        return -1;
    }
    *field = parsed;
    return 0;
}

int weather_parse_map_air_quality(const char *json, size_t len,
                                  WeatherMapField *field, char *error,
                                  size_t error_size)
{
    if (!json || !field) return -1;
    jsmntok_t *tokens = NULL;
    int count = parse_json(json, len, &tokens, error, error_size);
    if (count < 0) return -1;
    int available = tokens[0].type == JSMN_ARRAY ? tokens[0].size : 1;
    if (available > WEATHER_MAP_POINTS) available = WEATHER_MAP_POINTS;
    int valid_points = 0;
    for (int point = 0; point < available; ++point) {
        int object = map_response_object(tokens, count, point);
        if (object < 0 || tokens[object].type != JSMN_OBJECT) continue;
        int hourly = object_value(json, tokens, count, object, "hourly");
        int values = hourly >= 0
                         ? object_value(json, tokens, count, hourly, "us_aqi")
                         : -1;
        if (values < 0 || tokens[values].type != JSMN_ARRAY) continue;
        int hours = tokens[values].size;
        if (hours > WEATHER_MAP_HOURS) hours = WEATHER_MAP_HOURS;
        int valid_hours = 0;
        for (int hour = 0; hour < hours; ++hour) {
            double number;
            if (array_number(json, tokens, count, values, hour, &number) < 0)
                break;
            field->points[point].air_quality[hour] = (float)number;
            ++valid_hours;
        }
        if (valid_hours > 0) {
            field->points[point].air_quality_valid = 1;
            ++valid_points;
            if (!field->hour_count || valid_hours < field->hour_count)
                field->hour_count = valid_hours;
        }
    }
    if (field->point_count < available) field->point_count = available;
    free(tokens);
    if (!valid_points) {
        snprintf(error, error_size, "Map AQI contains no usable grid");
        return -1;
    }
    return 0;
}

const char *weather_condition_name(int code)
{
    switch (code) {
    case 0: return "Clear sky";
    case 1: return "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Foggy";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: case 77: return "Heavy snow";
    case 80: case 81: case 82: return "Rain showers";
    case 85: case 86: return "Snow showers";
    case 95: case 96: case 99: return "Thunderstorms";
    default: return "Weather";
    }
}

int weather_condition_group(int code)
{
    if (code == 0 || code == 1) return 0;
    if (code == 2 || code == 3) return 1;
    if (code == 45 || code == 48) return 5;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 2;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 3;
    if (code >= 95) return 4;
    return 1;
}

float weather_display_temperature(float celsius, int use_fahrenheit)
{
    return use_fahrenheit ? celsius * 9.0f / 5.0f + 32.0f : celsius;
}

const char *weather_temperature_unit(int use_fahrenheit)
{
    return use_fahrenheit ? "F" : "C";
}

float weather_display_wind(float kmh, int use_imperial)
{
    return use_imperial ? kmh * 0.621371192f : kmh;
}

const char *weather_wind_unit(int use_imperial)
{
    return use_imperial ? "mph" : "km/h";
}

float weather_display_visibility(float km, int use_imperial)
{
    return use_imperial ? km * 0.621371192f : km;
}

const char *weather_visibility_unit(int use_imperial)
{
    return use_imperial ? "mi" : "km";
}

float weather_display_pressure(float hpa, int use_imperial)
{
    return use_imperial ? hpa * 0.0295299831f : hpa;
}

const char *weather_pressure_unit(int use_imperial)
{
    return use_imperial ? "inHg" : "hPa";
}

void weather_load_demo(WeatherData *out)
{
    static const float highs[] = {25, 24, 22, 23, 26, 27, 25, 24, 23, 25};
    static const float lows[] = {17, 16, 15, 14, 16, 18, 17, 16, 15, 17};
    static const int day_codes[] = {2, 61, 3, 1, 0, 2, 80, 2, 61, 1};
    static const int day_rain[] = {12, 65, 20, 8, 4, 18, 55, 16, 48, 9};
    static const char *day_names[] = {
        "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN", "MON", "TUE", "WED"
    };
    static const char *day_dates[] = {
        "2026-08-24", "2026-08-25", "2026-08-26", "2026-08-27",
        "2026-08-28", "2026-08-29", "2026-08-30", "2026-08-31",
        "2026-09-01", "2026-09-02"
    };
    static const char *sunrises[] = {
        "06:10", "06:11", "06:12", "06:13", "06:14",
        "06:15", "06:16", "06:17", "06:18", "06:19"
    };
    static const char *sunsets[] = {
        "19:45", "19:44", "19:43", "19:42", "19:41",
        "19:40", "19:39", "19:38", "19:37", "19:36"
    };

    memset(out, 0, sizeof(*out));
    snprintf(out->location, sizeof(out->location), "New York");
    snprintf(out->region, sizeof(out->region), "New York, United States");
    snprintf(out->timezone, sizeof(out->timezone), "America/New_York");
    snprintf(out->updated, sizeof(out->updated), "2026-08-24T12:00");
    out->latitude = 40.7128;
    out->longitude = -74.0060;
    out->temperature_c = 22.0f;
    out->apparent_c = 22.5f;
    out->wind_kmh = 13.0f;
    out->wind_direction = 230.0f;
    out->pressure_hpa = 1016.0f;
    out->visibility_km = 16.0f;
    out->cloud_cover = 34.0f;
    out->uv_index = 5.0f;
    out->air_quality = 31.0f;
    out->pm2_5 = 7.0f;
    out->pm10 = 12.0f;
    out->humidity = 61;
    out->weather_code = 2;
    out->is_day = 1;
    out->hour_count = WEATHER_HOURS;
    out->current_hour_index = 12;
    out->day_count = WEATHER_DAYS;
    out->source = WEATHER_SOURCE_DEMO;
    for (int i = 0; i < WEATHER_HOURS; ++i) {
        int day = i / 24;
        int hour = i % 24;
        float daylight = hour <= 12 ? hour / 12.0f : (24 - hour) / 12.0f;
        if (daylight < 0.0f) daylight = 0.0f;
        snprintf(out->hours[i].time, sizeof(out->hours[i].time),
                 "%sT%02d:00", day_dates[day], hour);
        if (!i)
            snprintf(out->hours[i].label, sizeof(out->hours[i].label), "NOW");
        else
            snprintf(out->hours[i].label, sizeof(out->hours[i].label), "%02d:00", hour);
        out->hours[i].temperature_c = lows[day] +
            (highs[day] - lows[day]) * daylight;
        out->hours[i].apparent_c = out->hours[i].temperature_c -
            (hour < 7 || hour > 20 ? 1.2f : -0.4f);
        out->hours[i].weather_code = day_codes[day];
        int rain_variation = (hour % 6) * 2 - 5;
        int rain_chance = day_rain[day] + rain_variation;
        if (rain_chance < 0) rain_chance = 0;
        if (rain_chance > 100) rain_chance = 100;
        out->hours[i].precipitation_percent = rain_chance;
        out->hours[i].precipitation_mm = rain_chance > 45 ? 0.8f : 0.0f;
        out->hours[i].wind_kmh = 9.0f + (hour % 8);
        out->hours[i].wind_gust_kmh = out->hours[i].wind_kmh + 6.0f;
        out->hours[i].wind_direction = 210.0f + (hour % 12) * 4.0f;
        out->hours[i].air_quality = 28.0f + day + (hour % 4);
        out->hours[i].humidity = 72 - (int)(daylight * 25.0f);
        out->hours[i].dew_point_c = out->hours[i].temperature_c -
            (100.0f - out->hours[i].humidity) / 5.0f;
        out->hours[i].pressure_hpa = 1014.0f + sinf(i * 0.18f) * 3.0f;
        out->hours[i].visibility_km = 12.0f + daylight * 6.0f;
        out->hours[i].uv_index = daylight * (5.0f - day * 0.18f);
    }
    for (int i = 0; i < WEATHER_DAYS; ++i) {
        snprintf(out->days[i].label, sizeof(out->days[i].label), "%s", day_names[i]);
        snprintf(out->days[i].date, sizeof(out->days[i].date), "%s", day_dates[i]);
        snprintf(out->days[i].sunrise, sizeof(out->days[i].sunrise), "%s", sunrises[i]);
        snprintf(out->days[i].sunset, sizeof(out->days[i].sunset), "%s", sunsets[i]);
        out->days[i].high_c = highs[i];
        out->days[i].low_c = lows[i];
        out->days[i].uv_index = i < 5 ? 5.0f - i * 0.5f : 3.0f;
        out->days[i].weather_code = day_codes[i];
        out->days[i].precipitation_percent = day_rain[i];
        out->days[i].precipitation_mm = day_rain[i] > 45 ? 3.2f : 0.0f;
    }
}
