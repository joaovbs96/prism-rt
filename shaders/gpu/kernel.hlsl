// The render kernel of the GPU devices: one sample of one pixel per thread,
// in the pixel order and with the random-number seeding of the CPU render
// loops. Compiled by DXC for Metal (DXIL, then Metal shader converter) and
// for Vulkan (SPIR-V); resources.hlsl comes from shaders/gpu/metal or
// shaders/gpu/vulkan. The host inserts the generated MDL code at the marker.
//
// With PRISM_CALL_MATERIALS this is the raygen shader of the Vulkan ray
// tracing pipeline instead: no MDL code, the programs are callable shaders
// (gpu/vulkan/material_callable.hlsl).

#include "../shared/prism_gpu_types.h"
#include "../integrator/path_tracer.h"
#include "resources.hlsl"
#include "accessors.hlsl"
#include "geometry.hlsl"
#include "mdl_types.hlsl"
#ifndef PRISM_CALL_MATERIALS
#include "mdl_runtime.hlsl"
#endif
//@PRISM_MDL_CODE@
#include "gpu_backend.hlsl"

typedef PathTracer<GpuBackend, GpuHit, GpuShading> GpuPathTracer;

// The pinhole ray through a pixel's center, for geometric outputs.
PathRay pixelCenterRay(CameraParams camera, uint x, uint y, uint width, uint height)
{
    PathRay center;
    center.ray = cameraRay(camera, float(x) + 0.5f, float(y) + 0.5f, width, height, false, (float2)0.0f);
    center.footprint = emptyFootprint();
    return center;
}

CameraParams frameCamera(GpuFrame frame)
{
    CameraParams camera;
    for (int r = 0; r < 4; ++r) {
        camera.inverseProjection[r] = frame.inverseProjection[r];
        camera.cameraToWorld[r] = frame.cameraToWorld[r];
    }
    camera.nearClip = frame.camera.x;
    camera.farClip = frame.camera.y;
    camera.apertureRadius = frame.camera.z;
    camera.focusDistance = frame.camera.w;
    camera.orthographic = (frame.settings.z & kFrameOrthographic) != 0u;
    return camera;
}

// Debug views of the primary hit (PRISM_GPU_DEBUG): 1 normal, 2 texture
// coordinates, 3 material index, 4 hit distance. No material views: they
// would inline every material a second time.
float3 debugView(inout GpuPathTracer tracer, CameraParams camera, uint x, uint y, uint width, uint height,
                 uint mode)
{
    const GpuHit hit = tracer.backend.intersect(tracer.cameraSample(camera, x, y, width, height));
    if (!hit.surface.valid) return float3(0.0f, 0.0f, 0.2f);
    if (mode == 1u) return hit.surface.n * 0.5f + 0.5f;
    if (mode == 2u) return float3(frac(hit.surface.u), frac(hit.surface.v), 0.0f);
    if (mode == 3u) return float3(float(hit.material % 7u) / 6.0f, float(hit.material % 5u) / 4.0f, 1.0f);
    return (float3)hit.surface.t;
}

