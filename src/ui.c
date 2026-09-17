#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/sysmodule.h>
#include "map_math.h"
#include "i18n.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define SCREEN_STRIDE SCREEN_W
#define PI_F 3.14159265358979323846f
#define TRANSITION_US 220000u

typedef struct Palette {
    uint32_t text;
    uint32_t muted;
    uint32_t surface;
    uint32_t surface_alt;
    uint32_t divider;
} Palette;

static unsigned int g_draw_alpha = 255;
static const unsigned int g_font_sizes[] = {12, 14, 16, 18, 23, 30, 98};

static uint32_t faded(uint32_t color)
{
    unsigned int alpha = (color >> 24) & 0xff;
    alpha = (alpha * g_draw_alpha + 127) / 255;
    return (color & 0x00ffffffu) | (alpha << 24);
}

static float clamp01(float value)
{
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static float ease_out_cubic(float value)
{
    float inverse = 1.0f - clamp01(value);
    return 1.0f - inverse * inverse * inverse;
}

static Palette palette_for(int is_day)
{
    Palette palette;
    if (is_day) {
        palette.text = RGBA8(250, 253, 255, 255);
        palette.muted = RGBA8(220, 235, 248, 235);
        palette.surface = RGBA8(17, 45, 73, 168);
        palette.surface_alt = RGBA8(238, 247, 255, 28);
        palette.divider = RGBA8(225, 240, 252, 35);
    } else {
        palette.text = RGBA8(245, 248, 255, 255);
        palette.muted = RGBA8(169, 184, 213, 255);
        palette.surface = RGBA8(14, 29, 66, 176);
        palette.surface_alt = RGBA8(225, 235, 255, 24);
        palette.divider = RGBA8(175, 195, 225, 34);
    }
    return palette;
}

static void rect(float x, float y, float width, float height, uint32_t color)
{
    vita2d_draw_rectangle(x, y, width, height, faded(color));
}

static void circle(float x, float y, float radius, uint32_t color)
{
    vita2d_draw_fill_circle(x, y, radius, faded(color));
}

static void line_width(float x0, float y0, float x1, float y1,
                       float half_width, uint32_t color)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float length = sqrtf(dx * dx + dy * dy);
    if (length < 0.01f) return;
    float nx = -dy * half_width / length;
    float ny = dx * half_width / length;
    uint32_t vertex_color = faded(color);
    vita2d_color_vertex *vertices =
        vita2d_pool_memalign(sizeof(*vertices) * 6,
                             sizeof(vita2d_color_vertex));
    if (!vertices) {
        vita2d_draw_rectangle(x0, y0, 2, 2, vertex_color);
        return;
    }
    vita2d_color_vertex a = {x0 + nx, y0 + ny, 0.5f, vertex_color};
    vita2d_color_vertex b = {x1 + nx, y1 + ny, 0.5f, vertex_color};
    vita2d_color_vertex c = {x1 - nx, y1 - ny, 0.5f, vertex_color};
    vita2d_color_vertex d = {x0 - nx, y0 - ny, 0.5f, vertex_color};
    vertices[0] = a;
    vertices[1] = b;
    vertices[2] = c;
    vertices[3] = a;
    vertices[4] = c;
    vertices[5] = d;
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, vertices, 6);
}

static void line(float x0, float y0, float x1, float y1, uint32_t color)
{
    line_width(x0, y0, x1, y1, 0.65f, color);
}

static void horizontal_gradient(float x, float y, float width, float height,
                                 uint32_t left, uint32_t right)
{
    vita2d_color_vertex *v = vita2d_pool_memalign(sizeof(*v) * 6, sizeof(*v));
    if (!v) { rect(x, y, width, height, left); return; }
    left = faded(left);
    right = faded(right);
    v[0] = (vita2d_color_vertex){x, y, 0.5f, left};
    v[1] = (vita2d_color_vertex){x + width, y, 0.5f, right};
    v[2] = (vita2d_color_vertex){x + width, y + height, 0.5f, right};
    v[3] = v[0]; v[4] = v[2];
    v[5] = (vita2d_color_vertex){x, y + height, 0.5f, left};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 6);
}

static void chart_fill_segment(float x0, float y0, float x1, float y1,
                               float bottom, uint32_t top0, uint32_t top1,
                               uint32_t base)
{
    vita2d_color_vertex *v = vita2d_pool_memalign(sizeof(*v) * 6, sizeof(*v));
    if (!v) return;
    v[0] = (vita2d_color_vertex){x0, y0, 0.5f, faded(top0)};
    v[1] = (vita2d_color_vertex){x1, y1, 0.5f, faded(top1)};
    v[2] = (vita2d_color_vertex){x1, bottom, 0.5f, faded(base)};
    v[3] = v[0]; v[4] = v[2];
    v[5] = (vita2d_color_vertex){x0, bottom, 0.5f, faded(base)};
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 6);
}

static void draw_rain_field(float seconds, AppMotion motion, int pixelated,
                            int is_day, int storm)
{
    /* The physical-Vita R16 log ends immediately after the first rain frame.
     * Keep weather particles on vita2d-owned primitives instead of submitting
     * one large transient vertex array to GXM. The bounded count also leaves
     * ample room in the per-frame pool for cards, text, and map geometry. */
    int count = motion == APP_MOTION_FULL ? (storm ? 34 : 30)
                                          : (storm ? 18 : 16);
    for (int i = 0; i < count; ++i) {
        float x = fmodf(i * 83.0f, 1030.0f) - 25.0f +
                  sinf(seconds * 0.35f + i * 1.7f) * (storm ? 7.0f : 3.0f);
        float speed = 196.0f + (i % 6) * 19.0f;
        float y = 60.0f + fmodf(i * 67.0f + seconds * speed, 438.0f);
        if (pixelated) {
            float grid = 4.0f;
            float x0 = floorf(x / grid) * grid;
            float y0 = floorf(y / grid) * grid;
            float x1 = x0 + (storm ? 4.0f : 3.0f);
            float y1 = y0 + (storm ? 16.0f : 12.0f);
            unsigned int alpha = (motion == APP_MOTION_FULL ? 132u : 88u) +
                                 (unsigned int)(i % 4) * 9u;
            vita2d_draw_rectangle(x0, y0, x1 - x0, y1 - y0,
                                  faded(RGBA8(115, 203, 255, alpha)));
        } else {
            float depth = 0.72f + (i % 5) * 0.10f;
            float length = ((storm ? 31.0f : 25.0f) + (i % 4) * 3.0f) * depth;
            float slant = (storm ? 9.0f : 5.0f) * depth;
            float x1 = x - slant;
            float y1 = y + length;
            unsigned int tail_alpha = (motion == APP_MOTION_FULL ? 112u : 78u) +
                                      (unsigned int)(i % 4) * 10u;
            if (storm) tail_alpha += 20u;
            if (!is_day) tail_alpha += 8u;
            /* Fade drops in and out at the vertical wrap so they do not pop.
             * A faint wider quad plus a narrow core approximates a soft edge
             * without a texture, shader, or unbounded particle buffer. */
            float local_y = y - 60.0f;
            float edge_fade = clamp01(local_y / 30.0f);
            float bottom_fade = clamp01((438.0f - local_y) / 36.0f);
            if (bottom_fade < edge_fade) edge_fade = bottom_fade;
            tail_alpha = (unsigned int)(tail_alpha * edge_fade);
            if (tail_alpha < 4u) continue;
            line_width(x, y, x1, y1, 1.25f,
                       RGBA8(177, 218, 250, tail_alpha / 3u));
            line_width(x, y, x1, y1, 0.52f,
                       RGBA8(211, 237, 255, tail_alpha));
            float head = 1.15f + depth * 0.42f;
            circle(x1, y1 + head * 0.35f, head,
                   RGBA8(229, 245, 255,
                         tail_alpha < 205u ? tail_alpha + 34u : 239u));
        }
    }
}

static void draw_snow_field(float seconds, AppMotion motion, int pixelated)
{
    int count = motion == APP_MOTION_FULL ? 30 : 15;
    for (int i = 0; i < count; ++i) {
        float speed = 24.0f + (i % 5) * 5.5f;
        float y = 66.0f + fmodf(i * 59.0f + seconds * speed, 440.0f);
        float x = fmodf(i * 101.0f, 1000.0f) - 20.0f +
                  sinf(seconds * (0.65f + (i % 3) * 0.12f) + i) *
                      (8.0f + (i % 4) * 2.0f);
        float size = 2.4f + (i % 5) * 0.72f;
        unsigned int alpha = 150u + (unsigned int)(i % 5) * 18u;
        uint32_t color = faded(RGBA8(247, 251, 255, alpha));
        if (pixelated) {
            float grid = 4.0f;
            float px = floorf(x / grid) * grid;
            float py = floorf(y / grid) * grid;
            float block = i % 3 ? 4.0f : 6.0f;
            vita2d_draw_rectangle(px, py, block, block, color);
        } else {
            vita2d_draw_line(x, y - size, x, y + size, color);
            vita2d_draw_line(x - size * 0.86f, y - size * 0.5f,
                             x + size * 0.86f, y + size * 0.5f, color);
            vita2d_draw_line(x - size * 0.86f, y + size * 0.5f,
                             x + size * 0.86f, y - size * 0.5f, color);
        }
    }
}

