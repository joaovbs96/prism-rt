// hdPrism inside a third-party Hydra host (docs/hydra.md, "Third-party
// hosts"). The host here is a plain Hydra 1 application: it creates the
// delegate by plugin id, builds its own HdRenderIndex fed by the UsdImaging
// scene indices, drives an HdxTaskController through HdEngine, and talks to
// the delegate only through HdRenderDelegate (SetRenderSetting, Stop) and
// HdRenderPass::IsConverged. Each mode checks one part of that contract:
//
//   alpha SCENE                   (0,0,0,0) clear, dome camera visibility off:
//                                 background alpha 0, premultiplied colour
//   settings-before-index SCENE   SetRenderSetting before the render index
//   converge-without-execute SCENE
//   repeated-execute SCENE        Execute cost; accumulation never restarts
//   two-delegates SCENE           two live delegates render as one alone
//   create-destroy SCENE CYCLES   create/render/stop/destroy; memory per cycle
//   working-spaces SCENE          two delegates, different working spaces
//   textures DIR                  no open handle on textures; same-path edits
//   stop-latency SCENE CAMERA DEVICE WIDTH HEIGHT [REPEATS] [MAX_MS]
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#undef interface
#endif

#include <pxr/pxr.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/renderTask.h>
#include <pxr/imaging/hdx/taskController.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
using Clock = std::chrono::steady_clock;
double Milliseconds(Clock::duration d)
{ return std::chrono::duration<double, std::milli>(d).count(); }

void Require(bool condition, std::string const &reason)
{ if (!condition) throw std::runtime_error(reason); }

// Warnings the delegate issues (TF_WARN), counted per test.
class WarningCapture final : public TfDiagnosticMgr::Delegate {
public:
    WarningCapture() { TfDiagnosticMgr::GetInstance().AddDelegate(this); }
    ~WarningCapture() override { TfDiagnosticMgr::GetInstance().RemoveDelegate(this); }
    void IssueError(TfError const &error) override {
        std::lock_guard lock(_mutex);
        _errors.push_back(error.GetCommentary());
    }
    void IssueFatalError(TfCallContext const &, std::string const &) override {}
    void IssueStatus(TfStatus const &) override {}
    void IssueWarning(TfWarning const &warning) override {
        std::lock_guard lock(_mutex);
        _warnings.push_back(warning.GetCommentary());
    }
    std::vector<std::string> Warnings() const {
        std::lock_guard lock(_mutex);
        return _warnings;
    }
    // Errors, coding errors among them.
    std::vector<std::string> Errors() const {
        std::lock_guard lock(_mutex);
        return _errors;
    }
private:
    mutable std::mutex _mutex;
    std::vector<std::string> _warnings, _errors;
};

// A pass-through scene index the host uses to re-sync a prim (what a host's
// own scene index does when it decides a prim changed).
class Resync final : public HdSingleInputFilteringSceneIndexBase {
public:
    static TfRefPtr<Resync> New(HdSceneIndexBaseRefPtr const &input)
    { return TfCreateRefPtr(new Resync(input)); }
    HdSceneIndexPrim GetPrim(SdfPath const &path) const override
    { return _GetInputSceneIndex()->GetPrim(path); }
    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    { return _GetInputSceneIndex()->GetChildPrimPaths(path); }
    void Dirty(SdfPath const &path, HdDataSourceLocatorSet const &locators)
    { _SendPrimsDirtied({{path, locators}}); }
protected:
    void _PrimsAdded(HdSceneIndexBase const &, HdSceneIndexObserver::AddedPrimEntries const &e) override
    { _SendPrimsAdded(e); }
    void _PrimsRemoved(HdSceneIndexBase const &, HdSceneIndexObserver::RemovedPrimEntries const &e) override
    { _SendPrimsRemoved(e); }
    void _PrimsDirtied(HdSceneIndexBase const &, HdSceneIndexObserver::DirtiedPrimEntries const &e) override
    { _SendPrimsDirtied(e); }
    void _PrimsRenamed(HdSceneIndexBase const &, HdSceneIndexObserver::RenamedPrimEntries const &e) override
    { _SendPrimsRenamed(e); }
private:
    explicit Resync(HdSceneIndexBaseRefPtr const &input)
        : HdSingleInputFilteringSceneIndexBase(input) {}
};

struct Options {
    std::string device = "cpu";
    int width = 64, height = 64, spp = 16;
    SdfPath camera;              // empty: a free camera at (0,0,cameraZ)
    double cameraZ = 4.0;
    std::string workingSpace;    // empty: the default
    bool settingsBeforeIndex = false;
    bool backgroundPreparation = true;
    // The host applies the plugin's scene filters itself (filters mode).
    bool hostFilters = false;
};

int64_t StatInt(VtDictionary const &stats, char const *name)
{
    auto it = stats.find(name);
    if (it == stats.end()) return -1;
    if (it->second.IsHolding<int64_t>()) return it->second.UncheckedGet<int64_t>();
    if (it->second.IsHolding<int>()) return it->second.UncheckedGet<int>();
    return -1;
}
double StatDouble(VtDictionary const &stats, char const *name)
{
    auto it = stats.find(name);
    if (it == stats.end()) return -1;
    if (it->second.IsHolding<double>()) return it->second.UncheckedGet<double>();
    if (it->second.IsHolding<float>()) return it->second.UncheckedGet<float>();
    return -1;
}

