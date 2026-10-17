#pragma once

#include "../device.h"

#include <memory>
#include <string>

namespace prism::device {

// Metal render device: the shared integrator in HLSL, compiled with DXC and
// Metal shader converter, tracing with hardware ray tracing (inline ray
// queries). Geometric outputs (primary AOVs, picking) use the CPU scene.
class MetalDevice final : public RenderDevice {
public:
    // Null (with a reason) if no Metal GPU with ray tracing is available.
    static std::unique_ptr<MetalDevice> Create(std::string* reason);
    ~MetalDevice() override;

    DeviceInfo Info() const override;
    void SceneChanged(SceneManager& scene) override;
    std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) override;
    void RenderPass(SceneManager& scene, PassRequest const& request) override;
    bool RenderPreview(SceneManager& scene, PreviewRequest const& request) override;
    RTCRayHit Intersect(SceneManager& scene, prism::PathRay const& ray) override;

    struct Impl;

private:
    explicit MetalDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::device
