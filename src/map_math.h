#ifndef VITA_WEATHER_MAP_MATH_H
#define VITA_WEATHER_MAP_MATH_H

double map_tile_x(double longitude, int zoom);
double map_tile_y(double latitude, int zoom);
double map_tile_longitude(double tile_x, int zoom);
double map_tile_latitude(double tile_y, int zoom);
int map_wrap_x(int tile_x, int zoom);
int map_clamp_y(int tile_y, int zoom);
double map_align_wrapped_center(double center, double reference, int zoom);
double map_clamp_window_center(double center, double base, int tile_count,
                               double half_view_tiles);
int map_step_wrapped(int current, int target, int zoom, int maximum_step);
int map_step_linear(int current, int target, int maximum_step);
double map_approach(double current, double target, double maximum_step);

#endif
