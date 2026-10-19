#pragma once

// Render-loop logic shared by the GPU devices (Metal, Vulkan): request
// handling, tiling into dispatches, RNG and output transfer, and the CPU
// geometric outputs. A device implements scene preparation, host-visible
// per-render buffers and kernel dispatch on its graphics API.

#include "../device.h"
#include "gpu_scene.h"

#include <mutex>
#include <string>

namespace prism::device::gpu {

class GpuRenderDevice : public RenderDevice {
public:
    std::vector<float> RenderImage(SceneManager& scene, ImageRequest const& request) override;
    void RenderPass(SceneManager& scene, PassRequest const& request) override;
    bool RenderPreview(SceneManager& scene, PreviewRequest const& request) override;
    // Picking uses the CPU scene, which is exact and needs no GPU round trip.
    RTCRayHit Intersect(SceneManager& scene, prism::PathRay const& ray) override;
    void Prepare(SceneManager& scene) override;

protected:
    // Makes the device's copy of the scene current; data() is valid after.
    virtual bool PrepareScene(SceneManager& scene, std::string* error) = 0;
    virtual GpuSceneData const& Data() const = 0;
    virtual uint32_t MdlTextureCount() const = 0;
    // Host-visible storage for kSlotRng / Output / Accumulation / Pixels
    // of at least `bytes`, bound for the next dispatch.
    virtual void* RenderBuffer(int slot, size_t bytes) = 0;
    // Runs the kernel over `threads` threads and waits for completion.
    virtual bool Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds,
                          std::string* error) = 0;
    virtual const char* LogPrefix() const = 0;

    std::mutex m_renderMutex; // one render at a time

private:
    GpuFrame BaseFrame(prism::CameraRayParameters const& camera, unsigned width,
                                   unsigned height) const;
};

bool GpuVerbose();

} // namespace prism::device::gpu
