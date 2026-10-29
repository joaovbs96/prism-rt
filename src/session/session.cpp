// The public session (include/prism/session.h): the batch render path the
// CLI used to drive itself (docs/sdk_design.md, section 3.7), and the
// renderer methods that need devices.

#include <prism/renderer.h>
#include <prism/session.h>

#include "../camera/camera_projection.h"
#include "../camera_ray_diagnostics.h"
#include "../device/denoiser.h"
#include "../device/device.h"
#include "../output/aov.h"
#include "../load_profile.h"
#include "../render_defaults.h"
#include "../renderer/renderer_context.h"
#include "../scene.h"
#include "../scene/scene_internal.h"
#include "../texture_service.h"
#include "progressive_engine.h"
#include "session_internal.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

namespace prism {
namespace {

[[noreturn]] void Throw(ErrorCode code, std::string const& message) { throw Error(code, message); }

void Check(bool ok, std::string const& message)
{
    if (!ok) Throw(ErrorCode::InvalidArgument, message);
}

struct AovEntry {
    Aov aov;
    output::AovKind kind;
};
constexpr std::array<AovEntry, 16> kAovs{{
    {Aov::Color, output::AovKind::Color},
    {Aov::Depth, output::AovKind::Depth},
    {Aov::CameraDepth, output::AovKind::CameraDepth},
    {Aov::Normal, output::AovKind::Normal},
    {Aov::Neye, output::AovKind::Neye},
    {Aov::Peye, output::AovKind::Peye},
    {Aov::Pworld, output::AovKind::Pworld},
    {Aov::St, output::AovKind::St},
    {Aov::PrimId, output::AovKind::PrimId},
    {Aov::ElementId, output::AovKind::ElementId},
    {Aov::InstanceId, output::AovKind::InstanceId},
    {Aov::Albedo, output::AovKind::Albedo},
    {Aov::Diffuse, output::AovKind::Diffuse},
    {Aov::Specular, output::AovKind::Specular},
    {Aov::Emission, output::AovKind::Emission},
    {Aov::Denoised, output::AovKind::Denoised},
}};

output::AovKind KindOf(Aov aov)
{
    for (auto const& entry : kAovs)
        if (entry.aov == aov) return entry.kind;
    Throw(ErrorCode::InvalidArgument, "unknown AOV");
}

bool LightAov(Aov aov) { return aov == Aov::Albedo || aov == Aov::Diffuse || aov == Aov::Specular || aov == Aov::Emission; }

detail::SceneRenderSettings SceneSettings(RenderSettings const& settings)
{
    detail::SceneRenderSettings scene;
    scene.maxDepth = settings.maxDepth;
    scene.rouletteDepth = settings.rouletteDepth;
    scene.environmentSampling = settings.environmentMis;
    scene.textureFiltering = settings.textureFiltering;
    scene.honorDoubleSided = settings.honorDoubleSided;
    scene.textureStreaming = settings.textureStreaming;
    scene.gpuTextureBudgetMiB = defaults::ClampGpuTextureBudgetMiB(settings.gpuTextureBudgetMiB);
    return scene;
}

void Validate(RenderSettings const& s)
{
    Check(s.samples > 0, "render settings: samples must be positive");
    Check(s.maxDepth > 0 && s.rouletteDepth >= 0, "render settings: maxDepth must be positive and rouletteDepth nonnegative");
    Check(s.sampleClamp >= 0, "render settings: sampleClamp must be nonnegative");
    Check(s.nee.probabilityMin >= 0 && s.nee.probabilityMin <= 1 && s.nee.fromDepth >= 0,
          "render settings: nee.probabilityMin must be in [0, 1] and nee.fromDepth nonnegative");
    Check(s.adaptiveThreshold >= 0 && s.adaptiveMinSamples >= 1,
          "render settings: adaptiveThreshold must be nonnegative and adaptiveMinSamples positive");
    Check(s.timeLimitSeconds >= 0 && std::isfinite(s.timeLimitSeconds),
          "render settings: timeLimitSeconds must be a nonnegative number of seconds");
    Check(s.gpuTextureBudgetMiB >= 0, "render settings: gpuTextureBudgetMiB must be nonnegative");
    Check(std::isfinite(s.exposure), "render settings: exposure must be finite");
}

} // namespace

std::string_view AovName(Aov aov) { return output::FindAov(KindOf(aov))->name; }

std::optional<Aov> AovFromName(std::string_view name)
{
    const output::AovKind kind = output::AovKindFromName(name);
    for (auto const& entry : kAovs)
        if (entry.kind == kind) return entry.aov;
    return std::nullopt;
}

// ── Image

Image::Image(unsigned width, unsigned height, unsigned channels, DataType type)
    : m_width(width), m_height(height), m_channels(channels), m_type(type)
{
    Check(type == DataType::Float32 || type == DataType::Int32, "an image holds Float32 or Int32 values");
    const size_t count = size_t(width) * height * channels;
    if (type == DataType::Float32) {
        auto values = std::make_shared<std::vector<float>>(count, 0.0f);
        m_data = std::shared_ptr<void>(values, values->data());
    } else {
        auto values = std::make_shared<std::vector<int32_t>>(count, 0);
        m_data = std::shared_ptr<void>(values, values->data());
    }
}

float const* Image::floats() const
{
    Check(m_type == DataType::Float32, "the image does not hold floats");
    return static_cast<float const*>(m_data.get());
}
float* Image::floats()
{
    Check(m_type == DataType::Float32, "the image does not hold floats");
    return static_cast<float*>(m_data.get());
}
int32_t const* Image::ints() const
{
    Check(m_type == DataType::Int32, "the image does not hold integers");
    return static_cast<int32_t const*>(m_data.get());
}

Image const& RenderResult::aov(Aov aov) const
{
    auto found = aovs.find(aov);
    if (found == aovs.end()) Throw(ErrorCode::NotFound, "the " + std::string(AovName(aov)) + " AOV was not rendered");
    return found->second;
}

// ── Session

namespace {

// What both render paths derive from the options: validated, with the
// camera's rays for the data window.
struct ViewSetup {
    Window window;
    CameraDesc camera;
    CameraRayParameters rays;
    bool traced = false, lightAovs = false, denoise = false;
    // The camera's exposure (UsdGeomCamera's exposure attributes) and the
    // settings' stops; applied after the sample clamp, as the delegate does.
    float exposureScale = 1.0f;
};

ViewSetup Setup(SessionOptions const& options, Scene const& scene)
{
    ViewSetup setup;
    RenderSettings const& settings = options.settings;
    Validate(settings);
    Check(options.width > 0 && options.height > 0, "the session's image size must be positive");
    Window window{0, 0, options.width, options.height};
    if (options.dataWindow) window = *options.dataWindow;
    Check(window.width > 0 && window.height > 0 && uint64_t(window.x) + window.width <= options.width &&
              uint64_t(window.y) + window.height <= options.height,
          "the data window must be a nonempty part of the image");
    Check(uint64_t(window.width) * window.height <= std::numeric_limits<unsigned>::max() / 4, "the image is too large");
    Check(options.pixelAspectRatio > 0 && std::isfinite(options.pixelAspectRatio),
          "the pixel aspect ratio must be positive");
    Check(!options.aovs.empty(), "the session renders no AOVs");
    if (options.camera) setup.camera = scene.camera(options.camera);
    else if (options.cameraDesc) setup.camera = *options.cameraDesc;
    else Throw(ErrorCode::InvalidArgument, "the session has no camera (SessionOptions::camera or cameraDesc)");
    Check(!options.denoisedColor || options.denoise, "denoisedColor needs denoise settings");

    auto wants = [&](Aov aov) { return std::find(options.aovs.begin(), options.aovs.end(), aov) != options.aovs.end(); };
    for (Aov aov : options.aovs) {
        KindOf(aov);
        setup.lightAovs |= LightAov(aov);
        setup.traced |= aov == Aov::Color || aov == Aov::Denoised || LightAov(aov);
    }
    setup.denoise = setup.traced && (options.denoise.has_value() || wants(Aov::Denoised));
    if (setup.denoise && !device::DenoisingAvailable()) {
        std::string reason;
        device::SharedCpuDenoiser(&reason);
        Throw(ErrorCode::Unsupported, "denoising was asked for but " + reason);
    }

    camera::Framing framing;
    framing.width = int(options.width);
    framing.height = int(options.height);
    framing.dataX = int(window.x);
    framing.dataY = int(window.y);
    framing.dataWidth = int(window.width);
    framing.dataHeight = int(window.height);
    framing.pixelAspectRatio = options.pixelAspectRatio;
    framing.policy = options.conformPolicy;
    setup.rays = camera::RayParameters(setup.camera, framing);
    setup.window = window;
    float scale = std::isfinite(setup.camera.exposureScale) ? setup.camera.exposureScale : 1.0f;
    if (settings.exposure != 0.0f) scale *= std::exp2(settings.exposure);
    setup.exposureScale = scale;
    return setup;
}

ProgressiveOptions const& ValidProgressive(ProgressiveOptions const& p)
{
    Check(p.previewTargetMs > 0 && p.previewMaxDimension >= 0 && p.refinementDelayMs >= 0 && p.denoiseInterval >= 1 &&
              p.gpuPreparationPreviewMaxMiB >= 0,
          "progressive options: previewTargetMs and denoiseInterval must be positive, the others nonnegative");
    return p;
}

} // namespace

// The progressive render's state (start()); see progressiveLoop.
struct ProgressiveState {
    // A film per output of the accumulation (the engine's outputs): the
    // running sums and the current means, top row first.
    struct Film {
        Aov aov = Aov::Color;
        output::AovKind kind = output::AovKind::Color;
        unsigned components = 0;  // stored (the colour keeps its coverage)
        std::vector<float> sums, working;
        std::vector<uint32_t> counts;
        std::vector<int32_t> ints;
    };
    std::vector<Film> films;
    unsigned width = 0, height = 0;
    // The view of the current accumulation (pick).
    CameraRayParameters rays;
    Matrix4d worldToView, projection;
    detail::CommittedRevision revision;
    bool culling = false;
    bool hasView = false;
    // Published images (under the session's filmMutex); the colour shows
    // the denoised colour once one exists (denoisedColor).
    std::map<Aov, Image> published;
    bool colorDenoised = false;
    session::Frame frame;
};

struct Session::Impl {
    Impl(std::shared_ptr<RendererContext> context, Scene scene, SessionOptions options)
        : context(std::move(context)), scene(std::move(scene)), current(std::move(options))
    {
    }
    ~Impl();

