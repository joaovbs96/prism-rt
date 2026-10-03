#pragma once

#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/sceneIndexObserver.h"
#include "pxr/base/vt/dictionary.h"

#include <memory>
#include <map>
#include <vector>

namespace prism::hydra {
class SceneBridge;
struct RenderSettings;
struct SelectionSettings;
struct TaskContext;
}

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderThread;
class PrismTransportFilter;

class PrismTaskDelegate final : public HdRenderDelegate,
                                public HdSceneIndexObserver
{
public:
    PrismTaskDelegate();
    ~PrismTaskDelegate() override;

    void SetTerminalSceneIndex(
        HdSceneIndexBaseRefPtr const &terminalSceneIndex) override;
    void Update() override;

    void PrimsAdded(HdSceneIndexBase const &, AddedPrimEntries const &) override;
    void PrimsRemoved(HdSceneIndexBase const &, RemovedPrimEntries const &) override;
    void PrimsDirtied(HdSceneIndexBase const &, DirtiedPrimEntries const &) override;
    void PrimsRenamed(HdSceneIndexBase const &, RenamedPrimEntries const &) override;

    const TfTokenVector &GetSupportedRprimTypes() const override;
    const TfTokenVector &GetSupportedSprimTypes() const override;
    const TfTokenVector &GetSupportedBprimTypes() const override;
    HdResourceRegistrySharedPtr GetResourceRegistry() const override;
    HdRenderPassSharedPtr CreateRenderPass(
        HdRenderIndex *index, HdRprimCollection const &collection) override;
    HdInstancer *CreateInstancer(HdSceneDelegate *, SdfPath const &) override;
    void DestroyInstancer(HdInstancer *) override;
    HdRprim *CreateRprim(TfToken const &, SdfPath const &) override;
    void DestroyRprim(HdRprim *) override;
    HdSprim *CreateSprim(TfToken const &, SdfPath const &) override;
    HdSprim *CreateFallbackSprim(TfToken const &) override;
    void DestroySprim(HdSprim *) override;
    HdBprim *CreateBprim(TfToken const &, SdfPath const &) override;
    HdBprim *CreateFallbackBprim(TfToken const &) override;
    void DestroyBprim(HdBprim *) override;
    void CommitResources(HdChangeTracker *) override;

    HdAovDescriptor GetDefaultAovDescriptor(TfToken const &name) const override;
    TfToken GetMaterialBindingPurpose() const override;
    TfTokenVector GetMaterialRenderContexts() const override;
    TfTokenVector GetRenderSettingsNamespaces() const override;
    bool RequiresStormTasks() const override { return false; }

    HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;
    VtValue GetRenderSetting(TfToken const &key) const override;
    void SetRenderSetting(TfToken const &key, VtValue const &value) override;
    VtDictionary GetRenderStats() const override;

    bool IsPauseSupported() const override { return true; }
    bool Pause() override;
    bool Resume() override;
    bool IsStopSupported() const override { return true; }
    bool Stop(bool blocking = true) override;
    bool Restart() override;

private:
    void _SyncAuthoredSettings();
    void _RefreshRenderSettingsPaths();
    std::vector<size_t> _GetSceneIndexOrder(SdfPath const &path) const;

    HdSceneIndexBaseRefPtr _sourceSceneIndex;
    std::unique_ptr<HdRenderThread> _renderThread;
    std::unique_ptr<prism::hydra::SceneBridge> _ownedBridge;
    prism::hydra::SceneBridge *_bridge = nullptr;
    HdRenderThread *_thread = nullptr;
    std::shared_ptr<prism::hydra::TaskContext> _taskContext;
    std::shared_ptr<prism::hydra::SelectionSettings> _selectionSettings;
    PrismTransportFilter *_transport = nullptr;
    HdResourceRegistrySharedPtr _resources;
    bool _authoredSettingsDirty = true;
    SdfPathVector _renderSettingsPaths;
    std::map<SdfPath, std::vector<size_t>> _renderSettingsOrder;
    VtDictionary _authoredSettings;
    std::map<SdfPath, TfToken> _knownPrimTypes;
};

PXR_NAMESPACE_CLOSE_SCOPE
