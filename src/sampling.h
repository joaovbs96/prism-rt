#pragma once

#include "float3.h"
#include "math.h"
#include "rng.h"

// https://karthikkaranth.me/blog/generating-random-points-in-a-sphere/
Float3 random_unit_vector() {
  const float a = rnd() * TWO_PI;
  const float z = (2.0f * rnd()) - 1;
  const float r = sqrtf(1.0f - (z * z));

  return Float3(r * cosf(a), r * sinf(a), z);
}

Float3 sample_hemisphere() {
  const float r = sqrtf(rnd());
  const float phi = TWO_PI * rnd();
  const float x = r * cosf(phi), y = r * sinf(phi);
  const float z = sqrtf(fmaxf(0.0f, 1.0f - (x * x) - (y * y)));
  return Float3(x, y, z);
}