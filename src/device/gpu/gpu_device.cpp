#include "gpu_device.h"
#include "../../renderer/logger.h"

#include "../../scene.h"
#include "../../color/color_management.h"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace prism::device::gpu {


// The pixel-center hit `index` of `count` stored by the kernel
// (gpu_store_primary_hit: three planes of `count` float4), in the RayHit
// fields that geometric outputs read.
static RayHit PrimaryHit(float const* hits, size_t index, size_t count)
{
    float h[12];
    for (size_t plane = 0; plane < 3; ++plane)
        std::memcpy(h + 4 * plane, hits + 4 * (plane * count + index), 4 * sizeof(float));
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

// The calling thread's logger shows "gpu" debug output (PRISM_GPU_VERBOSE).
bool GpuVerbose()
{
    return prism::logging::Debug("gpu");
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
    if (data.lightLobes) flags |= kFrameLightLobes;
    frame.settings = {uint32_t(std::max(0, data.maxDepth)), uint32_t(std::max(0, data.rouletteDepth)), flags, 0};
    frame.counts = {MdlTextureCount(), 0, 0, 0};
    frame.lights = {uint32_t(data.lights.size()), data.analyticLights, uint32_t(data.environments.size()), 0};
    const auto& luminance = data.luminanceWeights;
    frame.color = {luminance[0], luminance[1], luminance[2], 0.0f};
    // Debug views of the primary hit (see gpu_kernel.inc).
    if (const char* debug = std::getenv("PRISM_GPU_DEBUG")) frame.counts.y = uint32_t(std::atoi(debug));
    return frame;
}

namespace {
// What a failed submission most likely means, for the error message.
// Light AOVs (kFrameLightAovs, accessors.hlsl): four float4 per slot, the
// denoising normal in the w of the first three.
constexpr size_t kAovEntries = 4;

LightAovPixel ReadLightAovs(float const* entries, float scale)
{
    auto read = [&](size_t k) {
        return Color3(entries[4 * k] * scale, entries[4 * k + 1] * scale, entries[4 * k + 2] * scale);
    };
    return {read(0), read(1), read(2), read(3),
            Float3(entries[3] * scale, entries[7] * scale, entries[11] * scale)};
}

std::string FailureHint(std::string const& error)
{
    if (error.find("device lost") != std::string::npos)
        return " The GPU stopped responding or its driver reset it (on Windows a TDR: look for nvlddmkm or "
               "Display events in Event Viewer > Windows Logs > System). Another GPU-heavy program, an "
               "overclock or a driver problem can cause this; try again with the GPU otherwise idle, or "
               "render with --device cpu.";
    if (error.find("out of device memory") != std::string::npos)
        return " The GPU ran out of memory: close other GPU programs, lower the resolution or "
               "PRISM_GPU_TEXTURE_BUDGET_MIB, or render with --device cpu.";
    return " Rerun with PRISM_GPU_VERBOSE=1 for details, or render with --device cpu.";
}
} // namespace

std::vector<float> GpuRenderDevice::RenderImage(SceneManager& scene, ImageRequest const& request)
{
    std::lock_guard lock(m_renderMutex);
    std::vector<float> pixels(size_t(request.width) * request.height * 3, 0.0f);
    if (request.meanSamples) *request.meanSamples = request.samples;
    std::string error;
    // A failed render throws: returning the zeroed image would save a black
    // picture and report success.
    if (!PrepareScene(scene, &error))
        throw std::runtime_error(std::string(LogPrefix()) + "cannot prepare the scene for the GPU: " + error);
    // Denoising needs the albedo and normal guides of the light AOVs.
    const bool lightAovs = request.lightAovs != nullptr || request.denoised != nullptr;
    if (lightAovs && !PrepareLightAovs(&error))
        throw std::runtime_error(std::string(LogPrefix()) + "cannot build the light AOV kernel: " + error);
    // Test hook: fail the dispatch of this sample index.
    const char* failAt = std::getenv("PRISM_GPU_FAIL_DISPATCH_AT");
    const int failSample = failAt && *failAt ? std::atoi(failAt) : -1;
    const AdaptiveSettings& adaptive = request.adaptive;
    const uint32_t count = request.width * request.height;
    // Colour sums (two float4 per pixel), then the light AOV sums.
    const size_t accumulationBytes = size_t(count) * (32 + (lightAovs ? 16 * kAovEntries : 0));
    RenderBuffer(kSlotRng, size_t(count) * 4);
    std::memset(RenderBuffer(kSlotAccumulation, accumulationBytes), 0, accumulationBytes);
    std::vector<uint32_t> list(count);
    for (uint32_t p = 0; p < count; ++p) list[p] = p;
    std::vector<int> samples(count, 0);
    ActivePixels active;
    active.Reset(request.width, request.height);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.shading = {request.sampleClamp, 1.0f, request.nee.probabilityMin, float(request.nee.fromDepth)};
    if (request.nee.spatialLights) frame.settings.z |= kFrameSpatialLights;
    frame.settings.w = request.seed;
    frame.counts.w = request.contributions & 7u;
    // Streamed textures settle first, so every sample sees the same levels.
    if (StreamsTextures()) {
        SettleWithFrame(frame, kModeImage, count);
        std::memset(RenderBuffer(kSlotAccumulation, accumulationBytes), 0, accumulationBytes);
    }
    if (lightAovs) frame.settings.z |= kFrameLightAovs;
    BeginWork(kModeImage, 0);
    double gpuSeconds = 0.0;
    // The time limit counts from here: scene preparation and texture
    // settling are excluded, as on the CPU device.
    const auto start = std::chrono::steady_clock::now();
    if (request.timeLimitReached) *request.timeLimitReached = false;
    ImageProgress* const progress = request.progress;
    if (progress) {
        progress->target.store(std::max(1, request.samples), std::memory_order_relaxed);
        progress->samples.store(0.0, std::memory_order_relaxed);
        progress->activeFraction.store(1.0, std::memory_order_relaxed);
    }
    // Pixels left active by adaptive sampling get several samples in
    // parallel, so the dispatch still fills the GPU.
    constexpr int maxReplicas = 16;
    for (int sample = 0; sample < request.samples && !list.empty();) {
        // Cancelled (ImageRequest::cancel): stop at this sample boundary.
        if (request.cancel && request.cancel->load(std::memory_order_relaxed)) return {};
        if (request.timeLimitSeconds > 0.0 && sample > 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
                request.timeLimitSeconds) {
            if (request.timeLimitReached) *request.timeLimitReached = true;
            break;
        }
        int replicas = 1;
        if (adaptive.Enabled() && list.size() < count) {
            int stop = sample + 1;
            while (stop < request.samples && !adaptive.CheckAfter(stop)) ++stop;
            replicas = std::clamp(int(count / list.size()), 1, std::min(maxReplicas, stop - sample));
        }
        const uint32_t threads = uint32_t(list.size()) * uint32_t(replicas);
        const size_t outputBytes = size_t(threads) * (32 + (lightAovs ? 16 * kAovEntries : 0));
        std::memcpy(RenderBuffer(kSlotPixels, list.size() * 4), list.data(), list.size() * 4);
        if (replicas > 1) RenderBuffer(kSlotOutput, outputBytes);
        GpuFrame f = frame;
        f.work = {threads, replicas > 1 ? uint32_t(list.size()) : 0u, kModeImage, 0};
        f.counts.z = uint32_t(sample);
        if (sample == 0) f.settings.z |= kFrameSeedRng;
        const bool dispatched = sample == failSample
            ? (error = "simulated dispatch failure (PRISM_GPU_FAIL_DISPATCH_AT)", false)
            : Dispatch(f, threads, &gpuSeconds, &error);
        if (!dispatched)
            throw std::runtime_error(std::string(LogPrefix()) + std::format(
                "GPU render failed at sample {} of {} ({}x{}, {} threads, {:.1f} s of GPU time so far): {}.{}",
                sample + 1, request.samples, request.width, request.height, threads, gpuSeconds, error,
                FailureHint(error)));
        if (replicas > 1) {
            auto const* out = static_cast<const float*>(RenderBuffer(kSlotOutput, outputBytes));
            auto* sums = static_cast<float*>(RenderBuffer(kSlotAccumulation, accumulationBytes));
            for (uint32_t t = 0; t < threads; ++t) {
                const uint32_t p = list[t % list.size()];
                for (int c = 0; c < 3; ++c) {
                    sums[8 * p + c] += out[8 * t + c];
                    sums[8 * p + 4 + c] += out[8 * t + 4 + c];
                }
                // The light AOVs follow the 2 * threads colour entries.
                if (lightAovs)
                    for (size_t i = 0; i < 4 * kAovEntries; ++i)
                        sums[8 * size_t(count) + 4 * kAovEntries * p + i] +=
                            out[8 * size_t(threads) + 4 * kAovEntries * t + i];
            }
        }
        sample += replicas;
        for (uint32_t p : list) samples[p] = sample;
        if (progress) progress->samples.store(double(sample), std::memory_order_relaxed);
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
            if (progress)
                progress->activeFraction.store(double(list.size()) / double(count), std::memory_order_relaxed);
        }
    }
    auto const* sums = static_cast<const float*>(RenderBuffer(kSlotAccumulation, accumulationBytes));
    std::vector<LightAovPixel> aovs(lightAovs ? count : 0);
    double total = 0.0;
    size_t atMax = 0;
    for (size_t p = 0; p < count; ++p) {
        const float scale = samples[p] > 0 ? 1.0f / float(samples[p]) : 0.0f;
        for (int c = 0; c < 3; ++c) pixels[3 * p + c] = sums[8 * p + c] * scale;
        if (lightAovs) aovs[p] = ReadLightAovs(sums + 8 * size_t(count) + 4 * kAovEntries * p, scale);
        total += samples[p];
        atMax += samples[p] >= request.samples;
    }
    if (request.meanSamples) *request.meanSamples = total / double(count);
    if (request.fractionAtMaxSamples)
        *request.fractionAtMaxSamples = count ? double(atMax) / double(count) : 0.0;
    // On the device when it has a denoiser of its own (Vulkan: vk_oidn on
    // the accumulation), otherwise on the host images.
    if (request.denoised) {
        const auto denoiseStart = std::chrono::steady_clock::now();
        if (DenoiseAccumulation(request, samples)) {
            if (request.denoiseMilliseconds)
                *request.denoiseMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - denoiseStart).count();
        } else {
            DenoiseRenderedImage(*this, request, pixels, aovs);
        }
    }
    if (request.lightAovs) *request.lightAovs = std::move(aovs);
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
    bool lightAovs = traceColor && request.lightAovs;
    if (lightAovs && !PrepareLightAovs(&error)) {
        std::cerr << LogPrefix() << "cannot build the light AOV kernel: " << error << '\n';
        lightAovs = false;
    }

    const size_t pixels = size_t(width) * height;
    // Colour and coverage per pixel, then the light AOVs.
    const size_t outputBytes = pixels * (16 + (lightAovs ? 16 * kAovEntries : 0));
    uint32_t* rng = nullptr;
    float* output = nullptr;
    GpuFrame frame{};
    if (prepared) {
        if (traceColor) {
            rng = static_cast<uint32_t*>(RenderBuffer(kSlotRng, pixels * 4));
            output = static_cast<float*>(RenderBuffer(kSlotOutput, outputBytes));
            std::memcpy(rng, request.rngStates, pixels * 4);
        }
        frame = BaseFrame(request.camera, width, height);
        frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                         request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
        if (request.shading.nee.spatialLights) frame.settings.z |= kFrameSpatialLights;
        if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
        if (request.sample == 0) frame.settings.z |= kFrameSeedRng;
        if (lightAovs) frame.settings.z |= kFrameLightAovs;
        frame.settings.w = request.seed;
        frame.counts.z = uint32_t(std::max(0, request.sample));
        if (gpuPrimary) {
            frame.settings.z |= kFramePrimaryHits;
            RenderBuffer(kSlotAccumulation, pixels * 48);
        }
        if (traceColor) BeginWork(kModePass, request.sample);
    }

    // Tiles are dispatched in chunks so callers see progress and can stop.
    constexpr size_t chunkPixels = 1u << 18;
    std::vector<uint32_t> list;
    size_t first = 0;
    auto const passStart = Clock::now();
    double passGpuSeconds = 0.0, dispatchSeconds = 0.0, readbackSeconds = 0.0;
    size_t passPixels = 0;
    struct PassLog {
        double& gpu; double& dispatch; double& readback; size_t& pixels; Clock::time_point start;
        ~PassLog() {
            if (GpuVerbose())
                std::cerr << std::format("PrismRT GPU pass: wall {:.2f} ms, dispatch {:.2f} ms (GPU {:.2f} ms), "
                                         "readback {:.2f} ms, {} pixels\n",
                                         std::chrono::duration<double, std::milli>(Clock::now() - start).count(),
                                         dispatch * 1e3, gpu * 1e3, readback * 1e3, pixels);
        }
    } passLog{passGpuSeconds, dispatchSeconds, readbackSeconds, passPixels, passStart};
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
            if (traceColor) passPixels += list.size();
        }
        auto const dispatched = Clock::now();
        dispatchSeconds += std::chrono::duration<double>(dispatched - rayStart).count();
        if (traceColor && !list.empty()) {
            // Buffers may move when resized; fetch them again after the dispatch.
            rng = static_cast<uint32_t*>(RenderBuffer(kSlotRng, pixels * 4));
            output = static_cast<float*>(RenderBuffer(kSlotOutput, outputBytes));
            tbb::parallel_for(tbb::blocked_range<size_t>(0, list.size(), 4096),
                [&](tbb::blocked_range<size_t> const& range) {
                    for (size_t i = range.begin(); i != range.end(); ++i) {
                        const uint32_t pixel = list[i];
                        request.rngStates[pixel] = rng[pixel];
                        request.colors[pixel] = Color3(output[4 * pixel], output[4 * pixel + 1],
                                                       output[4 * pixel + 2]);
                        request.coverage[pixel] = output[4 * pixel + 3];
                        if (lightAovs)
                            request.lightAovs[pixel] =
                                ReadLightAovs(output + 4 * pixels + 4 * kAovEntries * pixel, 1.0f);
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
            const bool culling = scene.cullsBackFaces();
            auto const* hits = gpuPrimary
                ? static_cast<const float*>(RenderBuffer(kSlotAccumulation, pixels * 48)) : nullptr;
            for (size_t t = first; t < last; ++t) {
                PixelTile const& tile = request.tiles[t];
                tbb::parallel_for(tile.y, tile.y + tile.height, [&](unsigned y) {
                    for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                        // Without a usable GPU scene, trace on the CPU scene.
                        const RayHit hit = hits ? PrimaryHit(hits, size_t(y) * width + x, pixels)
                            : scene.castRayCulled(prism::MakeCameraRayWithLens(request.camera,
                                  float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr), culling);
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
    bool lightAovs = bool(request.writeLightAovs);
    if (lightAovs && !PrepareLightAovs(&error)) {
        std::cerr << LogPrefix() << "cannot build the light AOV kernel: " << error << '\n';
        lightAovs = false;
    }
    auto const traceStart = std::chrono::steady_clock::now();
    const uint32_t count = request.previewWidth * request.previewHeight;
    const size_t outputBytes = size_t(count) * (16 + (lightAovs ? 16 * kAovEntries : 0));
    RenderBuffer(kSlotOutput, outputBytes);
    if (request.tracePrimary) RenderBuffer(kSlotAccumulation, size_t(count) * 48);
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.size.z = request.previewWidth;
    frame.size.w = request.previewHeight;
    frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                         request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
    if (request.shading.nee.spatialLights) frame.settings.z |= kFrameSpatialLights;
    if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
    if (request.tracePrimary) frame.settings.z |= kFramePrimaryHits;
    if (lightAovs) frame.settings.z |= kFrameLightAovs;
    frame.settings.w = request.seed;
    frame.work = {count, 0, kModePreview, 0};
    BeginWork(kModePreview, 0);
    double gpuSeconds = 0.0;
    if (!Dispatch(frame, count, &gpuSeconds, &error)) {
        std::cerr << LogPrefix() << error << '\n';
        return false;
    }
    if (GpuVerbose())
        std::cerr << std::format("PrismRT GPU preview: {}x{}, GPU {:.2f} ms\n", request.previewWidth,
                                 request.previewHeight, gpuSeconds * 1e3);
    if (request.colorPixels) request.colorPixels->fetch_add(count, std::memory_order_relaxed);
    auto const* values = static_cast<const float*>(RenderBuffer(kSlotOutput, outputBytes));
    auto const* hits = request.tracePrimary
        ? static_cast<const float*>(RenderBuffer(kSlotAccumulation, size_t(count) * 48)) : nullptr;
    const unsigned previewWidth = request.previewWidth, previewHeight = request.previewHeight;
    tbb::parallel_for(0u, previewHeight, [&](unsigned py) {
        for (unsigned px = 0; px < previewWidth; ++px) {
            const size_t i = size_t(py) * previewWidth + px;
            const Color3 color(values[4 * i], values[4 * i + 1], values[4 * i + 2]);
            if (!hits) {
                request.writePixel(px, py, color, values[4 * i + 3], nullptr);
            } else {
                const RayHit primary = PrimaryHit(hits, i, count);
                request.writePixel(px, py, color, values[4 * i + 3], &primary);
            }
            if (lightAovs)
                request.writeLightAovs(px, py, ReadLightAovs(values + 4 * size_t(count) + 4 * kAovEntries * i, 1.0f));
        }
    });
    if (request.traceMilliseconds)
        *request.traceMilliseconds = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - traceStart).count();
    return !(request.stopRequested && request.stopRequested());
}

void GpuRenderDevice::SettleWithFrame(GpuFrame const& frame, uint32_t mode, uint32_t pixels)
{
    // The rounds trace the first samples of the accumulation that follows,
    // in its mode (its seeds and random numbers), so the feedback covers the
    // paths it will trace; their results are discarded.
    std::vector<uint32_t> list(pixels);
    for (uint32_t p = 0; p < pixels; ++p) list[p] = p;
    std::memcpy(RenderBuffer(kSlotPixels, size_t(pixels) * 4), list.data(), size_t(pixels) * 4);
    RenderBuffer(kSlotRng, size_t(pixels) * 4);
    RenderBuffer(kSlotAccumulation, size_t(pixels) * 32);
    if (mode == kModePass) RenderBuffer(kSlotOutput, size_t(pixels) * 16);
    FullFeedback(true);
    BeginWork(mode, 0);
    std::string error;
    const auto start = std::chrono::steady_clock::now();
    int rounds = 0;
    // Each round's levels can change what later lookups ask for (paths
    // through sharper textures); a few rounds settle it.
    // Rounds stop once one changes nothing: requests leave out levels only
    // negligible paths ask for (PRISM_GPU_STREAMING_THRESHOLD), so they
    // settle; the cap bounds the time.
    static const int maxRounds = [] {
        const char* value = std::getenv("PRISM_GPU_STREAMING_SETTLE_ROUNDS");
        return value && *value ? std::max(1, std::atoi(value)) : 8;
    }();
    for (; rounds < maxRounds; ++rounds) {
        GpuFrame f = frame;
        f.work = {pixels, 0, mode, 0};
        f.counts.z = uint32_t(rounds);
        if (rounds == 0) f.settings.z |= kFrameSeedRng;
        if (!Dispatch(f, pixels, nullptr, &error)) {
            std::cerr << LogPrefix() << error << '\n';
            break;
        }
        // PRISM_GPU_STREAMING_SETTLE_MIN_ROUNDS (diagnostics): rounds even without changes.
        static const int minRounds = [] {
            const char* value = std::getenv("PRISM_GPU_STREAMING_SETTLE_MIN_ROUNDS");
            return value && *value ? std::atoi(value) : 0;
        }();
        if (!SettleResidency() && rounds + 1 >= minRounds) break;
    }
    FullFeedback(false);
    HoldResidency();
    if (GpuVerbose())
        std::cerr << LogPrefix() << std::format("textures settled in {} feedback round{}, {:.2f} s\n",
                                                std::min(rounds + 1, maxRounds), rounds ? "s" : "",
                                                std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                                              start).count());
}

bool GpuRenderDevice::SettleTextures(SceneManager& scene, PassRequest const& request)
{
    std::lock_guard lock(m_renderMutex);
    std::string error;
    if (!PrepareScene(scene, &error) || !StreamsTextures()) return false;
    GpuFrame frame = BaseFrame(request.camera, request.width, request.height);
    frame.shading = {request.shading.sampleClamp, request.shading.exposureScale,
                     request.shading.nee.probabilityMin, float(request.shading.nee.fromDepth)};
    if (request.shading.nee.spatialLights) frame.settings.z |= kFrameSpatialLights;
    if (request.shading.transparentEscapes) frame.settings.z |= kFrameTransparentEscapes;
    frame.settings.w = request.seed;
    SettleWithFrame(frame, kModePass, request.width * request.height);
    return true;
}

void GpuRenderDevice::Prepare(SceneManager& scene)
{
    std::lock_guard lock(m_renderMutex);
    std::string error;
    // Failures are reported again, once, by the render call that needs the scene.
    (void)PrepareScene(scene, &error);
}

RTCRayHit GpuRenderDevice::Intersect(SceneManager& scene, prism::PathRay const& ray)
{
    return scene.intersectRaw(ray);
}

} // namespace prism::device::gpu
