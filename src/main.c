#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include "audio.h"
#include "net.h"
#include "ui.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define CAPTURE_REQUEST "ux0:data/vita-weather/capture-view.txt"
#define CAPTURE_OUTPUT "ux0:data/vita-weather/visual.ppm"

int _newlib_heap_size_user = 64 * 1024 * 1024;

/* VitaSDK's FreeType archive references the optional bzip2 stream backend,
 * but this SDK does not ship libbz2. Vita Weather loads only a normal,
 * uncompressed TTF; these fail-closed symbols are therefore unreachable in
 * normal use and prevent FreeType from pretending a bzip2 stream succeeded. */
int BZ2_bzDecompressInit(void *stream, int verbosity, int small)
{
    (void)stream;
    (void)verbosity;
    (void)small;
    return -1;
}

int BZ2_bzDecompress(void *stream)
{
    (void)stream;
    return -1;
}

int BZ2_bzDecompressEnd(void *stream)
{
    (void)stream;
    return -1;
}

static int touch_action(UiState *ui, const AppSnapshot *snapshot,
                        int touch_x, int touch_y, uint64_t now_us)
{
    float x = touch_x * (SCREEN_W / 1920.0f);
    float y = touch_y * (SCREEN_H / 1088.0f);
    if (ui->screen == UI_SCREEN_SEARCH) {
        if (!ui->ime_active && x >= 122.0f && x <= 838.0f &&
            y >= 76.0f && y <= 118.0f) {
            ui_resume_search_keyboard(ui, now_us);
            app_log("touch action: search keyboard reopened");
            return 0;
        }
        if (!ui->ime_active && x >= 120.0f && x <= 840.0f &&
            y >= 126.0f && y < 126.0f + snapshot->search_result_count * 33.0f) {
            int row = (int)((y - 126.0f) / 33.0f);
            if (row >= 0 && row < snapshot->search_result_count) {
                ui->search_selected = row;
                app_select_suggestion(row);
                ui_close_overlay(ui);
                ui_close_overlay(ui);
                app_log("touch action: search result=%d", row);
            }
        } else if (!ui->ime_active && y >= 488.0f && x >= 632.0f) {
            ui_close_overlay(ui);
            app_log("touch action: search back");
        }
        return 0;
    }
    if (ui->screen == UI_SCREEN_LOCATIONS) {
        int rows = snapshot->saved_location_count + 1;
        if (x >= 138.0f && x <= 822.0f && y >= 130.0f &&
            y < 130.0f + rows * 37.0f) {
            int row = (int)((y - 130.0f) / 37.0f);
            ui->location_selected = row;
            if (row == 0) {
                app_queue_gps();
                app_log("touch action: native location requested");
            } else {
                app_select_saved_location(row - 1);
                ui_close_overlay(ui);
                app_log("touch action: saved location=%d", row - 1);
            }
        } else if (y >= 488.0f && x >= 252.0f && x < 480.0f) {
            ui_open_search(ui, now_us);
            app_log("touch action: location search");
        } else if (y >= 488.0f && x >= 480.0f && x < 708.0f &&
                   ui->location_selected > 0) {
            app_remove_saved_location(ui->location_selected - 1);
            app_log("touch action: remove saved location=%d",
                    ui->location_selected - 1);
        } else if (y >= 488.0f && x >= 708.0f) {
            ui_close_overlay(ui);
            app_log("touch action: locations back");
        }
        return 0;
    }
    if (ui->screen == UI_SCREEN_SETTINGS) {
        static const int starts[3] = {
            APP_SETTING_TEMPERATURE, APP_SETTING_THEME,
            APP_SETTING_MAP_PROVIDER
        };
        static const int counts[3] = {3, 4, 2};
        if (y >= 94.0f && y < 136.0f && x >= 50.0f && x < 910.0f) {
            int tab = (int)((x - 50.0f) / 290.0f);
            float tab_x = 50.0f + tab * 290.0f;
            if (tab >= 0 && tab < 3 && x < tab_x + 274.0f) {
                while (ui->settings_tab != tab)
                    ui_settings_tab_move(ui,
                        (tab - ui->settings_tab + 3) % 3 == 1 ? 1 : -1);
            }
            return 0;
        }
        if (x >= 64.0f && x <= 896.0f) {
            int tab = ui->settings_tab;
            for (int item = 0; item < counts[tab]; ++item) {
                float row_y = 164.0f + item * 63.0f;
                if (y >= row_y && y < row_y + 52.0f) {
                    int row = starts[tab] + item;
                    ui->settings_selected = row;
                    app_cycle_setting((AppSetting)row, 1);
                    app_log("touch action: setting=%d", row);
                    return 0;
                }
            }
        }
        if (y >= 488.0f && x >= 632.0f) {
            ui_close_overlay(ui);
            app_log("touch action: settings back");
        }
        return 0;
    }
    if (ui->screen == UI_SCREEN_MAP_OPTIONS) {
        if (y >= 488.0f && x >= 632.0f) {
            ui_close_overlay(ui);
            app_log("touch action: map options back");
            return 0;
        }
        if (x >= 198.0f && x <= 762.0f) {
            int row = -1;
            if (y >= 122.0f && y < 302.0f)
                row = (int)((y - 122.0f) / 45.0f);
            else if (y >= 337.0f && y < 378.0f)
                row = APP_MAP_LAYER_COUNT;
            if (row >= 0 && row < APP_MAP_LAYER_COUNT + 1) {
                ui->map_options_selected = row;
                if (row < APP_MAP_LAYER_COUNT)
                    app_set_map_layer((AppMapLayer)row);
                else
                    app_cycle_setting(APP_SETTING_MAP_PROVIDER, 1);
                app_log("touch action: map option=%d", row);
            }
        }
        return 0;
    }
    if (ui->screen == UI_SCREEN_DAY_METRICS) {
        if (x >= 267.0f && x <= 693.0f && y >= 117.0f &&
            y < 117.0f + DAY_METRIC_COUNT * 47.0f) {
            int row = (int)((y - 117.0f) / 47.0f);
            ui->day_metric_selected = row;
            ui_confirm_day_metric(ui);
            app_log("touch action: day metric=%d", row);
        } else if (y >= 488.0f && x >= 632.0f) {
            ui_close_overlay(ui);
            app_log("touch action: day metric back");
        }
        return 0;
    }
    if (ui->screen == UI_SCREEN_DAY_DETAIL) {
        if (y <= 92.0f && x >= 220.0f && x <= 740.0f) {
            ui_open_day_metrics(ui);
            app_log("touch action: day metric chooser");
            return 0;
        }
        float selector_y = 424.0f - ui->day_detail_scroll;
        if (ui->day_detail_metric == DAY_METRIC_CONDITIONS &&
            y >= selector_y && y <= selector_y + 36.0f &&
            x >= 101.0f && x <= 839.0f) {
            int feels_like = x >= 470.0f;
            if (ui->day_detail_feels_like != feels_like)
                ui_day_detail_toggle_metric(ui, feels_like ? 1 : -1);
            app_log("touch action: day temperature=%s",
                    feels_like ? "feels-like" : "actual");
            return 0;
        }
        if (y >= 488.0f) {
            if (x >= 252.0f && x < 480.0f) {
                ui_open_day_metrics(ui);
                app_log("touch action: day metric chooser");
            } else if (x >= 480.0f && x < 708.0f &&
                       ui->day_detail_metric == DAY_METRIC_CONDITIONS) {
                ui_day_detail_toggle_metric(ui, 1);
            } else if (x >= 708.0f) {
                ui_close_overlay(ui);
                app_log("touch action: day detail back");
            }
        }
        return 0;
    }
    if (ui->screen != UI_SCREEN_WEATHER) return 0;
    if (y >= 10.0f && y <= 67.0f && x >= 280.0f && x <= 780.0f) {
        int view = x < 374.0f ? VIEW_NOW :
                   x < 466.0f ? VIEW_HOURLY :
                   x < 570.0f ? VIEW_DAILY :
                   x < 653.0f ? VIEW_MAP : VIEW_DETAILS;
        ui_set_view(ui, view, now_us);
        app_log("touch action: view=%d", view);
        return 0;
    }
    if (y < 66.0f && x < 280.0f) return 1;
    if (ui->view == VIEW_DAILY && x >= 38.0f && x <= 922.0f &&
        y >= 141.0f && y < 456.0f) {
        int day = ui->daily_offset + (int)((y - 141.0f) / 45.0f);
        if (day >= 0 && day < snapshot->weather.day_count) {
            ui->daily_selected = day;
            ui_open_day_detail(ui, &snapshot->weather);
            app_log("touch action: day=%d", day + 1);
        }
        return 0;
    }
    if (ui->view == VIEW_MAP && !ui->map_fullscreen &&
        x >= 38.0f && x <= 610.0f && y >= 440.0f && y < 481.0f) {
        ui_map_recenter(ui, &snapshot->weather, now_us);
        app_log("touch action: map location recenter");
        return 0;
    }
    if (y >= 488.0f) {
        if (ui->view == VIEW_MAP) {
            int group = (int)((x - 24.0f) / 152.0f);
            if (group == 0) ui_map_toggle_fullscreen(ui);
            else if (group == 1) ui_open_map_options(ui);
            else if (group == 2)
                ui_map_time_move(ui, 1, &snapshot->weather);
            else if (group == 5) ui_open_settings(ui);
            app_log("touch action: map footer=%d", group);
        } else if (ui->view == VIEW_DAILY) {
            int group = (int)((x - 24.0f) / 152.0f);
            if (group == 0)
                ui_open_day_detail(ui, &snapshot->weather);
            else if (group == 1)
                return 1;
            else if (group == 2)
                app_toggle_units();
            else if (group == 3)
                ui_set_view(ui, ui->view + 1, now_us);
            else if (group == 4)
                ui_daily_move(ui, 1, &snapshot->weather);
            else if (group == 5)
                ui_open_settings(ui);
            app_log("touch action: weather footer=%d", group);
        } else if (ui->view == VIEW_HOURLY) {
            int group = (int)((x - 24.0f) / (912.0f / 5.0f));
            if (group == 0) return 1;
            if (group == 1) app_toggle_units();
            else if (group == 2)
                ui_hourly_move(ui, 1, &snapshot->weather);
            else if (group == 3)
                ui_set_view(ui, ui->view + 1, now_us);
            else if (group == 4)
                ui_open_settings(ui);
            app_log("touch action: weather footer=%d", group);
        } else {
            int group = (int)((x - 24.0f) / 228.0f);
            if (group == 0) return 1;
            if (group == 1) app_toggle_units();
            else if (group == 2)
                ui_set_view(ui, ui->view + 1, now_us);
            else if (group == 3)
                ui_open_settings(ui);
            app_log("touch action: weather footer=%d", group);
        }
    }
    return 0;
}

