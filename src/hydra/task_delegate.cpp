#include "task_delegate.h"

#include "render_buffer.h"
#include "render_pass.h"
#include "scene_bridge.h"
#include "selection_settings.h"
#include "native_tasks.h"
#include "transport_filter.h"
#include "../output/aov.h"
#include "../load_profile.h"

#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/instancer.h"
#include "pxr/imaging/hd/instancedBySchema.h"
#include "pxr/imaging/hd/instancerTopologySchema.h"
#include "pxr/imaging/hd/rprim.h"
#include "pxr/imaging/hd/material.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/imaging/hd/resourceRegistry.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hd/legacyTaskSchema.h"
#include "pxr/imaging/hd/lightSchema.h"
#include "pxr/imaging/hd/renderSettingsSchema.h"
#include "pxr/imaging/hd/sceneGlobalsSchema.h"
#include "pxr/imaging/hd/utils.h"
#include "pxr/imaging/hd/selectionsSchema.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/usd/sdf/assetPath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
TfToken const _samples("prism:samplesPerPixel");
TfToken const _maxDepth("prism:maxDepth");
TfToken const _rouletteDepth("prism:rrMinDepth");
TfToken const _seed("prism:seed");
TfToken const _sampleClamp("prism:maxSampleRadiance");
TfToken const _adaptiveThreshold("prism:adaptiveThreshold");
TfToken const _adaptiveMinSamples("prism:adaptiveMinSamples");
TfToken const _timeLimitSeconds("prism:timeLimitSeconds");
TfToken const _subdivisionLevel("prism:subdivisionLevel");
TfToken const _subdivisionMaxFaces("prism:subdivisionMaxFaces");
TfToken const _subdivisionBudgetFaces("prism:subdivisionBudgetFaces");
TfToken const _displacementLevel("prism:displacementLevel");
TfToken const _displacementMaxFaces("prism:displacementMaxFaces");
TfToken const _gpuTextureBudgetMiB("prism:gpuTextureBudgetMiB");
TfToken const _sssFallback("prism:sssFallback");
TfToken const _adaptivePreview("prism:adaptivePreview");
TfToken const _neeProbabilityMin("prism:neeProbabilityMin");
TfToken const _neeFromDepth("prism:neeFromDepth");
TfToken const _lightSelection("prism:lightSelection");
TfToken const _domeCameraVisibility("domeLightCameraVisibility");
TfToken const _sphereBatching("prism:sphereBatching");
TfToken const _honorDoubleSided("prism:honorDoubleSided");
TfToken const _hiddenGeometryBudget("prism:hiddenGeometryBudgetMiB");
TfToken const _previewTargetMs("prism:previewTargetMs");
TfToken const _previewMaxDimension("prism:previewMaxDimension");
TfToken const _refinementDelayMs("prism:refinementDelayMs");
TfToken const _environmentSampling("prism:environmentSampling");
TfToken const _textureFiltering("prism:textureFiltering");
TfToken const _textureStreaming("prism:textureStreaming");
TfToken const _textureStreamingPrepass("prism:textureStreamingPrepass");
TfToken const _device("prism:device");
TfToken const _denoise("prism:denoise");
TfToken const _denoiseInterval("prism:denoiseInterval");
TfToken const _denoiseQuality("prism:denoiseQuality");
TfToken const _denoisePrefilter("prism:denoisePrefilter");
TfToken const _gpuPreparationPreview("prism:gpuPreparationPreview");
TfToken const _gpuPreparationPreviewMaxMiB("prism:gpuPreparationPreviewMaxMiB");
TfToken const _backgroundPreparation("prism:backgroundPreparation");
TfToken const _resourceRevision("prism:resourceRevision");
TfToken const _selectionOutline("prism:selectionOutline");
TfToken const _selectionOutlineRadius("prism:selectionOutlineRadius");
TfToken const _selectionUseHostSettings("prism:selectionUseHostSettings");
TfToken const _usdviewSelection("prism:usdviewSelection");
TfToken const _colorConfig("prism:colorManagement:ocioConfig");
TfToken const _colorWorkingSpace("prism:colorManagement:workingSpace");
TfToken const _colorDisplay("prism:colorManagement:display");
TfToken const _colorView("prism:colorManagement:view");
TfToken const _colorOutputSpace("prism:colorManagement:outputSpace");
// Read-only: the OCIO name of the colour space the colour AOV is in.
TfToken const _colorAovSpace("prism:colorManagement:aovColorSpace");

template<class T>
T _Get(VtValue const &v, T fallback)
{
    return v.IsHolding<T>() ? v.UncheckedGet<T>() : fallback;
}

// A number of any numeric type (authored attributes may be float, double or
// int64).
template<class T>
T _Number(VtValue const &v, T fallback)
{
    return v.CanCast<T>() ? VtValue::Cast<T>(v).template UncheckedGet<T>() : fallback;
}
}

PrismTaskDelegate::PrismTaskDelegate()
    : _renderThread(std::make_unique<HdRenderThread>()),
      _thread(_renderThread.get()),
      _drive(std::make_shared<prism::hydra::RenderDrive>()),
      _taskContext(std::make_shared<prism::hydra::TaskContext>()),
      _selectionSettings(_taskContext->selectionSettings),
      _resources(std::make_shared<HdResourceRegistry>())
{
}

PrismTaskDelegate::~PrismTaskDelegate()
{
    if (_sourceSceneIndex)
        _sourceSceneIndex->RemoveObserver(HdSceneIndexObserverPtr(this));
    if (_taskContext)
        _taskContext->bridge.store(nullptr);
    // The preparation worker and running preparation first, so neither the
    // render thread nor the teardown waits for them.
    if (_bridge)
        _bridge->Shutdown();
    if (_renderThread) {
        prism::load_profile::Scope profile("teardown.render_thread");
        _renderThread->StopThread();
    }
    _ownedBridge.reset();
    _bridge = nullptr;
}

void PrismTaskDelegate::SetTerminalSceneIndex(
    HdSceneIndexBaseRefPtr const &terminalSceneIndex)
{
    _transport = dynamic_cast<PrismTransportFilter *>(terminalSceneIndex.operator->());
    if (_transport) {
        _taskContext = _transport->GetTaskContext();
        _sourceSceneIndex = _transport->GetSourceSceneIndex();
        _selectionSettings = _taskContext->selectionSettings;
    } else if (terminalSceneIndex) {
        // A host that builds its own HdRenderIndex for the delegate
        // (HdRenderIndex::New, as Hydra 1 hosts do) bypasses the plugin's
        // _CreateRenderer and its filters: apply them here, so implicit
        // surfaces, light linking and render settings work in such hosts too.
        _sourceSceneIndex = PrismAppendSceneFilters(terminalSceneIndex);
    }
    if (!_sourceSceneIndex)
        return;

    _ownedBridge = std::make_unique<prism::hydra::SceneBridge>(
        _sourceSceneIndex, _renderThread.get(), _drive);
    _bridge = _ownedBridge.get();
    if (_pendingSettings) {
        _bridge->SetSettings(*_pendingSettings);
        _pendingSettings.reset();
    }
    _taskContext->bridge.store(_bridge);
    _renderThread->SetRenderCallback([this] { _bridge->Render(); });
    _renderThread->StartThread();
    _knownPrimTypes.clear();
    SdfPathVector pending{SdfPath::AbsoluteRootPath()};
    while (!pending.empty()) {
        auto path = pending.back(); pending.pop_back();
        _knownPrimTypes[path] = _sourceSceneIndex->GetPrim(path).primType;
        auto children = _sourceSceneIndex->GetChildPrimPaths(path);
        pending.insert(pending.end(), children.begin(), children.end());
    }
    _RefreshRenderSettingsPaths();
    _sourceSceneIndex->AddObserver(HdSceneIndexObserverPtr(this));
}

