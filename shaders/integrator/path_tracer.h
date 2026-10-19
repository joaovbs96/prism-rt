#pragma once

// The Prism path tracer, shared by every render device: compiled as C++ for
// the CPU device and as HLSL for the GPU devices (see types.h for the common
// subset of the two languages this file keeps to).
//
// PathTracer<Backend, Hit, Shading> traces with a device backend:
//
//   Hit      backend.intersect(PathRay)        prepared closest hit; hit.surface.valid
//   Shading  backend.prepare(Hit)              Material::perturbNormal + prepare
//   BsdfSample     backend.sample(Shading, float4 xi)   xi: the BSDF's sample point; materials
//                                               without one draw from backend.rng
//   BsdfEvaluation backend.evaluate(Shading, float3 wi)
//   MediumCoefficients backend.medium(Shading)    the medium behind the front face
//                                                 (valid false: none, or thin-walled)
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
//   int      backend.maxDepth(), backend.rouletteDepth()
//   bool     backend.environmentMis()
//   Rng      backend.rng                          white noise for materials and cutouts
//
// The integrator's own dimensions (pixel, lens, BSDF, light and roulette
// samples) come from the low-discrepancy `sampler`, set up per pixel and
// sample by the device; the decisions are those of SceneManager::trace and
// SceneManager::traceEnvironmentMis, which this replaces with one loop.
//
// GPU compilers inline every call, and each material call expands to the code
// of every material in the scene: keep exactly one call site per backend
// material function (prepare, sample, evaluate, cutoutOpacity).

#include "types.h"
#include "lights.h"

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

// SceneManager::directionFootprint
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
};

struct SampleResult {
    float3 color;
    float coverage;
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

template <typename Backend, typename Hit, typename Shading>
struct PathTracer {
    Backend backend;
    Sampler sampler;

    // SceneManager::environmentVisible: stochastic transmission through cutouts.
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
    LightPick pickLight(float u)
    {
        const uint entries = backend.lightCount() + 1u;
        const float scaled = u * float(entries);
        const uint column = min(uint(scaled), entries - 1u);
        const float4 entry = backend.lightAlias(column);
        LightPick pick;
        pick.index = scaled - float(column) < entry.x ? column : asuint(entry.y);
        pick.pmf = backend.lightAlias(pick.index).z;
        return pick;
    }

    float domeSelectionProbability()
    {
        const uint count = backend.lightCount();
        return count == 0u ? 1.0f : backend.lightAlias(count).z;
    }

    // Emission of the analytic lights a ray segment passes through, and of
    // distant lights for escaped rays (tmax infinite), weighted against NEE.
    float3 crossedLights(PathRay ray, float tmax, bool escaped, bool cameraRay, bool weighted,
                         float bsdfPdf, float neeProbability, uint receiver, float3 from)
    {
        float3 sum = (float3)0.0f;
        const uint count = backend.analyticLightCount();
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
            if (weighted) misWeight = powerHeuristic(bsdfPdf, neeProbability * light.n.w * crossing.pdf);
            sum += crossing.radiance * misWeight;
        }
        return sum;
    }

