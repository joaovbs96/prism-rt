#pragma once

// Small value types shared by render settings and the renderer's internals
// (docs/sdk_design.md, section 3.7): devices, texture filtering, light
// sampling and denoising. Standard library types only.

#include <string>

namespace prism {

enum class DeviceKind { Cpu, Vulkan, Metal };

// Auto prefers the platform's GPU device and falls back to the CPU device.
enum class DevicePreference { Auto, Cpu, Vulkan, Metal };

struct DeviceInfo {
    DeviceKind kind = DeviceKind::Cpu;
    std::string name;
    bool hardwareRayTracing = false;
};

// How a camera's aperture is fitted to an image of another aspect ratio
// (UsdRenderSettings aspectRatioConformPolicy, named after its tokens).
enum class ConformPolicy {
    ExpandAperture,          // fit: the aperture grows to the image's aspect
    CropAperture,            // fill: the aperture shrinks
    AdjustApertureWidth,     // keep the vertical aperture
    AdjustApertureHeight,    // keep the horizontal aperture
    AdjustPixelAspectRatio,  // no change (the pixels stretch)
};

// Texture lookups: filtered by the ray footprint, or bilinear at level 0.
enum class TextureFiltering { Filtered, Bilinear };

// Probabilistic next-event estimation: from fromDepth on, lights are sampled
// with probability clamp(path weight, probabilityMin, 1), which saves NEE
// work on dim deep paths at a noise cost. A probabilityMin of 0 (the default)
// always samples them: skipping NEE raised the MSE of CornellBox by 40% for 1%
// of time, and was about neutral on dome-lit and subsurface scenes.
struct NeeSettings {
    float probabilityMin = 0.0f;
    int fromDepth = 2;
    // Spatial light selection among analytic lights (by their estimated
    // irradiance at the shading point, mixed with power); false: by power.
    bool spatialLights = true;
};

// OIDN's quality modes: fast and balanced for interactive previews, high for
// final frames.
enum class DenoiseQuality { Fast, Balanced, High };

struct DenoiseSettings {
    DenoiseQuality quality = DenoiseQuality::High;
    // The colour is high dynamic range linear radiance (path-traced colour
    // always is; false: values in [0, 1]).
    bool hdr = true;
    // Denoise the albedo and normal first and then treat them as noise-free
    // (OIDN's prefiltering and cleanAux): sharper textures for final frames,
    // at about three times the cost. Off: they are passed as they are, with
    // cleanAux off (OIDN's choice for noisy guides and previews).
    bool prefilter = true;

    bool operator==(DenoiseSettings const&) const = default;
};

} // namespace prism
