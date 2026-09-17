#ifndef VITA_WEATHER_WEATHER_H
#define VITA_WEATHER_WEATHER_H

#include <stddef.h>

#define WEATHER_HOURS 240
#define WEATHER_DAYS 10
#define WEATHER_MAP_COLUMNS 5
#define WEATHER_MAP_ROWS 3
#define WEATHER_MAP_POINTS (WEATHER_MAP_COLUMNS * WEATHER_MAP_ROWS)
#define WEATHER_MAP_HOURS 24

typedef enum WeatherSource {
    WEATHER_SOURCE_DEMO = 0,
    WEATHER_SOURCE_CACHE = 1,
    WEATHER_SOURCE_LIVE = 2
} WeatherSource;

typedef struct WeatherHour {
    char time[20];
    char label[8];
    float temperature_c;
    float apparent_c;
    float wind_kmh;
    float wind_gust_kmh;
    float wind_direction;
    float precipitation_mm;
    float air_quality;
    float dew_point_c;
    float pressure_hpa;
    float visibility_km;
    float uv_index;
    int humidity;
    int weather_code;
    int precipitation_percent;
} WeatherHour;

typedef struct WeatherDay {
    char date[16];
    char label[8];
    float high_c;
    float low_c;
    float uv_index;
    float precipitation_mm;
    int weather_code;
    int precipitation_percent;
    char sunrise[8];
    char sunset[8];
} WeatherDay;

typedef struct WeatherData {
    char location[64];
    char region[96];
    char timezone[64];
    char updated[24];
    double latitude;
    double longitude;
    float temperature_c;
    float apparent_c;
    float wind_kmh;
    float wind_direction;
    float pressure_hpa;
    float visibility_km;
    float cloud_cover;
    float uv_index;
    float air_quality;
    float pm2_5;
    float pm10;
    int humidity;
    int weather_code;
    int is_day;
    int hour_count;
    int current_hour_index;
    int day_count;
    WeatherHour hours[WEATHER_HOURS];
    WeatherDay days[WEATHER_DAYS];
    WeatherSource source;
} WeatherData;

typedef struct WeatherLocation {
    char name[64];
    char region[96];
    char timezone[64];
    double latitude;
    double longitude;
} WeatherLocation;

typedef struct WeatherMapPoint {
    float temperature_c[WEATHER_MAP_HOURS];
    float precipitation_mm[WEATHER_MAP_HOURS];
    float wind_kmh[WEATHER_MAP_HOURS];
    float wind_direction[WEATHER_MAP_HOURS];
    float air_quality[WEATHER_MAP_HOURS];
    unsigned char forecast_valid;
    unsigned char air_quality_valid;
} WeatherMapPoint;

typedef struct WeatherMapField {
    int point_count;
    int hour_count;
    WeatherMapPoint points[WEATHER_MAP_POINTS];
} WeatherMapField;

void weather_load_demo(WeatherData *out);
int weather_parse_forecast(const char *json, size_t len, WeatherData *out,
                           char *error, size_t error_size);
int weather_parse_geocode(const char *json, size_t len, WeatherLocation *out,
                          char *error, size_t error_size);
int weather_parse_geocodes(const char *json, size_t len, WeatherLocation *out,
                           int capacity, char *error, size_t error_size);
int weather_parse_air_quality(const char *json, size_t len, WeatherData *weather,
                              char *error, size_t error_size);
int weather_parse_map_forecast(const char *json, size_t len,
                               WeatherMapField *field, char *error,
                               size_t error_size);
int weather_parse_map_air_quality(const char *json, size_t len,
                                  WeatherMapField *field, char *error,
                                  size_t error_size);
const char *weather_condition_name(int code);
int weather_condition_group(int code);
float weather_display_temperature(float celsius, int use_fahrenheit);
const char *weather_temperature_unit(int use_fahrenheit);
float weather_display_wind(float kmh, int use_imperial);
const char *weather_wind_unit(int use_imperial);
float weather_display_visibility(float km, int use_imperial);
const char *weather_visibility_unit(int use_imperial);
float weather_display_pressure(float hpa, int use_imperial);
const char *weather_pressure_unit(int use_imperial);

#endif
