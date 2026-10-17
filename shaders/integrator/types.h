#pragma once

// Types shared by the path tracer and every device backend. This file is
// compiled as C++ (after src/device/hlsl_math.h) and as HLSL, so it keeps to
// their common subset: structs without constructors, aggregate
// initialization, (float3)x for scalar-to-vector conversion, no swizzles,
// references or pointers.

static const float kPi = 3.14159265358979323846f;

// Outcome of a primary path (SceneManager::PrimaryOutcome).
static const uint kPrimaryUnresolved = 0u;
static const uint kPrimaryEscaped = 1u;
static const uint kPrimaryAccepted = 2u;

inline float infinity() { return asfloat(0x7f800000u); }

// rng.h: the same seeds and stream on every device.
inline uint tea64(uint v0, uint v1)
{
    uint s0 = 0u;
    for (uint n = 0u; n < 64u; n++) {
        s0 += 0x9e3779b9u;
        v0 += ((v1 << 4u) + 0xa341316cu) ^ (v1 + s0) ^ ((v1 >> 5u) + 0xc8013ea4u);
        v1 += ((v0 << 4u) + 0xad90777du) ^ (v0 + s0) ^ ((v0 >> 5u) + 0x7e95761eu);
    }
    return v0;
}

struct Rng {
    uint state;

    // Uniform in [0, 1) from a 24-bit LCG (rnd in rng.h).
    float next()
    {
        state = 1664525u * state + 1013904223u;
        return float(state & 0x00FFFFFFu) / 16777216.0f;
    }
};

struct Ray {
    float3 origin;
    float3 direction;
    float tnear;
    float tfar;
};

// Ray differentials (prism::RayFootprint).
struct RayFootprint {
    float3 dOdx;
    float3 dOdy;
    float3 dDdx;
    float3 dDdy;
    float width;
    float spread;
    bool exact;
};

struct PathRay {
    Ray ray;
    RayFootprint footprint;
};

struct TextureFootprint {
    float dudx;
    float dvdx;
    float dudy;
    float dvdy;
    bool valid;
    bool fallback;
    bool filtered;
};

// The geometric part of a prepared hit (RayHit). Normals face the ray.
// Backend hit types carry one of these as `surface`.
struct SurfaceHit {
    bool valid;
    float3 p;
    float3 o;
    float3 d;
    float3 n;          // shading normal
    float3 n_g;        // geometric normal
    float3 tangent;    // dP/du; zero: none
    float bitangent_sign;
    float u;
    float v;
    bool front_face;
    float t;
    uint instance;     // placement id
    uint primitive;
    RayFootprint ray_footprint;
    TextureFootprint texture_footprint;
    float3 dpdx;
    float3 dpdy;
};

struct BsdfSample {
    float3 wi;
    float3 color;      // throughput weight: bsdf * |cos| / pdf
    float3 emission;
    bool scattered;
    float pdf;
    bool delta;
    bool null_event;   // cutout crossing: continue unchanged
};

struct BsdfEvaluation {
    float3 value;      // bsdf * |cos|
    float pdf;
    bool valid;
};

struct EnvironmentLightSample {
    float3 wi;
    float3 radiance;
    float pdf;
};

inline RayFootprint emptyFootprint()
{
    const RayFootprint f = {(float3)0.0f, (float3)0.0f, (float3)0.0f, (float3)0.0f, 0.0f, 0.0f, false};
    return f;
}

// initialize_ray: transport epsilon and an unbounded far distance.
inline Ray makeRay(float3 origin, float3 direction)
{
    const Ray r = {origin, direction, 0.001f, infinity()};
    return r;
}

inline float maxComponent(float3 v) { return max(v.x, max(v.y, v.z)); }
inline bool isZero(float3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }
