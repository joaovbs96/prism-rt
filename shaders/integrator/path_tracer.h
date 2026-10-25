#pragma once

// The Prism path tracer, shared by every render device: compiled as C++ for
// the CPU device and as HLSL for the GPU devices (see types.h for the common
// subset of the two languages this file keeps to).
//
// PathTracer<Backend, Hit, Shading> traces with a device backend:
//
//   Hit      backend.intersect(PathRay)        prepared closest hit; hit.surface.valid
//   Shading  backend.prepare(Hit)              Material::perturbNormal + prepare
//   MaterialResult backend.shade(Shading, MaterialQuery)   the material, once per vertex:
//                                               sample: the BSDF sampled at query.xi (materials
//                                                 without a sample point draw from backend.rng)
//                                               light: the BSDF towards query.lightWi, when
//                                                 evaluatesLight(query, sample)
//                                               medium: the medium behind the front face, when
//                                                 entersMedium(sample, front face) (valid
//                                                 false: none, or thin-walled)
//   bool     backend.supportsNee(Shading)
//   bool     backend.provenOpaque(Hit)
//   float    backend.cutoutOpacity(Hit)
//   bool     backend.occludedByOpaque(PathRay)   any hit on always-opaque geometry
//                                                 (may conservatively return false)
//   bool     backend.mayHaveCutouts()             false: occludedByOpaque is exact
//   Hit      backend.intersectNonOpaque(PathRay)  closest hit on geometry that may
//                                                 transmit (may include opaque geometry)
//   float3   backend.environmentRadiance(float3 direction, float angularRadius)
//   EnvironmentLightSample backend.sampleEnvironment(float4 u, float angularRadius)
//   float    backend.environmentPdf(float3 direction)
//   uint     backend.lightCount()                 records of the light table (lights.h)
//   uint     backend.analyticLightCount()         the first records: analytic shapes
//   LightRecord backend.light(uint i)
//   float4   backend.lightAlias(uint i)           alias table over lightCount() + 1 entries
//                                                 (the last one: the dome)
//   float3   backend.lightTexture(uint texture, float2 st)
//   uint     backend.lightLinkMask(Hit)           categories of the hit for light linking
//   uint     backend.shadowLinkMask(Hit)          categories of the hit for shadow linking
//                                                 (both ~0u without linking)
//   float    backend.emitterPdfArea(Hit)          selection density per unit area of a hit
//                                                 emissive triangle (0: not a light)
//   bool     backend.varyingEmitters()            the light table may hold kLightVaryingEmission
//                                                 triangles (GPU: a compile-time constant, so
//                                                 kernels without them keep their code)
//   float3   backend.emitterRadiance(LightRecord, float2 barycentrics, PathRay ray, float distance)
//                                                 a kLightVaryingEmission triangle's emission at
//                                                 the point with these barycentrics, as `ray`
//                                                 reaching it at `distance` sees it (its
//                                                 footprint filters the textures), expected
//                                                 over cutouts
//   int      backend.maxDepth(), backend.rouletteDepth()
//   bool     backend.environmentMis()
//   bool     backend.lightLobes()             some light or dome has UsdLux diffuse or specular
//                                                 multipliers other than 1
//   EnvironmentLobes backend.environmentLobes(float3 direction, float angularRadius)
//   Rng      backend.rng                          white noise for materials and cutouts
//
// The integrator's own dimensions (pixel, lens, BSDF, light and roulette
// samples) come from the low-discrepancy `path.sampler`, set up per pixel and
// sample by the device; the decisions are those of the original recursive
// CPU integrator (since removed), in one loop.
//
// The path is explicit state (PathState) advanced by step functions in the
// order of the original single loop; see "The steps of a path" below.
//
// GPU compilers inline every call, and each material call expands to the code
// of every material in the scene: keep exactly one call site per backend
// material function (prepare, shade, cutoutOpacity).

#include "types.h"
#include "lights.h"

// Light AOVs (albedo, diffuse, specular, emission; LightAovSample):
// compiled in with PRISM_LIGHT_AOVS 1 (the CPU backend always, the GPU
// kernels in a variant built when a render asks for them) and gathered when
// PathTracer::aovs is set. Without them the path tracer is the code it was
// before (the GPU kernels compile at -O0: the same source structure keeps
// their SPIR-V, and so the driver's code, unchanged).
#ifndef PRISM_LIGHT_AOVS
#define PRISM_LIGHT_AOVS 0
#endif

// Light reaching the camera (PathTracer::gather).
#if PRISM_LIGHT_AOVS
#define PRISM_GATHER(c) gather(c)
#else
#define PRISM_GATHER(c) path.radiance += c
#endif

inline float powerHeuristic(float a, float b)
{
    const float scale = max(a, b);
    if (!(scale > 0.0f) || !isfinite(scale)) return 0.0f;
    a /= scale;
    b /= scale;
    const float aa = a * a;
    const float bb = b * b;
    return aa + bb > 0.0f ? aa / (aa + bb) : 0.0f;
}

// SceneManager::spawnRay
inline PathRay spawnRay(SurfaceHit hit, float3 direction, bool nullEvent)
{
    PathRay result;
    result.ray = makeRay(hit.p, direction);
    result.footprint = emptyFootprint();
    result.footprint.width = max(length(hit.dpdx), length(hit.dpdy));
    result.footprint.spread = hit.ray_footprint.spread;
    if (nullEvent && hit.ray_footprint.exact) {
        result.footprint = hit.ray_footprint;
        result.footprint.dOdx = hit.dpdx;
        result.footprint.dOdy = hit.dpdy;
        result.footprint.width = max(length(hit.dpdx), length(hit.dpdy));
    } else {
        result.footprint.exact = false;
    }
    return result;
}

// The angular footprint of a direction (environment lookups).
inline float angularRadius(RayFootprint footprint) { return max(0.0f, footprint.spread); }

// ---------------------------------------------------------------------------
// Camera rays (camera_rays.h), in single precision.
// ---------------------------------------------------------------------------

struct CameraParams {
    float4 inverseProjection[4]; // rows, row-vector convention
    float4 cameraToWorld[4];
    float nearClip;
    float farClip;
    float apertureRadius;
    float focusDistance;
    bool orthographic;
};

inline float4 transformRows(float4 p, float4 r0, float4 r1, float4 r2, float4 r3)
{
    return r0 * p.x + r1 * p.y + r2 * p.z + r3 * p.w;
}

inline bool hasLens(CameraParams camera)
{
    return !camera.orthographic && camera.apertureRadius > 0.0f && camera.focusDistance > 0.0f;
}

// MakeCameraRayWithLens; useLens false for the pinhole ray.
inline Ray cameraRay(CameraParams camera, float sx, float sy, uint width, uint height, bool useLens,
                     float2 lens)
{
    const float nx = 2.0f * sx / float(width) - 1.0f;
    const float ny = 1.0f - 2.0f * sy / float(height);
    float4 p = transformRows(float4(nx, ny, -1.0f, 1.0f), camera.inverseProjection[0],
                             camera.inverseProjection[1], camera.inverseProjection[2],
                             camera.inverseProjection[3]);
    if (abs(p.w) > 1e-12f) p = p / p.w;
    float3 originCamera = 0.0f;
    float3 directionCamera;
    if (camera.orthographic) {
        originCamera = float3(p.x, p.y, 0.0f);
        directionCamera = float3(0.0f, 0.0f, -1.0f);
    } else {
        directionCamera = normalize(float3(p.x, p.y, p.z));
    }
    if (useLens && hasLens(camera)) {
        const float focusT = camera.focusDistance / max(1e-8f, -directionCamera.z);
        const float3 focus = directionCamera * focusT;
        originCamera = float3(lens.x * camera.apertureRadius, lens.y * camera.apertureRadius, 0.0f);
        directionCamera = normalize(focus - originCamera);
    }
    const float4 o = transformRows(float4(originCamera, 1.0f), camera.cameraToWorld[0], camera.cameraToWorld[1],
                                   camera.cameraToWorld[2], camera.cameraToWorld[3]);
    const float4 d = transformRows(float4(directionCamera, 0.0f), camera.cameraToWorld[0], camera.cameraToWorld[1],
                                   camera.cameraToWorld[2], camera.cameraToWorld[3]);
    Ray ray = makeRay(float3(o.x, o.y, o.z), normalize(float3(d.x, d.y, d.z)));
    const float dz = max(1e-8f, -directionCamera.z);
    ray.tnear = max(0.0f, camera.nearClip) / dz;
    ray.tfar = camera.farClip / dz;
    return ray;
}

// Light contributions by the number of surface interactions before the
// light (noise analysis): environment or emission seen directly, light
// reaching the first hit, and everything that bounced more often.
static const uint kContributionVisible = 1u;
static const uint kContributionDirect = 2u;
static const uint kContributionIndirect = 4u;
static const uint kContributionAll = 7u;

inline float contributionWeight(uint contributions, int bounces)
{
    const uint kind = bounces == 0 ? kContributionVisible
                                   : (bounces == 1 ? kContributionDirect : kContributionIndirect);
    return (contributions & kind) != 0u ? 1.0f : 0.0f;
}

struct SampleSettings {
    float sampleClamp;      // 0 disables
    float exposureScale;
    bool transparentEscapes;
    uint contributions;     // kContribution* mask
    // Probabilistic NEE: from neeFromDepth on, NEE happens with probability
    // clamp(path weight, neeProbabilityMin, 1). 0 disables.
    float neeProbabilityMin;
    int neeFromDepth;
    // Spatial light selection (see PathTracer::pickLight); false: by power only.
    bool spatialLights;
    // Luminance weights of the working space (light selection).
    float3 luminance;
};

// Light tables with more entries pick between a column and its alias with
// the white noise (PathTracer::pickLight).
static const uint kPreciseLightPickEntries = 4096u;

// Spatial light selection loops over the analytic lights: at most this many.
static const uint kMaxSpatialLights = 64u;

// Sums over the analytic lights at one shading point.
struct AnalyticLightSums {
    float importance;  // of lightImportance, for lights linked to the receiver
    float probability; // of the lights' table (power) selection probabilities
};

#if PRISM_LIGHT_AOVS
// The light AOVs of one sample (docs/hydra.md, "Light AOVs"), scaled as its
// colour (coverage, sample clamp, exposure) except the albedo:
//   albedo    the BSDF sample weight (an estimate of the directional albedo)
//             at the first vertex that is neither a cutout crossing nor a
//             sharp bounce (kAlbedoSharpPdf), times the weights of the sharp
//             bounces and crossings before it; 0 if the path ends first
//   emission  light reaching the camera before any scattering vertex:
//             emission, lights and the dome seen directly or through cutouts
//   diffuse   light whose first scattering vertex scattered it by a diffuse
//             lobe (MDL's bsdf_diffuse part, which includes diffuse
//             transmission) or into a scattering medium (subsurface)
//   specular  the rest: glossy and delta reflection and transmission
// diffuse + specular + emission is the colour.
// The albedo AOV passes through BSDF samples that are delta or whose density
// is at least this (per steradian): MDL reports smooth glass and mirrors as
// glossy lobes of near-zero roughness. GGX reflection reaches it below a
// roughness of about 0.1 (Cycles' denoising albedo: 0.075).
static const float kAlbedoSharpPdf = 1000.0f;

struct LightAovSample {
    float3 albedo;
    float3 diffuse;
    float3 specular;
    float3 emission;
};
#endif

struct SampleResult {
    float3 color;
    float coverage;
#if PRISM_LIGHT_AOVS
    LightAovSample aovs;
#endif
};

// Initial per-pixel random states of the render loops.
inline uint seedImage(uint x, uint y, uint width, uint seed) { return tea64((y * width) ^ seed, x); }
inline uint seedPass(uint x, uint y, uint width, uint seed) { return tea64((y * width) ^ seed, x); }
inline uint seedPreview(uint px, uint py, uint previewWidth, uint seed)
{
    return tea64(((py * previewWidth + px) ^ seed) ^ 0x9e3779b9u, (px + 0x85ebca6bu) ^ (py * 0xc2b2ae35u));
}

// Henyey-Greenstein phase function: a direction around the forward
// direction `w` (the direction of travel), sampled exactly (weight one).
inline float3 sampleHenyeyGreenstein(float3 w, float g, float2 u)
{
    float cosTheta;
    if (abs(g) < 1e-3f) {
        cosTheta = 1.0f - 2.0f * u.x;
    } else {
        const float square = (1.0f - g * g) / (1.0f + g - 2.0f * g * u.x);
        cosTheta = (1.0f + g * g - square * square) / (2.0f * g);
    }
    cosTheta = clamp(cosTheta, -1.0f, 1.0f);
    const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * kPi * u.y;
    const Frame f = frameAround(w);
    return normalize(f.t * (sinTheta * cos(phi)) + f.b * (sinTheta * sin(phi)) + w * cosTheta);
}

inline float average(float3 v) { return (v.x + v.y + v.z) / 3.0f; }

inline float component(float3 v, uint i) { return i == 0u ? v.x : (i == 1u ? v.y : v.z); }

// Random walks end after this many scattering events.
static const int kMaxVolumeEvents = 1024;

struct TraceResult {
    float3 radiance;
    uint outcome;      // kPrimary*
};

// Everything a path carries from one bounce to the next. The step functions
// of PathTracer read and update it; a wavefront integrator would keep it in
// memory between its kernels. (The white-noise state the materials draw from
// is backend.rng; it would travel with this state.)
struct PathState {
    PathRay ray;               // the next segment to trace
    Sampler sampler;           // the path's low-discrepancy dimensions
    float3 radiance;           // gathered so far
    float3 weight;             // product of the scattering weights so far
    float3 throughput;         // roulette state, as in the recursive form
    // The last scattering vertex, for MIS against next-event estimation of
    // the light the next segment may hit.
    float3 previousPosition;
    float previousPdf;         // BSDF density of the segment's direction
    // With light lobes: per channel, the diffuse part of the BSDF that sampled
    // the segment (MaterialResult::sampleDiffuse); 0 for camera rays.
    float3 previousDiffuse;
    float previousNeeProbability;
    bool previousNee;          // that vertex sampled lights: weight hits with MIS
    uint receiver;             // light-link categories of the last surface
    int depth;                 // scattering vertices so far
    int nullCrossings;         // cutout crossings in a row (environment MIS)
    int volumeEvents;          // scattering events inside media
    // The medium the path travels in (entered through a front face), traced
    // as a random walk with spectral (hero channel) MIS.
    bool inMedium;
    MediumCoefficients medium;
    uint outcome;              // kPrimary*
    bool tracking;             // primary outcome still unresolved
    bool active;               // false: the path has ended
#if PRISM_LIGHT_AOVS
    // Light AOVs (PathTracer::aovs; LightAovSample).
    LightAovSample aov;
    bool aovPrimary;           // no scattering vertex yet (cutout crossings do not count)
    float3 firstDiffuse;       // per channel: the diffuse part of the first vertex's continuation
    bool albedoOpen;           // the albedo's vertex is still ahead
    float3 albedoWeight;       // delta, crossing and roulette weights before it
#endif
};

// Next-event estimation at one surface vertex: the light sample, drawn
// before the material runs so that one material invocation (backend.shade)
// both samples the BSDF and evaluates it towards the light.
struct LightConnection {
    float3 wi;
    float3 radiance;           // with light lobes: as diffuse lobes see it
    float3 radianceSpecular;   // with light lobes: as glossy lobes see it
    float distance;            // infinity: the dome or a distant light
    float pdf;                 // neeProbability * selection * light density
    uint shadowBit;            // shadow-link category of the light (0: all)
    bool delta;
    bool valid;                // the BSDF is evaluated and a shadow ray traced
};

// What the steps of one surface bounce share.
struct SurfaceVertex {
    uint receiver;             // light-link categories of the surface
    bool canNee;
    float neeProbability;      // next-event estimation is a strategy with this weight
    // The sampler position after the BSDF's sample point: a cutout crossing
    // uses no light sample, so it rewinds to here.
    uint samplerDimension;
    LightConnection light;
};

template <typename Backend, typename Hit, typename Shading>
struct PathTracer {
    Backend backend;
    PathState path;
    // Constant over a path (set by beginPath).
    SampleSettings settings;
    bool mis;                  // environment MIS and next-event estimation
    int maxDepth;
    int rouletteDepth;
    bool spatial;              // spatial light selection
    bool lobes;                // UsdLux diffuse / specular light multipliers (backend.lightLobes)
#if PRISM_LIGHT_AOVS
    bool aovs;                 // gather the light AOVs (set by the caller; beginPath keeps it)
#endif

    // Stochastic transmission through cutouts.
    // Opaque geometry anywhere on the ray blocks it whatever cutouts lie in
    // front, so a cheap any-hit query on opaque geometry comes first.
    // Shadow rays end at shadow.ray.tfar (lights at a finite distance).
    // Surfaces outside a light's shadow-link category (shadowBit, 0: all
    // surfaces) let its light through.
    bool visible(PathRay shadow, uint shadowBit)
    {
        if (backend.occludedByOpaque(shadow)) return false;
        if (!backend.mayHaveCutouts()) return true;
        const float3 direction = shadow.ray.direction;
        float remaining = shadow.ray.tfar;
        for (int crossings = 0; crossings < 64; ++crossings) {
            const Hit hit = backend.intersectNonOpaque(shadow);
            if (!hit.surface.valid) return true;
            const bool casts = shadowBit == 0u || (backend.shadowLinkMask(hit) & shadowBit) != 0u;
            if (casts) {
                if (backend.provenOpaque(hit)) return false;
                const float opacity = saturate(backend.cutoutOpacity(hit));
                if (opacity >= 1.0f || (opacity > 0.0f && backend.rng.next() < opacity)) return false;
            }
            remaining -= hit.surface.t;
            shadow = spawnRay(hit.surface, direction, true);
            shadow.ray.tfar = remaining;
            if (!(remaining > shadow.ray.tnear)) return true;
        }
        return false;
    }