void PrismTaskDelegate::Update()
{
    _SyncAuthoredSettings();
}

const TfTokenVector &PrismTaskDelegate::GetSupportedRprimTypes() const
{ static const TfTokenVector v; return v; }
const TfTokenVector &PrismTaskDelegate::GetSupportedSprimTypes() const
{ static const TfTokenVector v{HdPrimTypeTokens->camera}; return v; }
const TfTokenVector &PrismTaskDelegate::GetSupportedBprimTypes() const
{ static const TfTokenVector v{HdPrimTypeTokens->renderBuffer}; return v; }
HdResourceRegistrySharedPtr PrismTaskDelegate::GetResourceRegistry() const
{ return _resources; }

HdRenderPassSharedPtr PrismTaskDelegate::CreateRenderPass(
    HdRenderIndex *index, HdRprimCollection const &collection)
{ return std::make_shared<PrismRenderPass>(index, collection, _bridge, _thread); }
// The bridge reads instancers from the scene index, but a host's own render
// index (HdRenderIndex::New) inserts every instancer it is given and later
// marks it dirty: without an instancer to track, its change tracker reports a
// coding error for each one. A plain HdInstancer, never synced, keeps the
// tracker consistent.
HdInstancer *PrismTaskDelegate::CreateInstancer(HdSceneDelegate *delegate, SdfPath const &id)
{ return new HdInstancer(delegate, id); }
void PrismTaskDelegate::DestroyInstancer(HdInstancer *p) { delete p; }
HdRprim *PrismTaskDelegate::CreateRprim(TfToken const &, SdfPath const &)
{ return nullptr; }
void PrismTaskDelegate::DestroyRprim(HdRprim *p) { delete p; }
HdSprim *PrismTaskDelegate::CreateSprim(TfToken const &type, SdfPath const &id)
{ return type == HdPrimTypeTokens->camera ? new HdCamera(id) : nullptr; }
HdSprim *PrismTaskDelegate::CreateFallbackSprim(TfToken const &type)
{ return type == HdPrimTypeTokens->camera ? new HdCamera(SdfPath::EmptyPath()) : nullptr; }
void PrismTaskDelegate::DestroySprim(HdSprim *p) { delete p; }
HdBprim *PrismTaskDelegate::CreateBprim(TfToken const &type, SdfPath const &id)
{ return type == HdPrimTypeTokens->renderBuffer ? new PrismRenderBuffer(id, _thread, _drive) : nullptr; }
HdBprim *PrismTaskDelegate::CreateFallbackBprim(TfToken const &type)
{ return type == HdPrimTypeTokens->renderBuffer
    ? new PrismRenderBuffer(SdfPath::EmptyPath(), _thread, _drive) : nullptr; }
void PrismTaskDelegate::DestroyBprim(HdBprim *p)
{
    if (_bridge)
        _bridge->RemoveRenderBuffer(dynamic_cast<HdRenderBuffer *>(p));
    delete p;
}
void PrismTaskDelegate::CommitResources(HdChangeTracker *) {}

HdAovDescriptor PrismTaskDelegate::GetDefaultAovDescriptor(TfToken const &name) const
{
    if (name == HdAovTokens->color)
        return {HdFormatFloat32Vec4, false, VtValue(GfVec4f(0, 0, 0, 1))};
    if (name == HdAovTokens->depth)
        return {HdFormatFloat32, false, VtValue(1.0f)};
    if (name == HdAovTokens->cameraDepth)
        return {HdFormatFloat32, false,
                VtValue(std::numeric_limits<float>::infinity())};
    // World position (Pworld, also P) and the light AOVs (albedo, diffuse,
    // specular, emission; src/output/aov.h).
    const auto kind = prism::output::AovKindFromName(name.GetString());
    // The denoised colour (docs/denoising.md), like the colour.
    if (kind == prism::output::AovKind::Denoised)
        return {HdFormatFloat32Vec4, false, VtValue(GfVec4f(0, 0, 0, 1))};
    if (name == HdAovTokens->normal || name == HdAovTokens->Neye ||
        name == HdAovTokens->Peye || kind == prism::output::AovKind::Pworld ||
        prism::output::LightAov(kind))
        return {HdFormatFloat32Vec3, false, VtValue(GfVec3f(0))};
    if (name == TfToken("primvars:st"))
        return {HdFormatFloat32Vec2, false, VtValue(GfVec2f(0))};
    if (name == HdAovTokens->primId || name == HdAovTokens->instanceId ||
        name == HdAovTokens->elementId)
        return {HdFormatInt32, false, VtValue(-1)};
    return {};
}

TfToken PrismTaskDelegate::GetMaterialBindingPurpose() const { return HdTokens->full; }
TfTokenVector PrismTaskDelegate::GetMaterialRenderContexts() const
{ return {TfToken("mtlx"), TfToken()}; }
TfTokenVector PrismTaskDelegate::GetRenderSettingsNamespaces() const
{ return {TfToken("prism")}; }

