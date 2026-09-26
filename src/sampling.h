#pragma once

#include "float3.h"
#include "math.h"
#include "rng.h"

// https://karthikkaranth.me/blog/generating-random-points-in-a-sphere/
inline Float3 random_unit_vector(unsigned int &seed) {
  const float a = rnd(seed) * TWO_PI;
  const float z = (2.0f * rnd(seed)) - 1;
  const float r = sqrtf(1.0f - (z * z));

  return Float3(r * cosf(a), r * sinf(a), z);
}

inline Float3 sample_hemisphere(unsigned int &seed) {
  const float r = sqrtf(rnd(seed));
  const float phi = TWO_PI * rnd(seed);
  const float x = r * cosf(phi), y = r * sinf(phi);
  const float z = sqrtf(fmaxf(0.0f, 1.0f - (x * x) - (y * y)));
  return Float3(x, y, z);
}

inline Float3 random_in_unit_disk(unsigned int &seed) {
  const float a = rnd(seed) * TWO_PI;
  const Float3 xy = Float3(sinf(a), cosf(a), 0);
  return xy * sqrtf(rnd(seed));
}