    // The light table entry for the selection number u (lightCount() > 0).
    // Small tables take the column and the choice between it and its alias
    // from u alone. Sample numbers lie on a 24-bit grid, so above
    // kPreciseLightPickEntries entries u reaches each column only a few
    // times (18 at a million entries) and the fraction left in it is that
    // coarse: entries came out with other probabilities than the table's,
    // which MIS assumes (the OpenPBR playground's dome lit its desk 8% too
    // brightly). Large tables draw the column (uniformly, by rejection) and
    // the choice from the white noise instead. Another dimension of the same
    // sample point is no good: at the column's scale it is nearly a function
    // of u.
    LightPick pickLight(float u)
    {
        const uint entries = backend.lightCount() + 1u;
        uint column;
        float choice;
        if (entries > kPreciseLightPickEntries) {
            const uint limit = (16777216u / entries) * entries;
            uint bits = 0u;
            for (int attempt = 0; attempt < 8; ++attempt) {
                bits = uint(backend.rng.next() * 16777216.0f);
                if (bits < limit) break;
            }
            column = bits % entries;
            choice = backend.rng.next();
        } else {
            const float scaled = u * float(entries);
            column = min(uint(scaled), entries - 1u);
            choice = scaled - float(column);
        }
        const float4 entry = backend.lightAlias(column);
        LightPick pick;
        pick.index = choice < entry.x ? column : asuint(entry.y);
        pick.pmf = backend.lightAlias(pick.index).z;
        return pick;
    }

    // Spatial light selection, when enabled and the scene has 2 to
    // kMaxSpatialLights analytic lights: an analytic light picked by the
    // alias table is picked again among the analytic lights in proportion to
    // lightImportance at the shading point (lights it cannot reach are never
    // picked). The analytic lights keep their total probability; emissive
    // triangles and the dome keep theirs. A mixture with power selection was
    // measured worse: the importance is zero only where a light cannot reach.
    bool spatialSelection(bool enabled)
    {
        const uint count = backend.analyticLightCount();
        return enabled && count > 1u && count <= kMaxSpatialLights;
    }

    float linkedImportance(LightRecord light, float3 p, uint receiver)
    {
        if (light.info.w != 0u && (receiver & light.info.w) == 0u) return 0.0f;
        return lightImportance(light, p, settings.luminance);
    }

    AnalyticLightSums analyticSums(float3 p, uint receiver)
    {
        AnalyticLightSums sums = {0.0f, 0.0f};
        const uint count = backend.analyticLightCount();
        for (uint i = 0u; i < count; ++i) {
            const LightRecord light = backend.light(i);
            sums.importance += linkedImportance(light, p, receiver);
            sums.probability += light.n.w;
        }
        return sums;
    }

    // Selection probability of an analytic light at p (spatial selection).
    float analyticPmf(LightRecord light, float3 p, uint receiver, AnalyticLightSums sums)
    {
        if (!(sums.importance > 0.0f)) return light.n.w;
        return sums.probability * linkedImportance(light, p, receiver) / sums.importance;
    }

    uint pickAnalytic(float3 p, uint receiver, AnalyticLightSums sums, float u)
    {
        const uint count = backend.analyticLightCount();
        float remaining = u * sums.importance;
        uint chosen = 0u;
        for (uint i = 0u; i < count; ++i) {
            const float w = linkedImportance(backend.light(i), p, receiver);
            if (w > 0.0f) {
                chosen = i;
                if (remaining < w) break;
                remaining -= w;
            }
        }
        return chosen;
    }

    float domeSelectionProbability()
    {
        const uint count = backend.lightCount();
        return count == 0u ? 1.0f : backend.lightAlias(count).z;
    }

    // Emission of the analytic lights a ray segment passes through, and of
    // distant lights for escaped rays (tmax infinite), weighted against NEE.
    float3 crossedLights(PathRay ray, float tmax, bool escaped, bool cameraRay, bool weighted,
                         float bsdfPdf, float neeProbability, uint receiver, float3 from, bool spatial)
    {
        float3 sum = (float3)0.0f;
        const uint count = backend.analyticLightCount();
        AnalyticLightSums sums = {0.0f, 0.0f};
        if (weighted && spatial) sums = analyticSums(from, receiver);
        for (uint i = 0u; i < count; ++i) {
            const LightRecord light = backend.light(i);
            if (cameraRay && (light.info.y & kLightCameraVisible) == 0u) continue;
            // Light linking: only the surfaces of its category see the light.
            if (!cameraRay && light.info.w != 0u && (receiver & light.info.w) == 0u) continue;
            LightHit crossing;
            if (light.info.x == kLightDistant) {
                if (!escaped) continue;
                crossing = escapedDistantLight(light, ray.ray.direction);
            } else {
                const LightHitTextured textured = intersectLight(light, ray.ray, tmax, from);
                crossing = textured.hit;
                if (crossing.hit && (light.info.y & kLightTextured) != 0u)
                    crossing.radiance *= backend.lightTexture(light.info.z, textured.texcoord);
            }
            if (!crossing.hit) continue;
            float misWeight = 1.0f;
            if (weighted) {
                const float pmf = spatial ? analyticPmf(light, from, receiver, sums) : light.n.w;
                misWeight = powerHeuristic(bsdfPdf, neeProbability * pmf * crossing.pdf);
            }
            if (lobes && !cameraRay)
                sum += crossing.radiance * lobeScale(light.lobes.x, light.lobes.y) * misWeight;
            else
                sum += crossing.radiance * misWeight;
        }
        return sum;
    }

