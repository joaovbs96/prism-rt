// The render kernel of the GPU devices: one sample of one pixel per thread,
// in the pixel order and with the random-number seeding of the CPU render
// loops. Compiled by DXC for Metal (DXIL, then Metal shader converter) and
// for Vulkan (SPIR-V); resources.hlsl comes from shaders/gpu/metal or
// shaders/gpu/vulkan. The host inserts the generated MDL code at the marker.

#include "../shared/prism_gpu_types.h"
#include "../integrator/path_tracer.h"
#include "resources.hlsl"
#include "accessors.hlsl"
#include "geometry.hlsl"
#include "mdl_types.hlsl"
#include "mdl_runtime.hlsl"
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

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const GpuFrame frame = gpu_frame();
    if (id.x >= frame.work.x) return;
    const uint thread = id.x;
    const uint flags = frame.settings.z;
    const CameraParams camera = frameCamera(frame);
    const SampleSettings settings = {frame.shading.x, frame.shading.y,
                                     (flags & kFrameTransparentEscapes) != 0u,
                                     frame.counts.w != 0u ? frame.counts.w : kContributionAll,
                                     frame.shading.z, int(frame.shading.w)};
    const uint width = frame.size.x;
    const uint height = frame.size.y;
    const uint mode = frame.work.z;
    GpuPathTracer tracer;

    if (mode == kModePreview) {
        const uint previewWidth = frame.size.z;
        const uint previewHeight = frame.size.w;
        const uint px = thread % previewWidth;
        const uint py = thread / previewWidth;
        const uint y = min(height - 1u, ((2u * py + 1u) * height) / (2u * previewHeight));
        const uint x = min(width - 1u, ((2u * px + 1u) * width) / (2u * previewWidth));
        tracer.backend.rng.state = seedPreview(px, py, previewWidth, frame.settings.w);
        const Sampler previewSampler = {0u, tracer.backend.rng.state, 0u};
        tracer.sampler = previewSampler;
        const SampleResult preview = tracer.renderSample(camera, x, y, width, height, settings);
        gpu_store_output(thread, float4(preview.color, preview.coverage));
        if ((flags & kFramePrimaryHits) != 0u)
            gpu_store_primary_hit(thread, tracer.backend.intersect(pixelCenterRay(camera, x, y, width, height)).surface);
        return;
    }

    // Image mode with work.y listed pixels traces several samples of each in
    // parallel: thread t takes sample counts.z + t / work.y of pixel t % work.y
    // and writes its own output slots, which the host adds up.
    const bool replicated = mode == kModeImage && frame.work.y != 0u;
    const uint replica = replicated ? thread / frame.work.y : 0u;
    const uint pixel = gpu_pixel(replicated ? thread % frame.work.y : thread);
    const uint x = pixel % width;
    const uint y = pixel / width;
    if (mode == kModePrimary) {
        gpu_store_primary_hit(pixel, tracer.backend.intersect(pixelCenterRay(camera, x, y, width, height)).surface);
        return;
    }
    const uint sampleIndex = frame.counts.z + replica;
    // The sampler's sequence is indexed by the sample number, scrambled per pixel.
    const Sampler sampler = {sampleIndex,
                             mode == kModeImage ? seedImage(x, y, width, frame.settings.w)
                                                : seedPass(x, y, width, frame.settings.w),
                             0u};
    tracer.sampler = sampler;
    if ((flags & kFrameSeedRng) != 0u)
        tracer.backend.rng.state = mode == kModeImage ? seedImage(x, y, width, frame.settings.w)
                                                      : seedPass(x, y, width, frame.settings.w);
    else if (replicated)
        tracer.backend.rng.state = hashCombine(sampler.seed, hashUint(sampleIndex));
    else
        tracer.backend.rng.state = gpu_load_rng(pixel);
    SampleResult result;
    if (frame.counts.y != 0u) {
        result.color = debugView(tracer, camera, x, y, width, height, frame.counts.y);
        result.coverage = 1.0f;
    } else {
        result = tracer.renderSample(camera, x, y, width, height, settings);
    }
    if (replicated) {
        gpu_store_output(2u * thread, float4(result.color, 0.0f));
        gpu_store_output(2u * thread + 1u, (sampleIndex & 1u) == 0u ? float4(result.color, 0.0f) : (float4)0.0f);
        return;
    }
    if (mode == kModePass && (flags & kFramePrimaryHits) != 0u)
        gpu_store_primary_hit(pixel, tracer.backend.intersect(pixelCenterRay(camera, x, y, width, height)).surface);
    gpu_store_rng(pixel, tracer.backend.rng.state);
    if (mode == kModeImage) gpu_accumulate(pixel, result.color, (frame.counts.z & 1u) == 0u);
    else gpu_store_output(pixel, float4(result.color, result.coverage));
}
