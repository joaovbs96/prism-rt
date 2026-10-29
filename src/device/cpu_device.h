#pragma once

#include "../internal_export.h"
#include "device.h"

#include <memory>
#include <mutex>

namespace prism::device {

struct LightTable;

// Per-scene state of the CPU device, rebuilt when the scene changes.
struct CpuSceneState {
    // Every material of the scene always blocks shadow rays.
    bool opaque = false;
    // Back faces of single-sided geometry are culled (SceneManager::cullsBackFaces).
    bool culling = false;
    std::shared_ptr<const LightTable> lights;
};

// CPU rendering (Embree intersection, TBB parallelism, MDL native code): the
// shared integrator (shaders/integrator) on a CPU backend. Stateless: every
// call renders the scene it is given.
class PRISM_INTERNAL_API CpuDevice final : public RenderDevice {
public:
    DeviceInfo Info() const override;
    std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) override;
    void RenderPass(SceneManager& scene, PassRequest const& request) override;
    bool RenderPreview(SceneManager& scene, PreviewRequest const& request) override;
    RTCRayHit Intersect(SceneManager& scene, prism::PathRay const& ray) override;
    void SceneChanged(SceneManager& scene) override;
    void Prepare(SceneManager& scene) override { (void)PreparedScene(scene); }

private:
    // The scene's CpuSceneState, cached until it changes.
    std::shared_ptr<const CpuSceneState> PreparedScene(SceneManager& scene);

    std::mutex m_stateMutex;
    SceneManager const* m_stateOf = nullptr;
    std::shared_ptr<const CpuSceneState> m_state;
};

} // namespace prism::device
