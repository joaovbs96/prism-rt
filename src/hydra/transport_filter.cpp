#include "transport_filter.h"
#include "native_tasks.h"
#include "selection_settings.h"
#include <pxr/imaging/hd/legacyTaskSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/selectionsSchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/colorizeSelectionTask.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/imaging/hdsi/lightLinkingSceneIndex.h>
#include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#include <pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h>
PXR_NAMESPACE_OPEN_SCOPE
HdSceneIndexBaseRefPtr PrismAppendSceneFilters(HdSceneIndexBaseRefPtr const& input)
{
    const auto toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(
        HdsiImplicitSurfaceSceneIndexTokens->toMesh);
    const auto conversion = HdRetainedContainerDataSource::New(
        HdPrimTypeTokens->cube, toMesh,
        HdPrimTypeTokens->cone, toMesh,
        HdPrimTypeTokens->cylinder, toMesh,
        HdPrimTypeTokens->capsule, toMesh,
        HdPrimTypeTokens->plane, toMesh);
    auto converted = HdsiImplicitSurfaceSceneIndex::New(input, conversion);
    // Pull CPU-computed primvars as ordinary values before owner-thread
    // snapshot capture severs their live data-source handles.
    auto computed = HdSiExtComputationPrimvarPruningSceneIndex::New(converted);
    // Light/shadow-link collections become categories on lights and geometry.
    auto linked = HdsiLightLinkingSceneIndex::New(computed, nullptr);
    // Render settings prims get their `active` flag (from the scene globals'
    // active render settings path) and only prism:* namespaced settings
    // (docs/hydra.md, "Render settings and products").
    VtArray<TfToken> prefixes{TfToken("prism:")};
    return HdsiRenderSettingsFilteringSceneIndex::New(linked,
        HdRetainedContainerDataSource::New(
            HdsiRenderSettingsFilteringSceneIndexTokens->namespacePrefixes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(prefixes)));
}

namespace {
bool IsTransport(HdSceneIndexPrim const& p) {
    return p.primType == HdPrimTypeTokens->camera ||
           p.primType == HdPrimTypeTokens->renderBuffer ||
           bool(HdLegacyTaskSchema::GetFromParent(p.dataSource));
}

class DynamicSelectionParams final
    : public HdTypedSampledDataSource<HdxColorizeSelectionTaskParams> {
public:
    HD_DECLARE_DATASOURCE(DynamicSelectionParams);

    VtValue GetValue(Time shutterOffset) override {
        return VtValue(GetTypedValue(shutterOffset));
    }
    HdxColorizeSelectionTaskParams GetTypedValue(Time shutterOffset) override {
        HdxColorizeSelectionTaskParams result;
        if (_original) {
            const VtValue value = _original->GetValue(shutterOffset);
            if (value.IsHolding<HdxColorizeSelectionTaskParams>())
                result = value.UncheckedGet<HdxColorizeSelectionTaskParams>();
        }
        if (_settings->overrideTaskSettings.load()) {
            result.enableOutline = _settings->enableOutline.load();
            result.outlineRadius = static_cast<unsigned>(
                _settings->outlineRadius.load());
        }
        return result;
    }
    bool GetContributingSampleTimesForInterval(
        Time start, Time end, std::vector<Time>* times) override {
        return _original && _original->GetContributingSampleTimesForInterval(
            start, end, times);
    }

private:
    DynamicSelectionParams(
        HdSampledDataSourceHandle original,
        std::shared_ptr<prism::hydra::SelectionSettings> settings)
        : _original(std::move(original)), _settings(std::move(settings)) {}

    HdSampledDataSourceHandle _original;
    std::shared_ptr<prism::hydra::SelectionSettings> _settings;
};
}
PrismTransportFilter::PrismTransportFilter(HdSceneIndexBaseRefPtr const& input,
    std::shared_ptr<prism::hydra::TaskContext> const& context)
    : HdSingleInputFilteringSceneIndexBase(input), _context(context) {}
