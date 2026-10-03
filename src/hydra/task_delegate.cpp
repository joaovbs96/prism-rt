#include "task_delegate.h"

#include "render_buffer.h"
#include "render_pass.h"
#include "scene_bridge.h"
#include "selection_settings.h"
#include "native_tasks.h"
#include "transport_filter.h"

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
#include "pxr/imaging/hd/selectionsSchema.h"

#include <algorithm>
#include <limits>

PXR_NAMESPACE_OPEN_SCOPE
namespace {
TfToken const _samples("prism:samplesPerPixel");
TfToken const _maxDepth("prism:maxDepth");
TfToken const _rouletteDepth("prism:rrMinDepth");
TfToken const _seed("prism:seed");
TfToken const _sampleClamp("prism:maxSampleRadiance");
TfToken const _adaptivePreview("prism:adaptivePreview");
TfToken const _domeCameraVisibility("domeLightCameraVisibility");
TfToken const _sphereBatching("prism:sphereBatching");
TfToken const _previewTargetMs("prism:previewTargetMs");
TfToken const _previewMaxDimension("prism:previewMaxDimension");
TfToken const _refinementDelayMs("prism:refinementDelayMs");
TfToken const _environmentSampling("prism:environmentSampling");
TfToken const _textureFiltering("prism:textureFiltering");
TfToken const _resourceRevision("prism:resourceRevision");
TfToken const _selectionOutline("prism:selectionOutline");
TfToken const _selectionOutlineRadius("prism:selectionOutlineRadius");
TfToken const _selectionUseHostSettings("prism:selectionUseHostSettings");
TfToken const _usdviewSelection("prism:usdviewSelection");

template<class T>
T _Get(VtValue const &v, T fallback)
{
    return v.IsHolding<T>() ? v.UncheckedGet<T>() : fallback;
}
}

PrismTaskDelegate::PrismTaskDelegate()
    : _renderThread(std::make_unique<HdRenderThread>()),
      _thread(_renderThread.get()),
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
    if (_renderThread)
        _renderThread->StopThread();
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
    } else {
        // Also keep the legacy CreateDelegate/HdRenderIndex path functional.
        _sourceSceneIndex = terminalSceneIndex;
    }
    if (!_sourceSceneIndex)
        return;

    _ownedBridge = std::make_unique<prism::hydra::SceneBridge>(
        _sourceSceneIndex, _renderThread.get());
    _bridge = _ownedBridge.get();
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
HdInstancer *PrismTaskDelegate::CreateInstancer(HdSceneDelegate *, SdfPath const &)
{ return nullptr; }
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
{ return type == HdPrimTypeTokens->renderBuffer ? new PrismRenderBuffer(id, _thread) : nullptr; }
HdBprim *PrismTaskDelegate::CreateFallbackBprim(TfToken const &type)
{ return type == HdPrimTypeTokens->renderBuffer
    ? new PrismRenderBuffer(SdfPath::EmptyPath(), _thread) : nullptr; }
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
    if (name == HdAovTokens->normal || name == HdAovTokens->Neye ||
        name == HdAovTokens->Peye)
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
    return {{"Samples", _samples, VtValue(256)},
            {"Maximum depth", _maxDepth, VtValue(32)},
            {"Roulette depth", _rouletteDepth, VtValue(4)},
            {"Seed", _seed, VtValue(0)},
            {"Sample clamp", _sampleClamp, VtValue(10.0f)},
            {"Adaptive preview", _adaptivePreview, VtValue(true)},
            {"Dome light camera visibility", _domeCameraVisibility, VtValue(true)},
            {"Sphere batching (experimental)", _sphereBatching, VtValue(false)},
            {"Preview target (ms)", _previewTargetMs, VtValue(33)},
            {"Preview maximum dimension", _previewMaxDimension, VtValue(96)},
            {"Refinement delay (ms)", _refinementDelayMs, VtValue(100)},
            {"Environment sampling", _environmentSampling, VtValue(std::string("mis"))},
            {"Texture filtering", _textureFiltering, VtValue(std::string("filtered"))},
            {"Resource revision", _resourceRevision, VtValue(0)},
            {"Selection outline", _selectionOutline, VtValue(true)},
            {"Selection outline radius", _selectionOutlineRadius, VtValue(2)},
            {"Use host selection settings", _selectionUseHostSettings, VtValue(false)}};
}

