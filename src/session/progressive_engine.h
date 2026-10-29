#pragma once

// The progressive render loop of a session (docs/sdk_design.md, 3.7 and
// step 5a): the device preparation (with the CPU preview that covers a GPU
// device's preparation), the low-resolution preview, the refinement delay,
// the full-resolution passes over refinement tiles with adaptive sampling
// and its publication, the time limit, texture-streaming restarts and the
// denoise interval. It moved here unchanged from the Hydra delegate's render
// thread. Sessions own it (src/session/session.cpp): the public
// prism::Session drives it with host films, and hdPrism's hosted session
// (session_internal.h) with the delegate's render buffers.
//
// A front end describes its outputs (AOV kinds), hands over a prepared view
// and settings, and receives pixels through a ProgressiveTarget
// (progressive_target.h). One accumulation's state (RNG states, adaptive
// sums, denoise sums, sample count) lives here and survives a stopped Run,
// which the next Run resumes; samples = 0 starts over.
//
// Internal to the core library (not exported): the render thread that calls
// Run is the session's own or the front end's (Hydra's render thread).

#include "../matrix4d.h"
#include "progressive_target.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

class SceneManager;

namespace prism {
class RendererContext;
}

namespace prism::session {

Color3 const &LightAovValue(device::LightAovPixel const &pixel, AovKind kind);

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

struct DenoiseJob;
class DenoiseWorker;

class ProgressiveEngine {
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
