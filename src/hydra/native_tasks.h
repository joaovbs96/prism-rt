#pragma once
#include "selection_settings.h"
#include <pxr/imaging/hd/legacyTaskFactory.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <atomic>
#include <memory>

namespace prism::hydra {
class SceneBridge;

// Task factories are populated before HdRenderDelegate::SetTerminalSceneIndex
// runs.  Keep this indirection stable while the delegate installs its native
// scene bridge immediately afterwards.
struct TaskContext final {
    TaskContext() : selectionSettings(std::make_shared<SelectionSettings>()) {}
    std::shared_ptr<SelectionSettings> selectionSettings;
    std::atomic<SceneBridge *> bridge{nullptr};
};
}
PXR_NAMESPACE_OPEN_SCOPE
HdLegacyTaskFactorySharedPtr PrismWrapTaskFactory(
    HdLegacyTaskFactorySharedPtr const& original,
    HdSceneIndexBaseRefPtr const& scene,
    std::shared_ptr<prism::hydra::TaskContext> const& context);
PXR_NAMESPACE_CLOSE_SCOPE
