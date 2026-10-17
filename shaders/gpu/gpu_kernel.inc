// Per-thread work of the GPU render kernel: one sample of one pixel, in the
// pixel order and with the random-number seeding of the CPU render loops.
// Written in the Prism shading dialect.

#ifndef PRISM_GPU_KERNEL_INC
#define PRISM_GPU_KERNEL_INC

void prism_kernel(uint dispatchThread)
{
    GpuFrame frame = gpu_frame();
    uint thread = dispatchThread + frame.work.w;
    if (dispatchThread >= frame.work.x) return;
    uint flags = frame.settings.z;
    CameraParams camera;
    for (int r = 0; r < 4; ++r) {
        camera.inverseProjection[r] = frame.inverseProjection[r];
        camera.cameraToWorld[r] = frame.cameraToWorld[r];
    }
    camera.nearClip = frame.camera.x;
    camera.farClip = frame.camera.y;
    camera.apertureRadius = frame.camera.z;
    camera.focusDistance = frame.camera.w;
    camera.orthographic = (flags & PRISM_FRAME_ORTHOGRAPHIC) != 0u;
    SampleSettings settings;
    settings.sampleClamp = frame.shading.x;
    settings.exposureScale = frame.shading.y;
    settings.transparentEscapes = (flags & PRISM_FRAME_TRANSPARENT_ESCAPES) != 0u;
    uint width = frame.size.x;
    uint height = frame.size.y;
    uint mode = frame.work.z;

    if (mode == PRISM_MODE_PREVIEW) {
        uint previewWidth = frame.size.z;
        uint previewHeight = frame.size.w;
        uint px = thread % previewWidth;
        uint py = thread / previewWidth;
        uint y = min(height - 1u, ((2u * py + 1u) * height) / (2u * previewHeight));
        uint x = min(width - 1u, ((2u * px + 1u) * width) / (2u * previewWidth));
        uint previewSeed = prism_seed_preview(px, py, previewWidth, frame.settings.w);
        SampleResult preview = prism_render_sample(camera, x, y, width, height, settings, previewSeed);
        gpu_store_output(thread, float4(preview.color, preview.coverage));
        return;
    }

    uint x;
    uint y;
    if (mode == PRISM_MODE_PASS) {
        // The last tile whose first thread is not after this one.
        uint lo = 0u;
        uint hi = frame.work.y - 1u;
        while (lo < hi) {
            uint mid = (lo + hi + 1u) / 2u;
            if (gpu_tile(mid).info.x <= thread) lo = mid;
            else hi = mid - 1u;
        }
        GpuTile tile = gpu_tile(lo);
        uint local = thread - tile.info.x;
        x = tile.rect.x + local % tile.rect.z;
        y = tile.rect.y + local / tile.rect.z;
    } else {
        x = thread % width;
        y = thread / width;
    }
    uint pixel = y * width + x;
    uint seed;
    if ((flags & PRISM_FRAME_SEED_RNG) != 0u)
        seed = mode == PRISM_MODE_IMAGE ? prism_seed_image(x, y, width, frame.settings.w)
                                        : prism_seed_pass(x, y, width, frame.settings.w);
    else
        seed = gpu_load_rng(pixel);
    SampleResult result;
    if (frame.counts.y != 0u) {
        // Debug views of the primary hit (PRISM_GPU_DEBUG): 1 normal,
        // 2 texture coordinates, 3 material index, 4 hit distance. Material
        // views would inline every material a second time.
        PathRay ray = prism_camera_sample(camera, x, y, width, height, seed);
        Hit hit = backend_intersect(ray);
        float3 c = splat3(0.0f);
        if (hit.hit) {
            if (frame.counts.y == 1u) c = hit.n * 0.5f + splat3(0.5f);
            else if (frame.counts.y == 2u) c = float3(hit.u - floor(hit.u), hit.v - floor(hit.v), 0.0f);
            else if (frame.counts.y == 3u) c = float3(float(hit.material % 7u) / 6.0f, float(hit.material % 5u) / 4.0f, 1.0f);
            else c = splat3(hit.t);
        } else {
            c = float3(0.0f, 0.0f, 0.2f);
        }
        result.color = c;
        result.coverage = 1.0f;
    } else {
        result = prism_render_sample(camera, x, y, width, height, settings, seed);
    }
    gpu_store_rng(pixel, seed);
    if (mode == PRISM_MODE_IMAGE) gpu_accumulate(pixel, result.color);
    else gpu_store_output(pixel, float4(result.color, result.coverage));
}

#endif // PRISM_GPU_KERNEL_INC
