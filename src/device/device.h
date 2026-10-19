#pragma once

// Render devices execute the per-pixel path tracing work for both frontends.
// Every device runs the shared integrator (shaders/integrator) on its own
// backend: the CPU device on Embree and the CPU materials, the GPU devices
// (Vulkan, Metal) in shading languages. The CPU reference device runs the
// original SceneManager::trace for validation.
// Frontends keep ownership of scene ingestion, accumulation, tiling policy and
// presentation, and describe each unit of work with a request struct.

#include "adaptive_sampling.h"
#include "../camera_rays.h"
#include "../float3.h"
#include "../ray.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SceneManager;

namespace prism::device {

// CpuReference runs the original CPU integrator (validation reference).
enum class DeviceKind { Cpu, CpuReference, Vulkan, Metal };

// Auto prefers the platform GPU device and falls back to the CPU device.
enum class DevicePreference { Auto, Cpu, CpuReference, Vulkan, Metal };

struct DeviceInfo {
    DeviceKind kind = DeviceKind::Cpu;
    std::string name;
    bool hardwareRayTracing = false;
};

const char* ToString(DeviceKind kind);
const char* ToString(DevicePreference preference);
bool ParseDevicePreference(std::string_view text, DevicePreference* preference);

// A screen-space block of pixels. Tiles are processed in the given order.
struct PixelTile {
    unsigned x = 0, y = 0, width = 0, height = 0;
    size_t index = 0;
};

// Probabilistic next-event estimation: from fromDepth on, environment light
// is sampled with probability clamp(path weight, probabilityMin, 1), which
// saves most NEE work on dim deep paths at a small noise cost. A
// probabilityMin of 0 always samples it.
struct NeeSettings {
    float probabilityMin = 0.05f;
    int fromDepth = 2;
    // Spatial light selection among analytic lights (PathTracer::pickLight).
    bool spatialLights = true;
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
    // Light contributions to keep (kContribution* mask; noise analysis).
    unsigned contributions = 7;
    NeeSettings nee;
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
    std::atomic<uint64_t>* colorPixels = nullptr;
    // Receives the time spent tracing, without scene preparation.
    double* traceMilliseconds = nullptr;
};

class RenderDevice {
public:
    virtual ~RenderDevice() = default;
    virtual DeviceInfo Info() const = 0;

    // Inform the device that a scene was (re)built or edited before rendering it.
    // The scene must stay alive and unmodified while the device renders it.
    virtual void SceneChanged(SceneManager& scene) { (void)scene; }

    virtual std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) = 0;
    virtual void RenderPass(SceneManager& scene, PassRequest const& request) = 0;
    // Returns false if stopped before every pixel was written.
    virtual bool RenderPreview(SceneManager& scene, PreviewRequest const& request) = 0;
    // Closest hit of a single ray (picking).
    virtual RTCRayHit Intersect(SceneManager& scene, prism::PathRay const& ray) = 0;
};

// Devices compiled into this build with a usable adapter, CPU always last.
std::vector<DeviceInfo> AvailableDevices();

// Creates the preferred device. If it is unavailable, falls back to the CPU
// device and explains why in *note (when non-null).
std::unique_ptr<RenderDevice> CreateDevice(DevicePreference preference,
                                           std::string* note = nullptr);

} // namespace prism::device