    std::shared_ptr<RendererContext> context;
    Scene scene;
    // A hosted session (detail::SessionAccess::createHosted) renders its
    // front end's scene revisions and has no scene of its own.
    bool hosted = false;
    // The options (under optionsMutex); renders copy them when they start.
    SessionOptions current;
    mutable std::mutex optionsMutex;
    // The device: fixed for a session from createSession; a hosted session's
    // changes with its front end's preference (under deviceMutex, set by its
    // render thread).
    std::shared_ptr<device::RenderDevice> device;
    DeviceInfo info;
    std::string note;
    mutable std::mutex deviceMutex;
    device::DevicePreference devicePreference = device::DevicePreference::Auto;
    // A hosted session's device creation (render thread only): it can take
    // seconds, so the render thread waits on it while polling for a stop and
    // an interrupted creation keeps running for the next call.
    struct CreatedDevice {
        std::shared_ptr<device::RenderDevice> device;
        std::string note;
    };
    std::future<CreatedDevice> deviceCreation;
    device::DevicePreference deviceCreationPreference = device::DevicePreference::Auto;
    // What the device last saw: the scene revision and the generation of the
    // scene's render settings (another session may have changed them since).
    uint64_t preparedRevision = 0;
    uint64_t preparedSettings = ~uint64_t(0);
    std::mutex renderMutex;  // the batch render, and the progressive thread's runs
    std::atomic<bool> cancelRequested{false};

    // ── Progressive rendering (start()): one thread per session, which
    // renders while it is active. The fields below are under `control`.
    std::unique_ptr<session::ProgressiveEngine> engine;
    std::thread thread;
    mutable std::mutex control;
    mutable std::condition_variable controlChanged;
    SessionState state = SessionState::Idle;
    bool active = false;             // started, not cancelled or failed
    bool quit = false;
    bool pauseRequested = false;
    bool restartRequested = false;   // a new accumulation at the next run
    bool cancelProgressive = false;
    int batchRenders = 0;           // render() and prepare() calls running
    ProgressCallback progressCallback;
    std::exception_ptr error;
    std::string errorText;
    uint64_t accumulations = 0;
    uint64_t runningRevision = 0;
    int targetSamples = 0;
    double timeLimitSeconds = 0.0;
    // Set to end the engine's current run at its next check.
    std::atomic<bool> stopRun{false};
    mutable std::mutex filmMutex;
    ProgressiveState film;  // films: the render thread; published, view: filmMutex

    SessionOptions options() const
    {
        std::lock_guard lock(optionsMutex);
        return current;
    }
    SceneManager& sceneForDevice(SessionOptions const& options);
    RenderResult render(ProgressCallback const& progress);
    // Changed options restart a progressive render's accumulation.
    void restartProgressive();
    void progressiveLoop();
    session::ProgressiveEngine::Outcome runProgressive(SessionOptions const& options, bool restart,
                                                      ProgressCallback const& callback);
    SessionStatus statusLocked() const;
    detail::HostedDevice acquireDevice(device::DevicePreference preference, std::function<bool()> const& stopRequested);
    void checkNotHosted() const
    {
        if (hosted) Throw(ErrorCode::InvalidArgument, "a hosted session renders through its front end");
    }
};

// Called with the scene's render mutex held (SceneAccess::renderMutex).
SceneManager& Session::Impl::sceneForDevice(SessionOptions const& options)
{
    SceneManager& manager = detail::SceneAccess::committed(scene);
    // The render settings still live on the committed scene, shared by its
    // sessions (docs/DECISIONS.md): set this session's when the scene holds
    // others, and tell the device whenever the scene's revision or settings
    // changed since it last saw them (also by another session).
    const detail::SceneRenderSettings wanted = SceneSettings(options.settings);
    if (!(detail::SceneAccess::renderSettings(scene) == wanted)) detail::SceneAccess::setRenderSettings(scene, wanted);
    const uint64_t generation = detail::SceneAccess::renderSettingsGeneration(scene);
    if (scene.revision() != preparedRevision || generation != preparedSettings) {
        device->SceneChanged(manager);
        preparedRevision = scene.revision();
        preparedSettings = generation;
    }
    return manager;
}

RenderResult Session::Impl::render(ProgressCallback const& progressCallback)
{
    using Clock = std::chrono::steady_clock;
    checkNotHosted();
    // Before the render mutex, which a progressive run holds: the thread
    // starts no run while a batch render runs.
    {
        std::lock_guard guard(control);
        if (state == SessionState::Running || state == SessionState::Paused)
            Throw(ErrorCode::InvalidArgument, "a progressive render is active (cancel() and wait() first)");
        ++batchRenders;
    }
    struct BatchEnd {
        Impl& impl;
        ~BatchEnd()
        {
            std::lock_guard guard(impl.control);
            --impl.batchRenders;
        }
    } batchEnd{*this};
    std::lock_guard lock(renderMutex);
    cancelRequested.store(false);
    const SessionOptions options = this->options();
    // The scene's sessions render one at a time (their render settings live
    // on the scene's committed revision); a cancel() while waiting holds.
    detail::RenderLock sceneLock(scene);
    const auto start = Clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };

