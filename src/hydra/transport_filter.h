#pragma once
#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <memory>
#include <cstdint>
namespace prism::hydra { struct TaskContext; }
PXR_NAMESPACE_OPEN_SCOPE
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
