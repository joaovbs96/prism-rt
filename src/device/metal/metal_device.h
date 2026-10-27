#pragma once

#include "../gpu/gpu_device.h"

#include <memory>
#include <string>

namespace prism::device {

// Metal render device: the shared integrator in HLSL, compiled with DXC and
// Metal shader converter, tracing with hardware ray tracing (inline ray
// queries). Geometric outputs (primary AOVs, picking) use the CPU scene.
class MetalDevice final : public gpu::GpuRenderDevice {
public:
    // Null (with a reason) if no Metal GPU with ray tracing is available.
    static std::unique_ptr<MetalDevice> Create(std::string* reason);
    ~MetalDevice() override;

    DeviceInfo Info() const override;
    void SceneChanged(SceneManager& scene) override;
    // OIDN's Metal device on this device's queue when OIDN has one
    // (PRISM_HAS_METAL_OIDN, docs/denoising.md), else OIDN's CPU device on
    // host copies.
    std::shared_ptr<Denoiser> CreateDenoiser(std::string* error = nullptr) override;

    struct Impl;

protected:
    bool PrepareScene(SceneManager& scene, std::string* error) override;
    gpu::GpuSceneData const& Data() const override;
    uint32_t MdlTextureCount() const override;
    void* RenderBuffer(int slot, size_t bytes) override;
    bool PrepareLightAovs(std::string* error) override;
    bool Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds,
                  std::string* error) override;
    const char* LogPrefix() const override { return "PrismRT Metal: "; }
    bool DenoiseAccumulation(ImageRequest const& request, std::vector<int> const& samples) override;

private:
    explicit MetalDevice(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::device
