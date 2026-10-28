#pragma once

// Render sessions (docs/sdk_design.md, section 3.7): a device, render
// settings and a camera that render committed revisions of one scene into
// images (the colour and the requested AOVs). Two paths:
// - batch: render() renders the whole image to completion (samples, adaptive
//   sampling, time limit) and returns it, as the CLI does;
// - progressive: start() renders passes on the session's own thread (a
//   low-resolution preview, then full-resolution passes with adaptive
//   sampling and periodic denoising), the loop the Hydra delegate runs;
//   aov() and pick() read the last published pass while it runs, pause(),
//   resume(), cancel() and restart() control it, status() and wait() report.
//
// Errors are exceptions (prism::Error): InvalidArgument for misuse (no
// committed scene, no camera, invalid settings), Unsupported for a device or
// denoiser the build or machine lacks (with allowFallback false), Cancelled
// when the progress callback or cancel() stopped a render.
//
// A progressive session's methods may be called from any thread while it
// renders. The sessions of one scene render one at a time (their render
// settings live on the scene's committed revision): a progressive session
// yields to another session's render, and to Scene::commit, at its next
// pass boundary, and takes a new revision there (its accumulation restarts).

#include "scene.h"
#include "settings.h"
#include "types.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

class Renderer;

// The AOVs a session renders (src/output/aov.h). Their names are the Hydra
// AOV tokens (AovName).
enum class Aov {
    Color,        // "color": linear RGB radiance in the working space (3 floats)
    Depth,        // "depth": NDC depth in [0, 1], 1 on a miss (1 float)
    CameraDepth,  // "cameraDepth": distance along -Z in eye space, +inf on a miss
    Normal,       // "normal": world-space shading normal at the pixel centre (3 floats)
    Neye,         // "Neye": eye-space normal
    Peye,         // "Peye": eye-space position (0 on a miss)
    Pworld,       // "Pworld": world-space position (0 on a miss)
    St,           // "primvars:st": the hit's barycentric st (2 floats)
    PrimId,       // "primId": InstanceDesc::primId, -1 on a miss (1 Int32)
    ElementId,    // "elementId": the mesh's face, -1 on a miss (1 Int32)
    InstanceId,   // "instanceId": InstanceDesc::instanceId, -1 on a miss (1 Int32)
    Albedo,       // "albedo": the first non-delta vertex's albedo, over the colour's samples (3 floats)
    Diffuse,      // "diffuse": the diffuse share of the colour
    Specular,     // "specular": the specular share of the colour
    Emission,     // "emission": emission seen directly
    Denoised,     // "denoised": the colour, denoised (3 floats)
};

// The Hydra AOV token of an AOV ("color", "primvars:st", "primId", ...).
std::string_view AovName(Aov aov);
// The AOV of a Hydra AOV token ("P" is Pworld); nullopt for an unknown name.
std::optional<Aov> AovFromName(std::string_view name);

// Shutter interval in time codes around the frame time; reserved (section 5):
// accepted, one instant renders until motion blur exists.
struct Shutter {
    double open = 0.0, close = 0.0;
};

struct RenderSettings {
    // Samples per pixel; with adaptive sampling the per-pixel maximum.
    int samples = 2048;
    // Adaptive sampling: a pixel stops once its error estimate is below the
    // threshold (0 disables: every pixel gets `samples`), checked from
    // adaptiveMinSamples on.
    float adaptiveThreshold = 0.01f;
    int adaptiveMinSamples = 64;
    // Wall-clock limit of the sampling in seconds, without scene and device
    // preparation (0: none). Every pixel gets at least one sample.
    double timeLimitSeconds = 0.0;
    int maxDepth = 32, rouletteDepth = 4;
    uint32_t seed = 0;
    // Samples whose brightest channel exceeds this are scaled down (0: off).
    float sampleClamp = 0.0f;
    // Exposure in stops, on top of the camera's exposureScale; scales the
    // colour, the denoised colour and the radiance AOVs (not the albedo).
    float exposure = 0.0f;
    // Environment lighting: multiple importance sampling (true) or BSDF
    // sampling only.
    bool environmentMis = true;
    TextureFiltering textureFiltering = TextureFiltering::Filtered;
    // USD doubleSided: back faces of single-sided meshes are not hit, unless
    // their material transmits.
    bool honorDoubleSided = false;
    NeeSettings nee;
    // GPU devices that stream textures load the mip levels shading asks for;
    // false uploads whole textures.
    bool textureStreaming = true;
    // GPU texture budget in MiB (0: 45% of device memory).
    int gpuTextureBudgetMiB = 0;
    // Noise analysis: the light contributions kept (1 visible, 2 direct,
    // 4 indirect; 7 all).
    unsigned debugContributions = 7;
    Shutter shutter;  // reserved (section 5)
};

