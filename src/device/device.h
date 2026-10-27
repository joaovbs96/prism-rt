#pragma once

// Render devices execute the per-pixel path tracing work for both frontends.
// Every device runs the shared integrator (shaders/integrator) on its own
// backend: the CPU device on Embree and the CPU materials, the GPU devices
// (Vulkan, Metal) in shading languages.
// Frontends keep ownership of scene ingestion, accumulation, tiling policy and
// presentation, and describe each unit of work with a request struct.

#include "adaptive_sampling.h"
#include "denoiser.h"
#include "../camera_rays.h"
#include "../float3.h"
#include "../ray.h"
#include "../renderer/renderer_options.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SceneManager;

namespace prism::device {

using prism::DeviceOptions;

enum class DeviceKind { Cpu, Vulkan, Metal };

// Auto prefers the platform GPU device and falls back to the CPU device.
enum class DevicePreference { Auto, Cpu, Vulkan, Metal };

struct DeviceInfo {
    DeviceKind kind = DeviceKind::Cpu;
    std::string name;
    bool hardwareRayTracing = false;
};

const char* ToString(DeviceKind kind);
const char* ToString(DevicePreference preference);
bool ParseDevicePreference(std::string_view text, DevicePreference* preference);
// Why a device name is not accepted ("the cpu-reference device was removed;
// use cpu" for that former device, empty for an ordinary unknown name).
std::string RemovedDeviceMessage(std::string_view text);

// A screen-space block of pixels. Tiles are processed in the given order.
struct PixelTile {
    unsigned x = 0, y = 0, width = 0, height = 0;
    size_t index = 0;
};

// Probabilistic next-event estimation: from fromDepth on, lights are sampled
// with probability clamp(path weight, probabilityMin, 1), which saves NEE
// work on dim deep paths at a noise cost. A probabilityMin of 0 (the default)
// always samples them: skipping NEE raised the MSE of CornellBox by 40% for 1%
// of time, and was about neutral on dome-lit and subsurface scenes.
struct NeeSettings {
    float probabilityMin = 0.0f;
    int fromDepth = 2;
    // Spatial light selection among analytic lights (PathTracer::pickLight).
    bool spatialLights = true;
};

// The light AOVs of a sample, or their mean over a pixel's samples
// (LightAovSample, shaders/integrator/path_tracer.h): diffuse + specular +
// emission is the colour; the albedo is that of the first non-delta vertex,
// and the normal (world space, the denoiser's guide) that vertex's shading
// normal.
struct LightAovPixel {
    Color3 albedo, diffuse, specular, emission;
    Float3 normal;
};

// Per-sample post-processing shared by every render loop.
struct SampleShading {
    // Samples whose brightest channel exceeds this are scaled down (0 disables).
    float sampleClamp = 0.0f;
    float exposureScale = 1.0f;
    // Escaped primary rays get zero color and zero coverage.
    bool transparentEscapes = false;
    NeeSettings nee;
};

// Progress of a whole-image render, written by the device at row and sample
// boundaries (relaxed atomics, no locks) and polled by the frontend (the
// CLI's progress bar).
struct ImageProgress {
    // Samples traced per still-active pixel; fractional within a round of
    // samples (the share of its pixels done).
    std::atomic<double> samples{0.0};
    // Fraction of pixels adaptive sampling still traces (1 without it).
    std::atomic<double> activeFraction{1.0};
    std::atomic<int> target{1};

