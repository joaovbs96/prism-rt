#pragma once

#include "device.h"

namespace prism::device {

// CPU rendering (Embree intersection, TBB parallelism, MDL native code).
// By default it runs the shared integrator (shaders/integrator) on a CPU
// backend; the reference variant runs the original SceneManager::trace and
// stays bit-identical to the renderer before the device abstraction.
// Stateless: every call renders the scene it is given.
class CpuDevice final : public RenderDevice {
public:
    explicit CpuDevice(bool reference = false) : m_reference(reference) {}
    DeviceInfo Info() const override;
    std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) override;
    void RenderPass(SceneManager& scene, PassRequest const& request) override;
    bool RenderPreview(SceneManager& scene, PreviewRequest const& request) override;
    RTCRayHit Intersect(SceneManager& scene, prism::PathRay const& ray) override;

private:
    bool m_reference;
};

} // namespace prism::device