// What the plugin's _CreateRenderer applies before its renderer sees the
// scene (PrismAppendSceneFilters in src/hydra/transport_filter.cpp).
HdSceneIndexBaseRefPtr HostFilters(HdSceneIndexBaseRefPtr const &input)
{
    const auto toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(
        HdsiImplicitSurfaceSceneIndexTokens->toMesh);
    auto converted = HdsiImplicitSurfaceSceneIndex::New(input, HdRetainedContainerDataSource::New(
        HdPrimTypeTokens->cube, toMesh, HdPrimTypeTokens->cone, toMesh,
        HdPrimTypeTokens->cylinder, toMesh, HdPrimTypeTokens->capsule, toMesh,
        HdPrimTypeTokens->plane, toMesh));
    auto computed = HdSiExtComputationPrimvarPruningSceneIndex::New(converted);
    auto linked = HdsiLightLinkingSceneIndex::New(computed, nullptr);
    VtArray<TfToken> prefixes{TfToken("prism:")};
    return HdsiRenderSettingsFilteringSceneIndex::New(linked, HdRetainedContainerDataSource::New(
        HdsiRenderSettingsFilteringSceneIndexTokens->namespacePrefixes,
        HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(prefixes)));
}

class Host {
public:
    Host(UsdStageRefPtr const &stage, Options const &options) : _options(options) {
        _delegate = HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(
            TfToken("HdPrismRendererPlugin"));
        Require(bool(_delegate), "HdPrismRendererPlugin did not load (PXR_PLUGINPATH_NAME?)");
        Require(_delegate->IsStopSupported() && _delegate->IsPauseSupported(),
                "Delegate does not support Stop/Pause");
        if (options.settingsBeforeIndex) ApplySettings();
        _index.reset(HdRenderIndex::New(_delegate.Get(), {}));
        Require(bool(_index), "HdRenderIndex::New failed");
        UsdImagingCreateSceneIndicesInfo info;
        auto scenes = UsdImagingCreateSceneIndices(info);
        _stageScene = scenes.stageSceneIndex;
        _resync = Resync::New(options.hostFilters ? HostFilters(scenes.finalSceneIndex)
                                                  : scenes.finalSceneIndex);
        _index->InsertSceneIndex(_resync, SdfPath::AbsoluteRootPath());
        _stageScene->SetStage(stage);
        _stageScene->SetTime(UsdTimeCode::Default());
        if (!options.settingsBeforeIndex) ApplySettings();
        _controller = std::make_unique<HdxTaskController>(
            _index.get(), SdfPath("/HostTasks"), /*gpuEnabled=*/false);
        _controller->SetRenderOutputs({HdAovTokens->color});
        HdAovDescriptor color = _delegate->GetDefaultAovDescriptor(HdAovTokens->color);
        color.clearValue = VtValue(GfVec4f(0.0f));
        _controller->SetRenderOutputSettings(HdAovTokens->color, color);
        _controller->SetRenderTags({HdRenderTagTokens->geometry, HdRenderTagTokens->render});
        _controller->SetRenderBufferSize(GfVec2i(options.width, options.height));
        _controller->SetRenderViewport(GfVec4d(0, 0, options.width, options.height));
        if (!options.camera.IsEmpty()) {
            _controller->SetCameraPath(options.camera);
        } else {
            GfMatrix4d view(1.0);
            view.SetTranslate(GfVec3d(0, 0, -options.cameraZ));
            GfFrustum frustum;
            frustum.SetPerspective(45.0, double(options.width) / options.height, 0.1, 100.0);
            _controller->SetFreeCameraMatrices(view, frustum.ComputeProjectionMatrix());
        }
        _controller->SetEnablePresentation(false);
    }
    ~Host() { Teardown(); }
    void Teardown() {
        if (!_delegate) return;
        _delegate->Stop();
        _controller.reset();
        _index.reset();
        _resync.Reset();
        _stageScene.Reset();
        _delegate = HdPluginRenderDelegateUniqueHandle();
    }
    void ApplySettings() {
        Set("prism:device", VtValue(_options.device));
        Set("prism:samplesPerPixel", VtValue(_options.spp));
        Set("prism:seed", VtValue(0));
        Set("prism:adaptiveThreshold", VtValue(0.0f));
        Set("domeLightCameraVisibility", VtValue(false));
        if (!_options.workingSpace.empty())
            Set("prism:colorManagement:workingSpace", VtValue(_options.workingSpace));
        if (!_options.backgroundPreparation)
            Set("prism:backgroundPreparation", VtValue(false));
    }
    void Set(char const *key, VtValue const &value)
    { _delegate->SetRenderSetting(TfToken(key), value); }
    VtValue Get(char const *key) const
    { return _delegate->GetRenderSetting(TfToken(key)); }
    void Execute() {
        _stageScene->ApplyPendingUpdates();
        auto tasks = _controller->GetRenderingTasks();
        _engine.Execute(_index.get(), &tasks);
    }
    // What a host polls: HdRenderPass::IsConverged, through the render task
    // that owns the pass.
    bool PassConverged() const {
        bool any = false, converged = true;
        for (auto const &task : _controller->GetRenderingTasks())
            if (auto render = std::dynamic_pointer_cast<HdxRenderTask>(task)) {
                any = true;
                converged = converged && render->IsConverged();
            }
        return any && converged;
    }
    void RenderToConvergence(std::chrono::seconds limit = std::chrono::seconds(300)) {
        const auto deadline = Clock::now() + limit;
        do {
            Execute();
            Require(Clock::now() < deadline, "Render did not converge in time");
            if (!PassConverged()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (!PassConverged());
        // A host stops polling here: the image must be complete.
        Require(StatInt(Stats(), "numCompletedSamples") == _options.spp,
                "IsConverged was true before the render finished:" + DescribeStats());
    }
    std::string DescribeStats() const;
    std::vector<float> Color() const {
        HdRenderBuffer *buffer = _controller->GetRenderOutput(HdAovTokens->color);
        Require(buffer, "No colour AOV");
        Require(buffer->GetFormat() == HdFormatFloat32Vec4,
                "Colour AOV format " + std::to_string(int(buffer->GetFormat())));
        Require(int(buffer->GetWidth()) == _options.width &&
                int(buffer->GetHeight()) == _options.height, "Colour AOV has the wrong size");
        auto const *pixels = static_cast<float const *>(buffer->Map());
        std::vector<float> result;
        if (pixels) result.assign(pixels, pixels + size_t(_options.width) * _options.height * 4);
        buffer->Unmap();
        Require(!result.empty(), "Colour AOV has no pixels");
        return result;
    }
    VtDictionary Stats() const { return _delegate->GetRenderStats(); }
    HdRenderDelegate *Delegate() const { return _delegate.Get(); }
    void Dirty(SdfPath const &path, HdDataSourceLocatorSet const &locators)
    { _resync->Dirty(path, locators); }
    Options const &GetOptions() const { return _options; }

private:
    Options _options;
    HdPluginRenderDelegateUniqueHandle _delegate;
    std::unique_ptr<HdRenderIndex> _index;
    UsdImagingStageSceneIndexRefPtr _stageScene;
    TfRefPtr<Resync> _resync;
    std::unique_ptr<HdxTaskController> _controller;
    HdEngine _engine;
};

UsdStageRefPtr Open(std::string const &path)
{
    auto stage = UsdStage::Open(path);
    Require(bool(stage), "Cannot open " + path);
    return stage;
}

struct Difference { double maxAbs = 0; size_t differing = 0; };
Difference Compare(std::vector<float> const &a, std::vector<float> const &b)
{
    Require(a.size() == b.size(), "Images differ in size");
    Difference d;
    for (size_t i = 0; i < a.size(); ++i) {
        const double diff = std::abs(double(a[i]) - double(b[i]));
        if (diff > 0) ++d.differing;
        d.maxAbs = std::max(d.maxAbs, diff);
    }
    return d;
}

// Committed private memory (0 where not measured: no memory bound there).
size_t PrivateBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters), sizeof(counters));
    return counters.PrivateUsage;
