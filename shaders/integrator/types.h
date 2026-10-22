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

// Owen-scrambled Sobol points (Burley 2020, "Practical Hash-based Owen
// Scrambling"): the integrator draws 4-D points, each group of four
// dimensions with its own index shuffle and scrambles, from a per-pixel seed.
static const uint kSobolDirections[4][32] = {
    {0x80000000u, 0x40000000u, 0x20000000u, 0x10000000u, 0x08000000u, 0x04000000u, 0x02000000u, 0x01000000u, 0x00800000u, 0x00400000u, 0x00200000u, 0x00100000u, 0x00080000u, 0x00040000u, 0x00020000u, 0x00010000u, 0x00008000u, 0x00004000u, 0x00002000u, 0x00001000u, 0x00000800u, 0x00000400u, 0x00000200u, 0x00000100u, 0x00000080u, 0x00000040u, 0x00000020u, 0x00000010u, 0x00000008u, 0x00000004u, 0x00000002u, 0x00000001u},
    {0x80000000u, 0xc0000000u, 0xa0000000u, 0xf0000000u, 0x88000000u, 0xcc000000u, 0xaa000000u, 0xff000000u, 0x80800000u, 0xc0c00000u, 0xa0a00000u, 0xf0f00000u, 0x88880000u, 0xcccc0000u, 0xaaaa0000u, 0xffff0000u, 0x80008000u, 0xc000c000u, 0xa000a000u, 0xf000f000u, 0x88008800u, 0xcc00cc00u, 0xaa00aa00u, 0xff00ff00u, 0x80808080u, 0xc0c0c0c0u, 0xa0a0a0a0u, 0xf0f0f0f0u, 0x88888888u, 0xccccccccu, 0xaaaaaaaau, 0xffffffffu},
    {0x80000000u, 0xc0000000u, 0x60000000u, 0x90000000u, 0xe8000000u, 0x5c000000u, 0x8e000000u, 0xc5000000u, 0x68800000u, 0x9cc00000u, 0xee600000u, 0x55900000u, 0x80680000u, 0xc09c0000u, 0x60ee0000u, 0x90550000u, 0xe8808000u, 0x5cc0c000u, 0x8e606000u, 0xc5909000u, 0x6868e800u, 0x9c9c5c00u, 0xeeee8e00u, 0x5555c500u, 0x8000e880u, 0xc0005cc0u, 0x60008e60u, 0x9000c590u, 0xe8006868u, 0x5c009c9cu, 0x8e00eeeeu, 0xc5005555u},
    {0x80000000u, 0xc0000000u, 0x20000000u, 0x50000000u, 0xf8000000u, 0x74000000u, 0xa2000000u, 0x93000000u, 0xd8800000u, 0x25400000u, 0x59e00000u, 0xe6d00000u, 0x78080000u, 0xb40c0000u, 0x82020000u, 0xc3050000u, 0x208f8000u, 0x51474000u, 0xfbea2000u, 0x75d93000u, 0xa0858800u, 0x914e5400u, 0xdbe79e00u, 0x25db6d00u, 0x58800080u, 0xe54000c0u, 0x79e00020u, 0xb6d00050u, 0x800800f8u, 0xc00c0074u, 0x200200a2u, 0x50050093u}
};

inline uint reverseBits(uint x)
{
    x = ((x >> 1u) & 0x55555555u) | ((x & 0x55555555u) << 1u);
    x = ((x >> 2u) & 0x33333333u) | ((x & 0x33333333u) << 2u);
    x = ((x >> 4u) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4u);
    x = ((x >> 8u) & 0x00FF00FFu) | ((x & 0x00FF00FFu) << 8u);
    return (x >> 16u) | (x << 16u);
}

inline uint hashUint(uint x)
{
    x ^= x >> 16u;
    x *= 0x21f0aaadu;
    x ^= x >> 15u;
    x *= 0xd35a2d97u;
    x ^= x >> 15u;
    return x;
}

inline uint hashCombine(uint seed, uint v) { return seed ^ (v + (seed << 6u) + (seed >> 2u)); }

inline uint nestedUniformScramble(uint x, uint seed)
{
    x = reverseBits(x);
    x += seed;
    x ^= x * 0x6c50b47cu;
    x ^= x * 0xb82f1e52u;
    x ^= x * 0xc7afe638u;
    x ^= x * 0x8d22f6e6u;
    return reverseBits(x);
}

// The first four Sobol dimensions of one index, in one pass over its bits
// (dimension 0 is the bit reversal of the index).
struct Sobol4 {
    uint x;
    uint y;
    uint z;
    uint w;
};

inline Sobol4 sobol4(uint index)
{
    Sobol4 r = {reverseBits(index), 0u, 0u, 0u};
    for (uint bit = 0u; index != 0u; ++bit, index >>= 1u) {
        if ((index & 1u) != 0u) {
            r.y ^= kSobolDirections[1][bit];
            r.z ^= kSobolDirections[2][bit];
            r.w ^= kSobolDirections[3][bit];
        }
    }
    return r;
}

