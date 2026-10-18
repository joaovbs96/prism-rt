// CPU render device. The loops below are the former standalone and Hydra
// render loops. The reference variant traces with SceneManager::trace exactly
// as before; the default variant runs the shared integrator per sample.

#include "cpu_device.h"
#include "cpu_backend.h"
#include "../batched_spheres.h"
#include "../hydra/fallback_material.h"
#include "../mdl_material.h"

#include "../rng.h"
#include "../scene.h"

#include <tbb/blocked_range.h>
#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <unordered_set>

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

// A material whose cutout opacity is 1 everywhere: shadow rays stop at it.
bool AlwaysBlocks(Material* material)
{
    if (!material) return true;
    if (auto* mdl = dynamic_cast<MDLMaterial*>(material)) {
        const auto cutout = mdl->constant_cutout();
        return !mdl->is_valid() || (cutout && *cutout >= 1.0f);
    }
    if (auto* fallback = dynamic_cast<prism::hydra::FallbackMaterial*>(material))
        return fallback->opacity() >= 1.0f;
    return true; // Material::cutoutOpacity is 1
}

bool AllMaterialsBlock(SceneManager& scene)
{
    std::unordered_set<Material*> checked;
    auto blocks = [&](Material* material) {
        return !checked.insert(material).second || AlwaysBlocks(material);
    };
    for (auto const& [id, geometry] : scene.geometry) {
        if (auto binding = scene.placement_materials.find(id); binding != scene.placement_materials.end()) {
            if (!blocks(binding->second.fallback)) return false;
            for (auto const& [primitive, material] : binding->second.triangles)
                if (!blocks(material)) return false;
        }
        if (auto* mesh = dynamic_cast<Mesh*>(geometry)) {
            if (!blocks(mesh->material_override)) return false;
            for (auto const& [index, material] : mesh->materials)
                if (!blocks(material)) return false;
        } else if (auto* sphere = dynamic_cast<Sphere*>(geometry)) {
            if (!blocks(sphere->material)) return false;
        } else if (auto* quad = dynamic_cast<Quad*>(geometry)) {
            if (!blocks(quad->material)) return false;
        } else if (auto* box = dynamic_cast<Box*>(geometry)) {
            if (!blocks(box->material)) return false;
        } else if (auto* batch = dynamic_cast<BatchedSpheres*>(geometry)) {
            for (auto const& entry : batch->entries)
                if (!blocks(entry.material)) return false;
        } else {
            return false; // unknown geometry: keep the general shadow loop
        }
    }
    return true;
}

