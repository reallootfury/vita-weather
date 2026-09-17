#ifndef VITA_WEATHER_AUDIO_H
#define VITA_WEATHER_AUDIO_H

#include "weather.h"

int weather_audio_init(void);
void weather_audio_update(const WeatherData *weather, int enabled);
void weather_audio_shutdown(void);

#endif
