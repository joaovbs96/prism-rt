// The progressive render loop (progressive_engine.h). Moved from the Hydra
// delegate's render thread (SceneBridge::Render and its helpers) unchanged:
// the delegate's images stay bit-identical.

#include "progressive_engine.h"

#include "../camera_ray_diagnostics.h"
#include "../load_profile.h"
#include "../progress.h"
#include "../render_profile.h"
#include "../renderer/renderer_context.h"
#include "../scene.h"

#include <tbb/blocked_range2d.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <thread>
#include <tuple>

namespace prism::session {

Color3 const &LightAovValue(device::LightAovPixel const &pixel, AovKind kind)
{
    return kind == AovKind::Albedo ? pixel.albedo : kind == AovKind::Diffuse ? pixel.diffuse
        : kind == AovKind::Specular ? pixel.specular : pixel.emission;
}

namespace {

// Per pixel: the colour's RGBA sums, then the albedo's and the normal's.
constexpr size_t kDenoiseFloats = 10;

constexpr const char *kGpuPreviewDevice = "cpu (preview while the GPU prepares)";

struct PreviewResult {
    unsigned int width = 0;
    unsigned int height = 0;
    bool published = false;
    double publicationMilliseconds = 0.0;
    // Tracing time without scene preparation (GPU devices upload the scene
    // on their first render).
    double traceMilliseconds = 0.0;
    uint64_t publishedBytes = 0;
    uint64_t tracedPixels = 0;
    uint64_t boundAovCount = 0;
};

// Waits for a device job, returning false (and leaving the job running) as
// soon as the target asks the render thread to stop.
template <class T>
bool WaitForDeviceJob(std::future<T> &job, ProgressiveTarget const &target)
{
    while (job.wait_for(std::chrono::milliseconds(2)) != std::future_status::ready)
        if (target.StopRequested()) return false;
    return true;
}

PreviewResult RenderPreview(SceneManager &scene, ProgressiveView const &view, ProgressiveSettings const &settings,
                            int previewMaxDimension, std::vector<ProgressiveOutput> const &outputs,
                            ProgressiveTarget &target, Frame const &frame, std::atomic<uint64_t> &colorTracedPixels,
                            device::RenderDevice &renderDevice)
{
    PreviewResult result;
    const unsigned width = view.width, height = view.height;
    if (width == 0 || height == 0) return result;

    const unsigned int maxPreviewEdge = static_cast<unsigned int>(std::clamp(previewMaxDimension, 16, 1024));
    if (width >= height) {
        result.width = std::min(width, maxPreviewEdge);
        result.height = std::max(1u, static_cast<unsigned int>(
            (static_cast<uint64_t>(height) * result.width + width / 2) / width));
    } else {
        result.height = std::min(height, maxPreviewEdge);
        result.width = std::max(1u, static_cast<unsigned int>(
            (static_cast<uint64_t>(width) * result.height + height / 2) / height));
    }

    struct Target {
        size_t output = 0;
        AovKind kind = AovKind::Unknown;
        size_t components = 0;
        std::vector<float> floats;
        std::vector<int> ints;
    };
    std::vector<Target> targets;
    bool needPrimary = false, needLight = false;
    const size_t pixels = static_cast<size_t>(result.width) * result.height;
    for (size_t index = 0; index < outputs.size(); ++index) {
        if (!outputs[index].bound) continue;
        Target t;
        t.output = index;
        t.kind = outputs[index].kind;
        // The denoised AOV shows the raw preview until the first denoise.
        if (output::ColorAov(t.kind)) {
            t.components = 4; t.floats.resize(pixels * 4);
        } else if (output::LightAov(t.kind)) {
            t.components = 3; t.floats.resize(pixels * 3);
            needLight = true;
        } else if (t.kind == AovKind::Normal || t.kind == AovKind::Neye || t.kind == AovKind::Peye ||
                   t.kind == AovKind::Pworld || t.kind == AovKind::St) {
            t.components = t.kind == AovKind::St ? 2 : 3;
            t.floats.resize(pixels * t.components);
            needPrimary = true;
        } else if (t.kind == AovKind::Depth || t.kind == AovKind::CameraDepth) {
            t.components = 1; t.floats.resize(pixels);
            needPrimary = true;
        } else if (t.kind == AovKind::PrimId || t.kind == AovKind::ElementId || t.kind == AovKind::InstanceId) {
            t.components = 1; t.ints.assign(pixels, -1);
            needPrimary = true;
        } else {
            target.WarnUnsupported(index);
            continue;
        }
        targets.push_back(std::move(t));
    }
    if (targets.empty()) return result;
    result.tracedPixels = pixels;
    result.boundAovCount = targets.size();
    for (Target const &t : targets) result.publishedBytes += target.PublishedBytes(t.output);

    device::PreviewRequest request;
    request.camera = view.rays;
    request.width = width;
    request.height = height;
    request.previewWidth = result.width;
    request.previewHeight = result.height;
    request.seed = static_cast<unsigned int>(settings.seed);
    request.shading.sampleClamp = settings.sampleClamp;
    request.shading.nee = settings.nee;
    request.shading.exposureScale = view.exposureScale;
    request.shading.transparentEscapes = settings.transparentEscapes;
    request.tracePrimary = needPrimary;
    request.colorPixels = &colorTracedPixels;
    request.stopRequested = [&target] { return target.StopRequested(); };
    request.traceMilliseconds = &result.traceMilliseconds;
    request.writePixel = [&](unsigned int px, unsigned int py, Color3 const &color, float coverage,
                             RayHit const *primary) {
        const size_t pixel = (static_cast<size_t>(result.height - 1 - py) * result.width + px);
        for (Target &t : targets) {
            if (output::ColorAov(t.kind)) {
                float *dst = t.floats.data() + pixel * 4;
                dst[0] = color.x(); dst[1] = color.y();
                dst[2] = color.z(); dst[3] = coverage;
                continue;
            }
            if (!primary || output::LightAov(t.kind)) continue;
            target.ResolvePrimary(t.kind, *primary,
                t.floats.empty() ? nullptr : t.floats.data() + pixel * t.components,
                t.ints.empty() ? nullptr : t.ints.data() + pixel);
        }
    };
    if (needLight)
        request.writeLightAovs = [&](unsigned int px, unsigned int py, device::LightAovPixel const &aovs) {
            const size_t pixel = (static_cast<size_t>(result.height - 1 - py) * result.width + px);
            for (Target &t : targets) {
                if (!output::LightAov(t.kind)) continue;
                Color3 const &value = LightAovValue(aovs, t.kind);
                float *dst = t.floats.data() + pixel * 3;
                dst[0] = value.x(); dst[1] = value.y(); dst[2] = value.z();
            }
        };
    if (!renderDevice.RenderPreview(scene, request)) return result;
    auto const publicationStart = std::chrono::steady_clock::now();
    for (Target const &t : targets) {
        if (!t.ints.empty())
            target.PublishPreviewInt(t.output, result.width, result.height, t.ints, t.components, frame);
        else
            target.PublishPreviewFloat(t.output, result.width, result.height, t.floats, t.components, frame,
                                       output::ColorAov(t.kind));
    }
    result.publicationMilliseconds = 1000.0 * std::chrono::duration<double>(
        std::chrono::steady_clock::now() - publicationStart).count();
    result.published = true;
    return result;
}

} // namespace

// One denoise of the accumulation (docs/denoising.md): its means, copied
// when it starts, and the denoised colour, top row first.
struct DenoiseJob {
    unsigned int width = 0, height = 0;
    uint64_t samples = 0;
    Frame frame;
    device::DenoiseSettings settings;
    // Null: the build has no denoiser and the output is the raw colour.
    std::shared_ptr<device::Denoiser> denoiser;
    // RGBA colour, then albedo and normal (kDenoiseFloats per pixel).
    std::vector<float> input;
    std::vector<float> output; // RGBA
    std::string error;
    double milliseconds = 0.0;
    std::atomic<bool> done{false};
    bool published = false; // render thread only
};

namespace {

void ExecuteDenoise(DenoiseJob &job)
{
    const size_t pixels = static_cast<size_t>(job.width) * job.height;
    job.output.resize(pixels * 4);
    for (size_t p = 0; p < pixels; ++p)
        std::copy_n(job.input.data() + p * kDenoiseFloats, 4, job.output.data() + 4 * p);
    if (!job.denoiser) return;
    const auto start = std::chrono::steady_clock::now();
    device::DenoiseRequest request;
    request.width = job.width;
    request.height = job.height;
    request.color = {job.input.data(), unsigned(kDenoiseFloats)};
    request.albedo = {job.input.data() + 4, unsigned(kDenoiseFloats)};
    request.normal = {job.input.data() + 7, unsigned(kDenoiseFloats)};
    // In the RGB of the RGBA output; alpha stays the coverage.
    request.output = job.output.data();
    request.outputStride = 4;
    request.settings = job.settings;
    try {
        job.denoiser->Denoise(request);
    } catch (std::exception const &error) {
        job.error = error.what();
        for (size_t p = 0; p < pixels; ++p)
            std::copy_n(job.input.data() + p * kDenoiseFloats, 4, job.output.data() + 4 * p);
    }
    job.milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

} // namespace

// Runs denoise jobs off the render thread, the latest submitted first: the
// render thread never waits for a denoise (it publishes finished jobs at
// pass boundaries). The thread starts with the first job and coordinates
// only; OIDN's CPU device runs on the process's oneTBB.
class DenoiseWorker {
public:
    ~DenoiseWorker()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
    void Submit(std::shared_ptr<DenoiseJob> job)
    {
        std::lock_guard lock(m_mutex);
        if (!m_thread.joinable()) m_thread = std::thread([this] { Run(); });
        // A job not started yet is replaced (its accumulation moved on).
        if (m_pending) m_pending->done.store(true, std::memory_order_release);
        m_pending = std::move(job);
        m_wake.notify_all();
    }

private:
    void Run()
    {
        for (;;) {
            std::shared_ptr<DenoiseJob> job;
            {
                std::unique_lock lock(m_mutex);
                m_wake.wait(lock, [&] { return m_stop || m_pending; });
                if (m_stop) return;
                job = std::move(m_pending);
            }
            {
                profiling::Attach profile;
                profiling::Scope timer(profiling::Denoise);
                ExecuteDenoise(*job);
            }
            job->done.store(true, std::memory_order_release);
        }
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::shared_ptr<DenoiseJob> m_pending;
    bool m_stop = false;
    std::thread m_thread;
};

ProgressiveEngine::ProgressiveEngine(std::shared_ptr<RendererContext> renderer) : m_renderer(std::move(renderer))
{
    if (const char *value = std::getenv("PRISM_GPU_PREPARATION_PREVIEW_DELAY_MS"); value && *value)
        m_gpuPreviewDelayMs = std::max(0, std::atoi(value));
    if (const char *value = std::getenv("PRISM_GPU_PREPARATION_PREVIEW_THREADS"); value && *value)
        m_gpuPreviewThreads = std::max(0, std::atoi(value));
    if (const char *value = std::getenv("PRISM_PROFILE_BUFFERS"))
        profileBufferWrites = value[0] != '\0' && std::strcmp(value, "0") != 0;
}

ProgressiveEngine::~ProgressiveEngine()
{
    WaitForJobs();
}

void ProgressiveEngine::CancelJobs()
{
    std::shared_ptr<device::RenderDevice> device;
    {
        std::lock_guard lock(m_mutex);
        device = m_preparingDevice.lock();
    }
    if (device) device->CancelPreparation();
}

void ProgressiveEngine::WaitForJobs()
{
    // A preparation job owns its device and scene; let it finish.
    if (m_devicePreparation.valid()) {
        try { m_devicePreparation.get(); }
        catch (...) {}
    }
}

std::string ProgressiveEngine::DescribeDevice(device::RenderDevice const &device,
                                              device::DevicePreference preference, std::string const &note)
{
    const auto info = device.Info();
    std::string name = info.name;
    for (std::string_view suffix : {" (Vulkan)", " (Metal)"})
        if (name.size() > suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            name.resize(name.size() - suffix.size());
    if (info.kind != device::DeviceKind::Cpu)
        return std::string(device::ToString(info.kind)) + ": " + name;
    if (note.empty()) return "cpu";
    // "no GPU device available (vulkan: reason); using the CPU device"
    std::string reason = note;
    const auto open = note.find('('), close = note.rfind(");");
    if (open != std::string::npos && close != std::string::npos && close > open)
        reason = note.substr(open + 1, close - open - 1);
    std::string kind = device::ToString(preference);
    if (reason.compare(0, kind.size() + 2, kind + ": ") == 0) reason.erase(0, kind.size() + 2);
    if (kind == "auto") kind = "GPU";
    else if (!kind.empty()) kind[0] = char(std::toupper(static_cast<unsigned char>(kind[0])));
    if (reason.size() > 60) reason = reason.substr(0, 57) + "...";
    return "cpu (" + kind + " fallback: " + reason + ")";
}

void ProgressiveEngine::DeviceReplaced()
{
    std::lock_guard lock(m_mutex);
    m_deviceScene = nullptr;
    ++m_deviceSceneEpoch;
}

void ProgressiveEngine::NoteScene(device::RenderDevice &device, SceneManager &scene, uint64_t generation)
{
    // Published edits replace the scene or advance its generation; camera
    // changes do neither. GPU devices copy these integrator settings into
    // their scene data, so a change must reach them like an edit.
    const auto deviceSettings = std::make_tuple(scene.max_depth, scene.rr_min_depth, scene.environmentSampling,
        int(scene.textureFiltering), scene.textureStreaming, scene.gpuTextureBudgetMiB);
    std::lock_guard lock(m_mutex);
    if (m_deviceScene != &scene || m_deviceGeneration != generation || m_deviceSettings != deviceSettings) {
        device.SceneChanged(scene);
        m_renderer->progress().End(Phase::Rendering);
        m_deviceScene = &scene;
        m_deviceGeneration = generation;
        m_deviceSettings = deviceSettings;
        ++m_deviceSceneEpoch;
    }
}

ProgressiveStats ProgressiveEngine::Stats() const
{
    std::lock_guard lock(m_mutex);
    ProgressiveStats stats = m_stats;
    stats.devicePreparing = m_devicePreparedEpoch != m_deviceSceneEpoch;
    // Render progress timing. A front end that restarted the accumulation
    // (samples 0) before the next run sees it as not started.
    Timing const &t = m_timing;
    if (samples.load() == 0 && !t.rendering && !t.preparing) return stats;
    stats.elapsedSeconds = t.seconds;
    if (t.rendering)
        stats.elapsedSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t.since).count();
    double pixelSamples = 0.0, seconds = 0.0;
    for (auto const &pass : t.passes) {
        pixelSamples += pass.pixelSamples;
        seconds += pass.seconds;
    }
    stats.samplesPerSecond = seconds > 0.0 ? pixelSamples / seconds : 0.0;
    if (t.complete) {
        stats.estimatedSecondsRemaining = 0.0;
    } else if (!t.preparing && t.completedPasses >= 2 && stats.samplesPerSecond > 0.0) {
        const double remaining = double(std::max<int64_t>(0, int64_t(t.targetSamples) - int64_t(t.completedPasses))) *
            double(t.activePixels);
        double eta = remaining / stats.samplesPerSecond;
        if (t.timeLimit > 0.0) eta = std::min(eta, std::max(0.0, t.timeLimit - stats.elapsedSeconds));
        stats.estimatedSecondsRemaining = eta;
    }
    return stats;
}

// Advances the device's per-scene preparation for the displayed scene
// without blocking: finishes a completed job and, once no job is running,
// starts the one for the current epoch. Render thread only. True when the
// device is prepared for the scene.
bool ProgressiveEngine::AdvanceDevicePreparation(std::shared_ptr<device::RenderDevice> const &device,
                                                 std::shared_ptr<SceneManager> const &prepared)
{
    uint64_t epoch;
    {
        std::lock_guard lock(m_mutex);
        epoch = m_deviceSceneEpoch;
        if (m_devicePreparedEpoch == epoch) return true;
    }
    if (m_devicePreparation.valid()) {
        // A superseded job still owns the device; it cannot be cancelled.
        if (m_devicePreparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return false;
        try {
            m_devicePreparation.get();
        } catch (std::exception const &e) {
            m_renderer->logger().warning("device", std::string("render device preparation failed: ") + e.what());
        } catch (...) {}
        ++devicePreparations;
        std::lock_guard lock(m_mutex);
        epoch = m_deviceSceneEpoch;
        if (m_deviceSceneEpoch == m_devicePreparationEpoch) {
            m_devicePreparedEpoch = m_devicePreparationEpoch;
            return true;
        }
    }
    m_devicePreparationEpoch = epoch;
    m_devicePreparationStart = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(m_mutex);
        m_preparingDevice = device;
    }
    // The job owns the scene (its holder's snapshot), so a published
    // replacement cannot free or edit the scene it is reading.
    m_devicePreparation = std::async(std::launch::async, [this, device, prepared] {
        logging::Bind logBinding(&m_renderer->logger());
        progress::Bind progressBinding(&m_renderer->progress());
        const auto start = std::chrono::steady_clock::now();
        load_profile::Scope profile("device.prepare");
        // Diagnostics, as in the CLI: print the scene's GPU programs (DXC
        // only, no GPU work; run with PRISM_DEVICE=cpu) and exit.
        if (const char *report = std::getenv("PRISM_GPU_PROGRAM_REPORT"); report && *report == '1') {
            if (!device::ReportGpuPrograms(*prepared, std::cout, m_renderer->varyingEmitters().get()))
                std::cerr << "PrismRT: this build has no Vulkan device\n";
            std::cout.flush();
            std::_Exit(0);
        }
        // Test hook: stretches the preparation to exercise the CPU preview
        // that covers it.
        if (const char *delay = std::getenv("PRISM_TEST_GPU_PREPARATION_DELAY_MS"); delay && *delay)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(delay)));
        device->Prepare(*prepared);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::lock_guard lock(m_mutex);
        m_stats.devicePreparationSeconds = seconds;
    });
    return false;
}

// Runs the device's per-scene preparation as a job and waits for it. Render
// thread only; false if a stop was requested first.
bool ProgressiveEngine::PrepareRenderDevice(std::shared_ptr<device::RenderDevice> const &device,
                                            std::shared_ptr<SceneManager> const &prepared,
                                            ProgressiveTarget const &target)
{
    while (!AdvanceDevicePreparation(device, prepared)) {
        if (m_devicePreparation.valid() && !WaitForDeviceJob(m_devicePreparation, target)) {
            ++devicePreparationsInterrupted;
            return false;
        }
    }
    return true;
}

// Like PrepareRenderDevice, but a GPU device that is still preparing after
// the preview delay yields Preview: the caller renders on m_previewDevice
// and polls AdvanceDevicePreparation to hand over.
ProgressiveEngine::PreparationOutcome ProgressiveEngine::PrepareOrPreview(
    std::shared_ptr<device::RenderDevice> const &device, std::shared_ptr<SceneManager> const &prepared,
    ProgressiveSettings const &settings, ProgressiveTarget const &target)
{
    if (AdvanceDevicePreparation(device, prepared)) return PreparationOutcome::Ready;
    const auto kind = device->Info().kind;
    const int64_t limit = int64_t(settings.gpuPreparationPreviewMaxMiB) << 20;
    const int64_t live = embreeLiveBytes ? embreeLiveBytes->load(std::memory_order_relaxed) : 0;
    const bool eligible = settings.gpuPreparationPreview &&
        (kind == device::DeviceKind::Vulkan || kind == device::DeviceKind::Metal) &&
        (limit <= 0 || live <= limit);
    if (!eligible)
        return PrepareRenderDevice(device, prepared, target) ? PreparationOutcome::Ready
                                                             : PreparationOutcome::Stopped;
    // Fast (cached) preparations never start the CPU preview.
    const auto deadline = m_devicePreparationStart + std::chrono::milliseconds(m_gpuPreviewDelayMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (target.StopRequested()) {
            ++devicePreparationsInterrupted;
            return PreparationOutcome::Stopped;
        }
        if (AdvanceDevicePreparation(device, prepared)) return PreparationOutcome::Ready;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (AdvanceDevicePreparation(device, prepared)) return PreparationOutcome::Ready;
    if (!m_previewDevice)
        m_previewDevice = device::CreateDevice(device::DevicePreference::Cpu, m_renderer->deviceOptions());
    return PreparationOutcome::Preview;
}

// Marks the CPU preview as the displayed device.
void ProgressiveEngine::BeginGpuPreview()
{
    if (gpuPreviewActive.exchange(true)) return;
    m_renderer->progress().SetDevice(kGpuPreviewDevice);
}

// Restores the GPU's device line after a handover (unless the device
// reported something more specific, such as a CPU fallback).
void ProgressiveEngine::EndGpuPreview(device::RenderDevice const &gpu, device::DevicePreference preference)
{
    if (!gpuPreviewActive.exchange(false)) return;
    if (m_renderer->progress().Read().device == kGpuPreviewDevice)
        m_renderer->progress().SetDevice(DescribeDevice(gpu, preference, {}));
}

// A new accumulation of `pixels` pixels (0: not denoised). A denoise in
// flight finishes on the worker unseen.
void ProgressiveEngine::ResetDenoise(size_t pixels)
{
    if (pixels) {
        m_denoiseSums.assign(pixels * kDenoiseFloats, 0.0f);
        m_denoiseCounts.assign(pixels, 0u);
    } else {
        std::vector<float>().swap(m_denoiseSums);
        std::vector<uint32_t>().swap(m_denoiseCounts);
    }
    m_denoiseJob.reset();
    m_denoisedSamples = 0;
}

ProgressiveEngine::Outcome ProgressiveEngine::Run(ProgressiveRun const &run, ProgressiveTarget &target)
{
    Progress &progress = m_renderer->progress();
    std::shared_ptr<device::RenderDevice> const &renderDevice = run.device;
    ProgressiveSettings const &settings = run.settings;
    std::vector<ProgressiveOutput> const &outputs = run.outputs;
    const unsigned int width = run.view.width, height = run.view.height;
    SceneManager &scene = *run.scene;
    // hasColor: some bound output is path traced (the colour or a light AOV).
    bool hasColor = false, hasGeometric = false, hasLight = false, hasDenoised = false;
    for (auto const &output : outputs) {
        if (!output.bound) continue;
        const AovKind kind = output.kind;
        hasColor |= TracedAov(kind);
        hasLight |= output::LightAov(kind);
        hasDenoised |= kind == AovKind::Denoised;
        hasGeometric |= !TracedAov(kind) && kind != AovKind::Unknown;
    }
    // Denoising (docs/denoising.md): the displayed colour or a bound denoised
    // AOV. Its albedo and normal guides come with the light AOVs; without it
    // nothing below changes.
    const bool denoising = hasColor && (settings.denoise || hasDenoised);
    // Denoising alone reads only the albedo and normal of the light AOVs.
    const bool guidesOnly = denoising && !hasLight;
    hasLight |= denoising;
    const int sampleCount = hasColor ? std::max(1, settings.samples) : 1;
    // Render progress timing: a new accumulation starts over; the time
    // counts from the end of device preparation to the end of this run.
    {
        std::lock_guard lock(m_mutex);
        if (samples.load() == 0) {
            m_timing = {};
            m_timing.activePixels = size_t(width) * height;
        }
        m_timing.preparing = true;
        m_timing.complete = false;
        m_timing.targetSamples = sampleCount;
        m_timing.timeLimit = hasColor ? settings.timeLimitSeconds : 0.0;
    }
    struct TimingEnd {
        ProgressiveEngine &engine;
        ~TimingEnd()
        {
            std::lock_guard lock(engine.m_mutex);
            Timing &t = engine.m_timing;
            if (t.rendering)
                t.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t.since).count();
            t.rendering = t.preparing = false;
        }
    } timingEnd{*this};
    // Kernel compilation and scene upload happen here, not inside the
    // preview or pass, so a host stop never waits for them.
    // While a GPU device is still preparing, the CPU device renders instead
    // (activeDevice) and the loop below hands over once the GPU is ready.
    device::RenderDevice *activeDevice = renderDevice.get();
    bool previewingGpu = false;
    if (hasColor || hasGeometric) {
        const PreparationOutcome outcome = PrepareOrPreview(renderDevice, run.scene, settings, target);
        if (outcome == PreparationOutcome::Stopped) return Outcome::Stopped;
        if (outcome == PreparationOutcome::Preview) {
            previewingGpu = true;
            activeDevice = m_previewDevice.get();
            BeginGpuPreview();
        }
    }
    if (!previewingGpu) EndGpuPreview(*renderDevice, run.preference);
    progress.Begin(Phase::Rendering);
    {
        std::lock_guard lock(m_mutex);
        m_timing.preparing = false;
        m_timing.rendering = true;
        m_timing.since = std::chrono::steady_clock::now();
    }
    for (size_t index = 0; index < outputs.size(); ++index)
        if (outputs[index].kind == AovKind::Unknown) target.WarnUnsupported(index);
    const int firstSample = static_cast<int>(samples.load());
    const Frame frame = target.CurrentFrame(firstSample != 0, renderDevice);
    if (firstSample == 0 && hasColor) {
        // The preview runs on the render thread. The front end captured its
        // outputs after stopping their previous user.
        const auto deviceKind = activeDevice->Info().kind;
        const bool gpuPreview = deviceKind == device::DeviceKind::Metal || deviceKind == device::DeviceKind::Vulkan;
        // The automatic maximum keeps CPU previews coarse; GPU devices afford
        // far more pixels within the target.
        const int maxDimension = settings.previewMaxDimension > 0
            ? std::clamp(settings.previewMaxDimension, 16, 1024) : (gpuPreview ? 512 : 96);
        int previewMaxDimension = maxDimension;
        if (settings.adaptivePreview) {
            std::lock_guard lock(m_mutex);
            if (m_stats.previewCostMilliseconds <= 0.0) {
                // Without a measurement, start below the maximum.
                previewMaxDimension = std::min(maxDimension, gpuPreview ? 256 : 96);
            } else {
                const double ratio = std::sqrt(
                    static_cast<double>(std::clamp(settings.previewTargetMs, 1, 1000)) /
                    m_stats.previewCostMilliseconds);
                previewMaxDimension = std::clamp(
                    static_cast<int>(std::lround(std::max(m_stats.previewWidth, m_stats.previewHeight) *
                        std::clamp(ratio, 0.5, 2.0))),
                    16, maxDimension);
            }
        }
        auto const previewStart = std::chrono::steady_clock::now();
        PreviewResult const preview = RenderPreview(scene, run.view, settings, previewMaxDimension, outputs, target,
                                                    frame, colorTracedPixels, *activeDevice);
        if (target.StopRequested()) return Outcome::Stopped;
        bool firstPreview = false;
        {
            std::lock_guard lock(m_mutex);
            m_stats.previewMilliseconds = 1000.0 * std::chrono::duration<double>(
                std::chrono::steady_clock::now() - previewStart).count();
            m_stats.previewPublicationMilliseconds = preview.publicationMilliseconds;
            m_stats.previewRayWorkMilliseconds = std::max(
                0.0, m_stats.previewMilliseconds - preview.publicationMilliseconds);
            m_stats.previewPublishedBytes = preview.publishedBytes;
            m_stats.previewTracedPixels = preview.tracedPixels;
            m_stats.previewBoundAovCount = preview.boundAovCount;
            m_stats.previewWidth = preview.width;
            m_stats.previewHeight = preview.height;
            if (preview.published) {
                m_stats.previewCostMilliseconds = preview.traceMilliseconds + preview.publicationMilliseconds;
                firstPreview = m_stats.previewPublishes++ == 0;
            }
        }
        if (preview.published) {
            if (firstPreview)
                load_profile::Mark("frame.first_preview", "generation=" + std::to_string(run.generation));
            target.FramePublished(frame);
        }
        // Do not let full-resolution work contend with camera interaction.
        // Each new camera state cancels this wait in at most two milliseconds
        // and starts a fresh quiet period; refinement begins automatically
        // after input stops.
        auto const quietStart = std::chrono::steady_clock::now();
        for (;;) {
            if (target.StopRequested()) return Outcome::Stopped;
            auto start = quietStart;
            const int delay = target.RefinementDelay(start);
            if (std::chrono::steady_clock::now() >= start +
                std::chrono::milliseconds(std::clamp(delay, 0, 2000))) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (target.StopRequested()) return Outcome::Stopped;
    }
    if (firstSample == 0) {
        for (size_t index = 0; index < outputs.size(); ++index)
            if (outputs[index].bound) target.BeginFrame(index, outputs[index].kind == AovKind::Unknown);
        // Full-resolution RNG setup belongs to the render thread. Camera
        // motion only pays for the bounded preview above.
        if (hasColor)
            m_committedRngStates.resize(static_cast<size_t>(width) * height);
        else
            std::vector<unsigned int>().swap(m_committedRngStates);
        if (hasColor && settings.adaptiveThreshold > 0.0f) {
            m_adaptiveSums.assign(static_cast<size_t>(width) * height * 7, 0.0f);
            m_adaptiveActive.Reset(width, height);
        } else {
            std::vector<float>().swap(m_adaptiveSums);
            m_adaptiveActive = {};
        }
        activePixelFraction.store(1.0);
        timeLimitFraction.store(0.0);
        timeLimitReached.store(false);
        ResetDenoise(denoising ? static_cast<size_t>(width) * height : 0);
    }
    // A resumed accumulation keeps its sums (they always match its size).
    if (denoising && m_denoiseCounts.size() != static_cast<size_t>(width) * height)
        ResetDenoise(static_cast<size_t>(width) * height);
    CameraRayParameters const &cameraParameters = run.view.rays;
    DumpCameraRayDiagnostics(cameraParameters, width, height, settings.seed, run.diagnosticsLabel);
    // Offline renders: a device that streams textures loads the levels this
    // view asks for before the first sample, and keeps them for the whole
    // accumulation (deterministic images).
    if (firstSample == 0 && firstSample < sampleCount && hasColor && !previewingGpu &&
        settings.textureStreamingPrepass) {
        device::PassRequest settle;
        settle.camera = cameraParameters;
        settle.width = width;
        settle.height = height;
        settle.seed = static_cast<unsigned int>(settings.seed);
        settle.shading.sampleClamp = settings.sampleClamp;
        settle.shading.nee = settings.nee;
        settle.shading.exposureScale = run.view.exposureScale;
        settle.shading.transparentEscapes = settings.transparentEscapes;
        load_profile::Scope settleProfile("frame.texture_settle");
        activeDevice->SettleTextures(scene, settle);
        if (target.StopRequested()) return Outcome::Stopped;
    }
    struct ResolvedOutput { bool bound; AovKind kind; size_t plane; };
    struct AovPlane {
        AovKind kind;
        size_t components;
        std::vector<float> floats;
        std::vector<int> ints;
    };
    std::vector<ResolvedOutput> resolvedOutputs;
    std::vector<AovPlane> primaryPlanes;
    const size_t scratchPixels = firstSample < sampleCount && (hasColor || hasGeometric)
        ? static_cast<size_t>(width) * height : 0;
    for (auto const &output : outputs) {
        const AovKind kind = output.kind;
        size_t plane = 0;
        if (firstSample == 0 && output.bound && !TracedAov(kind) && kind != AovKind::Unknown) {
            auto found = std::find_if(primaryPlanes.begin(), primaryPlanes.end(),
                [kind](auto const &p) { return p.kind == kind; });
            plane = static_cast<size_t>(found - primaryPlanes.begin());
            if (found == primaryPlanes.end()) {
                AovPlane staging{kind, static_cast<size_t>(output::AovComponents(kind)), {}, {}};
                if (output::IntegerAov(kind)) staging.ints.resize(scratchPixels);
                else staging.floats.resize(scratchPixels * staging.components);
                primaryPlanes.push_back(std::move(staging));
            }
        }
        resolvedOutputs.push_back({output.bound, kind, plane});
    }
    // Starts a denoise of the accumulation so far on the denoise worker.
    // `interval`: a preview while the render goes on (every denoiseInterval
    // passes), denoised without the guides' prefilter: the guides are means
    // that barely change between intervals, and on the CPU the prefilter
    // took about 6 times the plain denoise's time from the render's threads
    // (docs/denoising.md). The final and paused denoises keep the settings,
    // so the finished image is unchanged.
    auto launchDenoise = [&](bool interval) {
        const size_t count = static_cast<size_t>(width) * height;
        auto job = std::make_shared<DenoiseJob>();
        job->width = width;
        job->height = height;
        job->samples = samples.load();
        job->frame = frame;
        job->settings = settings.denoiseSettings;
        if (interval) job->settings.prefilter = false;
        std::string error;
        job->denoiser = renderDevice->CreateDenoiser(&error);
        if (!job->denoiser && !m_denoiserWarned) {
            m_denoiserWarned = true;
            m_renderer->logger().warning("denoise",
                "cannot denoise (" + error + "); the denoised colour is the raw colour");
        }
        job->input.resize(count * kDenoiseFloats);
        float const *sums = m_denoiseSums.data();
        uint32_t const *counts = m_denoiseCounts.data();
        tbb::parallel_for(size_t(0), count, [&](size_t pixel) {
            const float scale = counts[pixel] ? 1.0f / float(counts[pixel]) : 0.0f;
            for (size_t i = 0; i < kDenoiseFloats; ++i)
                job->input[pixel * kDenoiseFloats + i] = sums[pixel * kDenoiseFloats + i] * scale;
        });
        if (!m_denoiseWorker) m_denoiseWorker = std::make_unique<DenoiseWorker>();
        m_denoiseWorker->Submit(job);
        // What was denoised: an interval denoise does not stand for the
        // final one (the final loop below compares these settings).
        m_denoisedSettings = job->settings;
        m_denoiseJob = std::move(job);
        m_denoisedSamples = samples.load();
    };
    // Publishes a finished denoise of this accumulation: the denoised AOVs,
    // and with `denoise` the colour's display. True when one was.
    auto publishDenoise = [&]() -> bool {
        auto const job = m_denoiseJob;
        if (!job || job->published || !job->done.load(std::memory_order_acquire)) return false;
        job->published = true;
        if (job->frame != frame || job->output.size() != static_cast<size_t>(width) * height * 4)
            return false;
        if (!job->error.empty()) m_renderer->logger().warning("denoise", "denoising failed: " + job->error);
        for (size_t index = 0; index < resolvedOutputs.size(); ++index) {
            auto const &resolved = resolvedOutputs[index];
            if (resolved.kind != AovKind::Denoised || !resolved.bound) continue;
            target.PublishDenoised(index, job->output, width, height, frame);
        }
        if (settings.denoise) target.PresentDenoisedColor(job->output, width, height, frame);
        denoisesPublished.fetch_add(1);
        lastDenoiseMilliseconds.store(job->milliseconds);
        {
            std::lock_guard lock(m_mutex);
            m_stats.denoiserName = job->denoiser ? job->denoiser->Name() : std::string("none (raw colour)");
        }
        return true;
    };
    auto denoiseInFlight = [&] {
        return m_denoiseJob && !m_denoiseJob->done.load(std::memory_order_acquire);
    };
    std::vector<Color3> passColors(hasColor ? scratchPixels : 0);
    std::vector<float> passCoverage(hasColor ? scratchPixels : 0);
    std::vector<device::LightAovPixel> passLightAovs(hasLight ? scratchPixels : 0);
    std::vector<unsigned int> rngStates(hasColor ? scratchPixels : 0);
    {
        std::lock_guard lock(m_mutex);
        m_stats.firstPassPrimaryScratchBytes = 0;
        for (auto const &plane : primaryPlanes)
            m_stats.firstPassPrimaryScratchBytes += plane.floats.capacity() * sizeof(float) +
                plane.ints.capacity() * sizeof(int);
        m_stats.colorPassScratchBytes = passColors.capacity() * sizeof(Color3) +
            passCoverage.capacity() * sizeof(float) +
            passLightAovs.capacity() * sizeof(device::LightAovPixel);
        m_stats.colorRngStorageBytes = (rngStates.capacity() + m_committedRngStates.capacity()) * sizeof(unsigned int);
    }
    if (!hasColor && !hasGeometric) {
        if (target.StopRequested()) return Outcome::Stopped;
        if (firstSample == 0) {
            for (size_t index = 0; index < resolvedOutputs.size(); ++index)
                if (resolvedOutputs[index].bound) target.Publish(index, frame);
            samples.store(1);
        }
        if (target.StopRequested()) return Outcome::Stopped;
        target.FramePublished(frame);
        {
            std::lock_guard lock(m_mutex);
            m_timing.complete = true;
        }
        return Outcome::Finished;
    }
    struct PresentationCleanup {
        ProgressiveTarget &target;
        ~PresentationCleanup() { target.ClearPresentation(); }
    } presentationCleanup{target};
    if (m_tileWidth != width || m_tileHeight != height) {
        m_refinementTiles = MakeRefinementTiles(width, height);
        m_tileWidth = width; m_tileHeight = height;
    }
    auto const &tiles = m_refinementTiles;
    std::vector<device::PixelTile> deviceTiles;
    deviceTiles.reserve(tiles.size());
    for (auto const &tile : tiles)
        deviceTiles.push_back({tile.x, tile.y, tile.width, tile.height, tile.index});
    device::AdaptiveSettings adaptive;
    adaptive.threshold = m_adaptiveSums.empty() ? 0.0f : settings.adaptiveThreshold;
    adaptive.minSamples = settings.adaptiveMinSamples;
    // Pixels traced by the current pass; converged pixels keep their value.
    auto traced = [&](size_t pixel) {
        return !adaptive.Enabled() || m_adaptiveActive.Active(pixel);
    };
    // Set once texture streaming restarted the accumulation: the geometric
    // outputs of the first pass are already published.
    bool restarted = false;
    // The time limit: the wall-clock time the render thread spends on the
    // accumulation's full-resolution passes (preview, refinement delay,
    // texture settling and time between Runs excluded; pauses included).
    // Checked at pass boundaries, so every pixel has at least one sample; a
    // resumed accumulation keeps the time it spent.
    if (firstSample == 0) m_accumulationSeconds = 0.0;
    const double timeLimit = hasColor ? settings.timeLimitSeconds : 0.0;
    auto timeMark = std::chrono::steady_clock::now();
    auto elapsedFraction = [&] {
        const auto now = std::chrono::steady_clock::now();
        m_accumulationSeconds += std::chrono::duration<double>(now - timeMark).count();
        timeMark = now;
        return timeLimit > 0.0 ? m_accumulationSeconds / timeLimit : 0.0;
    };
    profiling::Attach profile;
    for (int sample = firstSample; sample < sampleCount; ++sample) {
        if (adaptive.Enabled() && m_adaptiveActive.Count() == 0) break;
        if (timeLimit > 0.0 && sample > 0) {
            const double fraction = elapsedFraction();
            timeLimitFraction.store(std::min(fraction, 1.0));
            if (fraction >= 1.0) {
                timeLimitReached.store(true);
                break;
            }
        }
        target.PassBoundary();
        if (target.StopRequested()) return Outcome::Stopped;
        while (target.PauseRequested()) {
            if (target.StopRequested()) return Outcome::Stopped;
            // Paused: show the accumulation so far denoised.
            if (denoising) {
                publishDenoise();
                if (!denoiseInFlight() && samples.load() > 0 &&
                    (samples.load() > m_denoisedSamples || !(m_denoisedSettings == settings.denoiseSettings)))
                    launchDenoise(false);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        // Pass boundary: continue on the GPU from this sample index. The
        // per-pixel RNG state and the accumulated average carry over, so the
        // image keeps converging instead of restarting.
        if (previewingGpu) {
            // Test hook: the CPU preview renders at most this many samples
            // and then waits here for the GPU, so the handover happens
            // however long the GPU's preparation takes.
            if (const char *limit = std::getenv("PRISM_TEST_GPU_PREVIEW_MAX_SAMPLES");
                limit && *limit && std::atoi(limit) > 0 && sample >= std::atoi(limit)) {
                while (!AdvanceDevicePreparation(renderDevice, run.scene)) {
                    if (target.StopRequested()) return Outcome::Stopped;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
            if (AdvanceDevicePreparation(renderDevice, run.scene)) {
                previewingGpu = false;
                activeDevice = renderDevice.get();
                gpuPreviewHandoverSample.store(static_cast<uint64_t>(sample));
                gpuPreviewHandovers.fetch_add(1);
                EndGpuPreview(*renderDevice, run.preference);
            } else {
                gpuPreviewSamples.fetch_add(1);
            }
        }
        // Texture streaming: once the levels the first samples asked for are
        // resident, the accumulation starts again (the device bounds how
        // often). Geometric outputs are kept: they do not sample textures.
        if (!previewingGpu && sample > 0 && hasColor && activeDevice->AccumulationOutdated(scene)) {
            for (size_t index = 0; index < outputs.size(); ++index)
                if (TracedAov(outputs[index].kind) && outputs[index].bound) target.BeginFrame(index, false);
            if (!m_adaptiveSums.empty()) {
                std::fill(m_adaptiveSums.begin(), m_adaptiveSums.end(), 0.0f);
                m_adaptiveActive.Reset(width, height);
            }
            samples.store(0);
            if (denoising) ResetDenoise(static_cast<size_t>(width) * height);
            {
                std::lock_guard lock(m_mutex);
                m_timing.passes.clear();
                m_timing.completedPasses = 0;
                m_timing.activePixels = size_t(width) * height;
            }
            textureStreamingRestarts.fetch_add(1);
            load_profile::Mark("frame.texture_restart", "sample=" + std::to_string(sample));
            restarted = true;
            sample = 0;
        }
        if (sample != 0)
            std::copy(m_committedRngStates.begin(), m_committedRngStates.end(), rngStates.begin());
        const bool presenting = target.BeginPresentation(frame, tiles.size());
        const auto traceStart = std::chrono::steady_clock::now();
        // The pixel-samples this pass traces (render progress timing).
        const size_t passPixels = !hasColor ? 0
            : adaptive.Enabled() ? m_adaptiveActive.Count() : static_cast<size_t>(width) * height;
        using Clock = std::chrono::steady_clock;
        const auto milliseconds = [](auto duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        std::vector<std::atomic<bool>> ready(tiles.size());
        for (auto &flag : ready) flag.store(false, std::memory_order_relaxed);
        std::vector<bool> emitted(tiles.size(), false);
        std::atomic<uint64_t> rayNanoseconds{0}, aovNanoseconds{0};
        std::atomic<bool> abandonPass{false};
        std::mutex completionMutex;
        std::condition_variable completion;
        bool finished = false;
        double waitMilliseconds = 0, payloadMilliseconds = 0, publicationMilliseconds = 0;
        device::PassRequest passRequest;
        passRequest.camera = cameraParameters;
        passRequest.width = width;
        passRequest.height = height;
        passRequest.seed = static_cast<unsigned int>(settings.seed);
        passRequest.sample = sample;
        passRequest.shading.sampleClamp = settings.sampleClamp;
        passRequest.shading.nee = settings.nee;
        passRequest.shading.exposureScale = run.view.exposureScale;
        passRequest.shading.transparentEscapes = settings.transparentEscapes;
        passRequest.tiles = deviceTiles;
        passRequest.activePixels = adaptive.Enabled() ? m_adaptiveActive.Mask() : nullptr;
        passRequest.traceColor = hasColor;
        passRequest.tracePrimary = sample == 0 && !primaryPlanes.empty();
        passRequest.rngStates = rngStates.data();
        passRequest.colors = passColors.data();
        passRequest.coverage = passCoverage.data();
        passRequest.lightAovs = hasLight ? passLightAovs.data() : nullptr;
        passRequest.lightAovGuidesOnly = guidesOnly;
        passRequest.abandoned = [&] {
            return abandonPass.load(std::memory_order_relaxed) || target.StopRequested();
        };
        passRequest.paused = [&] { return target.PauseRequested(); };
        passRequest.resolvePrimary = [&](unsigned x, unsigned y, RayHit const &hit) {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            for (auto &plane : primaryPlanes)
                target.ResolvePrimary(plane.kind, hit,
                    plane.floats.empty() ? nullptr : plane.floats.data() + pixel * plane.components,
                    plane.ints.empty() ? nullptr : plane.ints.data() + pixel);
        };
        passRequest.tileFinished = [&](size_t position) {
            ready[position].store(true, std::memory_order_release);
        };
        // Reserve execution capacity for the host and coordinator.
        passRequest.concurrency = std::max(1, tbb::this_task_arena::max_concurrency() - 2);
        // The CPU preview shares the machine with the GPU preparation (DXC and
        // driver compiles). Measured on 28 threads, 7 or more preview threads
        // slowed a 35 s preparation by 20-60%, 4 or fewer did not: keep an
        // eighth of the threads.
        if (previewingGpu)
            passRequest.concurrency = std::max(1, std::min(passRequest.concurrency,
                m_gpuPreviewThreads > 0 ? m_gpuPreviewThreads
                    : std::max(2, tbb::this_task_arena::max_concurrency() / 8)));
        passRequest.colorPixels = &colorTracedPixels;
        passRequest.geometricOnlyPixels = &geometricOnlyPixels;
        passRequest.rayNanoseconds = &rayNanoseconds;
        passRequest.primaryNanoseconds = &aovNanoseconds;
        // One pass-wide device job replaces the barrier after each 64 tiles.
        // The coordinator alone owns presentation packets. Acquire/release
        // completion flags make only fully written tiles visible to it.
        device::RenderDevice *const passDevice = activeDevice;
        auto worker = std::async(std::launch::async, [&] {
            logging::Bind logBinding(&m_renderer->logger());
            progress::Bind progressBinding(&progress);
            auto finish = [&] {
                { std::lock_guard lock(completionMutex); finished = true; }
                completion.notify_one();
            };
            try {
                passDevice->RenderPass(scene, passRequest);
            } catch (...) { finish(); throw; }
            finish();
        });
        struct DrainPass {
            std::atomic<bool> &abandon;
            std::future<void> &worker;
            ~DrainPass() {
                // Packing/allocation can throw while workers are paused.
                // Unblock them before the future's destructor waits, and
                // before any of the captured pass storage is destroyed.
                abandon.store(true, std::memory_order_relaxed);
                if (worker.valid()) worker.wait();
            }
        } drainPass{abandonPass, worker};
        auto nextDisplay = traceStart + std::chrono::milliseconds(100);
        for (;;) {
            const auto waitStart = Clock::now();
            bool done;
            {
                std::unique_lock lock(completionMutex);
                completion.wait_until(lock, nextDisplay, [&] { return finished; });
                done = finished;
            }
            waitMilliseconds += milliseconds(Clock::now() - waitStart);
            if (done) break;
            if (denoising && !target.StopRequested()) publishDenoise();
            if (!target.StopRequested() && presenting) {
                const auto payloadStart = Clock::now();
                std::vector<size_t> completed;
                for (size_t i = 0; i < tiles.size(); ++i)
                    if (!emitted[i] && ready[i].load(std::memory_order_acquire)) {
                        completed.push_back(i); emitted[i] = true;
                    }
                PassTiles passTiles;
                passTiles.tiles = tiles;
                passTiles.completed = completed;
                passTiles.width = width;
                passTiles.height = height;
                passTiles.sample = sample;
                passTiles.colors = passColors.data();
                passTiles.coverage = passCoverage.data();
                passTiles.traced = traced;
                // Packing a publication is bounded memory work. Running it
                // here avoids entering the render scheduler while the host
                // is waiting for the next immutable display packet.
                double publication = 0.0;
                const bool published = target.PresentTiles(passTiles, publication);
                payloadMilliseconds += milliseconds(Clock::now() - payloadStart) - publication;
                publicationMilliseconds += publication;
                if (published) {
                    std::lock_guard lock(m_mutex);
                    ++m_stats.refinementPublishes;
                }
            }
            // Keep cadence anchored to the pass, rather than adding payload
            // construction time to every interval. If packing overran an
            // interval, consume new completions immediately instead of
            // queuing a backlog of timer events.
            nextDisplay = std::max(nextDisplay + std::chrono::milliseconds(100), Clock::now());
        }
        // Also propagates worker errors; never release worker-owned memory
        // while cancellation or an exception is still draining the TBB job.
        worker.get();
        if (target.StopRequested()) return Outcome::Stopped;
        const auto commitStart = std::chrono::steady_clock::now();
        profiling::Scope filmTimer(profiling::Film);
        // Release previous raw image references so publication can recycle
        // their storage.
        if (presenting) target.EndPresentation();
        // Commit is bounded buffer work: finish it even if a stop arrives.
        // The next RNG state and sample count advance only with publication.
        tbb::parallel_for(tbb::blocked_range2d<unsigned int>(0, height, 8, 0, width, 8),
            [&](tbb::blocked_range2d<unsigned int> const &range) {
                std::vector<uint64_t> tileWriteNanoseconds(profileBufferWrites ? outputs.size() : 0, 0);
                for (unsigned int y = range.rows().begin(); y != range.rows().end(); ++y) {
                    for (unsigned int x = range.cols().begin(); x != range.cols().end(); ++x) {
                        const size_t pixel = static_cast<size_t>(y) * width + x;
                        // Front ends address their storage bottom-up (Hydra's
                        // render buffers). Camera sampling remains top-down
                        // so the generated rays match standalone PrismRT.
                        const unsigned int bufferY = height - 1 - y;
                        for (size_t index = 0; index < outputs.size(); ++index) {
                            auto const &resolved = resolvedOutputs[index];
                            const AovKind kind = resolved.kind;
                            if ((sample != 0 || restarted) && !TracedAov(kind)) continue;
                            // Written by the denoise publication.
                            if (kind == AovKind::Denoised) continue;
                            if (!resolved.bound) continue;
                            const auto writeStart = profileBufferWrites
                                ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
                            // Converged pixels keep their accumulated value
                            // in the storage this pass publishes.
                            if (TracedAov(kind) && !traced(pixel)) {
                                target.KeepAccumulated(index, x, bufferY);
                                continue;
                            }
                            if (kind == AovKind::Color) {
                                Color3 const &c = passColors[pixel];
                                float v[4] = {c.x(), c.y(), c.z(), passCoverage[pixel]};
                                target.WriteFloat(index, x, bufferY, v, 4, true);
                            } else if (output::LightAov(kind)) {
                                Color3 const &c = LightAovValue(passLightAovs[pixel], kind);
                                const float v[3] = {c.x(), c.y(), c.z()};
                                target.WriteFloat(index, x, bufferY, v, 3, true);
                            } else if (kind != AovKind::Unknown && sample == 0) {
                                auto const &plane = primaryPlanes[resolved.plane];
                                if (output::IntegerAov(kind))
                                    target.WriteInt(index, x, bufferY, plane.ints.data() + pixel, 1);
                                else
                                    target.WriteFloat(index, x, bufferY,
                                        plane.floats.data() + pixel * plane.components, plane.components, false);
                            }
                            if (profileBufferWrites) {
                                tileWriteNanoseconds[index] += static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - writeStart).count());
                            }
                        }
                    }
                }
                if (profileBufferWrites) {
                    for (size_t i = 0; i < outputs.size(); ++i) {
                        if (tileWriteNanoseconds[i] == 0) continue;
                        if (resolvedOutputs[i].bound) target.RecordWriteNanoseconds(i, tileWriteNanoseconds[i]);
                    }
                }
            });
        for (size_t index = 0; index < outputs.size(); ++index) {
            // Geometric AOVs are deterministic and only written on sample 0.
            // Color changes every pass as accumulation progresses.
            if ((sample != 0 || restarted) && !TracedAov(outputs[index].kind)) continue;
            if (outputs[index].kind == AovKind::Denoised) continue;
            if (outputs[index].bound) target.Publish(index, frame);
        }
        target.FramePublished(frame);
        filmTimer.stop();
        {
            std::lock_guard lock(m_mutex);
            m_stats.lastPassTraceMilliseconds = std::chrono::duration<double, std::milli>(commitStart - traceStart).count();
            m_stats.lastPassCommitMilliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - commitStart).count();
            m_stats.lastPassRayWorkerMilliseconds = rayNanoseconds.load() * 1e-6;
            m_stats.lastPassAovWorkerMilliseconds = aovNanoseconds.load() * 1e-6;
            m_stats.lastPassWaitMilliseconds = waitMilliseconds;
            m_stats.lastPassPayloadMilliseconds = payloadMilliseconds;
            m_stats.lastPassPublicationMilliseconds = publicationMilliseconds;
        }
        if (denoising) {
            float *sums = m_denoiseSums.data();
            uint32_t *counts = m_denoiseCounts.data();
            tbb::parallel_for(size_t(0), passColors.size(), [&](size_t pixel) {
                if (!traced(pixel)) return;
                float *s = sums + kDenoiseFloats * pixel;
                Color3 const &c = passColors[pixel];
                auto const &a = passLightAovs[pixel];
                for (int i = 0; i < 3; ++i) {
                    s[i] += c[i];
                    s[4 + i] += a.albedo[i];
                    s[7 + i] += a.normal[i];
                }
                s[3] += passCoverage[pixel];
                ++counts[pixel];
            });
        }
        if (adaptive.Enabled()) {
            float *sums = m_adaptiveSums.data();
            tbb::parallel_for(size_t(0), passColors.size(), [&](size_t pixel) {
                if (!traced(pixel)) return;
                Color3 const &c = passColors[pixel];
                for (int channel = 0; channel < 3; ++channel) {
                    sums[7 * pixel + channel] += c[channel];
                    if (sample % 2 == 0) sums[7 * pixel + 3 + channel] += c[channel];
                }
                sums[7 * pixel + 6] += 1.0f;
            });
            if (sample + 1 < sampleCount && adaptive.CheckAfter(sample + 1)) {
                profiling::Scope checkTimer(profiling::AdaptiveCheck);
                const float scale = device::ImageScale(passColors.size(), [&](size_t pixel) {
                    const float *p = sums + 7 * pixel;
                    return p[6] > 0.0f ? (p[0] + p[1] + p[2]) / p[6] : 0.0f;
                });
                const size_t active = m_adaptiveActive.Update(adaptive.threshold, [&](size_t pixel) {
                    return device::AdaptiveError(sums + 7 * pixel, sums + 7 * pixel + 3, sample + 1, scale);
                });
                activePixelFraction.store(passColors.empty() ? 0.0 :
                    static_cast<double>(active) / static_cast<double>(passColors.size()));
            }
        }
        m_committedRngStates.swap(rngStates);
        samples.store(static_cast<uint64_t>(sample + 1));
        {
            std::lock_guard lock(m_mutex);
            Timing &t = m_timing;
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - traceStart).count();
            if (passPixels && seconds > 0.0) {
                if (t.passes.size() == kRatePasses) t.passes.erase(t.passes.begin());
                t.passes.push_back({double(passPixels), seconds});
            }
            t.completedPasses = static_cast<uint64_t>(sample + 1);
            t.activePixels = adaptive.Enabled() ? m_adaptiveActive.Count() : static_cast<size_t>(width) * height;
        }
        if (denoising) {
            publishDenoise();
            // Every denoiseInterval passes (the last pass denoises below).
            if (!denoiseInFlight() && sample + 1 < sampleCount &&
                static_cast<uint64_t>(sample + 1) >=
                    m_denoisedSamples + static_cast<uint64_t>(std::max(1, settings.denoiseInterval)))
                launchDenoise(true);
        }
        if (sample == 0 && !restarted) {
            std::vector<AovPlane>().swap(primaryPlanes);
            load_profile::Mark("frame.full_sample", "generation=" + std::to_string(run.generation));
        }
    }
    if (target.StopRequested()) return Outcome::Stopped;
    // The final denoise: the last samples with the current settings. The
    // render thread waits for it here, still answering a stop at once.
    if (denoising && samples.load() > 0) {
        for (;;) {
            if (target.StopRequested()) return Outcome::Stopped;
            publishDenoise();
            if (!denoiseInFlight()) {
                const bool current = m_denoiseJob && m_denoisedSamples == samples.load() &&
                    m_denoisedSettings == settings.denoiseSettings;
                if (!current) launchDenoise(false);
                else if (m_denoiseJob->published) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    {
        std::lock_guard lock(m_mutex);
        m_timing.complete = true;
    }
    return Outcome::Finished;
}

} // namespace prism::session
