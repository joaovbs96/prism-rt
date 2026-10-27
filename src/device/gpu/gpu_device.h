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
    bool SettleTextures(SceneManager& scene, PassRequest const& request) override;

protected:
    // Texture streaming (Vulkan; the defaults stream nothing, as on Metal).
    // BeginWork precedes the dispatches of each pass, image or preview
    // (`mode` kMode*, `sample` its first sample index).
    virtual void BeginWork(uint32_t mode, int sample) { (void)mode; (void)sample; }
    virtual bool StreamsTextures() const { return false; }
    // Waits until the feedback recorded so far is processed and what it asks
    // for is resident; true when residency changed.
    virtual bool SettleResidency() { return false; }
    // Feedback from every pixel (settling) or the usual subset.
    virtual void FullFeedback(bool full) { (void)full; }
    // The next accumulation keeps residency unchanged.
    virtual void HoldResidency() {}

    // Makes the device's copy of the scene current; data() is valid after.
    virtual bool PrepareScene(SceneManager& scene, std::string* error) = 0;
    virtual GpuSceneData const& Data() const = 0;
    virtual uint32_t MdlTextureCount() const = 0;
    // Host-visible storage for kSlotRng / Output / Accumulation / Pixels
    // of at least `bytes`, bound for the next dispatch.
    virtual void* RenderBuffer(int slot, size_t bytes) = 0;
    // Builds (once per material set) the kernel variant that also writes the
    // light AOVs (PRISM_LIGHT_AOVS); Dispatch runs it for frames with
    // kFrameLightAovs. After PrepareScene.
    virtual bool PrepareLightAovs(std::string* error) = 0;
    // Runs the kernel over `threads` threads and waits for completion.
    virtual bool Dispatch(GpuFrame const& frame, uint32_t threads, double* gpuSeconds,
                          std::string* error) = 0;
    virtual const char* LogPrefix() const = 0;
    // Denoises a whole-image render where its accumulation lives
    // (kSlotAccumulation: the colour sums, then the light AOV sums; `samples`
    // per pixel) into *request.denoised, and names the denoiser in
    // *request.denoiser. False when the device has no denoiser of its own:
    // RenderImage then denoises the host images (DenoiseRenderedImage).
    virtual bool DenoiseAccumulation(ImageRequest const& request, std::vector<int> const& samples)
    {
        (void)request;
        (void)samples;
        return false;
    }

    std::mutex m_renderMutex; // one render at a time

private:
    GpuFrame BaseFrame(prism::CameraRayParameters const& camera, unsigned width,
                                   unsigned height) const;
    // Feedback passes of `frame` over every pixel, then SettleResidency, until
    // residency is stable (at most a few rounds); m_renderMutex held.
    void SettleWithFrame(GpuFrame const& frame, uint32_t mode, uint32_t pixels);
};

bool GpuVerbose();

} // namespace prism::device::gpu