    // UsdLux diffuse (kd) and specular (ks) light multipliers for a light the
    // path's last BSDF sample reached: the BSDF's diffuse part sees kd, the
    // rest ks (with light lobes).
    float3 lobeScale(float kd, float ks) { return (float3)ks + (kd - ks) * path.previousDiffuse; }

#if PRISM_LIGHT_AOVS
    // Light reaching the camera along the path (weighted; PRISM_GATHER): the
    // colour, and with light AOVs the emission before the first scattering
    // vertex, after it diffuse and specular by that vertex's lobes.
    void gather(float3 c)
    {
        path.radiance += c;
        if (aovs) {
            if (path.aovPrimary) {
                path.aov.emission += c;
            } else {
                const float3 d = c * path.firstDiffuse;
                path.aov.diffuse += d;
                path.aov.specular += c - d;
            }
        }
    }

    // Next-event estimation's contribution c at a vertex; at the first one
    // diffuse is the BSDF's own diffuse part (as diffuse lobes see the light,
    // with light lobes).
    void gatherDirect(float3 c, float3 firstVertexDiffuse)
    {
        path.radiance += c;
        if (!aovs) return;
        const float3 d = path.aovPrimary ? min(firstVertexDiffuse, c) : c * path.firstDiffuse;
        path.aov.diffuse += d;
        path.aov.specular += c - d;
    }

    // The albedo AOV at a surface vertex (LightAovSample): sharp bounces and
    // cutout crossings hand it on to the next vertex.
    void resolveAlbedo(BsdfSample bs)
    {
        if (bs.null_event || (bs.scattered && (bs.delta || bs.pdf >= kAlbedoSharpPdf))) {
            path.albedoWeight *= bs.color;
            return;
        }
        path.albedoOpen = false;
        if (bs.scattered) path.aov.albedo = path.albedoWeight * bs.color;
    }
#endif

    // -----------------------------------------------------------------------
    // The steps of a path. A bounce is extend (trace the segment, media,
    // lights it crosses or the environment it escapes to), then at a surface
    // chooseLight (the NEE light sample), backend.shade (the material,
    // once), arrive (emission and cutout crossings), connectLight (the shadow
    // ray) and continuePath (roulette and the next segment). Random numbers
    // are drawn in the order of the original single loop.
    //
    // With environment MIS: BSDF sampling and next-event estimation with the
    // power heuristic; cutout null events do not count as bounces (at most 64
    // in a row). Without: BSDF sampling only, and null events are bounces.
    // Both use Russian roulette from the roulette depth on.
    // -----------------------------------------------------------------------

    void beginPath(PathRay ray, SampleSettings sampleSettings)
    {
        settings = sampleSettings;
        mis = backend.environmentMis();
        lobes = backend.lightLobes();
        maxDepth = backend.maxDepth();
        rouletteDepth = backend.rouletteDepth();
        spatial = spatialSelection(settings.spatialLights);
        path.ray = ray;
        path.radiance = (float3)0.0f;
        path.weight = (float3)1.0f;
        path.throughput = (float3)1.0f;
        path.previousPosition = ray.ray.origin;
        path.previousPdf = 0.0f;
        path.previousNeeProbability = 1.0f;
        path.previousNee = false;
        path.previousDiffuse = (float3)0.0f;
        path.receiver = ~0u;
        path.depth = 0;
        path.nullCrossings = 0;
        path.volumeEvents = 0;
        path.inMedium = false;
        path.medium = noMedium();
        path.outcome = kPrimaryUnresolved;
        path.tracking = true;
        path.active = true;
#if PRISM_LIGHT_AOVS
        path.aov.albedo = (float3)0.0f;
        path.aov.diffuse = (float3)0.0f;
        path.aov.specular = (float3)0.0f;
        path.aov.emission = (float3)0.0f;
        path.aovPrimary = true;
        path.firstDiffuse = (float3)0.0f;
        path.albedoOpen = true;
        path.albedoWeight = (float3)1.0f;
#endif
    }

    // A scattering event inside the medium, scatterDistance along the ray.
    void scatterInMedium(float scatterDistance, float3 sigmaT)
    {
        const float3 transmittance = exp(-sigmaT * scatterDistance);
        const float pdf = average(sigmaT * transmittance);
        if (!(pdf > 0.0f)) {
            path.active = false;
            return;
        }
        const float3 factor = path.medium.sigmaS * transmittance / pdf;
        path.weight *= factor;
        path.throughput *= factor;
        if (++path.volumeEvents > kMaxVolumeEvents) {
            path.active = false;
            return;
        }
        if (path.volumeEvents > 8) {
            const float q = clamp(maxComponent(path.throughput), 0.05f, 0.95f);
            if (path.sampler.next1() >= q) {
                path.active = false;
                return;
            }
            path.weight *= 1.0f / q;
            path.throughput *= 1.0f / q;
        }
        const float3 position = path.ray.ray.origin + path.ray.ray.direction * scatterDistance;
        const float4 u = path.sampler.next4();
        const float3 direction = sampleHenyeyGreenstein(path.ray.ray.direction, path.medium.g, float2(u.x, u.y));
        const RayFootprint footprint = path.ray.footprint;
        path.ray.ray = makeRay(position, direction);
        path.ray.ray.tnear = 0.0f;
        path.ray.footprint = footprint;
        path.ray.footprint.exact = false;
        path.previousNee = false;
        path.previousDiffuse = (float3)1.0f; // lights see scattering in media as diffuse
    }