VtValue PrismTaskDelegate::GetRenderSetting(TfToken const &key) const
{
    prism::hydra::RenderSettings s = _bridge
        ? _bridge->GetSettings() : prism::hydra::RenderSettings{};
    if (key == _samples) return VtValue(s.samples);
    if (key == _maxDepth) return VtValue(s.maxDepth);
    if (key == _rouletteDepth) return VtValue(s.rouletteDepth);
    if (key == _seed) return VtValue(s.seed);
    if (key == _sampleClamp) return VtValue(s.sampleClamp);
    if (key == _adaptivePreview) return VtValue(s.adaptivePreview);
    if (key == _domeCameraVisibility) return VtValue(s.domeLightCameraVisibility);
    if (key == _sphereBatching) return VtValue(s.sphereBatching);
    if (key == _previewTargetMs) return VtValue(s.previewTargetMs);
    if (key == _previewMaxDimension) return VtValue(s.previewMaxDimension);
    if (key == _refinementDelayMs) return VtValue(s.refinementDelayMs);
    if (key == _environmentSampling) return VtValue(std::string(
        s.environmentSampling == prism::hydra::RenderSettings::EnvironmentSampling::Mis
            ? "mis" : "bsdf"));
    if (key == _textureFiltering) return VtValue(std::string(
        s.textureFiltering == prism::TextureFiltering::Filtered
            ? "filtered" : "bilinear"));
    if (key == _resourceRevision) return VtValue(s.resourceRevision);
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
    prism::hydra::RenderSettings s = _bridge
        ? _bridge->GetSettings() : prism::hydra::RenderSettings{};
    if (key == _samples) s.samples = std::max(1, _Get(value, s.samples));
    else if (key == _maxDepth) s.maxDepth = std::max(1, _Get(value, s.maxDepth));
    else if (key == _rouletteDepth) s.rouletteDepth = std::max(0, _Get(value, s.rouletteDepth));
    else if (key == _seed) s.seed = _Get(value, s.seed);
    else if (key == _sampleClamp) s.sampleClamp = std::max(0.0f, _Get(value, s.sampleClamp));
    else if (key == _adaptivePreview)
        s.adaptivePreview = _Get(value, s.adaptivePreview);
    else if (key == _domeCameraVisibility)
        s.domeLightCameraVisibility = _Get(value, s.domeLightCameraVisibility);
    else if (key == _sphereBatching)
        s.sphereBatching = _Get(value, s.sphereBatching);
    else if (key == _previewTargetMs)
        s.previewTargetMs = std::clamp(_Get(value, s.previewTargetMs), 1, 1000);
    else if (key == _previewMaxDimension)
        s.previewMaxDimension = std::clamp(_Get(value, s.previewMaxDimension), 16, 1024);
    else if (key == _refinementDelayMs)
        s.refinementDelayMs = std::clamp(_Get(value, s.refinementDelayMs), 0, 2000);
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
    else if (key == _resourceRevision)
        s.resourceRevision = _Get(value, s.resourceRevision);
    else return;
    if (_bridge) _bridge->SetSettings(s);
}

VtDictionary PrismTaskDelegate::GetRenderStats() const
{
    VtDictionary result = _bridge ? _bridge->GetStats() : VtDictionary{};
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
    return result;
}
bool PrismTaskDelegate::Pause() { _thread->PauseRender(); return true; }
bool PrismTaskDelegate::Resume() { _thread->ResumeRender(); return true; }
bool PrismTaskDelegate::Stop(bool) {
    // HdRenderThread exposes only the blocking StopRender operation here.
    _thread->StopRender();
    return true;
}
bool PrismTaskDelegate::Restart()
{ if (_bridge) _bridge->RestartAccumulation(); return true; }