    // Overall progress in [0, 1]: pixels adaptive sampling stopped count as
    // done, the others by their samples out of the target (the same measure
    // as the Hydra delegate's percentDone).
    double Fraction() const
    {
        const double active = activeFraction.load(std::memory_order_relaxed);
        const double done = samples.load(std::memory_order_relaxed) /
            double(std::max(1, target.load(std::memory_order_relaxed)));
        const double fraction = (1.0 - active) + active * std::min(1.0, done);
        return fraction < 0.0 ? 0.0 : fraction > 1.0 ? 1.0 : fraction;
    }
};

// Whole-image render with all samples of a pixel accumulated at once
// (standalone frontend). Writes width*height RGB triples, top row first.
struct ImageRequest {
    prism::CameraRayParameters camera;
    unsigned width = 0, height = 0;
    int samples = 1;
    unsigned seed = 0;
    float sampleClamp = 0.0f;
    // With adaptive sampling, `samples` is the per-pixel maximum.
    AdaptiveSettings adaptive;
    // Receives the mean number of samples per pixel (when non-null).
    double* meanSamples = nullptr;
    // Receives the fraction of pixels that received `samples` samples
    // (adaptive sampling did not stop them first).
    double* fractionAtMaxSamples = nullptr;
    // Wall-clock limit in seconds for the sampling, without scene preparation
    // (0: none). Every pixel gets at least one sample; then sampling stops at
    // the first sample boundary past the limit.
    double timeLimitSeconds = 0.0;
    // Set to whether the time limit stopped the render (when non-null).
    bool* timeLimitReached = nullptr;
    // Updated while the render runs (when non-null).
    ImageProgress* progress = nullptr;
    // Light contributions to keep (kContribution* mask; noise analysis).
    unsigned contributions = 7;
    NeeSettings nee;
    // When non-null, also renders the light AOVs: receives width*height
    // means over the same samples as the colour, top row first.
    std::vector<LightAovPixel>* lightAovs = nullptr;
    // When `denoised` is non-null, also denoises the colour with `denoise`
    // (docs/denoising.md): *denoised receives width*height RGB triples like
    // the returned colour, guided by the light AOVs' albedo and normal
    // (rendered for it even when lightAovs is null). The device denoises
    // where its accumulation lives (the Vulkan device on the GPU with
    // vk_oidn, the Metal device with OIDN's Metal device;
    // RenderDevice::CreateDenoiser). Throws when the build has no
    // denoiser.
    DenoiseSettings denoise;
    std::vector<float>* denoised = nullptr;
    // Receive the denoising time in milliseconds and the denoiser's name
    // (Denoiser::Name), when non-null.
    double* denoiseMilliseconds = nullptr;
    std::string* denoiser = nullptr;
};

// One progressive pass: one sample per pixel over the given tiles. All
// full-frame arrays are width*height, top row first, and must be allocated.
struct PassRequest {
    prism::CameraRayParameters camera;
    unsigned width = 0, height = 0;
    unsigned seed = 0;
    int sample = 0;
    SampleShading shading;
    std::span<const PixelTile> tiles;
    // When set, color samples are traced only for pixels whose flag is
    // nonzero (adaptive sampling); width*height flags, top row first.
    const uint8_t* activePixels = nullptr;
    // Trace one color sample per pixel; false traces only primary hits.
    bool traceColor = true;
    // Resolve pixel-center primary hits (deterministic geometric outputs).
    bool tracePrimary = false;
    // Persistent per-pixel RNG states; initialized by the device on sample 0.
    unsigned int* rngStates = nullptr;
    Color3* colors = nullptr;
    float* coverage = nullptr;
    // When non-null, also the light AOVs of each traced pixel's sample
    // (width*height, like colors).
    LightAovPixel* lightAovs = nullptr;
    // Polled between rows and tiles; returning true ends the pass early.
    std::function<bool()> abandoned;
    // While true, workers wait between tiles (unless abandoned).
    std::function<bool()> paused;
    // Receives the pixel-center primary hit of pixel (x, y), top row first.
    // May be called concurrently for different pixels.
    std::function<void(unsigned x, unsigned y, RayHit const& hit)> resolvePrimary;
    // Called with the tile's position in `tiles` once its color and primary
    // outputs are complete.
    std::function<void(size_t position)> tileFinished;
    // Worker threads available to the pass (CPU device).
    int concurrency = 1;
    std::atomic<uint64_t>* colorPixels = nullptr;
    std::atomic<uint64_t>* geometricOnlyPixels = nullptr;
    std::atomic<uint64_t>* rayNanoseconds = nullptr;
    std::atomic<uint64_t>* primaryNanoseconds = nullptr;
};

// A reduced-resolution single-sample preview of a width x height raster.
struct PreviewRequest {
    prism::CameraRayParameters camera;
    unsigned width = 0, height = 0;
    unsigned previewWidth = 0, previewHeight = 0;
    unsigned seed = 0;
    SampleShading shading;
    bool tracePrimary = false;
    std::function<bool()> stopRequested;
    // Called per preview pixel (px, py top row first); primary may be null.
    std::function<void(unsigned px, unsigned py, Color3 const& color, float coverage,
                       RayHit const* primary)> writePixel;
    // When set, also renders the light AOVs and calls this per preview pixel
    // (after writePixel).
    std::function<void(unsigned px, unsigned py, LightAovPixel const& aovs)> writeLightAovs;
    std::atomic<uint64_t>* colorPixels = nullptr;
    // Receives the time spent tracing, without scene preparation.
    double* traceMilliseconds = nullptr;
};

// Texture streaming state of a device that streams textures (Vulkan; see
// src/device/gpu/texture_streaming.h). All zero otherwise.
struct TextureStreamingStats {
    bool active = false;          // the current scene streams
    uint64_t budgetBytes = 0;     // for streamed images
    uint64_t residentBytes = 0;   // their resident levels (estimate)
    uint64_t imageBytes = 0;      // device memory of every image (allocated)
    uint64_t deviceBytes = 0;     // device-local memory the process uses (0: unknown)
    uint32_t images = 0;          // streamed images
    uint32_t requested = 0;       // requested finer than their tail
    uint32_t pending = 0;         // planned finer than resident
    uint32_t belowRequested = 0;  // kept coarser than requested (budget)
    uint64_t uploads = 0;         // images (re)created by streaming
    uint64_t uploadedBytes = 0;
    uint64_t feedbackRounds = 0;
    uint64_t restarts = 0;        // accumulation restarts asked for
};

class RenderDevice {
public:
    virtual ~RenderDevice() = default;
    virtual DeviceInfo Info() const = 0;

