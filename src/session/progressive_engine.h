#pragma once

// The progressive render loop of a session (docs/sdk_design.md, 3.7 and
// step 5a): the device preparation (with the CPU preview that covers a GPU
// device's preparation), the low-resolution preview, the refinement delay,
// the full-resolution passes over refinement tiles with adaptive sampling
// and its publication, the time limit, texture-streaming restarts and the
// denoise interval. It moved here unchanged from the Hydra delegate's render
// thread (src/hydra/scene_bridge.cpp), which drives it with its render
// buffers; the public prism::Session drives it with host films
// (src/session/session.cpp).
//
// A front end describes its outputs (AOV kinds), hands over a prepared view
// and settings, and receives pixels through a ProgressiveTarget: the
// delegate's render buffers or the session's films. One accumulation's state
// (RNG states, adaptive sums, denoise sums, sample count) lives here and
// survives a stopped Run, which the next Run resumes; Restart() starts over.
//
// Internal: std and PrismRT types only (no USD); the render thread that
// calls Run is the front end's (Hydra's render thread, the session's).

#include "../internal_export.h"
#include "../camera_rays.h"
#include "../device/adaptive_sampling.h"
#include "../device/denoiser.h"
#include "../device/device.h"
#include "../matrix4d.h"
#include "../output/aov.h"
#include "refinement_tiles.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <tuple>
#include <vector>

class SceneManager;

namespace prism {
class RendererContext;
}

namespace prism::session {

using output::AovKind;

// Outputs of the path tracer: the colour, the light AOVs and the denoised
// colour (written by the denoiser, not by the passes).
inline bool TracedAov(AovKind kind)
{
    return kind == AovKind::Color || output::LightAov(kind) || kind == AovKind::Denoised;
}

Color3 const &LightAovValue(device::LightAovPixel const &pixel, AovKind kind);

// What a front end publishes with its pixels (the delegate's picking tables
// and camera); opaque to the loop, which compares and forwards it.
using Frame = std::shared_ptr<const void>;

// The settings of one accumulation (the delegate's render settings).
struct ProgressiveSettings {
    int samples = 1;
    int seed = 0;
    float sampleClamp = 0.0f;
    float adaptiveThreshold = 0.0f;
    int adaptiveMinSamples = 1;
    double timeLimitSeconds = 0.0;
    device::NeeSettings nee;
    // Escaped primary rays get zero colour and coverage (no visible dome).
    bool transparentEscapes = false;
    // Preview: sized to previewTargetMs (adaptivePreview) up to
    // previewMaxDimension (0: 512 on GPU devices, 96 on the CPU).
    bool adaptivePreview = true;
    int previewTargetMs = 33;
    int previewMaxDimension = 0;
    bool textureStreamingPrepass = false;
    // Denoising (docs/denoising.md): `denoise` shows the colour denoised (the
    // target's PresentDenoisedColor); a bound Denoised output denoises
    // whatever it says.
    bool denoise = false;
    int denoiseInterval = 16;
    device::DenoiseSettings denoiseSettings;
    // While a GPU device prepares, render on the CPU and hand over.
    bool gpuPreparationPreview = true;
    int gpuPreparationPreviewMaxMiB = 8192;
};

// The camera of one accumulation.
struct ProgressiveView {
    CameraRayParameters rays;
    float exposureScale = 1.0f;
    unsigned width = 0, height = 0;
};

// The tiles a pass finished since the last presentation (the delegate shows
// them before the pass ends).
struct PassTiles {
    std::span<const RefinementTile> tiles;  // all tiles of the pass
    std::span<const size_t> completed;      // positions in `tiles`
    unsigned width = 0, height = 0;
    int sample = 0;
    Color3 const *colors = nullptr;          // this pass's samples, top row first
    float const *coverage = nullptr;
    // Pixels this pass traced (adaptive sampling skips converged ones).
    std::function<bool(size_t pixel)> traced;
};

// Where a front end's outputs go. `output` indexes the outputs given to Run.
// Rows are top-first as the loop renders them unless a method says
// otherwise; the delegate flips them into its bottom-up buffers.
class ProgressiveTarget {
public:
    virtual ~ProgressiveTarget() = default;

    // Control, polled by the loop and the device.
    virtual bool StopRequested() const = 0;
    virtual bool PauseRequested() const = 0;
    // At the top of each full-resolution pass, before the stop and pause
    // checks (the session yields its scene lock here).
    virtual void PassBoundary() {}