    // With environment MIS: BSDF sampling and environment NEE with the power
    // heuristic; cutout null events do not count as bounces (at most 64 in a
    // row). Without: BSDF sampling only, and null events are bounces. Both use
    // Russian roulette from the roulette depth on.
    TraceResult trace(PathRay ray, SampleSettings settings)
    {
        const uint contributions = settings.contributions;
        const bool mis = backend.environmentMis();
        const int maxDepth = backend.maxDepth();
        const int rouletteDepth = backend.rouletteDepth();
        TraceResult result = {(float3)0.0f, kPrimaryUnresolved};
        float3 weight = 1.0f;      // product of the scattering weights so far
        float3 throughput = 1.0f;  // roulette state, as in the recursive form
        int depth = 0;
        int nullCrossings = 0;
        float previousPdf = 0.0f;
        float previousNeeProbability = 1.0f;
        float3 previousPosition = ray.ray.origin; // the last scattering vertex
        bool previousNee = false;
        bool tracking = true;      // primary outcome still unresolved
        // The medium the path travels in (entered through a front face),
        // traced as a random walk with spectral (hero channel) MIS.
        bool inMedium = false;
        MediumCoefficients medium = {(float3)0.0f, (float3)0.0f, 0.0f, false};
        int volumeEvents = 0;
        uint receiver = ~0u;       // light-link categories of the last surface
        for (;;) {
            if (depth >= maxDepth) break;
            float scatterDistance = infinity();
            if (inMedium) {
                ray.ray.direction = normalize(ray.ray.direction);
                if (maxComponent(medium.sigmaS) > 0.0f) {
                    const float4 u = sampler.next4();
                    const float sigmaT = component(medium.sigmaA + medium.sigmaS, min(uint(u.x * 3.0f), 2u));
                    if (sigmaT > 0.0f) scatterDistance = -log(1.0f - u.y) / sigmaT;
                    ray.ray.tfar = min(ray.ray.tfar, scatterDistance);
                }
            }
            const Hit hit = backend.intersect(ray);
            if (inMedium) {
                const float3 sigmaT = medium.sigmaA + medium.sigmaS;
                if (!hit.surface.valid && !isfinite(scatterDistance)) break; // open surface: lost inside
                if (!hit.surface.valid) {
                    // A scattering event inside the medium.
                    const float3 transmittance = exp(-sigmaT * scatterDistance);
                    const float pdf = average(sigmaT * transmittance);
                    if (!(pdf > 0.0f)) break;
                    const float3 factor = medium.sigmaS * transmittance / pdf;
                    weight *= factor;
                    throughput *= factor;
                    if (++volumeEvents > kMaxVolumeEvents) break;
                    if (volumeEvents > 8) {
                        const float q = clamp(maxComponent(throughput), 0.05f, 0.95f);
                        if (sampler.next1() >= q) break;
                        weight *= 1.0f / q;
                        throughput *= 1.0f / q;
                    }
                    const float3 position = ray.ray.origin + ray.ray.direction * scatterDistance;
                    const float4 u = sampler.next4();
                    const float3 direction = sampleHenyeyGreenstein(ray.ray.direction, medium.g, float2(u.x, u.y));
                    const RayFootprint footprint = ray.footprint;
                    ray.ray = makeRay(position, direction);
                    ray.ray.tnear = 0.0f;
                    ray.footprint = footprint;
                    ray.footprint.exact = false;
                    previousNee = false;
                    continue;
                }
                // Reached the boundary (or another surface) without scattering.
                const float3 transmittance = exp(-sigmaT * hit.surface.t);
                if (maxComponent(medium.sigmaS) > 0.0f) {
                    const float pdf = average(transmittance);
                    if (!(pdf > 0.0f)) break;
                    weight *= transmittance / pdf;
                    throughput *= transmittance / pdf;
                } else {
                    weight *= transmittance;
                    throughput *= transmittance;
                }
            }
            const bool weighted = mis && previousNee;
            if (!inMedium && backend.analyticLightCount() > 0u) {
                const float3 crossed = crossedLights(ray, hit.surface.valid ? hit.surface.t : ray.ray.tfar,
                                                     !hit.surface.valid, depth == 0, weighted, previousPdf,
                                                     previousNeeProbability, receiver, previousPosition);
                result.radiance += weight * crossed * contributionWeight(contributions, depth);
            }
            if (!hit.surface.valid) {
                if (tracking) result.outcome = kPrimaryEscaped;
                const float3 direction = normalize(ray.ray.direction);
                const float3 li = backend.environmentRadiance(direction, angularRadius(ray.footprint));
                float misWeight = 1.0f;
                if (weighted)
                    misWeight = powerHeuristic(previousPdf, previousNeeProbability * domeSelectionProbability() *
                                                                backend.environmentPdf(direction));
                result.radiance += weight * li * (misWeight * contributionWeight(contributions, depth));
                break;
            }
            const Shading shading = backend.prepare(hit);
            const BsdfSample bs = backend.sample(shading, sampler.next4());
            const uint hitReceiver = backend.lightLinkMask(hit);
            float emissionWeight = 1.0f;
            if (weighted && !isZero(bs.emission)) {
                // Emissive triangles are also sampled by NEE: area to solid angle.
                const float pdfArea = backend.emitterPdfArea(hit);
                const float cosLight = abs(dot(hit.surface.n_g, normalize(ray.ray.direction)));
                const float distance = length(hit.surface.p - previousPosition);
                if (pdfArea > 0.0f && cosLight > 0.0f)
                    emissionWeight = powerHeuristic(previousPdf, previousNeeProbability * pdfArea *
                                                                     distance * distance / cosLight);
            }
            result.radiance += weight * bs.emission * (emissionWeight * contributionWeight(contributions, depth));
            if (mis && bs.null_event) {
                if (!bs.scattered) tracking = false;
                if (nullCrossings >= 64) break;
                weight *= bs.color;
                throughput *= bs.color;
                ray = spawnRay(hit.surface, bs.wi, true);
                ++nullCrossings;
                continue;
            }
            if (!bs.null_event && tracking) {
                result.outcome = kPrimaryAccepted;
                tracking = false;
            }
            // At the depth boundary the competing continuation is truncated too.
            const bool canNee = mis && depth + 1 < maxDepth && backend.supportsNee(shading);
            // Light sampling is a strategy with density neeProbability * light pdf.
            float neeProbability = 1.0f;
            if (canNee && settings.neeProbabilityMin > 0.0f && depth >= settings.neeFromDepth)
                neeProbability = clamp(maxComponent(weight), settings.neeProbabilityMin, 1.0f);
            const bool doNee = canNee && (neeProbability >= 1.0f || sampler.next1() < neeProbability);
            if (doNee) {
                // Without other lights the dome is the only entry (no selection number).
                LightPick pick;
                pick.index = 0u;
                pick.pmf = 1.0f;
                const uint lightCount = backend.lightCount();
                if (lightCount > 0u) pick = pickLight(sampler.next1());
                LightSample light = invalidLightSample();
                uint shadowBit = 0u;
                if (pick.index >= lightCount) {
                    const EnvironmentLightSample dome =
                        backend.sampleEnvironment(sampler.next4(), angularRadius(hit.surface.ray_footprint));
                    light.wi = dome.wi;
                    light.radiance = dome.radiance;
                    light.pdf = dome.pdf;
                    light.distance = infinity();
                    light.delta = false;
                    light.valid = dome.pdf > 0.0f && isfinite(dome.pdf);
                } else {
                    const LightRecord record = backend.light(pick.index);
                    const float4 u = sampler.next4();
                    const LightSampleTextured sampled = sampleLight(record, hit.surface.p, float2(u.x, u.y));
                    light = sampled.sample;
                    shadowBit = shadowLinkBit(record);
                    if (record.info.w != 0u && (hitReceiver & record.info.w) == 0u) light.valid = false;
                    if (light.valid && (record.info.y & kLightTextured) != 0u)
                        light.radiance *= backend.lightTexture(record.info.z, sampled.texcoord);
                }
                if (light.valid && pick.pmf > 0.0f) {
                    // The shadow ray is traced only for a nonzero contribution.
                    const BsdfEvaluation f = backend.evaluate(shading, light.wi);
                    if (f.valid && f.pdf >= 0.0f && isfinite(f.pdf)) {
                        const float lightPdf = neeProbability * pick.pmf * light.pdf;
                        const float misWeight = light.delta ? 1.0f : powerHeuristic(lightPdf, f.pdf);
                        const float3 contribution = weight * f.value * light.radiance * (misWeight / lightPdf);
                        PathRay shadow = spawnRay(hit.surface, light.wi, false);
                        if (isfinite(light.distance)) shadow.ray.tfar = light.distance * (1.0f - 1e-4f);
                        if (maxComponent(contribution) > 0.0f && visible(shadow, shadowBit))
                            result.radiance += contribution * contributionWeight(contributions, depth + 1);
                    }
                }
            }
            if (!bs.scattered) break;
            if (bs.transmitted && !bs.null_event) {
                if (hit.surface.front_face) {
                    medium = backend.medium(shading);
                    inMedium = medium.valid && (maxComponent(medium.sigmaA) > 0.0f ||
                                                maxComponent(medium.sigmaS) > 0.0f);
                } else {
                    inMedium = false;
                }
            }
            throughput *= bs.color;
            float rouletteWeight = 1.0f;
            if (depth >= rouletteDepth) {
                const float q = clamp(maxComponent(throughput), 0.05f, 0.95f);
                if (sampler.next1() >= q) break;
                rouletteWeight = 1.0f / q;
                throughput *= rouletteWeight;
            }
            previousNee = canNee && !bs.delta && !bs.null_event && bs.pdf > 0.0f;
            receiver = hitReceiver;
            previousPdf = bs.pdf;
            previousNeeProbability = neeProbability;
            previousPosition = hit.surface.p;
            weight *= bs.color * rouletteWeight;
            ray = spawnRay(hit.surface, bs.wi, bs.null_event);
            ++depth;
            nullCrossings = 0;
        }
        return result;
    }
    // GenerateCameraRays: jittered sample ray with exact pixel differentials.
    PathRay cameraSample(CameraParams camera, uint x, uint y, uint width, uint height)
    {
        const float4 u = sampler.next4();
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
                              SampleSettings settings)
    {
        const TraceResult traced = trace(cameraSample(camera, x, y, width, height), settings);
        SampleResult result;
        result.coverage = settings.transparentEscapes && traced.outcome == kPrimaryEscaped ? 0.0f : 1.0f;
        float3 color = result.coverage == 0.0f ? (float3)0.0f : traced.radiance;
        if (settings.sampleClamp > 0.0f) {
            const float peak = maxComponent(color);
            if (peak > settings.sampleClamp) color *= settings.sampleClamp / peak;
        }
        result.color = color * settings.exposureScale;
        return result;
    }
};
