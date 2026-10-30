// CPU render device: the standalone and Hydra render loops, running the
// shared integrator (shaders/integrator) per sample on the CPU backend.

#include "cpu_device.h"
#include "cpu_backend.h"
#include "opaque_occluders.h"
#include "../color/color_management.h"

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

// Light selection weighs radiance by the luminance of the scene's working
// space.
float3 WorkingLuminance(SceneManager const& scene)
{
    const auto& w = scene.color().luminanceWeights();
    return float3(w[0], w[1], w[2]);
}

// Count actual ray work once per task, including work abandoned by a stop.
struct WorkCounter {
    std::atomic<uint64_t>* total;
    uint64_t value = 0;
    ~WorkCounter() {
        if (total && value) total->fetch_add(value, std::memory_order_relaxed);
    }
};

// One sample of the shared path tracer; seed carries the pixel's random state.
// The sampler's sequence is indexed by `sample`, scrambled per pixel.
SampleResult TraceSample(SceneManager& scene, CpuSceneState const& state, CameraParams const& camera,
                         SampleSettings const& settings, unsigned x, unsigned y, unsigned width,
                         unsigned height, unsigned& seed, unsigned sample, unsigned pixelSeed,
                         bool lightAovs = false)
{
    CpuPathTracer tracer;
    tracer.aovs = lightAovs;
    tracer.backend.scene = &scene;
    tracer.backend.occluders = state.occluders.get();
    tracer.backend.lights = state.lights.get();
    tracer.backend.lobes = state.lights && state.lights->lobes;
    tracer.backend.culling = state.culling;
    tracer.backend.rng.state = seed;
    tracer.path.sampler = {sample, pixelSeed, 0u};
    profiling::Scope timer(profiling::Sample);
    const SampleResult result = tracer.renderSample(camera, x, y, width, height, settings);
    seed = tracer.backend.rng.state;
    return result;
}

LightAovPixel ToLightAovs(LightAovSample const& a)
{
    return {ToFloat3(a.albedo), ToFloat3(a.diffuse), ToFloat3(a.specular), ToFloat3(a.emission),
            ToFloat3(a.normal)};
}

void Add(LightAovPixel& sum, LightAovSample const& a)
{
    sum.albedo += ToFloat3(a.albedo);
    sum.diffuse += ToFloat3(a.diffuse);
    sum.specular += ToFloat3(a.specular);
    sum.emission += ToFloat3(a.emission);
    sum.normal += ToFloat3(a.normal);
}

} // namespace

void CpuDevice::SceneChanged(SceneManager&)
{
    std::lock_guard lock(m_stateMutex);
    m_stateOf = nullptr;
    m_state.reset();
}

std::shared_ptr<const CpuSceneState> CpuDevice::PreparedScene(SceneManager& scene)
{
    std::lock_guard lock(m_stateMutex);
    if (m_stateOf != &scene || !m_state) {
        auto state = std::make_shared<CpuSceneState>();
        state->culling = scene.cullsBackFaces();
        // Cutouts, shadow links and culled back faces let shadow rays
        // through some primitives: OpaqueOccluders tells them apart.
        state->occluders = std::make_shared<const OpaqueOccluders>(OpaqueOccluders::Build(scene, state->culling));
        state->lights = std::make_shared<LightTable>(BuildLightTable(scene));
        m_state = std::move(state);
        m_stateOf = &scene;
    }
    return m_state;
}

DeviceInfo CpuDevice::Info() const
{
    DeviceInfo info;
    info.kind = DeviceKind::Cpu;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    info.name = "CPU (Embree, " + std::to_string(threads) + " threads)";
    info.hardwareRayTracing = false;
    return info;
}

