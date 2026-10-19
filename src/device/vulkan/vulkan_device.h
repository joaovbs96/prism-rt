#pragma once

#include "../cpu_device.h"
#include "../gpu/gpu_device.h"

#include <memory>
#include <string>

namespace prism::device {

// Vulkan render device: the shared integrator in HLSL, compiled to SPIR-V
// with DXC, tracing with VK_KHR_ray_query. Mirrors the Metal device;
// only resource creation and dispatch differ. Geometric outputs (primary
// AOVs, picking) use the CPU scene. A scene the device cannot prepare (its
// kernel fails to compile, its resources do not fit) renders on the CPU
// device until it changes, with the reason printed once.
class VulkanDevice final : public gpu::GpuRenderDevice {
public:
    // Null (with a reason) unless a device supports ray queries.
    static std::unique_ptr<VulkanDevice> Create(std::string* reason);
    ~VulkanDevice() override;

    DeviceInfo Info() const override;
    void SceneChanged(SceneManager& scene) override;
    std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) override;
    void RenderPass(SceneManager& scene, PassRequest const& request) override;
    bool RenderPreview(SceneManager& scene, PreviewRequest const& request) override;
    void Prepare(SceneManager& scene) override;

    struct Impl;

protected:
    bool PrepareScene(SceneManager& scene, std::string* error) override;
    gpu::GpuSceneData const& Data() const override;
    uint32_t MdlTextureCount() const override;
    void* RenderBuffer(int slot, size_t bytes) override;
    bool Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds,
                  std::string* error) override;
    const char* LogPrefix() const override { return "PrismRT Vulkan: "; }

private:
    explicit VulkanDevice(std::unique_ptr<Impl> impl);
    // Prepares the scene; on failure reports it (once per reason) and
    // returns true: the scene is then rendered by m_cpu.
    bool RendersOnCpu(SceneManager& scene);

    std::unique_ptr<Impl> m_impl;
    CpuDevice m_cpu;
    std::string m_cpuReason; // the failure last reported
};

// The Vulkan kernel for the given MDL code, as SPIR-V (exposed for the shader compile test).
bool CompileVulkanKernel(std::string const& mdlCode, std::vector<uint32_t>* spirv, std::string* error,
                         uint32_t programCount = 0);

} // namespace prism::device
