#pragma once

#include "../gpu/gpu_device.h"

#include <memory>
#include <string>

namespace prism::device {

// Vulkan render device: the shared integrator in GLSL, compiled to SPIR-V
// with shaderc, tracing with VK_KHR_ray_query. Mirrors the Metal device;
// only resource creation and dispatch differ. Geometric outputs (primary
// AOVs, picking) use the CPU scene.
class VulkanDevice final : public gpu::GpuRenderDevice {
public:
    // Null (with a reason) unless a device supports ray queries.
    static std::unique_ptr<VulkanDevice> Create(std::string* reason);
    ~VulkanDevice() override;

    DeviceInfo Info() const override;
    void SceneChanged(SceneManager& scene) override;

    struct Impl;

protected:
    bool PrepareScene(SceneManager& scene, std::string* error) override;
    gpu::GpuSceneData const& Data() const override;
    uint32_t MdlTextureCount() const override;
    void* RenderBuffer(int slot, size_t bytes) override;
    bool Dispatch(prism::gpu::GpuFrame const& frame, uint32_t threads, double* gpuSeconds,
                  std::string* error) override;
    const char* LogPrefix() const override { return "PrismRT Vulkan: "; }

private:
    explicit VulkanDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// GLSL -> SPIR-V for the Vulkan kernel (exposed for the shader compile test).
bool CompileVulkanKernel(std::string const& mdlCode, std::vector<uint32_t>* spirv, std::string* error);

} // namespace prism::device
