// Types and helpers shared by the integrator and every device backend.
// Written in the Prism shading dialect (prism_sl.h). Before this file the
// backend defines MaterialHandle (an opaque material reference).

#ifndef PRISM_INTEGRATOR_TYPES_INC
#define PRISM_INTEGRATOR_TYPES_INC

// ---------------------------------------------------------------------------
// Random numbers: identical to rng.h, so every device draws the same stream.
// ---------------------------------------------------------------------------

PRISM_INLINE uint prism_tea64(uint val0, uint val1)
{
    uint v0 = val0;
    uint v1 = val1;
    uint s0 = 0u;
    for (uint n = 0u; n < 64u; n++) {
        s0 += 0x9e3779b9u;
        v0 += ((v1 << 4u) + 0xa341316cu) ^ (v1 + s0) ^ ((v1 >> 5u) + 0xc8013ea4u);
        v1 += ((v0 << 4u) + 0xad90777du) ^ (v0 + s0) ^ ((v0 >> 5u) + 0x7e95761eu);
    }
    return v0;
}

// Random float in [0, 1) from a 24-bit LCG.
PRISM_INLINE float prism_rnd(INOUT(uint) seed)
{
    seed = 1664525u * seed + 1013904223u;
    return float(seed & 0x00FFFFFFu) / 16777216.0f;
}

// ---------------------------------------------------------------------------
// Rays and hits
// ---------------------------------------------------------------------------

struct Ray {
    float3 origin;
    float3 direction;
    float tnear;
    float tfar;
};

// Ray differentials (see ray.h).
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

// A prepared surface hit (RayHit). Normals face the incoming ray.
struct Hit {
    bool hit;
    float3 p;
    float3 o;
    float3 d;
    float3 n;          // shading normal
    float3 n_g;        // geometric normal
    float3 n_s;        // normal-map sample (legacy materials)
    float3 tangent;    // dP/du; zero: none
    float bitangent_sign;
    float u;
    float v;
    bool front_face;
    float t;
    uint instance;     // placement id
    uint primitive;
    MaterialHandle material;
    RayFootprint ray_footprint;
    TextureFootprint texture_footprint;
    float3 dpdx;
    float3 dpdy;
};

struct BsdfSample {
    float3 wi;
    float3 color;      // throughput weight (bsdf * cos / pdf)
    float3 emission;
    bool scattered;
    float pdf;
    bool delta;
    bool null_event;
};

struct BsdfEvaluation {
    float3 value;      // bsdf * |cos|
    float pdf;
    bool valid;
};

struct EnvironmentSample {
    float3 wi;
    float3 radiance;
    float pdf;
};

PRISM_INLINE RayFootprint prism_empty_footprint()
{
    RayFootprint f;
    f.dOdx = splat3(0.0f);
    f.dOdy = splat3(0.0f);
    f.dDdx = splat3(0.0f);
    f.dDdy = splat3(0.0f);
    f.width = 0.0f;
    f.spread = 0.0f;
    f.exact = false;
    return f;
}

// initialize_ray: transport epsilon and an unbounded far distance.
PRISM_INLINE Ray prism_make_ray(float3 origin, float3 direction)
{
    Ray r;
    r.origin = origin;
    r.direction = direction;
    r.tnear = 0.001f;
    r.tfar = prism_infinity();
    return r;
}

PRISM_INLINE float prism_max_component(float3 v) { return max(v.x, max(v.y, v.z)); }

PRISM_INLINE bool prism_is_zero(float3 v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

// Outcome of a primary path (SceneManager::PrimaryOutcome).
#define PRISM_PRIMARY_UNRESOLVED 0u
#define PRISM_PRIMARY_ESCAPED 1u
#define PRISM_PRIMARY_ACCEPTED 2u

#endif // PRISM_INTEGRATOR_TYPES_INC