void renderThread(uint thread)
{
    const GpuFrame frame = gpu_frame();
    if (thread >= frame.work.x) return;
#ifdef PRISM_SHADER_SALT
    if (thread == PRISM_SHADER_SALT) return; // never: makes the code unique (cold-compile measurements)
#endif
    const uint flags = frame.settings.z;
    const CameraParams camera = frameCamera(frame);
    const SampleSettings settings = {frame.shading.x, frame.shading.y,
                                     (flags & kFrameTransparentEscapes) != 0u,
                                     frame.counts.w != 0u ? frame.counts.w : kContributionAll,
                                     frame.shading.z, int(frame.shading.w),
                                     (flags & kFrameSpatialLights) != 0u,
                                     float3(frame.color.x, frame.color.y, frame.color.z)};
    const uint width = frame.size.x;
    const uint height = frame.size.y;
    const uint mode = frame.work.z;
    GpuPathTracer tracer;
#if PRISM_LIGHT_AOVS
    // The light AOV variant of the kernel (PRISM_LIGHT_AOVS): every sample gathers them.
    tracer.aovs = true;
#endif

    // One call site of renderSample (and of the primary-hit intersection):
    // DXC inlines every call, so each extra site duplicates every material.
    const bool preview = mode == kModePreview;
    uint x, y;
    uint pixel = 0u;
    bool replicated = false;
    uint sampleIndex = 0u;
    if (preview) {
        const uint previewWidth = frame.size.z;
        const uint previewHeight = frame.size.w;
        const uint px = thread % previewWidth;
        const uint py = thread / previewWidth;
        y = min(height - 1u, ((2u * py + 1u) * height) / (2u * previewHeight));
        x = min(width - 1u, ((2u * px + 1u) * width) / (2u * previewWidth));
        tracer.backend.rng.state = seedPreview(px, py, previewWidth, frame.settings.w);
        const Sampler previewSampler = {0u, tracer.backend.rng.state, 0u};
        tracer.path.sampler = previewSampler;
    } else {
        // Image mode with work.y listed pixels traces several samples of each in
        // parallel: thread t takes sample counts.z + t / work.y of pixel t % work.y
        // and writes its own output slots, which the host adds up.
        replicated = mode == kModeImage && frame.work.y != 0u;
        const uint replica = replicated ? thread / frame.work.y : 0u;
        pixel = gpu_pixel(replicated ? thread % frame.work.y : thread);
        x = pixel % width;
        y = pixel / width;
        if (mode == kModePrimary) {
            gpu_store_primary_hit(pixel, width * height,
                                  tracer.backend.intersect(pixelCenterRay(camera, x, y, width, height)).surface);
            return;
        }
        sampleIndex = frame.counts.z + replica;
        // The sampler's sequence is indexed by the sample number, scrambled per pixel.
        const Sampler sampler = {sampleIndex,
                                 mode == kModeImage ? seedImage(x, y, width, frame.settings.w)
                                                    : seedPass(x, y, width, frame.settings.w),
                                 0u};
        tracer.path.sampler = sampler;
        if ((flags & kFrameSeedRng) != 0u)
            tracer.backend.rng.state = mode == kModeImage ? seedImage(x, y, width, frame.settings.w)
                                                          : seedPass(x, y, width, frame.settings.w);
        else if (replicated)
            tracer.backend.rng.state = hashCombine(sampler.seed, hashUint(sampleIndex));
        else
            tracer.backend.rng.state = gpu_load_rng(pixel);
    }
#ifdef PRISM_TEXTURE_STREAMING
    // Texture feedback: one pixel of each stride x stride block records the
    // levels its lookups ask for; the phase moves it from pass to pass.
    if ((flags & kFrameTextureFeedback) != 0u) {
        const uint s = max(frame.streaming.w, 1u);
        g_prism_feedback = (x % s) + (y % s) * s == frame.streaming.z % (s * s) ? 1u : 0u;
    }
#endif
    SampleResult result;
    if (!preview && frame.counts.y != 0u) {
        result.color = debugView(tracer, camera, x, y, width, height, frame.counts.y);
        result.coverage = 1.0f;
#if PRISM_LIGHT_AOVS
        result.aovs.albedo = (float3)0.0f;
        result.aovs.diffuse = (float3)0.0f;
        result.aovs.specular = (float3)0.0f;
        result.aovs.emission = (float3)0.0f;
#endif
    } else {
        result = tracer.renderSample(camera, x, y, width, height, settings);
    }
    if (replicated) {
        gpu_store_output(2u * thread, float4(result.color, 0.0f));
        gpu_store_output(2u * thread + 1u, (sampleIndex & 1u) == 0u ? float4(result.color, 0.0f) : (float4)0.0f);
#if PRISM_LIGHT_AOVS
        gpu_store_aovs(2u * frame.work.x, thread, result.aovs);
#endif
        return;
    }
    if (preview) gpu_store_output(thread, float4(result.color, result.coverage));
#if PRISM_LIGHT_AOVS
    if (preview) gpu_store_aovs(frame.size.z * frame.size.w, thread, result.aovs);
#endif
    if ((preview || mode == kModePass) && (flags & kFramePrimaryHits) != 0u)
        gpu_store_primary_hit(preview ? thread : pixel, preview ? frame.size.z * frame.size.w : width * height,
                              tracer.backend.intersect(pixelCenterRay(camera, x, y, width, height)).surface);
    if (preview) return;
    gpu_store_rng(pixel, tracer.backend.rng.state);
    if (mode == kModeImage) gpu_accumulate(pixel, result.color, (frame.counts.z & 1u) == 0u);
    else gpu_store_output(pixel, float4(result.color, result.coverage));
#if PRISM_LIGHT_AOVS
    if (mode == kModeImage) gpu_accumulate_aovs(2u * width * height, pixel, result.aovs);
    else gpu_store_aovs(width * height, pixel, result.aovs);
#endif
}

#ifdef PRISM_CALL_MATERIALS
[shader("raygeneration")]
void raygen()
{
    renderThread(DispatchRaysIndex().x);
}
#else
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    renderThread(id.x);
}
#endif
