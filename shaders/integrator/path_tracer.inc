// The Prism path tracer, shared by every render device.
//
// Written in the Prism shading dialect (prism_sl.h) against the backend
// interface below. A device provides MaterialHandle and MaterialContext
// before integrator_types.inc, then these functions before this file:
//
//   Hit backend_intersect(IN(PathRay) ray)
//       Closest hit, prepared like SceneManager::prepareHit (attributes,
//       face-forwarded normals, texture footprint); hit = false on a miss.
//   void backend_prepare_material(INOUT(Hit) hit, OUT(MaterialContext) context)
//       Material::perturbNormal + Material::prepare.
//   BsdfSample backend_sample_material(IN(Hit) hit, INOUT(MaterialContext) context, INOUT(uint) seed)
//   BsdfEvaluation backend_evaluate_material(IN(Hit) hit, float3 wi, INOUT(MaterialContext) context)
//   bool backend_supports_nee(IN(Hit) hit, INOUT(MaterialContext) context)
//   bool backend_proven_opaque(IN(Hit) hit)
//   float backend_cutout_opacity(IN(Hit) hit)
//   float3 backend_environment_radiance(float3 direction, float angularRadius)
//   EnvironmentSample backend_environment_sample(INOUT(uint) seed, float angularRadius)
//   float backend_environment_pdf(float3 direction)
//   int backend_max_depth()
//   int backend_roulette_depth()
//   bool backend_environment_mis()
//
// Random numbers are drawn in the same order as SceneManager::trace and
// SceneManager::traceEnvironmentMis, which this file replaces by loops.
// Another integrator can replace this file as long as it uses the same
// backend interface and provides prism_render_sample.

#ifndef PRISM_PATH_TRACER_INC
#define PRISM_PATH_TRACER_INC

PRISM_INLINE float prism_power_heuristic(float a, float b)
{
    float scale = max(a, b);
    if (!(scale > 0.0f) || !prism_isfinite(scale)) return 0.0f;
    a /= scale;
    b /= scale;
    float aa = a * a;
    float bb = b * b;
    return aa + bb > 0.0f ? aa / (aa + bb) : 0.0f;
}