    // Validation before any work.
    RenderSettings const& settings = options.settings;
    const ViewSetup setup = Setup(options, scene);
    auto wants = [&](Aov aov) { return std::find(options.aovs.begin(), options.aovs.end(), aov) != options.aovs.end(); };
    const bool lightAovs = setup.lightAovs, traced = setup.traced, denoise = setup.denoise;
    const Window window = setup.window;

    // Diagnostics and progress of deep code go to this renderer.
    progress::Bind progressBinding(&context->progress());
    logging::Bind logBinding(&context->logger());
    SceneManager& manager = sceneForDevice(options);

    const CameraRayParameters rays = setup.rays;
    const unsigned width = window.width, height = window.height;
    DumpCameraRayDiagnostics(rays, width, height, settings.seed, "session");

    RenderResult result;
    result.width = options.width;
    result.height = options.height;
    result.dataWindow = window;
    result.traced = traced;
    result.meanSamples = settings.samples;
    result.fractionAtMaxSamples = 1.0;
    const float scale = setup.exposureScale;

    // Progress: a monitor thread polls the device's progress about ten times
    // a second and calls the callback (it may cancel); never the render path.
    device::ImageProgress imageProgress;
    std::mutex monitorMutex;
    std::condition_variable wake;
    bool stop = false;
    std::string phase = "device";
    std::exception_ptr callbackError;
    auto snapshot = [&] {
        ProgressInfo info;
        info.phase = phase;
        info.samples = imageProgress.samples.load(std::memory_order_relaxed);
        info.targetSamples = imageProgress.target.load(std::memory_order_relaxed);
        info.activeFraction = imageProgress.activeFraction.load(std::memory_order_relaxed);
        info.fraction = phase == "rendering" ? imageProgress.Fraction() : phase == "done" ? 1.0 : 0.0;
        if (phase == "rendering" && settings.timeLimitSeconds > 0.0)
            info.fraction = std::max(info.fraction, std::min(1.0, seconds() / settings.timeLimitSeconds));
        info.elapsedSeconds = seconds();
        return info;
    };
    auto call = [&](ProgressInfo const& info) {
        try {
            if (!progressCallback(info)) cancelRequested.store(true);
        } catch (...) {
            callbackError = std::current_exception();
            cancelRequested.store(true);
        }
    };
    std::thread monitor;
    if (progressCallback) {
        monitor = std::thread([&] {
            std::unique_lock guard(monitorMutex);
            while (!stop) {
                const ProgressInfo info = snapshot();
                guard.unlock();
                call(info);
                guard.lock();
                wake.wait_for(guard, std::chrono::milliseconds(100), [&] { return stop; });
            }
        });
    }
    auto setPhase = [&](std::string name) {
        std::lock_guard guard(monitorMutex);
        phase = std::move(name);
    };
    struct MonitorStop {
        std::thread& thread;
        std::mutex& mutex;
        std::condition_variable& wake;
        bool& stop;
        void operator()()
        {
            if (!thread.joinable()) return;
            {
                std::lock_guard guard(mutex);
                stop = true;
            }
            wake.notify_all();
            thread.join();
        }
        ~MonitorStop() { (*this)(); }
    } stopMonitor{monitor, monitorMutex, wake, stop};
    auto throwIfCancelled = [&] {
        if (!cancelRequested.load()) return;
        stopMonitor();
        if (callbackError) std::rethrow_exception(callbackError);
        Throw(ErrorCode::Cancelled, "the render was cancelled");
    };

    if (traced) {
        setPhase("rendering");
        device::ImageRequest request;
        request.camera = rays;
        request.width = width;
        request.height = height;
        request.samples = settings.samples;
        request.seed = settings.seed;
        request.sampleClamp = settings.sampleClamp;
        request.adaptive.threshold = settings.adaptiveThreshold;
        request.adaptive.minSamples = settings.adaptiveMinSamples;
        request.contributions = settings.debugContributions;
        request.nee = settings.nee;
        request.meanSamples = &result.meanSamples;
        request.fractionAtMaxSamples = &result.fractionAtMaxSamples;
        request.progress = &imageProgress;
        request.cancel = &cancelRequested;
        if (settings.timeLimitSeconds > 0) {
            request.timeLimitSeconds = settings.timeLimitSeconds;
            request.timeLimitReached = &result.timeLimitReached;
        }
        std::vector<device::LightAovPixel> lightPixels;
        if (lightAovs) request.lightAovs = &lightPixels;
        std::vector<float> denoised;
        if (denoise) {
            request.denoise = options.denoise.value_or(DenoiseSettings{});
            request.denoised = &denoised;
            request.denoiseMilliseconds = &result.denoiseMilliseconds;
            request.denoiser = &result.denoiser;
        }
        std::vector<float> color = device->RenderImage(manager, request);
        throwIfCancelled();
        // Applied after the sample clamp; adaptive sampling measures its
        // error relative to the image's brightness, so scaling afterwards is
        // the same.
        if (scale != 1.0f)
            for (float& value : color) value *= scale;
        auto adopt = [&](std::vector<float> const& values, unsigned channels) {
            Image image(width, height, channels);
            std::copy(values.begin(), values.end(), image.floats());
            return image;
        };
        if (denoise) {
            // HDR denoising is scale invariant (OIDN scales its input to its
            // training range), so the exposure applies after.
            if (scale != 1.0f)
                for (float& value : denoised) value *= scale;
            if (options.denoisedColor) color = denoised;
            if (wants(Aov::Denoised)) result.aovs[Aov::Denoised] = adopt(denoised, 3);
        }
        if (wants(Aov::Color)) result.aovs[Aov::Color] = adopt(color, 3);
        if (lightAovs) {
            // Radiance AOVs take the colour's exposure; the albedo does not.
            for (Aov aov : {Aov::Albedo, Aov::Diffuse, Aov::Specular, Aov::Emission}) {
                if (!wants(aov)) continue;
                Image plane(width, height, 3);
                float* values = plane.floats();
                const float planeScale = aov == Aov::Albedo ? 1.0f : scale;
                for (size_t p = 0; p < lightPixels.size(); ++p) {
                    auto const& a = lightPixels[p];
                    const Color3& value = aov == Aov::Albedo ? a.albedo
                        : aov == Aov::Diffuse ? a.diffuse
                        : aov == Aov::Specular ? a.specular : a.emission;
                    for (int c = 0; c < 3; ++c) values[3 * p + c] = value[c] * planeScale;
                }
                result.aovs[aov] = std::move(plane);
            }
        }
    }

