#pragma once

// The Prism path tracer, shared by every render device: compiled as C++ for
// the CPU device and as HLSL for the GPU devices (see types.h for the common
// subset of the two languages this file keeps to).
//
// PathTracer<Backend, Hit, Shading> traces with a device backend:
//
//   Hit      backend.intersect(PathRay)        prepared closest hit; hit.surface.valid
//   Shading  backend.prepare(Hit)              Material::perturbNormal + prepare
//   BsdfSample     backend.sample(Shading)     draws from backend.rng
//   BsdfEvaluation backend.evaluate(Shading, float3 wi)
//   bool     backend.supportsNee(Shading)
//   bool     backend.provenOpaque(Hit)
//   float    backend.cutoutOpacity(Hit)
//   bool     backend.occludedByOpaque(PathRay)   any hit on always-opaque geometry
//                                                 (may conservatively return false)
//   bool     backend.mayHaveCutouts()             false: occludedByOpaque is exact
//   Hit      backend.intersectNonOpaque(PathRay)  closest hit on geometry that may
//                                                 transmit (may include opaque geometry)
//   float3   backend.environmentRadiance(float3 direction, float angularRadius)
//   EnvironmentLightSample backend.sampleEnvironment(float angularRadius)   draws from backend.rng
//   float    backend.environmentPdf(float3 direction)
//   int      backend.maxDepth(), backend.rouletteDepth()
//   bool     backend.environmentMis()
//   Rng      backend.rng
//
// Random numbers are drawn in the order of SceneManager::trace and
// SceneManager::traceEnvironmentMis, which this replaces with one loop.
//
// GPU compilers inline every call, and each material call expands to the code
// of every material in the scene: keep exactly one call site per backend
// material function (prepare, sample, evaluate, cutoutOpacity).

#include "types.h"

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

struct SampleSettings {
    float sampleClamp;      // 0 disables
    float exposureScale;
    bool transparentEscapes;
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

struct TraceResult {
    float3 radiance;
    uint outcome;      // kPrimary*
};

template <typename Backend, typename Hit, typename Shading>
struct PathTracer {
    Backend backend;

    // SceneManager::environmentVisible: stochastic transmission through cutouts.
    // Opaque geometry anywhere on the ray blocks it whatever cutouts lie in
    // front, so a cheap any-hit query on opaque geometry comes first.
    bool environmentVisible(PathRay shadow)
    {
        if (backend.occludedByOpaque(shadow)) return false;
        if (!backend.mayHaveCutouts()) return true;
        const float3 direction = shadow.ray.direction;
        for (int crossings = 0; crossings < 64; ++crossings) {
            const Hit hit = backend.intersectNonOpaque(shadow);
            if (!hit.surface.valid) return true;
            if (backend.provenOpaque(hit)) return false;
            const float opacity = saturate(backend.cutoutOpacity(hit));
            if (opacity >= 1.0f || (opacity > 0.0f && backend.rng.next() < opacity)) return false;
            shadow = spawnRay(hit.surface, direction, true);
        }
        return false;
    }

    // With environment MIS: BSDF sampling and environment NEE with the power
    // heuristic; cutout null events do not count as bounces (at most 64 in a
    // row). Without: BSDF sampling only, and null events are bounces. Both use
    // Russian roulette from the roulette depth on.
    TraceResult trace(PathRay ray)
    {
        const bool mis = backend.environmentMis();
        const int maxDepth = backend.maxDepth();
        const int rouletteDepth = backend.rouletteDepth();
        TraceResult result = {(float3)0.0f, kPrimaryUnresolved};
        float3 weight = 1.0f;      // product of the scattering weights so far
        float3 throughput = 1.0f;  // roulette state, as in the recursive form
        int depth = 0;
        int nullCrossings = 0;
        float previousPdf = 0.0f;
        bool previousNee = false;
        bool tracking = true;      // primary outcome still unresolved
        for (;;) {
            if (depth >= maxDepth) break;
            const Hit hit = backend.intersect(ray);
            if (!hit.surface.valid) {
                if (tracking) result.outcome = kPrimaryEscaped;
                const float3 direction = normalize(ray.ray.direction);
                const float3 li = backend.environmentRadiance(direction, angularRadius(ray.footprint));
                float misWeight = 1.0f;
                if (mis && previousNee)
                    misWeight = powerHeuristic(previousPdf, backend.environmentPdf(direction));
                result.radiance += weight * li * misWeight;
                break;
            }
            const Shading shading = backend.prepare(hit);
            const BsdfSample bs = backend.sample(shading);
            result.radiance += weight * bs.emission;
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
            if (canNee) {
                const EnvironmentLightSample light = backend.sampleEnvironment(angularRadius(hit.surface.ray_footprint));
                if (light.pdf > 0.0f && isfinite(light.pdf)) {
                    // The shadow ray is traced only for a nonzero contribution.
                    const BsdfEvaluation f = backend.evaluate(shading, light.wi);
                    if (f.valid && f.pdf >= 0.0f && isfinite(f.pdf)) {
                        const float3 contribution = weight * f.value * light.radiance *
                            (powerHeuristic(light.pdf, f.pdf) / light.pdf);
                        if (maxComponent(contribution) > 0.0f &&
                            environmentVisible(spawnRay(hit.surface, light.wi, false)))
                            result.radiance += contribution;
                    }
                }
            }
            if (!bs.scattered) break;
            throughput *= bs.color;
            float rouletteWeight = 1.0f;
            if (depth >= rouletteDepth) {
                const float q = clamp(maxComponent(throughput), 0.05f, 0.95f);
                if (backend.rng.next() >= q) break;
                rouletteWeight = 1.0f / q;
                throughput *= rouletteWeight;
            }
            previousNee = canNee && !bs.delta && !bs.null_event && bs.pdf > 0.0f;
            previousPdf = bs.pdf;
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
        const float sx = float(x) + backend.rng.next();
        const float sy = float(y) + backend.rng.next();
        float2 lens = 0.0f;
        const bool useLens = hasLens(camera);
        if (useLens) {
            // random_in_unit_disk
            const float a = backend.rng.next() * (2.0f * kPi);
            const float r = sqrt(backend.rng.next());
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
        const TraceResult traced = trace(cameraSample(camera, x, y, width, height));
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
