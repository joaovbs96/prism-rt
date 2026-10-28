#pragma once

// Denoising (docs/denoising.md): Intel Open Image Denoise's RT filter on the
// path-traced colour, guided by the denoising albedo and normal that the path
// tracer gathers with it (LightAovPixel::albedo, ::normal).
//
// A render device hands out its denoiser (RenderDevice::CreateDenoiser) and
// can denoise its own whole-image render (ImageRequest::denoise), so callers
// never pick a backend:
//   CPU device     OIDN's CPU device on host images (this file);
//   Vulkan         vk_oidn, OIDN's network as Vulkan compute, on the device's
//                  queue and buffers the denoiser holds
//                  (vulkan/vulkan_denoiser.h); the CPU denoiser on host copies
//                  when vk_oidn cannot run on the GPU;
//   Metal          OIDN's Metal device on the device's queue and MTLBuffers
//                  the denoiser shares with it (metal/metal_denoiser.h); the
//                  CPU denoiser on host copies when OIDN has no Metal device.
// Every backend runs the same network weights (OIDN 2.5.1's).

#include "../../include/prism/settings.h"

#include <memory>
#include <string>
#include <string_view>

namespace prism::device {

// The quality modes and settings are public (include/prism/settings.h).
using prism::DenoiseQuality;
using prism::DenoiseSettings;

const char* ToString(DenoiseQuality quality);
bool ParseDenoiseQuality(std::string_view text, DenoiseQuality* quality);

// The images of one denoise on the host: width x height pixels, top row
// first, `stride` floats per pixel of which the first three are used (RGB,
// or the normal's XYZ). The albedo and normal may be null (not used).
struct HostImage {
    const float* pixels = nullptr;
    unsigned stride = 3;
};

struct DenoiseRequest {
    unsigned width = 0, height = 0;
    HostImage color, albedo, normal;
    // May be the colour's storage (in place). Only the first three floats of
    // each pixel are written.
    float* output = nullptr;
    unsigned outputStride = 3;
    DenoiseSettings settings;
};

class Denoiser {
public:
    virtual ~Denoiser() = default;
    // The backend, e.g. "OIDN 2.5.1 CPU".
    virtual std::string Name() const = 0;
    // Denoises host images. Calls may come from any thread and are
    // serialized. Throws std::runtime_error when the backend fails.
    virtual void Denoise(DenoiseRequest const& request) = 0;
};

// True when this build has a denoiser (PRISM_ENABLE_OIDN).
bool DenoisingAvailable();

// OIDN's CPU device, created once per process and shared (its threads come
// from the process's oneTBB). Null with the reason in *error when the build
// has no OIDN or the device cannot be created.
std::shared_ptr<Denoiser> SharedCpuDenoiser(std::string* error = nullptr);

} // namespace prism::device