    std::vector<Aov> geometric;
    for (Aov aov : options.aovs)
        if (output::FindAov(KindOf(aov))->geometric && std::find(geometric.begin(), geometric.end(), aov) == geometric.end())
            geometric.push_back(aov);
    if (!geometric.empty()) {
        setPhase("aovs");
        // Pixel-centre primary hits, as the Hydra delegate resolves its
        // geometric AOVs.
        const Matrix4d worldToView = rays.cameraToWorld.inverse();
        const Matrix4d projection = rays.inverseProjection.inverse();
        struct Plane {
            output::AovKind kind;
            bool integer;
            int components;
            Image image;
        };
        std::vector<Plane> planes;
        for (Aov aov : geometric) {
            const output::AovKind kind = KindOf(aov);
            const int components = output::AovComponents(kind);
            const bool integer = output::IntegerAov(kind);
            planes.push_back({kind, integer, components,
                              Image(width, height, unsigned(components), integer ? DataType::Int32 : DataType::Float32)});
        }
        const bool culling = manager.cullsBackFaces();
        // Prim, element and instance ids (-1 on a miss): the instances'
        // primId and instanceId; the face of the mesh (triangle lists: the
        // triangle; spheres: none).
        auto resolveId = [&](output::AovKind kind, RayHit const& hit) -> int32_t {
            if (hit.geomid == int(RTC_INVALID_GEOMETRY_ID)) return -1;
            auto const* ids = detail::SceneAccess::placement(scene, unsigned(hit.geomid));
            if (!ids) return -1;
            if (kind == output::AovKind::PrimId) return ids->primId;
            if (kind == output::AovKind::InstanceId) return ids->instanceId;
            if (ids->triangleList && ids->coarseFaces.size() == 0) return hit.primid;
            return hit.primid >= 0 && size_t(hit.primid) < ids->coarseFaces.size()
                ? int32_t(ids->coarseFaces[size_t(hit.primid)]) : -1;
        };
        tbb::parallel_for(0u, height, [&](unsigned y) {
            if (cancelRequested.load(std::memory_order_relaxed)) return;
            for (unsigned x = 0; x < width; ++x) {
                const RayHit hit = manager.castRayCulled(MakeCameraRayWithLens(rays,
                    float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr), culling);
                const size_t pixel = size_t(y) * width + x;
                for (auto& plane : planes) {
                    if (plane.integer) {
                        static_cast<int32_t*>(plane.image.data())[pixel] = resolveId(plane.kind, hit);
                    } else {
                        float* values = plane.image.floats() + pixel * size_t(plane.components);
                        output::ResolveGeometricAov(plane.kind, hit, worldToView, projection, values);
                    }
                }
            }
        });
        throwIfCancelled();
        for (size_t i = 0; i < geometric.size(); ++i) result.aovs[geometric[i]] = std::move(planes[i].image);
    }
    result.renderSeconds = seconds();
    setPhase("done");
    stopMonitor();
    if (progressCallback) {
        ProgressInfo done = snapshot();
        done.fraction = 1.0;
        call(done);  // the render is complete: a cancel now is too late
    }
    if (callbackError) std::rethrow_exception(callbackError);
    return result;
}

// ── Progressive rendering

namespace {

// The session's films as the progressive loop's outputs: accumulation like
// the delegate's render buffers (running means), published as Images at pass
// boundaries. Rows are stored top first.
class FilmTarget final : public session::ProgressiveTarget {
public:
    FilmTarget(Session::Impl& impl, detail::RenderLock& sceneLock, SessionOptions const& options,
               ProgressCallback const& callback, std::function<void(ProgressInfo&)> describe)
        : m_impl(impl), m_sceneLock(sceneLock), m_options(options), m_callback(callback),
          m_describe(std::move(describe))
    {
    }

    // The callback's error, if it threw.
    std::exception_ptr callbackError;

    bool StopRequested() const override { return m_impl.stopRun.load(std::memory_order_relaxed); }
    bool PauseRequested() const override { return false; }  // pause() stops the run and keeps the accumulation

    void PassBoundary() override;

    session::Frame CurrentFrame(bool, std::shared_ptr<device::RenderDevice> const&) override { return film().frame; }
    void FramePublished(session::Frame const&) override {}
    int RefinementDelay(std::chrono::steady_clock::time_point&) override
    {
        return m_options.progressive.refinementDelayMs;
    }
    void WarnUnsupported(size_t) override {}

    void BeginFrame(size_t output, bool) override
    {
        auto& f = film().films[output];
        const size_t pixels = size_t(film().width) * film().height;
        if (output::IntegerAov(f.kind)) {
            f.ints.assign(pixels, -1);
        } else {
            f.sums.assign(pixels * f.components, 0.0f);
            f.counts.assign(pixels, 0u);
            f.working.assign(pixels * f.components, 0.0f);
        }
    }
    void WriteFloat(size_t output, unsigned x, unsigned bufferY, const float* values, size_t count,
                    bool accumulate) override
    {
        auto& f = film().films[output];
        const size_t pixel = index(x, bufferY);
        float* dst = f.working.data() + pixel * f.components;
        if (accumulate) {
            const uint32_t n = ++f.counts[pixel];
            for (size_t c = 0; c < f.components; ++c) {
                float& sum = f.sums[pixel * f.components + c];
                sum += c < count ? values[c] : (c == 3 ? 1.0f : 0.0f);
                dst[c] = sum / float(n);
            }
        } else {
            for (size_t c = 0; c < f.components; ++c) dst[c] = c < count ? values[c] : (c == 3 ? 1.0f : 0.0f);
        }
    }
    void WriteInt(size_t output, unsigned x, unsigned bufferY, const int* values, size_t) override
    {
        film().films[output].ints[index(x, bufferY)] = values[0];
    }
    void KeepAccumulated(size_t, unsigned, unsigned) override {}  // the working means keep their values
    void Publish(size_t output, session::Frame const&) override
    {
        auto const& f = film().films[output];
        const unsigned width = film().width, height = film().height;
        Image image;
        if (output::IntegerAov(f.kind)) {
            image = Image(width, height, 1, DataType::Int32);
            std::copy(f.ints.begin(), f.ints.end(), static_cast<int32_t*>(image.data()));
        } else {
            const unsigned channels = channelsOf(f);
            image = Image(width, height, channels);
            float* dst = image.floats();
            const size_t pixels = size_t(width) * height;
            for (size_t p = 0; p < pixels; ++p)
                for (unsigned c = 0; c < channels; ++c) dst[p * channels + c] = f.working[p * f.components + c];
        }
        publish(f.aov, std::move(image));
    }
    void SetConverged(size_t, bool) override {}
    uint64_t PublishedBytes(size_t output) const override
    {
        auto const& f = film().films[output];
        return uint64_t(film().width) * film().height * channelsOf(f) * 4;
    }
    void PublishPreviewFloat(size_t output, unsigned previewWidth, unsigned previewHeight,
                             std::vector<float> const& values, size_t components, session::Frame const&,
                             bool) override
    {
        auto const& f = film().films[output];
        const unsigned width = film().width, height = film().height, channels = channelsOf(f);
        Image image(width, height, channels);
        float* dst = image.floats();
        // Nearest preview pixel; preview rows are bottom first.
        for (unsigned y = 0; y < height; ++y) {
            const unsigned py = previewHeight - 1 - unsigned(uint64_t(y) * previewHeight / height);
            for (unsigned x = 0; x < width; ++x) {
                const unsigned px = unsigned(uint64_t(x) * previewWidth / width);
                float const* src = values.data() + (size_t(py) * previewWidth + px) * components;
                for (unsigned c = 0; c < channels; ++c)
                    dst[(size_t(y) * width + x) * channels + c] = c < components ? src[c] : 0.0f;
            }
        }
        publish(f.aov, std::move(image));
    }
    void PublishPreviewInt(size_t output, unsigned previewWidth, unsigned previewHeight,
                           std::vector<int> const& values, size_t components, session::Frame const&) override
    {
        auto const& f = film().films[output];
        const unsigned width = film().width, height = film().height;
        Image image(width, height, 1, DataType::Int32);
        auto* dst = static_cast<int32_t*>(image.data());
        for (unsigned y = 0; y < height; ++y) {
            const unsigned py = previewHeight - 1 - unsigned(uint64_t(y) * previewHeight / height);
            for (unsigned x = 0; x < width; ++x) {
                const unsigned px = unsigned(uint64_t(x) * previewWidth / width);
                dst[size_t(y) * width + x] = values[(size_t(py) * previewWidth + px) * components];
            }
        }
        publish(f.aov, std::move(image));
    }
    void PublishDenoised(size_t output, std::vector<float> const& rgba, unsigned width, unsigned height,
                         session::Frame const&) override
    {
        publish(film().films[output].aov, rgb(rgba, width, height));
    }
    void PresentDenoisedColor(std::vector<float> const& rgba, unsigned width, unsigned height,
                              session::Frame const&) override
    {
        if (!m_options.denoisedColor) return;
        Image image = rgb(rgba, width, height);
        std::lock_guard lock(m_impl.filmMutex);
        film().colorDenoised = true;
        film().published[Aov::Color] = std::move(image);
    }
    void ResolvePrimary(output::AovKind kind, RayHit const& hit, float* values, int* id) const override
    {
        if (output::IntegerAov(kind)) {
            *id = -1;
            if (hit.geomid == int(RTC_INVALID_GEOMETRY_ID)) return;
            auto const* ids = film().revision.placement(unsigned(hit.geomid));
            if (!ids) return;
            if (kind == output::AovKind::PrimId) *id = ids->primId;
            else if (kind == output::AovKind::InstanceId) *id = ids->instanceId;
            else if (ids->triangleList && ids->coarseFaces.size() == 0) *id = hit.primid;
            else if (hit.primid >= 0 && size_t(hit.primid) < ids->coarseFaces.size())
                *id = int32_t(ids->coarseFaces[size_t(hit.primid)]);
            return;
        }
        output::ResolveGeometricAov(kind, hit, film().worldToView, film().projection, values);
    }

private:
    ProgressiveState& film() const { return m_impl.film; }
    size_t index(unsigned x, unsigned bufferY) const
    {
        return size_t(film().height - 1 - bufferY) * film().width + x;
    }
    static unsigned channelsOf(ProgressiveState::Film const& f)
    {
        // The colours drop their coverage (as RenderResult's).
        return output::ColorAov(f.kind) ? 3u : f.components;
    }
    static Image rgb(std::vector<float> const& rgba, unsigned width, unsigned height)
    {
        Image image(width, height, 3);
        float* dst = image.floats();
        for (size_t p = 0; p < size_t(width) * height; ++p)
            for (int c = 0; c < 3; ++c) dst[p * 3 + c] = rgba[p * 4 + c];
        return image;
    }
    void publish(Aov aov, Image image)
    {
        std::lock_guard lock(m_impl.filmMutex);
        if (aov == Aov::Color && film().colorDenoised) return;  // the denoised colour stays shown
        film().published[aov] = std::move(image);
    }

