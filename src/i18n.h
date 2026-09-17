#ifndef VITA_WEATHER_I18N_H
#define VITA_WEATHER_I18N_H

#include <stddef.h>

#include "net.h"

typedef enum TextKey {
    TEXT_NOW = 0,
    TEXT_HOURS,
    TEXT_DAILY,
    TEXT_MAP,
    TEXT_RIGHT_NOW,
    TEXT_THIS_WEEK,
    TEXT_HIGH_LOW,
    TEXT_FEELS_LIKE,
    TEXT_AT_A_GLANCE,
    TEXT_HUMIDITY,
    TEXT_WIND,
    TEXT_RAIN_CHANCE,
    TEXT_SUNSET,
    TEXT_HOURLY_FORECAST,
    TEXT_TEMPERATURE,
    TEXT_RAIN,
    TEXT_SEVEN_DAY,
    TEXT_TODAY,
    TEXT_REFRESH,
    TEXT_LOCATIONS,
    TEXT_SETTINGS,
    TEXT_PAGES,
    TEXT_SELECT,
    TEXT_SEARCH,
    TEXT_REMOVE,
    TEXT_BACK,
    TEXT_USE_GPS,
    TEXT_SAVED_LOCATIONS,
    TEXT_NO_SAVED_LOCATIONS,
    TEXT_GPS_HELP,
    TEXT_TIME_FORMAT,
    TEXT_LANGUAGE,
    TEXT_APPEARANCE,
    TEXT_BACKGROUND,
    TEXT_PHOTOS,
    TEXT_CLASSIC,
    TEXT_MOTION,
    TEXT_WEATHER_SOUNDS,
    TEXT_AUTO_GPS,
    TEXT_FULL,
    TEXT_REDUCED,
    TEXT_STATIC,
    TEXT_OFF,
    TEXT_ON,
    TEXT_AUTO,
    TEXT_DAY,
    TEXT_NIGHT,
    TEXT_FIND_LOCATION,
    TEXT_TYPE_TO_SEARCH,
    TEXT_NO_MATCHES,
    TEXT_MAP_LAYER,
    TEXT_PRECIPITATION,
    TEXT_AIR_QUALITY,
    TEXT_TOMORROW,
    TEXT_DETAILS,
    TEXT_UV_INDEX,
    TEXT_VISIBILITY,
    TEXT_PRESSURE,
    TEXT_CLOUD_COVER,
    TEXT_SUNRISE,
    TEXT_SCROLL,
    TEXT_MAP_STYLE,
    TEXT_SELECT_DAY,
    TEXT_UNITS,
    TEXT_METRIC,
    TEXT_IMPERIAL,
    TEXT_GPS_ACTIVE,
    TEXT_GPS_INACTIVE,
    TEXT_HOURLY_UNAVAILABLE,
    TEXT_LIVE,
    TEXT_OFFLINE,
    TEXT_SAMPLE,
    TEXT_LAST_SYNC,
    TEXT_COUNT
} TextKey;

const char *tr(AppLanguage language, TextKey key);
const char *tr_day_label(AppLanguage language, const char *label);
const char *tr_language_name(AppLanguage language);
const char *tr_condition(AppLanguage language, int weather_code);
void tr_format_time(char *out, size_t out_size, const char *time_24,
                    int use_24_hour, AppLanguage language);
void tr_format_daily_summary(char *out, size_t out_size, AppLanguage language,
                             int weather_code, float high, float low);
void tr_format_precip_summary(char *out, size_t out_size, AppLanguage language,
                              int precipitation_percent);

#endif
