// CPU render device. The loops below are the former standalone and Hydra
// render loops. The reference variant traces with SceneManager::trace exactly
// as before; the default variant runs the shared integrator per sample.

#include "cpu_device.h"
#include "cpu_backend.h"

#include "../rng.h"
#include "../scene.h"

#include <tbb/blocked_range.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace prism::device {

namespace {

// Count actual ray work once per task, including work abandoned by a stop.
struct WorkCounter {
    std::atomic<uint64_t>* total;
    uint64_t value = 0;
    ~WorkCounter() {
        if (total && value) total->fetch_add(value, std::memory_order_relaxed);
    }
};

Color3 ShadeSample(Color3 color, SceneManager::PrimaryOutcome outcome,
                   SampleShading const& shading, float* coverage)
{
    *coverage = shading.transparentEscapes &&
        outcome == SceneManager::PrimaryOutcome::Escaped ? 0.0f : 1.0f;
    if (*coverage == 0.0f) color = Color3(0.0f);
    if (shading.sampleClamp > 0.0f) {
        const float peak = color.max_component();
        if (peak > shading.sampleClamp) color *= shading.sampleClamp / peak;
    }
    color *= shading.exposureScale;
    return color;
}

// One sample of the shared path tracer; seed carries the pixel's random state.
SampleResult TraceSample(SceneManager& scene, CameraParams const& camera, SampleSettings const& settings,
                         unsigned x, unsigned y, unsigned width, unsigned height, unsigned& seed)
{
    CpuPathTracer tracer;
    tracer.backend.scene = &scene;
    tracer.backend.rng.state = seed;
    const SampleResult result = tracer.renderSample(camera, x, y, width, height, settings);
    seed = tracer.backend.rng.state;
    return result;
}

} // namespace

DeviceInfo CpuDevice::Info() const
{
    DeviceInfo info;
    info.kind = m_reference ? DeviceKind::CpuReference : DeviceKind::Cpu;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    info.name = std::string(m_reference ? "CPU reference integrator" : "CPU") +
        " (Embree, " + std::to_string(threads) + " threads)";
    info.hardwareRayTracing = false;
    return info;
}

std::vector<float> CpuDevice::RenderImage(SceneManager& scene, ImageRequest const& request)
{
    const int width = static_cast<int>(request.width);
    const int height = static_cast<int>(request.height);
    std::vector<float> pixels(size_t(width)*height*3);
    const CameraParams camera = ToCameraParams(request.camera);
    const SampleSettings settings = {request.sampleClamp, 1.0f, false};
    tbb::parallel_for(0, height, [&](int y) {
        for (int x = 0; x < width; ++x) {
            unsigned seed = tea<64>(unsigned(y*width) ^ request.seed, unsigned(x));
            Color3 sum(0);
            if (!m_reference) {
                for (int sample = 0; sample < request.samples; ++sample) {
                    const auto result = TraceSample(scene, camera, settings, unsigned(x),
                        unsigned(y), unsigned(width), unsigned(height), seed);
                    sum += ToFloat3(result.color);
                }
            }
            for (int sample = 0; m_reference && sample < request.samples; ++sample) {
                const auto pair = prism::GenerateCameraRays(
                    request.camera, static_cast<unsigned int>(x),
                    static_cast<unsigned int>(y), static_cast<unsigned int>(width),
                    static_cast<unsigned int>(height), seed);
                const prism::PathRay r{pair.sampled, pair.footprint};
                Color3 value = scene.trace(r, 0, Float3(1), seed);
                const float peak = value.max_component();
                if (request.sampleClamp > 0 && peak > request.sampleClamp)
                    value *= request.sampleClamp / peak;
                sum += value;
            }
            sum /= float(request.samples);
            const size_t offset = (size_t(y)*width+x)*3;
            for (int c = 0; c < 3; ++c) pixels[offset+c] = sum[c];
        }
    });
    return pixels;
}

