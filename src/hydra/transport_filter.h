#pragma once
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <memory>
#include <cstdint>
namespace prism::hydra { struct TaskContext; }
PXR_NAMESPACE_OPEN_SCOPE
// The scene index filters hdPrism's converters rely on: implicit surfaces
// (cube, cone, cylinder, capsule, plane) as meshes, CPU-computed primvars as
// values, light/shadow-link collections as categories, and render settings
// prims with their `active` flag and only prism:* settings. The plugin's
// _CreateRenderer applies them before the transport filter; a host that gives
// the delegate its own render index (HdRenderIndex::New) gets them in
// SetTerminalSceneIndex. Applying them to a scene that has them already
// changes nothing.
HdSceneIndexBaseRefPtr PrismAppendSceneFilters(HdSceneIndexBaseRefPtr const& input);
// Only presentation/control data enters the private legacy task index.
class PrismTransportFilter final : public HdSingleInputFilteringSceneIndexBase {
public:
    static TfRefPtr<PrismTransportFilter> New(HdSceneIndexBaseRefPtr const& input,
        std::shared_ptr<prism::hydra::TaskContext> const& context);
    HdSceneIndexBaseRefPtr GetSourceSceneIndex() const;
    std::shared_ptr<prism::hydra::TaskContext> GetTaskContext() const;
    HdSceneIndexPrim GetPrim(SdfPath const& path) const override;
    SdfPathVector GetChildPrimPaths(SdfPath const& path) const override;
protected:
    void _PrimsAdded(HdSceneIndexBase const&, HdSceneIndexObserver::AddedPrimEntries const&) override;
    void _PrimsRemoved(HdSceneIndexBase const&, HdSceneIndexObserver::RemovedPrimEntries const&) override;
    void _PrimsDirtied(HdSceneIndexBase const&, HdSceneIndexObserver::DirtiedPrimEntries const&) override;
    void _PrimsRenamed(HdSceneIndexBase const&, HdSceneIndexObserver::RenamedPrimEntries const&) override;
private:
    PrismTransportFilter(HdSceneIndexBaseRefPtr const& input,
        std::shared_ptr<prism::hydra::TaskContext> const& context);
    std::shared_ptr<prism::hydra::TaskContext> _context;
};
PXR_NAMESPACE_CLOSE_SCOPE