    Session::Impl& m_impl;
    detail::RenderLock& m_sceneLock;
    SessionOptions const& m_options;
    ProgressCallback const& m_callback;
    std::function<void(ProgressInfo&)> m_describe;
    std::chrono::steady_clock::time_point m_lastCallback{};
};

} // namespace

// At each pass boundary: report progress (at most ten times a second), let
// other renders and commits of the scene go first, and end the run when the
// scene committed a new revision (restart) or another session changed the
// scene's render settings (the run resumes with this session's).
void FilmTarget::PassBoundary()
{
    if (m_callback) {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastCallback >= std::chrono::milliseconds(100)) {
            m_lastCallback = now;
            ProgressInfo info;
            m_describe(info);
            bool keep = true;
            try {
                keep = m_callback(info);
            } catch (...) {
                callbackError = std::current_exception();
                keep = false;
            }
            if (!keep) {
                std::lock_guard lock(m_impl.control);
                m_impl.cancelProgressive = true;
                m_impl.stopRun.store(true);
                return;
            }
        }
    }
    if (!m_sceneLock.yield()) return;
    const uint64_t revision = m_impl.scene.revision();
    if (revision != film().revision.revision) {
        std::lock_guard lock(m_impl.control);
        m_impl.restartRequested = true;
        m_impl.stopRun.store(true);
        return;
    }
    if (!(detail::SceneAccess::renderSettings(m_impl.scene) == SceneSettings(m_options.settings)))
        m_impl.stopRun.store(true);
}

Session::Impl::~Impl()
{
    {
        std::lock_guard lock(control);
        quit = true;
        stopRun.store(true);
    }
    controlChanged.notify_all();
    if (thread.joinable()) thread.join();
    // Device jobs own their device and scene; let them finish.
    if (deviceCreation.valid()) {
        try { (void)deviceCreation.get(); }
        catch (...) {}
    }
    engine.reset();
}

void Session::Impl::restartProgressive()
{
    {
        std::lock_guard lock(control);
        if (!active) return;
        restartRequested = true;
        stopRun.store(true);
    }
    controlChanged.notify_all();
}

SessionStatus Session::Impl::statusLocked() const
{
    SessionStatus status;
    status.state = state;
    status.targetSamples = targetSamples;
    status.accumulations = accumulations;
    status.revision = runningRevision;
    status.error = errorText;
    if (!engine) return status;
    // The timing first: the passes it counts are then at most `samples`.
    const session::ProgressiveStats stats = engine->Stats();
    status.elapsedSeconds = stats.elapsedSeconds;
    status.samplesPerSecond = stats.samplesPerSecond;
    status.estimatedSecondsRemaining = stats.estimatedSecondsRemaining;
    status.samples = engine->samples.load();
    status.activeFraction = status.samples ? engine->activePixelFraction.load() : 1.0;
    status.timeLimitReached = engine->timeLimitReached.load();
    // As the delegate's percentDone.
    const double fraction = detail::ProgressFraction(status.samples, targetSamples, status.activeFraction,
                                                     timeLimitSeconds, engine->timeLimitFraction.load());
    status.fraction = state == SessionState::Converged ? 1.0 : std::clamp(fraction, 0.0, 1.0);
    return status;
}