// SceneManager::spawnRay
PRISM_INLINE PathRay prism_spawn_ray(IN(Hit) hit, float3 direction, bool nullEvent)
{
    PathRay result;
    result.ray = prism_make_ray(hit.p, direction);
    result.footprint = prism_empty_footprint();
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
PRISM_INLINE float prism_angular_radius(IN(RayFootprint) footprint)
{
    return max(0.0f, footprint.spread);
}

// SceneManager::environmentVisible: stochastic transmission through cutouts.
PRISM_INLINE bool prism_environment_visible(PathRay shadow, INOUT(uint) seed)
{
    float3 direction = shadow.ray.direction;
    for (int crossings = 0; crossings < 64; ++crossings) {
        Hit hit = backend_intersect(shadow);
        if (!hit.hit) return true;
        if (backend_proven_opaque(hit)) return false;
        float opacity = clamp(backend_cutout_opacity(hit), 0.0f, 1.0f);
        if (opacity >= 1.0f || (opacity > 0.0f && prism_rnd(seed) < opacity)) return false;
        shadow = prism_spawn_ray(hit, direction, true);
    }
    return false;
}

// SceneManager::traceEnvironmentMis: BSDF sampling and environment NEE with
// the power heuristic, Russian roulette, and cutout null events (which do
// not count as bounces, at most 64 in a row).
PRISM_INLINE float3 prism_trace_mis(PathRay ray, INOUT(uint) seed, INOUT(uint) primaryOutcome)
{
    int maxDepth = backend_max_depth();
    int rouletteDepth = backend_roulette_depth();
    float3 radiance = splat3(0.0f);
    float3 weight = splat3(1.0f);     // product of the scattering weights so far
    float3 throughput = splat3(1.0f); // roulette state, as in the recursive form
    int depth = 0;
    int nullCrossings = 0;
    float previousPdf = 0.0f;
    bool previousNee = false;
    bool tracking = true;             // primary outcome still unresolved
    for (;;) {
        if (depth >= maxDepth) break;
        Hit hit = backend_intersect(ray);
        if (!hit.hit) {
            if (tracking) primaryOutcome = PRISM_PRIMARY_ESCAPED;
            float3 direction = normalize(ray.ray.direction);
            float3 li = backend_environment_radiance(direction, prism_angular_radius(ray.footprint));
            float lightPdf = backend_environment_pdf(direction);
            radiance += weight * li * (previousNee ? prism_power_heuristic(previousPdf, lightPdf) : 1.0f);
            break;
        }
        MaterialContext context;
        backend_prepare_material(hit, context);
        BsdfSample bs = backend_sample_material(hit, context, seed);
        radiance += weight * bs.emission;
        if (bs.null_event) {
            if (!bs.scattered) tracking = false;
            if (nullCrossings >= 64) break;
            weight *= bs.color;
            throughput *= bs.color;
            ray = prism_spawn_ray(hit, bs.wi, true);
            ++nullCrossings;
            continue;
        }
        if (tracking) {
            primaryOutcome = PRISM_PRIMARY_ACCEPTED;
            tracking = false;
        }
        // At the depth boundary the competing continuation is truncated too.
        bool canNee = depth + 1 < maxDepth && backend_supports_nee(hit, context);
        if (canNee) {
            EnvironmentSample light = backend_environment_sample(
                seed, prism_angular_radius(hit.ray_footprint));
            PathRay lightRay = prism_spawn_ray(hit, light.wi, false);
            if (light.pdf > 0.0f && prism_isfinite(light.pdf) &&
                prism_environment_visible(lightRay, seed)) {
                BsdfEvaluation f = backend_evaluate_material(hit, light.wi, context);
                if (f.valid && f.pdf >= 0.0f && prism_isfinite(f.pdf))
                    radiance += weight * f.value * light.radiance *
                        (prism_power_heuristic(light.pdf, f.pdf) / light.pdf);
            }
        }
        if (!bs.scattered) break;
        throughput *= bs.color;
        float rouletteWeight = 1.0f;
        if (depth >= rouletteDepth) {
            float q = clamp(prism_max_component(throughput), 0.05f, 0.95f);
            if (prism_rnd(seed) >= q) break;
            rouletteWeight = 1.0f / q;
            throughput *= rouletteWeight;
        }
        previousNee = canNee && !bs.delta && !bs.null_event && bs.pdf > 0.0f;
        previousPdf = bs.pdf;
        weight *= bs.color * rouletteWeight;
        ray = prism_spawn_ray(hit, bs.wi, false);
        ++depth;
        nullCrossings = 0;
    }
    return radiance;
}

// SceneManager::trace without environment sampling (BSDF sampling only).
PRISM_INLINE float3 prism_trace_bsdf(PathRay ray, INOUT(uint) seed, INOUT(uint) primaryOutcome)
{
    int maxDepth = backend_max_depth();
    int rouletteDepth = backend_roulette_depth();
    float3 radiance = splat3(0.0f);
    float3 weight = splat3(1.0f);
    float3 throughput = splat3(1.0f);
    bool tracking = true;
    for (int depth = 0; depth < maxDepth; ++depth) {
        Hit hit = backend_intersect(ray);
        if (!hit.hit) {
            if (tracking) primaryOutcome = PRISM_PRIMARY_ESCAPED;
            radiance += weight * backend_environment_radiance(
                normalize(hit.d), prism_angular_radius(ray.footprint));
            break;
        }
        MaterialContext context;
        backend_prepare_material(hit, context);
        BsdfSample bs = backend_sample_material(hit, context, seed);
        if (!bs.null_event && tracking) {
            primaryOutcome = PRISM_PRIMARY_ACCEPTED;
            tracking = false;
        }
        radiance += weight * bs.emission;
        if (!bs.scattered) break;
        throughput *= bs.color;
        float rouletteWeight = 1.0f;
        if (depth >= rouletteDepth) {
            float q = clamp(prism_max_component(throughput), 0.05f, 0.95f);
            if (prism_rnd(seed) >= q) break;
            rouletteWeight = 1.0f / q;
        }
        ray = prism_spawn_ray(hit, bs.wi, bs.null_event);
        throughput *= rouletteWeight;
        weight *= bs.color * rouletteWeight;
    }
    return radiance;
}

PRISM_INLINE float3 prism_trace(PathRay ray, INOUT(uint) seed, INOUT(uint) primaryOutcome)
{
    if (backend_environment_mis()) return prism_trace_mis(ray, seed, primaryOutcome);
    return prism_trace_bsdf(ray, seed, primaryOutcome);
}

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

PRISM_INLINE float4 prism_row_transform(float4 p, IN(CameraParams) camera, bool projection)
{
    if (projection)
        return camera.inverseProjection[0] * p.x + camera.inverseProjection[1] * p.y +
               camera.inverseProjection[2] * p.z + camera.inverseProjection[3] * p.w;
    return camera.cameraToWorld[0] * p.x + camera.cameraToWorld[1] * p.y +
           camera.cameraToWorld[2] * p.z + camera.cameraToWorld[3] * p.w;
}

PRISM_INLINE bool prism_camera_has_lens(IN(CameraParams) camera)
{
    return !camera.orthographic && camera.apertureRadius > 0.0f && camera.focusDistance > 0.0f;
}

// MakeCameraRayWithLens; lens = (0, 0) with useLens = false for pinhole rays.
PRISM_INLINE Ray prism_camera_ray(IN(CameraParams) camera, float sx, float sy, uint width,
                                  uint height, bool useLens, float2 lens)
{
    float nx = 2.0f * sx / float(width) - 1.0f;
    float ny = 1.0f - 2.0f * sy / float(height);
    float4 p = prism_row_transform(float4(nx, ny, -1.0f, 1.0f), camera, true);
    if (abs(p.w) > 1e-12f) p = p / p.w;
    float3 originCamera = splat3(0.0f);
    float3 directionCamera;
    if (camera.orthographic) {
        originCamera = float3(p.x, p.y, 0.0f);
        directionCamera = float3(0.0f, 0.0f, -1.0f);
    } else {
        directionCamera = normalize(float3(p.x, p.y, p.z));
    }
    if (useLens && prism_camera_has_lens(camera)) {
        float focusT = camera.focusDistance / max(1e-8f, -directionCamera.z);
        float3 focus = directionCamera * focusT;
        originCamera = float3(lens.x * camera.apertureRadius, lens.y * camera.apertureRadius, 0.0f);
        directionCamera = normalize(focus - originCamera);
    }
    float4 o = prism_row_transform(float4(originCamera, 1.0f), camera, false);
    float4 d = prism_row_transform(float4(directionCamera, 0.0f), camera, false);
    Ray ray = prism_make_ray(float3(o.x, o.y, o.z), normalize(float3(d.x, d.y, d.z)));
    float dz = max(1e-8f, -directionCamera.z);
    ray.tnear = max(0.0f, camera.nearClip) / dz;
    ray.tfar = camera.farClip / dz;
    return ray;
}

// GenerateCameraRays: jittered sample ray with exact pixel differentials.
PRISM_INLINE PathRay prism_camera_sample(IN(CameraParams) camera, uint x, uint y, uint width,
                                         uint height, INOUT(uint) seed)
{
    float sx = float(x) + prism_rnd(seed);
    float sy = float(y) + prism_rnd(seed);
    float2 lens = float2(0.0f, 0.0f);
    bool useLens = prism_camera_has_lens(camera);
    if (useLens) {
        // random_in_unit_disk
        float a = prism_rnd(seed) * (2.0f * PRISM_PI);
        float r = sqrt(prism_rnd(seed));
        lens = float2(sin(a) * r, cos(a) * r);
    }
    PathRay result;
    result.ray = prism_camera_ray(camera, sx, sy, width, height, useLens, lens);
    Ray rx = prism_camera_ray(camera, sx + 1.0f, sy, width, height, useLens, lens);
    Ray ry = prism_camera_ray(camera, sx, sy + 1.0f, width, height, useLens, lens);
    result.footprint = prism_empty_footprint();
    result.footprint.dOdx = rx.origin - result.ray.origin;
    result.footprint.dOdy = ry.origin - result.ray.origin;
    result.footprint.dDdx = rx.direction - result.ray.direction;
    result.footprint.dDdy = ry.direction - result.ray.direction;
    result.footprint.spread = max(length(result.footprint.dDdx), length(result.footprint.dDdy));
    result.footprint.width = max(length(result.footprint.dOdx), length(result.footprint.dOdy));
    result.footprint.exact = true;
    return result;
}

// ---------------------------------------------------------------------------
// One sample of one pixel, post-processed as in every render loop.
// ---------------------------------------------------------------------------

struct SampleSettings {
    float sampleClamp;      // 0 disables
    float exposureScale;
    bool transparentEscapes;
};

struct SampleResult {
    float3 color;
    float coverage;
};

PRISM_INLINE SampleResult prism_render_sample(IN(CameraParams) camera, uint x, uint y, uint width,
                                              uint height, IN(SampleSettings) settings,
                                              INOUT(uint) seed)
{
    PathRay ray = prism_camera_sample(camera, x, y, width, height, seed);
    uint outcome = PRISM_PRIMARY_UNRESOLVED;
    float3 color = prism_trace(ray, seed, outcome);
    SampleResult result;
    result.coverage = settings.transparentEscapes && outcome == PRISM_PRIMARY_ESCAPED ? 0.0f : 1.0f;
    if (result.coverage == 0.0f) color = splat3(0.0f);
    if (settings.sampleClamp > 0.0f) {
        float peak = prism_max_component(color);
        if (peak > settings.sampleClamp) color *= settings.sampleClamp / peak;
    }
    result.color = color * settings.exposureScale;
    return result;
}

// Initial per-pixel random states of the render loops.
PRISM_INLINE uint prism_seed_image(uint x, uint y, uint width, uint seed)
{
    return prism_tea64((y * width) ^ seed, x);
}
PRISM_INLINE uint prism_seed_pass(uint x, uint y, uint width, uint seed)
{
    return prism_tea64((y * width) ^ seed, x);
}
PRISM_INLINE uint prism_seed_preview(uint px, uint py, uint previewWidth, uint seed)
{
    return prism_tea64(((py * previewWidth + px) ^ seed) ^ 0x9e3779b9u,
                       (px + 0x85ebca6bu) ^ (py * 0xc2b2ae35u));
}

#endif // PRISM_PATH_TRACER_INC