HdRenderSettingDescriptorList PrismTaskDelegate::GetRenderSettingDescriptors() const
{
    // Defaults shared with the CLI (render_defaults.h); those with an
    // environment variable take its value.
    const prism::hydra::RenderSettings d;
    return {{"Samples (maximum with adaptive sampling)", _samples, VtValue(d.samples)},
            {"Maximum depth", _maxDepth, VtValue(32)},
            {"Roulette depth", _rouletteDepth, VtValue(4)},
            {"Seed", _seed, VtValue(0)},
            {"Sample clamp", _sampleClamp, VtValue(10.0f)},
            {"Adaptive sampling threshold (0 off)", _adaptiveThreshold, VtValue(d.adaptiveThreshold)},
            {"Adaptive sampling minimum samples", _adaptiveMinSamples, VtValue(d.adaptiveMinSamples)},
            // float: usdview's settings panel has no editor for double.
            {"Time limit (seconds, 0 none)", _timeLimitSeconds, VtValue(float(d.timeLimitSeconds))},
            {"Minimum NEE probability (0 always samples)", _neeProbabilityMin, VtValue(0.0f)},
            {"Probabilistic NEE from depth", _neeFromDepth, VtValue(2)},
            {"Light selection (spatial, power)", _lightSelection, VtValue(std::string("spatial"))},
            {"Adaptive preview", _adaptivePreview, VtValue(true)},
            {"Dome light camera visibility", _domeCameraVisibility, VtValue(true)},
            {"Sphere batching (experimental)", _sphereBatching, VtValue(false)},
            {"Honor doubleSided (cull back faces)", _honorDoubleSided, VtValue(false)},
            {"Hidden geometry budget (MiB, 0 unlimited)", _hiddenGeometryBudget, VtValue(0)},
            {"Subdivision level", _subdivisionLevel, VtValue(d.subdivisionLevel)},
            {"Subdivision faces per mesh", _subdivisionMaxFaces, VtValue(d.subdivisionMaxFaces)},
            {"Subdivision faces per scene", _subdivisionBudgetFaces, VtValue(d.subdivisionBudgetFaces)},
            {"Displacement subdivision level", _displacementLevel, VtValue(d.displacementLevel)},
            {"Displacement faces per mesh", _displacementMaxFaces, VtValue(d.displacementMaxFaces)},
            {"GPU texture budget (MiB, 0 automatic)", _gpuTextureBudgetMiB, VtValue(d.gpuTextureBudgetMiB)},
            {"SSS renders as diffuse", _sssFallback, VtValue(d.sssFallback)},
            {"Preview target (ms)", _previewTargetMs, VtValue(33)},
            {"Preview maximum dimension (0 automatic)", _previewMaxDimension, VtValue(0)},
            {"Refinement delay (ms)", _refinementDelayMs, VtValue(100)},
            {"Environment sampling", _environmentSampling, VtValue(std::string("mis"))},
            {"Texture filtering", _textureFiltering, VtValue(std::string("filtered"))},
            {"GPU texture streaming", _textureStreaming, VtValue(true)},
            {"Settle streamed textures before rendering", _textureStreamingPrepass, VtValue(false)},
            {"Device (auto, cpu, vulkan, metal)", _device, VtValue(std::string("auto"))},
            {"Denoise the displayed colour", _denoise, VtValue(d.denoise)},
            {"Denoise every N passes", _denoiseInterval, VtValue(d.denoiseInterval)},
            {"Denoise quality (fast, balanced, high)", _denoiseQuality,
             VtValue(std::string(prism::device::ToString(d.denoiseSettings.quality)))},
            {"Denoise the albedo and normal first", _denoisePrefilter, VtValue(d.denoiseSettings.prefilter)},
            {"CPU preview while the GPU prepares", _gpuPreparationPreview, VtValue(true)},
            {"Background preparation (off, publish, full)", _backgroundPreparation, VtValue(std::string("publish"))},
            {"CPU preview scene limit (MiB, 0 unlimited)", _gpuPreparationPreviewMaxMiB, VtValue(8192)},
            {"Resource revision", _resourceRevision, VtValue(0)},
            {"Selection outline", _selectionOutline, VtValue(true)},
            {"Selection outline radius", _selectionOutlineRadius, VtValue(2)},
            {"Use host selection settings", _selectionUseHostSettings, VtValue(false)},
            {"OpenColorIO config (empty: $OCIO, else the studio config)", _colorConfig, VtValue(std::string())},
            {"Working space (empty: ACEScg)", _colorWorkingSpace, VtValue(std::string())},
            {"Display (empty: the config's default; recorded only)", _colorDisplay, VtValue(std::string())},
            {"View (empty: the display's default; recorded only)", _colorView, VtValue(std::string())},
            {"AOV output space (empty: renderingColorSpace, else lin_rec709; working: the working space)",
             _colorOutputSpace, VtValue(std::string())},
            {"Rendering color space (below the settings prim's; empty: ACEScg)",
             HdRenderSettingsPrimTokens->renderingColorSpace, VtValue(std::string())}};
}

VtValue PrismTaskDelegate::GetRenderSetting(TfToken const &key) const
{
    prism::hydra::RenderSettings s = _CurrentSettings();
    if (key == _samples) return VtValue(s.samples);
    if (key == _maxDepth) return VtValue(s.maxDepth);
    if (key == _rouletteDepth) return VtValue(s.rouletteDepth);
    if (key == _seed) return VtValue(s.seed);
    if (key == _sampleClamp) return VtValue(s.sampleClamp);
    if (key == _adaptiveThreshold) return VtValue(s.adaptiveThreshold);
    if (key == _adaptiveMinSamples) return VtValue(s.adaptiveMinSamples);
    if (key == _timeLimitSeconds) return VtValue(float(s.timeLimitSeconds));
    if (key == _subdivisionLevel) return VtValue(s.subdivisionLevel);
    if (key == _subdivisionMaxFaces) return VtValue(s.subdivisionMaxFaces);
    if (key == _subdivisionBudgetFaces) return VtValue(s.subdivisionBudgetFaces);
    if (key == _displacementLevel) return VtValue(s.displacementLevel);
    if (key == _displacementMaxFaces) return VtValue(s.displacementMaxFaces);
    if (key == _gpuTextureBudgetMiB) return VtValue(s.gpuTextureBudgetMiB);
    if (key == _sssFallback) return VtValue(s.sssFallback);
    if (key == _adaptivePreview) return VtValue(s.adaptivePreview);
    if (key == _neeProbabilityMin) return VtValue(s.neeProbabilityMin);
    if (key == _neeFromDepth) return VtValue(s.neeFromDepth);
    if (key == _lightSelection) return VtValue(std::string(s.spatialLights ? "spatial" : "power"));
    if (key == _domeCameraVisibility) return VtValue(s.domeLightCameraVisibility);
    if (key == _sphereBatching) return VtValue(s.sphereBatching);
    if (key == _honorDoubleSided) return VtValue(s.honorDoubleSided);
    if (key == _hiddenGeometryBudget) return VtValue(s.hiddenGeometryBudgetMiB);
    if (key == _previewTargetMs) return VtValue(s.previewTargetMs);
    if (key == _previewMaxDimension) return VtValue(s.previewMaxDimension);
    if (key == _refinementDelayMs) return VtValue(s.refinementDelayMs);
    if (key == _environmentSampling) return VtValue(std::string(
        s.environmentSampling == prism::hydra::RenderSettings::EnvironmentSampling::Mis
            ? "mis" : "bsdf"));
    if (key == _textureFiltering) return VtValue(std::string(
        s.textureFiltering == prism::TextureFiltering::Filtered
            ? "filtered" : "bilinear"));
    if (key == _textureStreaming) return VtValue(s.textureStreaming);
    if (key == _textureStreamingPrepass) return VtValue(s.textureStreamingPrepass);
    if (key == _gpuPreparationPreview) return VtValue(s.gpuPreparationPreview);
    if (key == _backgroundPreparation)
        return VtValue(std::string(s.backgroundPreparation == prism::hydra::RenderSettings::BackgroundPreparation::Off
            ? "off" : s.backgroundPreparation == prism::hydra::RenderSettings::BackgroundPreparation::Full
            ? "full" : "publish"));
    if (key == _gpuPreparationPreviewMaxMiB) return VtValue(s.gpuPreparationPreviewMaxMiB);
    if (key == _resourceRevision) return VtValue(s.resourceRevision);
    if (key == _device) return VtValue(std::string(prism::device::ToString(s.device)));
    if (key == _denoise) return VtValue(s.denoise);
    if (key == _denoiseInterval) return VtValue(s.denoiseInterval);
    if (key == _denoiseQuality)
        return VtValue(std::string(prism::device::ToString(s.denoiseSettings.quality)));
    if (key == _denoisePrefilter) return VtValue(s.denoiseSettings.prefilter);
    if (key == _colorConfig) return VtValue(s.color.config);
    if (key == _colorWorkingSpace) return VtValue(s.color.workingSpace);
    if (key == _colorDisplay) return VtValue(s.color.display);
    if (key == _colorView) return VtValue(s.color.view);
    if (key == _colorOutputSpace) return VtValue(s.outputSpace);
    if (key == HdRenderSettingsPrimTokens->renderingColorSpace)
        return VtValue(s.renderingColorSpaceSetting);
    if (key == _colorAovSpace)
        return VtValue(_bridge ? _bridge->GetOutputColorSpace() : std::string());
    if (key == _selectionOutline)
        return VtValue(_selectionSettings->enableOutline.load());
    if (key == _selectionOutlineRadius)
        return VtValue(_selectionSettings->outlineRadius.load());
    if (key == _selectionUseHostSettings)
        return VtValue(!_selectionSettings->overrideTaskSettings.load());
    if (key == _usdviewSelection) {
        std::lock_guard<std::mutex> lock(_selectionSettings->usdviewMutex);
        if (!_selectionSettings->usdviewSelectionActive) return {};
        return VtValue(_selectionSettings->usdviewSelectionPayload);
    }
    return {};
}