// A rectangle of pixels, top row first.
struct Window {
    unsigned x = 0, y = 0, width = 0, height = 0;
    bool operator==(Window const&) const = default;
};

// The progressive path (start()): the delegate's interactive loop.
struct ProgressiveOptions {
    // A one-sample preview at reduced resolution first: about
    // previewTargetMs long (adaptivePreview, measured on the previous one)
    // and at most previewMaxDimension pixels on its long edge (0: 512 on GPU
    // devices, 96 on the CPU).
    bool adaptivePreview = true;
    int previewTargetMs = 33;
    int previewMaxDimension = 0;
    // The wait between the preview and the full-resolution passes (the
    // delegate's prism:refinementDelayMs lets camera input settle).
    int refinementDelayMs = 0;
    // With denoising: the Denoised AOV (and with denoisedColor the colour)
    // updates every denoiseInterval passes and at the end.
    int denoiseInterval = 16;
    // While a GPU device prepares the scene (kernel compilation, upload), the
    // CPU renders and hands over when the GPU is ready; not for scenes whose
    // geometry takes more than gpuPreparationPreviewMaxMiB (0: any size).
    bool gpuPreparationPreview = true;
    int gpuPreparationPreviewMaxMiB = 8192;
};

struct SessionOptions {
    DevicePreference device = DevicePreference::Auto;
    // Without the preferred device: the CPU device (with a note, deviceNote())
    // or, when false, Error(Unsupported).
    bool allowFallback = true;
    // The display window; dataWindow (inside it) is the part rendered (a
    // crop); empty: all of it.
    unsigned width = 1280, height = 720;
    std::optional<Window> dataWindow;
    float pixelAspectRatio = 1.0f;
    ConformPolicy conformPolicy = ConformPolicy::ExpandAperture;
    // The camera: a camera of the scene, or a description (camera, if set,
    // wins). Its exposureScale scales the radiance outputs.
    CameraId camera;
    std::optional<CameraDesc> cameraDesc;
    std::vector<Aov> aovs{Aov::Color};
    RenderSettings settings;
    // Denoising (docs/denoising.md): with these settings the colour is
    // denoised (the Denoised AOV); asking for the Denoised AOV denoises with
    // the default settings. denoisedColor: the Color AOV is the denoised
    // colour (requires denoising).
    std::optional<DenoiseSettings> denoise;
    bool denoisedColor = false;
    ProgressiveOptions progressive;
};

// Host pixels with shared ownership (copies share them): width x height
// pixels of `channels` scalars, row-major, top row first.
class Image {
public:
    Image() = default;
    // Zero-filled pixels.
    Image(unsigned width, unsigned height, unsigned channels, DataType type = DataType::Float32);

    unsigned width() const noexcept { return m_width; }
    unsigned height() const noexcept { return m_height; }
    unsigned channels() const noexcept { return m_channels; }
    DataType type() const noexcept { return m_type; }  // Float32, or Int32 for id AOVs
    bool empty() const noexcept { return !m_data; }
    size_t pixelCount() const noexcept { return size_t(m_width) * m_height; }
    size_t valueCount() const noexcept { return pixelCount() * m_channels; }

    const void* data() const noexcept { return m_data.get(); }
    void* data() noexcept { return m_data.get(); }
    // Typed access; throws Error(InvalidArgument) for the other type.
    float const* floats() const;
    float* floats();
    int32_t const* ints() const;
    // Keeps the pixels alive (zero-copy wrappers).
    std::shared_ptr<const void> owner() const { return m_data; }

private:
    unsigned m_width = 0, m_height = 0, m_channels = 0;
    DataType m_type = DataType::Float32;
    std::shared_ptr<void> m_data;
};

struct RenderResult {
    // The requested AOVs, dataWindow-sized.
    std::map<Aov, Image> aovs;
    // The display window and the part of it these pixels are.
    unsigned width = 0, height = 0;
    Window dataWindow;
    // Whether this render path traced (colour, light AOVs or denoising were
    // asked for); the statistics below are of that render.
    bool traced = false;
    double meanSamples = 0.0, fractionAtMaxSamples = 0.0;
    bool timeLimitReached = false;
    double renderSeconds = 0.0;      // tracing and geometric AOVs
    double denoiseMilliseconds = 0.0;
    std::string denoiser;            // the denoiser's name, when one ran

    bool has(Aov aov) const { return aovs.count(aov) != 0; }
    // Throws Error(NotFound) when the AOV was not rendered.
    Image const& aov(Aov aov) const;
};

// What a pixel centre sees (Session::pick): the instance and its ids, the
// surface point and normal in world space, and the NDC depth.
struct Pick {
    bool hit = false;
    InstanceId instance;
    int32_t primId = -1, instanceId = -1, elementId = -1;
    uint64_t userData = 0;
    Float3 position{0.0f}, normal{0.0f};
    float depth = 1.0f;
};

