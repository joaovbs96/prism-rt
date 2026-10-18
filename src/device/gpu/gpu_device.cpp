#include "gpu_device.h"

#include "../../scene.h"

#include <tbb/parallel_for.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>

namespace prism::device::gpu {


bool GpuVerbose()
{
    const char* value = std::getenv("PRISM_GPU_VERBOSE");
    return value && *value && std::string_view(value) != "0";
}

GpuFrame GpuRenderDevice::BaseFrame(prism::CameraRayParameters const& camera, unsigned width,
                                    unsigned height) const
{
    GpuSceneData const& data = Data();
    GpuFrame frame{};
    for (int r = 0; r < 4; ++r) {
        frame.inverseProjection[r] = {float(camera.inverseProjection[r][0]), float(camera.inverseProjection[r][1]),
                                      float(camera.inverseProjection[r][2]), float(camera.inverseProjection[r][3])};
        frame.cameraToWorld[r] = {float(camera.cameraToWorld[r][0]), float(camera.cameraToWorld[r][1]),
                                  float(camera.cameraToWorld[r][2]), float(camera.cameraToWorld[r][3])};
    }
    frame.camera = {camera.nearClip, camera.farClip, float(camera.apertureRadius), float(camera.focusDistance)};
    frame.size = {width, height, 0, 0};
    uint32_t flags = 0;
    if (camera.orthographic) flags |= kFrameOrthographic;
    if (data.environmentMis) flags |= kFrameEnvironmentMis;
    if (data.filteredTextures) flags |= kFrameFiltered;
    if (data.hasCutouts) flags |= kFrameCutouts;
    frame.settings = {uint32_t(std::max(0, data.maxDepth)), uint32_t(std::max(0, data.rouletteDepth)), flags, 0};
    frame.counts = {MdlTextureCount(), 0, 0, 0};
    // Debug views of the primary hit (see gpu_kernel.inc).
    if (const char* debug = std::getenv("PRISM_GPU_DEBUG")) frame.counts.y = uint32_t(std::atoi(debug));
    return frame;
}

std::vector<float> GpuRenderDevice::RenderImage(SceneManager& scene, ImageRequest const& request)
{
    std::lock_guard lock(m_renderMutex);
    std::vector<float> pixels(size_t(request.width) * request.height * 3, 0.0f);
    if (request.meanSamples) *request.meanSamples = request.samples;
    std::string error;
    if (!PrepareScene(scene, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return pixels;
    }
    const AdaptiveSettings& adaptive = request.adaptive;
    const uint32_t count = request.width * request.height;
    RenderBuffer(kSlotRng, size_t(count) * 4);
    std::memset(RenderBuffer(kSlotAccumulation, size_t(count) * 32), 0, size_t(count) * 32);
    std::vector<uint32_t> list(count);
    for (uint32_t p = 0; p < count; ++p) list[p] = p;
    std::vector<int> samples(count, 0);
    ActivePixels active;
    active.Reset(request.width, request.height);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.shading = {request.sampleClamp, 1.0f, request.nee.probabilityMin, float(request.nee.fromDepth)};
    frame.settings.w = request.seed;
    frame.counts.w = request.contributions & 7u;
    double gpuSeconds = 0.0;
    for (int sample = 0; sample < request.samples && !list.empty(); ++sample) {
        std::memcpy(RenderBuffer(kSlotPixels, list.size() * 4), list.data(), list.size() * 4);
        GpuFrame f = frame;
        f.work = {uint32_t(list.size()), 0, kModeImage, 0};
        f.counts.z = uint32_t(sample);
        if (sample == 0) f.settings.z |= kFrameSeedRng;
        if (!Dispatch(f, uint32_t(list.size()), &gpuSeconds, &error)) {
            std::cerr << LogPrefix() << error << '\n';
            break;
        }
        for (uint32_t p : list) samples[p] = sample + 1;
        if (sample + 1 < request.samples && adaptive.CheckAfter(sample + 1)) {
            auto const* sums = static_cast<const float*>(RenderBuffer(kSlotAccumulation, size_t(count) * 32));
            active.Update(adaptive.threshold, [&](size_t p) {
                return AdaptiveError(sums + 8 * p, sums + 8 * p + 4, sample + 1);
            });
            list.clear();
            for (uint32_t p = 0; p < count; ++p)
                if (active.Active(p)) list.push_back(p);
        }
    }
    auto const* sums = static_cast<const float*>(RenderBuffer(kSlotAccumulation, size_t(count) * 32));
    double total = 0.0;
    for (size_t p = 0; p < count; ++p) {
        const float scale = samples[p] > 0 ? 1.0f / float(samples[p]) : 0.0f;
        for (int c = 0; c < 3; ++c) pixels[3 * p + c] = sums[8 * p + c] * scale;
        total += samples[p];
    }
    if (request.meanSamples) *request.meanSamples = total / double(count);
    if (GpuVerbose())
        std::cerr << LogPrefix() << std::format("{} samples, GPU {:.3f} s\n", request.samples, gpuSeconds);
    return pixels;
}

void GpuRenderDevice::RenderPass(SceneManager& scene, PassRequest const& request)
{
    using Clock = std::chrono::steady_clock;
    std::lock_guard lock(m_renderMutex);
    std::string error;
    const unsigned width = request.width, height = request.height;
    const bool tracePrimary = request.tracePrimary && request.resolvePrimary;
    auto abandoned = [&] { return request.abandoned && request.abandoned(); };
    auto paused = [&] { return request.paused && request.paused(); };
    const bool traceColor = request.traceColor && PrepareScene(scene, &error);
    if (request.traceColor && !traceColor) std::cerr << LogPrefix() << error << '\n';

    const size_t pixels = size_t(width) * height;
    uint32_t* rng = nullptr;
    float* output = nullptr;
    GpuFrame frame{};
    if (traceColor) {
        rng = static_cast<uint32_t*>(RenderBuffer(kSlotRng, pixels * 4));
        output = static_cast<float*>(RenderBuffer(kSlotOutput, pixels * 16));
        std::memcpy(rng, request.rngStates, pixels * 4);
        frame = BaseFrame(request.camera, width, height);
        frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                         request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
        if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
        if (request.sample == 0) frame.settings.z |= kFrameSeedRng;
        frame.settings.w = request.seed;
        frame.counts.z = uint32_t(std::max(0, request.sample));
    }

    // Tiles are dispatched in chunks so callers see progress and can stop.
    constexpr size_t chunkPixels = 1u << 18;
    std::vector<uint32_t> list;
    size_t first = 0;
    while (first < request.tiles.size()) {
        while (paused()) {
            if (abandoned()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (abandoned()) return;
        size_t last = first;
        list.clear();
        while (last < request.tiles.size() && (list.size() < chunkPixels || last == first)) {
            PixelTile const& tile = request.tiles[last];
            for (unsigned y = tile.y; y < tile.y + tile.height; ++y)
                for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                    const uint32_t pixel = y * width + x;
                    if (!request.activePixels || request.activePixels[pixel]) list.push_back(pixel);
                }
            ++last;
        }
        auto const rayStart = Clock::now();
        if (traceColor && !list.empty()) {
            std::memcpy(RenderBuffer(kSlotPixels, list.size() * 4), list.data(), list.size() * 4);
            // Buffers may move when resized; fetch them again after the pixels.
            rng = static_cast<uint32_t*>(RenderBuffer(kSlotRng, pixels * 4));
            output = static_cast<float*>(RenderBuffer(kSlotOutput, pixels * 16));
            GpuFrame f = frame;
            f.work = {uint32_t(list.size()), 0, kModePass, 0};
            if (!Dispatch(f, uint32_t(list.size()), nullptr, &error)) {
                std::cerr << LogPrefix() << error << '\n';
                return;
            }
            for (uint32_t pixel : list) {
                request.rngStates[pixel] = rng[pixel];
                request.colors[pixel] = Color3(output[4 * pixel], output[4 * pixel + 1], output[4 * pixel + 2]);
                request.coverage[pixel] = output[4 * pixel + 3];
            }
            if (request.colorPixels) request.colorPixels->fetch_add(list.size(), std::memory_order_relaxed);
        }
        auto const primaryStart = Clock::now();
        if (request.rayNanoseconds)
            request.rayNanoseconds->fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                primaryStart - rayStart).count()), std::memory_order_relaxed);
        if (tracePrimary) {
            for (size_t t = first; t < last; ++t) {
                PixelTile const& tile = request.tiles[t];
                tbb::parallel_for(tile.y, tile.y + tile.height, [&](unsigned y) {
                    for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                        const RayHit hit = scene.cast_ray(prism::MakeCameraRayWithLens(request.camera,
                            float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr));
                        request.resolvePrimary(x, y, hit);
                    }
                });
                if (!request.traceColor && request.geometricOnlyPixels)
                    request.geometricOnlyPixels->fetch_add(size_t(tile.width) * tile.height,
                                                           std::memory_order_relaxed);
            }
        }
        if (request.primaryNanoseconds)
            request.primaryNanoseconds->fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Clock::now() - primaryStart).count()), std::memory_order_relaxed);
        if (request.tileFinished)
            for (size_t t = first; t < last; ++t) request.tileFinished(t);
        first = last;
    }
}

