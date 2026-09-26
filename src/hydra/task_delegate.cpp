#include "task_delegate.h"

#include "render_buffer.h"
#include "render_pass.h"
#include "scene_bridge.h"
#include "selection_settings.h"
#include "native_tasks.h"
#include "transport_filter.h"

#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/instancer.h"
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
void PrismTaskDelegate::DestroyBprim(HdBprim *p) { delete p; }
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
    if (name == HdAovTokens->normal || name == HdAovTokens->Neye)
        return {HdFormatFloat32Vec3, false, VtValue(GfVec3f(0))};
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
    else return;
    if (_bridge) _bridge->SetSettings(s);
}

VtDictionary PrismTaskDelegate::GetRenderStats() const
{ return _bridge ? _bridge->GetStats() : VtDictionary{}; }
bool PrismTaskDelegate::Pause() { _thread->PauseRender(); return true; }
bool PrismTaskDelegate::Resume() { _thread->ResumeRender(); return true; }
bool PrismTaskDelegate::Stop(bool) { _thread->StopRender(); return true; }
bool PrismTaskDelegate::Restart()
{ if (_bridge) _bridge->MarkSceneDirty(); return true; }

void PrismTaskDelegate::_SyncAuthoredSettings()
{
    if (!_authoredSettingsDirty || !_sourceSceneIndex || !_bridge) return;
    _authoredSettingsDirty = false;
    _renderSettingsPaths.clear();
    VtDictionary authored;
    SdfPathVector pending{SdfPath::AbsoluteRootPath()};
    bool foundActive = false;
    while (!pending.empty()) {
        const auto path = pending.back(); pending.pop_back();
        const auto prim = _sourceSceneIndex->GetPrim(path);
        if (prim.primType == HdPrimTypeTokens->renderSettings) {
            _renderSettingsPaths.push_back(path);
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
        const auto children = _sourceSceneIndex->GetChildPrimPaths(path);
        pending.insert(pending.end(), children.begin(), children.end());
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
    bool sceneChanged = false;
    for (const AddedPrimEntry &entry : entries) {
        if (entry.primType == HdPrimTypeTokens->renderSettings)
            _authoredSettingsDirty = true;
        if (_NeedsSceneRebuild(sender.GetPrim(entry.primPath)))
            sceneChanged = true;
    }
    if (sceneChanged && _bridge) _bridge->MarkSceneDirty();
}

void PrismTaskDelegate::PrimsRemoved(HdSceneIndexBase const &,
                                     RemovedPrimEntries const &entries)
{
    for (const auto& entry : entries)
        for (const auto& path : _renderSettingsPaths)
            if (path.HasPrefix(entry.primPath)) _authoredSettingsDirty = true;
    if (_bridge) _bridge->MarkSceneDirty();
}

void PrismTaskDelegate::PrimsDirtied(HdSceneIndexBase const &sender,
                                     DirtiedPrimEntries const &entries)
{
    bool sceneChanged = false;
    for (const DirtiedPrimEntry &entry : entries) {
        const auto prim = sender.GetPrim(entry.primPath);
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
        if (entry.primPath == SdfPath::AbsoluteRootPath() &&
            prim.primType.IsEmpty()) {
            bool currentFrameChanged = false;
            for (const HdDataSourceLocator &locator : entry.dirtyLocators) {
                if (locator.GetString() == "sceneGlobals/currentFrame") {
                    currentFrameChanged = true;
                    break;
                }
            }
            if (!currentFrameChanged) continue;
        }
        if (_NeedsSceneRebuild(prim)) sceneChanged = true;
    }
    if (sceneChanged && _bridge) _bridge->MarkSceneDirty();
}

void PrismTaskDelegate::PrimsRenamed(HdSceneIndexBase const &,
                                     RenamedPrimEntries const &)
{
    _authoredSettingsDirty = true;
    if (_bridge) _bridge->MarkSceneDirty();
}

PXR_NAMESPACE_CLOSE_SCOPE
