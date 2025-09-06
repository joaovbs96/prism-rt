#pragma once

#include "float3.h"
#include "math.h"
#include "rng.h"

// https://karthikkaranth.me/blog/generating-random-points-in-a-sphere/
Float3 random_unit_vector(unsigned int &seed) {
    const float a = rnd(seed) * TWO_PI;
    const float z = (2.0f * rnd(seed)) - 1;
    const float r = std::sqrtf(1.0f - (z * z));
    return Float3(r * std::cosf(a), r * std::sinf(a), z);
}

Float3 sample_hemisphere(unsigned int &seed) {
    const float r = std::sqrtf(rnd(seed));
    const float phi = TWO_PI * rnd(seed);
    const float x = r * std::cosf(phi);
    const float y = r * std::sinf(phi);
    const float z = std::sqrtf(std::fmaxf(0.0f, 1.0f - (x * x) - (y * y)));
    return Float3(x, y, z);
}

Float3 random_in_unit_disk(unsigned int &seed) {
    const float a = rnd(seed) * TWO_PI;
    const Float3 xy = Float3(std::sinf(a), std::cosf(a), 0);
    return xy * std::sqrtf(rnd(seed));
}

Float3 random_cosine_direction(unsigned int &seed) {
    /*
    Produces a random direction weighted by cos(θ) where θ
    is the angle from the z axis.
    */
    const float r = std::sqrtf(rnd(seed));
    const float phi = TWO_PI * rnd(seed);
    const float x = r * std::cosf(phi);
    const float y = r * std::sinf(phi);
    const float z = std::sqrt(1 - r);
    return Float3(x, y, z);
}