    // Traces the path's next segment. Returns the surface the path goes on
    // at (surface.valid), or an invalid hit when the path scattered inside a
    // medium (still active) or ended (escaped, absorbed or lost).
    Hit extend()
    {
        float scatterDistance = infinity();
        const bool inMedium = path.inMedium;
        if (inMedium) {
            path.ray.ray.direction = normalize(path.ray.ray.direction);
            if (maxComponent(path.medium.sigmaS) > 0.0f) {
                const float4 u = path.sampler.next4();
                const float sigmaT =
                    component(path.medium.sigmaA + path.medium.sigmaS, min(uint(u.x * 3.0f), 2u));
                if (sigmaT > 0.0f) scatterDistance = -log(1.0f - u.y) / sigmaT;
                path.ray.ray.tfar = min(path.ray.ray.tfar, scatterDistance);
            }
        }
        Hit hit = backend.intersect(path.ray);
        if (inMedium) {
            const float3 sigmaT = path.medium.sigmaA + path.medium.sigmaS;
            if (!hit.surface.valid && !isfinite(scatterDistance)) { // open surface: lost inside
                path.active = false;
                return hit;
            }
            if (!hit.surface.valid) {
                scatterInMedium(scatterDistance, sigmaT);
                return hit;
            }
            // Reached the boundary (or another surface) without scattering.
            const float3 transmittance = exp(-sigmaT * hit.surface.t);
            if (maxComponent(path.medium.sigmaS) > 0.0f) {
                const float pdf = average(transmittance);
                if (!(pdf > 0.0f)) {
                    path.active = false;
                    hit.surface.valid = false;
                    return hit;
                }
                path.weight *= transmittance / pdf;
                path.throughput *= transmittance / pdf;
            } else {
                path.weight *= transmittance;
                path.throughput *= transmittance;
            }
        }
        const bool weighted = mis && path.previousNee;
        // A camera ray that sees a light visible to the camera covers its pixel.
        bool sawLight = false;
        if (!inMedium && backend.analyticLightCount() > 0u) {
            const float3 crossed = crossedLights(path.ray, hit.surface.valid ? hit.surface.t : path.ray.ray.tfar,
                                                 !hit.surface.valid, path.depth == 0, weighted, path.previousPdf,
                                                 path.previousNeeProbability, path.receiver, path.previousPosition,
                                                 spatial);
            PRISM_GATHER(path.weight * crossed * contributionWeight(settings.contributions, path.depth));
            sawLight = path.depth == 0 && maxComponent(crossed) > 0.0f;
        }
        if (!hit.surface.valid) {
            if (path.tracking) path.outcome = sawLight ? kPrimaryAccepted : kPrimaryEscaped;
            const float3 direction = normalize(path.ray.ray.direction);
            float3 li;
            if (lobes && path.depth > 0) {
                const EnvironmentLobes l = backend.environmentLobes(direction, angularRadius(path.ray.footprint));
                li = l.specular + (l.diffuse - l.specular) * path.previousDiffuse;
            } else {
                li = backend.environmentRadiance(direction, angularRadius(path.ray.footprint));
            }
            float misWeight = 1.0f;
            if (weighted)
                misWeight = powerHeuristic(path.previousPdf, path.previousNeeProbability * domeSelectionProbability() *
                                                                 backend.environmentPdf(direction));
            PRISM_GATHER(path.weight * li * (misWeight * contributionWeight(settings.contributions, path.depth)));
            path.active = false;
        }
        return hit;
    }

    // Chooses the light sample of next-event estimation at a surface. Needs
    // nothing from the material but whether it supports NEE (canNee).
    SurfaceVertex chooseLight(Hit hit, bool canNee)
    {
        SurfaceVertex vertex;
        vertex.receiver = backend.lightLinkMask(hit);
        vertex.samplerDimension = path.sampler.dimension;
        vertex.light.wi = (float3)0.0f;
        vertex.light.radiance = (float3)0.0f;
        vertex.light.radianceSpecular = (float3)0.0f;
        vertex.light.distance = infinity();
        vertex.light.pdf = 0.0f;
        vertex.light.shadowBit = 0u;
        vertex.light.delta = false;
        vertex.light.valid = false;
        vertex.canNee = canNee;
        // Light sampling is a strategy with density neeProbability * light pdf.
        vertex.neeProbability = 1.0f;
        if (vertex.canNee && settings.neeProbabilityMin > 0.0f && path.depth >= settings.neeFromDepth)
            vertex.neeProbability = clamp(maxComponent(path.weight), settings.neeProbabilityMin, 1.0f);
        const bool doNee =
            vertex.canNee && (vertex.neeProbability >= 1.0f || path.sampler.next1() < vertex.neeProbability);
        if (!doNee) return vertex;
        // Without other lights the dome is the only entry (no selection number).
        LightPick pick;
        pick.index = 0u;
        pick.pmf = 1.0f;
        const uint lightCount = backend.lightCount();
        if (lightCount > 0u) {
            const float4 u = path.sampler.next4();
            pick = pickLight(u.x);
            if (spatial && pick.index < backend.analyticLightCount()) {
                const AnalyticLightSums sums = analyticSums(hit.surface.p, vertex.receiver);
                if (sums.importance > 0.0f) pick.index = pickAnalytic(hit.surface.p, vertex.receiver, sums, u.y);
                pick.pmf = analyticPmf(backend.light(pick.index), hit.surface.p, vertex.receiver, sums);
            }
        }
        LightSample light = invalidLightSample();
        // With light lobes: the light as glossy lobes see it (light.radiance:
        // as diffuse lobes see it).
        float3 specular = (float3)0.0f;
        if (pick.index >= lightCount) {
            const EnvironmentLightSample dome =
                backend.sampleEnvironment(path.sampler.next4(), angularRadius(hit.surface.ray_footprint));
            light.wi = dome.wi;
            light.radiance = dome.radiance;
            light.pdf = dome.pdf;
            light.distance = infinity();
            light.delta = false;
            light.valid = dome.pdf > 0.0f && isfinite(dome.pdf);
            if (lobes && light.valid) {
                const EnvironmentLobes l =
                    backend.environmentLobes(dome.wi, angularRadius(hit.surface.ray_footprint));
                light.radiance = l.diffuse;
                specular = l.specular;
            }
        } else {
            const LightRecord record = backend.light(pick.index);
            const float4 u = path.sampler.next4();
            const LightSampleTextured sampled = sampleLight(record, hit.surface.p, float2(u.x, u.y));
            light = sampled.sample;
            vertex.light.shadowBit = shadowLinkBit(record);
            if (record.info.w != 0u && (vertex.receiver & record.info.w) == 0u) light.valid = false;
            // Varying emission: the material's emission at the sampled point,
            // looked up as a BSDF ray along wi would (with its footprint): MIS
            // needs both strategies to see the same filtered textures.
            if (backend.varyingEmitters() && light.valid && (record.info.y & kLightVaryingEmission) != 0u) {
                light.radiance = backend.emitterRadiance(record, primitiveBarycentrics(record, sampled.texcoord),
                                                         spawnRay(hit.surface, light.wi, false), light.distance);
                light.valid = maxComponent(light.radiance) > 0.0f;
            }
            if (light.valid && (record.info.y & kLightTextured) != 0u)
                light.radiance *= backend.lightTexture(record.info.z, sampled.texcoord);
            if (lobes) {
                specular = light.radiance * record.lobes.y;
                light.radiance *= record.lobes.x;
            }
        }
        vertex.light.valid = light.valid && pick.pmf > 0.0f;
        vertex.light.wi = light.wi;
        vertex.light.radiance = light.radiance;
        vertex.light.radianceSpecular = specular;
        vertex.light.distance = light.distance;
        vertex.light.delta = light.delta;
        vertex.light.pdf = vertex.neeProbability * pick.pmf * light.pdf;
        return vertex;
    }

