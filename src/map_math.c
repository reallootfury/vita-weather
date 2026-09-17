#include "map_math.h"

#include <math.h>

#define MAP_PI 3.14159265358979323846
#define MAP_MAX_LATITUDE 85.05112878

static double clamp(double value, double minimum, double maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;
    return value;
}

double map_tile_x(double longitude, int zoom)
{
    double tiles = (double)(1u << zoom);
    longitude = clamp(longitude, -180.0, 180.0);
    return (longitude + 180.0) / 360.0 * tiles;
}

double map_tile_y(double latitude, int zoom)
{
    double tiles = (double)(1u << zoom);
    latitude = clamp(latitude, -MAP_MAX_LATITUDE, MAP_MAX_LATITUDE);
    double radians = latitude * MAP_PI / 180.0;
    double mercator = log(tan(radians) + 1.0 / cos(radians));
    return (1.0 - mercator / MAP_PI) * 0.5 * tiles;
}

double map_tile_longitude(double tile_x, int zoom)
{
    double tiles = (double)(1u << zoom);
    return tile_x / tiles * 360.0 - 180.0;
}

double map_tile_latitude(double tile_y, int zoom)
{
    double tiles = (double)(1u << zoom);
    double mercator = MAP_PI * (1.0 - 2.0 * tile_y / tiles);
    return atan(sinh(mercator)) * 180.0 / MAP_PI;
}

int map_wrap_x(int tile_x, int zoom)
{
    int tiles = 1 << zoom;
    tile_x %= tiles;
    return tile_x < 0 ? tile_x + tiles : tile_x;
}

int map_clamp_y(int tile_y, int zoom)
{
    int maximum = (1 << zoom) - 1;
    if (tile_y < 0) return 0;
    if (tile_y > maximum) return maximum;
    return tile_y;
}

double map_align_wrapped_center(double center, double reference, int zoom)
{
    double world = (double)(1u << zoom);
    return center + round((reference - center) / world) * world;
}

double map_clamp_window_center(double center, double base, int tile_count,
                               double half_view_tiles)
{
    double minimum = base + half_view_tiles;
    double maximum = base + tile_count - half_view_tiles;
    if (minimum > maximum) return base + tile_count * 0.5;
    return clamp(center, minimum, maximum);
}

int map_step_linear(int current, int target, int maximum_step)
{
    if (maximum_step < 1 || current == target) return target;
    if (target > current + maximum_step) return current + maximum_step;
    if (target < current - maximum_step) return current - maximum_step;
    return target;
}

int map_step_wrapped(int current, int target, int zoom, int maximum_step)
{
    int world = 1 << zoom;
    current = map_wrap_x(current, zoom);
    target = map_wrap_x(target, zoom);
    int delta = target - current;
    if (delta > world / 2) delta -= world;
    if (delta < -world / 2) delta += world;
    if (delta > maximum_step) delta = maximum_step;
    if (delta < -maximum_step) delta = -maximum_step;
    return map_wrap_x(current + delta, zoom);
}

double map_approach(double current, double target, double maximum_step)
{
    if (maximum_step <= 0.0) return target;
    double delta = target - current;
    if (delta > maximum_step) delta = maximum_step;
    if (delta < -maximum_step) delta = -maximum_step;
    return current + delta;
}