static int touch_map_gesture(UiState *ui, const AppSnapshot *snapshot,
                             int start_x, int start_y,
                             int end_x, int end_y, uint64_t now_us)
{
    float x = start_x * (SCREEN_W / 1920.0f);
    float y = start_y * (SCREEN_H / 1088.0f);
    float map_x = ui->map_fullscreen ? 0.0f : 38.0f;
    float top = ui->map_fullscreen ? 0.0f : 96.0f;
    float map_w = ui->map_fullscreen ? SCREEN_W : 884.0f;
    float map_h = ui->map_fullscreen ? SCREEN_H : 340.0f;
    float bottom = top + map_h;
    if (ui->view != VIEW_MAP || x < 0.0f || x > SCREEN_W ||
        y < top || y > bottom)
        return 0;
    int dx = end_x - start_x;
    int dy = end_y - start_y;
    if (ui->map_fullscreen && abs(dx) <= 20 && abs(dy) <= 20 &&
        x >= map_x + map_w - 68.0f) {
        if (y <= top + 64.0f) {
            ui_map_toggle_fullscreen(ui);
            app_log("touch action: map fullscreen close");
        } else if (y <= top + 120.0f) {
            ui_open_map_options(ui);
            app_log("touch action: map fullscreen layers");
        } else {
            return 0;
        }
        return 1;
    }
    float panel_y = top + map_h - 78.0f;
    float track_x = map_x + 48.0f;
    float track_w = map_w - 96.0f;
    if (abs(dx) <= 20 && abs(dy) <= 20 && y >= panel_y - 6.0f &&
        x >= track_x && x <= track_x + track_w) {
        int hour = (int)floorf((x - track_x) / track_w * 23.0f + 0.5f);
        ui_map_set_hour(ui, hour, &snapshot->weather);
        app_log("touch action: map hour=%d", hour);
    } else if (abs(dx) > 20 || abs(dy) > 20) {
        ui_map_pan_pixels(ui, -dx * 0.5f, -dy * 0.5f, now_us);
    }
    return 1;
}