bool GpuRenderDevice::RenderPreview(SceneManager& scene, PreviewRequest const& request)
{
    std::lock_guard lock(m_renderMutex);
    std::string error;
    if (request.stopRequested && request.stopRequested()) return false;
    if (!PrepareScene(scene, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return false;
    }
    const uint32_t count = request.previewWidth * request.previewHeight;
    RenderBuffer(kSlotOutput, size_t(count) * 16);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.size.z = request.previewWidth;
    frame.size.w = request.previewHeight;
    frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                         request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
    if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
    frame.settings.w = request.seed;
    frame.work = {count, 0, kModePreview, 0};
    if (!Dispatch(frame, count, nullptr, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return false;
    }
    if (request.colorPixels) request.colorPixels->fetch_add(count, std::memory_order_relaxed);
    auto const* values = static_cast<const float*>(RenderBuffer(kSlotOutput, size_t(count) * 16));
    const unsigned width = request.width, height = request.height;
    const unsigned previewWidth = request.previewWidth, previewHeight = request.previewHeight;
    tbb::parallel_for(0u, previewHeight, [&](unsigned py) {
        const unsigned y = std::min(height - 1, unsigned((uint64_t(2 * py + 1) * height) / (2 * previewHeight)));
        for (unsigned px = 0; px < previewWidth; ++px) {
            const size_t i = size_t(py) * previewWidth + px;
            const Color3 color(values[4 * i], values[4 * i + 1], values[4 * i + 2]);
            if (request.tracePrimary) {
                const unsigned x = std::min(width - 1, unsigned((uint64_t(2 * px + 1) * width) / (2 * previewWidth)));
                const RayHit primary = scene.cast_ray(prism::MakeCameraRayWithLens(request.camera,
                    float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr));
                request.writePixel(px, py, color, values[4 * i + 3], &primary);
            } else {
                request.writePixel(px, py, color, values[4 * i + 3], nullptr);
            }
        }
    });
    return !(request.stopRequested && request.stopRequested());
}

RTCRayHit GpuRenderDevice::Intersect(SceneManager& scene, prism::PathRay const& ray)
{
    return scene.intersectRaw(ray);
}

} // namespace prism::device::gpu