    // Emission at the surface and cutout crossings. False: the bounce ends
    // here (the path crossed a cutout or ended).
    bool arrive(Hit hit, SurfaceVertex vertex, BsdfSample bs)
    {
        const bool weighted = mis && path.previousNee;
        float emissionWeight = 1.0f;
        if (weighted && !isZero(bs.emission)) {
            // Emissive triangles are also sampled by NEE: area to solid angle.
            const float pdfArea = backend.emitterPdfArea(hit);
            const float cosLight = abs(dot(hit.surface.n_g, normalize(path.ray.ray.direction)));
            const float distance = length(hit.surface.p - path.previousPosition);
            if (pdfArea > 0.0f && cosLight > 0.0f)
                emissionWeight = powerHeuristic(path.previousPdf, path.previousNeeProbability * pdfArea *
                                                                      distance * distance / cosLight);
        }
        PRISM_GATHER(path.weight * bs.emission *
                     (emissionWeight * contributionWeight(settings.contributions, path.depth)));
        // A cutout hole crossed from the inside leaves the object, and with it
        // the medium entered by refraction; staying in it absorbed the rest of
        // the path and lost every path that then escaped to the environment.
        if (bs.null_event && !hit.surface.front_face) path.inMedium = false;
        if (mis && bs.null_event) {
            path.sampler.dimension = vertex.samplerDimension; // the light sample goes unused
            if (!bs.scattered) path.tracking = false;
            if (path.nullCrossings >= 64) {
                path.active = false;
                return false;
            }
            path.weight *= bs.color;
            path.throughput *= bs.color;
            path.ray = spawnRay(hit.surface, bs.wi, true);
            ++path.nullCrossings;
            return false;
        }
        if (!bs.null_event && path.tracking) {
            path.outcome = kPrimaryAccepted;
            path.tracking = false;
        }
        return true;
    }

    // Next-event estimation: the light sample weighted against BSDF
    // sampling, if the shadow ray reaches the light.
    void connectLight(Hit hit, SurfaceVertex vertex, BsdfEvaluation f)
    {
        if (!vertex.light.valid) return;
        // The shadow ray is traced only for a nonzero contribution.
        if (!(f.valid && f.pdf >= 0.0f && isfinite(f.pdf))) return;
        const float lightPdf = vertex.light.pdf;
        const float misWeight = vertex.light.delta ? 1.0f : powerHeuristic(lightPdf, f.pdf);
        float3 contribution;
        if (lobes)
            contribution = path.weight *
                (f.diffuse * vertex.light.radiance + (f.value - f.diffuse) * vertex.light.radianceSpecular) *
                (misWeight / lightPdf);
        else
            contribution = path.weight * f.value * vertex.light.radiance * (misWeight / lightPdf);
        PathRay shadow = spawnRay(hit.surface, vertex.light.wi, false);
        if (isfinite(vertex.light.distance)) shadow.ray.tfar = vertex.light.distance * (1.0f - 1e-4f);
        if (maxComponent(contribution) > 0.0f && visible(shadow, vertex.light.shadowBit))
#if PRISM_LIGHT_AOVS
            gatherDirect(contribution * contributionWeight(settings.contributions, path.depth + 1),
                         path.weight * f.diffuse * vertex.light.radiance * (misWeight / lightPdf) *
                             contributionWeight(settings.contributions, path.depth + 1));
#else
            path.radiance += contribution * contributionWeight(settings.contributions, path.depth + 1);
#endif
    }

    // Absorption, media, Russian roulette and the next segment.
    void continuePath(Hit hit, SurfaceVertex vertex, MaterialResult material)
    {
        const BsdfSample bs = material.sample;
        if (!bs.scattered) {
            path.active = false;
            return;
        }
        if (bs.transmitted && !bs.null_event) {
            if (hit.surface.front_face) {
                path.medium = material.medium;
                path.inMedium = path.medium.valid && (maxComponent(path.medium.sigmaA) > 0.0f ||
                                                      maxComponent(path.medium.sigmaS) > 0.0f);
            } else {
                path.inMedium = false;
            }
        }
#if PRISM_LIGHT_AOVS
        if (aovs && path.aovPrimary && !bs.null_event) {
            // The first scattering vertex classifies the rest of the path: its
            // diffuse part, or all of it into a scattering medium (subsurface).
            path.aovPrimary = false;
            path.firstDiffuse = material.sampleDiffuse;
            if (path.inMedium && maxComponent(path.medium.sigmaS) > 0.0f) path.firstDiffuse = (float3)1.0f;
        }
#endif
        path.throughput *= bs.color;
        float rouletteWeight = 1.0f;
        if (path.depth >= rouletteDepth) {
            const float q = clamp(maxComponent(path.throughput), 0.05f, 0.95f);
            if (path.sampler.next1() >= q) {
                path.active = false;
                return;
            }
            rouletteWeight = 1.0f / q;
            path.throughput *= rouletteWeight;
#if PRISM_LIGHT_AOVS
            if (aovs && path.albedoOpen) path.albedoWeight *= rouletteWeight;
#endif
        }
        path.previousNee = vertex.canNee && !bs.delta && !bs.null_event && bs.pdf > 0.0f;
        path.receiver = vertex.receiver;
        path.previousPdf = bs.pdf;
        path.previousNeeProbability = vertex.neeProbability;
        path.previousPosition = hit.surface.p;
        path.previousDiffuse = material.sampleDiffuse;
        path.weight *= bs.color * rouletteWeight;
        path.ray = spawnRay(hit.surface, bs.wi, bs.null_event);
        ++path.depth;
        path.nullCrossings = 0;
    }

