#pragma once

// Internal access to prism::Session for a front end that builds its own
// scene revisions (hdPrism), not part of the public API (docs/DECISIONS.md,
// 2026-10-29: Hydra renders through Session, keeping its own scene path).
//
// A hosted session has no public Scene: each run renders the SceneManager
// the front end hands it (the delegate's snapshot) into the front end's
// ProgressiveTarget (its render buffers, presentation tiles and picking
// frames), on the front end's render thread. The session owns the rest of
// what a progressive render needs: the render device (created as a job the
// render thread waits on while polling for a stop), the progressive loop
// with its accumulation, and the session state (accumulations, state,
// progress).

#include "../internal_export.h"
#include <prism/session.h>

#include "progressive_target.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SceneManager;

namespace prism {
class RendererContext;
}

namespace prism::detail {

// One run of a hosted session.
struct HostedRun {
    // The scene, owned by its holder (the delegate's snapshot); the run and
    // the device's jobs keep it alive.
    std::shared_ptr<SceneManager> scene;
    device::DevicePreference preference = device::DevicePreference::Auto;
    std::vector<session::ProgressiveOutput> outputs;
    session::ProgressiveSettings settings;
    session::ProgressiveView view;
    // The front end's scene generation (load profile marks).
    uint64_t generation = 0;
    // DumpCameraRayDiagnostics's label.
    const char* diagnosticsLabel = "hydra";
};

// The accumulation's counters (atomics: read without locks).
struct HostedCounters {
    uint64_t samples = 0;  // completed passes
    double activePixelFraction = 1.0;
    double timeLimitFraction = 0.0;
    bool timeLimitReached = false;
    uint64_t denoisesPublished = 0;
    double lastDenoiseMilliseconds = 0.0;
    uint64_t colorTracedPixels = 0;
    uint64_t geometricOnlyPixels = 0;
    bool gpuPreviewActive = false;
    uint64_t gpuPreviewSamples = 0;
    uint64_t gpuPreviewHandovers = 0;
    uint64_t gpuPreviewHandoverSample = 0;
    uint64_t textureStreamingRestarts = 0;
    uint64_t devicePreparations = 0;
    uint64_t devicePreparationsInterrupted = 0;
    // PRISM_PROFILE_BUFFERS: per-output write timing.
    bool profileBufferWrites = false;
};

// What acquireDevice did.
struct HostedDevice {
    // Null when a stop came before the device was ready.
    std::shared_ptr<device::RenderDevice> device;
    // This call created it (the front end reports it); then `error` is why
    // the preferred device failed (the CPU replaced it) and `note` why
    // another device than the preferred one renders.
    bool created = false;
    std::string error, note;
};

struct PRISM_INTERNAL_API SessionAccess {
    SessionAccess() = delete;
    SessionAccess& operator=(SessionAccess const&) = delete;
    // A session over a front end's own scene revisions. embreeLiveBytes: the
    // front end's Embree memory of the scene (the GPU preparation preview is
    // skipped above its limit); it must outlive the session.
    static Session createHosted(std::shared_ptr<RendererContext> context,
                                std::atomic<int64_t> const* embreeLiveBytes);
    // The render device for `preference`: the current one, else created (on
    // first use or when the preference changed) by a job the calling thread
    // waits on, returning a null device as soon as stopRequested() holds
    // (the job keeps running and the next call waits on it again). A failed
    // creation falls back to the CPU device.
    static HostedDevice acquireDevice(Session& session, device::DevicePreference preference,
                                      std::function<bool()> const& stopRequested);
    // The current device (null before acquireDevice made one).
    static std::shared_ptr<device::RenderDevice> device(Session const& session);
    // Tells the current device about `scene` when it, its generation or the
    // integrator settings the device copies changed since it last saw them.
    static void noteScene(Session& session, SceneManager& scene, uint64_t generation);
    // Renders the accumulation (resuming one a stop interrupted) on the
    // current device until it converges (true) or the target stops it
    // (false), on the calling thread (the target's writes also come from
    // the passes' TBB workers, as with the public session's films).
    static bool runHosted(Session& session, HostedRun const& run, session::ProgressiveTarget& target);
    // The next run starts a new accumulation.
    static void restartAccumulation(Session& session);
    static HostedCounters counters(Session const& session);
    // Statistics under the loop's mutex.
    static session::ProgressiveStats stats(Session const& session);
    // Waits for device jobs (creation, preparation) to finish.
    static void waitForJobs(Session& session);
};

// The fraction of a progressive render that is done (SessionStatus::fraction,
// the delegate's percentDone before convergence): pixels adaptive sampling
// stopped count as done and the others by their samples; with a time limit,
// at least its elapsed fraction.
PRISM_INTERNAL_API double ProgressFraction(uint64_t samples, int targetSamples, double activeFraction,
                                           double timeLimitSeconds, double timeLimitFraction);

} // namespace prism::detail
