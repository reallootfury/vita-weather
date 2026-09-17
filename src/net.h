#ifndef VITA_WEATHER_NET_H
#define VITA_WEATHER_NET_H

#include <stddef.h>

#include "weather.h"

#define MAP_TILE_COLUMNS 5
#define MAP_TILE_ROWS 3
#define MAP_TILE_COUNT (MAP_TILE_COLUMNS * MAP_TILE_ROWS)
#define MAP_TILE_WIDTH 256
#define MAP_TILE_HEIGHT 256
#define MAP_TILE_BYTES_PER_PIXEL 4
#define MAP_TILE_ROW_BYTES (MAP_TILE_WIDTH * MAP_TILE_BYTES_PER_PIXEL)
#define APP_SAVED_LOCATIONS 8
#define APP_SEARCH_RESULTS 5

typedef enum AppLanguage {
    APP_LANGUAGE_ENGLISH = 0,
    APP_LANGUAGE_SPANISH,
    APP_LANGUAGE_FRENCH,
    APP_LANGUAGE_GERMAN,
    APP_LANGUAGE_COUNT
} AppLanguage;

typedef enum AppTheme {
    APP_THEME_AUTO = 0,
    APP_THEME_DAY,
    APP_THEME_NIGHT,
    APP_THEME_COUNT
} AppTheme;

typedef enum AppMotion {
    APP_MOTION_OFF = 0,
    APP_MOTION_REDUCED,
    APP_MOTION_FULL,
    APP_MOTION_COUNT
} AppMotion;

typedef enum AppMapLayer {
    APP_MAP_TEMPERATURE = 0,
    APP_MAP_PRECIPITATION,
    APP_MAP_WIND,
    APP_MAP_AIR_QUALITY,
    APP_MAP_LAYER_COUNT
} AppMapLayer;

typedef enum AppMapProvider {
    APP_MAP_STANDARD = 0,
    APP_MAP_TOPOGRAPHIC,
    APP_MAP_CYCLOSM,
    APP_MAP_OSM_FRANCE,
    APP_MAP_HUMANITARIAN,
    APP_MAP_PROVIDER_COUNT
} AppMapProvider;

typedef enum AppSetting {
    APP_SETTING_TEMPERATURE = 0,
    APP_SETTING_TIME_FORMAT,
    APP_SETTING_LANGUAGE,
    APP_SETTING_THEME,
    APP_SETTING_BACKGROUND,
    APP_SETTING_MOTION,
    APP_SETTING_WEATHER_SOUNDS,
    APP_SETTING_MAP_PROVIDER,
    APP_SETTING_AUTO_GPS,
    APP_SETTING_COUNT
} AppSetting;

typedef struct MapSnapshot {
    AppMapProvider provider;
    int zoom;
    int center_x;
    int center_y;
    int base_x;
    int base_y;
    int loading;
    int ready_tiles;
    unsigned int revision;
    WeatherMapField field;
    int field_zoom;
    int field_base_x;
    int field_base_y;
    int field_loading;
    unsigned int field_revision;
    char notice[80];
} MapSnapshot;

typedef struct AppSnapshot {
    WeatherData weather;
    MapSnapshot map;
    int use_fahrenheit;
    int use_24_hour;
    AppLanguage language;
    AppTheme theme;
    int photo_background;
    AppMotion motion;
    int weather_sounds;
    AppMapLayer map_layer;
    AppMapProvider map_provider;
    int auto_gps;
    int gps_active;
    char gps_notice[128];
    WeatherLocation saved_locations[APP_SAVED_LOCATIONS];
    int saved_location_count;
    int selected_location;
    WeatherLocation search_results[APP_SEARCH_RESULTS];
    int search_result_count;
    int search_busy;
    char search_query[96];
    char search_notice[96];
    int busy;
    char activity[80];
    char notice[128];
} AppSnapshot;

int app_services_init(void);
void app_services_shutdown(void);
void app_snapshot_read(AppSnapshot *out);
void app_queue_refresh(void);
void app_queue_search(const char *query);
void app_queue_suggestions(const char *query);
void app_select_suggestion(int index);
void app_select_saved_location(int index);
void app_remove_saved_location(int index);
void app_queue_gps(void);
void app_queue_map(int zoom, int center_x, int center_y, int active_provider,
                   int active_zoom, int active_base_x, int active_base_y);
void app_map_tile_path(char *out, size_t out_size, AppMapProvider provider,
                       int zoom,
                       int tile_x, int tile_y);
void app_map_tile_raw_path(char *out, size_t out_size, AppMapProvider provider,
                           int zoom,
                           int tile_x, int tile_y);
int app_map_tile_staged(AppMapProvider provider, int zoom,
                        int tile_x, int tile_y);
int app_map_copy_staged_tile(AppMapProvider provider, int zoom,
                             int tile_x, int tile_y,
                             void *pixels, size_t stride);
int app_map_provider_max_zoom(AppMapProvider provider);
const char *app_map_provider_name(AppMapProvider provider);
const char *app_map_provider_attribution(AppMapProvider provider);
void app_toggle_units(void);
void app_cycle_setting(AppSetting setting, int direction);
void app_cycle_map_layer(int direction);
void app_set_map_layer(AppMapLayer layer);
int app_load_initial_state(void);
void app_log(const char *format, ...);

#endif