void PrismTaskDelegate::_SyncAuthoredSettings()
{
    if (!_authoredSettingsDirty || !_sourceSceneIndex || !_bridge) return;
    _authoredSettingsDirty = false;
    VtDictionary authored;
    bool foundActive = false;
    for (const SdfPath &path : _renderSettingsPaths) {
        const auto prim = _sourceSceneIndex->GetPrim(path);
        if (prim.primType == HdPrimTypeTokens->renderSettings) {
            const auto settings = HdRenderSettingsSchema::GetFromParent(prim.dataSource);
            const auto active = settings.GetActive();
            if (!foundActive && active && active->GetTypedValue(0)) {
                foundActive = true;
                const auto namespaced = settings.GetNamespacedSettings();
                for (const auto& name : namespaced.GetNames()) {
                    if (name.GetString().rfind("prism:", 0) == 0)
                        if (const auto value = namespaced.Get(name))
                            authored[name.GetString()] = value->GetValue(0);
                }
            }
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
    if (prim.primType != HdPrimTypeTokens->domeLight &&
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
    SdfPathVector paths;
    bool renderSettingsChanged = false;
    for (auto const& entry : entries) {
        auto previous = _knownPrimTypes.find(entry.primPath);
        const bool previousWasRenderSettings =
            previous != _knownPrimTypes.end() &&
            previous->second == HdPrimTypeTokens->renderSettings;
        renderSettingsChanged = renderSettingsChanged ||
            entry.primType == HdPrimTypeTokens->renderSettings ||
            previousWasRenderSettings;
        _knownPrimTypes[entry.primPath] = entry.primType;
        if (entry.primType == HdPrimTypeTokens->renderSettings ||
            previousWasRenderSettings)
            _authoredSettingsDirty = true;
        if (_NeedsSceneRebuild(sender.GetPrim(entry.primPath))) paths.push_back(entry.primPath);
    }
    if (renderSettingsChanged) _RefreshRenderSettingsPaths();
    if (_bridge && !paths.empty())
        _bridge->MarkScenePathsDirty(prism::hydra::SceneDirtyKind::Structure, paths);
}

void PrismTaskDelegate::PrimsRemoved(HdSceneIndexBase const &,
                                     RemovedPrimEntries const &entries)
{
    if (!entries.empty()) {
        _taskContext->selectionSceneRevision.fetch_add(1, std::memory_order_relaxed);
        _taskContext->instanceTopologyRevision.fetch_add(1, std::memory_order_relaxed);
    }
    SdfPathVector paths;
    bool renderSettingsChanged = false;
    for (auto const& entry : entries) {
        // Removed prims cannot be queried from the sender. Retain known types
        // until their entire subtree has been accounted for.
        for (auto it = _knownPrimTypes.lower_bound(entry.primPath);
             it != _knownPrimTypes.end() && it->first.HasPrefix(entry.primPath);) {
            if (it->second == HdPrimTypeTokens->renderSettings) {
                _authoredSettingsDirty = true;
                renderSettingsChanged = true;
            }
            it = _knownPrimTypes.erase(it);
        }
        paths.push_back(entry.primPath);
    }
    if (renderSettingsChanged) _RefreshRenderSettingsPaths();
    if (_bridge && !paths.empty())
        _bridge->MarkScenePathsDirty(prism::hydra::SceneDirtyKind::Structure, paths);
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
            prim.primType.IsEmpty()) continue;
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
                    if (prim.primType == HdPrimTypeTokens->domeLight)
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
    bool renderSettingsChanged = false;
    for (auto const& entry : entries) {
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
        _bridge->MarkScenePathsDirty(prism::hydra::SceneDirtyKind::Structure, paths);
}

PXR_NAMESPACE_CLOSE_SCOPE
