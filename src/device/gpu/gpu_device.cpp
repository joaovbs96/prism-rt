#include "gpu_device.h"

#include "../../scene.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <thread>

namespace prism::device::gpu {


// A pixel-center hit stored by the kernel (gpu_store_primary_hit), in the
// RayHit fields that geometric outputs read.
static RayHit PrimaryHit(float const* h)
{
    RTCRayHit miss{};
    miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    RayHit hit(miss);
    if (h[3] != 0.0f) {
        hit.geomid = int(std::bit_cast<uint32_t>(h[10]));
        hit.primid = int(std::bit_cast<uint32_t>(h[11]));
        hit.p = Float3(h[0], h[1], h[2]);
        hit.n = Float3(h[4], h[5], h[6]);
        hit.u = h[8];
        hit.v = h[9];
    }
    return hit;
}

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
    if (data.lightLinking) flags |= kFrameLightLinking;
    frame.settings = {uint32_t(std::max(0, data.maxDepth)), uint32_t(std::max(0, data.rouletteDepth)), flags, 0};
    frame.counts = {MdlTextureCount(), 0, 0, 0};
    frame.lights = {uint32_t(data.lights.size()), data.analyticLights, uint32_t(data.environments.size()), 0};
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
    // Pixels left active by adaptive sampling get several samples in
    // parallel, so the dispatch still fills the GPU.
    constexpr int maxReplicas = 16;
    for (int sample = 0; sample < request.samples && !list.empty();) {
        int replicas = 1;
        if (adaptive.Enabled() && list.size() < count) {
            int stop = sample + 1;
            while (stop < request.samples && !adaptive.CheckAfter(stop)) ++stop;
            replicas = std::clamp(int(count / list.size()), 1, std::min(maxReplicas, stop - sample));
        }
        const uint32_t threads = uint32_t(list.size()) * uint32_t(replicas);
        std::memcpy(RenderBuffer(kSlotPixels, list.size() * 4), list.data(), list.size() * 4);
        if (replicas > 1) RenderBuffer(kSlotOutput, size_t(threads) * 32);
        GpuFrame f = frame;
        f.work = {threads, replicas > 1 ? uint32_t(list.size()) : 0u, kModeImage, 0};
        f.counts.z = uint32_t(sample);
        if (sample == 0) f.settings.z |= kFrameSeedRng;
        if (!Dispatch(f, threads, &gpuSeconds, &error)) {
            std::cerr << LogPrefix() << error << '\n';
            break;
        }
        if (replicas > 1) {
            auto const* out = static_cast<const float*>(RenderBuffer(kSlotOutput, size_t(threads) * 32));
            auto* sums = static_cast<float*>(RenderBuffer(kSlotAccumulation, size_t(count) * 32));
            for (uint32_t t = 0; t < threads; ++t) {
                const uint32_t p = list[t % list.size()];
                for (int c = 0; c < 3; ++c) {
                    sums[8 * p + c] += out[8 * t + c];
                    sums[8 * p + 4 + c] += out[8 * t + 4 + c];
                }
            }
        }
        sample += replicas;
        for (uint32_t p : list) samples[p] = sample;
        if (sample < request.samples && adaptive.CheckAfter(sample)) {
            auto const* sums = static_cast<const float*>(RenderBuffer(kSlotAccumulation, size_t(count) * 32));
            const float scale = ImageScale(count, [&](size_t p) {
                return (sums[8 * p] + sums[8 * p + 1] + sums[8 * p + 2]) / float(samples[p]);
            });
            active.Update(adaptive.threshold, [&](size_t p) {
                return AdaptiveError(sums + 8 * p, sums + 8 * p + 4, sample, scale);
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
    const bool prepared = (request.traceColor || tracePrimary) && PrepareScene(scene, &error);
    if ((request.traceColor || tracePrimary) && !prepared) std::cerr << LogPrefix() << error << '\n';
    const bool traceColor = request.traceColor && prepared;
    // Geometric outputs come from the kernel's pixel-center hits, with or
    // without color work, so they do not depend on the bound outputs.
    const bool gpuPrimary = tracePrimary && prepared;

    const size_t pixels = size_t(width) * height;
    uint32_t* rng = nullptr;
    float* output = nullptr;
    GpuFrame frame{};
    if (prepared) {
        if (traceColor) {
            rng = static_cast<uint32_t*>(RenderBuffer(kSlotRng, pixels * 4));
            output = static_cast<float*>(RenderBuffer(kSlotOutput, pixels * 16));
            std::memcpy(rng, request.rngStates, pixels * 4);
        }
        frame = BaseFrame(request.camera, width, height);
        frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                         request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
        if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
        if (request.sample == 0) frame.settings.z |= kFrameSeedRng;
        frame.settings.w = request.seed;
        frame.counts.z = uint32_t(std::max(0, request.sample));
        if (gpuPrimary) {
            frame.settings.z |= kFramePrimaryHits;
            RenderBuffer(kSlotAccumulation, pixels * 48);
        }
    }

    // Tiles are dispatched in chunks so callers see progress and can stop.
    constexpr size_t chunkPixels = 1u << 18;
    std::vector<uint32_t> list;
    size_t first = 0;
    auto const passStart = Clock::now();
    double passGpuSeconds = 0.0, dispatchSeconds = 0.0, readbackSeconds = 0.0;
    struct PassLog {
        double& gpu; double& dispatch; double& readback; Clock::time_point start;
        ~PassLog() {
            static const bool verbose = GpuVerbose();
            if (verbose)
                std::cerr << std::format("PrismRT GPU pass: wall {:.2f} ms, dispatch {:.2f} ms (GPU {:.2f} ms), "
                                         "readback {:.2f} ms\n",
                                         std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                                         dispatch * 1e3, gpu * 1e3, readback * 1e3);
        }
    } passLog{passGpuSeconds, dispatchSeconds, readbackSeconds, passStart};
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
        if ((traceColor || gpuPrimary) && !list.empty()) {
            std::memcpy(RenderBuffer(kSlotPixels, list.size() * 4), list.data(), list.size() * 4);
            GpuFrame f = frame;
            f.work = {uint32_t(list.size()), 0, traceColor ? kModePass : kModePrimary, 0};
            if (!Dispatch(f, uint32_t(list.size()), &passGpuSeconds, &error)) {
                std::cerr << LogPrefix() << error << '\n';
                return;
            }
        }
        auto const dispatched = Clock::now();
        dispatchSeconds += std::chrono::duration<double>(dispatched - rayStart).count();
        if (traceColor && !list.empty()) {
            // Buffers may move when resized; fetch them again after the dispatch.
            rng = static_cast<uint32_t*>(RenderBuffer(kSlotRng, pixels * 4));
            output = static_cast<float*>(RenderBuffer(kSlotOutput, pixels * 16));
            tbb::parallel_for(tbb::blocked_range<size_t>(0, list.size(), 4096),
                [&](tbb::blocked_range<size_t> const& range) {
                    for (size_t i = range.begin(); i != range.end(); ++i) {
                        const uint32_t pixel = list[i];
                        request.rngStates[pixel] = rng[pixel];
                        request.colors[pixel] = Color3(output[4 * pixel], output[4 * pixel + 1],
                                                       output[4 * pixel + 2]);
                        request.coverage[pixel] = output[4 * pixel + 3];
                    }
                });
            if (request.colorPixels) request.colorPixels->fetch_add(list.size(), std::memory_order_relaxed);
        }
        readbackSeconds += std::chrono::duration<double>(Clock::now() - dispatched).count();
        auto const primaryStart = Clock::now();
        if (request.rayNanoseconds)
            request.rayNanoseconds->fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                primaryStart - rayStart).count()), std::memory_order_relaxed);
        if (tracePrimary) {
            auto const* hits = gpuPrimary
                ? static_cast<const float*>(RenderBuffer(kSlotAccumulation, pixels * 48)) : nullptr;
            for (size_t t = first; t < last; ++t) {
                PixelTile const& tile = request.tiles[t];
                tbb::parallel_for(tile.y, tile.y + tile.height, [&](unsigned y) {
                    for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                        // Without a usable GPU scene, trace on the CPU scene.
                        const RayHit hit = hits ? PrimaryHit(hits + 12 * (size_t(y) * width + x))
                            : scene.cast_ray(prism::MakeCameraRayWithLens(request.camera,
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
    auto const traceStart = std::chrono::steady_clock::now();
    const uint32_t count = request.previewWidth * request.previewHeight;
    RenderBuffer(kSlotOutput, size_t(count) * 16);
    if (request.tracePrimary) RenderBuffer(kSlotAccumulation, size_t(count) * 48);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.size.z = request.previewWidth;
    frame.size.w = request.previewHeight;
    frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                         request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
    if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
    if (request.tracePrimary) frame.settings.z |= kFramePrimaryHits;
    frame.settings.w = request.seed;
    frame.work = {count, 0, kModePreview, 0};
    if (!Dispatch(frame, count, nullptr, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return false;
    }
    if (request.colorPixels) request.colorPixels->fetch_add(count, std::memory_order_relaxed);
    auto const* values = static_cast<const float*>(RenderBuffer(kSlotOutput, size_t(count) * 16));
    auto const* hits = request.tracePrimary
        ? static_cast<const float*>(RenderBuffer(kSlotAccumulation, size_t(count) * 48)) : nullptr;
    const unsigned previewWidth = request.previewWidth, previewHeight = request.previewHeight;
    tbb::parallel_for(0u, previewHeight, [&](unsigned py) {
        for (unsigned px = 0; px < previewWidth; ++px) {
            const size_t i = size_t(py) * previewWidth + px;
            const Color3 color(values[4 * i], values[4 * i + 1], values[4 * i + 2]);
            if (!hits) {
                request.writePixel(px, py, color, values[4 * i + 3], nullptr);
                continue;
            }
            const RayHit primary = PrimaryHit(hits + 12 * i);
            request.writePixel(px, py, color, values[4 * i + 3], &primary);
        }
    });
    if (request.traceMilliseconds)
        *request.traceMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - traceStart).count();
    return !(request.stopRequested && request.stopRequested());
}

RTCRayHit GpuRenderDevice::Intersect(SceneManager& scene, prism::PathRay const& ray)
{
    return scene.intersectRaw(ray);
}

} // namespace prism::device::gpu