// One sample of the shared path tracer; seed carries the pixel's random state.
// The sampler's sequence is indexed by `sample`, scrambled per pixel.
SampleResult TraceSample(SceneManager& scene, CpuSceneState const& state, CameraParams const& camera,
                         SampleSettings const& settings, unsigned x, unsigned y, unsigned width,
                         unsigned height, unsigned& seed, unsigned sample, unsigned pixelSeed)
{
    CpuPathTracer tracer;
    tracer.backend.scene = &scene;
    tracer.backend.opaqueScene = state.opaque;
    tracer.backend.lights = state.lights.get();
    tracer.backend.rng.state = seed;
    tracer.sampler = {sample, pixelSeed, 0u};
    const SampleResult result = tracer.renderSample(camera, x, y, width, height, settings);
    seed = tracer.backend.rng.state;
    return result;
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
        state->opaque = AllMaterialsBlock(scene);
        state->lights = std::make_shared<LightTable>(BuildLightTable(scene));
        m_state = std::move(state);
        m_stateOf = &scene;
    }
    return m_state;
}

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
    if (request.meanSamples) *request.meanSamples = request.samples;
    if (m_reference) {
        tbb::parallel_for(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                unsigned seed = tea<64>(unsigned(y*width) ^ request.seed, unsigned(x));
                Color3 sum(0);
                for (int sample = 0; sample < request.samples; ++sample) {
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

    const CameraParams camera = ToCameraParams(request.camera);
    const auto state = PreparedScene(scene);
    const SampleSettings settings = {request.sampleClamp, 1.0f, false, request.contributions,
        request.nee.probabilityMin, request.nee.fromDepth};
    const AdaptiveSettings& adaptive = request.adaptive;
    const size_t count = size_t(width) * height;
    std::vector<unsigned> seeds(count);
    std::vector<Color3> sums(count, Color3(0));
    std::vector<Color3> evenSums(adaptive.Enabled() ? count : 0, Color3(0));
    std::vector<int> samples(count, 0);
    ActivePixels active;
    active.Reset(unsigned(width), unsigned(height));
    // Without adaptive sampling this is one round of all samples per pixel.
    int done = 0;
    while (done < request.samples) {
        const int next = adaptive.Enabled()
            ? std::min(request.samples, done < adaptive.minSamples ? adaptive.minSamples : done + adaptive.step)
            : request.samples;
        tbb::parallel_for(0, height, [&](int y) {
            for (int x = 0; x < width; ++x) {
                const size_t pixel = size_t(y) * width + x;
                if (!active.Active(pixel)) continue;
                unsigned& seed = seeds[pixel];
                if (done == 0) seed = tea<64>(unsigned(y*width) ^ request.seed, unsigned(x));
                for (int sample = done; sample < next; ++sample) {
                    const auto result = TraceSample(scene, *state, camera, settings, unsigned(x),
                        unsigned(y), unsigned(width), unsigned(height), seed, unsigned(sample),
                        seedImage(unsigned(x), unsigned(y), unsigned(width), request.seed));
                    sums[pixel] += ToFloat3(result.color);
                    if (adaptive.Enabled() && sample % 2 == 0) evenSums[pixel] += ToFloat3(result.color);
                }
                samples[pixel] = next;
            }
        });
        done = next;
        if (done < request.samples && adaptive.CheckAfter(done)) {
            const float scale = ImageScale(count, [&](size_t pixel) {
                return (sums[pixel].x() + sums[pixel].y() + sums[pixel].z()) / float(samples[pixel]);
            });
            if (active.Update(adaptive.threshold, [&](size_t pixel) {
                    const float sum[3] = {sums[pixel].x(), sums[pixel].y(), sums[pixel].z()};
                    const float even[3] = {evenSums[pixel].x(), evenSums[pixel].y(), evenSums[pixel].z()};
                    return AdaptiveError(sum, even, done, scale);
                }) == 0)
                break;
        }
    }
    double total = 0.0;
    for (size_t pixel = 0; pixel < count; ++pixel) {
        Color3 value = sums[pixel];
        value /= float(samples[pixel]);
        for (int c = 0; c < 3; ++c) pixels[3 * pixel + c] = value[c];
        total += samples[pixel];
    }
    if (request.meanSamples) *request.meanSamples = total / double(count);
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
    const auto state = m_reference ? std::make_shared<const CpuSceneState>() : PreparedScene(scene);
    const SampleSettings settings = {request.shading.sampleClamp,
        request.shading.exposureScale, request.shading.transparentEscapes, kContributionAll,
        request.shading.nee.probabilityMin, request.shading.nee.fromDepth};

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
                            if (!m_reference) {
                                if (tracePrimary)
                                    centers[(y-tile.y)*tile.width+x-tile.x] =
                                        prism::MakeCameraRayWithLens(request.camera,
                                            static_cast<float>(x) + 0.5f,
                                            static_cast<float>(y) + 0.5f,
                                            width, height, nullptr);
                                const auto result = TraceSample(scene, *state, camera, settings, x, y, width,
                                    height, seed, unsigned(request.sample), seedPass(x, y, width, request.seed));
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
    const auto state = m_reference ? std::make_shared<const CpuSceneState>() : PreparedScene(scene);
    const SampleSettings settings = {request.shading.sampleClamp,
        request.shading.exposureScale, request.shading.transparentEscapes, kContributionAll,
        request.shading.nee.probabilityMin, request.shading.nee.fromDepth};
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
                const auto result = TraceSample(scene, *state, camera, settings, x, y, width, height,
                                                seed, 0u, seed);
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