// One run of the progressive loop: a new accumulation (restart) or the
// current one resumed, until it converges or something stops it.
session::ProgressiveEngine::Outcome Session::Impl::runProgressive(SessionOptions const& options, bool restart,
                                                                  ProgressCallback const& callback)
{
    const ViewSetup setup = Setup(options, scene);
    std::lock_guard lock(renderMutex);
    detail::RenderLock sceneLock(scene);
    detail::CommittedRevision revision = detail::SceneAccess::committedRevision(scene);
    const detail::SceneRenderSettings wanted = SceneSettings(options.settings);
    if (!(detail::SceneAccess::renderSettings(scene) == wanted)) detail::SceneAccess::setRenderSettings(scene, wanted);
    const uint64_t generation = detail::SceneAccess::renderSettingsGeneration(scene);
    if (revision.revision != film.revision.revision || !film.hasView) restart = true;
    if (restart || engine->samples.load() == 0) {
        engine->samples.store(0);
        // The outputs: each AOV once.
        std::vector<Aov> aovs;
        for (Aov aov : options.aovs)
            if (std::find(aovs.begin(), aovs.end(), aov) == aovs.end()) aovs.push_back(aov);
        film.films.clear();
        for (Aov aov : aovs) {
            ProgressiveState::Film f;
            f.aov = aov;
            f.kind = KindOf(aov);
            f.components = unsigned(output::AovComponents(f.kind));
            film.films.push_back(std::move(f));
        }
        film.width = setup.window.width;
        film.height = setup.window.height;
        film.frame = std::make_shared<uint64_t>(accumulations);
        std::lock_guard filmLock(filmMutex);
        film.published.clear();
        film.colorDenoised = false;
        film.rays = setup.rays;
        film.worldToView = setup.rays.cameraToWorld.inverse();
        film.projection = setup.rays.inverseProjection.inverse();
        film.revision = revision;
        film.culling = revision.manager->cullsBackFaces();
        film.hasView = true;
        std::lock_guard controlLock(control);
        ++accumulations;
        runningRevision = revision.revision;
        targetSamples = options.settings.samples;
        timeLimitSeconds = options.settings.timeLimitSeconds;
    }
    engine->NoteScene(*device, *revision.manager, (revision.revision << 32) ^ generation);

    session::ProgressiveRun run;
    run.device = device;
    run.preference = options.device;
    run.scene = revision.manager;
    for (auto const& f : film.films) run.outputs.push_back({f.kind, true});
    session::ProgressiveSettings& s = run.settings;
    RenderSettings const& settings = options.settings;
    s.samples = settings.samples;
    s.seed = int(settings.seed);
    s.sampleClamp = settings.sampleClamp;
    s.adaptiveThreshold = settings.adaptiveThreshold;
    s.adaptiveMinSamples = settings.adaptiveMinSamples;
    s.timeLimitSeconds = settings.timeLimitSeconds;
    s.nee = settings.nee;
    s.transparentEscapes = false;  // escapes see the environment, as render()'s
    s.adaptivePreview = options.progressive.adaptivePreview;
    s.previewTargetMs = options.progressive.previewTargetMs;
    s.previewMaxDimension = options.progressive.previewMaxDimension;
    s.denoise = options.denoise.has_value();
    s.denoiseInterval = options.progressive.denoiseInterval;
    s.denoiseSettings = options.denoise.value_or(DenoiseSettings{});
    s.gpuPreparationPreview = options.progressive.gpuPreparationPreview;
    s.gpuPreparationPreviewMaxMiB = options.progressive.gpuPreparationPreviewMaxMiB;
    run.view.rays = setup.rays;
    run.view.exposureScale = setup.exposureScale;
    run.view.width = setup.window.width;
    run.view.height = setup.window.height;
    run.generation = revision.revision;
    run.diagnosticsLabel = "session";

    auto describe = [&](ProgressInfo& info) {
        SessionStatus status;
        {
            std::lock_guard controlLock(control);
            status = statusLocked();
        }
        info.phase = "rendering";
        info.samples = double(status.samples);
        info.targetSamples = status.targetSamples;
        info.activeFraction = status.activeFraction;
        info.fraction = status.fraction;
        info.elapsedSeconds = status.elapsedSeconds;
        info.samplesPerSecond = status.samplesPerSecond;
        info.estimatedSecondsRemaining = status.estimatedSecondsRemaining;
    };
    FilmTarget target(*this, sceneLock, options, callback, describe);
    const auto outcome = engine->Run(run, target);
    if (target.callbackError) std::rethrow_exception(target.callbackError);
    if (outcome == session::ProgressiveEngine::Outcome::Finished && callback) {
        ProgressInfo info;
        describe(info);
        info.phase = "done";
        info.fraction = 1.0;
        callback(info);  // complete: a false return is too late to cancel
    }
    return outcome;
}

// The session's render thread: waits for work, runs the progressive loop,
// and records how each run ended.
void Session::Impl::progressiveLoop()
{
    progress::Bind progressBinding(&context->progress());
    logging::Bind logBinding(&context->logger());
    for (;;) {
        bool restart = false;
        ProgressCallback callback;
        {
            std::unique_lock lock(control);
            for (;;) {
                if (quit) return;
                // A cancel() that came between two runs (a run stopped for a
                // restart or a new revision): the next run must not start, or
                // it would clear the stop and render on.
                if (cancelProgressive && active && state == SessionState::Running) {
                    cancelProgressive = false;
                    state = SessionState::Cancelled;
                    active = false;
                    restartRequested = false;
                    lock.unlock();
                    controlChanged.notify_all();
                    lock.lock();
                    continue;
                }
                const bool runnable = active && !pauseRequested && batchRenders == 0;
                if (runnable && (restartRequested || state == SessionState::Running)) break;
                // A converged render takes a new commit.
                if (runnable && state == SessionState::Converged && scene.revision() != runningRevision) {
                    restartRequested = true;
                    break;
                }
                controlChanged.wait_for(lock, std::chrono::milliseconds(20));
            }
            restart = restartRequested;
            restartRequested = false;
            stopRun.store(false);
            state = SessionState::Running;
            callback = progressCallback;
        }
        controlChanged.notify_all();
        session::ProgressiveEngine::Outcome outcome = session::ProgressiveEngine::Outcome::Stopped;
        std::exception_ptr failure;
        try {
            outcome = runProgressive(options(), restart, callback);
        } catch (...) {
            failure = std::current_exception();
        }
        {
            std::lock_guard lock(control);
            if (failure) {
                state = SessionState::Failed;
                error = failure;
                try {
                    std::rethrow_exception(failure);
                } catch (std::exception const& e) {
                    errorText = e.what();
                } catch (...) {
                    errorText = "unknown error";
                }
                active = false;
            } else if (outcome == session::ProgressiveEngine::Outcome::Finished) {
                state = restartRequested ? SessionState::Running : SessionState::Converged;
            } else if (quit) {
                return;
            } else if (cancelProgressive) {
                cancelProgressive = false;
                state = SessionState::Cancelled;
                active = false;
            } else if (pauseRequested) {
                state = SessionState::Paused;
            }  // else: a restart or a resume, at once
        }
        controlChanged.notify_all();
    }
}

