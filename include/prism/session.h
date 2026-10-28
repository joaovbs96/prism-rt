#pragma once

// Render sessions (docs/sdk_design.md, section 3.7): a device, render
// settings and a camera that render committed revisions of one scene into
// images (the colour and the requested AOVs). This version has the batch
// path: render() renders the whole image to completion (samples, adaptive
// sampling, time limit), as the CLI does; progressive sessions (passes on an
// internal thread, previews, picking) come later.
//
// Errors are exceptions (prism::Error): InvalidArgument for misuse (no
// committed scene, no camera, invalid settings), Unsupported for a device or
// denoiser the build or machine lacks (with allowFallback false), Cancelled
// when the progress callback or cancel() stopped a render.

#include "scene.h"
#include "settings.h"
#include "types.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

class Renderer;

// The AOVs a session renders (src/output/aov.h). Their names are the Hydra
// AOV tokens (AovName).
enum class Aov {
    Color,        // "color": linear RGB radiance in the working space (3 floats)
    Depth,        // "depth": NDC depth in [0, 1], 1 on a miss (1 float)
    CameraDepth,  // "cameraDepth": distance along -Z in eye space, +inf on a miss
    Normal,       // "normal": world-space shading normal at the pixel centre (3 floats)
    Neye,         // "Neye": eye-space normal
    Peye,         // "Peye": eye-space position (0 on a miss)
    Pworld,       // "Pworld": world-space position (0 on a miss)
    St,           // "primvars:st": the hit's barycentric st (2 floats)
    PrimId,       // "primId": InstanceDesc::primId, -1 on a miss (1 Int32)
    ElementId,    // "elementId": the mesh's face, -1 on a miss (1 Int32)
    InstanceId,   // "instanceId": InstanceDesc::instanceId, -1 on a miss (1 Int32)
    Albedo,       // "albedo": the first non-delta vertex's albedo, over the colour's samples (3 floats)
    Diffuse,      // "diffuse": the diffuse share of the colour
    Specular,     // "specular": the specular share of the colour
    Emission,     // "emission": emission seen directly
    Denoised,     // "denoised": the colour, denoised (3 floats)
};

// The Hydra AOV token of an AOV ("color", "primvars:st", "primId", ...).
std::string_view AovName(Aov aov);
// The AOV of a Hydra AOV token ("P" is Pworld); nullopt for an unknown name.
std::optional<Aov> AovFromName(std::string_view name);

// Shutter interval in time codes around the frame time; reserved (section 5):
// accepted, one instant renders until motion blur exists.
struct Shutter {
    double open = 0.0, close = 0.0;
};

struct RenderSettings {
    // Samples per pixel; with adaptive sampling the per-pixel maximum.
    int samples = 2048;
    // Adaptive sampling: a pixel stops once its error estimate is below the
    // threshold (0 disables: every pixel gets `samples`), checked from
    // adaptiveMinSamples on.
    float adaptiveThreshold = 0.01f;
    int adaptiveMinSamples = 64;
    // Wall-clock limit of the sampling in seconds, without scene and device
    // preparation (0: none). Every pixel gets at least one sample.
    double timeLimitSeconds = 0.0;
    int maxDepth = 32, rouletteDepth = 4;
    uint32_t seed = 0;
    // Samples whose brightest channel exceeds this are scaled down (0: off).
    float sampleClamp = 0.0f;
    // Exposure in stops, on top of the camera's exposureScale; scales the
    // colour, the denoised colour and the radiance AOVs (not the albedo).
    float exposure = 0.0f;
    // Environment lighting: multiple importance sampling (true) or BSDF
    // sampling only.
    bool environmentMis = true;
    TextureFiltering textureFiltering = TextureFiltering::Filtered;
    // USD doubleSided: back faces of single-sided meshes are not hit, unless
    // their material transmits.
    bool honorDoubleSided = false;
    NeeSettings nee;
    // GPU devices that stream textures load the mip levels shading asks for;
    // false uploads whole textures.
    bool textureStreaming = true;
    // GPU texture budget in MiB (0: 45% of device memory).
    int gpuTextureBudgetMiB = 0;
    // Noise analysis: the light contributions kept (1 visible, 2 direct,
    // 4 indirect; 7 all).
    unsigned debugContributions = 7;
    Shutter shutter;  // reserved (section 5)
};

// A rectangle of pixels, top row first.
struct Window {
    unsigned x = 0, y = 0, width = 0, height = 0;
    bool operator==(Window const&) const = default;
};