enum class SessionState {
    Idle,       // no progressive render yet
    Running,    // preparing the device, previewing or refining
    Paused,
    Converged,  // the accumulation is complete (samples, adaptive, time limit)
    Cancelled,
    Failed,     // SessionStatus::error says why; wait() rethrows it
};

struct SessionStatus {
    SessionState state = SessionState::Idle;
    // Completed full-resolution passes of the current accumulation and the
    // target (adaptive sampling stops converged pixels earlier).
    uint64_t samples = 0;
    int targetSamples = 0;
    // Overall progress in [0, 1] (1 when converged): pixels adaptive sampling
    // stopped count as done; with a time limit, at least its elapsed share.
    double fraction = 0.0;
    double activeFraction = 1.0;
    bool timeLimitReached = false;
    // Render progress timing of the current accumulation: the time spent
    // rendering it since it (re)started (the preview and the passes; scene
    // loading and device preparation excluded), the pixel-samples per second
    // over the last few passes, and the estimated seconds until it completes:
    // the remaining pixel-samples ((target - completed) passes over the
    // pixels adaptive sampling still traces) at that rate, at most the time
    // limit's remainder; -1 while unknown (preparing, or before two passes),
    // 0 once complete.
    double elapsedSeconds = 0.0;
    double samplesPerSecond = 0.0;
    double estimatedSecondsRemaining = -1.0;
    // Accumulations started (each restart counts) and the scene revision the
    // current one renders.
    uint64_t accumulations = 0;
    uint64_t revision = 0;
    std::string error;
};

class Session {
public:
    // Sessions come from Renderer::createSession. Copies share the session.
    Session(Session const&) = default;
    Session& operator=(Session const&) = default;
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    ~Session();

    // The device it renders on, and why it is not the preferred one (empty
    // when it is).
    DeviceInfo device() const;
    std::string deviceNote() const;

    SessionOptions const& options() const;
    // Changes take effect at the next render; a progressive render restarts
    // its accumulation with them.
    void setSettings(RenderSettings const& settings);
    void setCamera(CameraId camera);
    void setCamera(CameraDesc const& camera);
    void resize(unsigned width, unsigned height, std::optional<Window> dataWindow = {},
                float pixelAspectRatio = 1.0f, ConformPolicy policy = ConformPolicy::ExpandAperture);
    void setAovs(std::vector<Aov> const& aovs);
    void setDenoise(std::optional<DenoiseSettings> const& denoise, bool denoisedColor = false);

    // Device setup for the scene's committed revision (kernel compilation,
    // upload, light tables). Optional: render() does it.
    void prepare();

    // Renders the scene's last committed revision to completion. Blocking;
    // the callback runs on an internal thread. The sessions of one scene
    // render one at a time: a render (or prepare()) waits for another
    // session's on the same scene to finish (the render settings still live
    // on the scene's revision). Throws Error(Cancelled) when
    // the callback returned false or cancel() was called.
    RenderResult render(ProgressCallback const& progress = {});

    // Stops the running render (from any thread) at its next sample
    // boundary (a CPU render also between rows); render() then throws
    // Error(Cancelled). A progressive render stops at its next tile (state
    // Cancelled). Without a running render it does nothing.
    void cancel();

    // Progressive rendering on the session's own thread (render() throws
    // Error(InvalidArgument) while it is active, until cancel() and wait()).
    // Renders the scene's last committed revision: a preview, then
    // full-resolution passes until the accumulation converges. A new
    // commit, a setting change or restart() starts a new accumulation, also
    // after one converged. The callback runs on the session's thread at pass
    // boundaries (at most ten times a second) and once when an accumulation
    // completes; returning false cancels. On a running session start() only
    // replaces the callback; on a paused one it resumes; after a cancel or a
    // failure it starts over. Invalid options throw here; errors while
    // rendering set Failed.
    void start(ProgressCallback const& progress = {});
    // pause() stops at the next tile and keeps the accumulation; resume()
    // continues it.
    void pause();
    void resume();
    // A new accumulation from sample 0.
    void restart();
    // Waits until the progressive render is not running (converged, paused,
    // cancelled, failed or idle) or the timeout passed (negative: none), and
    // rethrows the error of a failed render.
    SessionStatus wait(double timeoutSeconds = -1.0) const;
    SessionStatus status() const;
    // The progressive render's last published pass of an AOV (before the
    // first pass, its preview, upscaled), dataWindow-sized; empty before
    // anything was published. The image shares nothing the render writes
    // later. Throws Error(NotFound) for an AOV the session does not render.
    Image aov(Aov aov) const;
    // What the pixel centre (x, y) of the progressive render's camera sees
    // in the scene revision it renders (top row first, data window
    // coordinates). Throws Error(InvalidArgument) before start() or outside
    // the image.
    Pick pick(unsigned x, unsigned y) const;

    struct Impl;

private:
    friend class Renderer;
    explicit Session(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

} // namespace prism
