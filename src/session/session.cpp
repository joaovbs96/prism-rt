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
#include "../render_defaults.h"
#include "../renderer/renderer_context.h"
#include "../scene.h"
#include "../scene/scene_internal.h"
#include "../texture_service.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <limits>
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

struct Session::Impl {
    Impl(std::shared_ptr<RendererContext> context, Scene scene, SessionOptions options)
        : context(std::move(context)), scene(std::move(scene)), options(std::move(options))
    {
    }

    std::shared_ptr<RendererContext> context;
    Scene scene;
    SessionOptions options;
    std::unique_ptr<device::RenderDevice> device;
    DeviceInfo info;
    std::string note;
    // What the device last saw: the scene revision and its render settings.
    uint64_t preparedRevision = 0;
    detail::SceneRenderSettings applied;
    std::mutex renderMutex;
    std::atomic<bool> cancelRequested{false};

    SceneManager& sceneForDevice();
    CameraDesc camera() const;
    RenderResult render(ProgressCallback const& progress);
};

SceneManager& Session::Impl::sceneForDevice()
{
    SceneManager& manager = detail::SceneAccess::committed(scene);
    const detail::SceneRenderSettings wanted = SceneSettings(options.settings);
    if (scene.revision() != preparedRevision || !(wanted == applied)) {
        // The render settings that still live on the committed scene
        // (docs/DECISIONS.md): set before the device sees it.
        detail::SceneAccess::setRenderSettings(scene, wanted);
        device->SceneChanged(manager);
        preparedRevision = scene.revision();
        applied = wanted;
    }
    return manager;
}

CameraDesc Session::Impl::camera() const
{
    if (options.camera) return scene.camera(options.camera);
    if (options.cameraDesc) return *options.cameraDesc;
    Throw(ErrorCode::InvalidArgument, "the session has no camera (SessionOptions::camera or cameraDesc)");
}

RenderResult Session::Impl::render(ProgressCallback const& progressCallback)
{
    using Clock = std::chrono::steady_clock;
    std::lock_guard lock(renderMutex);
    cancelRequested.store(false);
    const auto start = Clock::now();
    auto seconds = [&] { return std::chrono::duration<double>(Clock::now() - start).count(); };

    // Validation before any work.
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
    const CameraDesc camera = this->camera();
    Check(!options.denoisedColor || options.denoise, "denoisedColor needs denoise settings");

    auto wants = [&](Aov aov) { return std::find(options.aovs.begin(), options.aovs.end(), aov) != options.aovs.end(); };
    bool lightAovs = false, traced = false;
    for (Aov aov : options.aovs) {
        KindOf(aov);
        lightAovs |= LightAov(aov);
        traced |= aov == Aov::Color || aov == Aov::Denoised || LightAov(aov);
    }
    const bool denoise = traced && (options.denoise.has_value() || wants(Aov::Denoised));
    if (denoise && !device::DenoisingAvailable()) {
        std::string reason;
        device::SharedCpuDenoiser(&reason);
        Throw(ErrorCode::Unsupported, "denoising was asked for but " + reason);
    }

    // Diagnostics and progress of deep code go to this renderer.
    progress::Bind progressBinding(&context->progress());
    logging::Bind logBinding(&context->logger());
    SceneManager& manager = sceneForDevice();

    camera::Framing framing;
    framing.width = int(options.width);
    framing.height = int(options.height);
    framing.dataX = int(window.x);
    framing.dataY = int(window.y);
    framing.dataWidth = int(window.width);
    framing.dataHeight = int(window.height);
    framing.pixelAspectRatio = options.pixelAspectRatio;
    framing.policy = options.conformPolicy;
    const CameraRayParameters rays = camera::RayParameters(camera, framing);
    const unsigned width = window.width, height = window.height;
    DumpCameraRayDiagnostics(rays, width, height, settings.seed, "session");

    RenderResult result;
    result.width = options.width;
    result.height = options.height;
    result.dataWindow = window;
    result.traced = traced;
    result.meanSamples = settings.samples;
    result.fractionAtMaxSamples = 1.0;
    // The camera's exposure (UsdGeomCamera's exposure attributes) and the
    // settings' stops; applied after the sample clamp, as the delegate does.
    float scale = std::isfinite(camera.exposureScale) ? camera.exposureScale : 1.0f;
    if (settings.exposure != 0.0f) scale *= std::exp2(settings.exposure);

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

Session::Session(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
Session::~Session() = default;

DeviceInfo Session::device() const { return m_impl->info; }
std::string Session::deviceNote() const { return m_impl->note; }
SessionOptions const& Session::options() const { return m_impl->options; }

void Session::setSettings(RenderSettings const& settings)
{
    Validate(settings);
    std::lock_guard lock(m_impl->renderMutex);
    m_impl->options.settings = settings;
}
void Session::setCamera(CameraId camera)
{
    m_impl->scene.camera(camera);  // throws for an invalid handle
    std::lock_guard lock(m_impl->renderMutex);
    m_impl->options.camera = camera;
}
void Session::setCamera(CameraDesc const& camera)
{
    Check(!camera.cameraToWorld.empty(), "a camera needs a transform");
    std::lock_guard lock(m_impl->renderMutex);
    m_impl->options.camera = CameraId{};
    m_impl->options.cameraDesc = camera;
}
void Session::resize(unsigned width, unsigned height, std::optional<Window> dataWindow, float pixelAspectRatio,
                     ConformPolicy policy)
{
    Check(width > 0 && height > 0, "the session's image size must be positive");
    std::lock_guard lock(m_impl->renderMutex);
    m_impl->options.width = width;
    m_impl->options.height = height;
    m_impl->options.dataWindow = dataWindow;
    m_impl->options.pixelAspectRatio = pixelAspectRatio;
    m_impl->options.conformPolicy = policy;
}
void Session::setAovs(std::vector<Aov> const& aovs)
{
    for (Aov aov : aovs) KindOf(aov);
    std::lock_guard lock(m_impl->renderMutex);
    m_impl->options.aovs = aovs;
}
void Session::setDenoise(std::optional<DenoiseSettings> const& denoise, bool denoisedColor)
{
    Check(!denoisedColor || denoise, "denoisedColor needs denoise settings");
    std::lock_guard lock(m_impl->renderMutex);
    m_impl->options.denoise = denoise;
    m_impl->options.denoisedColor = denoisedColor;
}

void Session::prepare()
{
    std::lock_guard lock(m_impl->renderMutex);
    Validate(m_impl->options.settings);
    progress::Bind progressBinding(&m_impl->context->progress());
    logging::Bind logBinding(&m_impl->context->logger());
    m_impl->device->Prepare(m_impl->sceneForDevice());
}

RenderResult Session::render(ProgressCallback const& progress) { return m_impl->render(progress); }

void Session::cancel() { m_impl->cancelRequested.store(true); }

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