struct SessionOptions {
    DevicePreference device = DevicePreference::Auto;
    // Without the preferred device: the CPU device (with a note, deviceNote())
    // or, when false, Error(Unsupported).
    bool allowFallback = true;
    // The display window; dataWindow (inside it) is the part rendered (a
    // crop); empty: all of it.
    unsigned width = 1280, height = 720;
    std::optional<Window> dataWindow;
    float pixelAspectRatio = 1.0f;
    ConformPolicy conformPolicy = ConformPolicy::ExpandAperture;
    // The camera: a camera of the scene, or a description (camera, if set,
    // wins). Its exposureScale scales the radiance outputs.
    CameraId camera;
    std::optional<CameraDesc> cameraDesc;
    std::vector<Aov> aovs{Aov::Color};
    RenderSettings settings;
    // Denoising (docs/denoising.md): with these settings the colour is
    // denoised (the Denoised AOV); asking for the Denoised AOV denoises with
    // the default settings. denoisedColor: the Color AOV is the denoised
    // colour (requires denoising).
    std::optional<DenoiseSettings> denoise;
    bool denoisedColor = false;
};

// Host pixels with shared ownership (copies share them): width x height
// pixels of `channels` scalars, row-major, top row first.
class Image {
public:
    Image() = default;
    // Zero-filled pixels.
    Image(unsigned width, unsigned height, unsigned channels, DataType type = DataType::Float32);

    unsigned width() const noexcept { return m_width; }
    unsigned height() const noexcept { return m_height; }
    unsigned channels() const noexcept { return m_channels; }
    DataType type() const noexcept { return m_type; }  // Float32, or Int32 for id AOVs
    bool empty() const noexcept { return !m_data; }
    size_t pixelCount() const noexcept { return size_t(m_width) * m_height; }
    size_t valueCount() const noexcept { return pixelCount() * m_channels; }

    const void* data() const noexcept { return m_data.get(); }
    void* data() noexcept { return m_data.get(); }
    // Typed access; throws Error(InvalidArgument) for the other type.
    float const* floats() const;
    float* floats();
    int32_t const* ints() const;
    // Keeps the pixels alive (zero-copy wrappers).
    std::shared_ptr<const void> owner() const { return m_data; }

private:
    unsigned m_width = 0, m_height = 0, m_channels = 0;
    DataType m_type = DataType::Float32;
    std::shared_ptr<void> m_data;
};

struct RenderResult {
    // The requested AOVs, dataWindow-sized.
    std::map<Aov, Image> aovs;
    // The display window and the part of it these pixels are.
    unsigned width = 0, height = 0;
    Window dataWindow;
    // Whether this render path traced (colour, light AOVs or denoising were
    // asked for); the statistics below are of that render.
    bool traced = false;
    double meanSamples = 0.0, fractionAtMaxSamples = 0.0;
    bool timeLimitReached = false;
    double renderSeconds = 0.0;      // tracing and geometric AOVs
    double denoiseMilliseconds = 0.0;
    std::string denoiser;            // the denoiser's name, when one ran

    bool has(Aov aov) const { return aovs.count(aov) != 0; }
    // Throws Error(NotFound) when the AOV was not rendered.
    Image const& aov(Aov aov) const;
};

class Session {
public:
    // Sessions come from Renderer::createSession. Copies share the session.
    Session(Session const&) = default;
    Session& operator=(Session const&) = default;
    Session(Session&&) noexcept = default;
    Session& operator=(Session&&) noexcept = default;
    ~Session();

    // The device it renders on, and why it is not the preferred one (empty
    // when it is).
    DeviceInfo device() const;
    std::string deviceNote() const;

    SessionOptions const& options() const;
    // Changes take effect at the next render.
    void setSettings(RenderSettings const& settings);
    void setCamera(CameraId camera);
    void setCamera(CameraDesc const& camera);
    void resize(unsigned width, unsigned height, std::optional<Window> dataWindow = {},
                float pixelAspectRatio = 1.0f, ConformPolicy policy = ConformPolicy::ExpandAperture);
    void setAovs(std::vector<Aov> const& aovs);
    void setDenoise(std::optional<DenoiseSettings> const& denoise, bool denoisedColor = false);

    // Device setup for the scene's committed revision (kernel compilation,
    // upload, light tables). Optional: render() does it.
    void prepare();

    // Renders the scene's last committed revision to completion. Blocking;
    // the callback runs on an internal thread. Throws Error(Cancelled) when
    // the callback returned false or cancel() was called.
    RenderResult render(ProgressCallback const& progress = {});

    // Stops the running render (from any thread) at its next sample
    // boundary (a CPU render also between rows); render() then throws
    // Error(Cancelled). Without a running render it does nothing.
    void cancel();

    struct Impl;

private:
    friend class Renderer;
    explicit Session(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> m_impl;
};

} // namespace prism