void PrismTaskDelegate::SetRenderSetting(TfToken const &key, VtValue const &value)
{
    if (key == _usdviewSelection) {
        if (!value.IsHolding<VtStringArray>()) return;
        const auto payload = value.UncheckedGet<VtStringArray>();
        std::map<SdfPath, std::set<int>> parsed;
        for (const std::string &record : payload) {
            const size_t tab = record.find('\t');
            if (tab == std::string::npos) continue;
            const SdfPath path(record.substr(0, tab));
            if (!path.IsAbsolutePath() || path == SdfPath::AbsoluteRootPath())
                continue;
            std::set<int> indices;
            size_t begin = tab + 1;
            while (begin <= record.size()) {
                const size_t end = record.find(',', begin);
                const std::string text = record.substr(
                    begin, end == std::string::npos ? end : end - begin);
                try {
                    size_t consumed = 0;
                    const int index = std::stoi(text, &consumed);
                    if (consumed == text.size() && index >= -1)
                        indices.insert(index);
                } catch (...) {
                }
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (!indices.empty())
                parsed[path].insert(indices.begin(), indices.end());
        }
        {
            std::lock_guard<std::mutex> lock(_selectionSettings->usdviewMutex);
            _selectionSettings->usdviewSelectionActive = true;
            _selectionSettings->usdviewSelection = std::move(parsed);
            _selectionSettings->usdviewSelectionPayload = payload;
        }
        _selectionSettings->revision.fetch_add(1);
        return;
    }
    if (key == _selectionOutline || key == _selectionOutlineRadius ||
        key == _selectionUseHostSettings) {
        if (key == _selectionOutline && value.IsHolding<bool>())
            _selectionSettings->enableOutline.store(value.UncheckedGet<bool>());
        else if (key == _selectionOutlineRadius && value.IsHolding<int>())
            _selectionSettings->outlineRadius.store(
                std::clamp(value.UncheckedGet<int>(), 0, 16));
        else if (key == _selectionUseHostSettings && value.IsHolding<bool>())
            _selectionSettings->overrideTaskSettings.store(
                !value.UncheckedGet<bool>());
        else return;
        _selectionSettings->revision.fetch_add(1);
        return;
    }
    prism::hydra::RenderSettings s = _CurrentSettings();
    if (key == _samples) s.samples = std::max(1, _Get(value, s.samples));
    else if (key == _maxDepth) s.maxDepth = std::max(1, _Get(value, s.maxDepth));
    else if (key == _rouletteDepth) s.rouletteDepth = std::max(0, _Get(value, s.rouletteDepth));
    else if (key == _seed) s.seed = _Get(value, s.seed);
    else if (key == _sampleClamp) s.sampleClamp = std::max(0.0f, _Get(value, s.sampleClamp));
    else if (key == _adaptiveThreshold)
        s.adaptiveThreshold = std::max(0.0f, _Number(value, s.adaptiveThreshold));
    else if (key == _adaptiveMinSamples)
        s.adaptiveMinSamples = std::max(1, _Get(value, s.adaptiveMinSamples));
    else if (key == _timeLimitSeconds) {
        const double seconds = _Number(value, s.timeLimitSeconds);
        s.timeLimitSeconds = std::isfinite(seconds) ? std::max(0.0, seconds) : s.timeLimitSeconds;
    }
    else if (key == _subdivisionLevel)
        s.subdivisionLevel = prism::defaults::ClampSubdivisionLevel(_Number(value, s.subdivisionLevel));
    else if (key == _subdivisionMaxFaces)
        s.subdivisionMaxFaces = prism::defaults::ClampFaces(_Number<int64_t>(value, s.subdivisionMaxFaces));
    else if (key == _subdivisionBudgetFaces)
        s.subdivisionBudgetFaces = prism::defaults::ClampFaces(_Number<int64_t>(value, s.subdivisionBudgetFaces));
    else if (key == _displacementLevel)
        s.displacementLevel = prism::defaults::ClampSubdivisionLevel(_Number(value, s.displacementLevel));
    else if (key == _displacementMaxFaces)
        s.displacementMaxFaces = prism::defaults::ClampFaces(_Number<int64_t>(value, s.displacementMaxFaces));
    else if (key == _gpuTextureBudgetMiB)
        s.gpuTextureBudgetMiB = prism::defaults::ClampGpuTextureBudgetMiB(_Number<int64_t>(value, s.gpuTextureBudgetMiB));
    else if (key == _sssFallback)
        s.sssFallback = _Get(value, s.sssFallback);
    else if (key == _neeProbabilityMin)
        s.neeProbabilityMin = std::clamp(_Get(value, s.neeProbabilityMin), 0.0f, 1.0f);
    else if (key == _neeFromDepth)
        s.neeFromDepth = std::max(0, _Get(value, s.neeFromDepth));
    else if (key == _adaptivePreview)
        s.adaptivePreview = _Get(value, s.adaptivePreview);
    else if (key == _domeCameraVisibility)
        s.domeLightCameraVisibility = _Get(value, s.domeLightCameraVisibility);
    else if (key == _sphereBatching)
        s.sphereBatching = _Get(value, s.sphereBatching);
    else if (key == _honorDoubleSided)
        s.honorDoubleSided = _Get(value, s.honorDoubleSided);
    else if (key == _hiddenGeometryBudget) {
        if (!value.IsHolding<int>() || value.UncheckedGet<int>() < 0) {
            TF_WARN("PrismRT: prism:hiddenGeometryBudgetMiB requires a non-negative integer; retaining the previous value");
            return;
        }
        s.hiddenGeometryBudgetMiB = value.UncheckedGet<int>();
    }
    else if (key == _previewTargetMs)
        s.previewTargetMs = std::clamp(_Get(value, s.previewTargetMs), 1, 1000);
    else if (key == _previewMaxDimension)
        s.previewMaxDimension = _Get(value, s.previewMaxDimension) <= 0
            ? 0 : std::clamp(_Get(value, s.previewMaxDimension), 16, 1024);
    else if (key == _refinementDelayMs)
        s.refinementDelayMs = std::clamp(_Get(value, s.refinementDelayMs), 0, 2000);
    else if (key == _lightSelection &&
             (value.IsHolding<std::string>() || value.IsHolding<TfToken>())) {
        const std::string mode = value.IsHolding<std::string>()
            ? value.UncheckedGet<std::string>()
            : value.UncheckedGet<TfToken>().GetString();
        if (mode == "spatial") s.spatialLights = true;
        else if (mode == "power") s.spatialLights = false;
        else return;
    }
    else if (key == _environmentSampling &&
             (value.IsHolding<std::string>() || value.IsHolding<TfToken>())) {
        const std::string mode = value.IsHolding<std::string>()
            ? value.UncheckedGet<std::string>()
            : value.UncheckedGet<TfToken>().GetString();
        if (mode == "mis")
            s.environmentSampling = prism::hydra::RenderSettings::EnvironmentSampling::Mis;
        else if (mode == "bsdf")
            s.environmentSampling = prism::hydra::RenderSettings::EnvironmentSampling::Bsdf;
        else return;
    }
    else if (key == _textureFiltering &&
             (value.IsHolding<std::string>() || value.IsHolding<TfToken>())) {
        const std::string mode = value.IsHolding<std::string>()
            ? value.UncheckedGet<std::string>()
            : value.UncheckedGet<TfToken>().GetString();
        if (mode == "filtered")
            s.textureFiltering = prism::TextureFiltering::Filtered;
        else if (mode == "bilinear")
            s.textureFiltering = prism::TextureFiltering::Bilinear;
        else return;
    }
    else if (key == _textureStreaming)
        s.textureStreaming = _Get(value, s.textureStreaming);
    else if (key == _textureStreamingPrepass)
        s.textureStreamingPrepass = _Get(value, s.textureStreamingPrepass);
    else if (key == _gpuPreparationPreview)
        s.gpuPreparationPreview = _Get(value, s.gpuPreparationPreview);
    else if (key == _backgroundPreparation) {
        // off | publish | full (a bool: false off, true full).
        using Mode = prism::hydra::RenderSettings::BackgroundPreparation;
        if (value.IsHolding<bool>()) {
            s.backgroundPreparation = value.UncheckedGet<bool>() ? Mode::Full : Mode::Off;
        } else if (value.IsHolding<std::string>() || value.IsHolding<TfToken>()) {
            const std::string name = value.IsHolding<std::string>()
                ? value.UncheckedGet<std::string>()
                : value.UncheckedGet<TfToken>().GetString();
            if (name == "off") s.backgroundPreparation = Mode::Off;
            else if (name == "publish") s.backgroundPreparation = Mode::Publish;
            else if (name == "full") s.backgroundPreparation = Mode::Full;
            else {
                TF_WARN("PrismRT: prism:backgroundPreparation must be off, publish or full; retaining the previous value");
                return;
            }
        } else return;
    }
    else if (key == _gpuPreparationPreviewMaxMiB)
        s.gpuPreparationPreviewMaxMiB = std::max(0, _Get(value, s.gpuPreparationPreviewMaxMiB));
    else if (key == _resourceRevision)
        s.resourceRevision = _Get(value, s.resourceRevision);
    else if (key == _denoise) {
        if (!value.IsHolding<bool>()) {
            TF_WARN("PrismRT: prism:denoise must be a bool; retaining the previous value");
            return;
        }
        s.denoise = value.UncheckedGet<bool>();
    }
    else if (key == _denoiseInterval)
        s.denoiseInterval = std::max(1, _Number(value, s.denoiseInterval));
    else if (key == _denoisePrefilter)
        s.denoiseSettings.prefilter = _Get(value, s.denoiseSettings.prefilter);
    else if (key == _denoiseQuality &&
             (value.IsHolding<std::string>() || value.IsHolding<TfToken>())) {
        const std::string name = value.IsHolding<std::string>()
            ? value.UncheckedGet<std::string>()
            : value.UncheckedGet<TfToken>().GetString();
        if (!prism::device::ParseDenoiseQuality(name, &s.denoiseSettings.quality)) {
            TF_WARN("PrismRT: prism:denoiseQuality must be fast, balanced or high; retaining the previous quality");
            return;
        }
    }
    else if (key == _device &&
             (value.IsHolding<std::string>() || value.IsHolding<TfToken>())) {
        const std::string name = value.IsHolding<std::string>()
            ? value.UncheckedGet<std::string>()
            : value.UncheckedGet<TfToken>().GetString();
        if (!prism::device::ParseDevicePreference(name, &s.device)) {
            const std::string removed = prism::device::RemovedDeviceMessage(name);
            if (!removed.empty())
                TF_WARN("PrismRT: prism:device: %s; retaining the previous device", removed.c_str());
            else
                TF_WARN("PrismRT: prism:device must be auto, cpu, vulkan or metal; retaining the previous device");
            return;
        }
    }
    else if ((key == _colorConfig || key == _colorWorkingSpace || key == _colorDisplay ||
              key == _colorView || key == _colorOutputSpace ||
              key == HdRenderSettingsPrimTokens->renderingColorSpace) &&
             (value.IsHolding<std::string>() || value.IsHolding<TfToken>() ||
              value.IsHolding<SdfAssetPath>())) {
        std::string text;
        if (value.IsHolding<std::string>()) text = value.UncheckedGet<std::string>();
        else if (value.IsHolding<TfToken>()) text = value.UncheckedGet<TfToken>().GetString();
        else {
            const auto &asset = value.UncheckedGet<SdfAssetPath>();
            text = asset.GetResolvedPath().empty() ? asset.GetAssetPath() : asset.GetResolvedPath();
        }
        if (key == _colorConfig) s.color.config = text;
        else if (key == _colorWorkingSpace) s.color.workingSpace = text;
        else if (key == _colorDisplay) s.color.display = text;
        else if (key == _colorOutputSpace) s.outputSpace = text;
        else if (key == HdRenderSettingsPrimTokens->renderingColorSpace) s.renderingColorSpaceSetting = text;
        else s.color.view = text;
    }
    else return;
    // A host may set settings before its render index exists (no bridge
    // yet); they apply when the bridge is created.
    if (_bridge) _bridge->SetSettings(s);
    else _pendingSettings = std::make_unique<prism::hydra::RenderSettings>(s);
}

prism::hydra::RenderSettings PrismTaskDelegate::_CurrentSettings() const
{
    if (_bridge) return _bridge->GetSettings();
    return _pendingSettings ? *_pendingSettings : prism::hydra::RenderSettings{};
}

VtDictionary PrismTaskDelegate::GetRenderStats() const
{
    VtDictionary result = _bridge ? _bridge->GetStats() : VtDictionary{};
    result["activeRenderSettings"] = VtValue(_activeRenderSettings.GetString());
    result["selectionOffsetMilliseconds"] = VtValue(
        _taskContext->selectionOffsetNanoseconds.load(
            std::memory_order_relaxed) / 1.0e6);
    result["selectionOffsetCalls"] = VtValue(static_cast<int64_t>(
        _taskContext->selectionOffsetCalls.load(std::memory_order_relaxed)));
    result["selectionPathsVisited"] = VtValue(static_cast<int64_t>(
        _taskContext->selectionPathsVisited.load(std::memory_order_relaxed)));
    result["selectionOffsetCacheHits"] = VtValue(static_cast<int64_t>(
        _taskContext->selectionOffsetCacheHits.load(
            std::memory_order_relaxed)));
    result["selectionOffsetCacheMisses"] = VtValue(static_cast<int64_t>(
        _taskContext->selectionOffsetCacheMisses.load(
            std::memory_order_relaxed)));
    result["pickMilliseconds"] = VtValue(
        _taskContext->pickNanoseconds.load(std::memory_order_relaxed) / 1.0e6);
    result["pickCalls"] = VtValue(static_cast<int64_t>(
        _taskContext->pickCalls.load(std::memory_order_relaxed)));
    result["presentationUploadBytes"] = VtValue(static_cast<int64_t>(
        _taskContext->presentationUploadBytes.load(std::memory_order_relaxed)));
    result["presentationUploadCalls"] = VtValue(static_cast<int64_t>(
        _taskContext->presentationUploadCalls.load(std::memory_order_relaxed)));
    result["presentationSkippedUploads"] = VtValue(static_cast<int64_t>(
        _taskContext->presentationSkippedUploads.load(std::memory_order_relaxed)));
    result["presentationAtlasBytes"] = VtValue(static_cast<int64_t>(
        _taskContext->presentationAtlasBytes.load(std::memory_order_relaxed)));
    result["presentationAtlasDraws"] = VtValue(static_cast<int64_t>(
        _taskContext->presentationAtlasDraws.load(std::memory_order_relaxed)));
    return result;
}
bool PrismTaskDelegate::Pause() { _thread->PauseRender(); return true; }
bool PrismTaskDelegate::Resume() { _thread->ResumeRender(); return true; }
bool PrismTaskDelegate::Stop(bool) {
    // HdRenderThread exposes only the blocking StopRender operation here.
    // The bridge also keeps its preparation worker from restarting it.
    if (_bridge) _bridge->Stop();
    else _thread->StopRender();
    return true;
}
bool PrismTaskDelegate::Restart()
{ if (_bridge) _bridge->RestartAccumulation(); return true; }

void PrismTaskDelegate::_SyncAuthoredSettings()
{
    if (!_authoredSettingsDirty || !_sourceSceneIndex || !_bridge) return;
    _authoredSettingsDirty = false;
    VtDictionary authored;
    // The active render settings prim: the scene globals' path (the stage's
    // renderSettingsPrimPath, usdview's render settings menu, or the batch
    // renderer's choice), else the first prim flagged active.
    // HdsiRenderSettingsFilteringSceneIndex (plugin.cpp) computes the flag
    // and keeps only prism:* namespaced settings. The host owns the camera,
    // resolution, framing and purposes; prism_hydra_render reads those from
    // the same prim itself (src/output/render_products.h).
    SdfPath activePath;
    HdRenderSettingsSchema active{nullptr};
    if (HdUtils::HasActiveRenderSettingsPrim(_sourceSceneIndex, &activePath)) {
        active = HdRenderSettingsSchema::GetFromParent(
            _sourceSceneIndex->GetPrim(activePath).dataSource);
    } else {
        activePath = SdfPath();
        for (const SdfPath &path : _renderSettingsPaths) {
            const auto prim = _sourceSceneIndex->GetPrim(path);
            if (prim.primType != HdPrimTypeTokens->renderSettings) continue;
            const auto settings = HdRenderSettingsSchema::GetFromParent(prim.dataSource);
            const auto flag = settings.GetActive();
            if (flag && flag->GetTypedValue(0)) {
                active = settings;
                activePath = path;
                break;
            }
        }
    }
    _activeRenderSettings = active ? activePath : SdfPath();
    TfTokenVector bindingPurposes = prism::hydra::RenderSettings{}.materialBindingPurposes;
    std::string renderingColorSpace;
    if (active) {
        if (const auto space = active.GetRenderingColorSpace())
            renderingColorSpace = space->GetTypedValue(0).GetString();
        const auto namespaced = active.GetNamespacedSettings();
        for (const auto& name : namespaced.GetNames()) {
            if (name.GetString().rfind("prism:", 0) == 0)
                if (const auto value = namespaced.Get(name))
                    authored[name.GetString()] = value->GetValue(0);
        }
        if (const auto purposes = active.GetMaterialBindingPurposes()) {
            const VtArray<TfToken> value = purposes->GetTypedValue(0);
            bindingPurposes.assign(value.begin(), value.end());
        }
    }
    {
        prism::hydra::RenderSettings s = _bridge->GetSettings();
        // renderingColorSpace is the working space unless
        // prism:colorManagement:workingSpace names one (SetRenderSetting).
        if (s.materialBindingPurposes != bindingPurposes ||
            s.color.renderingColorSpace != renderingColorSpace) {
            s.materialBindingPurposes = bindingPurposes;
            s.color.renderingColorSpace = renderingColorSpace;
            _bridge->SetSettings(s);
        }
    }
    for (const auto& descriptor : GetRenderSettingDescriptors()) {
        const auto key = descriptor.key.GetString();
        if (_authoredSettings.count(key) && !authored.count(key))
            SetRenderSetting(descriptor.key, descriptor.defaultValue);
    }
    for (const auto& [name, value] : authored) {
        const auto old = _authoredSettings.find(name);
        if (old == _authoredSettings.end() || old->second != value)
            SetRenderSetting(TfToken(name), value);
    }
    _authoredSettings = std::move(authored);
}

std::vector<size_t> PrismTaskDelegate::_GetSceneIndexOrder(
    SdfPath const &path) const
{
    SdfPathVector ancestry;
    for (SdfPath current = path;
         current != SdfPath::AbsoluteRootPath() && !current.IsEmpty();
         current = current.GetParentPath())
        ancestry.push_back(current);
    std::reverse(ancestry.begin(), ancestry.end());

    std::vector<size_t> order;
    order.reserve(ancestry.size());
    SdfPath parent = SdfPath::AbsoluteRootPath();
    for (SdfPath const &child : ancestry) {
        const SdfPathVector children = _sourceSceneIndex->GetChildPrimPaths(parent);
        const auto found = std::find(children.begin(), children.end(), child);
        order.push_back(found == children.end()
            ? std::numeric_limits<size_t>::max()
            : static_cast<size_t>(found - children.begin()));
        parent = child;
    }
    return order;
}

void PrismTaskDelegate::_RefreshRenderSettingsPaths()
{
    _renderSettingsPaths.clear();
    _renderSettingsOrder.clear();
    for (auto const &[path, type] : _knownPrimTypes) {
        if (type != HdPrimTypeTokens->renderSettings) continue;
        _renderSettingsPaths.push_back(path);
        _renderSettingsOrder.emplace(path, _GetSceneIndexOrder(path));
    }
    std::sort(_renderSettingsPaths.begin(), _renderSettingsPaths.end(),
        [this](SdfPath const &a, SdfPath const &b) {
            const auto &aOrder = _renderSettingsOrder.at(a);
            const auto &bOrder = _renderSettingsOrder.at(b);
            const size_t common = std::min(aOrder.size(), bOrder.size());
            for (size_t i = 0; i < common; ++i) {
                if (aOrder[i] != bOrder[i])
                    // Initial population visits the last child first because
                    // it pushes children onto a LIFO traversal stack.
                    return aOrder[i] > bOrder[i];
            }
            return aOrder.size() < bOrder.size();
        });
}

static bool _NeedsSceneRebuild(HdSceneIndexPrim const &prim)
{
    if (prim.primType == HdPrimTypeTokens->camera ||
        prim.primType == HdPrimTypeTokens->renderSettings ||
        prim.primType == HdPrimTypeTokens->renderBuffer ||
        HdLegacyTaskSchema::GetFromParent(prim.dataSource)) return false;
    // Light filters and other light-like prims Prism does not render.
    if (!HdPrimTypeIsLight(prim.primType) &&
        HdLightSchema::GetFromParent(prim.dataSource)) return false;
    return true;
}

void PrismTaskDelegate::PrimsAdded(HdSceneIndexBase const &sender,
                                   AddedPrimEntries const &entries)
{
    if (!entries.empty()) {
        _taskContext->selectionSceneRevision.fetch_add(1, std::memory_order_relaxed);
        _taskContext->instanceTopologyRevision.fetch_add(1, std::memory_order_relaxed);
    }
    using prism::hydra::StructuralChange;
    std::vector<prism::hydra::SceneDirtyNotice> notices;
    bool renderSettingsChanged = false;
    for (auto const& entry : entries) {
        auto previous = _knownPrimTypes.find(entry.primPath);
        const TfToken previousType = previous == _knownPrimTypes.end() ? TfToken{} : previous->second;
        auto next = previous == _knownPrimTypes.end() ? _knownPrimTypes.upper_bound(entry.primPath) : std::next(previous);
        const bool hadChildren = next != _knownPrimTypes.end() && next->first.HasPrefix(entry.primPath);
        const bool previousWasRenderSettings = previousType == HdPrimTypeTokens->renderSettings;
        renderSettingsChanged = renderSettingsChanged ||
            entry.primType == HdPrimTypeTokens->renderSettings || previousWasRenderSettings;
        const bool previousWasGeometry = previousType == HdPrimTypeTokens->mesh || previousType == HdPrimTypeTokens->sphere;
        if (HdPrimTypeIsLight(entry.primType) && !previousWasGeometry && !hadChildren &&
            (previousType.IsEmpty() || HdPrimTypeIsLight(previousType))) {
            // Lights are rebuilt on their own, without the geometry.
            notices.push_back({prism::hydra::SceneDirtyKind::Environment, {entry.primPath},
                               entry.primPath.GetString() + ": light added"});
        } else if (_NeedsSceneRebuild(sender.GetPrim(entry.primPath)) || previousWasGeometry || hadChildren) {
            StructuralChange change{previous == _knownPrimTypes.end() ? StructuralChange::Operation::Add :
                StructuralChange::Operation::Replace, entry.primPath,previousType,entry.primType,hadChildren};
            notices.push_back({prism::hydra::SceneDirtyKind::Structure,{entry.primPath},{},{change}});
        }
        _knownPrimTypes[entry.primPath] = entry.primType;
        if (entry.primType == HdPrimTypeTokens->renderSettings || previousWasRenderSettings)
            _authoredSettingsDirty = true;
    }
    if (renderSettingsChanged) _RefreshRenderSettingsPaths();
    if (_bridge && !notices.empty()) _bridge->MarkSceneDirtyBatch(notices);
}

void PrismTaskDelegate::PrimsRemoved(HdSceneIndexBase const &,
                                     RemovedPrimEntries const &entries)
{
    if (!entries.empty()) {
        _taskContext->selectionSceneRevision.fetch_add(1, std::memory_order_relaxed);
        _taskContext->instanceTopologyRevision.fetch_add(1, std::memory_order_relaxed);
    }
    using prism::hydra::StructuralChange;
    std::vector<prism::hydra::SceneDirtyNotice> notices;
    bool renderSettingsChanged = false;
    for (auto const& entry : entries) {
        auto previous = _knownPrimTypes.find(entry.primPath);
        const TfToken previousType = previous == _knownPrimTypes.end() ? TfToken{} : previous->second;
        auto next = previous == _knownPrimTypes.end() ? _knownPrimTypes.lower_bound(entry.primPath) : std::next(previous);
        const bool hadChildren = next != _knownPrimTypes.end() && next->first.HasPrefix(entry.primPath);
        StructuralChange change{previous == _knownPrimTypes.end() ? StructuralChange::Operation::Unknown :
            StructuralChange::Operation::Remove,entry.primPath,previousType,{},hadChildren};
        if (previous != _knownPrimTypes.end() && !hadChildren && HdPrimTypeIsLight(previousType))
            notices.push_back({prism::hydra::SceneDirtyKind::Environment, {entry.primPath},
                               entry.primPath.GetString() + ": light removed"});
        else
            notices.push_back({prism::hydra::SceneDirtyKind::Structure,{entry.primPath},{},{change}});
        if (previous != _knownPrimTypes.end() && !hadChildren) {
            if (previousType == HdPrimTypeTokens->renderSettings) {
                _authoredSettingsDirty = true; renderSettingsChanged = true;
            }
            _knownPrimTypes.erase(previous);
        } else {
            // Ancestor/unknown removal remains a conservative subtree operation.
            for (auto it = _knownPrimTypes.lower_bound(entry.primPath);
                 it != _knownPrimTypes.end() && it->first.HasPrefix(entry.primPath);) {
                if (it->second == HdPrimTypeTokens->renderSettings) {
                    _authoredSettingsDirty = true; renderSettingsChanged = true;
                }
                it = _knownPrimTypes.erase(it);
            }
        }
    }
    if (renderSettingsChanged) _RefreshRenderSettingsPaths();
    if (_bridge && !notices.empty()) _bridge->MarkSceneDirtyBatch(notices);
}

void PrismTaskDelegate::PrimsDirtied(HdSceneIndexBase const &sender,
                                     DirtiedPrimEntries const &entries)
{
    std::vector<prism::hydra::SceneDirtyNotice> notices;
    for (const DirtiedPrimEntry &entry : entries) {
        const auto prim = sender.GetPrim(entry.primPath);
        bool selectionEncodingChanged = entry.dirtyLocators.IsEmpty();
        bool instanceTopologyChanged = entry.dirtyLocators.IsEmpty();
        for (HdDataSourceLocator const &locator : entry.dirtyLocators) {
            auto intersects = [&](HdDataSourceLocator const &schema) {
                return locator.HasPrefix(schema) || schema.HasPrefix(locator);
            };
            selectionEncodingChanged = selectionEncodingChanged ||
                intersects(HdSelectionsSchema::GetDefaultLocator()) ||
                intersects(HdInstancedBySchema::GetDefaultLocator()) ||
                intersects(HdInstancerTopologySchema::GetDefaultLocator());
            instanceTopologyChanged = instanceTopologyChanged ||
                intersects(HdInstancedBySchema::GetDefaultLocator()) ||
                intersects(HdInstancerTopologySchema::GetDefaultLocator());
        }
        if (selectionEncodingChanged)
            _taskContext->selectionSceneRevision.fetch_add(
                1, std::memory_order_relaxed);
        if (instanceTopologyChanged)
            _taskContext->instanceTopologyRevision.fetch_add(
                1, std::memory_order_relaxed);
        if (prim.primType == HdPrimTypeTokens->renderSettings) {
            _authoredSettingsDirty = true;
            continue;
        }
        bool selectionOnly = !entry.dirtyLocators.IsEmpty();
        for (const HdDataSourceLocator &locator : entry.dirtyLocators) {
            if (!locator.HasPrefix(HdSelectionsSchema::GetDefaultLocator())) {
                selectionOnly = false;
                break;
            }
        }
        if (selectionOnly) continue;
        // Timeline changes arrive as root sceneGlobals notices as well as
        // per-prim notices for animated data. The renderer captures sampled
        // values from those prims, so the root notice must not force a global
        // scene recapture and cancel that targeted work.
        if (entry.primPath == SdfPath::AbsoluteRootPath() &&
            prim.primType.IsEmpty()) {
            // A new active render settings prim (scene globals).
            for (HdDataSourceLocator const &locator : entry.dirtyLocators)
                if (locator.Intersects(HdSceneGlobalsSchema::GetActiveRenderSettingsPrimLocator()))
                    _authoredSettingsDirty = true;
            continue;
        }
        if (_NeedsSceneRebuild(prim)) {
            if (_bridge) {
                bool marked = false;
                for (const HdDataSourceLocator &locator : entry.dirtyLocators) {
                    prism::hydra::SceneDirtyKind kind =
                        prism::hydra::SceneDirtyKind::Unknown;
                    const std::string text = locator.GetString();
                    // These locators affect Hydra's display representation,
                    // but are not consumed by Prism's retained snapshot.
                    if (text.rfind("extent", 0) == 0 ||
                        text.rfind("displayStyle", 0) == 0) {
                        marked = true;
                        continue;
                    }
                    SdfPathVector dirtyPaths{entry.primPath};
                    if (HdPrimTypeIsLight(prim.primType))
                        kind = prism::hydra::SceneDirtyKind::Environment;
                    else if (prim.primType == HdPrimTypeTokens->material)
                        kind = prism::hydra::SceneDirtyKind::Material;
                    else if (prim.primType == HdPrimTypeTokens->geomSubset) {
                        // Subset data is captured with its parent mesh. Hydra
                        // exposes materialBindings at the root, while subset
                        // topology lives below geomSubset.
                        kind = text.rfind("materialBindings", 0) == 0
                            ? prism::hydra::SceneDirtyKind::Material
                            : prism::hydra::SceneDirtyKind::Prototype;
                        dirtyPaths.push_back(entry.primPath.GetParentPath());
                    }
                    else if (text.rfind("xform", 0) == 0 ||
                        text.rfind("visibility", 0) == 0 ||
                        text.rfind("purpose", 0) == 0 ||
                        text.rfind("instancerTopology", 0) == 0 ||
                        (prim.primType == HdPrimTypeTokens->instancer && text.rfind("primvars", 0) == 0))
                        kind = prism::hydra::SceneDirtyKind::Placement;
                    else if (text.rfind("mesh", 0) == 0 || text.rfind("sphere", 0) == 0 ||
                             text.rfind("primvars", 0) == 0)
                        kind = prism::hydra::SceneDirtyKind::Prototype;
                    else if (text.rfind("materialBindings", 0) == 0)
                        kind = prism::hydra::SceneDirtyKind::Material;
                    notices.push_back({kind, std::move(dirtyPaths),
                        entry.primPath.GetString() + ": " + text});
                    marked = true;
                }
                if (!marked) notices.push_back({
                    prism::hydra::SceneDirtyKind::Unknown, {entry.primPath},
                    entry.primPath.GetString() + ": empty dirty locator set"});
            }
        }
    }
    if (_bridge) _bridge->MarkSceneDirtyBatch(notices);
}

void PrismTaskDelegate::PrimsRenamed(HdSceneIndexBase const &,
                                     RenamedPrimEntries const &entries)
{
    _taskContext->selectionSceneRevision.fetch_add(1, std::memory_order_relaxed);
    _taskContext->instanceTopologyRevision.fetch_add(1, std::memory_order_relaxed);
    SdfPathVector paths;
    std::vector<prism::hydra::StructuralChange> changes;
    bool renderSettingsChanged = false;
    for (auto const& entry : entries) {
        auto previous = _knownPrimTypes.find(entry.oldPrimPath);
        TfToken oldType = previous == _knownPrimTypes.end() ? TfToken{} : previous->second;
        changes.push_back({prism::hydra::StructuralChange::Operation::Rename,
            entry.oldPrimPath,oldType,oldType,true,entry.newPrimPath});
        std::vector<std::pair<SdfPath, TfToken>> moved;
        for (auto it = _knownPrimTypes.lower_bound(entry.oldPrimPath);
             it != _knownPrimTypes.end() && it->first.HasPrefix(entry.oldPrimPath);) {
            renderSettingsChanged = renderSettingsChanged ||
                it->second == HdPrimTypeTokens->renderSettings;
            moved.emplace_back(it->first.ReplacePrefix(entry.oldPrimPath, entry.newPrimPath), it->second);
            it = _knownPrimTypes.erase(it);
        }
        for (auto const& item : moved) _knownPrimTypes.insert(item);
        paths.push_back(entry.oldPrimPath);
        paths.push_back(entry.newPrimPath);
    }
    if (renderSettingsChanged) {
        _authoredSettingsDirty = true;
        _RefreshRenderSettingsPaths();
    }
    if (_bridge && !paths.empty())
        _bridge->MarkSceneDirtyBatch({{prism::hydra::SceneDirtyKind::Structure,paths,{},changes}});
}

PXR_NAMESPACE_CLOSE_SCOPE
