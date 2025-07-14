#pragma once

#include <cmath>

typedef unsigned int uint;

#define PI 3.1415926f
#define TWO_PI 6.2831852f
#define FOUR_PI 12.5663704f
#define HALF_PI 1.5707963f
#define INV_PI 0.3183099f
#define INV_TWOPI 0.15915494f

// radians and degrees
inline float deg2rad(float deg) { return (PI / 180.f) * deg; }
inline float rad2deg(float rad) { return (180.f / PI) * rad; }
