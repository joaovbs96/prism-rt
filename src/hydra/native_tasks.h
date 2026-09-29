#pragma once
#include "selection_settings.h"
#include "scene_bridge.h"
#include <pxr/imaging/hd/legacyTaskFactory.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <map>
#include <limits>
#include <memory>
#include <set>
#include <vector>

namespace prism::hydra {

// Task factories are populated before HdRenderDelegate::SetTerminalSceneIndex
// runs.  Keep this indirection stable while the delegate installs its native
// scene bridge immediately afterwards.
struct TaskContext final {
    TaskContext() : selectionSettings(std::make_shared<SelectionSettings>()) {}
    std::shared_ptr<SelectionSettings> selectionSettings;
    std::atomic<SceneBridge *> bridge{nullptr};
    std::atomic<uint64_t> selectionOffsetNanoseconds{0};
    std::atomic<uint64_t> selectionOffsetCalls{0};
    std::atomic<uint64_t> selectionPathsVisited{0};
    std::atomic<uint64_t> selectionOffsetCacheHits{0};
    std::atomic<uint64_t> selectionOffsetCacheMisses{0};
    std::atomic<uint64_t> pickNanoseconds{0};
    std::atomic<uint64_t> pickCalls{0};
    std::atomic<uint64_t> selectionSceneRevision{0};
    std::atomic<uint64_t> instanceTopologyRevision{0};
};

inline std::set<int> TranslatePickingInstanceSelection(
    std::vector<PickingTable::InstanceLevel> const &topology,
    std::map<pxr::SdfPath, std::set<int>> const &constraints,
    bool constraintsAreSourceIndices)
{
    if (topology.empty()) return {};
    for (auto const &[instancer, indices] : constraints)
        if (std::none_of(topology.begin(), topology.end(),
                [&](auto const &level) {
                    return level.instancerPath == instancer;
                }) && !constraintsAreSourceIndices) return {};
    std::vector<int> flattened{0};
    for (auto const &level : topology) {
        if (level.sourceIndicesByOrdinal.empty()) return {};
        std::vector<int> next;
        auto const constraint = constraints.find(level.instancerPath);
        for (int parent : flattened) {
            for (size_t ordinal = 0;
                 ordinal < level.sourceIndicesByOrdinal.size(); ++ordinal) {
                const int selectedIndex = constraintsAreSourceIndices
                    ? level.sourceIndicesByOrdinal[ordinal]
                    : static_cast<int>(ordinal);
                if (constraint != constraints.end() &&
                    !constraint->second.count(-1) &&
                    !constraint->second.count(selectedIndex)) continue;
                const uint64_t id = uint64_t(parent) *
                    level.sourceIndicesByOrdinal.size() + ordinal;
                if (id > uint64_t(std::numeric_limits<int>::max())) return {};
                next.push_back(static_cast<int>(id));
            }
        }
        flattened = std::move(next);
    }
    return {flattened.begin(), flattened.end()};
}
}
PXR_NAMESPACE_OPEN_SCOPE
HdLegacyTaskFactorySharedPtr PrismWrapTaskFactory(
    HdLegacyTaskFactorySharedPtr const& original,
    HdSceneIndexBaseRefPtr const& scene,
    std::shared_ptr<prism::hydra::TaskContext> const& context);
PXR_NAMESPACE_CLOSE_SCOPE