void CpuDevice::RenderPass(SceneManager& scene, PassRequest const& request)
{
    using Clock = std::chrono::steady_clock;
    const unsigned width = request.width;
    const unsigned height = request.height;
    size_t maxTilePixels = 0;
    for (PixelTile const& tile : request.tiles)
        maxTilePixels = std::max(maxTilePixels, size_t(tile.width) * tile.height);
    const bool tracePrimary = request.tracePrimary && request.resolvePrimary;
    auto abandoned = [&] { return request.abandoned && request.abandoned(); };
    auto paused = [&] { return request.paused && request.paused(); };
    const CameraParams camera = ToCameraParams(request.camera);
    const SampleSettings settings = {request.shading.sampleClamp,
        request.shading.exposureScale, request.shading.transparentEscapes};

    tbb::task_arena renderArena(std::max(1, request.concurrency));
    renderArena.execute([&] {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, request.tiles.size(), 1),
            [&](tbb::blocked_range<size_t> const &range) {
                WorkCounter colorWork{request.colorPixels};
                WorkCounter geometricWork{request.geometricOnlyPixels};
                WorkCounter rayTime{request.rayNanoseconds};
                WorkCounter primaryTime{request.primaryNanoseconds};
                std::vector<RTCRay> centers(tracePrimary || !request.traceColor
                    ? maxTilePixels : 0);
                for (size_t tileIndex = range.begin(); tileIndex != range.end(); ++tileIndex) {
                    while (paused()) {
                        if (abandoned()) return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                    auto const &tile = request.tiles[tileIndex];
                    auto rayStart = Clock::now();
                    for (unsigned y = tile.y; y < tile.y + tile.height; ++y) {
                        if (abandoned()) return;
                        for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                            const size_t pixel = static_cast<size_t>(y) * width + x;
                            if (!request.traceColor) {
                                centers[(y-tile.y)*tile.width+x-tile.x] =
                                    prism::MakeCameraRayWithLens(request.camera,
                                        static_cast<float>(x) + 0.5f,
                                        static_cast<float>(y) + 0.5f,
                                        width, height, nullptr);
                                continue;
                            }
                            unsigned int &seed = request.rngStates[pixel];
                            if (request.sample == 0) seed = tea<64>((y * width) ^ request.seed, x);
                            if (!m_reference) {
                                if (tracePrimary)
                                    centers[(y-tile.y)*tile.width+x-tile.x] =
                                        prism::MakeCameraRayWithLens(request.camera,
                                            static_cast<float>(x) + 0.5f,
                                            static_cast<float>(y) + 0.5f,
                                            width, height, nullptr);
                                const auto result = TraceSample(scene, camera, settings, x, y, width, height, seed);
                                ++colorWork.value;
                                request.colors[pixel] = ToFloat3(result.color);
                                request.coverage[pixel] = result.coverage;
                                continue;
                            }
                            prism::CameraRayPair rays = prism::GenerateCameraRays(
                                request.camera, x, y, width, height, seed);
                            if (tracePrimary)
                                centers[(y-tile.y)*tile.width+x-tile.x] = rays.center;
                            SceneManager::PrimaryOutcome outcome = SceneManager::PrimaryOutcome::Unresolved;
                            Color3 c = scene.trace(
                                prism::PathRay{rays.sampled, rays.footprint},
                                0, Color3(1.0f), seed, &outcome);
                            ++colorWork.value;
                            float coverage;
                            c = ShadeSample(c, outcome, request.shading, &coverage);
                            request.colors[pixel] = c;
                            request.coverage[pixel] = coverage;
                        }
                    }
                    auto primaryStart = Clock::now();
                    rayTime.value += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        primaryStart-rayStart).count();
                    if (tracePrimary) {
                        for (unsigned y = tile.y; y < tile.y + tile.height; ++y) {
                            if (abandoned()) return;
                            for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                                const auto hit = scene.cast_ray(centers[(y-tile.y)*tile.width+x-tile.x]);
                                if (!request.traceColor) ++geometricWork.value;
                                request.resolvePrimary(x, y, hit);
                            }
                        }
                    }
                    primaryTime.value += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Clock::now()-primaryStart).count();
                    if (request.tileFinished) request.tileFinished(tileIndex);
                }
            });
    });
}

bool CpuDevice::RenderPreview(SceneManager& scene, PreviewRequest const& request)
{
    const unsigned width = request.width, height = request.height;
    const unsigned previewWidth = request.previewWidth, previewHeight = request.previewHeight;
    std::atomic<bool> cancelled{false};
    auto stopRequested = [&] { return request.stopRequested && request.stopRequested(); };
    const CameraParams camera = ToCameraParams(request.camera);
    const SampleSettings settings = {request.shading.sampleClamp,
        request.shading.exposureScale, request.shading.transparentEscapes};
    tbb::parallel_for(
        tbb::blocked_range2d<unsigned int>(
            0, previewHeight, 4, 0, previewWidth, 8),
        [&](tbb::blocked_range2d<unsigned int> const &range) {
    WorkCounter colorWork{request.colorPixels};
    if (cancelled.load(std::memory_order_relaxed)) return;
    for (unsigned int py = range.rows().begin();
         py != range.rows().end(); ++py) {
        if (stopRequested()) {
            cancelled.store(true, std::memory_order_relaxed);
            return;
        }
        const unsigned int y = std::min(height - 1,
            static_cast<unsigned int>((static_cast<uint64_t>(2 * py + 1) *
                                       height) / (2 * previewHeight)));
        for (unsigned int px = range.cols().begin();
             px != range.cols().end(); ++px) {
            const unsigned int x = std::min(width - 1,
                static_cast<unsigned int>((static_cast<uint64_t>(2 * px + 1) *
                                           width) / (2 * previewWidth)));
            unsigned int seed = tea<64>(
                ((py * previewWidth + px) ^ request.seed) ^ 0x9e3779b9u,
                (px + 0x85ebca6bu) ^ (py * 0xc2b2ae35u));
            Color3 color;
            float coverage;
            RTCRay center;
            if (m_reference) {
                prism::CameraRayPair rays = prism::GenerateCameraRays(
                    request.camera, x, y, width, height, seed);
                SceneManager::PrimaryOutcome outcome = SceneManager::PrimaryOutcome::Unresolved;
                color = scene.trace(
                    prism::PathRay{rays.sampled, rays.footprint},
                    0, Color3(1.0f), seed, &outcome);
                color = ShadeSample(color, outcome, request.shading, &coverage);
                center = rays.center;
            } else {
                const auto result = TraceSample(scene, camera, settings, x, y, width, height, seed);
                color = ToFloat3(result.color);
                coverage = result.coverage;
                center = prism::MakeCameraRayWithLens(request.camera,
                    static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                    width, height, nullptr);
            }
            ++colorWork.value;
            if (request.tracePrimary) {
                const RayHit primary = scene.cast_ray(center);
                request.writePixel(px, py, color, coverage, &primary);
            } else {
                request.writePixel(px, py, color, coverage, nullptr);
            }
        }
    }
        });
    return !cancelled.load(std::memory_order_relaxed) && !stopRequested();
}

RTCRayHit CpuDevice::Intersect(SceneManager& scene, prism::PathRay const& ray)
{
    return scene.intersectRaw(ray);
}

} // namespace prism::device