    // The frame this accumulation publishes: the last published one when it
    // resumes, else a new one for the device.
    virtual Frame CurrentFrame(bool resumed, std::shared_ptr<device::RenderDevice> const &device) = 0;
    // A preview or pass published `frame`.
    virtual void FramePublished(Frame const &frame) = 0;
    // The refinement delay in milliseconds after a preview, and when the
    // quiet interval started (in: when the preview ended).
    virtual int RefinementDelay(std::chrono::steady_clock::time_point &start) = 0;
    // The output's kind is not supported (once per name).
    virtual void WarnUnsupported(size_t output) = 0;

    // Accumulation storage of an output. bufferY counts from the bottom.
    virtual void BeginFrame(size_t output, bool clearWorking) = 0;
    virtual void WriteFloat(size_t output, unsigned x, unsigned bufferY, const float *values, size_t count,
                            bool accumulate) = 0;
    virtual void WriteInt(size_t output, unsigned x, unsigned bufferY, const int *values, size_t count) = 0;
    virtual void KeepAccumulated(size_t output, unsigned x, unsigned bufferY) = 0;
    virtual void RecordWriteNanoseconds(size_t, uint64_t) {}
    virtual void Publish(size_t output, Frame const &frame) = 0;
    virtual void SetConverged(size_t output, bool converged) = 0;
    // Bytes of a full-resolution publication of the output (statistics).
    virtual uint64_t PublishedBytes(size_t output) const = 0;
    // A preview of previewWidth x previewHeight values, bottom row first.
    virtual void PublishPreviewFloat(size_t output, unsigned previewWidth, unsigned previewHeight,
                                     std::vector<float> const &values, size_t components, Frame const &frame,
                                     bool smooth) = 0;
    virtual void PublishPreviewInt(size_t output, unsigned previewWidth, unsigned previewHeight,
                                   std::vector<int> const &values, size_t components, Frame const &frame) = 0;
    // A finished denoise: RGBA, top row first.
    virtual void PublishDenoised(size_t output, std::vector<float> const &rgba, unsigned width, unsigned height,
                                 Frame const &frame) = 0;
    // With ProgressiveSettings::denoise: show the colour denoised (RGBA, top
    // row first).
    virtual void PresentDenoisedColor(std::vector<float> const &rgba, unsigned width, unsigned height,
                                      Frame const &frame) = 0;
    // A geometric or id AOV of a pixel-centre hit (values: the AOV's floats,
    // id: its integer).
    virtual void ResolvePrimary(AovKind kind, RayHit const &hit, float *values, int *id) const = 0;