static void rounded_rect(float x, float y, float width, float height,
                         float radius, uint32_t color)
{
    if (radius * 2.0f > width) radius = width * 0.5f;
    if (radius * 2.0f > height) radius = height * 0.5f;
    enum {
        ARC_SEGMENTS = 8,
        EDGE_POINTS = 4 * (ARC_SEGMENTS + 1),
        VERTEX_COUNT = EDGE_POINTS * 3
    };
    vita2d_color_vertex edge[EDGE_POINTS];
    vita2d_color_vertex *vertices =
        vita2d_pool_memalign(sizeof(*vertices) * VERTEX_COUNT,
                             sizeof(vita2d_color_vertex));
    uint32_t vertex_color = faded(color);
    if (!vertices) {
        vita2d_draw_rectangle(x, y, width, height, vertex_color);
        return;
    }
    vita2d_color_vertex center = {x + width * 0.5f,
                                  y + height * 0.5f, 0.5f, vertex_color};
    static const float center_x[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    static const float center_y[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    static const float start_angle[4] = {PI_F, PI_F * 1.5f, 0.0f, PI_F * 0.5f};
    int edge_at = 0;
    for (int corner = 0; corner < 4; ++corner) {
        float cx = x + radius + center_x[corner] * (width - radius * 2.0f);
        float cy = y + radius + center_y[corner] * (height - radius * 2.0f);
        for (int step = 0; step <= ARC_SEGMENTS; ++step) {
            float angle = start_angle[corner] +
                          (PI_F * 0.5f * step) / (float)ARC_SEGMENTS;
            edge[edge_at++] = (vita2d_color_vertex){cx + cosf(angle) * radius,
                                                     cy + sinf(angle) * radius,
                                                     0.5f, vertex_color};
        }
    }
    int at = 0;
    for (int i = 0; i < EDGE_POINTS; ++i) {
        vertices[at++] = center;
        vertices[at++] = edge[i];
        vertices[at++] = edge[(i + 1) % EDGE_POINTS];
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, vertices, (size_t)at);
}

static void surface(float x, float y, float width, float height,
                    float radius, uint32_t color)
{
    rounded_rect(x, y, width, height, radius, color);
}

static void mask_rounded_map_corners(float x, float y, float width,
                                     float height, float radius,
                                     uint32_t mask)
{
    /* GXM scissoring is rectangular. Cover only the few pixels outside the
     * intended corner arcs after drawing the tiles so the inner map and its
     * outer card share the same silhouette on physical hardware. */
    int rows = (int)ceilf(radius);
    for (int row = 0; row < rows; ++row) {
        float dy = radius - row - 0.5f;
        float inside = sqrtf(fmaxf(0.0f, radius * radius - dy * dy));
        float inset = ceilf(radius - inside);
        if (inset < 1.0f) continue;
        rect(x, y + row, inset, 1, mask);
        rect(x + width - inset, y + row, inset, 1, mask);
        rect(x, y + height - row - 1, inset, 1, mask);
        rect(x + width - inset, y + height - row - 1, inset, 1, mask);
    }
}

static int font_bucket(float scale)
{
    unsigned int requested = (unsigned int)(scale * 22.0f + 0.5f);
    int nearest = 0;
    unsigned int distance = requested > g_font_sizes[0]
                                ? requested - g_font_sizes[0]
                                : g_font_sizes[0] - requested;
    for (int i = 1; i < (int)(sizeof(g_font_sizes) / sizeof(g_font_sizes[0])); ++i) {
        unsigned int candidate = requested > g_font_sizes[i]
                                     ? requested - g_font_sizes[i]
                                     : g_font_sizes[i] - requested;
        if (candidate < distance) {
            nearest = i;
            distance = candidate;
        }
    }
    return nearest;
}

static void draw_text(UiState *ui, float x, float y, uint32_t color,
                      float scale, const char *text)
{
    int bucket = font_bucket(scale);
    vita2d_font_draw_text(ui->fonts[bucket], (int)x, (int)y, faded(color),
                          g_font_sizes[bucket], text);
}

static void draw_textf(UiState *ui, float x, float y, uint32_t color,
                       float scale, const char *format, ...)
{
    char text[192];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    draw_text(ui, x, y, color, scale, text);
}

static int text_width(UiState *ui, float scale, const char *text)
{
    int bucket = font_bucket(scale);
    return vita2d_font_text_width(ui->fonts[bucket], g_font_sizes[bucket], text);
}

/* Fit by measured glyph width, keeping the fixed font atlas and complete
 * UTF-8 characters. Long place names never intrude into adjacent controls. */
static void fit_text(UiState *ui, char *out, size_t capacity,
                      float scale, float width, const char *text)
{
    snprintf(out, capacity, "%s", text ? text : "");
    if (text_width(ui, scale, out) <= width) return;
    size_t end = strlen(out);
    while (end) {
        do { --end; } while (end && ((unsigned char)out[end] & 0xc0) == 0x80);
        out[end] = '\0';
        if (end + 4 > capacity) continue;
        memcpy(out + end, "…", 4);
        if (text_width(ui, scale, out) <= width) return;
        out[end] = '\0';
    }
}

static void draw_fitted_text(UiState *ui, float x, float y, float width,
                             uint32_t color, float scale, const char *text)
{
    char fitted[384];
    fit_text(ui, fitted, sizeof(fitted), scale, width, text);
    draw_text(ui, x, y, color, scale, fitted);
}

static void draw_centered_text(UiState *ui, float center, float y,
                               uint32_t color, float scale, const char *text)
{
    draw_text(ui, center - text_width(ui, scale, text) * 0.5f,
              y, color, scale, text);
}

static void draw_check(float x, float y, uint32_t color)
{
    line_width(x - 4, y, x - 1, y + 3, 0.9f, color);
    line_width(x - 1, y + 3, x + 5, y - 4, 0.9f, color);
}

enum MetricIcon {
    ICON_TEMPERATURE, ICON_SUN, ICON_WIND, ICON_RAIN, ICON_HUMIDITY,
    ICON_VISIBILITY, ICON_PRESSURE, ICON_AIR, ICON_MOON, ICON_LOCATION
};

static void draw_metric_icon(float x, float y, int icon, uint32_t color)
{
    if (icon == ICON_TEMPERATURE) {
        rounded_rect(x - 2, y - 8, 4, 12, 2, color);
        circle(x, y + 5, 4, color);
    } else if (icon == ICON_WIND || icon == ICON_AIR) {
        for (int i = 0; i < 3; ++i) {
            float yy = y - 5 + i * 5;
            line_width(x - 8, yy, x + 4 - i * 2, yy, 0.8f, color);
            line_width(x + 4 - i * 2, yy, x + 7 - i * 2,
                       yy - 2, 0.8f, color);
        }
    } else if (icon == ICON_RAIN || icon == ICON_HUMIDITY) {
        line_width(x, y - 8, x - 5, y + 1, 0.8f, color);
        line_width(x, y - 8, x + 5, y + 1, 0.8f, color);
        line_width(x - 5, y + 1, x - 4, y + 5, 0.8f, color);
        line_width(x + 5, y + 1, x + 4, y + 5, 0.8f, color);
        line_width(x - 4, y + 5, x, y + 7, 0.8f, color);
        line_width(x + 4, y + 5, x, y + 7, 0.8f, color);
    } else if (icon == ICON_SUN || icon == ICON_MOON) {
        circle(x, y, 4, color);
        for (int ray = 0; ray < 8; ++ray) {
            float a = ray * PI_F / 4;
            line_width(x + cosf(a) * 6, y + sinf(a) * 6,
                       x + cosf(a) * 9, y + sinf(a) * 9, 0.7f, color);
        }
    } else if (icon == ICON_VISIBILITY) {
        line_width(x - 9, y, x - 3, y - 5, 0.8f, color);
        line_width(x - 3, y - 5, x + 3, y - 5, 0.8f, color);
        line_width(x + 3, y - 5, x + 9, y, 0.8f, color);
        line_width(x + 9, y, x + 3, y + 5, 0.8f, color);
        line_width(x + 3, y + 5, x - 3, y + 5, 0.8f, color);
        line_width(x - 3, y + 5, x - 9, y, 0.8f, color);
        circle(x, y, 2.5f, color);
    } else if (icon == ICON_LOCATION) {
        line_width(x, y - 8, x + 7, y - 2, 0.8f, color);
        line_width(x + 7, y - 2, x, y + 9, 0.8f, color);
        line_width(x, y + 9, x - 7, y - 2, 0.8f, color);
        line_width(x - 7, y - 2, x, y - 8, 0.8f, color);
        circle(x, y - 2, 2, color);
    } else {
        for (int i = 0; i < 12; ++i) {
            float a = PI_F + i * PI_F / 11;
            circle(x + cosf(a) * 8, y + sinf(a) * 8, 1, color);
        }
        line_width(x, y + 3, x + 4, y - 5, 0.9f, color);
    }
}

static void draw_focus(float x, float y, float width, float height)
{
    rounded_rect(x, y, width, height, 14, RGBA8(222, 238, 252, 30));
    rounded_rect(x + 1, y + 10, 3, height - 20, 1.5f,
                 RGBA8(169, 218, 249, 235));
}

static float draw_wrapped_text(UiState *ui, float x, float y,
                               float max_width, float line_height,
                               uint32_t color, float scale,
                               const char *text, int max_lines)
{
    char remaining[384];
    char line_buffer[192];
    snprintf(remaining, sizeof(remaining), "%s", text ? text : "");
    char *cursor = remaining;
    int lines = 0;
    while (*cursor && lines < max_lines) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;
        line_buffer[0] = '\0';
        char *line_start = cursor;
        char *last_space = NULL;
        char *scan = cursor;
        while (*scan) {
            if (*scan == ' ') last_space = scan;
            char saved = *scan;
            *scan = '\0';
            int width = text_width(ui, scale, line_start);
            *scan = saved;
            if (width > (int)max_width) break;
            ++scan;
        }
        char *line_end;
        if (!*scan)
            line_end = scan;
        else if (last_space && last_space >= line_start)
            line_end = last_space;
        else
            line_end = scan;
        size_t length = (size_t)(line_end - line_start);
        if (length >= sizeof(line_buffer)) length = sizeof(line_buffer) - 1;
        memcpy(line_buffer, line_start, length);
        line_buffer[length] = '\0';
        draw_text(ui, x, y + lines * line_height, color, scale, line_buffer);
        ++lines;
        cursor = line_end;
        while (*cursor == ' ') ++cursor;
    }
    return lines * line_height;
}

static int warm_font_atlases(UiState *ui)
{
    if (ui->fonts_warmed) return 0;
    static const char warm_text[] =
        "0123456789: ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz.-/%&°·□△○×↑↓…–";

    /* Finish and swap one invisible command list before normal UI rendering.
     * A separate presented frame is important: starting a second command list
     * against the same back buffer let Vita3K reuse stale transient vertices.
     * The configured clear color makes this a single unobtrusive blue frame.
     * vita2d
     * creates TTF glyph textures lazily; doing that for AQI/PM2.5 after map
     * tiles were submitted caused a one-frame stale-texture flash in Vita3K. */
    vita2d_start_drawing();
    vita2d_clear_screen();
    for (int i = 0; i < 3; ++i) {
        const char *text = warm_text;
        vita2d_font_draw_text(ui->fonts[i], -2048, -2048,
                              RGBA8(255, 255, 255, 0),
                              g_font_sizes[i], text);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
    ui->fonts_warmed = 1;
    return 1;
}

static uint8_t lerp_byte(int start, int end, float amount)
{
    return (uint8_t)(start + (end - start) * amount);
}

static void draw_background_cloud(float x, float y, float size, uint32_t color)
{
    /* Quantized motion keeps the classic pixel character, while a few
     * fixed-segment rounded rectangles restore the soft cloud silhouette.
     * Avoid radius-dependent vita2d circle fans in this full-screen path. */
    const float grid = 4.0f;
    float px = floorf(x / grid) * grid;
    float py = floorf(y / grid) * grid;
    float s = floorf(size / grid) * grid;
    rounded_rect(px - s * 0.49f, py - s * 0.01f,
                 s * 0.98f, s * 0.31f, s * 0.15f, color);
    rounded_rect(px - s * 0.39f, py - s * 0.13f,
                 s * 0.38f, s * 0.34f, s * 0.17f, color);
    rounded_rect(px - s * 0.15f, py - s * 0.34f,
                 s * 0.47f, s * 0.52f, s * 0.23f, color);
    rounded_rect(px + s * 0.17f, py - s * 0.16f,
                 s * 0.35f, s * 0.37f, s * 0.17f, color);
}

enum {
    WEATHER_BG_CLEAR = 0,
    WEATHER_BG_CLOUDY,
    WEATHER_BG_RAIN,
    WEATHER_BG_SNOW,
    WEATHER_BG_STORM,
    WEATHER_BG_FOG,
    WEATHER_BG_NIGHT,
    WEATHER_BG_COUNT
};

static const char *const g_weather_background_paths[WEATHER_BG_COUNT] = {
    "app0:weather-clear.png",
    "app0:weather-day.png",
    "app0:weather-rain.png",
    "app0:weather-snow.png",
    "app0:weather-storm.png",
    "app0:weather-fog.png",
    "app0:weather-night.png"
};

static const char *const g_weather_background_fast_paths[WEATHER_BG_COUNT] = {
    "app0:weather-clear.rgba8",
    "app0:weather-day.rgba8",
    "app0:weather-rain.rgba8",
    "app0:weather-snow.rgba8",
    "app0:weather-storm.rgba8",
    "app0:weather-fog.rgba8",
    "app0:weather-night.rgba8"
};

static void cancel_weather_background_load(UiState *ui)
{
    if (ui->weather_bg_file) {
        fclose((FILE *)ui->weather_bg_file);
        ui->weather_bg_file = NULL;
    }
    if (ui->weather_pending_bg) {
        vita2d_free_texture(ui->weather_pending_bg);
        ui->weather_pending_bg = NULL;
    }
    ui->weather_pending_bg_key = -1;
    ui->weather_pending_row = 0;
}

static int weather_background_key(const WeatherData *weather)
{
    int group = weather_condition_group(weather->weather_code);
    if (!weather->is_day && group <= 1) return WEATHER_BG_NIGHT;
    if (group < WEATHER_BG_CLEAR || group > WEATHER_BG_FOG)
        return WEATHER_BG_CLOUDY;
    return group;
}

static void update_weather_background(UiState *ui, const WeatherData *weather,
                                      int photo_background, uint64_t now_us)
{
    const uint64_t transition_us = 650000u;
    if (ui->weather_retired_bg &&
        now_us - ui->weather_bg_transition_us >= transition_us) {
        /* ui_draw fences the previous scene before any resource maintenance. */
        vita2d_free_texture(ui->weather_retired_bg);
        ui->weather_retired_bg = NULL;
    }

    if (!photo_background) {
        cancel_weather_background_load(ui);
        return;
    }
    int desired = weather_background_key(weather);
    if (desired == ui->weather_bg_key) {
        if (ui->weather_pending_bg_key >= 0)
            cancel_weather_background_load(ui);
        return;
    }
    if (ui->weather_retired_bg) return;
    if (ui->weather_pending_bg_key != desired) {
        enum { WIDTH = 1088, HEIGHT = 544 };
        cancel_weather_background_load(ui);
        FILE *file = fopen(g_weather_background_fast_paths[desired], "rb");
        vita2d_texture *texture = file
            ? vita2d_create_empty_texture_format(
                  WIDTH, HEIGHT, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR)
            : NULL;
        if (!file || !texture) {
            if (file) fclose(file);
            if (texture) vita2d_free_texture(texture);
            app_log("weather background unavailable: key=%d raw='%s' png='%s'",
                    desired, g_weather_background_fast_paths[desired],
                    g_weather_background_paths[desired]);
            ui->weather_bg_key = desired;
            return;
        }
        ui->weather_bg_file = file;
        ui->weather_pending_bg = texture;
        ui->weather_pending_bg_key = desired;
        ui->weather_pending_row = 0;
        ui->weather_pending_start_us = now_us;
        app_log("weather background streaming: key=%d format=rgba8-abgr",
                desired);
    }

    enum { WIDTH = 1088, HEIGHT = 544, BYTES_PER_PIXEL = 4, ROWS_PER_FRAME = 8 };
    FILE *file = (FILE *)ui->weather_bg_file;
    vita2d_texture *next = ui->weather_pending_bg;
    unsigned int stride = vita2d_texture_get_stride(next);
    unsigned char *pixels = vita2d_texture_get_datap(next);
    size_t row_size = WIDTH * BYTES_PER_PIXEL;
    int end_row = ui->weather_pending_row + ROWS_PER_FRAME;
    if (end_row > HEIGHT) end_row = HEIGHT;
    int valid = stride >= row_size;
    while (valid && ui->weather_pending_row < end_row) {
        int row = ui->weather_pending_row++;
        valid = fread(pixels + (size_t)row * stride, row_size, 1, file) == 1;
    }
    if (!valid) {
        app_log("weather background stream failed: key=%d row=%d",
                desired, ui->weather_pending_row);
        cancel_weather_background_load(ui);
        ui->weather_bg_key = desired;
        return;
    }
    if (ui->weather_pending_row < HEIGHT) return;
    if (fgetc(file) != EOF) {
        app_log("weather background size mismatch: key=%d", desired);
        cancel_weather_background_load(ui);
        ui->weather_bg_key = desired;
        return;
    }
    fclose(file);
    ui->weather_bg_file = NULL;
    ui->weather_pending_bg = NULL;
    ui->weather_pending_bg_key = -1;
    vita2d_texture_set_filters(next, SCE_GXM_TEXTURE_FILTER_LINEAR,
                               SCE_GXM_TEXTURE_FILTER_LINEAR);
    ui->weather_retired_bg = ui->weather_bg;
    ui->weather_bg = next;
    ui->weather_bg_key = desired;
    ui->weather_bg_transition_us = now_us;
    app_log("weather background changed: key=%d format=rgba8-abgr stream=%.1fms",
            desired, (now_us - ui->weather_pending_start_us) / 1000.0);
}

static void draw_background_texture(vita2d_texture *texture, float seconds,
                                    AppMotion motion, unsigned int alpha)
{
    if (!texture || !alpha) return;
    float width = (float)vita2d_texture_get_width(texture);
    float height = (float)vita2d_texture_get_height(texture);
    float scale = SCREEN_H / height;
    float overflow = width * scale - SCREEN_W;
    float drift = motion == APP_MOTION_OFF
                      ? 0.0f
                      : sinf(seconds * 0.075f) * overflow * 0.34f;
    vita2d_draw_texture_tint_scale(texture, -overflow * 0.5f + drift, 0,
                                   scale, scale,
                                   RGBA8(255, 255, 255, alpha));
}

static void draw_background(UiState *ui, const WeatherData *weather,
                            int photo_background, AppMotion motion,
                            uint64_t now_us)
{
    int is_day = weather->is_day;
    int group = weather_condition_group(weather->weather_code);
    vita2d_texture *background = photo_background ? ui->weather_bg : NULL;
    int has_image = background != NULL;
    float speed = motion == APP_MOTION_FULL ? 1.0f :
                  motion == APP_MOTION_REDUCED ? 0.28f : 0.0f;
    float seconds = (float)(now_us % 60000000u) / 1000000.0f * speed;
    /* Never submit two full-screen photo textures in one frame.  The R17
     * physical log runs normally until a location switch completes, then ends
     * on the first rain-to-cloudy crossfade frame.  Keep the retired texture
     * alive briefly so the preceding GPU frame can finish, but do not draw it;
     * the fully streamed replacement is swapped atomically here. */
    draw_background_texture(background, seconds, motion, 255);
    int top_r = is_day ? 47 : 18;
    int top_g = is_day ? 132 : 28;
    int top_b = is_day ? 202 : 67;
    int bottom_r = is_day ? 126 : 33;
    int bottom_g = is_day ? 193 : 55;
    int bottom_b = is_day ? 226 : 101;
    const int bands = 34;
    for (int i = 0; i < bands; ++i) {
        float t = (float)i / (float)(bands - 1);
        uint32_t color = RGBA8(lerp_byte(top_r, bottom_r, t),
                               lerp_byte(top_g, bottom_g, t),
                               lerp_byte(top_b, bottom_b, t),
                               has_image ? (is_day ? 74 : 96) : 255);
        vita2d_draw_rectangle(0, i * 16, SCREEN_W, 16, color);
    }

    float drift = sinf(seconds * 0.23f) * 15.0f;
    g_draw_alpha = 255;
    if (!photo_background) {
        uint32_t haze = is_day ? RGBA8(241, 249, 255, 28)
                               : RGBA8(120, 151, 225, 18);
        rounded_rect(690 + drift, -58, 232, 232, 116, haze);
        rounded_rect(831 + drift * 0.6f, 30, 164, 164, 82, haze);
        rounded_rect(658 + drift * 0.35f, -40, 134, 134, 67, haze);
    }

    if (!is_day) {
        int star_count = motion == APP_MOTION_OFF ? 14 : 22;
        for (int i = 0; i < star_count; ++i) {
            float x = fmodf(41.0f + i * 83.0f, SCREEN_W - 20.0f);
            float y = 74.0f + fmodf(i * 47.0f, 350.0f);
            float twinkle = motion == APP_MOTION_OFF ? 0.55f :
                0.50f + 0.35f * sinf(seconds * 0.9f + i * 1.7f);
            circle(x, y, 1.1f + (i % 3) * 0.35f,
                   RGBA8(225, 235, 255, (unsigned int)(70 + 120 * twinkle)));
        }
    }

    if (group == 0) {
        if (is_day) {
            /* The clear scene uses a broad photographic bloom rather than a
             * small icon. Fixed-segment rounded discs
             * keep that softness without radius-dependent circle fans. */
            float sun_x = 150.0f + drift * 0.10f;
            const float sun_y = 32.0f;
            if (ui->sun_glow)
                vita2d_draw_texture_tint_scale(
                    ui->sun_glow, sun_x - 198.0f, sun_y - 198.0f,
                    1.55f, 1.55f,
                    RGBA8(255, 255, 255, has_image ? 238 : 205));
            else
                rounded_rect(sun_x - 22, sun_y - 22, 44, 44, 22,
                             RGBA8(255, 244, 207, 210));
        } else {
            circle(816 + drift * 0.2f, 115, 30,
                   RGBA8(231, 239, 255, has_image ? 168 : 60));
            circle(829 + drift * 0.2f, 105, 27,
                   RGBA8(26, 43, 83, has_image ? 166 : 60));
        }
    } else if (!photo_background) {
        int cloud_count = motion == APP_MOTION_REDUCED ? 2 : 3;
        if (motion == APP_MOTION_OFF) cloud_count = 2;
        for (int i = 0; i < cloud_count; ++i) {
            float travel = motion == APP_MOTION_OFF ? 0.0f :
                fmodf(seconds * (7.0f + i * 2.0f) + i * 310.0f, 1280.0f);
            float x = -170.0f + travel;
            float y = 92.0f + i * 105.0f;
            float size = 145.0f + i * 24.0f;
            uint32_t cloud = is_day ? RGBA8(241, 248, 255, 42 + i * 7)
                                    : RGBA8(139, 164, 214, 30 + i * 6);
            draw_background_cloud(x, y, size, cloud);
        }
    }
    if (group == 2 && motion != APP_MOTION_OFF) {
        draw_rain_field(seconds, motion, !photo_background, is_day, 0);
    } else if (group == 3 && motion != APP_MOTION_OFF) {
        draw_snow_field(seconds, motion, !photo_background);
    } else if (group == 4 && motion != APP_MOTION_OFF) {
        draw_rain_field(seconds, motion, !photo_background, is_day, 1);
        float phase = fmodf(seconds, 7.0f);
        if (phase < 0.07f)
            rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(236, 242, 255, 42));
    } else if (group == 5 && motion != APP_MOTION_OFF) {
        int count = motion == APP_MOTION_FULL ? 4 : 2;
        for (int i = 0; i < count; ++i) {
            float travel = fmodf(seconds * (8.0f + i * 1.5f) + i * 290.0f,
                                  1260.0f);
            float x = -220.0f + travel;
            float y = 116.0f + i * 86.0f;
            rounded_rect(x, y, 370, 34 + (i % 2) * 12, 18,
                         RGBA8(226, 237, 242, is_day ? 24 : 18));
        }
    }
}

static void draw_sun(float x, float y, float size, uint32_t color)
{
    float radius = size * 0.24f;
    circle(x, y, radius, color);
    for (int i = 0; i < 8; ++i) {
        float angle = i * PI_F / 4.0f;
        line(x + cosf(angle) * size * 0.34f,
             y + sinf(angle) * size * 0.34f,
             x + cosf(angle) * size * 0.48f,
             y + sinf(angle) * size * 0.48f, color);
    }
}

static void draw_cloud(float x, float y, float size, uint32_t color)
{
    circle(x - size * 0.18f, y + size * 0.05f, size * 0.20f, color);
    circle(x + size * 0.02f, y - size * 0.08f, size * 0.28f, color);
    circle(x + size * 0.27f, y + size * 0.06f, size * 0.18f, color);
    rect(x - size * 0.36f, y + size * 0.05f,
         size * 0.72f, size * 0.21f, color);
}

static void draw_weather_icon(float x, float y, float size, int code, int is_day)
{
    int group = weather_condition_group(code);
    uint32_t cloud = RGBA8(231, 239, 247, 255);
    if (group == 0) {
        if (is_day) draw_sun(x, y, size, RGBA8(255, 197, 77, 255));
        else {
            circle(x, y, size * 0.29f, RGBA8(235, 241, 255, 255));
            circle(x + size * 0.14f, y - size * 0.10f,
                   size * 0.27f, RGBA8(41, 62, 111, 255));
        }
        return;
    }
    if (group == 1 && is_day)
        draw_sun(x - size * 0.22f, y - size * 0.20f, size * 0.72f,
                 RGBA8(255, 197, 77, 255));
    if (group == 5) cloud = RGBA8(200, 215, 229, 255);
    draw_cloud(x, y, size, cloud);
    if (group == 2) {
        for (int i = -1; i <= 1; ++i)
            line(x + i * size * 0.18f, y + size * 0.34f,
                 x + i * size * 0.18f - size * 0.07f,
                 y + size * 0.51f, RGBA8(55, 164, 239, 255));
    } else if (group == 3) {
        for (int i = -1; i <= 1; ++i) {
            float sx = x + i * size * 0.20f;
            float sy = y + size * 0.43f;
            line(sx - 4, sy, sx + 4, sy, RGBA8(233, 241, 250, 255));
            line(sx, sy - 4, sx, sy + 4, RGBA8(233, 241, 250, 255));
        }
    } else if (group == 4) {
        uint32_t bolt = RGBA8(255, 215, 65, 255);
        line(x + 2, y + size * 0.24f, x - size * 0.08f,
             y + size * 0.43f, bolt);
        line(x - size * 0.08f, y + size * 0.43f, x + size * 0.04f,
             y + size * 0.43f, bolt);
        line(x + size * 0.04f, y + size * 0.43f, x - size * 0.06f,
             y + size * 0.62f, bolt);
    } else if (group == 5) {
        for (int i = -1; i <= 1; ++i)
            line(x - size * 0.34f, y + size * (0.34f + i * 0.13f),
                 x + size * 0.36f, y + size * (0.34f + i * 0.13f),
                 RGBA8(187, 211, 226, 220));
    }
}

static void draw_spinner(uint64_t now_us)
{
    float phase = (float)(now_us % 1000000u) / 1000000.0f * 2.0f * PI_F;
    for (int i = 0; i < 8; ++i) {
        float angle = phase + i * PI_F / 4.0f;
        circle(925 + cosf(angle) * 9.0f, 31 + sinf(angle) * 9.0f,
               2.3f, RGBA8(255, 255, 255, 55 + i * 24));
    }
}

static float tab_x_for(int view)
{
    static const float positions[VIEW_COUNT] = {294, 378, 470, 576, 657};
    return positions[view];
}

static float tab_w_for(int view)
{
    static const float widths[VIEW_COUNT] = {76, 84, 98, 73, 106};
    return widths[view];
}

static void format_sync_time(char *out, size_t out_size,
                             const AppSnapshot *snapshot)
{
    const char *updated = snapshot->weather.updated;
    if (!updated || strlen(updated) < 16 || updated[10] != 'T') {
        snprintf(out, out_size, "%s", tr(snapshot->language, TEXT_LAST_SYNC));
        return;
    }
    char local_time[6];
    memcpy(local_time, updated + 11, 5);
    local_time[5] = '\0';
    char display_time[16];
    tr_format_time(display_time, sizeof(display_time), local_time,
                   snapshot->use_24_hour, snapshot->language);
    snprintf(out, out_size, "%s %s",
             tr(snapshot->language, TEXT_LAST_SYNC), display_time);
}

static void draw_header(UiState *ui, const AppSnapshot *snapshot, uint64_t now_us)
{
    uint32_t white = RGBA8(250, 252, 255, 255);
    uint32_t soft = RGBA8(236, 245, 255, 205);
    g_draw_alpha = 255;
    draw_fitted_text(ui, 28, 34, 244, white, 1.03f,
                     snapshot->weather.location);
    if (snapshot->weather.region[0])
        draw_fitted_text(ui, 29, 57, 244, soft, 0.61f,
                         snapshot->weather.region);

    rounded_rect(286, 15, 485, 46, 23, RGBA8(12, 36, 70, 72));
    float selected_x = tab_x_for(ui->view);
    float selected_w = tab_w_for(ui->view);
    uint64_t elapsed = now_us - ui->transition_start_us;
    if (ui->transition_start_us && elapsed < TRANSITION_US) {
        float t = ease_out_cubic((float)elapsed / (float)TRANSITION_US);
        selected_x = tab_x_for(ui->previous_view) +
                     (selected_x - tab_x_for(ui->previous_view)) * t;
        selected_w = tab_w_for(ui->previous_view) +
                     (selected_w - tab_w_for(ui->previous_view)) * t;
    }
    rounded_rect(selected_x, 20, selected_w, 36, 18,
                 RGBA8(250, 252, 255, 226));
    const char *tabs[VIEW_COUNT] = {
        tr(snapshot->language, TEXT_NOW), tr(snapshot->language, TEXT_HOURS),
        tr(snapshot->language, TEXT_DAILY), tr(snapshot->language, TEXT_MAP),
        tr(snapshot->language, TEXT_DETAILS)
    };
    for (int i = 0; i < VIEW_COUNT; ++i) {
        uint32_t color = i == ui->view ? RGBA8(28, 62, 99, 255) : soft;
        int width = text_width(ui, 0.67f, tabs[i]);
        draw_text(ui, tab_x_for(i) + (tab_w_for(i) - width) * 0.5f,
                  44, color, 0.67f, tabs[i]);
    }

    const char *source = snapshot->weather.source == WEATHER_SOURCE_LIVE
                             ? tr(snapshot->language, TEXT_LIVE)
                         : snapshot->weather.source == WEATHER_SOURCE_CACHE
                             ? tr(snapshot->language, TEXT_OFFLINE)
                             : tr(snapshot->language, TEXT_SAMPLE);
    uint32_t dot = snapshot->weather.source == WEATHER_SOURCE_LIVE ?
                   RGBA8(87, 230, 168, 255) : RGBA8(255, 210, 104, 255);
    rounded_rect(792, 10, 140, 50, 20, RGBA8(12, 36, 70, 96));
    if (!snapshot->busy)
        circle(809, snapshot->weather.source == WEATHER_SOURCE_CACHE ? 27 : 35,
               3, dot);
    if (snapshot->weather.source == WEATHER_SOURCE_CACHE) {
        char sync[32];
        format_sync_time(sync, sizeof(sync), snapshot);
        draw_fitted_text(ui, 820, 31, snapshot->busy ? 91 : 101,
                         white, 0.57f, source);
        draw_fitted_text(ui, 802, 49, 122, soft, 0.49f, sync);
    } else {
        draw_fitted_text(ui, 802 + (snapshot->busy ? 0 : 18), 41,
                         snapshot->busy ? 107 : 101, white, 0.60f, source);
    }
    if (snapshot->busy) draw_spinner(now_us);
}

static unsigned int staged_alpha(unsigned int page_alpha, uint64_t age_us,
                                 unsigned int delay_us)
{
    if (age_us >= delay_us + 220000u) return page_alpha;
    if (age_us <= delay_us) return 0;
    float t = ease_out_cubic((float)(age_us - delay_us) / 220000.0f);
    return (unsigned int)(page_alpha * t);
}

static float staged_rise(uint64_t age_us, unsigned int delay_us)
{
    if (age_us <= delay_us) return 12.0f;
    return 12.0f * (1.0f - ease_out_cubic((float)(age_us - delay_us) / 220000.0f));
}

static void draw_now(UiState *ui, const AppSnapshot *snapshot, float ox,
                     unsigned int page_alpha, uint64_t age_us)
{
    const WeatherData *weather = &snapshot->weather;
    Palette palette = palette_for(weather->is_day);
    int fahrenheit = snapshot->use_fahrenheit;
    uint32_t hero_text = RGBA8(250, 253, 255, 255);
    uint32_t hero_muted = RGBA8(241, 248, 255, 210);

    g_draw_alpha = staged_alpha(page_alpha, age_us, 0);
    float rise = staged_rise(age_us, 0);
    draw_text(ui, ox + 32, 117 + rise, hero_muted, 0.66f,
              tr(snapshot->language, TEXT_RIGHT_NOW));
    draw_textf(ui, ox + 30, 229 + rise, hero_text, 4.45f, "%.0f°",
               weather_display_temperature(weather->temperature_c, fahrenheit));
    if (!(snapshot->photo_background && weather->is_day &&
          weather_condition_group(weather->weather_code) == 0))
        draw_weather_icon(ox + 280, 166 + rise, 88,
                          weather->weather_code, weather->is_day);
    draw_fitted_text(ui, ox + 34, 271 + rise, 312, hero_text, 1.06f,
              tr_condition(snapshot->language, weather->weather_code));
    draw_textf(ui, ox + 34, 298 + rise, hero_muted, 0.60f,
               "%s %.0f°",
               tr(snapshot->language, TEXT_FEELS_LIKE),
               weather_display_temperature(weather->apparent_c, fahrenheit));
    char day_range[40];
    snprintf(day_range, sizeof(day_range), "H:%.0f°  L:%.0f°",
               weather_display_temperature(weather->day_count ? weather->days[0].high_c : weather->temperature_c, fahrenheit),
               weather_display_temperature(weather->day_count ? weather->days[0].low_c : weather->temperature_c, fahrenheit));
    draw_text(ui, ox + 340 - text_width(ui, 0.60f, day_range), 298 + rise,
              hero_muted, 0.60f, day_range);

    g_draw_alpha = staged_alpha(page_alpha, age_us, 45000);
    rise = staged_rise(age_us, 45000);
    float card_x = ox + 366;
    float card_y = 86 + rise;
    surface(card_x, card_y, 566, 226, 26, palette.surface);
    draw_text(ui, card_x + 25, card_y + 34, palette.text, 0.78f,
              tr(snapshot->language, TEXT_THIS_WEEK));
    draw_text(ui, card_x + 436, card_y + 33, palette.muted, 0.56f,
              tr(snapshot->language, TEXT_HIGH_LOW));
    int count = weather->day_count < 5 ? weather->day_count : 5;
    if (count > 0) {
        float min_t = weather->days[0].low_c;
        float max_t = weather->days[0].high_c;
        for (int i = 1; i < count; ++i) {
            if (weather->days[i].low_c < min_t) min_t = weather->days[i].low_c;
            if (weather->days[i].high_c > max_t) max_t = weather->days[i].high_c;
        }
        if (max_t - min_t < 4.0f) max_t = min_t + 4.0f;
        float high_x[5], high_y[5], low_y[5];
        for (int i = 0; i < count; ++i) {
            const WeatherDay *day = &weather->days[i];
            float x = card_x + 45 + i * (475.0f / 4.0f);
            high_x[i] = x;
            high_y[i] = card_y + 83 + (max_t - day->high_c) * 72.0f / (max_t - min_t);
            low_y[i] = card_y + 83 + (max_t - day->low_c) * 72.0f / (max_t - min_t);
        }
        for (int i = 1; i < count; ++i) {
            line(high_x[i - 1], high_y[i - 1], high_x[i], high_y[i],
                 RGBA8(255, 162, 73, 255));
            line(high_x[i - 1], high_y[i - 1] + 1, high_x[i], high_y[i] + 1,
                 RGBA8(255, 162, 73, 125));
            line(high_x[i - 1], low_y[i - 1], high_x[i], low_y[i],
                 RGBA8(56, 158, 231, 255));
        }
        for (int i = 0; i < count; ++i) {
            circle(high_x[i], high_y[i], 4, RGBA8(255, 162, 73, 255));
            circle(high_x[i], low_y[i], 4, RGBA8(106, 201, 255, 255));
            const WeatherDay *day = &weather->days[i];
            const char *day_label = i == 0
                                        ? tr(snapshot->language, TEXT_TODAY)
                                        : tr_day_label(snapshot->language, day->label);
            int label_width = text_width(ui, 0.55f, day_label);
            draw_text(ui, high_x[i] - label_width * 0.5f, card_y + 202,
                      palette.muted, 0.55f, day_label);
            char value[16];
            snprintf(value, sizeof(value), "%.0f°",
                       weather_display_temperature(day->high_c, fahrenheit));
            draw_centered_text(ui, high_x[i], high_y[i] - 10,
                               palette.text, 0.57f, value);
            snprintf(value, sizeof(value), "%.0f°",
                       weather_display_temperature(day->low_c, fahrenheit));
            draw_centered_text(ui, high_x[i], low_y[i] + 22,
                               palette.muted, 0.53f, value);
        }
    }

    g_draw_alpha = staged_alpha(page_alpha, age_us, 90000);
    rise = staged_rise(age_us, 90000);
    const char *labels[4] = {
        tr(snapshot->language, TEXT_HUMIDITY),
        tr(snapshot->language, TEXT_WIND),
        tr(snapshot->language, TEXT_RAIN_CHANCE),
        tr(snapshot->language, TEXT_SUNSET)
    };
    char values[4][32];
    snprintf(values[0], sizeof(values[0]), "%d%%", weather->humidity);
    snprintf(values[1], sizeof(values[1]), "%.0f %s",
             weather_display_wind(weather->wind_kmh, fahrenheit),
             weather_wind_unit(fahrenheit));
    snprintf(values[2], sizeof(values[2]), "%d%%",
             weather->day_count ? weather->days[0].precipitation_percent : 0);
    tr_format_time(values[3], sizeof(values[3]),
                   weather->day_count ? weather->days[0].sunset : "--:--",
                   snapshot->use_24_hour, snapshot->language);
    char subvalues[4][48];
    snprintf(subvalues[0], sizeof(subvalues[0]), "%s %.0f%%",
             tr(snapshot->language, TEXT_CLOUD_COVER), weather->cloud_cover);
    snprintf(subvalues[1], sizeof(subvalues[1]), "%.0f°",
             weather->wind_direction);
    snprintf(subvalues[2], sizeof(subvalues[2]), "%s %d%%",
             tr(snapshot->language, TEXT_TOMORROW),
             weather->day_count > 1
                 ? weather->days[1].precipitation_percent
                 : weather->day_count ? weather->days[0].precipitation_percent : 0);
    char sunrise[24];
    tr_format_time(sunrise, sizeof(sunrise),
                   weather->day_count ? weather->days[0].sunrise : "--:--",
                   snapshot->use_24_hour, snapshot->language);
    snprintf(subvalues[3], sizeof(subvalues[3]), "%s %s",
             tr(snapshot->language, TEXT_SUNRISE), sunrise);
    const int icons[4] = {ICON_HUMIDITY, ICON_WIND, ICON_RAIN, ICON_SUN};
    for (int i = 0; i < 4; ++i) {
        float x = ox + 28 + i * 229.0f;
        rounded_rect(x, 330 + rise, 217, 147, 23, palette.surface);
        draw_metric_icon(x + 28, 364 + rise, icons[i], palette.muted);
        draw_fitted_text(ui, x + 43, 370 + rise, 152,
                         palette.muted, 0.55f, labels[i]);
        draw_text(ui, x + 24, 426 + rise, palette.text, 1.06f, values[i]);
        draw_fitted_text(ui, x + 24, 457 + rise, 169, palette.muted, 0.50f,
                  subvalues[i]);
    }
}

static int clock_minutes(const char *clock)
{
    int hour = 0;
    int minute = 0;
    if (!clock || sscanf(clock, "%d:%d", &hour, &minute) != 2)
        return -1;
    return hour * 60 + minute;
}

static int forecast_hour_is_day(const WeatherData *weather, int hour_index)
{
    const WeatherHour *hour = &weather->hours[hour_index];
    const WeatherDay *day = NULL;
    for (int i = 0; i < weather->day_count; ++i) {
        if (!strncmp(hour->time, weather->days[i].date, 10)) {
            day = &weather->days[i];
            break;
        }
    }
    if (!day && weather->day_count)
        day = &weather->days[0];
    const char *clock = strchr(hour->time, 'T');
    int current = clock_minutes(clock ? clock + 1 : hour->label);
    int sunrise = day ? clock_minutes(day->sunrise) : -1;
    int sunset = day ? clock_minutes(day->sunset) : -1;
    if (current < 0 || sunrise < 0 || sunset < 0)
        return weather->is_day;
    return current >= sunrise && current < sunset;
}

static void draw_hourly(UiState *ui, const AppSnapshot *snapshot, float ox,
                        unsigned int page_alpha, uint64_t age_us)
{
    const WeatherData *weather = &snapshot->weather;
    Palette palette = palette_for(weather->is_day);
    int first_hour = weather->current_hour_index;
    if (first_hour < 0 || first_hour >= weather->hour_count) first_hour = 0;
    int available = weather->hour_count - first_hour;
    if (available > 24) available = 24;
    int maximum_start = available > 8 ? available - 8 : 0;
    if (ui->hourly_offset < 0) ui->hourly_offset = 0;
    if (ui->hourly_offset > maximum_start) ui->hourly_offset = maximum_start;
    int count = available < 8 ? available : 8;
    int fahrenheit = snapshot->use_fahrenheit;
    g_draw_alpha = staged_alpha(page_alpha, age_us, 0);
    float rise = staged_rise(age_us, 0);
    surface(ox + 28, 86 + rise, 904, 391, 27, palette.surface);
    draw_text(ui, ox + 54, 124 + rise, palette.text, 0.84f,
              tr(snapshot->language, TEXT_HOURLY_FORECAST));
    char range_label[64];
    snprintf(range_label, sizeof(range_label), "%d–%d / %d",
               count ? ui->hourly_offset + 1 : 0,
               ui->hourly_offset + count, available);
    int range_width = text_width(ui, 0.50f, range_label);
    draw_text(ui, ox + 905 - range_width, 123 + rise, palette.muted, 0.50f,
              range_label);
    if (!count) {
        draw_centered_text(ui, ox + 480, 283 + rise, palette.muted, 0.68f,
                           tr(snapshot->language, TEXT_HOURLY_UNAVAILABLE));
        return;
    }

    uint64_t now_us = sceKernelGetProcessTimeWide();
    if (!ui->hourly_visual_update_us)
        ui->hourly_visual_update_us = now_us;
    float dt = (float)(now_us - ui->hourly_visual_update_us) / 1000000.0f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    ui->hourly_visual_update_us = now_us;
    if (snapshot->motion == APP_MOTION_OFF)
        ui->hourly_visual_offset = (float)ui->hourly_offset;
    else {
        float blend = 1.0f - expf(-15.0f * dt);
        ui->hourly_visual_offset +=
            (ui->hourly_offset - ui->hourly_visual_offset) * blend;
        if (fabsf(ui->hourly_visual_offset - ui->hourly_offset) < 0.002f)
            ui->hourly_visual_offset = (float)ui->hourly_offset;
    }

    float step = count > 1 ? 796.0f / (float)(count - 1) : 0.0f;
    int render_start = (int)floorf(ui->hourly_visual_offset);
    int render_end = (int)ceilf(ui->hourly_visual_offset) + count;
    if (render_start < 0) render_start = 0;
    if (render_end >= available) render_end = available - 1;
    vita2d_set_clip_rectangle((int)(ox + 39), (int)(139 + rise),
                              (int)(ox + 921), (int)(455 + rise));
    vita2d_enable_clipping();
    for (int relative = render_start; relative <= render_end; ++relative) {
        int hour_index = first_hour + relative;
        float x = ox + 82 + (relative - ui->hourly_visual_offset) * step;
        if (relative == 0)
            rounded_rect(x - 43, 139 + rise, 86, 313, 19,
                         RGBA8(150, 216, 255, 25));
        char hour_label[16];
        if (!strncmp(weather->hours[hour_index].time, weather->updated, 13))
            snprintf(hour_label, sizeof(hour_label), "%s",
                     tr(snapshot->language, TEXT_NOW));
        else
            tr_format_time(hour_label, sizeof(hour_label),
                           weather->hours[hour_index].label,
                           snapshot->use_24_hour, snapshot->language);
        int hour_width = text_width(ui, 0.53f, hour_label);
        draw_text(ui, x - hour_width * 0.5f, 164 + rise,
                  palette.muted, 0.53f, hour_label);
        draw_weather_icon(x, 211 + rise, 38,
                          weather->hours[hour_index].weather_code,
                          forecast_hour_is_day(weather, hour_index));
        char temperature[12];
        snprintf(temperature, sizeof(temperature), "%.0f°",
                 weather_display_temperature(
                                             weather->hours[hour_index].temperature_c,
                                             fahrenheit));
        int temperature_width = text_width(ui, 1.06f, temperature);
        draw_text(ui, x - temperature_width * 0.5f, 286 + rise,
                  palette.text, 1.06f, temperature);

        char rain_label[12];
        snprintf(rain_label, sizeof(rain_label), "%d%%",
                 weather->hours[hour_index].precipitation_percent);
        int rain_width = text_width(ui, 0.55f, rain_label);
        draw_text(ui, x - rain_width * 0.5f, 329 + rise,
                  RGBA8(107, 201, 255, 255), 0.55f, rain_label);
        float rain_fraction =
            weather->hours[hour_index].precipitation_percent / 100.0f;
        rounded_rect(x - 35, 354 + rise, 70, 7, 3.5f,
                     RGBA8(203, 226, 241, 45));
        float rain_fill_width = rain_fraction * 70.0f;
        if (rain_fill_width > 0.0f && rain_fill_width < 4.0f)
            rain_fill_width = 4.0f;
        if (rain_fill_width > 0.0f)
            rounded_rect(x - 35, 354 + rise, rain_fill_width, 7, 3.5f,
                         RGBA8(96, 194, 249, 225));

        char wind[20];
        snprintf(wind, sizeof(wind), "%.0f %s",
                 weather_display_wind(weather->hours[hour_index].wind_kmh,
                                      fahrenheit),
                 weather_wind_unit(fahrenheit));
        int wind_width = text_width(ui, 0.47f, wind);
        draw_text(ui, x - wind_width * 0.5f, 439 + rise,
                  palette.muted, 0.47f, wind);
    }
    vita2d_disable_clipping();
}

static void draw_daily(UiState *ui, const AppSnapshot *snapshot, float ox,
                       unsigned int page_alpha, uint64_t age_us)
{
    const WeatherData *weather = &snapshot->weather;
    Palette palette = palette_for(weather->is_day);
    int fahrenheit = snapshot->use_fahrenheit;
    g_draw_alpha = staged_alpha(page_alpha, age_us, 0);
    float rise = staged_rise(age_us, 0);
    draw_text(ui, ox + 50, 119 + rise, palette.text, 0.84f,
              tr(snapshot->language, TEXT_SEVEN_DAY));
    int start = ui->daily_offset;
    int maximum_start = weather->day_count > 7 ? weather->day_count - 7 : 0;
    if (start > maximum_start) start = maximum_start;
    if (start < 0) start = 0;
    ui->daily_offset = start;
    int count = weather->day_count - start;
    if (count > 7) count = 7;
    float minimum = count ? weather->days[start].low_c : 0.0f;
    float maximum = count ? weather->days[start].high_c : 1.0f;
    for (int i = 1; i < count; ++i) {
        const WeatherDay *range_day = &weather->days[start + i];
        if (range_day->low_c < minimum) minimum = range_day->low_c;
        if (range_day->high_c > maximum) maximum = range_day->high_c;
    }
    if (maximum - minimum < 1.0f) maximum = minimum + 1.0f;
    char page_label[32];
    snprintf(page_label, sizeof(page_label), "%d–%d / %d",
             count ? start + 1 : 0, start + count, weather->day_count);
    int page_width = text_width(ui, 0.49f, page_label);
    draw_text(ui, ox + 904 - page_width, 116 + rise, palette.muted, 0.49f,
              page_label);
    surface(ox + 42, 132 + rise, 876, 333, 25, palette.surface);
    for (int i = 0; i < count; ++i) {
        int day_index = start + i;
        const WeatherDay *day = &weather->days[day_index];
        float row_y = 141 + i * 45.0f + rise;
        int selected = day_index == ui->daily_selected;
        if (selected)
            draw_focus(ox + 50, row_y, 860, 40);
        else
            rect(ox + 62, row_y + 42, 836, 1, palette.divider);
        draw_text(ui, ox + 68, row_y + 27,
                  day_index == 0 ? RGBA8(116, 211, 255, 255)
                                 : palette.text,
                  0.68f, day_index == 0
                             ? tr(snapshot->language, TEXT_TODAY)
                             : tr_day_label(snapshot->language, day->label));
        draw_weather_icon(ox + 178, row_y + 18, 27, day->weather_code, 1);
        char rain[12];
        snprintf(rain, sizeof(rain), "%d%%", day->precipitation_percent);
        draw_text(ui, ox + 205, row_y + 27, RGBA8(103, 201, 255, 255),
                  0.55f, rain);
        draw_fitted_text(ui, ox + 267, row_y + 27, 336, palette.text, 0.58f,
                  tr_condition(snapshot->language, day->weather_code));
        char low[12];
        char high[12];
        snprintf(low, sizeof(low), "%.0f°",
                 weather_display_temperature(day->low_c, fahrenheit));
        snprintf(high, sizeof(high), "%.0f°",
                 weather_display_temperature(day->high_c, fahrenheit));
        draw_text(ui, ox + 650 - text_width(ui, 0.66f, low),
                  row_y + 27, palette.muted, 0.66f, low);
        float range_start = (day->low_c - minimum) / (maximum - minimum);
        float range_end = (day->high_c - minimum) / (maximum - minimum);
        rounded_rect(ox + 675, row_y + 17, 142, 7, 3.5f,
                     RGBA8(212, 230, 244, 38));
        float bar_x = ox + 675 + range_start * 142.0f;
        float bar_width = (range_end - range_start) * 142.0f;
        if (bar_width < 8.0f) bar_width = 8.0f;
        if (bar_x + bar_width > ox + 817) bar_width = ox + 817 - bar_x;
        horizontal_gradient(bar_x, row_y + 17, bar_width, 7,
                             RGBA8(90, 205, 222, 255),
                             RGBA8(255, 171, 75, 255));
        circle(bar_x, row_y + 20.5f, 3.5f,
               RGBA8(90, 205, 222, 255));
        circle(bar_x + bar_width, row_y + 20.5f, 3.5f,
               RGBA8(255, 171, 75, 255));
        if (day_index == 0) {
            float now_c = weather->temperature_c;
            if (now_c < day->low_c) now_c = day->low_c;
            if (now_c > day->high_c) now_c = day->high_c;
            float now_position = (now_c - minimum) / (maximum - minimum);
            float now_x = ox + 675 + now_position * 142.0f;
            circle(now_x, row_y + 20.5f, 5.0f, RGBA8(17, 52, 84, 180));
            circle(now_x, row_y + 20.5f, 3.3f, RGBA8(255, 255, 255, 255));
        }
        draw_text(ui, ox + 842, row_y + 27, palette.text, 0.66f, high);
    }
}

static void free_map_texture_set(vita2d_texture **textures,
                                 unsigned char *loaded)
{
    for (int i = 0; i < MAP_TILE_COUNT; ++i) {
        if (textures[i]) {
            vita2d_free_texture(textures[i]);
            textures[i] = NULL;
        }
    }
    memset(loaded, 0, MAP_TILE_COUNT * sizeof(*loaded));
}

static void free_map_tiles(UiState *ui)
{
    free_map_texture_set(ui->map_tiles, ui->map_tile_loaded);
    free_map_texture_set(ui->map_pending_tiles,
                         ui->map_pending_tile_loaded);
    free_map_texture_set(ui->map_retired_tiles,
                         ui->map_retired_tile_loaded);
    ui->map_retired_zoom = -1;
    ui->map_retired_recyclable = 0;
    memset(ui->map_pending_reuse, -1, sizeof(ui->map_pending_reuse));
    memset(ui->map_active_attempted, 0, sizeof(ui->map_active_attempted));
}

static int map_texture_set_any(const unsigned char *loaded)
{
    for (int i = 0; i < MAP_TILE_COUNT; ++i)
        if (loaded[i]) return 1;
    return 0;
}

static int map_texture_set_complete(const unsigned char *loaded)
{
    for (int i = 0; i < MAP_TILE_COUNT; ++i)
        if (!loaded[i]) return 0;
    return 1;
}

static int map_pending_complete(const UiState *ui)
{
    for (int i = 0; i < MAP_TILE_COUNT; ++i)
        if (!ui->map_pending_tile_loaded[i] &&
            ui->map_pending_reuse[i] < 0)
            return 0;
    return 1;
}

static int map_texture_free_slot(vita2d_texture **textures)
{
    for (int i = 0; i < MAP_TILE_COUNT; ++i)
        if (!textures[i]) return i;
    return -1;
}

static int map_pending_has_anchor(const UiState *ui)
{
    int anchor = (MAP_TILE_ROWS / 2) * MAP_TILE_COLUMNS +
                 MAP_TILE_COLUMNS / 2;
    return ui->map_pending_tile_loaded[anchor] ||
           ui->map_pending_reuse[anchor] >= 0;
}

static vita2d_texture *load_map_texture(UiState *ui,
                                        AppMapProvider provider, int zoom,
                                        int tile_x, int tile_y,
                                        int allow_placeholder,
                                        vita2d_texture *reuse)
{
    int staged = app_map_tile_staged(provider, zoom, tile_x, tile_y);
    if (!staged && !allow_placeholder) return NULL;
    uint64_t started_us = sceKernelGetProcessTimeWide();
    vita2d_texture *texture = reuse ? reuse :
        vita2d_create_empty_texture_format(
            MAP_TILE_WIDTH, MAP_TILE_HEIGHT,
            SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
    if (!texture) return NULL;
    int copied = staged && app_map_copy_staged_tile(
        provider, zoom, tile_x, tile_y, vita2d_texture_get_datap(texture),
        vita2d_texture_get_stride(texture)) == 0;
    if (!copied) {
        if (allow_placeholder) {
            unsigned int *pixels = vita2d_texture_get_datap(texture);
            int stride = (int)(vita2d_texture_get_stride(texture) / 4);
            unsigned int grey = RGBA8(42, 66, 87, 255);
            for (int y = 0; y < MAP_TILE_HEIGHT; ++y)
                for (int x = 0; x < MAP_TILE_WIDTH; ++x)
                    pixels[y * stride + x] = grey;
        } else {
            if (!reuse) vita2d_free_texture(texture);
            return NULL;
        }
    }
    /* Movement is eased independently below. Keep streamed/recycled tiles on
     * the stable native sampler: forcing linear filtering made repeated map
     * window replacement terminate Vita3K's texture path during stress. */
    vita2d_texture_set_filters(texture, SCE_GXM_TEXTURE_FILTER_POINT,
                              SCE_GXM_TEXTURE_FILTER_POINT);
    uint64_t elapsed = sceKernelGetProcessTimeWide() - started_us;
    ++ui->map_upload_count;
    ui->map_upload_total_us += elapsed;
    if (elapsed > ui->map_upload_max_us) ui->map_upload_max_us = elapsed;
    if (!copied) ++ui->map_upload_placeholders;
    return texture;
}

static void log_map_upload_summary(UiState *ui, int zoom,
                                   int base_x, int base_y)
{
    if (!ui->map_upload_count) return;
    app_log("map GPU upload summary: zoom=%d base=%d,%d count=%u total=%.1fms average=%.2fms max=%.1fms placeholders=%u",
            zoom, base_x, base_y, ui->map_upload_count,
            ui->map_upload_total_us / 1000.0,
            ui->map_upload_total_us / 1000.0 / ui->map_upload_count,
            ui->map_upload_max_us / 1000.0,
            ui->map_upload_placeholders);
    ui->map_upload_count = 0;
    ui->map_upload_total_us = 0;
    ui->map_upload_max_us = 0;
    ui->map_upload_placeholders = 0;
}

static void reset_map_pending(UiState *ui)
{
    for (int i = 0; i < MAP_TILE_COUNT; ++i) {
        if (!ui->map_pending_tiles[i]) continue;
        int slot = map_texture_free_slot(ui->map_retired_tiles);
        if (slot >= 0) {
            ui->map_retired_tiles[slot] = ui->map_pending_tiles[i];
            ui->map_retired_tile_loaded[slot] = 1;
            ui->map_retired_zoom = -1;
        } else {
            /* This texture was never submitted; normal navigation keeps
             * pending allocations in the bounded recycle set above. */
            vita2d_free_texture(ui->map_pending_tiles[i]);
        }
        ui->map_pending_tiles[i] = NULL;
        ui->map_pending_tile_loaded[i] = 0;
    }
    memset(ui->map_pending_reuse, -1, sizeof(ui->map_pending_reuse));
    memset(ui->map_pending_attempted, 0, sizeof(ui->map_pending_attempted));
}

static void sync_map_tiles(UiState *ui, const MapSnapshot *map,
                           uint64_t now_us)
{
    if (!map->zoom) return;
    int revision_changed = ui->map_revision != map->revision;
    ui->map_revision = map->revision;

    int active_target = ui->map_loaded_provider == map->provider &&
                        ui->map_loaded_zoom == map->zoom &&
                        ui->map_loaded_base_x == map->base_x &&
                        ui->map_loaded_base_y == map->base_y;
    int retired_target = ui->map_retired_provider == map->provider &&
                         ui->map_retired_zoom == map->zoom &&
                         ui->map_retired_base_x == map->base_x &&
                         ui->map_retired_base_y == map->base_y &&
                         map_texture_set_complete(ui->map_retired_tile_loaded);
    if (!active_target && retired_target) {
        /* A quick zoom reversal frequently requests the exact level we just
         * retired. Swap those complete sets instead of allocating duplicate
         * tiles and freeing equal-content textures while Vita3K's texture
         * cache still references them. This also makes zoom-back immediate on
         * hardware and avoids another disk pass. */
        reset_map_pending(ui);
        for (int i = 0; i < MAP_TILE_COUNT; ++i) {
            vita2d_texture *texture = ui->map_tiles[i];
            unsigned char loaded = ui->map_tile_loaded[i];
            ui->map_tiles[i] = ui->map_retired_tiles[i];
            ui->map_tile_loaded[i] = ui->map_retired_tile_loaded[i];
            ui->map_retired_tiles[i] = texture;
            ui->map_retired_tile_loaded[i] = loaded;
        }
        int old_provider = ui->map_loaded_provider;
        int old_zoom = ui->map_loaded_zoom;
        int old_base_x = ui->map_loaded_base_x;
        int old_base_y = ui->map_loaded_base_y;
        ui->map_loaded_provider = ui->map_retired_provider;
        ui->map_loaded_zoom = ui->map_retired_zoom;
        ui->map_loaded_base_x = ui->map_retired_base_x;
        ui->map_loaded_base_y = ui->map_retired_base_y;
        ui->map_retired_provider = old_provider;
        ui->map_retired_zoom = old_zoom;
        ui->map_retired_base_x = old_base_x;
        ui->map_retired_base_y = old_base_y;
        ui->map_retired_recyclable = 0;
        ui->map_pending_zoom = -1;
        memset(ui->map_active_attempted, 0,
               sizeof(ui->map_active_attempted));
        ui->map_transition_start_us = now_us;
        ui->map_navigation_locked = 0;
        ui->map_preview_dx = 0;
        ui->map_preview_dy = 0;
        ui->map_preview_zoom = 0;
        active_target = 1;
        app_log("map retired window restored: zoom=%d base=%d,%d",
                ui->map_loaded_zoom, ui->map_loaded_base_x,
                ui->map_loaded_base_y);
    }
    if (active_target) {
        if (ui->map_pending_zoom >= 0) {
            reset_map_pending(ui);
            ui->map_pending_zoom = -1;
        }
        if (revision_changed)
            memset(ui->map_active_attempted, 0,
                   sizeof(ui->map_active_attempted));
        static const unsigned char active_order[MAP_TILE_COUNT] = {
            7, 6, 8, 2, 12, 1, 3, 11, 13, 5, 9, 0, 4, 10, 14
        };
        for (int order = 0; order < MAP_TILE_COUNT; ++order) {
            int i = active_order[order];
            if (ui->map_tile_loaded[i] || ui->map_active_attempted[i])
                continue;
            ui->map_active_attempted[i] = 1;
            int column = i % MAP_TILE_COLUMNS;
            int row = i / MAP_TILE_COLUMNS;
            vita2d_texture *texture = load_map_texture(
                ui,
                (AppMapProvider)ui->map_loaded_provider,
                ui->map_loaded_zoom, ui->map_loaded_base_x + column,
                ui->map_loaded_base_y + row, !map->loading, NULL);
            if (texture) {
                ui->map_tiles[i] = texture;
                ui->map_tile_loaded[i] = 1;
            }
            break;
        }
        if (!map->loading && map_texture_set_complete(ui->map_tile_loaded)) {
            ui->map_navigation_locked = 0;
            log_map_upload_summary(ui, ui->map_loaded_zoom,
                                   ui->map_loaded_base_x,
                                   ui->map_loaded_base_y);
        }
        return;
    }

    int pending_target = ui->map_pending_provider == map->provider &&
                         ui->map_pending_zoom == map->zoom &&
                         ui->map_pending_base_x == map->base_x &&
                         ui->map_pending_base_y == map->base_y;
    if (!pending_target) {
        reset_map_pending(ui);
        ui->map_upload_count = 0;
        ui->map_upload_total_us = 0;
        ui->map_upload_max_us = 0;
        ui->map_upload_placeholders = 0;
        if (map_texture_set_any(ui->map_retired_tile_loaded)) {
            /* Vita3K and the physical-Vita log both exposed corruption after
             * old tile allocations were rewritten for another zoom level.
             * Fence the last submitted window, then release one retired tile
             * per upload frame. VitaMaps' bounded LRU follows the same gradual
             * eviction pattern; freeing a whole 15-tile window in one frame
             * crashes Vita3K's texture cache. New textures remain immutable. */
            ui->map_retired_recyclable = 1;
            ui->map_retired_zoom = -1;
        }
        ui->map_pending_provider = map->provider;
        ui->map_pending_zoom = map->zoom;
        ui->map_pending_base_x = map->base_x;
        ui->map_pending_base_y = map->base_y;
        if (ui->map_loaded_provider == map->provider &&
            ui->map_loaded_zoom == map->zoom) {
            for (int target = 0; target < MAP_TILE_COUNT; ++target) {
                int target_column = target % MAP_TILE_COLUMNS;
                int target_row = target / MAP_TILE_COLUMNS;
                int target_x = map_wrap_x(map->base_x + target_column,
                                          map->zoom);
                int target_y = map_clamp_y(map->base_y + target_row,
                                           map->zoom);
                for (int active = 0; active < MAP_TILE_COUNT; ++active) {
                    int active_column = active % MAP_TILE_COLUMNS;
                    int active_row = active / MAP_TILE_COLUMNS;
                    int active_x = map_wrap_x(ui->map_loaded_base_x +
                                              active_column, map->zoom);
                    int active_y = map_clamp_y(ui->map_loaded_base_y +
                                              active_row, map->zoom);
                    if (ui->map_tile_loaded[active] && active_x == target_x &&
                        active_y == target_y) {
                        ui->map_pending_reuse[target] = (signed char)active;
                        break;
                    }
                }
            }
        }
    } else if (revision_changed) {
        for (int i = 0; i < MAP_TILE_COUNT; ++i)
            if (!ui->map_pending_tile_loaded[i] &&
                ui->map_pending_reuse[i] < 0)
                ui->map_pending_attempted[i] = 0;
    }

    static const unsigned char pending_order[MAP_TILE_COUNT] = {
        7, 6, 8, 2, 12, 1, 3, 11, 13, 5, 9, 0, 4, 10, 14
    };
    for (int order = 0; order < MAP_TILE_COUNT; ++order) {
        int i = pending_order[order];
        if (ui->map_pending_tile_loaded[i] ||
            ui->map_pending_reuse[i] >= 0 ||
            ui->map_pending_attempted[i])
            continue;
        ui->map_pending_attempted[i] = 1;
        int column = i % MAP_TILE_COLUMNS;
        int row = i / MAP_TILE_COLUMNS;
        int recycle_slot = -1;
        vita2d_texture *recycle = NULL;
        if (ui->map_retired_recyclable) {
            for (int candidate = 0; candidate < MAP_TILE_COUNT;
                 ++candidate) {
                if (ui->map_retired_tiles[candidate]) {
                    recycle_slot = candidate;
                    recycle = ui->map_retired_tiles[candidate];
                    break;
                }
            }
        }
        vita2d_texture *texture = load_map_texture(
            ui,
            (AppMapProvider)ui->map_pending_provider,
            ui->map_pending_zoom, ui->map_pending_base_x + column,
            ui->map_pending_base_y + row, !map->loading, recycle);
        if (texture) {
            /* ui_draw has fenced the previous scene. Refill an unused retired
             * tile in place instead of deleting and recreating a GXM/GL
             * texture every upload frame. This keeps the two-window memory
             * bound, removes allocator churn, and avoids Vita3K's intermittent
             * color-surface trap during rapid multi-level zoom. The recycled
             * texture is never part of the active set. */
            if (recycle_slot >= 0) {
                ui->map_retired_tiles[recycle_slot] = NULL;
                ui->map_retired_tile_loaded[recycle_slot] = 0;
            }
            ui->map_pending_tiles[i] = texture;
            ui->map_pending_tile_loaded[i] = 1;
        }
        if (!map_texture_set_any(ui->map_retired_tile_loaded))
            ui->map_retired_recyclable = 0;
        break;
    }

    int had_active = map_texture_set_any(ui->map_tile_loaded);
    int activate_pending = (!had_active && map_pending_has_anchor(ui)) ||
                           (had_active && map_pending_complete(ui));
    if (activate_pending) {
        int reused_count = 0;
        for (int i = 0; i < MAP_TILE_COUNT; ++i)
            reused_count += ui->map_pending_reuse[i] >= 0;
        if (had_active) {
            int outgoing = 0;
            for (int i = 0; i < MAP_TILE_COUNT; ++i) {
                int reused = 0;
                for (int target = 0; target < MAP_TILE_COUNT; ++target)
                    reused |= ui->map_pending_reuse[target] == i;
                outgoing += !reused && ui->map_tiles[i] != NULL;
            }
            int free_slots = 0;
            for (int i = 0; i < MAP_TILE_COUNT; ++i)
                free_slots += ui->map_retired_tiles[i] == NULL;
            if (outgoing > free_slots) {
                app_log("map activation deferred: outgoing=%d recycle-free=%d",
                        outgoing, free_slots);
                return;
            }
            int retired_was_empty = free_slots == MAP_TILE_COUNT;
            for (int i = 0; i < MAP_TILE_COUNT; ++i) {
                int reused = 0;
                for (int target = 0; target < MAP_TILE_COUNT; ++target)
                    reused |= ui->map_pending_reuse[target] == i;
                if (!reused && ui->map_tiles[i]) {
                    int retired = map_texture_free_slot(
                        ui->map_retired_tiles);
                    ui->map_retired_tiles[retired] = ui->map_tiles[i];
                    ui->map_retired_tile_loaded[retired] = 1;
                }
            }
            ui->map_retired_recyclable = 0;
            if (reused_count == 0 && retired_was_empty &&
                outgoing == MAP_TILE_COUNT) {
                ui->map_retired_provider = ui->map_loaded_provider;
                ui->map_retired_zoom = ui->map_loaded_zoom;
                ui->map_retired_base_x = ui->map_loaded_base_x;
                ui->map_retired_base_y = ui->map_loaded_base_y;
            } else {
                ui->map_retired_zoom = -1;
            }
        } else {
            free_map_texture_set(ui->map_tiles, ui->map_tile_loaded);
        }
        vita2d_texture *next_tiles[MAP_TILE_COUNT] = {NULL};
        unsigned char next_loaded[MAP_TILE_COUNT] = {0};
        for (int i = 0; i < MAP_TILE_COUNT; ++i) {
            int reuse = ui->map_pending_reuse[i];
            if (reuse >= 0) {
                next_tiles[i] = ui->map_tiles[reuse];
                next_loaded[i] = ui->map_tile_loaded[reuse];
                ui->map_tiles[reuse] = NULL;
                ui->map_tile_loaded[reuse] = 0;
            } else {
                next_tiles[i] = ui->map_pending_tiles[i];
                next_loaded[i] = ui->map_pending_tile_loaded[i];
                ui->map_pending_tiles[i] = NULL;
                ui->map_pending_tile_loaded[i] = 0;
            }
        }
        for (int i = 0; i < MAP_TILE_COUNT; ++i) {
            ui->map_tiles[i] = next_tiles[i];
            ui->map_tile_loaded[i] = next_loaded[i];
        }
        memset(ui->map_pending_reuse, -1, sizeof(ui->map_pending_reuse));
        memset(ui->map_pending_attempted, 0,
               sizeof(ui->map_pending_attempted));
        memset(ui->map_active_attempted, 0,
               sizeof(ui->map_active_attempted));
        ui->map_loaded_provider = ui->map_pending_provider;
        ui->map_loaded_zoom = ui->map_pending_zoom;
        ui->map_loaded_base_x = ui->map_pending_base_x;
        ui->map_loaded_base_y = ui->map_pending_base_y;
        ui->map_pending_zoom = -1;
        if (!had_active)
            ui->map_present_initialized = 0;
        ui->map_transition_start_us = had_active ? now_us : 0;
        /* The first window may activate as soon as its center tile is ready
         * so the user sees useful pixels quickly.  Do not accept another
         * pan/zoom until the worker has finished and all 15 GPU textures are
         * present: the map worker treats the advertised active window as
         * reusable, and moving earlier made it skip tiles that the UI had not
         * uploaded yet.  Those missing overlaps became gray placeholders on
         * the next window.  The active-target path releases this lock once
         * the complete texture set is proven. */
        if (!map->loading && map_texture_set_complete(ui->map_tile_loaded))
            ui->map_navigation_locked = 0;
        ui->map_preview_dx = 0;
        ui->map_preview_dy = 0;
        ui->map_preview_zoom = 0;
        app_log("map window active: zoom=%d base=%d,%d reused=%d",
                ui->map_loaded_zoom, ui->map_loaded_base_x,
                ui->map_loaded_base_y, reused_count);
        if (had_active)
            log_map_upload_summary(ui, ui->map_loaded_zoom,
                                   ui->map_loaded_base_x,
                                   ui->map_loaded_base_y);
    }
}

static const char *map_layer_label(const AppSnapshot *snapshot,
                                   AppMapLayer layer)
{
    switch (layer) {
    case APP_MAP_PRECIPITATION:
        return tr(snapshot->language, TEXT_PRECIPITATION);
    case APP_MAP_WIND:
        return tr(snapshot->language, TEXT_WIND);
    case APP_MAP_AIR_QUALITY:
        return tr(snapshot->language, TEXT_AIR_QUALITY);
    default:
        return tr(snapshot->language, TEXT_TEMPERATURE);
    }
}

static const char *map_layer_name(const AppSnapshot *snapshot)
{
    return map_layer_label(snapshot, snapshot->map_layer);
}

static float map_hour_value(const AppSnapshot *snapshot, int index)
{
    if (snapshot->weather.hour_count < 1) return 0.0f;
    int first = snapshot->weather.current_hour_index;
    if (first < 0 || first >= snapshot->weather.hour_count) first = 0;
    if (index < 0) index = 0;
    index += first;
    if (index >= snapshot->weather.hour_count)
        index = snapshot->weather.hour_count - 1;
    const WeatherHour *hour = &snapshot->weather.hours[index];
    switch (snapshot->map_layer) {
    case APP_MAP_PRECIPITATION: return (float)hour->precipitation_percent;
    case APP_MAP_WIND:
        return weather_display_wind(hour->wind_kmh,
                                    snapshot->use_fahrenheit);
    case APP_MAP_AIR_QUALITY:
        return hour->air_quality > 0.0f ? hour->air_quality
                                       : snapshot->weather.air_quality;
    default:
        return weather_display_temperature(hour->temperature_c,
                                           snapshot->use_fahrenheit);
    }
}

static int map_hour_count(const WeatherData *weather)
{
    if (!weather || weather->hour_count < 1) return 0;
    int first = weather->current_hour_index;
    if (first < 0 || first >= weather->hour_count) first = 0;
    int remaining = weather->hour_count - first;
    return remaining < 24 ? remaining : 24;
}

static float map_interpolated_value(const UiState *ui,
                                    const AppSnapshot *snapshot)
{
    int count = map_hour_count(&snapshot->weather);
    if (!count) return 0.0f;
    float hour = ui->map_visual_hour;
    if (hour < 0.0f) hour = 0.0f;
    if (hour > count - 1) hour = (float)(count - 1);
    int first = (int)floorf(hour);
    int second = first + 1 < count ? first + 1 : first;
    float amount = hour - first;
    return map_hour_value(snapshot, first) * (1.0f - amount) +
           map_hour_value(snapshot, second) * amount;
}

static float map_interpolated_direction(const UiState *ui,
                                        const WeatherData *weather)
{
    int count = map_hour_count(weather);
    if (!count) return weather->wind_direction;
    float hour = ui->map_visual_hour;
    if (hour < 0.0f) hour = 0.0f;
    if (hour > count - 1) hour = (float)(count - 1);
    int first = (int)floorf(hour);
    int second = first + 1 < count ? first + 1 : first;
    float amount = hour - first;
    int base = weather->current_hour_index;
    if (base < 0 || base >= weather->hour_count) base = 0;
    float start = weather->hours[base + first].wind_direction;
    float delta = weather->hours[base + second].wind_direction - start;
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    return start + delta * amount;
}

static float map_field_point_value(const WeatherMapField *field, int point,
                                   int layer, int hour)
{
    const WeatherMapPoint *sample = &field->points[point];
    if (hour < 0) hour = 0;
    if (hour >= field->hour_count) hour = field->hour_count - 1;
    switch (layer) {
    case APP_MAP_PRECIPITATION: return sample->precipitation_mm[hour];
    case APP_MAP_WIND: return sample->wind_kmh[hour];
    case APP_MAP_AIR_QUALITY: return sample->air_quality[hour];
    default: return sample->temperature_c[hour];
    }
}

static int map_field_point_valid(const WeatherMapField *field, int point,
                                 int layer)
{
    return layer == APP_MAP_AIR_QUALITY
               ? field->points[point].air_quality_valid
               : field->points[point].forecast_valid;
}

static int map_field_sample(const WeatherMapField *field, int layer, int hour,
                            float unit_x, float unit_y, float *value)
{
    if (!field || field->hour_count < 1 || !value) return 0;
    float grid_x = clamp01(unit_x) * WEATHER_MAP_COLUMNS - 0.5f;
    float grid_y = clamp01(unit_y) * WEATHER_MAP_ROWS - 0.5f;
    if (grid_x < 0.0f) grid_x = 0.0f;
    if (grid_y < 0.0f) grid_y = 0.0f;
    if (grid_x > WEATHER_MAP_COLUMNS - 1)
        grid_x = WEATHER_MAP_COLUMNS - 1;
    if (grid_y > WEATHER_MAP_ROWS - 1)
        grid_y = WEATHER_MAP_ROWS - 1;
    int x0 = (int)floorf(grid_x);
    int y0 = (int)floorf(grid_y);
    int x1 = x0 + 1 < WEATHER_MAP_COLUMNS ? x0 + 1 : x0;
    int y1 = y0 + 1 < WEATHER_MAP_ROWS ? y0 + 1 : y0;
    int indices[4] = {
        y0 * WEATHER_MAP_COLUMNS + x0,
        y0 * WEATHER_MAP_COLUMNS + x1,
        y1 * WEATHER_MAP_COLUMNS + x0,
        y1 * WEATHER_MAP_COLUMNS + x1
    };
    for (int i = 0; i < 4; ++i)
        if (indices[i] >= field->point_count ||
            !map_field_point_valid(field, indices[i], layer))
            return 0;
    float fx = grid_x - x0;
    float fy = grid_y - y0;
    float top = map_field_point_value(field, indices[0], layer, hour) *
                    (1.0f - fx) +
                map_field_point_value(field, indices[1], layer, hour) * fx;
    float bottom = map_field_point_value(field, indices[2], layer, hour) *
                       (1.0f - fx) +
                   map_field_point_value(field, indices[3], layer, hour) * fx;
    *value = top * (1.0f - fy) + bottom * fy;
    return 1;
}

static uint32_t map_field_color(int layer, float value)
{
    static const unsigned char temperature[6][3] = {
        {44, 80, 204}, {40, 174, 224}, {53, 186, 128},
        {230, 208, 66}, {246, 132, 49}, {213, 62, 76}
    };
    static const unsigned char aqi[6][3] = {
        {55, 195, 122}, {210, 205, 57}, {240, 154, 50},
        {229, 80, 68}, {145, 72, 164}, {115, 51, 89}
    };
    const unsigned char (*stops)[3] = temperature;
    float at;
    unsigned int alpha = 112;
    if (layer == APP_MAP_PRECIPITATION) {
        if (value < 0.03f) return RGBA8(0, 0, 0, 0);
        float strength = clamp01(log2f(1.0f + value * 2.0f) / 4.0f);
        return RGBA8(lerp_byte(94, 103, strength),
                     lerp_byte(209, 70, strength),
                     lerp_byte(255, 210, strength),
                     (unsigned int)(72.0f + strength * 104.0f));
    }
    if (layer == APP_MAP_WIND) {
        float strength = clamp01(value / 70.0f);
        return RGBA8(lerp_byte(37, 87, strength),
                     lerp_byte(139, 222, strength),
                     lerp_byte(193, 162, strength),
                     (unsigned int)(66.0f + strength * 55.0f));
    }
    if (layer == APP_MAP_AIR_QUALITY) {
        stops = aqi;
        at = clamp01(value / 300.0f) * 5.0f;
        alpha = 126;
    } else {
        at = clamp01((value + 20.0f) / 65.0f) * 5.0f;
    }
    int segment = (int)floorf(at);
    if (segment > 4) segment = 4;
    float blend = at - segment;
    return RGBA8(lerp_byte(stops[segment][0], stops[segment + 1][0], blend),
                 lerp_byte(stops[segment][1], stops[segment + 1][1], blend),
                 lerp_byte(stops[segment][2], stops[segment + 1][2], blend),
                 alpha);
}

static int update_map_field_texture(UiState *ui,
                                    const AppSnapshot *snapshot)
{
    enum { FIELD_WIDTH = 160, FIELD_HEIGHT = 96 };
    const WeatherMapField *field = &snapshot->map.field;
    if (field->point_count < WEATHER_MAP_POINTS || field->hour_count < 1)
        return 0;
    int layer = snapshot->map_layer;
    int hour = ui->map_hour;
    if (hour < 0) hour = 0;
    if (hour >= field->hour_count) hour = field->hour_count - 1;
    if (!ui->map_field_texture) {
        ui->map_field_texture = vita2d_create_empty_texture_format(
            FIELD_WIDTH, FIELD_HEIGHT,
            SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!ui->map_field_texture) return 0;
        vita2d_texture_set_filters(ui->map_field_texture,
                                  SCE_GXM_TEXTURE_FILTER_LINEAR,
                                  SCE_GXM_TEXTURE_FILTER_LINEAR);
        ui->map_field_revision = 0;
        ui->map_field_layer = -1;
        ui->map_field_hour = -1;
    }
    if (ui->map_field_revision == snapshot->map.field_revision &&
        ui->map_field_layer == layer && ui->map_field_hour == hour)
        return 1;
    unsigned int *pixels = vita2d_texture_get_datap(ui->map_field_texture);
    int stride = (int)(vita2d_texture_get_stride(ui->map_field_texture) / 4);
    for (int py = 0; py < FIELD_HEIGHT; ++py) {
        float unit_y = (py + 0.5f) / FIELD_HEIGHT;
        for (int px = 0; px < FIELD_WIDTH; ++px) {
            float value = 0.0f;
            int valid = map_field_sample(field, layer, hour,
                                         (px + 0.5f) / FIELD_WIDTH,
                                         unit_y, &value);
            pixels[py * stride + px] = valid
                                           ? map_field_color(layer, value)
                                           : RGBA8(0, 0, 0, 0);
        }
    }
    ui->map_field_revision = snapshot->map.field_revision;
    ui->map_field_layer = layer;
    ui->map_field_hour = hour;
    return 1;
}

static void draw_map_weather_layer(UiState *ui,
                                   const AppSnapshot *snapshot,
                                   float x, float y, float width,
                                   float height, double center_x,
                                   double center_y, float tile_size,
                                   uint64_t now_us)
{
    const MapSnapshot *map = &snapshot->map;
    if (map->field_zoom != ui->map_loaded_zoom ||
        !update_map_field_texture(ui, snapshot))
        return;
    double loaded_world = (double)(1u << ui->map_loaded_zoom);
    double delta_x = map->field_base_x - center_x;
    if (delta_x > loaded_world * 0.5) delta_x -= loaded_world;
    if (delta_x < -loaded_world * 0.5) delta_x += loaded_world;
    float field_x = x + width * 0.5f + (float)delta_x * tile_size;
    float field_y = y + height * 0.5f +
                    (float)(map->field_base_y - center_y) * tile_size;
    vita2d_draw_texture_tint_scale(ui->map_field_texture, field_x, field_y,
                                   tile_size / 32.0f, tile_size / 32.0f,
                                   RGBA8(255, 255, 255, g_draw_alpha));

    if (snapshot->map_layer != APP_MAP_WIND) return;
    float speed_value = 0.0f;
    if (!map_field_sample(&map->field, APP_MAP_WIND, ui->map_hour,
                          0.5f, 0.5f, &speed_value))
        return;
    int center_point = (WEATHER_MAP_ROWS / 2) * WEATHER_MAP_COLUMNS +
                       WEATHER_MAP_COLUMNS / 2;
    float direction = map->field.points[center_point].
                          wind_direction[ui->map_hour < map->field.hour_count
                                             ? ui->map_hour
                                             : map->field.hour_count - 1];
    float angle = (direction - 90.0f) * PI_F / 180.0f;
    float speed = clamp01(speed_value / 70.0f);
    float seconds = now_us / 1000000.0f;
    int stream_count = snapshot->motion == APP_MOTION_FULL ? 54 :
                       snapshot->motion == APP_MOTION_REDUCED ? 36 : 24;
    float forward_x = cosf(angle);
    float forward_y = sinf(angle);
    float side_x = -forward_y;
    float side_y = forward_x;
    for (int stream = 0; stream < stream_count; ++stream) {
        float depth = 0.62f + (stream % 7) * 0.065f;
        float cycle = width + height + 110.0f;
        float travel = snapshot->motion == APP_MOTION_OFF
                           ? fmodf(stream * 47.0f, cycle)
                           : fmodf(seconds * (29.0f + speed * 72.0f) * depth +
                                   stream * 47.0f, cycle);
        float cx = x + fmodf(17.0f + stream * 113.0f +
                             forward_x * travel + width, width);
        float cy = y + fmodf(29.0f + stream * 71.0f +
                             forward_y * travel + height, height);
        float curve = sinf(stream * 0.91f + seconds * 0.34f) * 2.2f;
        float tail = 29.0f + speed * 27.0f;
        float head_x = cx + forward_x * tail * 0.25f;
        float head_y = cy + forward_y * tail * 0.25f;
        float tail_x = head_x - forward_x * tail + side_x * curve;
        float tail_y = head_y - forward_y * tail + side_y * curve;
        if (tail_x >= x && tail_x <= x + width &&
            tail_y >= y && tail_y <= y + height &&
            head_x >= x && head_x <= x + width &&
            head_y >= y && head_y <= y + height)
            line_width(tail_x, tail_y, head_x, head_y, 0.65f,
                       RGBA8(230, 255, 248, 215));
    }
}

static void map_display_center(UiState *ui, int fullscreen,
                               float frame_dt, float width, float height,
                               float tile_size,
                               double *center_x, double *center_y)
{
    double loaded_world = (double)(1u << ui->map_loaded_zoom);
    double target_world = (double)(1u << ui->map_zoom);
    double raw_x = ui->map_camera_x / target_world * loaded_world;
    double raw_y = ui->map_camera_y / target_world * loaded_world;
    if (!fullscreen) {
        ui->map_present_x = raw_x;
        ui->map_present_y = raw_y;
        ui->map_present_zoom = ui->map_loaded_zoom;
        ui->map_present_initialized = 1;
        if (ui->map_edge_clamped)
            app_log("map fullscreen raster coverage restored: display=%.3f,%.3f",
                    raw_x, raw_y);
        ui->map_edge_clamped = 0;
        *center_x = raw_x;
        *center_y = raw_y;
        return;
    }
    double window_middle = ui->map_loaded_base_x +
                           MAP_TILE_COLUMNS * 0.5;
    raw_x = map_align_wrapped_center(raw_x, window_middle,
                                     ui->map_loaded_zoom);
    int initialized_now = 0;
    if (!ui->map_present_initialized) {
        ui->map_present_x = raw_x;
        ui->map_present_y = raw_y;
        ui->map_present_zoom = ui->map_loaded_zoom;
        ui->map_present_initialized = 1;
        initialized_now = 1;
    } else if (ui->map_present_zoom != ui->map_loaded_zoom) {
        double old_world = (double)(1u << ui->map_present_zoom);
        ui->map_present_x = ui->map_present_x / old_world * loaded_world;
        ui->map_present_y = ui->map_present_y / old_world * loaded_world;
        ui->map_present_zoom = ui->map_loaded_zoom;
    }
    double previous_x = map_align_wrapped_center(
        ui->map_present_x, window_middle, ui->map_loaded_zoom);
    double previous_y = ui->map_present_y;
    double target_x = map_align_wrapped_center(
        raw_x, previous_x, ui->map_loaded_zoom);
    float blend = 1.0f - expf(-10.5f * frame_dt);
    double eased_x = previous_x + (target_x - previous_x) * blend;
    double eased_y = previous_y + (raw_y - previous_y) * blend;
    /* A newly activated adjacent window can expose a large amount of queued
     * camera travel at once. Exponential easing alone still moved about 0.36
     * tile (roughly 100 fullscreen pixels) on that first release frame. Cap
     * the visible velocity while leaving the request camera responsive. */
    double maximum_step = fmin(0.08, fmax(0.025, 3.0 * frame_dt));
    double display_x = map_approach(previous_x, eased_x, maximum_step);
    double display_y = map_approach(previous_y, eased_y, maximum_step);
    int underlay_visible = 0;
    if (tile_size > 0.0f) {
        /* Fullscreen is cleared to a deterministic map-colored underlay every
         * frame. Let the camera keep moving when it outruns the current raster
         * instead of freezing it at the edge; uncovered pixels are therefore
         * clean gray/blue, never stale framebuffer data. The replacement
         * window activates at the same world-space camera, so it fills those
         * pixels without a positional snap. */
        double half_width = (width * 0.5 + 2.0) / tile_size;
        double half_height = (height * 0.5 + 2.0) / tile_size;
        double covered_x = map_clamp_window_center(
            display_x, ui->map_loaded_base_x, MAP_TILE_COLUMNS, half_width);
        double covered_y = map_clamp_window_center(
            display_y, ui->map_loaded_base_y, MAP_TILE_ROWS, half_height);
        underlay_visible = fabs(display_x - covered_x) > 0.0001 ||
                           fabs(display_y - covered_y) > 0.0001;
    }
    if (underlay_visible != ui->map_edge_clamped) {
        if (underlay_visible)
            app_log("map fullscreen loading underlay visible: camera=%.3f,%.3f display=%.3f,%.3f base=%d,%d",
                    raw_x, raw_y, display_x, display_y,
                    ui->map_loaded_base_x, ui->map_loaded_base_y);
        else if (ui->map_edge_clamped)
            app_log("map fullscreen raster coverage restored: display=%.3f,%.3f",
                    display_x, display_y);
        ui->map_edge_clamped = underlay_visible;
    }
    double previous_aligned = map_align_wrapped_center(
        previous_x, display_x, ui->map_loaded_zoom);
    float step_x = (float)fabs(display_x - previous_aligned);
    float step_y = (float)fabs(display_y - previous_y);
    /* The first raster frame has no preceding visible map frame to move from,
     * so it is an initialization placement rather than camera motion. */
    if (!initialized_now) {
        if (step_x > ui->map_present_peak_dx)
            ui->map_present_peak_dx = step_x;
        if (step_y > ui->map_present_peak_dy)
            ui->map_present_peak_dy = step_y;
    }
    ui->map_present_x = display_x;
    ui->map_present_y = display_y;
    ui->map_present_zoom = ui->map_loaded_zoom;
    *center_x = display_x;
    *center_y = display_y;
}

static const char *aqi_category(float value)
{
    if (value <= 50.0f) return "Good";
    if (value <= 100.0f) return "Moderate";
    if (value <= 150.0f) return "Sensitive";
    if (value <= 200.0f) return "Unhealthy";
    return "Very unhealthy";
}

static void draw_aqi_scale(float x, float y, float width, float height,
                           float value)
{
    static const unsigned char colors[6][3] = {
        {55, 195, 122}, {210, 205, 57}, {240, 154, 50},
        {229, 80, 68}, {145, 72, 164}, {115, 51, 89}
    };
    /* A continuous spectrum avoids the visible dividers produced by six
     * solid blocks, especially on the larger Details card. */
    for (int segment = 0; segment < 5; ++segment) {
        float left = x + segment * width / 5;
        float right = x + (segment + 1) * width / 5;
        horizontal_gradient(left, y, right - left, height,
            RGBA8(colors[segment][0], colors[segment][1], colors[segment][2], 255),
            RGBA8(colors[segment + 1][0], colors[segment + 1][1],
                  colors[segment + 1][2], 255));
    }
    circle(x + height * 0.5f, y + height * 0.5f, height * 0.5f,
           RGBA8(colors[0][0], colors[0][1], colors[0][2], 255));
    circle(x + width - height * 0.5f, y + height * 0.5f,
           height * 0.5f,
           RGBA8(colors[5][0], colors[5][1], colors[5][2], 255));
    float marker = clamp01(value / 300.0f);
    float marker_x = x + height * 0.5f +
                     marker * (width - height);
    circle(marker_x, y + height * 0.5f, height * 0.72f,
           RGBA8(250, 253, 255, 255));
    circle(marker_x, y + height * 0.5f, height * 0.31f,
           RGBA8(31, 42, 56, 255));
}

static long days_from_civil(int year, unsigned int month, unsigned int day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned int year_of_era = (unsigned int)(year - era * 400);
    unsigned int adjusted_month = month > 2 ? month - 3u : month + 9u;
    unsigned int day_of_year =
        (153u * adjusted_month + 2u) / 5u + day - 1u;
    unsigned int day_of_era = year_of_era * 365u + year_of_era / 4u -
                              year_of_era / 100u + day_of_year;
    return era * 146097L + (long)day_of_era - 719468L;
}

static void moon_phase_info(const char *date, char *phase, size_t phase_size,
                            int *illumination, int *days_to_full,
                            float *phase_fraction)
{
    int year = 2000;
    unsigned int month = 1;
    unsigned int day = 6;
    if (!date || sscanf(date, "%d-%u-%u", &year, &month, &day) != 3) {
        year = 2000;
        month = 1;
        day = 6;
    }
    const double cycle = 29.53058867;
    const long reference = days_from_civil(2000, 1, 6);
    double age = fmod((double)(days_from_civil(year, month, day) - reference) -
                      0.26, cycle);
    if (age < 0.0) age += cycle;
    double fraction = age / cycle;
    const char *name = fraction < 0.03 || fraction >= 0.97 ? "New Moon" :
                       fraction < 0.22 ? "Waxing Crescent" :
                       fraction < 0.28 ? "First Quarter" :
                       fraction < 0.47 ? "Waxing Gibbous" :
                       fraction < 0.53 ? "Full Moon" :
                       fraction < 0.72 ? "Waning Gibbous" :
                       fraction < 0.78 ? "Last Quarter" :
                                         "Waning Crescent";
    snprintf(phase, phase_size, "%s", name);
    *illumination = (int)lround((1.0 - cos(fraction * 2.0 * PI_F)) * 50.0);
    double until_full = fmod(cycle * 0.5 - age + cycle, cycle);
    *days_to_full = (int)ceil(until_full);
    *phase_fraction = (float)fraction;
}

static const char *wind_compass(float degrees)
{
    static const char *directions[] = {
        "N", "NE", "E", "SE", "S", "SW", "W", "NW"
    };
    int index = ((int)floorf((degrees + 22.5f) / 45.0f)) & 7;
    return directions[index];
}

static void draw_details(UiState *ui, const AppSnapshot *snapshot, float ox,
                         unsigned int page_alpha, uint64_t age_us)
{
    const WeatherData *weather = &snapshot->weather;
    Palette palette = palette_for(weather->is_day);
    int fahrenheit = snapshot->use_fahrenheit;
    const char *labels[8] = {
        tr(snapshot->language, TEXT_FEELS_LIKE),
        tr(snapshot->language, TEXT_UV_INDEX),
        tr(snapshot->language, TEXT_VISIBILITY),
        tr(snapshot->language, TEXT_PRESSURE),
        tr(snapshot->language, TEXT_AIR_QUALITY),
        tr(snapshot->language, TEXT_HUMIDITY),
        tr(snapshot->language, TEXT_WIND),
        "Sun & moon"
    };
    char values[8][40];
    char details[8][56];
    memset(details, 0, sizeof(details));
    snprintf(values[0], sizeof(values[0]), "%.0f°",
             weather_display_temperature(weather->apparent_c, fahrenheit));
    snprintf(details[0], sizeof(details[0]), "%.0f° %s",
             weather_display_temperature(weather->temperature_c, fahrenheit),
             weather_temperature_unit(fahrenheit));
    snprintf(values[1], sizeof(values[1]), "%.1f", weather->uv_index);
    snprintf(details[1], sizeof(details[1]), "0-2  /  3-5  /  6+");
    snprintf(values[2], sizeof(values[2]), "%.1f %s",
             weather_display_visibility(weather->visibility_km, fahrenheit),
             weather_visibility_unit(fahrenheit));
    snprintf(values[3], sizeof(values[3]), fahrenheit ? "%.2f %s" : "%.0f %s",
             weather_display_pressure(weather->pressure_hpa, fahrenheit),
             weather_pressure_unit(fahrenheit));
    snprintf(values[4], sizeof(values[4]), "AQI %.0f", weather->air_quality);
    snprintf(details[4], sizeof(details[4]), "%s · PM2.5 %.0f / PM10 %.0f",
             aqi_category(weather->air_quality), weather->pm2_5,
             weather->pm10);
    snprintf(values[5], sizeof(values[5]), "%d%%", weather->humidity);
    snprintf(details[5], sizeof(details[5]), "%s %.0f%%",
             tr(snapshot->language, TEXT_CLOUD_COVER), weather->cloud_cover);
    snprintf(values[6], sizeof(values[6]), "%.0f %s",
             weather_display_wind(weather->wind_kmh, fahrenheit),
             weather_wind_unit(fahrenheit));
    snprintf(details[6], sizeof(details[6]), "%s  %.0f°",
             wind_compass(weather->wind_direction), weather->wind_direction);
    char moon_phase[32];
    int moon_illumination = 0;
    int days_to_full = 0;
    float moon_fraction = 0.0f;
    moon_phase_info(weather->day_count ? weather->days[0].date : NULL,
                    moon_phase, sizeof(moon_phase), &moon_illumination,
                    &days_to_full, &moon_fraction);
    snprintf(values[7], sizeof(values[7]), "%s", moon_phase);
    char sunrise[24];
    char sunset[24];
    tr_format_time(sunrise, sizeof(sunrise),
                   weather->day_count ? weather->days[0].sunrise : "--:--",
                   snapshot->use_24_hour, snapshot->language);
    tr_format_time(sunset, sizeof(sunset),
                   weather->day_count ? weather->days[0].sunset : "--:--",
                   snapshot->use_24_hour, snapshot->language);
    snprintf(details[7], sizeof(details[7]), "%d%% lit · Full in %dd",
             moon_illumination, days_to_full);
    char sun_line[64];
    snprintf(sun_line, sizeof(sun_line), "Sun %s - %s", sunrise, sunset);

    const int icons[8] = {
        ICON_TEMPERATURE, ICON_SUN, ICON_VISIBILITY, ICON_PRESSURE,
        ICON_AIR, ICON_HUMIDITY, ICON_WIND, ICON_MOON
    };
    g_draw_alpha = staged_alpha(page_alpha, age_us, 0);
    float rise = staged_rise(age_us, 0);
    for (int i = 0; i < 8; ++i) {
        int column = i % 4;
        int row = i / 4;
        float x = ox + 28 + column * 229.0f;
        float y = 86 + row * 201.0f + rise;
        rounded_rect(x, y, 217, 190, 23, palette.surface);
        draw_metric_icon(x + 27, y + 34, icons[i], palette.muted);
        draw_fitted_text(ui, x + 43, y + 39, 153, palette.muted, 0.53f, labels[i]);
        draw_fitted_text(ui, x + 23, y + 99, i == 7 ? 136 : 172, palette.text,
                  i == 7 ? 0.69f : 1.06f, values[i]);
        if (details[i][0])
            draw_fitted_text(ui, x + 23, y + 139, 173, palette.muted, 0.50f,
                      details[i]);
        if (i == 4) {
            draw_aqi_scale(x + 23, y + 159, 171, 8,
                           weather->air_quality);
        } else if (i == 6) {
            float seconds = snapshot->motion == APP_MOTION_OFF ? 0.0f :
                sceKernelGetProcessTimeWide() / 1000000.0f;
            float angle = (weather->wind_direction - 90.0f) *
                          PI_F / 180.0f;
            float dx = cosf(angle);
            float dy = sinf(angle);
            float side_x = -dy;
            float side_y = dx;
            for (int stream = 0; stream < 12; ++stream) {
                float travel = fmodf(seconds * (25.0f + weather->wind_kmh) +
                                     stream * 29.0f, 188.0f);
                float cx = x + 16.0f + fmodf(23.0f + stream * 53.0f +
                                             dx * travel + 188.0f, 188.0f);
                float cy = y + 151.0f + fmodf(11.0f + stream * 17.0f +
                                              dy * travel + 28.0f, 28.0f);
                float curve = sinf(stream * 0.83f + seconds * 0.5f) * 2.0f;
                float head_x = cx + dx * 8.0f;
                float head_y = cy + dy * 8.0f;
                float mid_x = cx - dx * 7.0f + side_x * curve;
                float mid_y = cy - dy * 7.0f + side_y * curve;
                float tail_x = cx - dx * 18.0f + side_x * curve;
                float tail_y = cy - dy * 18.0f + side_y * curve;
                if (fminf(tail_x, fminf(mid_x, head_x)) >= x + 16 &&
                    fmaxf(tail_x, fmaxf(mid_x, head_x)) <= x + 201 &&
                    fminf(tail_y, fminf(mid_y, head_y)) >= y + 147 &&
                    fmaxf(tail_y, fmaxf(mid_y, head_y)) <= y + 181) {
                    line(tail_x, tail_y, mid_x, mid_y,
                         RGBA8(156, 244, 218, 110));
                    line(mid_x, mid_y, head_x, head_y,
                         RGBA8(218, 255, 245, 220));
                    circle(head_x, head_y, 1.4f,
                           RGBA8(240, 255, 250, 235));
                }
            }
        } else if (i == 7) {
            draw_fitted_text(ui, x + 23, y + 164, 173, palette.muted, 0.43f,
                      sun_line);
            float moon_x = x + 181.0f;
            float moon_y = y + 93.0f;
            circle(moon_x, moon_y, 15.0f, RGBA8(242, 239, 205, 245));
            if (moon_illumination < 97) {
                float lit = moon_illumination / 100.0f;
                float edge = moon_fraction < 0.5f
                                 ? 15.0f - lit * 30.0f
                                 : -15.0f + lit * 30.0f;
                for (int moon_column = -15; moon_column <= 15;
                     ++moon_column) {
                    int illuminated = moon_fraction < 0.5f
                                          ? moon_column >= edge
                                          : moon_column <= edge;
                    if (illuminated) continue;
                    float half = sqrtf(fmaxf(0.0f, 225.0f -
                                                   moon_column * moon_column));
                    rect(moon_x + moon_column, moon_y - half, 1.15f,
                         half * 2.0f,
                         RGBA8(27, 40, 66, 252));
                }
            }
        }
    }
}

static void draw_map_overlay(UiState *ui, const AppSnapshot *snapshot,
                             float x, float y, float width, float height)
{
    const WeatherData *weather = &snapshot->weather;
    uint32_t accent = snapshot->map_layer == APP_MAP_PRECIPITATION
                          ? RGBA8(76, 174, 242, 255)
                      : snapshot->map_layer == APP_MAP_WIND
                          ? RGBA8(104, 222, 181, 255)
                      : snapshot->map_layer == APP_MAP_AIR_QUALITY
                          ? RGBA8(221, 187, 85, 255)
                          : RGBA8(255, 155, 79, 255);
    int count = map_hour_count(weather);
    if (!count) return;
    int selected = ui->map_hour;
    if (selected < 0) selected = 0;
    if (selected >= count) selected = count - 1;
    int first_hour = weather->current_hour_index;
    if (first_hour < 0 || first_hour >= weather->hour_count) first_hour = 0;
    const WeatherHour *hour = &weather->hours[first_hour + selected];
    char time[16];
    if (selected == 0)
        snprintf(time, sizeof(time), "%s", tr(snapshot->language, TEXT_NOW));
    else
        tr_format_time(time, sizeof(time), hour->label,
                       snapshot->use_24_hour, snapshot->language);
    char value[40];
    float measurement = map_interpolated_value(ui, snapshot);
    float field_value = 0.0f;
    int spatial_value = snapshot->map.field_zoom == ui->map_loaded_zoom &&
        map_field_sample(&snapshot->map.field, snapshot->map_layer,
                         selected, 0.5f, 0.5f, &field_value);
    if (spatial_value) {
        measurement = field_value;
        if (snapshot->map_layer == APP_MAP_TEMPERATURE)
            measurement = weather_display_temperature(
                measurement, snapshot->use_fahrenheit);
        else if (snapshot->map_layer == APP_MAP_WIND)
            measurement = weather_display_wind(
                measurement, snapshot->use_fahrenheit);
    }
    float displayed_direction = map_interpolated_direction(ui, weather);
    if (spatial_value && snapshot->map_layer == APP_MAP_WIND) {
        int center_point = (WEATHER_MAP_ROWS / 2) * WEATHER_MAP_COLUMNS +
                           WEATHER_MAP_COLUMNS / 2;
        displayed_direction = snapshot->map.field.points[center_point].
            wind_direction[selected < snapshot->map.field.hour_count
                               ? selected
                               : snapshot->map.field.hour_count - 1];
    }
    if (snapshot->map_layer == APP_MAP_PRECIPITATION) {
        if (spatial_value)
            snprintf(value, sizeof(value), snapshot->use_fahrenheit
                         ? "%.2f in/h" : "%.1f mm/h",
                     snapshot->use_fahrenheit ? measurement / 25.4f : measurement);
        else
            snprintf(value, sizeof(value), "%.0f%% chance", measurement);
    }
    else if (snapshot->map_layer == APP_MAP_WIND)
        snprintf(value, sizeof(value), "%.0f %s  %s", measurement,
                 weather_wind_unit(snapshot->use_fahrenheit),
                 wind_compass(displayed_direction));
    else if (snapshot->map_layer == APP_MAP_AIR_QUALITY)
        snprintf(value, sizeof(value), "AQI %.0f  %s", measurement,
                 aqi_category(measurement));
    else
        snprintf(value, sizeof(value), "%.0f° %s", measurement,
                 weather_temperature_unit(snapshot->use_fahrenheit));

    rounded_rect(x + 12, y + 12, 392, 66, 19, RGBA8(9, 25, 48, 218));
    char layer_title[48];
    snprintf(layer_title, sizeof(layer_title), "%s %s",
             spatial_value ? "Forecast" : "Local",
             map_layer_name(snapshot));
    draw_fitted_text(ui, x + 29, y + 37, 272, RGBA8(226, 238, 249, 220), 0.50f,
              layer_title);
    draw_fitted_text(ui, x + 29, y + 64,
                     snapshot->map_layer == APP_MAP_AIR_QUALITY ? 221 : 288,
                     accent, 0.76f, value);
    int time_width = text_width(ui, 0.52f, time);
    draw_text(ui, x + 386 - time_width, y + 35,
              RGBA8(250, 252, 255, 235), 0.52f, time);
    if (snapshot->map_layer == APP_MAP_WIND) {
        float direction = displayed_direction;
        float angle = (direction - 90.0f) * PI_F / 180.0f;
        const float dial_x = x + 348.0f;
        const float dial_y = y + 52.0f;
        circle(dial_x, dial_y, 18, RGBA8(18, 49, 75, 238));
        circle(dial_x, dial_y, 2.5f, RGBA8(231, 248, 243, 245));
        line_width(dial_x - cosf(angle) * 12.0f,
                   dial_y - sinf(angle) * 12.0f,
                   dial_x + cosf(angle) * 12.0f,
                   dial_y + sinf(angle) * 12.0f, 1.1f,
                   RGBA8(140, 247, 211, 245));
        draw_text(ui, dial_x - 4, dial_y - 8,
                  RGBA8(231, 244, 249, 220), 0.37f, "N");
    } else if (snapshot->map_layer == APP_MAP_AIR_QUALITY) {
        draw_aqi_scale(x + 266, y + 51, 108, 7, measurement);
    }

    float panel_x = x + 12;
    float panel_y = y + height - 78;
    float panel_w = width - 24;
    rounded_rect(panel_x, panel_y, panel_w, 66, 20,
                 RGBA8(9, 25, 48, 218));
    float track_x = panel_x + 36;
    float track_y = panel_y + 28;
    float track_w = panel_w - 72;
    rounded_rect(track_x, track_y - 2, track_w, 4, 2,
                 RGBA8(211, 229, 244, 92));
    for (int i = 0; i < count; ++i) {
        float item_x = count > 1
                           ? track_x + track_w * i / (float)(count - 1)
                           : track_x;
        float radius = i % 6 == 0 || i == count - 1 ? 3.0f : 1.7f;
        circle(item_x, track_y, radius, RGBA8(233, 243, 252, 155));
    }
    float visual = ui->map_visual_hour;
    if (visual < 0.0f) visual = 0.0f;
    if (visual > count - 1) visual = (float)(count - 1);
    float knob_x = count > 1
                       ? track_x + track_w * visual / (float)(count - 1)
                       : track_x;
    circle(knob_x, track_y, 9, RGBA8(8, 27, 52, 210));
    circle(knob_x, track_y, 6, accent);

    static const int label_hours[] = {0, 6, 12, 18, 23};
    int previous_index = -1;
    for (int label = 0; label < 5; ++label) {
        int index = label_hours[label];
        if (index >= count) index = count - 1;
        if (index == previous_index) continue;
        previous_index = index;
        char tick_time[16];
        if (index == 0)
            snprintf(tick_time, sizeof(tick_time), "%s",
                     tr(snapshot->language, TEXT_NOW));
        else
            tr_format_time(tick_time, sizeof(tick_time),
                           weather->hours[first_hour + index].label,
                           snapshot->use_24_hour, snapshot->language);
        float label_x = count > 1
                            ? track_x + track_w * index / (float)(count - 1)
                            : track_x;
        int label_width = text_width(ui, 0.42f, tick_time);
        draw_text(ui, label_x - label_width * 0.5f, panel_y + 55,
                  RGBA8(222, 234, 246, 205), 0.42f, tick_time);
    }
}

static void draw_fullscreen_map_controls(float x, float y, float width)
{
    const float cx = x + width - 34.0f;
    const uint32_t panel = RGBA8(9, 25, 48, 218);
    const uint32_t glyph = RGBA8(240, 248, 254, 245);
    const float close_y = y + 35.0f;
    const float layer_y = y + 91.0f;
    circle(cx, close_y, 22.0f, panel);
    line_width(cx - 7, close_y - 7, cx + 7, close_y + 7, 2.0f, glyph);
    line_width(cx + 7, close_y - 7, cx - 7, close_y + 7, 2.0f, glyph);

    circle(cx, layer_y, 22.0f, panel);
    /* Two slim diamonds communicate map layers without a text label. */
    line_width(cx, layer_y - 10, cx + 11, layer_y - 4, 1.5f, glyph);
    line_width(cx + 11, layer_y - 4, cx, layer_y + 2, 1.5f, glyph);
    line_width(cx, layer_y + 2, cx - 11, layer_y - 4, 1.5f, glyph);
    line_width(cx - 11, layer_y - 4, cx, layer_y - 10, 1.5f, glyph);
    line_width(cx - 10, layer_y + 3, cx, layer_y + 9, 1.4f, glyph);
    line_width(cx, layer_y + 9, cx + 10, layer_y + 3, 1.4f, glyph);
}

static void draw_map(UiState *ui, const AppSnapshot *snapshot, float ox,
                     unsigned int page_alpha, uint64_t age_us)
{
    const MapSnapshot *map = &snapshot->map;
    Palette palette = palette_for(snapshot->weather.is_day);
    g_draw_alpha = staged_alpha(page_alpha, age_us, 0);
    int fullscreen = ui->map_fullscreen;
    float rise = fullscreen ? 0.0f : staged_rise(age_us, 0);
    uint32_t map_frame = snapshot->weather.is_day
                             ? RGBA8(17, 53, 88, 255)
                             : RGBA8(14, 29, 66, 255);
    if (!fullscreen)
        surface(ox + 28, 86 + rise, 904, 391, 27, map_frame);

    const float inner_x = fullscreen ? 0.0f : ox + 38;
    const float inner_y = fullscreen ? 0.0f : 96 + rise;
    const float inner_w = fullscreen ? SCREEN_W : 884;
    const float inner_h = fullscreen ? SCREEN_H : 340;
    int pixels_ready = map_texture_set_any(ui->map_tile_loaded);
    /* Fullscreen used to skip this clear once any tile was present. If the
     * camera outran the last complete 5x3 window, uncovered edge pixels came
     * from the previous framebuffer and looked like a constantly tearing
     * border. Keep a deterministic underlay in both layouts. */
    rect(inner_x, inner_y, inner_w, inner_h, RGBA8(14, 38, 66, 255));
    uint64_t map_now_us = sceKernelGetProcessTimeWide();
    if (!ui->map_visual_update_us) ui->map_visual_update_us = map_now_us;
    float zoom_dt = (float)(map_now_us - ui->map_visual_update_us) / 1000000.0f;
    if (zoom_dt < 0.0f) zoom_dt = 0.0f;
    if (zoom_dt > 0.05f) zoom_dt = 0.05f;
    ui->map_visual_update_us = map_now_us;
    float blend = 1.0f - expf(-11.0f * zoom_dt);
    ui->map_visual_zoom += (ui->map_zoom - ui->map_visual_zoom) * blend;
    ui->map_visual_hour += (ui->map_hour - ui->map_visual_hour) * blend;
    if (fabsf(ui->map_visual_zoom - ui->map_zoom) < 0.002f)
        ui->map_visual_zoom = (float)ui->map_zoom;
    if (fabsf(ui->map_visual_hour - ui->map_hour) < 0.002f)
        ui->map_visual_hour = (float)ui->map_hour;

    if (pixels_ready) {
        const float tile_height = inner_h;
        unsigned int texture_alpha = g_draw_alpha;
        double loaded_world = (double)(1u << ui->map_loaded_zoom);
        /* Keep the last complete raster level at its native scale while a new
         * level is staged. Shrinking a 5x3 window during zoom-out exposes and
         * magnifies its empty perimeter, which appeared as a distorted map on
         * Vita. The new level replaces it atomically when its window is ready. */
        float render_zoom = ui->map_loaded_zoom == ui->map_zoom
                                ? ui->map_visual_zoom
                                : (float)ui->map_loaded_zoom;
        float tile_size = 256.0f *
            powf(2.0f, render_zoom - ui->map_loaded_zoom);
        if (fullscreen) {
            /* Consecutive 5x3 windows need overlapping safe camera ranges.
             * A small fullscreen overscan scale prevents even a one-tile
             * vertical handoff from exposing a gap between those ranges. */
            float minimum_tile_size = fmaxf(
                (inner_w + 4.0f) / (MAP_TILE_COLUMNS - 1),
                (inner_h + 4.0f) / (MAP_TILE_ROWS - 1));
            if (tile_size < minimum_tile_size)
                tile_size = minimum_tile_size;
        }
        double center_x;
        double center_y;
        map_display_center(ui, fullscreen, zoom_dt,
                           inner_w, inner_h, tile_size, &center_x,
                           &center_y);
        float screen_center_x = inner_x + inner_w * 0.5f;
        float screen_center_y = inner_y + tile_height * 0.5f;
        vita2d_set_clip_rectangle((int)inner_x, (int)inner_y,
                                  (int)(inner_x + inner_w),
                                  (int)(inner_y + tile_height));
        vita2d_enable_clipping();
        for (int i = 0; i < MAP_TILE_COUNT; ++i) {
            if (!ui->map_tile_loaded[i] || !ui->map_tiles[i]) continue;
            int column = i % MAP_TILE_COLUMNS;
            int row = i / MAP_TILE_COLUMNS;
            double tile_x = ui->map_loaded_base_x + column;
            double delta_x = tile_x - center_x;
            if (delta_x > loaded_world * 0.5) delta_x -= loaded_world;
            if (delta_x < -loaded_world * 0.5) delta_x += loaded_world;
            double tile_y = ui->map_loaded_base_y + row;
            float map_x = screen_center_x + (float)delta_x * tile_size;
            float map_y = screen_center_y +
                          (float)(tile_y - center_y) * tile_size;
            if (map_x >= inner_x + inner_w || map_y >= inner_y + inner_h ||
                map_x + tile_size <= inner_x ||
                map_y + tile_size <= inner_y)
                continue;
            vita2d_draw_texture_tint_scale(
                ui->map_tiles[i], map_x, map_y,
                tile_size / 256.0f, tile_size / 256.0f,
                RGBA8(255, 255, 255, texture_alpha));
        }
        draw_map_weather_layer(ui, snapshot, inner_x, inner_y, inner_w,
                               inner_h, center_x, center_y, tile_size,
                               map_now_us);
        if (ui->map_loaded_zoom > 0) {
            double location_x = map_tile_x(snapshot->weather.longitude,
                                           ui->map_loaded_zoom);
            double location_y = map_tile_y(snapshot->weather.latitude,
                                           ui->map_loaded_zoom);
            double location_dx = location_x - center_x;
            if (location_dx > loaded_world * 0.5) location_dx -= loaded_world;
            if (location_dx < -loaded_world * 0.5) location_dx += loaded_world;
            float marker_x = screen_center_x + (float)location_dx * tile_size;
            float marker_y = screen_center_y +
                (float)(location_y - center_y) * tile_size;
            circle(marker_x, marker_y, 9, RGBA8(255, 255, 255, 245));
            circle(marker_x, marker_y, 5,
                   snapshot->gps_active ? RGBA8(49, 211, 127, 255)
                                        : RGBA8(30, 131, 226, 255));
        }
        /* The marker represents an exact geographic coordinate. Keep it
         * clipped to the map instead of letting it bleed across the page when
         * the user pans away; tapping the location row recenters it. */
        vita2d_disable_clipping();
    }
    if (!fullscreen)
        mask_rounded_map_corners(inner_x, inner_y, inner_w, inner_h, 19.0f,
                                 map_frame);
    draw_map_overlay(ui, snapshot, inner_x, inner_y, inner_w, inner_h);
    if (fullscreen)
        draw_fullscreen_map_controls(inner_x, inner_y, inner_w);
    char map_status[96];
    const char *map_style = app_map_provider_name(map->provider);
    const char *location_status = snapshot->gps_active ? "GPS location" : "Saved location";
    snprintf(map_status, sizeof(map_status), "%s%s", map_style,
             !pixels_ready || map->loading ? " · Loading…" : "");
    if (fullscreen) {
        int status_width = text_width(ui, 0.52f, map_status);
        float status_x = inner_x + inner_w - status_width - 104;
        rounded_rect(status_x - 13, inner_y + 14, status_width + 26, 34, 17,
                     RGBA8(9, 25, 48, 210));
        draw_text(ui, status_x, inner_y + 37, RGBA8(240, 247, 253, 238),
                  0.52f, map_status);
    }
    char attribution[160];
    snprintf(attribution, sizeof(attribution), "%s%s",
             app_map_provider_attribution(map->provider),
             map->field.point_count > 0
                 ? " | Weather: Open-Meteo / CAMS" : "");
    int attribution_width = text_width(ui, 0.40f, attribution);
    float attribution_right = fullscreen ? 84.0f : 20.0f;
    rounded_rect(inner_x + inner_w - attribution_width - attribution_right,
                 inner_y + inner_h - 104, attribution_width + 14, 23, 8,
                 RGBA8(9, 25, 48, 218));
    draw_text(ui, inner_x + inner_w - attribution_width - attribution_right + 7,
              inner_y + inner_h - 86, RGBA8(239, 246, 252, 205),
              0.40f, attribution);
    if (!fullscreen) {
        char location[160];
        snprintf(location, sizeof(location), "%s · %s",
                 snapshot->weather.location, location_status);
        draw_fitted_text(ui, ox + 54, 462 + rise, 526,
                         palette.muted, 0.55f, location);
        draw_text(ui, ox + 904 - text_width(ui, 0.50f, map_status),
                  462 + rise, palette.muted, 0.50f, map_status);
    } else {
        draw_text(ui, inner_x + 17, inner_y + inner_h - 88,
                  RGBA8(239, 246, 252, 220), 0.43f,
                  "<> Time    △ Layers    ○ Windowed");
    }
}

static void draw_page(UiState *ui, const AppSnapshot *snapshot, int view,
                      float offset, unsigned int alpha, uint64_t age_us)
{
    if (view == VIEW_NOW)
        draw_now(ui, snapshot, offset, alpha, age_us);
    else if (view == VIEW_HOURLY)
        draw_hourly(ui, snapshot, offset, alpha, age_us);
    else if (view == VIEW_DAILY)
        draw_daily(ui, snapshot, offset, alpha, age_us);
    else if (view == VIEW_MAP)
        draw_map(ui, snapshot, offset, alpha, age_us);
    else
        draw_details(ui, snapshot, offset, alpha, age_us);
}

static void draw_button_hint(UiState *ui, float group_x, float group_width,
                             const char *button, const char *label,
                             uint32_t tint)
{
    (void)tint; /* All hints share one quiet treatment; focus lives in content. */
    int glyph_width = text_width(ui, 0.56f, button);
    float key_width = glyph_width + 14.0f;
    if (key_width < 27.0f) key_width = 27.0f;
    if (key_width > 58.0f) key_width = 58.0f;
    char fitted_label[96];
    fit_text(ui, fitted_label, sizeof(fitted_label), 0.50f,
             group_width - key_width - 18, label);
    int label_width = text_width(ui, 0.50f, fitted_label);
    float content_width = key_width + 8.0f + label_width;
    float x = group_x + (group_width - content_width) * 0.5f;
    rounded_rect(x, 505, key_width, 27, 10, RGBA8(191, 214, 236, 34));
    draw_text(ui, x + (key_width - glyph_width) * 0.5f, 523,
              RGBA8(233, 245, 255, 255), 0.56f, button);
    draw_text(ui, x + key_width + 8, 523,
              RGBA8(220, 233, 247, 230), 0.50f, fitted_label);
}

static void draw_footer(UiState *ui, const AppSnapshot *snapshot)
{
    g_draw_alpha = 255;
    rounded_rect(17, 496, 926, 42, 18, RGBA8(7, 20, 37, 175));
    int groups = ui->view == VIEW_DAILY ? 6 :
                 ui->view == VIEW_MAP ? 6 :
                 ui->view == VIEW_HOURLY ? 5 : 4;
    float group_width = 912.0f / groups;
    for (int i = 1; i < groups; ++i)
        rect(24 + i * group_width, 506, 1, 22, RGBA8(225, 239, 250, 28));
    int group = 0;
    if (ui->view == VIEW_DAILY) {
        draw_button_hint(ui, 24, group_width, "×",
                         tr(snapshot->language, TEXT_SELECT_DAY),
                         RGBA8(131, 211, 255, 255));
        group = 1;
    }
    if (ui->view == VIEW_MAP) {
        draw_button_hint(ui, 24, group_width, "×", "Full map",
                         RGBA8(131, 211, 255, 255));
        draw_button_hint(ui, 24 + group_width, group_width, "△", "Layers",
                         RGBA8(146, 232, 184, 255));
        draw_button_hint(ui, 24 + group_width * 2, group_width, "<>", "Time",
                         RGBA8(117, 207, 255, 255));
        draw_button_hint(ui, 24 + group_width * 3, group_width, "LS", "Pan",
                         RGBA8(255, 224, 143, 255));
        draw_button_hint(ui, 24 + group_width * 4, group_width, "RS", "Zoom",
                         RGBA8(198, 190, 255, 255));
        draw_button_hint(ui, 24 + group_width * 5, group_width, "START",
                         tr(snapshot->language, TEXT_SETTINGS),
                         RGBA8(222, 230, 242, 240));
        return;
    }
    draw_button_hint(ui, 24 + group_width * group++, group_width, "□",
                     tr(snapshot->language, TEXT_LOCATIONS),
                     RGBA8(255, 169, 203, 255));
    draw_button_hint(ui, 24 + group_width * group++, group_width, "△",
                     ui->view == VIEW_MAP
                         ? tr(snapshot->language, TEXT_MAP_LAYER)
                         : tr(snapshot->language, TEXT_UNITS),
                     RGBA8(146, 232, 184, 255));
    if (ui->view == VIEW_HOURLY) {
        draw_button_hint(ui, 24 + group_width * group++, group_width, "<>",
                         "24 hours", RGBA8(117, 207, 255, 255));
    }
    draw_button_hint(ui, 24 + group_width * group++, group_width, "L/R",
                     tr(snapshot->language, TEXT_PAGES),
                     RGBA8(204, 216, 231, 238));
    if (ui->view == VIEW_DAILY) {
        draw_button_hint(ui, 24 + group_width * group++, group_width, "↑↓",
                         tr(snapshot->language, TEXT_SELECT),
                         RGBA8(255, 224, 143, 255));
    }
    draw_button_hint(ui, 24 + group_width * group, group_width,
                     "START", tr(snapshot->language, TEXT_SETTINGS),
                     RGBA8(222, 230, 242, 240));
}

static void draw_modal_footer(UiState *ui, const AppSnapshot *snapshot)
{
    g_draw_alpha = 255;
    rounded_rect(17, 496, 926, 42, 21, RGBA8(7, 20, 43, 218));
    if (ui->screen == UI_SCREEN_SETTINGS) {
        float width = 912.0f / 3.0f;
        draw_button_hint(ui, 24, width, "L/R", "Category",
                         RGBA8(198, 190, 255, 255));
        draw_button_hint(ui, 24 + width, width, "×", "Change",
                         RGBA8(131, 211, 255, 255));
        draw_button_hint(ui, 24 + width * 2.0f, width, "○",
                         tr(snapshot->language, TEXT_BACK),
                         RGBA8(204, 216, 231, 238));
    } else if (ui->screen == UI_SCREEN_LOCATIONS) {
        float width = 912.0f / 4.0f;
        draw_button_hint(ui, 24, width, "×",
                         tr(snapshot->language, TEXT_SELECT),
                         RGBA8(131, 211, 255, 255));
        draw_button_hint(ui, 24 + width, width, "□",
                         tr(snapshot->language, TEXT_SEARCH),
                         RGBA8(255, 169, 203, 255));
        draw_button_hint(ui, 24 + width * 2, width, "△",
                         tr(snapshot->language, TEXT_REMOVE),
                         RGBA8(146, 232, 184, 255));
        draw_button_hint(ui, 24 + width * 3, width, "○",
                         tr(snapshot->language, TEXT_BACK),
                         RGBA8(204, 216, 231, 238));
    } else if (ui->screen == UI_SCREEN_DAY_DETAIL) {
        float width = 912.0f / 4.0f;
        draw_button_hint(ui, 24, width, "LS", "Scroll",
                         RGBA8(131, 211, 255, 255));
        draw_button_hint(ui, 24 + width, width, "△", "Metrics",
                         RGBA8(255, 224, 143, 255));
        if (ui->day_detail_metric == DAY_METRIC_CONDITIONS)
            draw_button_hint(ui, 24 + width * 2.0f, width, "<>",
                             "Actual / Feels", RGBA8(146, 232, 184, 255));
        draw_button_hint(ui, 24 + width * 3.0f, width, "○",
                         tr(snapshot->language, TEXT_BACK),
                         RGBA8(204, 216, 231, 238));
    } else {
        if (ui->ime_active) {
            const char *hint = "Close the keyboard to browse suggestions";
            int width = text_width(ui, 0.55f, hint);
            draw_text(ui, (SCREEN_W - width) * 0.5f, 523,
                      RGBA8(247, 251, 255, 225), 0.55f, hint);
        } else {
            float width = 912.0f / 3.0f;
            draw_button_hint(ui, 24, width, "↑↓",
                             tr(snapshot->language, TEXT_SELECT),
                             RGBA8(255, 224, 143, 255));
            draw_button_hint(ui, 24 + width, width, "×", "Choose",
                             RGBA8(131, 211, 255, 255));
            draw_button_hint(ui, 24 + width * 2, width, "○",
                             tr(snapshot->language, TEXT_BACK),
                             RGBA8(204, 216, 231, 238));
        }
    }
}

static void draw_modal_backdrop(void)
{
    rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(3, 10, 24, 164));
}

static const char *settings_value(const AppSnapshot *snapshot, int row)
{
    switch (row) {
    case APP_SETTING_TEMPERATURE:
        return snapshot->use_fahrenheit
                   ? tr(snapshot->language, TEXT_IMPERIAL)
                   : tr(snapshot->language, TEXT_METRIC);
    case APP_SETTING_TIME_FORMAT:
        return snapshot->use_24_hour ? "24-hour" : "12-hour AM/PM";
    case APP_SETTING_LANGUAGE:
        return tr_language_name(snapshot->language);
    case APP_SETTING_THEME:
        return snapshot->theme == APP_THEME_DAY ? tr(snapshot->language, TEXT_DAY) :
               snapshot->theme == APP_THEME_NIGHT ? tr(snapshot->language, TEXT_NIGHT) :
               tr(snapshot->language, TEXT_AUTO);
    case APP_SETTING_BACKGROUND:
        return snapshot->photo_background
                   ? tr(snapshot->language, TEXT_PHOTOS)
                   : tr(snapshot->language, TEXT_CLASSIC);
    case APP_SETTING_MOTION:
        return snapshot->motion == APP_MOTION_FULL ? tr(snapshot->language, TEXT_FULL) :
               snapshot->motion == APP_MOTION_REDUCED ? tr(snapshot->language, TEXT_REDUCED) :
               tr(snapshot->language, TEXT_STATIC);
    case APP_SETTING_WEATHER_SOUNDS:
        return snapshot->weather_sounds ? tr(snapshot->language, TEXT_ON)
                                        : tr(snapshot->language, TEXT_OFF);
    case APP_SETTING_MAP_PROVIDER:
        return app_map_provider_name(snapshot->map_provider);
    default:
        return snapshot->gps_active ? "GPS active" :
               snapshot->auto_gps ? "On - waiting" :
                                    tr(snapshot->language, TEXT_OFF);
    }
}

static int settings_tab_start(int tab)
{
    static const int starts[3] = {
        APP_SETTING_TEMPERATURE, APP_SETTING_THEME, APP_SETTING_MAP_PROVIDER
    };
    return starts[tab < 0 ? 0 : tab > 2 ? 2 : tab];
}

static int settings_tab_count(int tab)
{
    static const int counts[3] = {3, 4, 2};
    return counts[tab < 0 ? 0 : tab > 2 ? 2 : tab];
}

static void draw_settings_rows(UiState *ui, const AppSnapshot *snapshot,
                               int tab, int selected_row, float offset_x)
{
    static const TextKey row_keys[] = {
        TEXT_UNITS, TEXT_TIME_FORMAT, TEXT_LANGUAGE,
        TEXT_APPEARANCE, TEXT_BACKGROUND, TEXT_MOTION, TEXT_WEATHER_SOUNDS,
        TEXT_MAP_STYLE, TEXT_AUTO_GPS
    };
    int start = settings_tab_start(tab);
    int count = settings_tab_count(tab);
    for (int item = 0; item < count; ++item) {
        int row = start + item;
        float y = 164.0f + item * 63.0f;
        if (row == selected_row)
            draw_focus(64 + offset_x, y, 832, 52);
        else if (item)
            rect(86 + offset_x, y - 6, 788, 1,
                 RGBA8(170, 196, 224, 28));
        uint32_t label_color = RGBA8(239, 245, 255, 244);
        uint32_t value_color = RGBA8(204, 226, 245, 255);
        draw_fitted_text(ui, 90 + offset_x, y + 33, 428, label_color, 0.68f,
                  tr(snapshot->language, row_keys[row]));
        const char *value = settings_value(snapshot, row);
        int width = text_width(ui, 0.58f, value);
        float pill_w = width + 28.0f;
        if (pill_w < 92.0f) pill_w = 92.0f;
        float pill_x = 870.0f - pill_w + offset_x;
        rounded_rect(pill_x, y + 13, pill_w, 27, 13.5f,
                     row == selected_row
                         ? RGBA8(131, 184, 222, 35)
                         : RGBA8(18, 42, 75, 150));
        draw_text(ui, pill_x + (pill_w - width) * 0.5f, y + 33,
                  value_color, 0.58f, value);
    }
}

static void draw_settings(UiState *ui, const AppSnapshot *snapshot)
{
    uint64_t now_us = sceKernelGetProcessTimeWide();
    uint64_t elapsed = now_us - ui->overlay_open_us;
    float reveal = elapsed >= 180000u ? 1.0f : elapsed / 180000.0f;
    reveal = reveal * reveal * (3.0f - 2.0f * reveal);
    unsigned int base_alpha = (unsigned int)(255.0f * reveal);
    g_draw_alpha = base_alpha;
    draw_modal_backdrop();
    surface(18, 14, 924, 468, 28, RGBA8(24, 34, 49, 252));
    draw_text(ui, 50, 58, RGBA8(250, 252, 255, 255), 1.02f,
              tr(snapshot->language, TEXT_SETTINGS));
    draw_text(ui, 50, 80, RGBA8(164, 190, 221, 225), 0.50f,
              "Make the forecast yours");

    static const char *const tabs[3] = {
        "Display", "Appearance", "Map & GPS"
    };
    for (int tab = 0; tab < 3; ++tab) {
        float x = 50.0f + tab * 290.0f;
        int selected = tab == ui->settings_tab;
        rounded_rect(x, 94, 274, 42, 18,
                     selected ? RGBA8(129, 151, 177, 95)
                              : RGBA8(42, 58, 79, 120));
        draw_centered_text(ui, x + 137, 121,
                  selected ? RGBA8(255, 255, 255, 255)
                           : RGBA8(207, 221, 237, 235),
                  0.62f, tabs[tab]);
    }

    uint64_t tab_elapsed = now_us - ui->settings_transition_start_us;
    int tab_transition = ui->settings_transition_start_us &&
                         tab_elapsed < TRANSITION_US;
    float panel_height = 26.0f + settings_tab_count(ui->settings_tab) * 63.0f;
    if (tab_transition) {
        float previous_height = 26.0f +
            settings_tab_count(ui->settings_previous_tab) * 63.0f;
        float t = ease_out_cubic((float)tab_elapsed / TRANSITION_US);
        panel_height = previous_height + (panel_height - previous_height) * t;
    }
    rounded_rect(50, 151, 860, panel_height, 22, RGBA8(64, 82, 105, 95));
    vita2d_set_clip_rectangle(50, 151, 910, 425);
    vita2d_enable_clipping();
    if (tab_transition) {
        float t = ease_out_cubic((float)tab_elapsed / TRANSITION_US);
        float old_offset = -ui->settings_transition_direction * 38.0f * t;
        float new_offset = ui->settings_transition_direction * 48.0f *
                           (1.0f - t);
        g_draw_alpha = (unsigned int)(base_alpha * (1.0f - t));
        draw_settings_rows(ui, snapshot, ui->settings_previous_tab,
                           ui->settings_previous_selected, old_offset);
        g_draw_alpha = (unsigned int)(base_alpha * t);
        draw_settings_rows(ui, snapshot, ui->settings_tab,
                           ui->settings_selected, new_offset);
    } else {
        g_draw_alpha = base_alpha;
        draw_settings_rows(ui, snapshot, ui->settings_tab,
                           ui->settings_selected, 0.0f);
    }
    vita2d_disable_clipping();
    g_draw_alpha = base_alpha;
    draw_text(ui, 69, 173 + panel_height, RGBA8(170, 196, 224, 225), 0.49f,
              ui->settings_tab == 2
                  ? "Native GPS status is shown in Locations and the map marker."
                  : "Changes save automatically and apply immediately.");
    draw_modal_footer(ui, snapshot);
}

static void draw_map_options(UiState *ui, const AppSnapshot *snapshot)
{
    draw_modal_backdrop();
    surface(174, 42, 612, 370, 28, RGBA8(24, 34, 49, 252));
    draw_text(ui, 210, 83, RGBA8(250, 252, 255, 255), 1.00f,
              "Map layers");
    draw_text(ui, 210, 106, RGBA8(165, 192, 222, 230), 0.50f,
              "Choose the forecast overlay and base map");

    const int layer_icons[APP_MAP_LAYER_COUNT] = {
        ICON_TEMPERATURE, ICON_RAIN, ICON_WIND, ICON_AIR
    };
    for (int row = 0; row < APP_MAP_LAYER_COUNT; ++row) {
        float y = 122.0f + row * 45.0f;
        int selected = ui->map_options_selected == row;
        if (selected)
            draw_focus(198, y, 564, 39);
        else if (row)
            rect(216, y, 528, 1, RGBA8(173, 198, 226, 26));
        draw_metric_icon(222, y + 20, layer_icons[row], RGBA8(193, 219, 240, 255));
        draw_text(ui, 240, y + 25,
                  RGBA8(241, 247, 253, 246),
                  0.65f, map_layer_label(snapshot, (AppMapLayer)row));
        if (snapshot->map_layer == (AppMapLayer)row) {
            draw_check(724, y + 20, RGBA8(145, 218, 255, 255));
        }
    }

    draw_text(ui, 210, 326, RGBA8(159, 188, 220, 238), 0.46f,
              "Base map");
    const char *provider = app_map_provider_name(snapshot->map_provider);
    int provider_selected =
        ui->map_options_selected == APP_MAP_LAYER_COUNT;
    rounded_rect(198, 337, 564, 41, 16, RGBA8(48, 72, 108, 118));
    if (provider_selected) draw_focus(198, 337, 564, 41);
    draw_text(ui, 220, 363,
              RGBA8(241, 247, 253, 246),
              0.60f, "Provider");
    int provider_width = text_width(ui, 0.52f, provider);
    draw_text(ui, 737 - provider_width, 363,
              RGBA8(154, 210, 242, 245),
              0.52f, provider);
    draw_modal_footer(ui, snapshot);
}

static void draw_locations(UiState *ui, const AppSnapshot *snapshot)
{
    draw_modal_backdrop();
    int rows = snapshot->saved_location_count + 1;
    surface(112, 48, 736, 99 + rows * 37, 28, RGBA8(24, 34, 49, 252));
    draw_text(ui, 147, 94, RGBA8(250, 252, 255, 255), 1.06f,
              tr(snapshot->language, TEXT_LOCATIONS));
    draw_text(ui, 147, 119, RGBA8(177, 194, 219, 230), 0.55f,
              tr(snapshot->language, TEXT_SAVED_LOCATIONS));
    for (int row = 0; row < rows; ++row) {
        float y = 130 + row * 37.0f;
        int selected = row == ui->location_selected;
        if (selected)
            draw_focus(138, y, 684, 33);
        const char *name;
        const char *region;
        if (row == 0) {
            name = snapshot->gps_active
                       ? tr(snapshot->language, TEXT_GPS_ACTIVE)
                       : tr(snapshot->language, TEXT_USE_GPS);
            region = snapshot->gps_active && snapshot->weather.region[0]
                         ? snapshot->weather.region
                     : snapshot->gps_notice[0] ? snapshot->gps_notice
                     : (strstr(snapshot->notice, "Location") ||
                        strstr(snapshot->notice, "GPS")) ? snapshot->notice
                         : tr(snapshot->language, TEXT_GPS_INACTIVE);
        } else {
            const WeatherLocation *location = &snapshot->saved_locations[row - 1];
            name = location->name;
            region = location->region;
        }
        uint32_t main_color = RGBA8(239, 245, 255, 245);
        uint32_t sub_color = RGBA8(184, 206, 228, 238);
        if (row == 0) {
            draw_metric_icon(161, y + 17, ICON_LOCATION,
                   snapshot->gps_active ? RGBA8(75, 224, 162, 255)
                                        : RGBA8(178, 200, 222, 255));
        } else if (!snapshot->gps_active &&
                   row - 1 == snapshot->selected_location) {
            draw_check(161, y + 17, RGBA8(145, 218, 255, 255));
        }
        draw_fitted_text(ui, 180, y + 23, 298, main_color, 0.66f, name);
        char region_label[256];
        fit_text(ui, region_label, sizeof(region_label), 0.50f, 304, region);
        draw_text(ui, 800 - text_width(ui, 0.50f, region_label), y + 22,
                  sub_color, 0.50f, region_label);
    }
    draw_modal_footer(ui, snapshot);
}

static void draw_search(UiState *ui, const AppSnapshot *snapshot)
{
    draw_modal_backdrop();
    surface(94, 18, 772, 290, 26, RGBA8(24, 34, 49, 252));
    draw_text(ui, 126, 61, RGBA8(250, 252, 255, 255), 0.92f,
              tr(snapshot->language, TEXT_FIND_LOCATION));
    rounded_rect(122, 76, 716, 42, 15, RGBA8(122, 153, 187, 53));
    char query_label[128];
    fit_text(ui, query_label, sizeof(query_label), 0.72f, 665,
             ui->search_query[0] ? ui->search_query
                                 : tr(snapshot->language, TEXT_TYPE_TO_SEARCH));
    draw_text(ui, 143, 103, ui->search_query[0] ? RGBA8(245, 249, 255, 255)
                                             : RGBA8(177, 200, 225, 235),
              0.72f, query_label);
    if (ui->ime_active &&
        ((sceKernelGetProcessTimeWide() / 500000u) & 1u) == 0) {
        int cursor_x = ui->search_query[0]
                           ? text_width(ui, 0.72f, query_label)
                           : 0;
        rect(145 + cursor_x, 86, 2, 23, RGBA8(136, 210, 255, 255));
    }
    if (snapshot->search_busy)
        draw_spinner(sceKernelGetProcessTimeWide());
    int count = snapshot->search_result_count;
    for (int i = 0; i < count; ++i) {
        float y = 126 + i * 33.0f;
        if (i == ui->search_selected)
            draw_focus(120, y, 720, 30);
        draw_fitted_text(ui, 142, y + 22, 330, RGBA8(244, 248, 255, 248), 0.64f,
                  snapshot->search_results[i].name);
        char region[192];
        fit_text(ui, region, sizeof(region), 0.50f, 321,
                 snapshot->search_results[i].region);
        draw_text(ui, 817 - text_width(ui, 0.50f, region), y + 21,
                  RGBA8(184, 206, 228, 238), 0.50f, region);
    }
    if (!snapshot->search_busy && ui->search_query[0] && !count)
        draw_fitted_text(ui, 142, 155, 670, RGBA8(174, 194, 222, 230), 0.58f,
                  snapshot->search_notice[0] ? snapshot->search_notice
                                             : tr(snapshot->language, TEXT_NO_MATCHES));
    draw_modal_footer(ui, snapshot);
}

static int collect_day_hours(const WeatherData *weather, const char *date,
                             int *indices, int capacity)
{
    int matches[24];
    int match_count = 0;
    for (int i = 0; i < weather->hour_count && match_count < 24; ++i) {
        if (!strncmp(weather->hours[i].time, date, 10))
            matches[match_count++] = i;
    }
    if (!match_count || capacity < 1) return 0;
    int count = match_count < capacity ? match_count : capacity;
    for (int i = 0; i < count; ++i) {
        int at = count == 1 ? 0 : i * (match_count - 1) / (count - 1);
        indices[i] = matches[at];
    }
    return count;
}

static const char *day_metric_name(int metric)
{
    static const char *names[DAY_METRIC_COUNT] = {
        "Conditions", "UV Index", "Wind", "Precipitation",
        "Humidity", "Visibility", "Pressure"
    };
    return metric >= 0 && metric < DAY_METRIC_COUNT
               ? names[metric] : names[0];
}

static const char *day_metric_description(int metric)
{
    static const char *descriptions[DAY_METRIC_COUNT] = {
        "The actual temperature and how it feels over the selected day.",
        "The UV index over time. Protection is recommended as levels rise.",
        "Sustained wind speed and direction over the selected day.",
        "The hourly chance of precipitation over the selected day.",
        "Relative humidity over time. Higher values can feel more humid.",
        "The distance at which objects can be clearly seen.",
        "Atmospheric pressure over time at the forecast location."
    };
    return metric >= 0 && metric < DAY_METRIC_COUNT
               ? descriptions[metric] : descriptions[0];
}

static uint32_t day_metric_color(int metric)
{
    switch (metric) {
    case DAY_METRIC_UV: return RGBA8(255, 202, 65, 255);
    case DAY_METRIC_WIND: return RGBA8(72, 208, 220, 255);
    case DAY_METRIC_PRECIPITATION: return RGBA8(100, 214, 255, 255);
    case DAY_METRIC_HUMIDITY: return RGBA8(111, 204, 255, 255);
    case DAY_METRIC_VISIBILITY: return RGBA8(139, 197, 255, 255);
    case DAY_METRIC_PRESSURE: return RGBA8(205, 180, 255, 255);
    default: return RGBA8(255, 180, 34, 255);
    }
}

static uint32_t day_metric_color_alpha(int metric, unsigned int alpha)
{
    return (day_metric_color(metric) & 0x00ffffffu) |
           ((alpha & 0xffu) << 24);
}

/* Elapsed samples keep exactly the same hue as the live curve.  A uniform
 * luminance reduction makes the past visibly quieter without letting the
 * hatched background change the apparent color of individual dashes. */
static uint32_t day_metric_elapsed_color(int metric, unsigned int alpha)
{
    uint32_t source = day_metric_color(metric);
    unsigned int red = source & 0xffu;
    unsigned int green = (source >> 8) & 0xffu;
    unsigned int blue = (source >> 16) & 0xffu;
    red = red * 72u / 100u;
    green = green * 72u / 100u;
    blue = blue * 72u / 100u;
    return RGBA8(red, green, blue, alpha);
}

static float day_metric_value(const WeatherData *weather,
                              const WeatherHour *hour, int metric,
                              int feels_like, int imperial)
{
    switch (metric) {
    case DAY_METRIC_UV:
        return hour->uv_index;
    case DAY_METRIC_WIND:
        return weather_display_wind(hour->wind_kmh, imperial);
    case DAY_METRIC_PRECIPITATION:
        return (float)hour->precipitation_percent;
    case DAY_METRIC_HUMIDITY:
        return hour->humidity > 0 ? (float)hour->humidity
                                  : (float)weather->humidity;
    case DAY_METRIC_VISIBILITY:
        return weather_display_visibility(
            hour->visibility_km > 0.0f ? hour->visibility_km
                                       : weather->visibility_km,
            imperial);
    case DAY_METRIC_PRESSURE:
        return weather_display_pressure(
            hour->pressure_hpa > 800.0f ? hour->pressure_hpa
                                        : weather->pressure_hpa,
            imperial);
    default:
        return weather_display_temperature(
            feels_like ? hour->apparent_c : hour->temperature_c,
            imperial);
    }
}

static void format_day_metric_axis(char *out, size_t out_size, int metric,
                                   float value, int imperial)
{
    switch (metric) {
    case DAY_METRIC_CONDITIONS:
        snprintf(out, out_size, "%.0f°", value);
        break;
    case DAY_METRIC_PRECIPITATION:
    case DAY_METRIC_HUMIDITY:
        snprintf(out, out_size, "%.0f%%", value);
        break;
    case DAY_METRIC_PRESSURE:
        snprintf(out, out_size, imperial ? "%.1f" : "%.0f", value);
        break;
    default:
        snprintf(out, out_size, "%.0f", value);
        break;
    }
}

static int weather_hour_minutes(const WeatherHour *hour)
{
    if (hour->time[10] == 'T') return clock_minutes(hour->time + 11);
    return clock_minutes(hour->label);
}

static void draw_elapsed_hours(const WeatherData *weather,
                               const WeatherDay *day, float x, float top,
                               float width, float bottom)
{
    if (strncmp(weather->updated, day->date, 10)) return;
    const char *time = strchr(weather->updated, 'T');
    int minutes = time ? clock_minutes(time + 1) : -1;
    if (minutes <= 0) return;
    float past_width = width * minutes / 1440.0f;
    if (past_width > width) past_width = width;
    rect(x, top, past_width, bottom - top, RGBA8(18, 20, 25, 96));
    for (float stripe = x - (bottom - top); stripe < x + past_width;
         stripe += 15.0f) {
        float x1 = stripe;
        float y1 = top;
        float x2 = stripe + (bottom - top);
        float y2 = bottom;
        if (x1 < x) {
            y1 += x - x1;
            x1 = x;
        }
        if (x2 > x + past_width) {
            y2 -= x2 - (x + past_width);
            x2 = x + past_width;
        }
        if (x2 > x1)
            line(x1, y1, x2, y2, RGBA8(8, 10, 14, 54));
    }
}

static void draw_day_time_axis(UiState *ui, const AppSnapshot *snapshot,
                               float x, float width, float y)
{
    static const char *const labels_12[] = {"12AM", "6AM", "12PM", "6PM"};
    static const char *const labels_24[] = {"00:00", "06:00", "12:00", "18:00"};
    const char *const *labels = snapshot->use_24_hour ? labels_24 : labels_12;
    for (int mark = 0; mark < 4; ++mark) {
        float px = x + width * mark / 4.0f;
        int label_width = text_width(ui, 0.42f, labels[mark]);
        float label_x = mark == 0 ? px : px - label_width * 0.5f;
        draw_text(ui, label_x, y, RGBA8(177, 184, 199, 232), 0.42f,
                  labels[mark]);
    }
}

static void day_metric_bounds(int metric, int imperial, float observed_low,
                              float observed_high, float *low, float *high)
{
    if (metric == DAY_METRIC_PRECIPITATION ||
        metric == DAY_METRIC_HUMIDITY) {
        *low = 0.0f;
        *high = 100.0f;
        return;
    }
    if (metric == DAY_METRIC_UV) {
        *low = 0.0f;
        *high = fmaxf(11.0f, ceilf(observed_high));
        return;
    }
    if (metric == DAY_METRIC_WIND) {
        *low = 0.0f;
        *high = fmaxf(10.0f, ceilf(observed_high / 5.0f) * 5.0f);
        return;
    }
    if (metric == DAY_METRIC_VISIBILITY) {
        *low = 0.0f;
        *high = fmaxf(5.0f, ceilf(observed_high / 5.0f) * 5.0f);
        return;
    }
    float step = metric == DAY_METRIC_PRESSURE
                     ? (imperial ? 0.5f : 5.0f)
                     : (imperial ? 6.0f : 3.0f);
    *low = floorf(observed_low / step) * step;
    *high = ceilf(observed_high / step) * step;
    float minimum = step * 2.0f;
    if (*high - *low < minimum) {
        *low -= step;
        *high += step;
    }
}

static void draw_day_temperature_graph(UiState *ui,
                                       const AppSnapshot *snapshot,
                                       const WeatherDay *day, float y)
{
    const WeatherData *weather = &snapshot->weather;
    int indices[24];
    int count = collect_day_hours(weather, day->date, indices, 24);
    rounded_rect(82, y, 796, 222, 12, RGBA8(108, 109, 115, 170));
    rounded_rect(83, y + 1, 794, 220, 11, RGBA8(35, 35, 37, 255));
    if (count < 2) {
        draw_text(ui, 310, y + 138, RGBA8(185, 195, 210, 235), 0.61f,
                  tr(snapshot->language, TEXT_HOURLY_UNAVAILABLE));
        return;
    }
    int metric = ui->day_detail_metric;
    int imperial = snapshot->use_fahrenheit;
    float observed_low = day_metric_value(
        weather, &weather->hours[indices[0]], metric,
        ui->day_detail_feels_like, imperial);
    float observed_high = observed_low;
    int low_at = 0;
    int high_at = 0;
    float gust_max = 0.0f;
    for (int i = 0; i < count; ++i)
        gust_max = fmaxf(gust_max, weather_display_wind(
            weather->hours[indices[i]].wind_gust_kmh, imperial));
    for (int i = 1; i < count; ++i) {
        float value = day_metric_value(weather, &weather->hours[indices[i]],
                                       metric, ui->day_detail_feels_like,
                                       imperial);
        if (value < observed_low) { observed_low = value; low_at = i; }
        if (value > observed_high) { observed_high = value; high_at = i; }
    }
    float low;
    float high;
    day_metric_bounds(metric, imperial, observed_low,
                      metric == DAY_METRIC_WIND ? fmaxf(observed_high, gust_max)
                                               : observed_high,
                      &low, &high);
    float span = high - low;
    const float plot_x = 106.0f;
    const float plot_w = 690.0f;
    const float plot_top = y + 58.0f;
    const float plot_bottom = y + 198.0f;
    if (metric == DAY_METRIC_CONDITIONS || metric == DAY_METRIC_WIND) {
        for (int icon = 0; icon < 6; ++icon) {
            int hour = icon * 4;
            int nearest = 0;
            int nearest_delta = 2000;
            for (int i = 0; i < count; ++i) {
                int delta = abs(weather_hour_minutes(
                    &weather->hours[indices[i]]) - hour * 60);
                if (delta < nearest_delta) { nearest = i; nearest_delta = delta; }
            }
            float ix = plot_x + plot_w * hour / 24.0f;
            unsigned int icon_alpha = g_draw_alpha;
            if (!strncmp(weather->updated, day->date, 10) &&
                hour * 60 < clock_minutes(weather->updated + 11))
                g_draw_alpha = icon_alpha * 45 / 100;
            if (metric == DAY_METRIC_CONDITIONS)
                draw_weather_icon(ix, y + 31.0f, 21,
                              weather->hours[indices[nearest]].weather_code,
                              forecast_hour_is_day(weather, indices[nearest]));
            else {
                float direction = weather->hours[indices[nearest]].wind_direction;
                float angle = (direction + 90.0f) * PI_F / 180.0f;
                float dx = cosf(angle), dy = sinf(angle);
                float iy = y + 31;
                uint32_t arrow = RGBA8(222, 226, 232, 255);
                line_width(ix - dx * 6, iy - dy * 6, ix + dx * 6, iy + dy * 6, 1, arrow);
                line_width(ix + dx * 6, iy + dy * 6,
                           ix + dx * 1 - dy * 4, iy + dy * 1 + dx * 4, 1, arrow);
                line_width(ix + dx * 6, iy + dy * 6,
                           ix + dx * 1 + dy * 4, iy + dy * 1 - dx * 4, 1, arrow);
            }
            g_draw_alpha = icon_alpha;
        }
    } else {
        char chart_label[64];
        const char *unit = metric == DAY_METRIC_WIND
                               ? weather_wind_unit(imperial)
                           : metric == DAY_METRIC_VISIBILITY
                               ? weather_visibility_unit(imperial)
                           : metric == DAY_METRIC_PRESSURE
                               ? weather_pressure_unit(imperial) : "";
        snprintf(chart_label, sizeof(chart_label), "%s%s%s",
                 day_metric_name(metric), unit[0] ? " · " : "", unit);
        draw_text(ui, plot_x, y + 37.0f, RGBA8(225, 231, 241, 245),
                  0.58f, chart_label);
    }
    draw_elapsed_hours(weather, day, plot_x, plot_top, plot_w, plot_bottom);
    int divisions = metric == DAY_METRIC_PRECIPITATION ||
                    metric == DAY_METRIC_HUMIDITY ? 5 : 4;
    for (int row = 0; row <= divisions; ++row) {
        float gy = plot_top + row * (plot_bottom - plot_top) / divisions;
        line(plot_x, gy, plot_x + plot_w, gy, RGBA8(229, 235, 244, 40));
        char axis[20];
        format_day_metric_axis(axis, sizeof(axis), metric,
                               high - span * row / divisions, imperial);
        draw_text(ui, 808, gy + 5, RGBA8(188, 194, 207, 235), 0.43f,
                  axis);
    }
    for (int mark = 0; mark <= 4; ++mark) {
        float gx = plot_x + mark * plot_w / 4.0f;
        line(gx, plot_top, gx, plot_bottom,
             RGBA8(229, 235, 244, mark == 0 || mark == 4 ? 45 : 27));
    }
    float px[24];
    float py[24];
    int point_minutes[24];
    for (int i = 0; i < count; ++i) {
        int minutes = weather_hour_minutes(&weather->hours[indices[i]]);
        if (minutes < 0) minutes = i * 1440 / count;
        point_minutes[i] = minutes;
        float value = day_metric_value(weather, &weather->hours[indices[i]],
                                       metric, ui->day_detail_feels_like,
                                       imperial);
        px[i] = plot_x + plot_w * minutes / 1440.0f;
        py[i] = plot_bottom - (value - low) / span *
                              (plot_bottom - plot_top);
    }
    int now_minutes = -1;
    if (!strncmp(weather->updated, day->date, 10)) {
        const char *now_clock = strchr(weather->updated, 'T');
        if (now_clock) now_minutes = clock_minutes(now_clock + 1);
    }
    /* Fill and curve use identical samples: no mismatched linear fill edge,
     * overlapping translucent strips, or hundreds of one-pixel draw calls. */
    for (int i = 1; i < count; ++i) {
        float segment_w = px[i] - px[i - 1];
        if (segment_w <= 0.0f) continue;
        float last_x = px[i - 1];
        float last_y = py[i - 1];
        for (int sub = 1; sub <= 4; ++sub) {
            float t = sub / 4.0f;
            float next_x = px[i - 1] * (1.0f - t) + px[i] * t;
            float smooth = t * t * (3.0f - 2.0f * t);
            float next_y = py[i - 1] * (1.0f - smooth) + py[i] * smooth;
            int segment_minutes = (int)(
                point_minutes[i - 1] * (1.0f - t) +
                point_minutes[i] * t);
            int elapsed_sub = now_minutes >= 0 &&
                              segment_minutes <= now_minutes;
            uint32_t fill_top = day_metric_color_alpha(metric, elapsed_sub ? 32 : 64);
            uint32_t fill_base = day_metric_color_alpha(metric, elapsed_sub ? 15 : 27);
            if (metric == DAY_METRIC_CONDITIONS)
                fill_base = RGBA8(78, 184, 196, elapsed_sub ? 40 : 105);
            chart_fill_segment(last_x, last_y, next_x, next_y, plot_bottom,
                                fill_top, fill_top, fill_base);
            if (!elapsed_sub || ((i * 4 + sub) & 1) == 0)
                line_width(last_x, last_y, next_x, next_y, 1.65f,
                           elapsed_sub
                                           ? day_metric_elapsed_color(metric,
                                                                      255)
                                           : day_metric_color(metric));
            last_x = next_x;
            last_y = next_y;
        }
    }
    if (px[count - 1] < plot_x + plot_w) {
        float tail_start = px[count - 1];
        float tail_end = plot_x + plot_w;
        int tail_elapsed = now_minutes >= 1440;
        chart_fill_segment(tail_start, py[count - 1], tail_end, py[count - 1],
                            plot_bottom, day_metric_color_alpha(metric, 64),
                            day_metric_color_alpha(metric, 64),
                            metric == DAY_METRIC_CONDITIONS ? RGBA8(78, 184, 196, 105)
                                                           : day_metric_color_alpha(metric, 27));
        for (int sub = 1; sub <= 4; ++sub) {
            float x0 = tail_start + (tail_end - tail_start) *
                                      (sub - 1) / 4.0f;
            float x1 = tail_start + (tail_end - tail_start) * sub / 4.0f;
            if (!tail_elapsed || (sub & 1))
                line_width(x0, py[count - 1], x1, py[count - 1], 1.65f,
                           tail_elapsed
                               ? day_metric_elapsed_color(metric, 255)
                               : day_metric_color(metric));
        }
    }
    if (metric == DAY_METRIC_WIND) {
        for (int i = 1; i < count; ++i) {
            float previous_gust = weather_display_wind(weather->hours[indices[i - 1]].wind_gust_kmh, imperial);
            float next_gust = weather_display_wind(weather->hours[indices[i]].wind_gust_kmh, imperial);
            float last_x = px[i - 1];
            float last_y = plot_bottom - (previous_gust - low) / span * (plot_bottom - plot_top);
            for (int sub = 1; sub <= 4; ++sub) {
                float t = sub / 4.0f, smooth = t * t * (3.0f - 2.0f * t);
                float next_x = px[i - 1] + (px[i] - px[i - 1]) * t;
                float gust = previous_gust + (next_gust - previous_gust) * smooth;
                float next_y = plot_bottom - (gust - low) / span * (plot_bottom - plot_top);
                int past = now_minutes >= 0 &&
                    point_minutes[i - 1] + (point_minutes[i] - point_minutes[i - 1]) * t <= now_minutes;
                if (!past || (sub & 1))
                    line_width(last_x, last_y, next_x, next_y, 1.2f,
                               past ? RGBA8(37, 112, 98, 255) : RGBA8(57, 177, 151, 255));
                last_x = next_x; last_y = next_y;
            }
        }
    }
    if (now_minutes >= point_minutes[0] && now_minutes <= 1440) {
        float now_x = plot_x + plot_w * now_minutes / 1440.0f;
        float now_y = py[count - 1];
        for (int i = 1; i < count; ++i) {
            if (now_minutes > point_minutes[i]) continue;
            float t = (now_minutes - point_minutes[i - 1]) /
                (float)(point_minutes[i] - point_minutes[i - 1]);
            t = t * t * (3 - 2 * t);
            now_y = py[i - 1] + (py[i] - py[i - 1]) * t;
            break;
        }
        line(now_x, plot_top, now_x, plot_bottom, RGBA8(235, 238, 242, 65));
        circle(now_x, now_y, 6, RGBA8(35, 35, 37, 255));
        circle(now_x, now_y, 2.7f, RGBA8(255, 255, 255, 255));
    }
    for (int marker = 0; marker < 2; ++marker) {
        int at = marker ? high_at : low_at;
        int marker_elapsed = now_minutes >= 0 &&
                             point_minutes[at] <= now_minutes;
        circle(px[at], py[at], 6.5f,
               marker_elapsed ? RGBA8(68, 71, 78, 255)
                              : RGBA8(42, 46, 54, 255));
        circle(px[at], py[at], 2.8f,
               marker_elapsed ? day_metric_elapsed_color(metric, 255)
                              : RGBA8(255, 255, 255, 255));
        if (metric == DAY_METRIC_CONDITIONS)
            draw_text(ui, px[at] - 4.0f, py[at] - 13.0f,
                      RGBA8(218, 222, 230, 245), 0.43f,
                      marker ? "H" : "L");
    }
    draw_day_time_axis(ui, snapshot, plot_x, plot_w, y + 219.0f);
    rounded_rect(101, y + 230, 738, 34, 17, RGBA8(79, 82, 92, 235));
    if (metric == DAY_METRIC_CONDITIONS) {
        float half = 369.0f;
        rounded_rect(101 + ui->day_detail_feels_like * half, y + 231,
                     half, 32, 16, RGBA8(148, 151, 161, 245));
        const char *actual = "Actual";
        const char *feels = tr(snapshot->language, TEXT_FEELS_LIKE);
        draw_text(ui, 101 + half * 0.5f -
                  text_width(ui, 0.55f, actual) * 0.5f,
                  y + 254, RGBA8(255, 255, 255, 255), 0.55f, actual);
        draw_text(ui, 101 + half * 1.5f -
                  text_width(ui, 0.55f, feels) * 0.5f,
                  y + 254, RGBA8(255, 255, 255, 255), 0.55f, feels);
    } else {
        float average = 0.0f;
        for (int i = 0; i < count; ++i)
            average += day_metric_value(weather, &weather->hours[indices[i]],
                                        metric, 0, imperial);
        average /= count;
        char low_text[20], high_text[20], summary[96];
        format_day_metric_axis(low_text, sizeof(low_text), metric,
                               observed_low, imperial);
        format_day_metric_axis(high_text, sizeof(high_text), metric,
                               observed_high, imperial);
        if (metric == DAY_METRIC_WIND)
            snprintf(summary, sizeof(summary), "Wind %s–%s %s · Gusts up to %.0f %s",
                     low_text, high_text, weather_wind_unit(imperial),
                     gust_max, weather_wind_unit(imperial));
        else
            snprintf(summary, sizeof(summary), "Range %s – %s    Average %.1f",
                     low_text, high_text, average);
        int width = text_width(ui, 0.52f, summary);
        draw_text(ui, 101 + (738 - width) * 0.5f, y + 253,
                  RGBA8(244, 247, 252, 250), 0.52f, summary);
    }
}

static void draw_day_precipitation(UiState *ui,
                                   const AppSnapshot *snapshot,
                                   const WeatherDay *day, float y)
{
    const WeatherData *weather = &snapshot->weather;
    int indices[24];
    int count = collect_day_hours(weather, day->date, indices, 24);
    draw_text(ui, 82, y + 29, RGBA8(250, 252, 255, 255), 0.84f,
              "Chance of Precipitation");
    draw_textf(ui, 82, y + 53, RGBA8(180, 186, 199, 235), 0.52f,
               "Daily chance: %d%%", day->precipitation_percent);
    rounded_rect(82, y + 65, 796, 168, 12, RGBA8(108, 109, 115, 170));
    rounded_rect(83, y + 66, 794, 166, 11, RGBA8(35, 35, 37, 255));
    if (count < 2) return;
    const float plot_x = 106.0f;
    const float plot_w = 690.0f;
    const float plot_top = y + 82.0f;
    const float plot_bottom = y + 196.0f;
    draw_elapsed_hours(weather, day, plot_x, plot_top, plot_w, plot_bottom);
    for (int row = 0; row <= 5; ++row) {
        float gy = plot_top + row * (plot_bottom - plot_top) / 5.0f;
        line(plot_x, gy, plot_x + plot_w, gy, RGBA8(229, 235, 244, 40));
        draw_textf(ui, 809, gy + 5, RGBA8(185, 192, 205, 235), 0.40f,
                   "%d%%", 100 - row * 20);
    }
    int first_minutes = weather_hour_minutes(&weather->hours[indices[0]]);
    if (first_minutes < 0) first_minutes = 0;
    int now_minutes = -1;
    if (!strncmp(weather->updated, day->date, 10)) {
        const char *now_clock = strchr(weather->updated, 'T');
        if (now_clock) now_minutes = clock_minutes(now_clock + 1);
    }
    float last_x = plot_x + plot_w * first_minutes / 1440.0f;
    float last_y = plot_bottom - weather->hours[indices[0]].precipitation_percent *
                                 (plot_bottom - plot_top) / 100.0f;
    for (int i = 1; i < count; ++i) {
        int minutes = weather_hour_minutes(&weather->hours[indices[i]]);
        if (minutes < 0) minutes = i * 1440 / count;
        float px = plot_x + plot_w * minutes / 1440.0f;
        float py = plot_bottom - weather->hours[indices[i]].precipitation_percent *
                                 (plot_bottom - plot_top) / 100.0f;
        int elapsed_segment = now_minutes >= 0 && minutes <= now_minutes;
        uint32_t fill = elapsed_segment
                 ? day_metric_elapsed_color(DAY_METRIC_PRECIPITATION, 22)
                 : day_metric_color_alpha(DAY_METRIC_PRECIPITATION, 27);
        chart_fill_segment(last_x, last_y, px, py, plot_bottom, fill, fill, fill);
        if (elapsed_segment) {
            for (int sub = 0; sub < 6; sub += 2) {
                float t0 = sub / 6.0f;
                float t1 = (sub + 1) / 6.0f;
                line_width(last_x + (px - last_x) * t0,
                           last_y + (py - last_y) * t0,
                           last_x + (px - last_x) * t1,
                           last_y + (py - last_y) * t1, 1.35f,
                           day_metric_elapsed_color(
                               DAY_METRIC_PRECIPITATION, 255));
            }
        } else {
            line_width(last_x, last_y, px, py, 1.35f,
                       day_metric_color(DAY_METRIC_PRECIPITATION));
        }
        last_x = px;
        last_y = py;
    }
    draw_day_time_axis(ui, snapshot, plot_x, plot_w, y + 221.0f);
}

static void draw_day_comparison(UiState *ui,
                                const AppSnapshot *snapshot,
                                const WeatherDay *day, int index, float y)
{
    const WeatherData *weather = &snapshot->weather;
    int other_index = index > 0 ? index - 1 : (weather->day_count > 1 ? 1 : 0);
    const WeatherDay *other = &weather->days[other_index];
    int metric = ui->day_detail_metric;
    float row_low[2] = {0.0f, 0.0f};
    float row_high[2] = {0.0f, 0.0f};
    float row_average[2] = {0.0f, 0.0f};
    const WeatherDay *rows[2] = {day, other};
    for (int row = 0; row < 2; ++row) {
        if (metric == DAY_METRIC_CONDITIONS) {
            row_low[row] = weather_display_temperature(
                rows[row]->low_c, snapshot->use_fahrenheit);
            row_high[row] = weather_display_temperature(
                rows[row]->high_c, snapshot->use_fahrenheit);
            row_average[row] = (row_low[row] + row_high[row]) * 0.5f;
            continue;
        }
        int indices[24];
        int count = collect_day_hours(weather, rows[row]->date, indices, 24);
        if (count < 1) continue;
        row_low[row] = day_metric_value(
            weather, &weather->hours[indices[0]], metric, 0,
            snapshot->use_fahrenheit);
        row_high[row] = row_low[row];
        for (int hour = 0; hour < count; ++hour) {
            float value = day_metric_value(
                weather, &weather->hours[indices[hour]], metric, 0,
                snapshot->use_fahrenheit);
            row_average[row] += value;
            if (value < row_low[row]) row_low[row] = value;
            if (value > row_high[row]) row_high[row] = value;
        }
        row_average[row] /= count;
    }
    float scale_low = fminf(row_low[0], row_low[1]);
    float scale_high = fmaxf(row_high[0], row_high[1]);
    if (metric != DAY_METRIC_CONDITIONS)
        day_metric_bounds(metric, snapshot->use_fahrenheit,
                          scale_low, scale_high, &scale_low, &scale_high);
    float span = scale_high - scale_low;
    if (span < 0.01f) span = 1.0f;
    float delta = row_average[0] - row_average[1];
    float similar = metric == DAY_METRIC_PRESSURE
                        ? (snapshot->use_fahrenheit ? 0.05f : 1.5f)
                    : metric == DAY_METRIC_UV ? 0.5f : 2.0f;
    char comparison[128];
    snprintf(comparison, sizeof(comparison),
             fabsf(delta) < similar
                 ? "The selected day's average %s is similar."
                 : delta > 0.0f
                       ? "The selected day's average %s is higher."
                       : "The selected day's average %s is lower.",
             metric == DAY_METRIC_CONDITIONS ? "temperature"
                                              : day_metric_name(metric));
    draw_text(ui, 82, y + 28, RGBA8(250, 252, 255, 255), 0.82f,
              "Daily Comparison");
    surface(82, y + 42, 796, 138, 22, RGBA8(57, 57, 61, 255));
    draw_text(ui, 108, y + 70, RGBA8(246, 248, 252, 248), 0.52f,
              comparison);
    const char *names[2] = {
        index == 0 ? tr(snapshot->language, TEXT_TODAY)
                   : tr_day_label(snapshot->language, day->label),
        other_index == 0 ? tr(snapshot->language, TEXT_TODAY)
                         : tr_day_label(snapshot->language, other->label)
    };
    for (int row = 0; row < 2; ++row) {
        float ry = y + 102 + row * 37.0f;
        draw_text(ui, 108, ry + 12, RGBA8(247, 249, 253, 255), 0.58f,
                  names[row]);
        char low_text[20];
        char high_text[20];
        format_day_metric_axis(low_text, sizeof(low_text), metric,
                               row_low[row], snapshot->use_fahrenheit);
        format_day_metric_axis(high_text, sizeof(high_text), metric,
                               row_high[row], snapshot->use_fahrenheit);
        draw_text(ui, 278, ry + 12, RGBA8(188, 194, 207, 245), 0.52f,
                  low_text);
        float start = (row_low[row] - scale_low) / span;
        float end = (row_high[row] - scale_low) / span;
        rounded_rect(350, ry + 3, 350, 7, 3.5f, RGBA8(28, 31, 39, 180));
        rounded_rect(350 + start * 350.0f, ry + 3,
                     fmaxf(8.0f, (end - start) * 350.0f), 7, 3.5f,
                     day_metric_color_alpha(metric, row ? 178 : 245));
        if (row == 0 && index == 0 &&
            metric == DAY_METRIC_CONDITIONS) {
            float now = weather_display_temperature(
                weather->temperature_c, snapshot->use_fahrenheit);
            if (now < scale_low) now = scale_low;
            if (now > scale_high) now = scale_high;
            float dot_x = 350 + (now - scale_low) / span * 350.0f;
            circle(dot_x, ry + 6.5f, 4.1f, RGBA8(255, 255, 255, 255));
        }
        draw_text(ui, 746, ry + 12, RGBA8(250, 251, 255, 255), 0.52f,
                  high_text);
    }
}

static void day_metric_stats(const WeatherData *weather,
                             const WeatherDay *day, int metric,
                             int imperial, float *minimum, float *maximum,
                             float *average)
{
    int indices[24];
    int count = collect_day_hours(weather, day->date, indices, 24);
    if (count < 1) {
        *minimum = *maximum = *average = 0.0f;
        return;
    }
    *minimum = *maximum = day_metric_value(
        weather, &weather->hours[indices[0]], metric, 0, imperial);
    *average = 0.0f;
    for (int i = 0; i < count; ++i) {
        float value = day_metric_value(weather, &weather->hours[indices[i]],
                                       metric, 0, imperial);
        if (value < *minimum) *minimum = value;
        if (value > *maximum) *maximum = value;
        *average += value;
    }
    *average /= count;
}

static int wrapped_line_count(UiState *ui, const char *text,
                              float max_width, float scale, int max_lines)
{
    char remaining[384];
    snprintf(remaining, sizeof(remaining), "%s", text ? text : "");
    char *cursor = remaining;
    int lines = 0;
    while (*cursor && lines < max_lines) {
        while (*cursor == ' ') ++cursor;
        if (!*cursor) break;
        char *line_start = cursor;
        char *last_space = NULL;
        char *scan = cursor;
        while (*scan) {
            if (*scan == ' ') last_space = scan;
            char saved = *scan;
            *scan = '\0';
            int width = text_width(ui, scale, line_start);
            *scan = saved;
            if (width > (int)max_width) break;
            ++scan;
        }
        char *line_end = !*scan ? scan :
                         last_space && last_space >= line_start
                             ? last_space : scan;
        cursor = line_end;
        while (*cursor == ' ') ++cursor;
        ++lines;
    }
    return lines > 0 ? lines : 1;
}

static float draw_day_text_card(UiState *ui, const char *title,
                                const char *text, float y)
{
    const float line_height = 25.0f;
    int lines = wrapped_line_count(ui, text, 730, 0.59f, 5);
    float card_height = 36.0f + lines * line_height;
    draw_text(ui, 82, y + 28, RGBA8(250, 252, 255, 255), 0.82f,
              title);
    surface(82, y + 42, 796, card_height, 22,
            RGBA8(57, 57, 61, 255));
    draw_wrapped_text(ui, 108, y + 76, 730, line_height,
                      RGBA8(242, 245, 250, 248), 0.59f, text, 5);
    return y + 42 + card_height;
}

static void day_metric_summary(char *out, size_t out_size,
                               const AppSnapshot *snapshot,
                               const WeatherDay *day, int metric)
{
    const WeatherData *weather = &snapshot->weather;
    float minimum;
    float maximum;
    float average;
    day_metric_stats(weather, day, metric, snapshot->use_fahrenheit,
                     &minimum, &maximum, &average);
    char low[20];
    char high[20];
    char mean[20];
    format_day_metric_axis(low, sizeof(low), metric, minimum,
                           snapshot->use_fahrenheit);
    format_day_metric_axis(high, sizeof(high), metric, maximum,
                           snapshot->use_fahrenheit);
    format_day_metric_axis(mean, sizeof(mean), metric, average,
                           snapshot->use_fahrenheit);
    switch (metric) {
    case DAY_METRIC_UV:
        snprintf(out, out_size,
                 "The UV index ranges from %s to %s, averaging %s. Seek shade and use sun protection when levels are high.",
                 low, high, mean);
        break;
    case DAY_METRIC_WIND:
        snprintf(out, out_size,
                 "Wind averages %s %s, ranges from %s to %s, and changes direction through the day. Gusts may be stronger.",
                 mean, weather_wind_unit(snapshot->use_fahrenheit), low, high);
        break;
    case DAY_METRIC_PRECIPITATION:
        snprintf(out, out_size,
                 "The daily chance is %d%% with %.2f %s expected. The hourly probability ranges from %s to %s.",
                 day->precipitation_percent,
                 snapshot->use_fahrenheit ? day->precipitation_mm / 25.4f
                                          : day->precipitation_mm,
                 snapshot->use_fahrenheit ? "in" : "mm", low, high);
        break;
    case DAY_METRIC_HUMIDITY:
        snprintf(out, out_size,
                 "Relative humidity averages %s and ranges from %s to %s. Higher humidity can make warm air feel warmer.",
                 mean, low, high);
        break;
    case DAY_METRIC_VISIBILITY:
        snprintf(out, out_size,
                 "Visibility ranges from %s to %s %s, averaging %s. Lower values indicate haze, fog, or precipitation.",
                 low, high, weather_visibility_unit(snapshot->use_fahrenheit),
                 mean);
        break;
    case DAY_METRIC_PRESSURE:
        snprintf(out, out_size,
                 "Pressure averages %s %s and ranges from %s to %s. Rapid changes can signal an approaching weather system.",
                 mean, weather_pressure_unit(snapshot->use_fahrenheit), low,
                 high);
        break;
    default:
        snprintf(out, out_size,
                 "%s brings %s with temperatures from %.0f° to %.0f°. The daily precipitation chance is %d%%.",
                 day->label, tr_condition(snapshot->language,
                                          day->weather_code),
                 weather_display_temperature(day->low_c,
                                             snapshot->use_fahrenheit),
                 weather_display_temperature(day->high_c,
                                             snapshot->use_fahrenheit),
                 day->precipitation_percent);
        break;
    }
}

static const char *day_metric_about(int metric)
{
    static const char *const about[DAY_METRIC_COUNT] = {
        "Feels Like describes how warm or cold it feels compared with the measured temperature. Humidity, sunlight, and wind can change it.",
        "The UV index measures ultraviolet radiation. Values of 3 or higher call for protection; 11 or higher is extreme.",
        "Wind speed is a short-period average. Gusts are brief bursts above that average, while direction shows where the wind comes from.",
        "Precipitation probability is the chance of measurable precipitation at this location during an hour. It always uses a 0 to 100 percent scale.",
        "Relative humidity describes how much moisture the air contains compared with its capacity at the same temperature. Dew point describes the temperature at which moisture condenses.",
        "Visibility estimates how far clearly defined objects can be seen. Fog, haze, smoke, rain, and snow can reduce it.",
        "Atmospheric pressure is the force exerted by the air. Falling pressure often accompanies unsettled weather; rising pressure often brings improvement."
    };
    return about[metric >= 0 && metric < DAY_METRIC_COUNT ? metric : 0];
}

static float draw_day_precipitation_totals(UiState *ui,
                                           const AppSnapshot *snapshot,
                                           const WeatherDay *day,
                                           float hourly_total, float y)
{
    float selected_total = snapshot->use_fahrenheit
                               ? day->precipitation_mm / 25.4f
                               : day->precipitation_mm;
    float summed_total = snapshot->use_fahrenheit
                             ? hourly_total / 25.4f : hourly_total;
    const char *unit = snapshot->use_fahrenheit ? "in" : "mm";
    draw_text(ui, 82, y + 28, RGBA8(250, 252, 255, 255), 0.82f,
              "Precipitation Totals");
    surface(82, y + 42, 796, 116, 22, RGBA8(57, 57, 61, 255));
    draw_text(ui, 108, y + 68, RGBA8(180, 186, 200, 240), 0.45f,
              "SELECTED DAY");
    draw_text(ui, 108, y + 94, RGBA8(250, 252, 255, 255), 0.61f,
              tr(snapshot->language, TEXT_PRECIPITATION));
    char total[28];
    snprintf(total, sizeof(total), snapshot->use_fahrenheit
                                       ? "%.2f %s" : "%.1f %s",
             selected_total, unit);
    draw_text(ui, 849 - text_width(ui, 0.61f, total), y + 94,
              RGBA8(250, 252, 255, 255), 0.61f, total);
    rect(108, y + 108, 744, 1, RGBA8(228, 233, 242, 28));
    draw_text(ui, 108, y + 133, RGBA8(180, 186, 200, 240), 0.45f,
              "SUM OF HOURLY VALUES");
    snprintf(total, sizeof(total), snapshot->use_fahrenheit
                                       ? "%.2f %s" : "%.1f %s",
             summed_total, unit);
    draw_text(ui, 849 - text_width(ui, 0.54f, total), y + 134,
              RGBA8(250, 252, 255, 255), 0.54f, total);
    return y + 158.0f;
}

static void draw_day_detail(UiState *ui, const AppSnapshot *snapshot)
{
    const WeatherData *weather = &snapshot->weather;
    if (weather->day_count < 1) return;
    int index = ui->daily_selected;
    if (index < 0) index = 0;
    if (index >= weather->day_count) index = weather->day_count - 1;
    const WeatherDay *day = &weather->days[index];
    int metric = ui->day_detail_metric;
    draw_modal_backdrop();
    surface(54, 10, 852, 476, 30, RGBA8(35, 35, 37, 255));

    uint64_t now_us = sceKernelGetProcessTimeWide();
    if (!ui->day_detail_scroll_update_us)
        ui->day_detail_scroll_update_us = now_us;
    float dt = (float)(now_us - ui->day_detail_scroll_update_us) / 1000000.0f;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    ui->day_detail_scroll_update_us = now_us;
    if (snapshot->motion == APP_MOTION_OFF)
        ui->day_detail_scroll = ui->day_detail_scroll_target;
    else {
        float blend = 1.0f - expf(-14.0f * dt);
        ui->day_detail_scroll +=
            (ui->day_detail_scroll_target - ui->day_detail_scroll) * blend;
        if (fabsf(ui->day_detail_scroll_target - ui->day_detail_scroll) < 0.1f)
            ui->day_detail_scroll = ui->day_detail_scroll_target;
    }
    if (ui->day_detail_max_scroll > 0.0f &&
        ui->day_detail_scroll_target > ui->day_detail_max_scroll)
        ui->day_detail_scroll_target = ui->day_detail_max_scroll;
    float content_y = 104.0f - ui->day_detail_scroll;
    float display_high = weather_display_temperature(day->high_c,
                                                     snapshot->use_fahrenheit);
    float display_low = weather_display_temperature(day->low_c,
                                                    snapshot->use_fahrenheit);
    float hero_temperature = index == 0 ? weather->temperature_c : day->high_c;
    hero_temperature = weather_display_temperature(hero_temperature,
                                                   snapshot->use_fahrenheit);
    int indices[24];
    int hour_count = collect_day_hours(weather, day->date, indices, 24);
    float hourly_total = 0.0f;
    for (int i = 0; i < hour_count; ++i)
        hourly_total += weather->hours[indices[i]].precipitation_mm;

    float minimum;
    float maximum;
    float average;
    day_metric_stats(weather, day, metric, snapshot->use_fahrenheit,
                     &minimum, &maximum, &average);
    char hero_value[48];
    char hero_detail[128];
    switch (metric) {
    case DAY_METRIC_UV:
        snprintf(hero_value, sizeof(hero_value), "%.0f", maximum);
        snprintf(hero_detail, sizeof(hero_detail), "%s · Daily maximum",
                 maximum < 3.0f ? "Low" : maximum < 6.0f ? "Moderate" :
                 maximum < 8.0f ? "High" : maximum < 11.0f ? "Very high" :
                                                        "Extreme");
        break;
    case DAY_METRIC_WIND:
        snprintf(hero_value, sizeof(hero_value), "%.0f %s", average,
                 weather_wind_unit(snapshot->use_fahrenheit));
        snprintf(hero_detail, sizeof(hero_detail), "Average · %.0f–%.0f %s",
                 minimum, maximum,
                 weather_wind_unit(snapshot->use_fahrenheit));
        break;
    case DAY_METRIC_PRECIPITATION:
        snprintf(hero_value, sizeof(hero_value), "%d%%",
                 day->precipitation_percent);
        snprintf(hero_detail, sizeof(hero_detail), "Daily chance · %.2f %s",
                 snapshot->use_fahrenheit ? day->precipitation_mm / 25.4f
                                          : day->precipitation_mm,
                 snapshot->use_fahrenheit ? "in" : "mm");
        break;
    case DAY_METRIC_HUMIDITY:
        snprintf(hero_value, sizeof(hero_value), "%.0f%%", average);
        snprintf(hero_detail, sizeof(hero_detail), "Average · %.0f–%.0f%%",
                 minimum, maximum);
        break;
    case DAY_METRIC_VISIBILITY:
        snprintf(hero_value, sizeof(hero_value), "%.1f %s", average,
                 weather_visibility_unit(snapshot->use_fahrenheit));
        snprintf(hero_detail, sizeof(hero_detail), "Average · %.1f–%.1f %s",
                 minimum, maximum,
                 weather_visibility_unit(snapshot->use_fahrenheit));
        break;
    case DAY_METRIC_PRESSURE:
        snprintf(hero_value, sizeof(hero_value),
                 snapshot->use_fahrenheit ? "%.2f %s" : "%.0f %s",
                 average, weather_pressure_unit(snapshot->use_fahrenheit));
        snprintf(hero_detail, sizeof(hero_detail), "Daily average pressure");
        break;
    default:
        snprintf(hero_value, sizeof(hero_value), "%.0f°", hero_temperature);
        snprintf(hero_detail, sizeof(hero_detail), "%s · H:%.0f°  L:%.0f°",
                 tr_condition(snapshot->language, day->weather_code),
                 display_high, display_low);
        break;
    }

    vita2d_set_clip_rectangle(74, 92, 886, 472);
    vita2d_enable_clipping();
    draw_text(ui, 105, content_y + 52, RGBA8(250, 252, 255, 255),
              metric == DAY_METRIC_CONDITIONS ? 1.42f : 1.02f,
              hero_value);
    float hero_detail_x = metric == DAY_METRIC_CONDITIONS ? 324.0f : 448.0f;
    if (metric == DAY_METRIC_CONDITIONS)
        draw_weather_icon(274, content_y + 39, 52, day->weather_code, 1);
    draw_fitted_text(ui, hero_detail_x, content_y + 47, 854 - hero_detail_x,
              RGBA8(205, 212, 224, 243), 0.57f, hero_detail);

    draw_day_temperature_graph(ui, snapshot, day, content_y + 90);
    draw_fitted_text(ui, 101, content_y + 385, 754, RGBA8(181, 188, 202, 235), 0.49f,
              day_metric_description(ui->day_detail_metric));
    float cursor = content_y + 408.0f;
    if (metric == DAY_METRIC_CONDITIONS) {
        draw_day_precipitation(ui, snapshot, day, cursor);
        cursor += 257.0f;
        cursor = draw_day_precipitation_totals(
            ui, snapshot, day, hourly_total, cursor);
        cursor += 25.0f;
    } else if (metric == DAY_METRIC_PRECIPITATION) {
        cursor = draw_day_precipitation_totals(
            ui, snapshot, day, hourly_total, cursor);
        cursor += 25.0f;
    }
    char summary[384];
    day_metric_summary(summary, sizeof(summary), snapshot, day, metric);
    cursor = draw_day_text_card(ui,
                                metric == DAY_METRIC_CONDITIONS
                                    ? "Forecast" : "Daily Summary",
                                summary, cursor);
    cursor += 26.0f;
    draw_day_comparison(ui, snapshot, day, index, cursor);
    cursor += 206.0f;
    char about_title[72];
    snprintf(about_title, sizeof(about_title), "About %s",
             metric == DAY_METRIC_CONDITIONS
                 ? "Feels Like Temperature"
                 : day_metric_name(metric));
    cursor = draw_day_text_card(ui, about_title,
                                day_metric_about(metric), cursor);
    float content_height = cursor + 14.0f - content_y;
    float max_scroll = fmaxf(0.0f, content_height - 370.0f);
    ui->day_detail_max_scroll = max_scroll;
    if (ui->day_detail_scroll_target > max_scroll)
        ui->day_detail_scroll_target = max_scroll;
    if (ui->day_detail_scroll > max_scroll)
        ui->day_detail_scroll = max_scroll;
    vita2d_disable_clipping();

    /* Fixed dark header and footer masks keep custom GXM primitives inside
     * the scroll viewport on both vita2d and physical Vita drivers. */
    rounded_rect(54, 10, 852, 82, 30, RGBA8(35, 35, 37, 255));
    rect(54, 50, 852, 42, RGBA8(35, 35, 37, 255));
    const char *metric_name = day_metric_name(ui->day_detail_metric);
    int metric_width = text_width(ui, 0.96f, metric_name);
    /* Center the complete indicator + title group, rather than centering the
     * title and hanging the condition icon off its left edge. */
    const float indicator_span = 42.0f;
    float metric_left =
        (SCREEN_W - (metric_width + indicator_span)) * 0.5f + indicator_span;
    if (ui->day_detail_metric == DAY_METRIC_CONDITIONS) {
        draw_weather_icon(metric_left - 25, 51, 34, day->weather_code, 1);
    } else {
        draw_metric_icon(metric_left - 25, 50, ui->day_detail_metric,
                          day_metric_color(ui->day_detail_metric));
    }
    draw_text(ui, metric_left, 63, RGBA8(250, 252, 255, 255), 0.96f,
              metric_name);
    const char *day_name = index == 0 ? tr(snapshot->language, TEXT_TODAY)
                                      : tr_day_label(snapshot->language, day->label);
    char date_line[48];
    snprintf(date_line, sizeof(date_line), "%s · %s", day_name, day->date);
    int date_width = text_width(ui, 0.49f, date_line);
    draw_text(ui, (SCREEN_W - date_width) * 0.5f, 82,
              RGBA8(179, 187, 202, 235), 0.49f, date_line);
    if (max_scroll > 0.0f) {
        rounded_rect(887, 102, 4, 360, 2, RGBA8(176, 184, 199, 38));
        float thumb_h = fmaxf(72.0f, 360.0f * 370.0f / content_height);
        float thumb_y = 102.0f + (360.0f - thumb_h) *
                        ui->day_detail_scroll / max_scroll;
        rounded_rect(886, thumb_y, 6, thumb_h, 3,
                     RGBA8(202, 218, 238, 175));
    }
    draw_modal_footer(ui, snapshot);
}

static void draw_day_metric_selector(UiState *ui,
                                     const AppSnapshot *snapshot)
{
    rect(0, 0, SCREEN_W, SCREEN_H, RGBA8(4, 9, 18, 132));
    surface(246, 39, 468, 441, 30, RGBA8(17, 19, 24, 253));
    draw_text(ui, 286, 82, RGBA8(250, 252, 255, 255), 0.92f,
              "Forecast metric");
    draw_text(ui, 286, 104, RGBA8(170, 178, 192, 235), 0.48f,
              "Hourly detail for the selected day");
    for (int row = 0; row < DAY_METRIC_COUNT; ++row) {
        float y = 117.0f + row * 47.0f;
        int selected = row == ui->day_metric_selected;
        if (selected)
            draw_focus(267, y, 426, 40);
        else if (row)
            rect(289, y, 382, 1, RGBA8(220, 226, 236, 24));
        draw_metric_icon(292, y + 20, row, RGBA8(210, 222, 238, 255));
        draw_text(ui, 314, y + 26,
                  RGBA8(244, 247, 252, 250),
                  0.66f, day_metric_name(row));
        if (ui->day_detail_metric == row)
            draw_check(654, y + 20, RGBA8(145, 218, 255, 255));
    }
    draw_modal_footer(ui, snapshot);
}

int ui_init(UiState *ui)
{
    memset(ui, 0, sizeof(*ui));
    for (int i = 0; i < (int)(sizeof(g_font_sizes) / sizeof(g_font_sizes[0])); ++i) {
        ui->fonts[i] = vita2d_load_font_file("app0:font.ttf");
        if (!ui->fonts[i]) {
            ui_shutdown(ui);
            return -1;
        }
    }
    ui->sun_glow = vita2d_load_PNG_file("app0:sun-glow.png");
    if (ui->sun_glow)
        vita2d_texture_set_filters(ui->sun_glow, SCE_GXM_TEXTURE_FILTER_LINEAR,
                                  SCE_GXM_TEXTURE_FILTER_LINEAR);
    /* The actual condition is not known until app_services_init loads the
     * saved forecast. Avoid decoding a redundant cloudy image first. */
    ui->weather_bg_key = -1;
    ui->weather_pending_bg_key = -1;
    ui->map_loaded_zoom = -1;
    ui->map_loaded_provider = -1;
    ui->map_pending_zoom = -1;
    ui->map_pending_provider = -1;
    ui->map_retired_zoom = -1;
    ui->map_retired_provider = -1;
    ui->map_field_layer = -1;
    ui->map_field_hour = -1;
    memset(ui->map_pending_reuse, -1, sizeof(ui->map_pending_reuse));
    memset(ui->map_pending_attempted, 0,
           sizeof(ui->map_pending_attempted));
    return 0;
}

void ui_shutdown(UiState *ui)
{
    free_map_tiles(ui);
    if (ui->map_field_texture) {
        vita2d_free_texture(ui->map_field_texture);
        ui->map_field_texture = NULL;
    }
    cancel_weather_background_load(ui);
    if (ui->weather_bg) {
        vita2d_free_texture(ui->weather_bg);
        ui->weather_bg = NULL;
    }
    if (ui->weather_retired_bg) {
        vita2d_free_texture(ui->weather_retired_bg);
        ui->weather_retired_bg = NULL;
    }
    if (ui->sun_glow) {
        vita2d_free_texture(ui->sun_glow);
        ui->sun_glow = NULL;
    }
    for (int i = 0; i < (int)(sizeof(g_font_sizes) / sizeof(g_font_sizes[0])); ++i) {
        if (ui->fonts[i]) {
            vita2d_free_font(ui->fonts[i]);
            ui->fonts[i] = NULL;
        }
    }
}

void ui_set_view(UiState *ui, int view, uint64_t now_us)
{
    if (view < 0) view = VIEW_COUNT - 1;
    if (view >= VIEW_COUNT) view = 0;
    if (view == ui->view) return;
    int previous = ui->view;
    ui->previous_view = ui->view;
    ui->transition_direction = view > ui->view ? 1 : -1;
    if (ui->view == VIEW_DETAILS && view == VIEW_NOW)
        ui->transition_direction = 1;
    else if (ui->view == VIEW_NOW && view == VIEW_DETAILS)
        ui->transition_direction = -1;
    ui->view = view;
    ui->transition_start_us = now_us;
    app_log("UI view changed: from=%d to=%d", previous, view);
}

void ui_map_focus(UiState *ui, const WeatherData *weather)
{
    if (!weather) return;
    if (ui->map_initialized &&
        fabs(ui->map_location_lat - weather->latitude) < 0.0001 &&
        fabs(ui->map_location_lon - weather->longitude) < 0.0001)
        return;
    ui->map_zoom = 8;
    ui->map_camera_x = map_tile_x(weather->longitude, ui->map_zoom);
    ui->map_camera_y = map_tile_y(weather->latitude, ui->map_zoom);
    ui->map_present_x = ui->map_camera_x;
    ui->map_present_y = ui->map_camera_y;
    ui->map_present_zoom = ui->map_zoom;
    ui->map_present_initialized = 1;
    ui->map_present_peak_dx = 0.0f;
    ui->map_present_peak_dy = 0.0f;
    ui->map_center_x = (int)floor(ui->map_camera_x);
    ui->map_center_y = (int)floor(ui->map_camera_y);
    ui->map_visual_zoom = (float)ui->map_zoom;
    ui->map_hour = 0;
    ui->map_visual_hour = 0.0f;
    ui->map_visual_update_us = 0;
    ui->map_location_lat = weather->latitude;
    ui->map_location_lon = weather->longitude;
    ui->map_initialized = 1;
    ui->map_request_pending = 1;
}

void ui_map_constrain_provider(UiState *ui, AppMapProvider provider)
{
    if (!ui || !ui->map_initialized) return;
    int max_zoom = app_map_provider_max_zoom(provider);
    if (ui->map_zoom <= max_zoom) return;
    int old_zoom = ui->map_zoom;
    double old_world = (double)(1u << old_zoom);
    double next_world = (double)(1u << max_zoom);
    ui->map_camera_x = ui->map_camera_x / old_world * next_world;
    ui->map_camera_y = ui->map_camera_y / old_world * next_world;
    ui->map_zoom = max_zoom;
    ui->map_center_x = map_wrap_x((int)floor(ui->map_camera_x), max_zoom);
    ui->map_center_y = map_clamp_y((int)floor(ui->map_camera_y), max_zoom);
    ui->map_request_pending = 1;
    ui->map_input_us = 0;
    ui->map_preview_zoom = -1;
    app_log("map provider zoom constrained: old=%d max=%d center=%d,%d",
            old_zoom, max_zoom, ui->map_center_x, ui->map_center_y);
}

void ui_map_pan_pixels(UiState *ui, float dx, float dy, uint64_t now_us)
{
    if (!ui->map_initialized || (fabsf(dx) < 0.01f && fabsf(dy) < 0.01f))
        return;
    double world = (double)(1u << ui->map_zoom);
    ui->map_camera_x += dx / 256.0;
    ui->map_camera_x = fmod(ui->map_camera_x, world);
    if (ui->map_camera_x < 0.0) ui->map_camera_x += world;
    ui->map_camera_y += dy / 256.0;
    if (ui->map_camera_y < 0.0) ui->map_camera_y = 0.0;
    if (ui->map_camera_y > world - 0.0001)
        ui->map_camera_y = world - 0.0001;
    int center_x = (int)floor(ui->map_camera_x);
    int center_y = (int)floor(ui->map_camera_y);
    if (center_x != ui->map_center_x || center_y != ui->map_center_y) {
        ui->map_center_x = center_x;
        ui->map_center_y = center_y;
        ui->map_request_pending = 1;
    }
    ui->map_preview_dx = dx < 0.0f ? -1 : dx > 0.0f ? 1 : 0;
    ui->map_preview_dy = dy < 0.0f ? -1 : dy > 0.0f ? 1 : 0;
    ui->map_preview_zoom = 0;
    ui->map_preview_start_us = now_us;
    ui->map_input_us = now_us;
}

void ui_map_pan(UiState *ui, int dx, int dy, uint64_t now_us)
{
    ui_map_pan_pixels(ui, dx * 4.5f, dy * 4.5f, now_us);
}

void ui_map_zoom(UiState *ui, const WeatherData *weather,
                 AppMapProvider provider, int direction,
                 uint64_t now_us)
{
    if (!ui->map_initialized) ui_map_focus(ui, weather);
    if (!direction) return;
    /* Keep fractional fallback within one raster level, as VitaMaps does.
     * Repeated zoom requests used to stretch a stale 5x3 window through
     * several levels while the worker caught up, producing the severe
     * distorted-map frame reported on hardware. */
    if (ui->map_loaded_zoom > 0 && ui->map_zoom != ui->map_loaded_zoom) {
        app_log("map zoom deferred: target=%d active=%d",
                ui->map_zoom, ui->map_loaded_zoom);
        return;
    }
    int old_zoom = ui->map_zoom;
    int next_zoom = old_zoom + (direction > 0 ? 1 : -1);
    if (next_zoom < 6) next_zoom = 6;
    int max_zoom = app_map_provider_max_zoom(provider);
    if (next_zoom > max_zoom) next_zoom = max_zoom;
    if (next_zoom == old_zoom) return;
    double old_world = (double)(1u << old_zoom);
    double next_world = (double)(1u << next_zoom);
    double normalized_x = ui->map_camera_x / old_world;
    double normalized_y = ui->map_camera_y / old_world;
    ui->map_zoom = next_zoom;
    ui->map_camera_x = normalized_x * next_world;
    ui->map_camera_y = normalized_y * next_world;
    ui->map_center_x = map_wrap_x((int)floor(ui->map_camera_x), next_zoom);
    ui->map_center_y = map_clamp_y((int)floor(ui->map_camera_y), next_zoom);
    ui->map_request_pending = 1;
    ui->map_preview_dx = 0;
    ui->map_preview_dy = 0;
    ui->map_preview_zoom = direction > 0 ? 1 : -1;
    ui->map_preview_start_us = now_us;
    ui->map_input_us = now_us;
    app_log("map zoom accepted: zoom=%d center=%d,%d", ui->map_zoom,
            ui->map_center_x, ui->map_center_y);
}

void ui_map_time_move(UiState *ui, int direction,
                      const WeatherData *weather)
{
    if (!ui || !direction) return;
    ui_map_set_hour(ui, ui->map_hour + (direction > 0 ? 1 : -1), weather);
}

void ui_map_set_hour(UiState *ui, int hour, const WeatherData *weather)
{
    int count = map_hour_count(weather);
    if (!ui || !count) return;
    if (hour < 0) hour = 0;
    if (hour >= count) hour = count - 1;
    ui->map_hour = hour;
}

void ui_map_toggle_fullscreen(UiState *ui)
{
    if (!ui) return;
    ui->map_fullscreen = !ui->map_fullscreen;
    app_log("map display mode: %s",
            ui->map_fullscreen ? "fullscreen" : "windowed");
}

void ui_map_recenter(UiState *ui, const WeatherData *weather,
                     uint64_t now_us)
{
    if (!ui || !weather) return;
    if (!ui->map_initialized) {
        ui_map_focus(ui, weather);
        return;
    }
    double world = (double)(1u << ui->map_zoom);
    ui->map_camera_x = map_tile_x(weather->longitude, ui->map_zoom);
    ui->map_camera_y = map_tile_y(weather->latitude, ui->map_zoom);
    ui->map_camera_x = fmod(ui->map_camera_x, world);
    if (ui->map_camera_x < 0.0) ui->map_camera_x += world;
    if (ui->map_camera_y < 0.0) ui->map_camera_y = 0.0;
    if (ui->map_camera_y > world - 0.0001)
        ui->map_camera_y = world - 0.0001;
    ui->map_center_x = map_wrap_x((int)floor(ui->map_camera_x), ui->map_zoom);
    ui->map_center_y = map_clamp_y((int)floor(ui->map_camera_y), ui->map_zoom);
    ui->map_request_pending = 1;
    ui->map_preview_dx = 0;
    ui->map_preview_dy = 0;
    ui->map_preview_zoom = 0;
    ui->map_preview_start_us = now_us;
    ui->map_input_us = now_us;
    app_log("map recentered: zoom=%d center=%d,%d source=%s",
            ui->map_zoom, ui->map_center_x, ui->map_center_y,
            weather->source == WEATHER_SOURCE_LIVE ? "live" :
            weather->source == WEATHER_SOURCE_CACHE ? "cache" : "sample");
}

void ui_hourly_move(UiState *ui, int direction, const WeatherData *weather)
{
    if (!ui || !weather || !direction) return;
    int first = weather->current_hour_index;
    if (first < 0 || first >= weather->hour_count) first = 0;
    int count = weather->hour_count - first;
    if (count > 24) count = 24;
    int maximum_start = count > 8 ? count - 8 : 0;
    int next = ui->hourly_offset + (direction > 0 ? 1 : -1);
    if (next < 0) next = 0;
    if (next > maximum_start) next = maximum_start;
    ui->hourly_offset = next;
}

void ui_daily_move(UiState *ui, int direction, const WeatherData *weather)
{
    if (!weather || !direction) return;
    int next = ui->daily_selected + (direction > 0 ? 1 : -1);
    if (next < 0) next = 0;
    if (next >= weather->day_count) next = weather->day_count - 1;
    if (next == ui->daily_selected) return;
    ui->daily_selected = next;
    if (next < ui->daily_offset) ui->daily_offset = next;
    if (next >= ui->daily_offset + 7) ui->daily_offset = next - 6;
    app_log("daily forecast selected: day=%d total=%d",
            next + 1, weather->day_count);
}

void ui_open_day_detail(UiState *ui, const WeatherData *weather)
{
    if (!ui || !weather || weather->day_count < 1) return;
    if (ui->daily_selected >= weather->day_count)
        ui->daily_selected = weather->day_count - 1;
    if (ui->daily_selected < 0) ui->daily_selected = 0;
    ui->day_detail_scroll = 0.0f;
    ui->day_detail_scroll_target = 0.0f;
    ui->day_detail_max_scroll = 1200.0f;
    ui->day_detail_scroll_update_us = 0;
    ui->day_detail_metric = DAY_METRIC_CONDITIONS;
    ui->day_detail_feels_like = 0;
    ui->screen = UI_SCREEN_DAY_DETAIL;
    app_log("day detail opened: day=%d", ui->daily_selected + 1);
}

void ui_open_day_metrics(UiState *ui)
{
    if (!ui) return;
    ui->day_metric_selected = ui->day_detail_metric;
    ui->screen = UI_SCREEN_DAY_METRICS;
}

void ui_confirm_day_metric(UiState *ui)
{
    if (!ui || ui->screen != UI_SCREEN_DAY_METRICS) return;
    if (ui->day_metric_selected < 0 ||
        ui->day_metric_selected >= DAY_METRIC_COUNT)
        ui->day_metric_selected = DAY_METRIC_CONDITIONS;
    ui->day_detail_metric = ui->day_metric_selected;
    ui->day_detail_scroll = 0.0f;
    ui->day_detail_scroll_target = 0.0f;
    ui->day_detail_max_scroll = 1200.0f;
    ui->screen = UI_SCREEN_DAY_DETAIL;
    app_log("day detail metric selected: %s",
            day_metric_name(ui->day_detail_metric));
}

void ui_day_detail_scroll(UiState *ui, float amount)
{
    if (!ui || fabsf(amount) < 0.01f) return;
    ui->day_detail_scroll_target += amount;
    if (ui->day_detail_scroll_target < 0.0f)
        ui->day_detail_scroll_target = 0.0f;
    float maximum = ui->day_detail_max_scroll > 0.0f
                        ? ui->day_detail_max_scroll : 1200.0f;
    if (ui->day_detail_scroll_target > maximum)
        ui->day_detail_scroll_target = maximum;
}

void ui_day_detail_toggle_metric(UiState *ui, int direction)
{
    if (!ui || !direction) return;
    if (ui->day_detail_metric != DAY_METRIC_CONDITIONS) return;
    ui->day_detail_feels_like = ui->day_detail_feels_like ? 0 : 1;
    app_log("day detail temperature: %s",
            ui->day_detail_feels_like ? "feels-like" : "actual");
}

int ui_map_take_request(UiState *ui, int *zoom, int *center_x, int *center_y,
                        int *active_provider, int *active_zoom,
                        int *active_base_x, int *active_base_y)
{
    /* Physical stick/touch input can keep updating the desired camera while
     * the current 5x3 window is downloading. Submit only after that window is
     * complete; the pending values then represent the latest camera position
     * instead of cancelling and replacing the worker job every frame. */
    if (!ui->map_request_pending || ui->map_navigation_locked) return 0;
    *zoom = ui->map_zoom;
    int desired_x = ui->map_center_x;
    int desired_y = ui->map_center_y;
    *center_x = desired_x;
    *center_y = desired_y;
    int catch_up = 0;
    if (ui->map_loaded_zoom == ui->map_zoom && ui->map_loaded_zoom > 0) {
        int active_center_x = map_wrap_x(
            ui->map_loaded_base_x + MAP_TILE_COLUMNS / 2, ui->map_zoom);
        int active_center_y = map_clamp_y(
            ui->map_loaded_base_y + MAP_TILE_ROWS / 2, ui->map_zoom);
        if (ui->map_fullscreen) {
            /* Input continues while the worker is busy. Jump the next raster
             * request directly to the newest camera cell so a long stick hold
             * does not replay a queue of obsolete one-tile windows after the
             * user releases it. The visible camera remains independently
             * smoothed over the loading underlay. */
            if (desired_x != active_center_x || desired_y != active_center_y)
                app_log("map fullscreen request latest: desired=%d,%d active=%d,%d",
                        desired_x, desired_y,
                        active_center_x, active_center_y);
        } else {
            *center_x = map_step_wrapped(active_center_x, desired_x,
                                         ui->map_zoom, 1);
            *center_y = map_step_linear(active_center_y, desired_y, 1);
            *center_y = map_clamp_y(*center_y, ui->map_zoom);
            catch_up = *center_x != desired_x || *center_y != desired_y;
            if (catch_up)
                app_log("map request catch-up: desired=%d,%d submit=%d,%d active=%d,%d",
                        desired_x, desired_y, *center_x, *center_y,
                        active_center_x, active_center_y);
        }
    }
    *active_provider = ui->map_loaded_provider;
    *active_zoom = ui->map_loaded_zoom;
    *active_base_x = ui->map_loaded_base_x;
    *active_base_y = ui->map_loaded_base_y;
    ui->map_request_pending = catch_up;
    ui->map_navigation_locked = 1;
    return 1;
}

void ui_request_capture(UiState *ui, const char *path)
{
    if (!path || !path[0]) return;
    snprintf(ui->capture_path, sizeof(ui->capture_path), "%s", path);
    ui->capture_pending = 1;
    ui->capture_delay_frames = 2;
}

static void capture_framebuffer(UiState *ui)
{
    const uint32_t *framebuffer = vita2d_get_current_fb();
    FILE *file = framebuffer ? fopen(ui->capture_path, "wb") : NULL;
    if (!file) {
        app_log("visual capture failed: path='%s' framebuffer=%p",
                ui->capture_path, (const void *)framebuffer);
        ui->capture_pending = 0;
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; ++y) {
        unsigned char row[SCREEN_W * 3];
        for (int x = 0; x < SCREEN_W; ++x) {
            uint32_t pixel = framebuffer[y * SCREEN_STRIDE + x];
            row[x * 3] = (unsigned char)(pixel & 0xff);
            row[x * 3 + 1] = (unsigned char)((pixel >> 8) & 0xff);
            row[x * 3 + 2] = (unsigned char)((pixel >> 16) & 0xff);
        }
        fwrite(row, sizeof(row), 1, file);
    }
    int failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    app_log("visual capture %s: path='%s' view=%d",
            failed ? "failed" : "saved", ui->capture_path, ui->view);
    ui->capture_pending = 0;
}

void ui_draw(UiState *ui, const AppSnapshot *snapshot, uint64_t now_us)
{
    /* libvita2d has one transient vertex pool, reset by start_drawing().
     * Queueing a framebuffer swap does not fence that pool. Finish the previous
     * scene before reusing vertices or retiring textures, including the warm-up
     * frame. Otherwise the GPU can read next-frame UI/map vertices mid-draw. */
    vita2d_wait_rendering_done();
    AppSnapshot display = *snapshot;
    if (display.theme == APP_THEME_DAY)
        display.weather.is_day = 1;
    else if (display.theme == APP_THEME_NIGHT)
        display.weather.is_day = 0;
    snapshot = &display;
    if (warm_font_atlases(ui)) return;
    update_weather_background(ui, &snapshot->weather,
                              snapshot->photo_background, now_us);
    if (ui->view == VIEW_MAP)
        sync_map_tiles(ui, &snapshot->map, now_us);
    vita2d_start_drawing();
    int fullscreen_map = ui->view == VIEW_MAP && ui->map_fullscreen;
    if (fullscreen_map) {
        vita2d_clear_screen();
        draw_map(ui, snapshot, 0, 255, 1000000u);
    } else {
        draw_background(ui, &snapshot->weather, snapshot->photo_background,
                        snapshot->motion, now_us);
        draw_header(ui, snapshot, now_us);
        vita2d_set_clip_rectangle(0, 74, SCREEN_W, 488);
        vita2d_enable_clipping();
        uint64_t elapsed = now_us - ui->transition_start_us;
        int transition_active = ui->transition_start_us &&
                                elapsed < TRANSITION_US;
        float transition_t = transition_active
            ? ease_out_cubic((float)elapsed / (float)TRANSITION_US) : 1.0f;
        if (transition_active) {
            float t = transition_t;
            float old_offset = -ui->transition_direction * 38.0f * t;
            float new_offset = ui->transition_direction * 48.0f * (1.0f - t);
            draw_page(ui, snapshot, ui->previous_view, old_offset,
                      (unsigned int)(255.0f * (1.0f - t)), 1000000u);
            draw_page(ui, snapshot, ui->view, new_offset,
                      (unsigned int)(255.0f * t), elapsed);
        } else {
            draw_page(ui, snapshot, ui->view, 0, 255, 1000000u);
        }
        vita2d_disable_clipping();
    }
    if (ui->screen == UI_SCREEN_WEATHER) {
        if (!fullscreen_map) draw_footer(ui, snapshot);
    } else if (ui->screen == UI_SCREEN_SETTINGS)
        draw_settings(ui, snapshot);
    else if (ui->screen == UI_SCREEN_LOCATIONS)
        draw_locations(ui, snapshot);
    else if (ui->screen == UI_SCREEN_SEARCH)
        draw_search(ui, snapshot);
    else if (ui->screen == UI_SCREEN_MAP_OPTIONS)
        draw_map_options(ui, snapshot);
    else if (ui->screen == UI_SCREEN_DAY_DETAIL)
        draw_day_detail(ui, snapshot);
    else {
        draw_day_detail(ui, snapshot);
        draw_day_metric_selector(ui, snapshot);
    }
    vita2d_end_drawing();
    int map_capture_ready = ui->view != VIEW_MAP ||
        (!snapshot->map.loading &&
         !snapshot->map.field_loading &&
         map_texture_set_complete(ui->map_tile_loaded));
    if (ui->capture_pending && map_capture_ready) {
        if (ui->capture_delay_frames > 0)
            --ui->capture_delay_frames;
        else {
            vita2d_wait_rendering_done();
            capture_framebuffer(ui);
        }
    }
    vita2d_swap_buffers();
}

static void utf16_to_utf8(const uint16_t *text, char *out, size_t capacity)
{
    size_t at = 0;
    while (*text && at + 1 < capacity) {
        unsigned int cp = *text++;
        if (cp < 0x80) out[at++] = (char)cp;
        else if (cp < 0x800 && at + 2 < capacity) {
            out[at++] = (char)(0xc0 | (cp >> 6));
            out[at++] = (char)(0x80 | (cp & 0x3f));
        } else if (at + 3 < capacity) {
            out[at++] = (char)(0xe0 | (cp >> 12));
            out[at++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[at++] = (char)(0x80 | (cp & 0x3f));
        } else break;
    }
    out[at] = '\0';
}

static void ime_event_handler(void *arg, const SceImeEventData *event)
{
    UiState *ui = arg;
    if (!ui || !event) return;
    if (event->id == SCE_IME_EVENT_UPDATE_TEXT) {
        const uint16_t *source = event->param.text.str;
        if (source && source != ui->ime_input) {
            int at = 0;
            while (source[at] && at < 95) {
                ui->ime_input[at] = source[at];
                ++at;
            }
            ui->ime_input[at] = 0;
        }
        ui->ime_dirty = 1;
    } else if (event->id == SCE_IME_EVENT_PRESS_ENTER) {
        ui->ime_dirty = 1;
        ui->ime_submit = 1;
    } else if (event->id == SCE_IME_EVENT_PRESS_CLOSE) {
        /* Keep the IME marked open until the main thread calls sceImeClose().
         * Clearing this here skipped the close call and could leave the
         * keyboard owning controller focus behind the visible result list. */
        ui->ime_close = 1;
    }
}

void ui_open_settings(UiState *ui)
{
    if (!ui) return;
    if (ui->screen == UI_SCREEN_SEARCH && ui->ime_active)
        sceImeClose();
    ui->ime_active = 0;
    ui->settings_tab = ui->settings_selected < APP_SETTING_THEME ? 0 :
                       ui->settings_selected < APP_SETTING_MAP_PROVIDER ? 1 : 2;
    ui->settings_previous_tab = ui->settings_tab;
    ui->settings_previous_selected = ui->settings_selected;
    ui->settings_transition_start_us = 0;
    ui->overlay_open_us = sceKernelGetProcessTimeWide();
    ui->screen = UI_SCREEN_SETTINGS;
    app_log("UI overlay opened: settings tab=%d", ui->settings_tab);
}

void ui_open_map_options(UiState *ui)
{
    if (!ui) return;
    if (ui->screen == UI_SCREEN_SEARCH && ui->ime_active)
        sceImeClose();
    ui->ime_active = 0;
    ui->screen = UI_SCREEN_MAP_OPTIONS;
    ui->map_options_selected = 0;
    app_log("UI overlay opened: map-layers");
}

void ui_open_locations(UiState *ui)
{
    if (!ui) return;
    if (ui->screen == UI_SCREEN_SEARCH && ui->ime_active)
        sceImeClose();
    ui->ime_active = 0;
    ui->screen = UI_SCREEN_LOCATIONS;
    ui->location_selected = 0;
    app_log("UI overlay opened: locations");
}

int ui_open_search(UiState *ui, uint64_t now_us)
{
    if (!ui) return -1;
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    memset(ui->ime_work, 0, sizeof(ui->ime_work));
    memset(ui->ime_initial, 0, sizeof(ui->ime_initial));
    memset(ui->ime_input, 0, sizeof(ui->ime_input));
    memset(ui->search_query, 0, sizeof(ui->search_query));
    ui->ime_dirty = 0;
    ui->ime_submit = 0;
    ui->ime_close = 0;
    ui->search_query_pending = 0;
    ui->search_selected = 0;
    ui->search_changed_us = now_us;

    SceImeParam parameters;
    sceImeParamInit(&parameters);
    parameters.supportedLanguages = 0;
    parameters.languagesForced = 0;
    parameters.type = SCE_IME_TYPE_DEFAULT;
    parameters.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    parameters.work = ui->ime_work;
    parameters.arg = ui;
    parameters.handler = ime_event_handler;
    parameters.initialText = ui->ime_initial;
    parameters.maxTextLength = 95;
    parameters.inputTextBuffer = ui->ime_input;
    parameters.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;
    int result = sceImeOpen(&parameters);
    if (result < 0) {
        app_log("sceImeOpen failed: 0x%08x", result);
        return result;
    }
    ui->ime_active = 1;
    ui->screen = UI_SCREEN_SEARCH;
    app_log("UI overlay opened: search IME=active");
    return 0;
}

int ui_resume_search_keyboard(UiState *ui, uint64_t now_us)
{
    if (!ui || ui->screen != UI_SCREEN_SEARCH) return -1;
    if (ui->ime_active) return 0;
    sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
    memset(ui->ime_work, 0, sizeof(ui->ime_work));
    memcpy(ui->ime_initial, ui->ime_input, sizeof(ui->ime_initial));
    ui->ime_dirty = 0;
    ui->ime_submit = 0;
    ui->ime_close = 0;
    ui->search_changed_us = now_us;

    SceImeParam parameters;
    sceImeParamInit(&parameters);
    parameters.supportedLanguages = 0;
    parameters.languagesForced = 0;
    parameters.type = SCE_IME_TYPE_DEFAULT;
    parameters.option = SCE_IME_OPTION_NO_AUTO_CAPITALIZATION;
    parameters.work = ui->ime_work;
    parameters.arg = ui;
    parameters.handler = ime_event_handler;
    parameters.initialText = ui->ime_initial;
    parameters.maxTextLength = 95;
    parameters.inputTextBuffer = ui->ime_input;
    parameters.enterLabel = SCE_IME_ENTER_LABEL_SEARCH;
    int result = sceImeOpen(&parameters);
    if (result < 0) {
        app_log("sceImeOpen resume failed: 0x%08x", result);
        return result;
    }
    ui->ime_active = 1;
    app_log("search IME resumed: query='%s'", ui->search_query);
    return 0;
}

void ui_close_overlay(UiState *ui)
{
    if (!ui) return;
    UiScreen previous = ui->screen;
    if (ui->screen == UI_SCREEN_DAY_METRICS) {
        ui->screen = UI_SCREEN_DAY_DETAIL;
    } else if (ui->screen == UI_SCREEN_SEARCH) {
        if (ui->ime_active) sceImeClose();
        ui->ime_active = 0;
        ui->screen = UI_SCREEN_LOCATIONS;
    } else {
        ui->screen = UI_SCREEN_WEATHER;
    }
    app_log("UI overlay closed: from=%d to=%d", previous, ui->screen);
}

void ui_hide_search_keyboard(UiState *ui)
{
    if (!ui) return;
    if (ui->ime_active) sceImeClose();
    ui->ime_active = 0;
}

void ui_settings_tab_move(UiState *ui, int direction)
{
    if (!ui || !direction) return;
    ui->settings_previous_tab = ui->settings_tab;
    ui->settings_previous_selected = ui->settings_selected;
    ui->settings_transition_direction = direction > 0 ? 1 : -1;
    ui->settings_tab = (ui->settings_tab + (direction > 0 ? 1 : 2)) % 3;
    ui->settings_selected = settings_tab_start(ui->settings_tab);
    ui->settings_transition_start_us = sceKernelGetProcessTimeWide();
    app_log("settings tab changed: from=%d to=%d",
            ui->settings_previous_tab, ui->settings_tab);
}

void ui_move_selection(UiState *ui, int direction,
                       const AppSnapshot *snapshot)
{
    if (!ui || !snapshot || !direction) return;
    int *selection = NULL;
    int count = 0;
    if (ui->screen == UI_SCREEN_SETTINGS) {
        selection = &ui->settings_selected;
        int start = settings_tab_start(ui->settings_tab);
        count = settings_tab_count(ui->settings_tab);
        int local = *selection - start;
        if (local < 0 || local >= count) local = 0;
        local = (local + (direction > 0 ? 1 : count - 1)) % count;
        *selection = start + local;
        return;
    } else if (ui->screen == UI_SCREEN_MAP_OPTIONS) {
        selection = &ui->map_options_selected;
        count = APP_MAP_LAYER_COUNT + 1;
    } else if (ui->screen == UI_SCREEN_LOCATIONS) {
        selection = &ui->location_selected;
        count = snapshot->saved_location_count + 1;
    } else if (ui->screen == UI_SCREEN_SEARCH) {
        selection = &ui->search_selected;
        count = snapshot->search_result_count;
    } else if (ui->screen == UI_SCREEN_DAY_METRICS) {
        selection = &ui->day_metric_selected;
        count = DAY_METRIC_COUNT;
    }
    if (!selection || count < 1) return;
    *selection = (*selection + (direction > 0 ? 1 : count - 1)) % count;
}

void ui_update_ime(UiState *ui, uint64_t now_us)
{
    if (!ui || ui->screen != UI_SCREEN_SEARCH) return;
    if (ui->ime_active) sceImeUpdate();
    if (ui->ime_dirty) {
        char updated[sizeof(ui->search_query)];
        utf16_to_utf8(ui->ime_input, updated, sizeof(updated));
        ui->ime_dirty = 0;
        if (strcmp(updated, ui->search_query)) {
            snprintf(ui->search_query, sizeof(ui->search_query), "%s", updated);
            ui->search_changed_us = now_us;
            ui->search_query_pending = 1;
            ui->search_selected = 0;
        }
    }
}

int ui_take_search_query(UiState *ui, uint64_t now_us,
                         char *out, size_t out_size)
{
    if (!ui || !out || !out_size || !ui->search_query_pending ||
        now_us - ui->search_changed_us < 350000u)
        return 0;
    ui->search_query_pending = 0;
    snprintf(out, out_size, "%s", ui->search_query);
    return 1;
}

int ui_take_search_submit(UiState *ui)
{
    if (!ui || !ui->ime_submit) return 0;
    ui->ime_submit = 0;
    return 1;
}

int ui_take_search_close(UiState *ui)
{
    if (!ui || !ui->ime_close) return 0;
    ui->ime_close = 0;
    return 1;
}
