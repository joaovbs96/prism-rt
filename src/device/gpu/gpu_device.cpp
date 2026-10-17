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

using namespace prism::gpu;

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
    if (camera.orthographic) flags |= PRISM_FRAME_ORTHOGRAPHIC;
    if (data.environmentMis) flags |= PRISM_FRAME_ENVIRONMENT_MIS;
    if (data.filteredTextures) flags |= PRISM_FRAME_FILTERED;
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
    std::string error;
    if (!PrepareScene(scene, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return pixels;
    }
    const uint32_t count = request.width * request.height;
    RenderBuffer(PRISM_SLOT_RNG, size_t(count) * 4);
    auto* sums = static_cast<float*>(RenderBuffer(PRISM_SLOT_ACCUMULATION, size_t(count) * 16));
    std::memset(sums, 0, size_t(count) * 16);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.shading = {request.sampleClamp, 1.0f, 0.0f, 0.0f};
    frame.settings.w = request.seed;
    frame.work = {count, 0, PRISM_MODE_IMAGE, 0};
    double gpuSeconds = 0.0;
    for (int sample = 0; sample < request.samples; ++sample) {
        GpuFrame f = frame;
        if (sample == 0) f.settings.z |= PRISM_FRAME_SEED_RNG;
        if (!Dispatch(f, count, &gpuSeconds, &error)) {
            std::cerr << LogPrefix() << error << '\n';
            break;
        }
    }
    const float scale = request.samples > 0 ? 1.0f / float(request.samples) : 0.0f;
    for (size_t p = 0; p < count; ++p)
        for (int c = 0; c < 3; ++c) pixels[3 * p + c] = sums[4 * p + c] * scale;
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
        rng = static_cast<uint32_t*>(RenderBuffer(PRISM_SLOT_RNG, pixels * 4));
        output = static_cast<float*>(RenderBuffer(PRISM_SLOT_OUTPUT, pixels * 16));
        std::memcpy(rng, request.rngStates, pixels * 4);
        frame = BaseFrame(request.camera, width, height);
        frame.shading = {request.shading.sampleClamp, request.shading.exposureScale, 0.0f, 0.0f};
        if (request.shading.transparentEscapes) frame.settings.z |= PRISM_FRAME_TRANSPARENT_ESCAPES;
        if (request.sample == 0) frame.settings.z |= PRISM_FRAME_SEED_RNG;
        frame.settings.w = request.seed;
    }

    // Tiles are dispatched in chunks so callers see progress and can stop.
    constexpr size_t chunkPixels = 1u << 18;
    size_t first = 0;
    while (first < request.tiles.size()) {
        while (paused()) {
            if (abandoned()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (abandoned()) return;
        size_t last = first;
        size_t threads = 0;
        std::vector<GpuTile> tiles;
        while (last < request.tiles.size() && (threads < chunkPixels || last == first)) {
            PixelTile const& tile = request.tiles[last];
            GpuTile record;
            record.rect = {tile.x, tile.y, tile.width, tile.height};
            record.info = {uint32_t(threads), 0, 0, 0};
            tiles.push_back(record);
            threads += size_t(tile.width) * tile.height;
            ++last;
        }
        auto const rayStart = Clock::now();
        if (traceColor) {
            std::memcpy(RenderBuffer(PRISM_SLOT_TILES, tiles.size() * sizeof(GpuTile)), tiles.data(),
                        tiles.size() * sizeof(GpuTile));
            // Buffers may move when resized; fetch them again after the tiles.
            rng = static_cast<uint32_t*>(RenderBuffer(PRISM_SLOT_RNG, pixels * 4));
            output = static_cast<float*>(RenderBuffer(PRISM_SLOT_OUTPUT, pixels * 16));
            GpuFrame f = frame;
            f.work = {uint32_t(threads), uint32_t(tiles.size()), PRISM_MODE_PASS, 0};
            if (!Dispatch(f, uint32_t(threads), nullptr, &error)) {
                std::cerr << LogPrefix() << error << '\n';
                return;
            }
            for (size_t t = first; t < last; ++t) {
                PixelTile const& tile = request.tiles[t];
                for (unsigned y = tile.y; y < tile.y + tile.height; ++y)
                    for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                        const size_t pixel = size_t(y) * width + x;
                        request.rngStates[pixel] = rng[pixel];
                        request.colors[pixel] = Color3(output[4 * pixel], output[4 * pixel + 1], output[4 * pixel + 2]);
                        request.coverage[pixel] = output[4 * pixel + 3];
                    }
            }
            if (request.colorPixels) request.colorPixels->fetch_add(threads, std::memory_order_relaxed);
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
    RenderBuffer(PRISM_SLOT_OUTPUT, size_t(count) * 16);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.size.z = request.previewWidth;
    frame.size.w = request.previewHeight;
    frame.shading = {request.shading.sampleClamp, request.shading.exposureScale, 0.0f, 0.0f};
    if (request.shading.transparentEscapes) frame.settings.z |= PRISM_FRAME_TRANSPARENT_ESCAPES;
    frame.settings.w = request.seed;
    frame.work = {count, 0, PRISM_MODE_PREVIEW, 0};
    if (!Dispatch(frame, count, nullptr, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return false;
    }
    if (request.colorPixels) request.colorPixels->fetch_add(count, std::memory_order_relaxed);
    auto const* values = static_cast<const float*>(RenderBuffer(PRISM_SLOT_OUTPUT, size_t(count) * 16));
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