int main(void)
{
    /* Weather cards, glyphs, particles, and the map all submit geometry in
     * one frame.  The old 2 MiB transient pool was exhausted on the map view,
     * which left stale textured vertices and produced gray/triangular flashes. */
    vita2d_init_advanced(8 * 1024 * 1024);
    vita2d_set_vblank_wait(1);
    vita2d_set_clear_color(RGBA8(8, 25, 48, 255));

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                             SCE_TOUCH_SAMPLING_STATE_START);
    UiState ui;
    if (ui_init(&ui) < 0) {
        vita2d_fini();
        sceKernelExitProcess(1);
        return 1;
    }
    if (app_services_init() < 0)
        app_log("warning: background services unavailable; sample UI remains active");

    int map_stress_remaining = 0;
    int map_stress_phase = 0;
    int map_stress_zoom = 0;
    int map_stress_continuous = 0;
    int map_stress_capture = 0;
    uint64_t map_stress_us = 0;
    int capture_weather_code = -1;
    int capture_force_night = 0;
    int capture_classic_background = 0;
    int capture_force_photo = 0;
    int capture_force_sounds = 0;
    int capture_force_static = 0;
    int capture_force_cache = 0;
    int capture_ui_case = 0;
    int capture_delay_frames = -1;
    FILE *capture_request = fopen(CAPTURE_REQUEST, "r");
    if (capture_request) {
        int capture_view = VIEW_NOW;
        if (fscanf(capture_request, "%d", &capture_view) != 1)
            capture_view = VIEW_NOW;
        fclose(capture_request);
        remove(CAPTURE_REQUEST);
        if (capture_view >= 80 && capture_view <= 92) {
            /* Display-only fixtures: never persist test locations/preferences. */
            capture_ui_case = capture_view;
            capture_view = VIEW_NOW;
            if (capture_ui_case == 81 || capture_ui_case == 89 || capture_ui_case == 90) {
                ui_open_settings(&ui);
                if (capture_ui_case == 89) {
                    ui.settings_tab = 1;
                    ui.settings_selected = APP_SETTING_THEME;
                }
            } else if (capture_ui_case == 82 || capture_ui_case == 91) {
                ui_open_locations(&ui);
            } else if (capture_ui_case == 83 || capture_ui_case == 84) {
                ui.screen = UI_SCREEN_SEARCH;
                snprintf(ui.search_query, sizeof(ui.search_query), "%s",
                         capture_ui_case == 83 ? "Springfield" : "No matching city");
            } else if (capture_ui_case == 85 || capture_ui_case == 87) {
                capture_view = VIEW_HOURLY;
            } else if (capture_ui_case == 88) {
                capture_view = VIEW_DAILY;
            } else if (capture_ui_case == 92) {
                capture_view = VIEW_DETAILS;
            }
            capture_delay_frames = 45;
        }
        else if (capture_view == 10)
            ui_open_settings(&ui);
        else if (capture_view == 11)
            ui_open_locations(&ui);
        else if (capture_view == 12)
            ui_open_search(&ui, sceKernelGetProcessTimeWide());
        else if (capture_view == 13 || capture_view == 43 ||
                 capture_view == 61 || capture_view == 63 ||
                 capture_view == 64 || capture_view == 65 ||
                 capture_view == 66 || capture_view == 69) {
            AppSnapshot detail_snapshot;
            app_snapshot_read(&detail_snapshot);
            ui.daily_selected = 0;
            if (capture_view != 65 && capture_view != 66 &&
                capture_view != 69) {
                ui.daily_selected = detail_snapshot.weather.day_count > 1 ? 1 : 0;
                for (int day = ui.daily_selected + 1;
                     day < detail_snapshot.weather.day_count; ++day) {
                    if (detail_snapshot.weather.days[day].precipitation_percent >
                        detail_snapshot.weather.days[ui.daily_selected]
                            .precipitation_percent)
                        ui.daily_selected = day;
                }
            }
            ui_open_day_detail(&ui, &detail_snapshot.weather);
            if (capture_view == 43) {
                ui_day_detail_scroll(&ui, 1200.0f);
                ui.day_detail_scroll = ui.day_detail_scroll_target;
            } else if (capture_view == 61) {
                ui_day_detail_scroll(&ui, 430.0f);
                ui.day_detail_scroll = ui.day_detail_scroll_target;
            } else if (capture_view == 63) {
                ui_open_day_metrics(&ui);
            } else if (capture_view == 64) {
                ui.day_detail_metric = DAY_METRIC_WIND;
            } else if (capture_view == 66) {
                ui.day_detail_metric = DAY_METRIC_PRECIPITATION;
            } else if (capture_view == 69) {
                ui.day_detail_metric = DAY_METRIC_HUMIDITY;
            }
            capture_view = VIEW_DAILY;
        }
        else if (capture_view == 67) {
            capture_view = VIEW_DETAILS;
        }
        else if (capture_view == 68) {
            ui_open_settings(&ui);
            ui.settings_tab = 2;
            ui.settings_selected = APP_SETTING_MAP_PROVIDER;
            capture_view = VIEW_NOW;
        }
        else if (capture_view == 14) {
            ui_open_map_options(&ui);
            capture_view = VIEW_MAP;
        }
        else if (capture_view == 40) {
            map_stress_remaining = 12;
            map_stress_capture = 1;
            capture_view = VIEW_MAP;
            app_log("map stress diagnostic armed: steps=%d",
                    map_stress_remaining);
        }
        else if (capture_view == 41) {
            map_stress_remaining = 6;
            map_stress_zoom = 1;
            map_stress_capture = 1;
            capture_view = VIEW_MAP;
            app_log("map zoom diagnostic armed: steps=%d",
                    map_stress_remaining);
        }
        else if (capture_view == 44) {
            map_stress_remaining = 48;
            map_stress_continuous = 1;
            map_stress_capture = 1;
            capture_view = VIEW_MAP;
            app_log("map continuous-input diagnostic armed: steps=%d",
                    map_stress_remaining);
        }
        else if (capture_view == 45) {
            map_stress_remaining = 48;
            map_stress_continuous = 1;
            map_stress_capture = 1;
            ui.map_fullscreen = 1;
            capture_view = VIEW_MAP;
            app_log("map fullscreen continuous-input diagnostic armed: steps=%d",
                    map_stress_remaining);
        }
        else if (capture_view == 42) {
            ui.map_fullscreen = 1;
            capture_view = VIEW_MAP;
        }
        else if (capture_view >= 30 && capture_view <= 33) {
            AppMapLayer layer = (AppMapLayer)(capture_view - 30);
            AppSnapshot capture_snapshot;
            app_snapshot_read(&capture_snapshot);
            while (capture_snapshot.map_layer != layer) {
                app_cycle_map_layer(1);
                app_snapshot_read(&capture_snapshot);
            }
            capture_view = VIEW_MAP;
        }
        else if (capture_view >= 50 && capture_view <= 56) {
            static const int weather_codes[] = {0, 2, 63, 73, 95, 45, 0};
            capture_weather_code = weather_codes[capture_view - 50];
            capture_force_night = capture_view == 56;
            capture_force_photo = 1;
            capture_view = VIEW_NOW;
        }
        else if (capture_view >= 70 && capture_view <= 76) {
            static const int weather_codes[] = {0, 2, 63, 73, 95, 45, 0};
            capture_weather_code = weather_codes[capture_view - 70];
            capture_force_night = capture_view == 76;
            capture_classic_background = 1;
            capture_view = VIEW_NOW;
        }
        else if (capture_view == 57) {
            capture_weather_code = 2;
            capture_classic_background = 1;
            capture_view = VIEW_NOW;
        }
        else if (capture_view == 58) {
            capture_weather_code = 63;
            capture_force_sounds = 1;
            capture_force_photo = 1;
            capture_view = VIEW_NOW;
        }
        else if (capture_view == 59) {
            capture_weather_code = 63;
            capture_force_static = 1;
            capture_force_photo = 1;
            capture_view = VIEW_NOW;
        }
        else if (capture_view == 60) {
            capture_force_cache = 1;
            capture_view = VIEW_NOW;
        }
        else if (capture_view == 62) {
            AppSnapshot capture_snapshot;
            app_snapshot_read(&capture_snapshot);
            while (capture_snapshot.map_layer != APP_MAP_WIND) {
                app_cycle_map_layer(1);
                app_snapshot_read(&capture_snapshot);
            }
            capture_delay_frames = 150;
            capture_view = VIEW_MAP;
        }
        else if (capture_view < VIEW_NOW || capture_view >= VIEW_COUNT)
            capture_view = VIEW_NOW;
        ui.view = capture_view < VIEW_COUNT ? capture_view : VIEW_NOW;
        ui.previous_view = ui.view;
        ui.transition_start_us = 0;
        ui_request_capture(&ui, CAPTURE_OUTPUT);
        if (capture_weather_code >= 0)
            ui.capture_delay_frames = 90;
        if (capture_delay_frames > 0)
            ui.capture_delay_frames = capture_delay_frames;
        app_log("visual capture requested: view=%d screen=%d",
                ui.view, ui.screen);
    }

    unsigned int previous_buttons = 0;
    int touch_down = 0;
    int touch_x = 0;
    int touch_y = 0;
    int touch_last_x = 0;
    int touch_last_y = 0;
    int touch_dragged = 0;
    uint64_t cadence_start = sceKernelGetProcessTimeWide();
    unsigned int cadence_frames = 0;
    uint64_t map_control_update_us = cadence_start;
    uint64_t map_zoom_repeat_us = 0;

    for (;;) {
        uint64_t now = sceKernelGetProcessTimeWide();
        AppSnapshot input_snapshot;
        app_snapshot_read(&input_snapshot);
        if (map_stress_remaining > 0 && ui.view == VIEW_MAP &&
            ui.screen == UI_SCREEN_WEATHER && ui.map_initialized &&
            (map_stress_continuous ||
             (!input_snapshot.map.loading && !ui.map_navigation_locked)) &&
            (!map_stress_us || now - map_stress_us >=
                (map_stress_continuous ? 50000u : 400000u))) {
            static const int stress_dx[] = {1, 0, -1, 0};
            static const int stress_dy[] = {0, 1, 0, -1};
            int was_pending = ui.map_request_pending;
            int old_zoom = ui.map_zoom;
            if (map_stress_zoom) {
                int direction = map_stress_phase < 2 ? 1 : -1;
                ui_map_zoom(&ui, &input_snapshot.weather,
                            input_snapshot.map_provider, direction, now);
            }
            else if (map_stress_continuous)
                ui_map_pan_pixels(&ui, 22.0f, 5.0f, now);
            else
                ui_map_pan_pixels(&ui, stress_dx[map_stress_phase] * 256.0f,
                                  stress_dy[map_stress_phase] * 256.0f, now);
            int accepted = map_stress_continuous ? 1 :
                           map_stress_zoom ? ui.map_zoom != old_zoom
                                           : !was_pending &&
                                             ui.map_request_pending;
            if (accepted) {
                ++map_stress_phase;
                if (!map_stress_zoom) map_stress_phase &= 3;
                --map_stress_remaining;
                map_stress_us = now;
                app_log("map %s step accepted: remaining=%d",
                        map_stress_continuous ? "continuous-input" :
                        map_stress_zoom ? "zoom" : "stress",
                        map_stress_remaining);
                if (!map_stress_remaining) {
                    if (map_stress_continuous)
                        app_log("map continuous-input diagnostic completed: peak-display-step=%.3f,%.3f tiles",
                                ui.map_present_peak_dx,
                                ui.map_present_peak_dy);
                    else
                        app_log("map %s diagnostic completed",
                                map_stress_zoom ? "zoom" : "stress");
                }
            }
        }
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (map_stress_capture) {
            /* A host controller can retain a mapped shoulder during bounded
             * emulator diagnostics.  Keep the diagnostic on Map so cadence
             * samples measure the intended view; normal runs are untouched. */
            pad.buttons = 0;
            pad.lx = pad.ly = pad.rx = pad.ry = 127;
            previous_buttons = 0;
        }
        unsigned int pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        if (pressed)
            app_log("controller input: buttons=0x%08x screen=%d view=%d",
                    pressed, ui.screen, ui.view);
        float map_control_dt = (float)(now - map_control_update_us) /
                               1000000.0f;
        if (map_control_dt < 0.0f) map_control_dt = 0.0f;
        if (map_control_dt > 0.05f) map_control_dt = 0.05f;
        map_control_update_us = now;

        ui_update_ime(&ui, now);
        char suggestion_query[96];
        if (ui_take_search_query(&ui, now, suggestion_query,
                                 sizeof(suggestion_query)))
            app_queue_suggestions(suggestion_query);
        int search_keyboard_closed_this_frame = 0;
        if (ui_take_search_close(&ui)) {
            ui_hide_search_keyboard(&ui);
            search_keyboard_closed_this_frame = 1;
        }
        if (ui_take_search_submit(&ui)) {
            ui_hide_search_keyboard(&ui);
            search_keyboard_closed_this_frame = 1;
        }

        if (pressed & SCE_CTRL_START) {
            if (ui.screen == UI_SCREEN_SETTINGS)
                ui_close_overlay(&ui);
            else
                ui_open_settings(&ui);
        } else if (ui.screen == UI_SCREEN_WEATHER) {
            if ((pressed & SCE_CTRL_CROSS) && ui.view == VIEW_DAILY)
                ui_open_day_detail(&ui, &input_snapshot.weather);
            else if ((pressed & SCE_CTRL_CROSS) && ui.view == VIEW_MAP)
                ui_map_toggle_fullscreen(&ui);
            if ((pressed & SCE_CTRL_CIRCLE) && ui.view == VIEW_MAP &&
                ui.map_fullscreen)
                ui_map_toggle_fullscreen(&ui);
            if (pressed & SCE_CTRL_SQUARE) ui_open_locations(&ui);
            if (pressed & SCE_CTRL_TRIANGLE) {
                if (ui.view == VIEW_MAP) ui_open_map_options(&ui);
                else app_toggle_units();
            }
            if ((pressed & SCE_CTRL_LTRIGGER) && !ui.map_fullscreen)
                ui_set_view(&ui, ui.view - 1, now);
            if ((pressed & SCE_CTRL_RTRIGGER) && !ui.map_fullscreen)
                ui_set_view(&ui, ui.view + 1, now);
            if (ui.view == VIEW_MAP) {
                float pan_x = ((int)pad.lx - 127) / 128.0f;
                float pan_y = ((int)pad.ly - 127) / 128.0f;
                if (fabsf(pan_x) < 0.18f) pan_x = 0.0f;
                if (fabsf(pan_y) < 0.18f) pan_y = 0.0f;
                if (pan_x != 0.0f || pan_y != 0.0f) {
                    const float pan_speed = ui.map_fullscreen ? 330.0f : 260.0f;
                    ui_map_pan_pixels(&ui, pan_x * pan_speed * map_control_dt,
                                      pan_y * pan_speed * map_control_dt, now);
                }
                int zoom_direction = pad.ry < 60 ? 1 : pad.ry > 194 ? -1 : 0;
                if (zoom_direction &&
                    (!map_zoom_repeat_us ||
                     now - map_zoom_repeat_us >= 240000u)) {
                    ui_map_zoom(&ui, &input_snapshot.weather,
                                input_snapshot.map_provider,
                                zoom_direction, now);
                    map_zoom_repeat_us = now;
                }
                if (!zoom_direction) map_zoom_repeat_us = 0;
                if (pressed & SCE_CTRL_LEFT)
                    ui_map_time_move(&ui, -1, &input_snapshot.weather);
                if (pressed & SCE_CTRL_RIGHT)
                    ui_map_time_move(&ui, 1, &input_snapshot.weather);
            } else {
                if (ui.view == VIEW_DAILY) {
                    if (pressed & SCE_CTRL_UP)
                        ui_daily_move(&ui, -1, &input_snapshot.weather);
                    if (pressed & SCE_CTRL_DOWN)
                        ui_daily_move(&ui, 1, &input_snapshot.weather);
                } else if (ui.view == VIEW_HOURLY) {
                    if (pressed & SCE_CTRL_LEFT)
                        ui_hourly_move(&ui, -1, &input_snapshot.weather);
                    if (pressed & SCE_CTRL_RIGHT)
                        ui_hourly_move(&ui, 1, &input_snapshot.weather);
                }
            }
        } else if (ui.screen == UI_SCREEN_SETTINGS) {
            if (pressed & SCE_CTRL_UP) ui_move_selection(&ui, -1, &input_snapshot);
            if (pressed & SCE_CTRL_DOWN) ui_move_selection(&ui, 1, &input_snapshot);
            if (pressed & SCE_CTRL_LTRIGGER)
                ui_settings_tab_move(&ui, -1);
            if (pressed & SCE_CTRL_RTRIGGER)
                ui_settings_tab_move(&ui, 1);
            if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT))
                app_cycle_setting((AppSetting)ui.settings_selected, 1);
            if (pressed & SCE_CTRL_LEFT)
                app_cycle_setting((AppSetting)ui.settings_selected, -1);
            if (pressed & SCE_CTRL_CIRCLE)
                ui_close_overlay(&ui);
        } else if (ui.screen == UI_SCREEN_MAP_OPTIONS) {
            if (pressed & SCE_CTRL_UP)
                ui_move_selection(&ui, -1, &input_snapshot);
            if (pressed & SCE_CTRL_DOWN)
                ui_move_selection(&ui, 1, &input_snapshot);
            if (pressed & (SCE_CTRL_CROSS | SCE_CTRL_RIGHT)) {
                if (ui.map_options_selected < APP_MAP_LAYER_COUNT)
                    app_set_map_layer((AppMapLayer)ui.map_options_selected);
                else
                    app_cycle_setting(APP_SETTING_MAP_PROVIDER, 1);
            }
            if (pressed & SCE_CTRL_LEFT) {
                if (ui.map_options_selected < APP_MAP_LAYER_COUNT)
                    app_set_map_layer((AppMapLayer)ui.map_options_selected);
                else
                    app_cycle_setting(APP_SETTING_MAP_PROVIDER, -1);
            }
            if (pressed & SCE_CTRL_CIRCLE)
                ui_close_overlay(&ui);
        } else if (ui.screen == UI_SCREEN_LOCATIONS) {
            if (pressed & SCE_CTRL_UP) ui_move_selection(&ui, -1, &input_snapshot);
            if (pressed & SCE_CTRL_DOWN) ui_move_selection(&ui, 1, &input_snapshot);
            if (pressed & SCE_CTRL_CROSS) {
                if (ui.location_selected == 0) {
                    app_queue_gps();
                } else {
                    app_select_saved_location(ui.location_selected - 1);
                    ui_close_overlay(&ui);
                }
            }
            if (pressed & SCE_CTRL_SQUARE)
                ui_open_search(&ui, now);
            if ((pressed & SCE_CTRL_TRIANGLE) && ui.location_selected > 0)
                app_remove_saved_location(ui.location_selected - 1);
            if (pressed & SCE_CTRL_CIRCLE)
                ui_close_overlay(&ui);
        } else if (ui.screen == UI_SCREEN_SEARCH) {
            if (pressed & SCE_CTRL_UP)
                ui_move_selection(&ui, -1, &input_snapshot);
            if (pressed & SCE_CTRL_DOWN)
                ui_move_selection(&ui, 1, &input_snapshot);
            if ((pressed & SCE_CTRL_CROSS) &&
                !search_keyboard_closed_this_frame) {
                if (input_snapshot.search_result_count > 0) {
                    app_select_suggestion(ui.search_selected);
                    ui_close_overlay(&ui);
                    ui_close_overlay(&ui);
                } else if (ui.search_query[0]) {
                    app_queue_search(ui.search_query);
                    ui_close_overlay(&ui);
                    ui_close_overlay(&ui);
                }
            }
            if (pressed & SCE_CTRL_CIRCLE) {
                if (ui.ime_active)
                    ui_hide_search_keyboard(&ui);
                else if (!search_keyboard_closed_this_frame)
                    ui_close_overlay(&ui);
            }
        } else if (ui.screen == UI_SCREEN_DAY_METRICS) {
            if (pressed & SCE_CTRL_UP)
                ui_move_selection(&ui, -1, &input_snapshot);
            if (pressed & SCE_CTRL_DOWN)
                ui_move_selection(&ui, 1, &input_snapshot);
            if (pressed & SCE_CTRL_CROSS)
                ui_confirm_day_metric(&ui);
            if (pressed & SCE_CTRL_CIRCLE)
                ui_close_overlay(&ui);
        } else if (ui.screen == UI_SCREEN_DAY_DETAIL) {
            if (pad.ly > 150)
                ui_day_detail_scroll(&ui, (pad.ly - 150) * 0.09f);
            else if (pad.ly < 104)
                ui_day_detail_scroll(&ui, -(104 - pad.ly) * 0.09f);
            if (pressed & SCE_CTRL_DOWN)
                ui_day_detail_scroll(&ui, 72.0f);
            if (pressed & SCE_CTRL_UP)
                ui_day_detail_scroll(&ui, -72.0f);
            if (pressed & SCE_CTRL_TRIANGLE)
                ui_open_day_metrics(&ui);
            if (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT))
                ui_day_detail_toggle_metric(&ui,
                    (pressed & SCE_CTRL_LEFT) ? -1 : 1);
            if (pressed & SCE_CTRL_CIRCLE)
                ui_close_overlay(&ui);
        }

        int open_locations = 0;
        SceTouchData touch;
        memset(&touch, 0, sizeof(touch));
        if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0) {
            if (touch.reportNum > 0) {
                if (!touch_down) {
                    touch_x = touch.report[0].x;
                    touch_y = touch.report[0].y;
                    touch_last_x = touch_x;
                    touch_last_y = touch_y;
                    touch_dragged = 0;
                } else if (ui.screen == UI_SCREEN_WEATHER &&
                           ui.view == VIEW_MAP) {
                    float start_x = touch_x * (SCREEN_W / 1920.0f);
                    float start_y = touch_y * (SCREEN_H / 1088.0f);
                    float map_x = ui.map_fullscreen ? 0.0f : 38.0f;
                    float map_top = ui.map_fullscreen ? 0.0f : 96.0f;
                    float map_w = ui.map_fullscreen ? SCREEN_W : 884.0f;
                    float map_h = ui.map_fullscreen ? SCREEN_H : 340.0f;
                    float map_bottom = map_top + map_h;
                    if (start_x >= 0.0f && start_x <= SCREEN_W &&
                        start_y >= map_top && start_y <= map_bottom) {
                        int delta_x = touch.report[0].x - touch_last_x;
                        int delta_y = touch.report[0].y - touch_last_y;
                        float panel_y = map_top + map_h - 78.0f;
                        float track_x = map_x + 48.0f;
                        float track_w = map_w - 96.0f;
                        int on_fullscreen_controls = ui.map_fullscreen &&
                            start_x >= map_x + map_w - 68.0f &&
                            start_y <= map_top + 120.0f;
                        if (!on_fullscreen_controls &&
                            start_y >= panel_y - 6.0f &&
                            start_x >= track_x &&
                            start_x <= track_x + track_w) {
                            float current_x = touch.report[0].x *
                                              (SCREEN_W / 1920.0f);
                            int hour = (int)floorf((current_x - track_x) /
                                                  track_w * 23.0f + 0.5f);
                            ui_map_set_hour(&ui, hour,
                                            &input_snapshot.weather);
                        } else if (!on_fullscreen_controls &&
                                   (abs(delta_x) > 1 || abs(delta_y) > 1)) {
                            ui_map_pan_pixels(&ui, -delta_x * 0.5f,
                                              -delta_y * 0.5f, now);
                        }
                        if (abs(touch.report[0].x - touch_x) > 20 ||
                            abs(touch.report[0].y - touch_y) > 20)
                            touch_dragged = 1;
                    }
                } else if (ui.screen == UI_SCREEN_DAY_DETAIL) {
                    int delta_y = touch.report[0].y - touch_last_y;
                    if (abs(delta_y) > 1)
                        ui_day_detail_scroll(&ui, -delta_y * 0.5f);
                    if (abs(touch.report[0].x - touch_x) > 20 ||
                        abs(touch.report[0].y - touch_y) > 20)
                        touch_dragged = 1;
                } else if (ui.screen == UI_SCREEN_WEATHER &&
                           ui.view == VIEW_HOURLY) {
                    if (abs(touch.report[0].x - touch_x) > 20 ||
                        abs(touch.report[0].y - touch_y) > 20)
                        touch_dragged = 1;
                }
                touch_last_x = touch.report[0].x;
                touch_last_y = touch.report[0].y;
                touch_down = 1;
            } else if (touch_down) {
                if (touch_dragged && ui.screen == UI_SCREEN_WEATHER &&
                    ui.view == VIEW_HOURLY) {
                    int dx = touch_last_x - touch_x;
                    if (abs(dx) > 40) {
                        int steps = abs(dx) / 150;
                        if (steps < 1) steps = 1;
                        if (steps > 6) steps = 6;
                        for (int step = 0; step < steps; ++step)
                            ui_hourly_move(&ui, dx < 0 ? 1 : -1,
                                           &input_snapshot.weather);
                        app_log("touch gesture: hourly swipe steps=%d direction=%d",
                                steps, dx < 0 ? 1 : -1);
                    }
                } else if (touch_dragged &&
                           ui.screen == UI_SCREEN_DAY_DETAIL) {
                    app_log("touch gesture: day scroll target=%.1f/%.1f",
                            ui.day_detail_scroll_target,
                            ui.day_detail_max_scroll);
                } else if (touch_dragged &&
                           ui.screen == UI_SCREEN_WEATHER &&
                           ui.view == VIEW_MAP) {
                    app_log("touch gesture: map drag center=%.3f,%.3f zoom=%d",
                            ui.map_camera_x, ui.map_camera_y, ui.map_zoom);
                } else if (!touch_dragged &&
                    (ui.screen != UI_SCREEN_WEATHER ||
                    !touch_map_gesture(&ui, &input_snapshot,
                                       touch_x, touch_y,
                                       touch_last_x, touch_last_y, now)))
                    open_locations |= touch_action(&ui, &input_snapshot,
                                                   touch_x, touch_y, now);
                touch_down = 0;
                touch_dragged = 0;
            }
        }

        if (open_locations)
            ui_open_locations(&ui);

        AppSnapshot snapshot;
        app_snapshot_read(&snapshot);
        if (capture_weather_code >= 0) {
            snapshot.weather.weather_code = capture_weather_code;
            snapshot.weather.is_day = capture_force_night ? 0 : 1;
        }
        if (capture_classic_background)
            snapshot.photo_background = 0;
        if (capture_force_photo)
            snapshot.photo_background = 1;
        if (capture_force_sounds)
            snapshot.weather_sounds = 1;
        if (capture_force_static)
            snapshot.motion = APP_MOTION_OFF;
        if (capture_force_cache)
            snapshot.weather.source = WEATHER_SOURCE_CACHE;
        if (capture_ui_case) {
            if (capture_ui_case == 80) {
                snprintf(snapshot.weather.location, sizeof(snapshot.weather.location),
                         "San Fernando del Valle de Catamarca");
                snprintf(snapshot.weather.region, sizeof(snapshot.weather.region),
                         "Catamarca Province, Argentina · Long location example");
                snapshot.weather.source = WEATHER_SOURCE_CACHE;
                snapshot.busy = 1;
            }
            if (capture_ui_case == 81 || capture_ui_case == 88)
                snapshot.language = APP_LANGUAGE_GERMAN;
            if (capture_ui_case == 86)
                snapshot.language = APP_LANGUAGE_SPANISH;
            if (capture_ui_case == 87 || capture_ui_case == 90)
                snapshot.language = APP_LANGUAGE_FRENCH;
            if (capture_ui_case == 82) {
                snapshot.saved_location_count = 8;
                for (int i = 0; i < 8; ++i) {
                    snprintf(snapshot.saved_locations[i].name,
                             sizeof(snapshot.saved_locations[i].name),
                             "San Fernando del Valle de Catamarca %d", i + 1);
                    snprintf(snapshot.saved_locations[i].region,
                             sizeof(snapshot.saved_locations[i].region),
                             "Catamarca Province, Argentina · Long region example");
                }
            }
            if (capture_ui_case == 83 || capture_ui_case == 84) {
                snapshot.search_busy = 0;
                snapshot.search_result_count = capture_ui_case == 83 ? 5 : 0;
                snapshot.search_notice[0] = '\0';
                for (int i = 0; i < snapshot.search_result_count; ++i) {
                    snprintf(snapshot.search_results[i].name,
                             sizeof(snapshot.search_results[i].name),
                             "Springfield %d", i + 1);
                    snprintf(snapshot.search_results[i].region,
                             sizeof(snapshot.search_results[i].region),
                             "A deliberately long region name, United States of America");
                }
            }
            if (capture_ui_case == 85) snapshot.weather.hour_count = 0;
            if (capture_ui_case == 91) snapshot.saved_location_count = 0;
            if (capture_ui_case == 92) snapshot.theme = APP_THEME_NIGHT;
        }
        if (map_stress_capture)
            snapshot.weather_sounds = 0;
        if (ui.view == VIEW_MAP) {
            ui_map_focus(&ui, &snapshot.weather);
            ui_map_constrain_provider(&ui, snapshot.map_provider);
            if (!snapshot.map.zoom)
                ui.map_request_pending = 1;
            int zoom;
            int center_x;
            int center_y;
            int active_provider;
            int active_zoom;
            int active_base_x;
            int active_base_y;
            if (ui_map_take_request(&ui, &zoom, &center_x, &center_y,
                                    &active_provider, &active_zoom,
                                    &active_base_x,
                                    &active_base_y))
                app_queue_map(zoom, center_x, center_y, active_provider,
                              active_zoom, active_base_x, active_base_y);
            app_snapshot_read(&snapshot);
        }
        weather_audio_update(&snapshot.weather, snapshot.weather_sounds);
        ui_draw(&ui, &snapshot, now);
        ++cadence_frames;
        uint64_t elapsed = now - cadence_start;
        if (elapsed >= 5000000) {
            double fps = cadence_frames * 1000000.0 / (double)elapsed;
            app_log("render cadence: %.2f fps over %u frames; view=%d busy=%d",
                    fps, cadence_frames, ui.view, snapshot.busy);
            cadence_start = now;
            cadence_frames = 0;
        }
    }

}