#else
    return 0;
#endif
}

std::string Describe(VtDictionary const &stats)
{
    std::string text;
    for (char const *name : {"completedSamples", "preparationGeneration", "displayedGeneration",
                             "captureQueueRemaining", "capturedPrims", "snapshotBuilds"})
        text += std::string(" ") + name + "=" + std::to_string(StatInt(stats, name));
    auto pending = stats.find("preparationPending");
    if (pending != stats.end() && pending->second.IsHolding<bool>())
        text += std::string(" preparationPending=") +
            (pending->second.UncheckedGet<bool>() ? "true" : "false");
    return text;
}

std::string Host::DescribeStats() const { return Describe(Stats()); }

double Median(std::vector<double> values)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}
double Percentile(std::vector<double> values, double p)
{
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(p * (values.size() - 1) + 0.5))];
}

// 1. Background alpha 0 and premultiplied colour.
void Alpha(std::string const &scene)
{
    Options options; options.width = options.height = 64; options.spp = 32;
    Host host(Open(scene), options);
    host.RenderToConvergence();
    auto const color = host.Color();
    size_t background = 0, covered = 0, partial = 0;
    float coveredMax = 0;
    for (size_t i = 0; i < color.size(); i += 4)
        if (color[i + 3] == 1.0f)
            coveredMax = std::max({coveredMax, color[i], color[i + 1], color[i + 2]});
    Require(coveredMax > 0, "Covered pixels are black");
    for (size_t i = 0; i < color.size(); i += 4) {
        const float a = color[i + 3];
        if (a == 0.0f) {
            ++background;
            Require(color[i] == 0 && color[i + 1] == 0 && color[i + 2] == 0,
                    "A background pixel (alpha 0) carries colour");
        } else if (a == 1.0f) {
            ++covered;
        } else {
            ++partial;
            Require(a > 0 && a < 1, "Alpha out of [0,1]");
            // Premultiplied: colour scales with coverage. Straight alpha would
            // give an edge pixel the covered colour whatever its coverage.
            for (int c = 0; c < 3; ++c)
                Require(color[i + c] <= a * coveredMax * 1.5f + 1e-4f,
                        "Edge colour is not premultiplied by its alpha");
        }
    }
    // The corners are background (the scene keeps its geometry centred).
    for (size_t corner : {size_t(0), size_t(options.width - 1),
                          size_t(options.width) * (options.height - 1),
                          size_t(options.width) * options.height - 1})
        Require(color[corner * 4 + 3] == 0.0f, "A corner pixel has alpha above 0");
    std::cout << "alpha: background " << background << ", covered " << covered
              << ", partial " << partial << " pixels\n";
    Require(background > 0 && covered > 0 && partial > 0,
            "Expected background, covered and edge pixels");
}