    // Texture streaming. Progressive frontends ask at each pass boundary
    // after the first pass of an accumulation: true means the samples so far
    // used textures coarser than the device holds now, and the frontend
    // should start the accumulation again (its next pass is sample 0). A
    // device asks for a bounded number of restarts per accumulation, once
    // the residency its feedback asked for is in place.
    virtual bool AccumulationOutdated(SceneManager& scene) { (void)scene; return false; }
    // Renders a few passes of `request` (camera, size, seed and shading; no
    // outputs) that only collect texture feedback, loads what they ask for,
    // and returns once residency is stable; the next accumulation then keeps
    // it unchanged, so all of its samples see the same textures (offline
    // rendering). False when the device does not stream textures.
    virtual bool SettleTextures(SceneManager& scene, PassRequest const& request)
    {
        (void)scene; (void)request;
        return false;
    }
    virtual TextureStreamingStats TextureStreaming() const { return {}; }

    // Inform the device that a scene was (re)built or edited before rendering it.
    // The scene must stay alive and unmodified while the device renders it.
    virtual void SceneChanged(SceneManager& scene) { (void)scene; }
    // Performs the per-scene setup that rendering would otherwise do on first
    // use (GPU kernel compilation, scene upload, light tables). Rendering
    // calls it implicitly; calling it first lets a host run that work on a
    // thread it can stop waiting for. Not concurrent with rendering calls;
    // SceneChanged may be called while it runs.
    virtual void Prepare(SceneManager& scene) { (void)scene; }

    virtual std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) = 0;
    virtual void RenderPass(SceneManager& scene, PassRequest const& request) = 0;
    // Returns false if stopped before every pixel was written.
    virtual bool RenderPreview(SceneManager& scene, PreviewRequest const& request) = 0;
    // Closest hit of a single ray (picking).
    virtual RTCRayHit Intersect(SceneManager& scene, prism::PathRay const& ray) = 0;

    // The denoiser for this device's images (docs/denoising.md); null with
    // the reason in *error when the build has none. The default is OIDN's CPU
    // device on host images (the CPU device's). The Vulkan device returns
    // vk_oidn's and the Metal device OIDN's Metal device when they can run
    // (host images are copied to the GPU), else this default.
    virtual std::shared_ptr<Denoiser> CreateDenoiser(std::string* error = nullptr);
};

// ImageRequest::denoised for a render's colour and light AOVs (width*height
// each), on the device's denoiser: the devices' RenderImage call it. Throws
// std::runtime_error when the device has no denoiser.
void DenoiseRenderedImage(RenderDevice& device, ImageRequest const& request,
                          std::vector<float> const& color, std::vector<LightAovPixel> const& aovs);

// Devices compiled into this build with a usable adapter and not disabled by
// `options`, CPU always last.
std::vector<DeviceInfo> AvailableDevices(DeviceOptions const& options);

// Diagnostics without a GPU device (PRISM_GPU_PROGRAM_REPORT): the scene's GPU programs and their sizes.
// False if this build has no Vulkan device.
bool ReportGpuPrograms(SceneManager& scene, std::ostream& out);

// Creates the preferred device. If it is unavailable, falls back to the CPU
// device and explains why in *note (when non-null). `options` disable
// devices and set their initial texture budget (the renderer's).
std::unique_ptr<RenderDevice> CreateDevice(DevicePreference preference, DeviceOptions const& options,
                                           std::string* note = nullptr);

} // namespace prism::device