    // One bounce at a surface. The material runs once (backend.shade): it
    // samples the BSDF and evaluates it towards the light sample.
    void scatterAtSurface(Hit hit)
    {
        const Shading shading = backend.prepare(hit);
        MaterialQuery query;
        query.xi = path.sampler.next4();
        // At the depth boundary the competing continuation is truncated too.
        const bool canNee = mis && path.depth + 1 < maxDepth && backend.supportsNee(shading);
        const SurfaceVertex vertex = chooseLight(hit, canNee);
        query.lightWi = vertex.light.wi;
        query.evaluateLight = vertex.light.valid;
        query.lobes = lobes;
#if PRISM_LIGHT_AOVS
        // The first vertex's lobes split diffuse from specular.
        if (aovs && path.aovPrimary) query.lobes = true;
#endif
        backend.pathWeight(path.throughput); // texture streaming feedback (GPU)
        const MaterialResult material = backend.shade(shading, query);
#if PRISM_LIGHT_AOVS
        if (aovs && path.albedoOpen) resolveAlbedo(material.sample);
#endif
        if (!arrive(hit, vertex, material.sample)) return;
        connectLight(hit, vertex, material.light);
        continuePath(hit, vertex, material);
    }

    TraceResult trace(PathRay ray, SampleSettings sampleSettings)
    {
        beginPath(ray, sampleSettings);
        // Leave the loop as soon as a step ends the path. Testing path.active
        // only at the top of the loop sends every escaped path back through
        // it, which made the GPU kernels about 2.5 times slower on rays that
        // leave the scene (and slower overall).
        for (;;) {
            if (path.depth >= maxDepth) break;
            const Hit hit = extend();
            if (!path.active) break;
            if (!hit.surface.valid) continue; // scattered inside a medium
            scatterAtSurface(hit);
            if (!path.active) break;
        }
        TraceResult result = {path.radiance, path.outcome};
        return result;
    }

    // GenerateCameraRays: jittered sample ray with exact pixel differentials.
    PathRay cameraSample(CameraParams camera, uint x, uint y, uint width, uint height)
    {
        const float4 u = path.sampler.next4();
        const float sx = float(x) + u.x;
        const float sy = float(y) + u.y;
        float2 lens = 0.0f;
        const bool useLens = hasLens(camera);
        if (useLens) {
            // random_in_unit_disk
            const float a = u.z * (2.0f * kPi);
            const float r = sqrt(u.w);
            lens = float2(sin(a) * r, cos(a) * r);
        }
        PathRay result;
        result.ray = cameraRay(camera, sx, sy, width, height, useLens, lens);
        const Ray rx = cameraRay(camera, sx + 1.0f, sy, width, height, useLens, lens);
        const Ray ry = cameraRay(camera, sx, sy + 1.0f, width, height, useLens, lens);
        result.footprint = emptyFootprint();
        result.footprint.dOdx = rx.origin - result.ray.origin;
        result.footprint.dOdy = ry.origin - result.ray.origin;
        result.footprint.dDdx = rx.direction - result.ray.direction;
        result.footprint.dDdy = ry.direction - result.ray.direction;
        result.footprint.spread = max(length(result.footprint.dDdx), length(result.footprint.dDdy));
        result.footprint.width = max(length(result.footprint.dOdx), length(result.footprint.dOdy));
        result.footprint.exact = true;
        return result;
    }

    // One sample of one pixel, post-processed as in every render loop.
    SampleResult renderSample(CameraParams camera, uint x, uint y, uint width, uint height,
                              SampleSettings sampleSettings)
    {
        const TraceResult traced = trace(cameraSample(camera, x, y, width, height), sampleSettings);
        SampleResult result;
        result.coverage = sampleSettings.transparentEscapes && traced.outcome == kPrimaryEscaped ? 0.0f : 1.0f;
        float3 color = result.coverage == 0.0f ? (float3)0.0f : traced.radiance;
        if (sampleSettings.sampleClamp > 0.0f) {
            const float peak = maxComponent(color);
            if (peak > sampleSettings.sampleClamp) color *= sampleSettings.sampleClamp / peak;
        }
        result.color = color * sampleSettings.exposureScale;
#if PRISM_LIGHT_AOVS
        result.aovs.albedo = (float3)0.0f;
        result.aovs.diffuse = (float3)0.0f;
        result.aovs.specular = (float3)0.0f;
        result.aovs.emission = (float3)0.0f;
        if (aovs && result.coverage != 0.0f) {
            // The colour's scale: sample clamp and exposure.
            float scale = sampleSettings.exposureScale;
            if (sampleSettings.sampleClamp > 0.0f) {
                const float peak = maxComponent(traced.radiance);
                if (peak > sampleSettings.sampleClamp) scale *= sampleSettings.sampleClamp / peak;
            }
            result.aovs.albedo = path.aov.albedo;
            result.aovs.diffuse = path.aov.diffuse * scale;
            result.aovs.specular = path.aov.specular * scale;
            result.aovs.emission = path.aov.emission * scale;
        }
#endif
        return result;
    }
};
