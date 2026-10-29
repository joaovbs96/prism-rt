#pragma once

// What a front end hands the progressive render loop and what it gets back
// (docs/sdk_design.md, 3.7): its outputs, settings and view, the
// ProgressiveTarget its pixels go to, and the loop's statistics. The loop
// itself (ProgressiveEngine, progressive_engine.h) is internal to the core;
// the public prism::Session drives it with host films, and hdPrism through a
// hosted session (session_internal.h) with its render buffers.
//
// Internal: std and PrismRT types only (no USD).

#include "../camera_rays.h"
#include "../device/adaptive_sampling.h"
#include "../device/denoiser.h"
#include "../device/device.h"
#include "../output/aov.h"
#include "refinement_tiles.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace prism::session {

using output::AovKind;

// Outputs of the path tracer: the colour, the light AOVs and the denoised
// colour (written by the denoiser, not by the passes).
inline bool TracedAov(AovKind kind)
{
    return kind == AovKind::Color || output::LightAov(kind) || kind == AovKind::Denoised;
}

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

} // namespace prism::session
