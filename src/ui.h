#ifndef VITA_WEATHER_UI_H
#define VITA_WEATHER_UI_H

#include <stdint.h>
#include <psp2/libime.h>
#include <vita2d.h>

#include "net.h"

enum {
    VIEW_NOW = 0,
    VIEW_HOURLY = 1,
    VIEW_DAILY = 2,
    VIEW_MAP = 3,
    VIEW_DETAILS = 4,
    VIEW_COUNT = 5
};

typedef enum UiScreen {
    UI_SCREEN_WEATHER = 0,
    UI_SCREEN_SETTINGS,
    UI_SCREEN_LOCATIONS,
    UI_SCREEN_SEARCH,
    UI_SCREEN_MAP_OPTIONS,
    UI_SCREEN_DAY_DETAIL,
    UI_SCREEN_DAY_METRICS
} UiScreen;

typedef enum DayDetailMetric {
    DAY_METRIC_CONDITIONS = 0,
    DAY_METRIC_UV,
    DAY_METRIC_WIND,
    DAY_METRIC_PRECIPITATION,
    DAY_METRIC_HUMIDITY,
    DAY_METRIC_VISIBILITY,
    DAY_METRIC_PRESSURE,
    DAY_METRIC_COUNT
} DayDetailMetric;

typedef struct UiState {
    vita2d_font *fonts[7];
    vita2d_texture *weather_bg;
    vita2d_texture *sun_glow;
    vita2d_texture *weather_retired_bg;
    vita2d_texture *weather_pending_bg;
    void *weather_bg_file;
    int weather_bg_key;
    int weather_pending_bg_key;
    int weather_pending_row;
    uint64_t weather_pending_start_us;
    uint64_t weather_bg_transition_us;
    int fonts_warmed;
    UiScreen screen;
    int settings_selected;
    int settings_tab;
    int settings_previous_selected;
    int settings_previous_tab;
    int settings_transition_direction;
    uint64_t settings_transition_start_us;
    uint64_t overlay_open_us;
    int map_options_selected;
    int location_selected;
    int search_selected;
    int daily_offset;
    int daily_selected;
    float day_detail_scroll;
    float day_detail_scroll_target;
    float day_detail_max_scroll;
    uint64_t day_detail_scroll_update_us;
    int day_detail_metric;
    int day_detail_feels_like;
    int day_metric_selected;
    int ime_active;
    unsigned char ime_work[SCE_IME_WORK_BUFFER_SIZE] __attribute__((aligned(64)));
    uint16_t ime_initial[96];
    uint16_t ime_input[96];
    char search_query[96];
    volatile int ime_dirty;
    volatile int ime_submit;
    volatile int ime_close;
    int search_query_pending;
    uint64_t search_changed_us;
    int hourly_offset;
    float hourly_visual_offset;
    uint64_t hourly_visual_update_us;
    int view;
    int previous_view;
    int transition_direction;
    uint64_t transition_start_us;
    vita2d_texture *map_tiles[MAP_TILE_COUNT];
    unsigned char map_tile_loaded[MAP_TILE_COUNT];
    vita2d_texture *map_pending_tiles[MAP_TILE_COUNT];
    unsigned char map_pending_tile_loaded[MAP_TILE_COUNT];
    signed char map_pending_reuse[MAP_TILE_COUNT];
    unsigned char map_pending_attempted[MAP_TILE_COUNT];
    unsigned char map_active_attempted[MAP_TILE_COUNT];
    vita2d_texture *map_retired_tiles[MAP_TILE_COUNT];
    unsigned char map_retired_tile_loaded[MAP_TILE_COUNT];
    vita2d_texture *map_field_texture;
    unsigned int map_field_revision;
    int map_field_layer;
    int map_field_hour;
    int map_retired_zoom;
    int map_retired_provider;
    int map_retired_base_x;
    int map_retired_base_y;
    int map_retired_recyclable;
    int map_zoom;
    int map_center_x;
    int map_center_y;
    double map_camera_x;
    double map_camera_y;
    double map_present_x;
    double map_present_y;
    int map_present_zoom;
    int map_present_initialized;
    float map_present_peak_dx;
    float map_present_peak_dy;
    float map_visual_zoom;
    uint64_t map_visual_update_us;
    int map_hour;
    float map_visual_hour;
    int map_fullscreen;
    int map_loaded_zoom;
    int map_loaded_provider;
    int map_loaded_base_x;
    int map_loaded_base_y;
    int map_pending_zoom;
    int map_pending_provider;
    int map_pending_base_x;
    int map_pending_base_y;
    uint64_t map_transition_start_us;
    int map_preview_dx;
    int map_preview_dy;
    int map_preview_zoom;
    uint64_t map_preview_start_us;
    int map_initialized;
    int map_request_pending;
    int map_navigation_locked;
    int map_edge_clamped;
    unsigned int map_revision;
    unsigned int map_upload_count;
    unsigned int map_upload_placeholders;
    uint64_t map_upload_total_us;
    uint64_t map_upload_max_us;
    uint64_t map_input_us;
    double map_location_lat;
    double map_location_lon;
    int capture_pending;
    int capture_delay_frames;
    char capture_path[96];
} UiState;

