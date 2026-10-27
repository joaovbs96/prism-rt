#pragma once

// The Metal device's denoiser (docs/denoising.md): Open Image Denoise's own
// Metal device (OIDN 2.5.1, the same RT filter and weights as its CPU
// device), created on the render device's MTLCommandQueue, on MTLBuffers the
// denoiser holds and shares with OIDN (oidnNewSharedBufferFromMetal).
// Prefiltering of the guides is OIDN's (its albedo and normal filters), as
// on the CPU device.
//
// Objective-C++ (built only with the Metal device and OIDN:
// PRISM_HAS_METAL_OIDN).

#include "../denoiser.h"

#import <Metal/Metal.h>

#include <memory>
#include <string>
#include <vector>

namespace prism::device {

// PRISM_GPU_DENOISER=cpu: the GPU devices denoise with OIDN's CPU device on
// host copies.
bool MetalGpuDenoiserWanted();

class MetalDenoiser final : public Denoiser {
public:
    // Null with the reason when OIDN has no Metal device for `device` (an
    // OIDN built without OIDN_DEVICE_METAL, or an unsupported GPU).
    static std::shared_ptr<MetalDenoiser> Create(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                                 std::string* error);
    ~MetalDenoiser() override;

    // "OIDN 2.5.1 Metal".
    std::string Name() const override;
    // Host images: copied into the shared buffers, denoised, read back.
    void Denoise(DenoiseRequest const& request) override;
    // A whole-image render's accumulation (GpuRenderDevice::RenderImage's
    // kSlotAccumulation: the colour sums, then the light AOV sums), averaged
    // over each pixel's `samples` by a resolve kernel into the colour, albedo
    // and normal buffers; *output receives width*height RGB triples.
    void DenoiseAccumulation(id<MTLBuffer> accumulation, std::vector<int> const& samples, unsigned width,
                             unsigned height, DenoiseSettings const& settings, std::vector<float>* output);
    // The render device is going away: releases OIDN's device, the filters
    // and the buffers. Later calls of Denoise use OIDN's CPU device.
    void Shutdown();

    struct Impl;

private:
    explicit MetalDenoiser(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::device