    // Presentation while a pass runs (the delegate's refinement tiles): true
    // when some output shows the pass's tiles before it ends.
    virtual bool BeginPresentation(Frame const &, size_t /*tileCount*/) { return false; }
    // Packs the completed tiles and, unless none completed or a stop is
    // requested, publishes them: true when it did, with the time publishing
    // took in publicationMilliseconds.
    virtual bool PresentTiles(PassTiles const &, double & /*publicationMilliseconds*/) { return false; }
    virtual void EndPresentation() {}
    // When Run returns: drop presentation state.
    virtual void ClearPresentation() {}
};

// One output of the front end: its kind, and whether it has storage (the
// delegate's bindings to buffers of other renderers have none).
struct ProgressiveOutput {
    AovKind kind = AovKind::Unknown;
    bool bound = true;
};

struct ProgressiveRun {
    // The device rendering the accumulation (once prepared).
    std::shared_ptr<device::RenderDevice> device;
    device::DevicePreference preference = device::DevicePreference::Auto;
    // The scene, owned by its holder (the delegate's snapshot).
    std::shared_ptr<SceneManager> scene;
    std::vector<ProgressiveOutput> outputs;
    ProgressiveSettings settings;
    ProgressiveView view;
    // For load profile marks.
    uint64_t generation = 0;
    // DumpCameraRayDiagnostics's label.
    const char *diagnosticsLabel = "session";
};

// Statistics under the engine's mutex (the delegate reports them).
struct ProgressiveStats {
    uint64_t previewPublishes = 0;
    uint64_t refinementPublishes = 0;
    double previewMilliseconds = 0.0;
    // Tracing plus publication of the last preview: what adaptive preview
    // sizes against.
    double previewCostMilliseconds = 0.0;
    double previewRayWorkMilliseconds = 0.0;
    double previewPublicationMilliseconds = 0.0;
    uint64_t previewPublishedBytes = 0;
    uint64_t previewTracedPixels = 0;
    uint64_t previewBoundAovCount = 0;
    unsigned previewWidth = 0, previewHeight = 0;
    uint64_t firstPassPrimaryScratchBytes = 0;
    uint64_t colorPassScratchBytes = 0;
    uint64_t colorRngStorageBytes = 0;
    double lastPassTraceMilliseconds = 0;
    double lastPassCommitMilliseconds = 0;
    double lastPassRayWorkerMilliseconds = 0;
    double lastPassAovWorkerMilliseconds = 0;
    double lastPassWaitMilliseconds = 0;
    double lastPassPayloadMilliseconds = 0;
    double lastPassPublicationMilliseconds = 0;
    double devicePreparationSeconds = 0.0;
    bool devicePreparing = false;
    std::string denoiserName;
    // Render progress timing of the current accumulation:
    // - elapsedSeconds: time spent rendering it since it (re)started (the
    //   preview and the passes; scene loading and device preparation
    //   excluded);
    // - samplesPerSecond: pixel-samples per second over the last few passes
    //   (0 before the first);
    // - estimatedSecondsRemaining: the remaining pixel-samples ((target -
    //   completed) passes over the pixels adaptive sampling still traces) at
    //   that rate, at most the time limit's remainder; -1 while unknown
    //   (preparing, or before two passes), 0 once the accumulation is done.
    double elapsedSeconds = 0.0;
    double samplesPerSecond = 0.0;
    double estimatedSecondsRemaining = -1.0;
};

struct DenoiseJob;
class DenoiseWorker;

class PRISM_INTERNAL_API ProgressiveEngine {
public:
    explicit ProgressiveEngine(std::shared_ptr<RendererContext> renderer);
    ~ProgressiveEngine();
    ProgressiveEngine(ProgressiveEngine const &) = delete;
    ProgressiveEngine &operator=(ProgressiveEngine const &) = delete;

    // How a created device is shown in progress (and the delegate's stats):
    // "cpu", "vulkan: <name>", or "cpu (GPU fallback: <reason>)".
    static std::string DescribeDevice(device::RenderDevice const &device, device::DevicePreference preference,
                                      std::string const &note);

    // The front end replaced its device: the scene must reach the new one.
    void DeviceReplaced();
    // Tells the device about the scene when it, its generation or the
    // integrator settings the device copies changed since it last saw them.
    void NoteScene(device::RenderDevice &device, SceneManager &scene, uint64_t generation);

    enum class Outcome { Stopped, Finished };
    // Renders the accumulation (resuming one stopped before) until it
    // converges (Finished) or the target asks to stop (Stopped). Calls
    // SetConverged only through the front end (it knows what it displays).
    Outcome Run(ProgressiveRun const &run, ProgressiveTarget &target);

    ProgressiveStats Stats() const;
    // Waits for device jobs (device preparation) to finish.
    void WaitForJobs();

    // The accumulation's completed passes; zero starts over at the next Run.
    std::atomic<uint64_t> samples{0};
    // Progress of the accumulation (percentDone): the fraction of pixels
    // adaptive sampling still traces, and the elapsed fraction of the time
    // limit.
    std::atomic<double> activePixelFraction{1.0};
    std::atomic<double> timeLimitFraction{0.0};
    std::atomic<bool> timeLimitReached{false};
    std::atomic<uint64_t> denoisesPublished{0};
    std::atomic<double> lastDenoiseMilliseconds{0.0};
    std::atomic<uint64_t> colorTracedPixels{0};
    std::atomic<uint64_t> geometricOnlyPixels{0};
    // CPU preview while a GPU device prepares.
    std::atomic<bool> gpuPreviewActive{false};
    std::atomic<uint64_t> gpuPreviewSamples{0};
    std::atomic<uint64_t> gpuPreviewHandovers{0};
    std::atomic<uint64_t> gpuPreviewHandoverSample{0};
    // Accumulation restarts after texture streaming made textures finer.
    std::atomic<uint64_t> textureStreamingRestarts{0};
    std::atomic<uint64_t> devicePreparationsInterrupted{0};
    std::atomic<uint64_t> devicePreparations{0};
    // Embree memory of the scene (the GPU preparation preview is skipped
    // above gpuPreparationPreviewMaxMiB); the front end keeps it.
    std::atomic<int64_t> const *embreeLiveBytes = nullptr;
    // PRISM_PROFILE_BUFFERS: per-output write timing.
    bool profileBufferWrites = false;

private:
    enum class PreparationOutcome { Stopped, Ready, Preview };
    bool AdvanceDevicePreparation(std::shared_ptr<device::RenderDevice> const &device,
                                  std::shared_ptr<SceneManager> const &prepared);
    bool PrepareRenderDevice(std::shared_ptr<device::RenderDevice> const &device,
                             std::shared_ptr<SceneManager> const &prepared, ProgressiveTarget const &target);
    PreparationOutcome PrepareOrPreview(std::shared_ptr<device::RenderDevice> const &device,
                                        std::shared_ptr<SceneManager> const &prepared,
                                        ProgressiveSettings const &settings, ProgressiveTarget const &target);
    void BeginGpuPreview();
    void EndGpuPreview(device::RenderDevice const &gpu, device::DevicePreference preference);
    void ResetDenoise(size_t pixels);