// SetRenderSetting before the host has built its render index.
void SettingsBeforeIndex(std::string const &scene)
{
    Options before; before.width = before.height = 32; before.spp = 5;
    before.settingsBeforeIndex = true;
    Host host(Open(scene), before);
    const VtValue spp = host.Get("prism:samplesPerPixel");
    Require(spp.IsHolding<int>() && spp.UncheckedGet<int>() == 5,
            "prism:samplesPerPixel set before the render index was lost");
    const VtValue dome = host.Get("domeLightCameraVisibility");
    Require(dome.IsHolding<bool>() && !dome.UncheckedGet<bool>(),
            "domeLightCameraVisibility set before the render index was lost");
    host.RenderToConvergence();
    Require(StatInt(host.Stats(), "numCompletedSamples") == 5,
            "The render did not use the samples set before the render index");
    auto const early = host.Color();
    Options after = before; after.settingsBeforeIndex = false;
    Host reference(Open(scene), after);
    reference.RenderToConvergence();
    const auto d = Compare(early, reference.Color());
    Require(d.differing == 0, "Settings applied before the render index give another image");
    std::cout << "settings-before-index: honoured\n";
}

// 2. IsConverged turns true while nothing calls Execute. Hydra syncs the
// scene in Execute; the delegate continues capturing and publishing the
// converted scene on its own worker when Execute calls stop (docs/hydra.md,
// "Third-party hosts"), so one Execute starts the render.
void PollToConvergence(Host &host, char const *what)
{
    const auto start = Clock::now();
    while (!host.PassConverged()) {
        Require(Clock::now() - start < std::chrono::seconds(120),
                std::string(what) + ": not converged after 120 s of polling:" + Describe(host.Stats()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void ConvergeWithoutExecute(std::string const &scene)
{
    Options options; options.width = options.height = 64; options.spp = 64;
    std::vector<float> reference;
    {
        Host host(Open(scene), options);
        host.RenderToConvergence();
        reference = host.Color();
    }
    // A single Execute, then polling only.
    {
        Host host(Open(scene), options);
        host.Execute();
        const auto start = Clock::now();
        PollToConvergence(host, "single Execute");
        const double ms = Milliseconds(Clock::now() - start);
        Require(StatInt(host.Stats(), "numCompletedSamples") == options.spp,
                "Converged after a single Execute before reaching the sample count");
        const auto d = Compare(host.Color(), reference);
        std::cout << "converge-without-execute: a single Execute, then converged after " << ms
                  << " ms of polling (background preparation steps "
                  << StatInt(host.Stats(), "backgroundPreparationSteps") << ", render starts "
                  << StatInt(host.Stats(), "backgroundRenderStarts")
                  << "); differing values vs Execute-driven render " << d.differing << "\n";
        Require(d.differing == 0, "A single-Execute render differs from the Execute-driven render");
    }
    // An edit while the delegate prepares on its own: the host re-syncs a prim
    // (a notice, no Execute), and polling still converges to the same image.
    {
        Host host(Open(scene), options);
        host.Execute();
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        host.Dirty(SdfPath("/World/Triangle"), HdDataSourceLocatorSet{HdDataSourceLocator::EmptyLocator()});
        PollToConvergence(host, "edit during background preparation");
        const auto d = Compare(host.Color(), reference);
        std::cout << "converge-without-execute: edit during background preparation, differing values "
                  << d.differing << "\n";
        Require(d.differing == 0, "An edit during background preparation changed the image");
    }
    // With prism:backgroundPreparation off, a host executes until the first
    // samples (the earlier contract), and the image is the same.
    Options driven = options;
    driven.backgroundPreparation = false;
    Host host(Open(scene), driven);
    const auto deadline = Clock::now() + std::chrono::seconds(120);
    int executes = 0;
    while (StatInt(host.Stats(), "completedSamples") <= 0) {
        host.Execute();
        ++executes;
        Require(Clock::now() < deadline, "The render did not start:" + Describe(host.Stats()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const int64_t startedAt = StatInt(host.Stats(), "completedSamples");
    Require(startedAt < options.spp, "The render converged before polling began; raise spp");
    Require(!host.PassConverged(), "IsConverged was true while rendering");
    const auto start = Clock::now();
    while (!host.PassConverged()) {
        Require(Clock::now() - start < std::chrono::seconds(120),
                "IsConverged never turned true without Execute:" + Describe(host.Stats()));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const double ms = Milliseconds(Clock::now() - start);
    Require(StatInt(host.Stats(), "numCompletedSamples") == options.spp,
            "Converged without Execute before reaching the sample count");
    const auto d = Compare(host.Color(), reference);
    std::cout << "converge-without-execute: " << executes << " Execute calls until samples "
              << startedAt << "/" << options.spp << ", then converged after " << ms
              << " ms of polling only; differing values vs Execute-driven render "
              << d.differing << " (max " << d.maxAbs << ")\n";
    Require(d.differing == 0, "Buffers differ from the Execute-driven render");
}

// 3. Execute cost and no restarts when nothing changed.
void RepeatedExecute(std::string const &scene)
{
    Options options; options.width = options.height = 128; options.spp = 256;
    Host host(Open(scene), options);
    std::vector<double> unconverged, converged;
    int64_t samples = -1, cameraRevision = -1, preparation = -1;
    int restarts = 0, frames = 0;
    bool wasConverged = false;
    const auto deadline = Clock::now() + std::chrono::seconds(300);
    auto check = [&] {
        const auto stats = host.Stats();
        const int64_t now = StatInt(stats, "completedSamples");
        if (now < samples) ++restarts;
        samples = now;
        const int64_t camera = StatInt(stats, "requestedCameraRevision");
        const int64_t generation = StatInt(stats, "preparationGeneration");
        if (StatInt(stats, "displayedGeneration") >= generation && generation >= 0) {
            if (preparation >= 0 && generation != preparation) ++restarts;
            if (cameraRevision >= 0 && camera != cameraRevision) ++restarts;
            preparation = generation;
            cameraRevision = camera;
        }
    };
    while (true) {
        const bool before = host.PassConverged();
        const auto t0 = Clock::now();
        host.Execute();
        const double ms = Milliseconds(Clock::now() - t0);
        ++frames;
        check();
        // The first calls prepare the scene; time steady-state frames only.
        if (samples > 0) (before ? converged : unconverged).push_back(ms);
        if (wasConverged) Require(host.PassConverged(), "A converged render un-converged");
        wasConverged = wasConverged || host.PassConverged();
        if (converged.size() >= 500) break;
        Require(Clock::now() < deadline, "repeated-execute timed out");
        if (!before) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Require(StatInt(host.Stats(), "numCompletedSamples") == options.spp,
            "Converged below the sample count");
    std::cout << "repeated-execute: " << frames << " frames; unconverged Execute median "
              << Median(unconverged) << " ms p95 " << Percentile(unconverged, 0.95)
              << " (n=" << unconverged.size() << "); converged median " << Median(converged)
              << " ms p95 " << Percentile(converged, 0.95) << " (n=" << converged.size()
              << "); restarts " << restarts << "\n";
    Require(restarts == 0, "An unchanged Execute restarted the accumulation");
    Require(!unconverged.empty(), "No unconverged frames measured");
}

// 4a. Two live delegates with the same settings.
void TwoDelegates(std::string const &scene)
{
    Options options; options.width = options.height = 64; options.spp = 64;
    std::vector<float> reference;
    {
        Host alone(Open(scene), options);
        alone.RenderToConvergence();
        reference = alone.Color();
    }
    Host a(Open(scene), options), b(Open(scene), options);
    const auto deadline = Clock::now() + std::chrono::seconds(300);
    do {
        a.Execute(); b.Execute();
        Require(Clock::now() < deadline, "Two delegates did not converge");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (!(a.PassConverged() && b.PassConverged()));
    const auto da = Compare(a.Color(), reference), db = Compare(b.Color(), reference);
    std::cout << "two-delegates: differing values vs one delegate: " << da.differing
              << " and " << db.differing << "\n";
    Require(da.differing == 0 && db.differing == 0,
            "A delegate beside another renders differently");
}

// 4b. Create, render, stop and destroy many times.
void CreateDestroy(std::string const &scene, int cycles)
{
    Options options; options.width = options.height = 32; options.spp = 4;
    auto stage = Open(scene);
    std::vector<float> reference;
    std::vector<size_t> memory;
    const auto start = Clock::now();
    for (int cycle = 0; cycle < cycles; ++cycle) {
        {
            Host host(stage, options);
            host.RenderToConvergence();
            auto const color = host.Color();
            if (reference.empty()) reference = color;
            Require(Compare(color, reference).differing == 0,
                    "A recreated delegate renders differently");
            // Teardown while rendering (Stop first) on every other cycle.
            if (cycle % 2) { host.Set("prism:samplesPerPixel", VtValue(1 << 20)); host.Execute(); }
        }
        memory.push_back(PrivateBytes());
    }
    const int warm = std::min(5, cycles - 1);
    const double perCycle = cycles - 1 > warm
        ? (double(memory.back()) - double(memory[warm])) / (cycles - 1 - warm) : 0.0;
    std::cout << "create-destroy: " << cycles << " cycles in "
              << Milliseconds(Clock::now() - start) / 1000.0 << " s; private bytes after cycle 1 "
              << memory.front() / 1048576.0 << " MiB, after cycle " << warm + 1 << " "
              << memory[warm] / 1048576.0 << " MiB, last " << memory.back() / 1048576.0
              << " MiB; growth per cycle after warm-up " << perCycle / 1024.0 << " KiB\n";
    Require(perCycle < 2.0 * 1048576.0, "Memory grows more than 2 MiB per create/destroy cycle");
}

// 4c. Two delegates with different working spaces.
void WorkingSpaces(std::string const &scene)
{
    Options aces; aces.width = 96; aces.height = 32; aces.spp = 16; aces.cameraZ = 9;
    Options rec709 = aces; rec709.workingSpace = "lin_rec709";
    std::vector<float> acesAlone, rec709Alone;
    { Host h(Open(scene), aces); h.RenderToConvergence(); acesAlone = h.Color(); }
    { Host h(Open(scene), rec709); h.RenderToConvergence(); rec709Alone = h.Color(); }
    Require(Compare(acesAlone, rec709Alone).differing > 0,
            "The working spaces render the same image; the test sees nothing");
    WarningCapture warnings;
    {
        Host a(Open(scene), aces), b(Open(scene), rec709);
        const auto deadline = Clock::now() + std::chrono::seconds(300);
        do {
            a.Execute(); b.Execute();
            Require(Clock::now() < deadline, "Working-space delegates did not converge");
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } while (!(a.PassConverged() && b.PassConverged()));
        const auto da = Compare(a.Color(), acesAlone), db = Compare(b.Color(), rec709Alone);
        std::cout << "working-spaces: ACEScg delegate differs from alone in " << da.differing
                  << " values, lin_rec709 delegate in " << db.differing << "; warnings "
                  << warnings.Warnings().size() << "\n";
        for (auto const &w : warnings.Warnings()) std::cout << "  warning: " << w << "\n";
        Require(da.differing == 0 && db.differing == 0,
                "Delegates with different working spaces disturb each other");
        // Then a third delegate starts while both live: the defaults again.
        Host c(Open(scene), aces);
        c.RenderToConvergence();
        Require(Compare(c.Color(), acesAlone).differing == 0,
                "A new delegate inherits another delegate's working space");
    }
}

// 5. Texture handles and same-path overwrites.
void WriteSolid(std::filesystem::path const &path, float r, float g, float b)
{
    OIIO::ImageSpec spec(16, 16, 3, OIIO::TypeDesc::UINT8);
    OIIO::ImageBuf image(spec);
    const float value[3] = {r, g, b};
    OIIO::ImageBufAlgo::fill(image, value);
    const auto temp = path.parent_path() / ("staging_" + path.filename().string());
    Require(image.write(temp.string()), "Cannot write texture");
    // Overwrite in place (same path), as a host's texture editor would.
    std::ifstream in(temp, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
    in.close();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Require(bool(out), "Cannot open the texture for overwriting (held open?)");
    out.write(bytes.data(), bytes.size());
    out.close();
    Require(bool(out), "Overwriting the texture failed");
    std::filesystem::remove(temp);
}

std::array<double, 3> Mean(std::vector<float> const &color)
{
    std::array<double, 3> sum{};
    size_t n = 0;
    for (size_t i = 0; i < color.size(); i += 4) {
        if (color[i + 3] < 1.0f) continue;
        for (int c = 0; c < 3; ++c) sum[c] += color[i + c];
        ++n;
    }
    Require(n > 0, "No covered pixels");
    for (auto &v : sum) v /= double(n);
    return sum;
}

void Textures(std::filesystem::path const &dir)
{
    std::filesystem::create_directories(dir);
    const auto texture = dir / "albedo.png";
    WriteSolid(texture, 1, 0, 0);
    const auto scene = dir / "textured.usda";
    {
        std::ofstream out(scene);
        out << R"(#usda 1.0
def Xform "World"
{
    def Mesh "Panel" (prepend apiSchemas = ["MaterialBindingAPI"])
    {
        point3f[] points = [(-1,-1,0), (1,-1,0), (1,1,0), (-1,1,0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        texCoord2f[] primvars:st = [(0,0), (1,0), (1,1), (0,1)] (interpolation = "vertex")
        rel material:binding = </World/Mat>
    }
    def Material "Mat"
    {
        token outputs:surface.connect = </World/Mat/Surface.outputs:surface>
        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor.connect = </World/Mat/Texture.outputs:rgb>
            float inputs:roughness = 1
            int inputs:useSpecularWorkflow = 1
            color3f inputs:specularColor = (0,0,0)
            token outputs:surface
        }
        def Shader "Texture"
        {
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @./albedo.png@
            token inputs:sourceColorSpace = "raw"
            float2 inputs:st.connect = </World/Mat/St.outputs:result>
            float3 outputs:rgb
        }
        def Shader "St"
        {
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }
    }
    def DomeLight "Dome"
    {
        color3f inputs:color = (1,1,1)
        float inputs:intensity = 1
    }
}
)";
    }
    Options options; options.width = options.height = 32; options.spp = 8;
    options.cameraZ = 2.0;
    Host host(Open(scene.string()), options);
    host.RenderToConvergence();
    const auto red = Mean(host.Color());
    std::cout << "textures: red texture renders (" << red[0] << ", " << red[1] << ", "
              << red[2] << ")\n";
    Require(red[0] > 4 * red[1] && red[0] > 4 * red[2], "The texture did not render red");
    // The delegate is alive and has read the texture: renaming and
    // overwriting it must succeed (no handle without FILE_SHARE_DELETE/WRITE).
    const auto renamed = dir / "albedo_renamed.png";
    std::error_code error;
    std::filesystem::rename(texture, renamed, error);
    Require(!error, "Renaming the texture failed while the delegate lives: " + error.message());
    std::filesystem::rename(renamed, texture, error);
    Require(!error, "Renaming the texture back failed: " + error.message());
    std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a later mtime
    WriteSolid(texture, 0, 1, 0);
    // An Execute without any re-sync keeps the converged image (nothing told
    // the delegate the file changed).
    host.Execute();
    // The host re-syncs the material: Hydra marks its network dirty.
    host.Dirty(SdfPath("/World/Mat"), HdDataSourceLocatorSet{HdMaterialSchema::GetDefaultLocator()});
    const auto deadline = Clock::now() + std::chrono::seconds(120);
    std::array<double, 3> now{};
    do {
        host.RenderToConvergence();
        now = Mean(host.Color());
        if (now[1] > 4 * now[0]) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (Clock::now() < deadline);
    std::cout << "textures: after overwrite + material re-sync (" << now[0] << ", " << now[1]
              << ", " << now[2] << ")\n";
    Require(now[1] > 4 * now[0] && now[1] > 4 * now[2],
            "An overwritten texture was not picked up on a material re-sync");
    const VtValue revision = host.Get("prism:resourceRevision");
    Require(revision.IsHolding<int>() && revision.UncheckedGet<int>() == 0,
            "prism:resourceRevision changed");
}

// 6. Time from Stop to an idle render thread, and teardown right after.
void StopLatency(std::string const &scene, std::string const &camera, std::string const &device,
                 int width, int height, int repeats, double maxMs)
{
    Options options; options.device = device; options.width = width; options.height = height;
    options.spp = 1 << 20; options.camera = SdfPath(camera);
    std::vector<double> stops, teardowns, stopsWhilePreparing;
    const auto loadStart = Clock::now();
    // During preparation: Stop right after a few Executes, then teardown.
    double teardownWhilePreparing = 0;
    {
        auto preparing = std::make_unique<Host>(Open(scene), options);
        for (int i = 0; i < 3; ++i) preparing->Execute();
        const auto t0 = Clock::now();
        preparing->Delegate()->Stop();
        stopsWhilePreparing.push_back(Milliseconds(Clock::now() - t0));
        const auto t1 = Clock::now();
        preparing.reset();
        teardownWhilePreparing = Milliseconds(Clock::now() - t1);
    }
    auto host = std::make_unique<Host>(Open(scene), options);
    const auto deadline = Clock::now() + std::chrono::seconds(300);
    // Rendering: completed samples move (Restart begins a new accumulation).
    auto waitForProgress = [&] {
        const int64_t before = StatInt(host->Stats(), "completedSamples");
        while (true) {
            host->Execute();
            const int64_t now = StatInt(host->Stats(), "completedSamples");
            if (now > 0 && now != before) return;
            Require(Clock::now() < deadline,
                    "stop-latency: render did not start:" + Describe(host->Stats()));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    waitForProgress();
    std::cout << "stop-latency: first samples after "
              << Milliseconds(Clock::now() - loadStart) / 1000.0 << " s\n";
    for (int i = 0; i < repeats; ++i) {
        // Stop at varying points inside a pass.
        std::this_thread::sleep_for(std::chrono::milliseconds(7 * (i % 5)));
        const auto t0 = Clock::now();
        host->Delegate()->Stop();
        stops.push_back(Milliseconds(Clock::now() - t0));
        // Hydra: rendering stays stopped until Restart.
        host->Delegate()->Restart();
        waitForProgress();
    }
    // Teardown right after Stop, while a pass was running.
    for (int i = 0; i < 3; ++i) {
        const auto t0 = Clock::now();
        host->Delegate()->Stop();
        const double stopMs = Milliseconds(Clock::now() - t0);
        const auto t1 = Clock::now();
        host.reset();
        teardowns.push_back(Milliseconds(Clock::now() - t1));
        stops.push_back(stopMs);
        if (i < 2) {
            host = std::make_unique<Host>(Open(scene), options);
            waitForProgress();
        }
    }
    std::cout << "stop-latency (" << device << ", " << width << "x" << height
              << "): Stop while rendering median " << Median(stops) << " ms, p95 "
              << Percentile(stops, 0.95) << " ms, max "
              << *std::max_element(stops.begin(), stops.end()) << " ms (n=" << stops.size()
              << "); Stop while preparing " << stopsWhilePreparing.front()
              << " ms, teardown while preparing " << teardownWhilePreparing
              << " ms; teardown after Stop median " << Median(teardowns) << " ms, max "
              << *std::max_element(teardowns.begin(), teardowns.end()) << " ms\n";
    if (maxMs > 0)
        Require(*std::max_element(stops.begin(), stops.end()) <= maxMs,
                "Stop took longer than the limit");
}
// 7. Instancers in a host's own render index: no change tracker errors.
void Instancers(std::string const &scene, std::string const &camera)
{
    Options options; options.width = options.height = 48; options.spp = 4;
    options.camera = SdfPath(camera);
    WarningCapture diagnostics;
    {
        Host host(Open(scene), options);
        host.RenderToConvergence();
    }
    size_t instancerErrors = 0;
    for (auto const &error : diagnostics.Errors())
        if (error.find("instancer") != std::string::npos || error.find("Instancer") != std::string::npos) {
            ++instancerErrors;
            std::cout << "  error: " << error << "\n";
        }
    std::cout << "instancers: " << diagnostics.Errors().size() << " errors, " << instancerErrors
              << " about instancers\n";
    Require(instancerErrors == 0, "The render index reported instancer errors");
}

// 8. The plugin's scene filters apply in a host's own render index: the
// same image as a host that applies them itself (implicit surfaces, light
// linking).
void Filters(std::string const &scene, std::string const &camera)
{
    Options options; options.width = 96; options.height = 64; options.spp = 8;
    options.camera = SdfPath(camera);
    std::vector<float> plain, filtered;
    { Host host(Open(scene), options); host.RenderToConvergence(); plain = host.Color(); }
    options.hostFilters = true;
    { Host host(Open(scene), options); host.RenderToConvergence(); filtered = host.Color(); }
    size_t covered = 0;
    for (size_t i = 3; i < plain.size(); i += 4) covered += plain[i] > 0.0f;
    const auto d = Compare(plain, filtered);
    std::cout << "filters: " << covered << " covered pixels; differing values vs host-filtered "
              << d.differing << " (max " << d.maxAbs << ")\n";
    Require(covered > 0, "Nothing rendered");
    Require(d.differing == 0, "The delegate's own filters give another image than the plugin's");
}

// 9. Deleting the delegate while it prepares the scene (materials, GPU
// shaders) returns within MAX_MS. Run with an empty PRISM_CACHE_DIR, so the
// preparation compiles: the test empties it first when its name says so.
void TeardownPreparing(std::string const &scene, std::string const &camera, std::string const &device,
                       int waitMs, double maxMs)
{
    if (const char *cache = std::getenv("PRISM_CACHE_DIR");
        cache && std::string(cache).find("teardown_cache") != std::string::npos) {
        std::error_code ignored;
        std::filesystem::remove_all(cache, ignored);
        std::filesystem::create_directories(cache, ignored);
    }
    Options options; options.device = device; options.width = 320; options.height = 180;
    options.spp = 1 << 20; options.camera = SdfPath(camera);
    std::vector<double> teardowns;
    for (int round = 0; round < 2; ++round) {
        auto host = std::make_unique<Host>(Open(scene), options);
        host->Execute();
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs * (round + 1) / 2));
        const auto stats = host->Stats();
        const int64_t samples = StatInt(stats, "completedSamples");
        std::cout << "teardown-preparing (" << device << ") round " << round << " after " << waitMs * (round + 1) / 2
                  << " ms:" << Describe(stats) << "\n";
        const auto t0 = Clock::now();
        host.reset();
        teardowns.push_back(Milliseconds(Clock::now() - t0));
        std::cout << "teardown-preparing (" << device << ") round " << round << ": teardown "
                  << teardowns.back() << " ms" << (samples > 0 ? " (the render had started)" : "") << "\n";
    }
    const double worst = *std::max_element(teardowns.begin(), teardowns.end());
    Require(worst <= maxMs, "Teardown while preparing took " + std::to_string(worst) + " ms");
}
} // namespace

int main(int argc, char **argv)
{
    try {
        Require(argc >= 3, "usage: prism_hydra_host_contract_test MODE ARGS...");
        const std::string mode = argv[1];
        if (mode == "alpha") Alpha(argv[2]);
        else if (mode == "settings-before-index") SettingsBeforeIndex(argv[2]);
        else if (mode == "converge-without-execute") ConvergeWithoutExecute(argv[2]);
        else if (mode == "repeated-execute") RepeatedExecute(argv[2]);
        else if (mode == "two-delegates") TwoDelegates(argv[2]);
        else if (mode == "create-destroy")
            CreateDestroy(argv[2], argc > 3 ? std::atoi(argv[3]) : 24);
        else if (mode == "working-spaces") WorkingSpaces(argv[2]);
        else if (mode == "textures") Textures(argv[2]);
        else if (mode == "stop-latency") {
            Require(argc >= 7, "stop-latency SCENE CAMERA DEVICE WIDTH HEIGHT [REPEATS] [MAX_MS]");
            StopLatency(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atoi(argv[6]),
                        argc > 7 ? std::atoi(argv[7]) : 10, argc > 8 ? std::atof(argv[8]) : 0.0);
        } else if (mode == "instancers") {
            Require(argc >= 4, "instancers SCENE CAMERA");
            Instancers(argv[2], argv[3]);
        } else if (mode == "filters") {
            Require(argc >= 4, "filters SCENE CAMERA");
            Filters(argv[2], argv[3]);
        } else if (mode == "teardown-preparing") {
            Require(argc >= 7, "teardown-preparing SCENE CAMERA DEVICE WAIT_MS MAX_MS");
            TeardownPreparing(argv[2], argv[3], argv[4], std::atoi(argv[5]), std::atof(argv[6]));
        } else throw std::runtime_error("Unknown mode " + mode);
    } catch (std::exception const &error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        std::cout.flush();
        // Static objects in prism.dll whose destructors touch threads are
        // leaked on purpose; skip static destruction (CLAUDE.md).
        std::_Exit(1);
    }
    std::cout << "PASSED\n";
    std::cout.flush();
    std::_Exit(0);
}