Session::Session(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Session::~Session() = default;

DeviceInfo Session::device() const { return m_impl->info; }
std::string Session::deviceNote() const { return m_impl->note; }
SessionOptions const& Session::options() const { return m_impl->current; }

void Session::setSettings(RenderSettings const& settings)
{
    Validate(settings);
    {
        std::lock_guard lock(m_impl->optionsMutex);
        m_impl->current.settings = settings;
    }
    m_impl->restartProgressive();
}
void Session::setCamera(CameraId camera)
{
    m_impl->scene.camera(camera);  // throws for an invalid handle
    {
        std::lock_guard lock(m_impl->optionsMutex);
        m_impl->current.camera = camera;
    }
    m_impl->restartProgressive();
}
void Session::setCamera(CameraDesc const& camera)
{
    Check(!camera.cameraToWorld.empty(), "a camera needs a transform");
    {
        std::lock_guard lock(m_impl->optionsMutex);
        m_impl->current.camera = CameraId{};
        m_impl->current.cameraDesc = camera;
    }
    m_impl->restartProgressive();
}
void Session::resize(unsigned width, unsigned height, std::optional<Window> dataWindow, float pixelAspectRatio,
                     ConformPolicy policy)
{
    Check(width > 0 && height > 0, "the session's image size must be positive");
    {
        std::lock_guard lock(m_impl->optionsMutex);
        m_impl->current.width = width;
        m_impl->current.height = height;
        m_impl->current.dataWindow = dataWindow;
        m_impl->current.pixelAspectRatio = pixelAspectRatio;
        m_impl->current.conformPolicy = policy;
    }
    m_impl->restartProgressive();
}
void Session::setAovs(std::vector<Aov> const& aovs)
{
    for (Aov aov : aovs) KindOf(aov);
    {
        std::lock_guard lock(m_impl->optionsMutex);
        m_impl->current.aovs = aovs;
    }
    m_impl->restartProgressive();
}
void Session::setDenoise(std::optional<DenoiseSettings> const& denoise, bool denoisedColor)
{
    Check(!denoisedColor || denoise, "denoisedColor needs denoise settings");
    {
        std::lock_guard lock(m_impl->optionsMutex);
        m_impl->current.denoise = denoise;
        m_impl->current.denoisedColor = denoisedColor;
    }
    m_impl->restartProgressive();
}

void Session::prepare()
{
    m_impl->checkNotHosted();
    {
        std::lock_guard guard(m_impl->control);
        if (m_impl->state == SessionState::Running || m_impl->state == SessionState::Paused)
            Throw(ErrorCode::InvalidArgument, "a progressive render is active (cancel() and wait() first)");
        ++m_impl->batchRenders;
    }
    struct BatchEnd {
        Impl& impl;
        ~BatchEnd()
        {
            std::lock_guard guard(impl.control);
            --impl.batchRenders;
        }
    } batchEnd{*m_impl};
    std::lock_guard lock(m_impl->renderMutex);
    const SessionOptions options = m_impl->options();
    detail::RenderLock sceneLock(m_impl->scene);
    Validate(options.settings);
    progress::Bind progressBinding(&m_impl->context->progress());
    logging::Bind logBinding(&m_impl->context->logger());
    m_impl->device->Prepare(m_impl->sceneForDevice(options));
}

RenderResult Session::render(ProgressCallback const& progress) { return m_impl->render(progress); }

void Session::cancel()
{
    m_impl->cancelRequested.store(true);
    {
        std::lock_guard lock(m_impl->control);
        if (!m_impl->active) return;
        if (m_impl->state == SessionState::Running) {
            m_impl->cancelProgressive = true;
            m_impl->stopRun.store(true);
        } else {
            // Not rendering (paused or converged): cancelled at once.
            m_impl->state = SessionState::Cancelled;
            m_impl->active = false;
            m_impl->pauseRequested = false;
            m_impl->restartRequested = false;
        }
    }
    m_impl->controlChanged.notify_all();
}

void Session::start(ProgressCallback const& progress)
{
    m_impl->checkNotHosted();
    const SessionOptions options = m_impl->options();
    Setup(options, m_impl->scene);  // throws for invalid options
    ValidProgressive(options.progressive);
    Check(m_impl->scene.revision() > 0, "the scene has not been committed");
    {
        std::lock_guard lock(m_impl->control);
        if (m_impl->batchRenders > 0)
            Throw(ErrorCode::InvalidArgument, "a batch render() is running on this session");
        m_impl->progressCallback = progress;
        switch (m_impl->state) {
        case SessionState::Running:
        case SessionState::Converged:
            if (m_impl->active) break;
            [[fallthrough]];
        case SessionState::Idle:
        case SessionState::Cancelled:
        case SessionState::Failed:
            m_impl->active = true;
            m_impl->restartRequested = true;
            m_impl->pauseRequested = false;
            m_impl->cancelProgressive = false;
            m_impl->error = nullptr;
            m_impl->errorText.clear();
            m_impl->state = SessionState::Running;
            break;
        case SessionState::Paused:
            m_impl->pauseRequested = false;
            m_impl->state = SessionState::Running;
            break;
        }
        if (!m_impl->engine) m_impl->engine = std::make_unique<session::ProgressiveEngine>(m_impl->context);
        if (!m_impl->thread.joinable()) {
            Impl* impl = m_impl.get();
            m_impl->thread = std::thread([impl] { impl->progressiveLoop(); });
        }
    }
    m_impl->controlChanged.notify_all();
}

void Session::pause()
{
    {
        std::lock_guard lock(m_impl->control);
        if (!m_impl->active || m_impl->state != SessionState::Running) return;
        m_impl->pauseRequested = true;
        m_impl->stopRun.store(true);
    }
    m_impl->controlChanged.notify_all();
}

void Session::resume()
{
    {
        std::lock_guard lock(m_impl->control);
        if (!m_impl->active) return;
        m_impl->pauseRequested = false;
        if (m_impl->state == SessionState::Paused) m_impl->state = SessionState::Running;
    }
    m_impl->controlChanged.notify_all();
}

void Session::restart() { m_impl->restartProgressive(); }

SessionStatus Session::wait(double timeoutSeconds) const
{
    std::unique_lock lock(m_impl->control);
    auto done = [&] {
        return !m_impl->active || (m_impl->state != SessionState::Running && !m_impl->restartRequested);
    };
    if (timeoutSeconds < 0) m_impl->controlChanged.wait(lock, done);
    else m_impl->controlChanged.wait_for(lock, std::chrono::duration<double>(timeoutSeconds), done);
    if (m_impl->state == SessionState::Failed && m_impl->error) std::rethrow_exception(m_impl->error);
    return m_impl->statusLocked();
}

SessionStatus Session::status() const
{
    std::lock_guard lock(m_impl->control);
    return m_impl->statusLocked();
}

Image Session::aov(Aov aov) const
{
    KindOf(aov);
    {
        const SessionOptions options = m_impl->options();
        if (std::find(options.aovs.begin(), options.aovs.end(), aov) == options.aovs.end())
            Throw(ErrorCode::NotFound, "the session does not render the " + std::string(AovName(aov)) + " AOV");
    }
    std::lock_guard lock(m_impl->filmMutex);
    auto found = m_impl->film.published.find(aov);
    return found == m_impl->film.published.end() ? Image() : found->second;
}

Pick Session::pick(unsigned x, unsigned y) const
{
    CameraRayParameters rays;
    Matrix4d worldToView, projection;
    detail::CommittedRevision revision;
    unsigned width, height;
    bool culling;
    {
        std::lock_guard lock(m_impl->filmMutex);
        Check(m_impl->film.hasView, "pick() needs a progressive render (start())");
        rays = m_impl->film.rays;
        worldToView = m_impl->film.worldToView;
        projection = m_impl->film.projection;
        revision = m_impl->film.revision;
        width = m_impl->film.width;
        height = m_impl->film.height;
        culling = m_impl->film.culling;
    }
    Check(x < width && y < height, "pick(): the pixel is outside the image");
    const RayHit hit = revision.manager->castRayCulled(
        MakeCameraRayWithLens(rays, float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr), culling);
    Pick pick;
    if (hit.geomid == int(RTC_INVALID_GEOMETRY_ID)) return pick;
    pick.hit = true;
    pick.position = hit.p;
    pick.normal = hit.n;
    output::ResolveGeometricAov(output::AovKind::Depth, hit, worldToView, projection, &pick.depth);
    if (auto const* ids = revision.placement(unsigned(hit.geomid))) {
        pick.instance = ids->instance;
        pick.primId = ids->primId;
        pick.instanceId = ids->instanceId;
        pick.userData = ids->userData;
        if (ids->triangleList && ids->coarseFaces.size() == 0) pick.elementId = hit.primid;
        else if (hit.primid >= 0 && size_t(hit.primid) < ids->coarseFaces.size())
            pick.elementId = int32_t(ids->coarseFaces[size_t(hit.primid)]);
    }
    return pick;
}

// ── Hosted sessions (session_internal.h): the front end's scene revisions
// and render targets, this session's device, loop and state.

double detail::ProgressFraction(uint64_t samples, int targetSamples, double activeFraction, double timeLimitSeconds,
                                double timeLimitFraction)
{
    const double sampleFraction = std::clamp(double(samples) / double(std::max(1, targetSamples)), 0.0, 1.0);
    double fraction = (1.0 - activeFraction) + activeFraction * sampleFraction;
    if (samples && timeLimitSeconds > 0.0) fraction = std::max(fraction, timeLimitFraction);
    return fraction;
}

// Moved from the Hydra delegate's render worker (AcquireRenderDevice).
detail::HostedDevice Session::Impl::acquireDevice(device::DevicePreference preference,
                                                  std::function<bool()> const& stopRequested)
{
    detail::HostedDevice result;
    {
        std::lock_guard lock(deviceMutex);
        if (device && devicePreference == preference) {
            result.device = device;
            return result;
        }
    }
    auto waitFor = [&](std::future<CreatedDevice>& job) {
        while (job.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
            if (stopRequested()) return false;
        return true;
    };
    if (deviceCreation.valid() && deviceCreationPreference != preference) {
        if (!waitFor(deviceCreation)) {
            ++engine->devicePreparationsInterrupted;
            return result;
        }
        try { (void)deviceCreation.get(); }
        catch (...) {}
    }
    if (!deviceCreation.valid()) {
        deviceCreationPreference = preference;
        context->progress().Begin(Phase::Device);
        deviceCreation = std::async(std::launch::async, [preference, context = context] {
            logging::Bind logBinding(&context->logger());
            progress::Bind progressBinding(&context->progress());
            load_profile::Scope profile("device.create");
            CreatedDevice created;
            created.device = device::CreateDevice(preference, context->deviceOptions(), &created.note);
            context->progress().End(Phase::Device);
            return created;
        });
    }
    if (!waitFor(deviceCreation)) {
        ++engine->devicePreparationsInterrupted;
        return result;
    }
    CreatedDevice created;
    try {
        created = deviceCreation.get();
    } catch (std::exception const& e) {
        result.error = e.what();
    }
    if (!created.device)
        created.device = device::CreateDevice(device::DevicePreference::Cpu, context->deviceOptions(), &created.note);
    context->progress().SetDevice(session::ProgressiveEngine::DescribeDevice(*created.device, preference, created.note));
    {
        std::lock_guard lock(deviceMutex);
        device = created.device;
        devicePreference = preference;
        info = device->Info();
        note = created.note;
    }
    engine->DeviceReplaced();
    result.device = std::move(created.device);
    result.created = true;
    result.note = std::move(created.note);
    return result;
}

namespace detail {

Session SessionAccess::createHosted(std::shared_ptr<RendererContext> context,
                                    std::atomic<int64_t> const* embreeLiveBytes)
{
    auto impl = std::make_shared<Session::Impl>(context, SceneAccess::detached(), SessionOptions{});
    impl->hosted = true;
    impl->engine = std::make_unique<session::ProgressiveEngine>(impl->context);
    impl->engine->embreeLiveBytes = embreeLiveBytes;
    return Session(std::move(impl));
}

HostedDevice SessionAccess::acquireDevice(Session& session, device::DevicePreference preference,
                                          std::function<bool()> const& stopRequested)
{
    return session.m_impl->acquireDevice(preference, stopRequested);
}

std::shared_ptr<device::RenderDevice> SessionAccess::device(Session const& session)
{
    std::lock_guard lock(session.m_impl->deviceMutex);
    return session.m_impl->device;
}

void SessionAccess::noteScene(Session& session, SceneManager& scene, uint64_t generation)
{
    if (auto current = device(session)) session.m_impl->engine->NoteScene(*current, scene, generation);
}

bool SessionAccess::runHosted(Session& session, HostedRun const& hosted, session::ProgressiveTarget& target)
{
    Session::Impl& impl = *session.m_impl;
    session::ProgressiveEngine& engine = *impl.engine;
    {
        std::lock_guard lock(impl.control);
        if (engine.samples.load() == 0) ++impl.accumulations;
        impl.active = true;
        impl.state = SessionState::Running;
        impl.runningRevision = hosted.generation;
        impl.targetSamples = hosted.settings.samples;
        impl.timeLimitSeconds = hosted.settings.timeLimitSeconds;
    }
    session::ProgressiveRun run;
    run.device = device(session);
    run.preference = hosted.preference;
    run.scene = hosted.scene;
    run.outputs = hosted.outputs;
    run.settings = hosted.settings;
    run.view = hosted.view;
    run.generation = hosted.generation;
    run.diagnosticsLabel = hosted.diagnosticsLabel;
    const auto outcome = engine.Run(run, target);
    {
        // A stopped run keeps its accumulation for the next one (paused).
        std::lock_guard lock(impl.control);
        impl.state = outcome == session::ProgressiveEngine::Outcome::Finished ? SessionState::Converged
                                                                               : SessionState::Paused;
    }
    impl.controlChanged.notify_all();
    return outcome == session::ProgressiveEngine::Outcome::Finished;
}

void SessionAccess::restartAccumulation(Session& session) { session.m_impl->engine->samples.store(0); }

HostedCounters SessionAccess::counters(Session const& session)
{
    session::ProgressiveEngine const& engine = *session.m_impl->engine;
    HostedCounters counters;
    counters.samples = engine.samples.load();
    counters.activePixelFraction = engine.activePixelFraction.load();
    counters.timeLimitFraction = engine.timeLimitFraction.load();
    counters.timeLimitReached = engine.timeLimitReached.load();
    counters.denoisesPublished = engine.denoisesPublished.load();
    counters.lastDenoiseMilliseconds = engine.lastDenoiseMilliseconds.load();
    counters.colorTracedPixels = engine.colorTracedPixels.load();
    counters.geometricOnlyPixels = engine.geometricOnlyPixels.load();
    counters.gpuPreviewActive = engine.gpuPreviewActive.load(std::memory_order_relaxed);
    counters.gpuPreviewSamples = engine.gpuPreviewSamples.load();
    counters.gpuPreviewHandovers = engine.gpuPreviewHandovers.load();
    counters.gpuPreviewHandoverSample = engine.gpuPreviewHandoverSample.load();
    counters.textureStreamingRestarts = engine.textureStreamingRestarts.load();
    counters.devicePreparations = engine.devicePreparations.load();
    counters.devicePreparationsInterrupted = engine.devicePreparationsInterrupted.load();
    counters.profileBufferWrites = engine.profileBufferWrites;
    return counters;
}

session::ProgressiveStats SessionAccess::stats(Session const& session) { return session.m_impl->engine->Stats(); }

void SessionAccess::waitForJobs(Session& session)
{
    Session::Impl& impl = *session.m_impl;
    if (impl.deviceCreation.valid()) {
        try { (void)impl.deviceCreation.get(); }
        catch (...) {}
    }
    impl.engine->WaitForJobs();
}

} // namespace detail

// ── Renderer (the parts with devices and sessions)

RendererOptions const& Renderer::options() const { return m_context->options(); }

ColorInfo Renderer::color() const
{
    std::shared_ptr<const color::ColorManagement> color;
    try {
        color = m_context->colorManagement();
    } catch (std::exception const& error) {
        Throw(ErrorCode::InvalidArgument, error.what());
    }
    ColorInfo info;
    info.config = color->config();
    info.configName = color->configName();
    info.workingSpace = color->workingSpace();
    info.display = color->display();
    info.view = color->view();
    return info;
}

std::vector<DeviceInfo> Renderer::devices() const
{
    return device::AvailableDevices(DeviceResources(m_context->options().devices));
}

Capabilities Renderer::capabilities() const
{
    Capabilities capabilities;
    capabilities.denoising = device::DenoisingAvailable();
    if (!capabilities.denoising) device::SharedCpuDenoiser(&capabilities.denoisingNote);
    return capabilities;
}

RendererStats Renderer::stats() const
{
    const auto textures = m_context->textures()->stats();
    RendererStats stats;
    stats.texturePrepareCalls = textures.prepareCalls;
    stats.textureMemoryCacheHits = textures.memoryCacheHits;
    stats.textureDiskCacheHits = textures.diskCacheHits;
    stats.textureSourceBytesHashed = textures.sourceBytesHashed;
    stats.texturePreparationNanoseconds = textures.preparationNanoseconds;
    return stats;
}

Session Renderer::createSession(Scene const& scene, SessionOptions const& options) const
{
    Check(&detail::SceneAccess::context(scene) == m_context.get(), "the scene belongs to another renderer");
    Validate(options.settings);
    for (Aov aov : options.aovs) KindOf(aov);
    auto impl = std::make_shared<Session::Impl>(m_context, scene, options);
    std::string note;
    impl->device = device::CreateDevice(options.device, m_context->deviceOptions(), &note);
    impl->info = impl->device->Info();
    if (!note.empty() && !options.allowFallback && options.device != DevicePreference::Auto &&
        options.device != DevicePreference::Cpu)
        Throw(ErrorCode::Unsupported, note);
    impl->note = std::move(note);
    return Session(std::move(impl));
}

} // namespace prism