std::vector<float> CpuDevice::RenderImage(SceneManager& scene, ImageRequest const& request)
{
    const int width = static_cast<int>(request.width);
    const int height = static_cast<int>(request.height);
    std::vector<float> pixels(size_t(width)*height*3);
    if (request.meanSamples) *request.meanSamples = request.samples;
    ImageProgress* const progress = request.progress;
    if (progress) {
        progress->target.store(std::max(1, request.samples), std::memory_order_relaxed);
        progress->samples.store(0.0, std::memory_order_relaxed);
        progress->activeFraction.store(1.0, std::memory_order_relaxed);
    }
    const CameraParams camera = ToCameraParams(request.camera);
    const auto state = PreparedScene(scene);
    profiling::Attach profile;
    const SampleSettings settings = {request.sampleClamp, 1.0f, false, request.contributions,
        request.nee.probabilityMin, request.nee.fromDepth, request.nee.spatialLights, WorkingLuminance(scene)};
    const AdaptiveSettings& adaptive = request.adaptive;
    const size_t count = size_t(width) * height;
    std::vector<unsigned> seeds(count);
    std::vector<Color3> sums(count, Color3(0));
    std::vector<Color3> evenSums(adaptive.Enabled() ? count : 0, Color3(0));
    std::vector<int> samples(count, 0);
    // Denoising needs the albedo and normal guides of the light AOVs.
    const bool lightAovs = request.lightAovs != nullptr || request.denoised != nullptr;
    std::vector<LightAovPixel> aovSums(lightAovs ? count : 0);
    ActivePixels active;
    active.Reset(unsigned(width), unsigned(height));
    // Without adaptive sampling or a time limit this is one round of all
    // samples per pixel. A time limit renders one sample per round, so it is
    // checked at every sample boundary; the samples are the same.
    const bool timed = request.timeLimitSeconds > 0.0;
    const auto start = std::chrono::steady_clock::now();
    if (request.timeLimitReached) *request.timeLimitReached = false;
    int done = 0;
    auto cancelled = [&] { return request.cancel && request.cancel->load(std::memory_order_relaxed); };
    while (done < request.samples) {
        if (cancelled()) return {};
        if (timed && done > 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
                request.timeLimitSeconds) {
            if (request.timeLimitReached) *request.timeLimitReached = true;
            break;
        }
        const int next = timed ? done + 1
            : adaptive.Enabled()
            ? std::min(request.samples, done < adaptive.minSamples ? adaptive.minSamples : done + adaptive.step)
            : request.samples;
        // Progress: the round's traced pixel-samples, added once per row.
        const uint64_t roundWork = uint64_t(active.Count()) * uint64_t(next - done);
        std::atomic<uint64_t> roundDone{0};
        tbb::parallel_for(0, height, [&](int y) {
            if (cancelled()) return;
            profiling::Attach rowProfile;
            uint64_t rowWork = 0;
            for (int x = 0; x < width; ++x) {
                const size_t pixel = size_t(y) * width + x;
                if (!active.Active(pixel)) continue;
                rowWork += uint64_t(next - done);
                unsigned& seed = seeds[pixel];
                if (done == 0) seed = tea<64>(unsigned(y*width) ^ request.seed, unsigned(x));
                for (int sample = done; sample < next; ++sample) {
                    // A cancelled render's pixels are dropped: stop within
                    // a pixel's samples too (one round may be all of them).
                    if ((sample & 15) == 15 && cancelled()) return;
                    const auto result = TraceSample(scene, *state, camera, settings, unsigned(x),
                        unsigned(y), unsigned(width), unsigned(height), seed, unsigned(sample),
                        seedImage(unsigned(x), unsigned(y), unsigned(width), request.seed), lightAovs);
                    sums[pixel] += ToFloat3(result.color);
                    if (lightAovs) Add(aovSums[pixel], result.aovs);
                    if (adaptive.Enabled() && sample % 2 == 0) evenSums[pixel] += ToFloat3(result.color);
                }
                samples[pixel] = next;
            }
            if (progress && rowWork && roundWork) {
                const uint64_t finished = roundDone.fetch_add(rowWork, std::memory_order_relaxed) + rowWork;
                progress->samples.store(done + double(next - done) * double(finished) / double(roundWork),
                                        std::memory_order_relaxed);
            }
        });
        if (cancelled()) return {};
        done = next;
        if (progress) progress->samples.store(double(done), std::memory_order_relaxed);
        if (done < request.samples && adaptive.CheckAfter(done)) {
            profiling::Scope checkTimer(profiling::AdaptiveCheck);
            const float scale = ImageScale(count, [&](size_t pixel) {
                return (sums[pixel].x() + sums[pixel].y() + sums[pixel].z()) / float(samples[pixel]);
            });
            active.Update(adaptive.threshold, [&](size_t pixel) {
                    const float sum[3] = {sums[pixel].x(), sums[pixel].y(), sums[pixel].z()};
                    const float even[3] = {evenSums[pixel].x(), evenSums[pixel].y(), evenSums[pixel].z()};
                    return AdaptiveError(sum, even, done, scale);
                });
            if (progress) progress->activeFraction.store(double(active.Count()) / double(count), std::memory_order_relaxed);
            if (active.Count() == 0) break;
        }
    }
    profiling::Scope filmTimer(profiling::Film);
    double total = 0.0;
    size_t atMax = 0;
    for (size_t pixel = 0; pixel < count; ++pixel) {
        Color3 value = sums[pixel];
        value /= float(samples[pixel]);
        for (int c = 0; c < 3; ++c) pixels[3 * pixel + c] = value[c];
        if (lightAovs) {
            LightAovPixel& a = aovSums[pixel];
            const float scale = 1.0f / float(samples[pixel]);
            a.albedo *= scale;
            a.diffuse *= scale;
            a.specular *= scale;
            a.emission *= scale;
            a.normal *= scale;
        }
        total += samples[pixel];
        atMax += samples[pixel] >= request.samples;
    }
    if (request.meanSamples) *request.meanSamples = total / double(count);
    if (request.fractionAtMaxSamples) *request.fractionAtMaxSamples = count ? double(atMax) / double(count) : 0.0;
    filmTimer.stop();
    {
        profiling::Scope denoiseTimer(profiling::Denoise);
        DenoiseRenderedImage(*this, request, pixels, aovSums);
    }
    if (request.lightAovs) *request.lightAovs = std::move(aovSums);
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
    const auto state = PreparedScene(scene);
    const SampleSettings settings = {request.shading.sampleClamp,
        request.shading.exposureScale, request.shading.transparentEscapes, kContributionAll,
        request.shading.nee.probabilityMin, request.shading.nee.fromDepth,
        request.shading.nee.spatialLights, WorkingLuminance(scene)};

    tbb::task_arena renderArena(std::max(1, request.concurrency));
    renderArena.execute([&] {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, request.tiles.size(), 1),
            [&](tbb::blocked_range<size_t> const &range) {
                profiling::Attach profile;
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
                            if (request.activePixels && !request.activePixels[pixel]) {
                                if (tracePrimary)
                                    centers[(y-tile.y)*tile.width+x-tile.x] =
                                        prism::MakeCameraRayWithLens(request.camera,
                                            static_cast<float>(x) + 0.5f,
                                            static_cast<float>(y) + 0.5f,
                                            width, height, nullptr);
                                continue;
                            }
                            unsigned int &seed = request.rngStates[pixel];
                            if (request.sample == 0) seed = tea<64>((y * width) ^ request.seed, x);
                            if (tracePrimary)
                                centers[(y-tile.y)*tile.width+x-tile.x] =
                                    prism::MakeCameraRayWithLens(request.camera,
                                        static_cast<float>(x) + 0.5f,
                                        static_cast<float>(y) + 0.5f,
                                        width, height, nullptr);
                            const auto result = TraceSample(scene, *state, camera, settings, x, y, width,
                                height, seed, unsigned(request.sample), seedPass(x, y, width, request.seed),
                                request.lightAovs != nullptr);
                            ++colorWork.value;
                            request.colors[pixel] = ToFloat3(result.color);
                            request.coverage[pixel] = result.coverage;
                            if (request.lightAovs) request.lightAovs[pixel] = ToLightAovs(result.aovs);
                        }
                    }
                    auto primaryStart = Clock::now();
                    rayTime.value += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        primaryStart-rayStart).count();
                    if (tracePrimary) {
                        for (unsigned y = tile.y; y < tile.y + tile.height; ++y) {
                            if (abandoned()) return;
                            for (unsigned x = tile.x; x < tile.x + tile.width; ++x) {
                                const auto hit = scene.castRayCulled(centers[(y-tile.y)*tile.width+x-tile.x], state->culling);
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
    auto const traceStart = std::chrono::steady_clock::now();
    struct ReportTime {
        PreviewRequest const& request;
        std::chrono::steady_clock::time_point start;
        ~ReportTime() {
            if (request.traceMilliseconds)
                *request.traceMilliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
        }
    } reportTime{request, traceStart};
    const unsigned width = request.width, height = request.height;
    const unsigned previewWidth = request.previewWidth, previewHeight = request.previewHeight;
    std::atomic<bool> cancelled{false};
    auto stopRequested = [&] { return request.stopRequested && request.stopRequested(); };
    const CameraParams camera = ToCameraParams(request.camera);
    const auto state = PreparedScene(scene);
    const SampleSettings settings = {request.shading.sampleClamp,
        request.shading.exposureScale, request.shading.transparentEscapes, kContributionAll,
        request.shading.nee.probabilityMin, request.shading.nee.fromDepth,
        request.shading.nee.spatialLights, WorkingLuminance(scene)};
    tbb::parallel_for(
        tbb::blocked_range2d<unsigned int>(
            0, previewHeight, 4, 0, previewWidth, 8),
        [&](tbb::blocked_range2d<unsigned int> const &range) {
    WorkCounter colorWork{request.colorPixels};
    profiling::Attach profile;
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
            const auto result = TraceSample(scene, *state, camera, settings, x, y, width, height,
                                            seed, 0u, seed, bool(request.writeLightAovs));
            const Color3 color = ToFloat3(result.color);
            const float coverage = result.coverage;
            const RTCRay center = prism::MakeCameraRayWithLens(request.camera,
                static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                width, height, nullptr);
            ++colorWork.value;
            if (request.tracePrimary) {
                const RayHit primary = scene.castRayCulled(center, state->culling);
                request.writePixel(px, py, color, coverage, &primary);
            } else {
                request.writePixel(px, py, color, coverage, nullptr);
            }
            if (request.writeLightAovs) request.writeLightAovs(px, py, ToLightAovs(result.aovs));
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