TfRefPtr<PrismTransportFilter> PrismTransportFilter::New(HdSceneIndexBaseRefPtr const& input,
    std::shared_ptr<prism::hydra::TaskContext> const& context) {
    return TfCreateRefPtr(new PrismTransportFilter(input, context));
}
HdSceneIndexBaseRefPtr PrismTransportFilter::GetSourceSceneIndex() const {
    return _GetInputSceneIndex();
}
std::shared_ptr<prism::hydra::TaskContext> PrismTransportFilter::GetTaskContext() const {
    return _context;
}
HdSceneIndexPrim PrismTransportFilter::GetPrim(SdfPath const& path) const {
    auto p = _GetInputSceneIndex()->GetPrim(path);
    if (!IsTransport(p)) {
        // Keep selection data visible to the selection tracker without
        // admitting a geometry prim type into this task-only render index.
        auto selections = HdSelectionsSchema::GetFromParent(p.dataSource);
        if (selections) {
            return {TfToken(), HdRetainedContainerDataSource::New(
                HdSelectionsSchema::GetSchemaToken(), selections.GetVector())};
        }
        return {};
    }
    auto task = HdLegacyTaskSchema::GetFromParent(p.dataSource);
    if (task && task.GetFactory()) {
        auto original = task.GetFactory()->GetTypedValue(0);
        if (original) {
            auto builder = HdLegacyTaskSchema::Builder().SetFactory(
                HdRetainedTypedSampledDataSource<HdLegacyTaskFactorySharedPtr>::New(
                    PrismWrapTaskFactory(original, _GetInputSceneIndex(), _context)));
            const auto& settings = _context->selectionSettings;
            if (task.GetParameters()) {
                auto value = task.GetParameters()->GetValue(0);
                if (value.IsHolding<HdxColorizeSelectionTaskParams>()) {
                    builder.SetParameters(DynamicSelectionParams::New(
                        task.GetParameters(), settings));
                }
            }
            auto overrideTask = builder.Build();
            p.dataSource = HdOverlayContainerDataSource::New(
                HdRetainedContainerDataSource::New(HdLegacyTaskSchema::GetSchemaToken(), overrideTask),
                p.dataSource);
        }
    }
    return p;
}
SdfPathVector PrismTransportFilter::GetChildPrimPaths(SdfPath const& path) const {
    return _GetInputSceneIndex()->GetChildPrimPaths(path);
}
void PrismTransportFilter::_PrimsAdded(HdSceneIndexBase const&, HdSceneIndexObserver::AddedPrimEntries const& entries) {
    HdSceneIndexObserver::AddedPrimEntries result;
    result.reserve(entries.size());
    for (auto const& e : entries) result.push_back({e.primPath, GetPrim(e.primPath).primType});
    _SendPrimsAdded(result);
}
void PrismTransportFilter::_PrimsRemoved(HdSceneIndexBase const&, HdSceneIndexObserver::RemovedPrimEntries const& entries) {
    _SendPrimsRemoved(entries);
}
void PrismTransportFilter::_PrimsDirtied(HdSceneIndexBase const&, HdSceneIndexObserver::DirtiedPrimEntries const& entries) {
    HdSceneIndexObserver::DirtiedPrimEntries result;
    for (auto const& e : entries) {
        if (IsTransport(_GetInputSceneIndex()->GetPrim(e.primPath)) ||
            e.dirtyLocators.Intersects(HdSelectionsSchema::GetDefaultLocator()))
            result.push_back(e);
    }
    if (!result.empty()) _SendPrimsDirtied(result);
}
void PrismTransportFilter::_PrimsRenamed(HdSceneIndexBase const&, HdSceneIndexObserver::RenamedPrimEntries const& entries) {
    _SendPrimsRenamed(entries);
}
PXR_NAMESPACE_CLOSE_SCOPE