int ui_init(UiState *ui);
void ui_shutdown(UiState *ui);
void ui_set_view(UiState *ui, int view, uint64_t now_us);
void ui_map_focus(UiState *ui, const WeatherData *weather);
void ui_map_constrain_provider(UiState *ui, AppMapProvider provider);
void ui_map_pan(UiState *ui, int dx, int dy, uint64_t now_us);
void ui_map_pan_pixels(UiState *ui, float dx, float dy, uint64_t now_us);
void ui_map_zoom(UiState *ui, const WeatherData *weather,
                 AppMapProvider provider, int direction,
                 uint64_t now_us);
void ui_map_time_move(UiState *ui, int direction,
                      const WeatherData *weather);
void ui_map_set_hour(UiState *ui, int hour, const WeatherData *weather);
void ui_map_toggle_fullscreen(UiState *ui);
void ui_map_recenter(UiState *ui, const WeatherData *weather,
                     uint64_t now_us);
void ui_hourly_move(UiState *ui, int direction, const WeatherData *weather);
void ui_daily_move(UiState *ui, int direction, const WeatherData *weather);
void ui_open_day_detail(UiState *ui, const WeatherData *weather);
void ui_open_day_metrics(UiState *ui);
void ui_confirm_day_metric(UiState *ui);
void ui_day_detail_scroll(UiState *ui, float amount);
void ui_day_detail_toggle_metric(UiState *ui, int direction);
void ui_settings_tab_move(UiState *ui, int direction);
void ui_hide_search_keyboard(UiState *ui);
int ui_map_take_request(UiState *ui, int *zoom, int *center_x, int *center_y,
                        int *active_provider, int *active_zoom,
                        int *active_base_x,
                        int *active_base_y);
void ui_request_capture(UiState *ui, const char *path);
void ui_open_settings(UiState *ui);
void ui_open_map_options(UiState *ui);
void ui_open_locations(UiState *ui);
int ui_open_search(UiState *ui, uint64_t now_us);
int ui_resume_search_keyboard(UiState *ui, uint64_t now_us);
void ui_close_overlay(UiState *ui);
void ui_move_selection(UiState *ui, int direction, const AppSnapshot *snapshot);
int ui_take_search_query(UiState *ui, uint64_t now_us,
                         char *out, size_t out_size);
int ui_take_search_submit(UiState *ui);
int ui_take_search_close(UiState *ui);
void ui_update_ime(UiState *ui, uint64_t now_us);
void ui_draw(UiState *ui, const AppSnapshot *snapshot, uint64_t now_us);

#endif
