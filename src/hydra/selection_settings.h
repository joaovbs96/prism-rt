#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>

#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>

namespace prism::hydra {

// Presentation settings are shared with Hydra tasks and never invalidate
// path-tracing accumulation.
struct SelectionSettings {
    std::atomic<bool> enableOutline{true};
    std::atomic<int> outlineRadius{2};
    std::atomic<bool> overrideTaskSettings{true};
    std::atomic<std::uint64_t> revision{0};

    // usdview 26.05 drops the authored point-instance index while converting
    // its selection model to HdSelectionsSchema.  The public usdview plugin
    // transports those source indices through a renderer setting instead.
    mutable std::mutex usdviewMutex;
    bool usdviewSelectionActive = false;
    std::map<pxr::SdfPath, std::set<int>> usdviewSelection;
    pxr::VtStringArray usdviewSelectionPayload;
};

} // namespace prism::hydra