// The top 24 bits as a number in (0, 1): 0 becomes the midpoint of its
// stratum, 2^-25. MDL's layered BSDFs select a lobe of zero weight for xi
// exactly 0 and divide by its probability (one sample in about 2^24 then
// weighs ~1e8). Every other value stays on the 24-bit grid, which the
// environment's alias sampling relies on (it rebuilds 24-bit integers).
inline float uintToUnit(uint x) { return max(float(x >> 8u), 0.5f) / 16777216.0f; }

// Sobol points pay off in the first dimensions (pixel, lens, first bounce);
// later groups use cheaper hashed random points.
static const uint kSobolGroups = 4u;

struct Sampler {
    uint index;      // sample number of the pixel
    uint seed;       // per pixel
    uint dimension;  // next 4-D group

    float4 next4()
    {
        const uint groupSeed = hashCombine(seed, hashUint(dimension));
        dimension += 1u;
        if (dimension > kSobolGroups) {
            uint h = hashUint(hashCombine(groupSeed, hashUint(index)));
            const float x = uintToUnit(h);
            h = hashUint(h);
            const float y = uintToUnit(h);
            h = hashUint(h);
            const float z = uintToUnit(h);
            h = hashUint(h);
            return float4(x, y, z, uintToUnit(h));
        }
        const Sobol4 bits = sobol4(nestedUniformScramble(index, groupSeed));
        return float4(uintToUnit(nestedUniformScramble(bits.x, hashCombine(groupSeed, 0u))),
                      uintToUnit(nestedUniformScramble(bits.y, hashCombine(groupSeed, 1u))),
                      uintToUnit(nestedUniformScramble(bits.z, hashCombine(groupSeed, 2u))),
                      uintToUnit(nestedUniformScramble(bits.w, hashCombine(groupSeed, 3u))));
    }

    float next1() { return next4().x; }
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
    // Triangle barycentrics, and (GPU) the hit's vertices and prototype, for
    // named primvar lookups.
    float bary_u;
    float bary_v;
    uint vertex0;
    uint vertex1;
    uint vertex2;
    uint prototype;
};

struct BsdfSample {
    float3 wi;
    float3 color;      // throughput weight: bsdf * |cos| / pdf
    float3 emission;
    bool scattered;
    float pdf;
    bool delta;
    bool null_event;   // cutout crossing: continue unchanged
    bool transmitted;  // crossed the interface: enters or leaves the medium
};

// The homogeneous medium behind a surface, per scene unit.
struct MediumCoefficients {
    float3 sigmaA;
    float3 sigmaS;
    float g;           // Henyey-Greenstein anisotropy
    bool valid;
};

struct BsdfEvaluation {
    float3 value;      // bsdf * |cos|
    float pdf;
    bool valid;
    float3 diffuse;    // the diffuse lobes' part of value (the rest is glossy)
};

// One material invocation per surface vertex: the BSDF sample, the BSDF
// towards the light chosen for next-event estimation, and the medium behind
// the surface. Backends answer it with backend.shade (path_tracer.h), so a
// material runs once per bounce wherever it is compiled (an inlined call, a
// callable shader, or later a shading queue).
struct MaterialQuery {
    float4 xi;          // the BSDF's sample point
    float3 lightWi;     // the light direction of next-event estimation
    bool evaluateLight; // evaluate the BSDF towards lightWi
    bool lobes;         // also the diffuse fraction of the BSDF towards the sample
};

struct MaterialResult {
    BsdfSample sample;
    BsdfEvaluation light;      // valid false unless evaluated
    MediumCoefficients medium; // valid false unless the path enters it
    // With query.lobes: per channel, the part of the BSDF towards the sampled
    // direction that is diffuse (0 for delta and unevaluated samples).
    float3 sampleDiffuse;
};

// The diffuse part of an evaluation, per channel (0 where it is zero).
inline float3 diffuseFraction(BsdfEvaluation e)
{
    if (!e.valid) return (float3)0.0f;
    const float3 v = e.value;
    return float3(v.x > 0.0f ? clamp(e.diffuse.x / v.x, 0.0f, 1.0f) : 0.0f,
                  v.y > 0.0f ? clamp(e.diffuse.y / v.y, 0.0f, 1.0f) : 0.0f,
                  v.z > 0.0f ? clamp(e.diffuse.z / v.z, 0.0f, 1.0f) : 0.0f);
}

// Which parts of a MaterialQuery a backend computes after sampling: a cutout
// crossing (null event) neither connects to the light nor enters a medium.
inline bool evaluatesLight(MaterialQuery query, BsdfSample sample)
{
    return query.evaluateLight && !sample.null_event;
}
inline bool entersMedium(BsdfSample sample, bool frontFace)
{
    return sample.scattered && sample.transmitted && !sample.null_event && frontFace;
}

inline BsdfEvaluation invalidBsdfEvaluation()
{
    const BsdfEvaluation e = {(float3)0.0f, 0.0f, false, (float3)0.0f};
    return e;
}

inline MediumCoefficients noMedium()
{
    const MediumCoefficients m = {(float3)0.0f, (float3)0.0f, 0.0f, false};
    return m;
}

struct EnvironmentLightSample {
    float3 wi;
    float3 radiance;
    float pdf;
};

// The domes' radiance as diffuse and as glossy lobes see it: each dome's
// radiance times its UsdLux diffuse or specular multiplier.
struct EnvironmentLobes {
    float3 diffuse;
    float3 specular;
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