    std::shared_ptr<RendererContext> m_renderer;
    mutable std::mutex m_mutex;  // statistics and the device epochs
    ProgressiveStats m_stats;    // under m_mutex (devicePreparing is computed)

    // Device preparation (GPU kernel compilation, scene and texture upload)
    // can take tens of seconds. It runs as a job the render thread waits on
    // while polling for a stop, so a host stop never waits for it; an
    // interrupted job keeps running and the next Run resumes waiting on it.
    SceneManager const *m_deviceScene = nullptr;
    uint64_t m_deviceGeneration = 0;
    std::tuple<int, int, bool, int, bool, int> m_deviceSettings{};
    // Advanced (under m_mutex) whenever the device or its view of the scene
    // changes; m_devicePreparedEpoch records the last epoch fully prepared.
    uint64_t m_deviceSceneEpoch = 1;
    uint64_t m_devicePreparedEpoch = 0;
    std::future<void> m_devicePreparation;
    uint64_t m_devicePreparationEpoch = 0;
    std::chrono::steady_clock::time_point m_devicePreparationStart;
    std::shared_ptr<device::RenderDevice> m_previewDevice;
    int m_gpuPreviewDelayMs = 1000;
    int m_gpuPreviewThreads = 0;  // 0: a fraction of the cores

    // The accumulation (render thread only).
    std::vector<unsigned int> m_committedRngStates;
    // Adaptive sampling: per-pixel RGB sums of all and of even-numbered
    // samples and the sample count (7 floats per pixel), and the pixels
    // still sampled.
    std::vector<float> m_adaptiveSums;
    device::ActivePixels m_adaptiveActive;
    // Denoising: the accumulation's sums as the denoiser takes them and
    // their sample counts, kept while it resumes; the latest denoise (in
    // flight, or done and maybe not yet published) and what it denoised.
    std::vector<float> m_denoiseSums;
    std::vector<uint32_t> m_denoiseCounts;
    std::shared_ptr<DenoiseJob> m_denoiseJob;
    uint64_t m_denoisedSamples = 0;
    device::DenoiseSettings m_denoisedSettings;
    bool m_denoiserWarned = false;
    std::unique_ptr<DenoiseWorker> m_denoiseWorker;
    // Time spent on the accumulation's full-resolution passes (the time
    // limit); kept while it resumes.
    double m_accumulationSeconds = 0.0;
    unsigned m_tileWidth = 0, m_tileHeight = 0;
    std::vector<RefinementTile> m_refinementTiles;

    // Render progress timing (under m_mutex; ProgressiveStats).
    struct Timing {
        bool preparing = false;   // inside Run, before rendering
        bool rendering = false;   // inside Run, past device preparation
        bool complete = false;    // the accumulation finished
        std::chrono::steady_clock::time_point since;  // while rendering
        double seconds = 0.0;     // rendering time before `since`
        struct Pass {
            double pixelSamples, seconds;
        };
        std::vector<Pass> passes;  // the last few
        uint64_t completedPasses = 0;
        size_t activePixels = 0;
        int targetSamples = 0;
        double timeLimit = 0.0;
    } m_timing;
    static constexpr size_t kRatePasses = 4;
};

} // namespace prism::session
