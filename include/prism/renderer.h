#pragma once

// The public renderer context (docs/sdk_design.md, section 3.4): colour
// management, the material system, textures, the Embree device, logging and
// progress of one renderer. Several renderers per process are allowed; the
// MDL SDK and content-addressed caches are shared by the process.

#include "options.h"
#include "scene.h"
#include "session.h"
#include "settings.h"

#include <memory>
#include <string>
#include <vector>

namespace prism {

class RendererContext;

// The colour management everything this renderer loads converts into.
struct ColorInfo {
    std::string config;        // the OCIO config loaded (path or URI)
    std::string configName;
    std::string workingSpace;  // OCIO name of the working space
    std::string display, view; // of 8-bit outputs
};

struct Capabilities {
    // The build has a denoiser (OIDN); otherwise denoisingNote says why not.
    bool denoising = false;
    std::string denoisingNote;
    bool motionBlur = false;   // section 5: the shapes are reserved
};

// Texture counters of the renderer's texture service.
struct RendererStats {
    uint64_t texturePrepareCalls = 0;
    uint64_t textureMemoryCacheHits = 0;
    uint64_t textureDiskCacheHits = 0;
    uint64_t textureSourceBytesHashed = 0;
    uint64_t texturePreparationNanoseconds = 0;  // summed over workers
};

class Renderer {
public:
    // The front ends' defaults: options from the environment (OCIO, PRISM_*).
    Renderer();
    explicit Renderer(RendererOptions const& options);
    // A renderer around an existing internal context (front ends in this
    // repository that create one themselves).
    explicit Renderer(std::shared_ptr<RendererContext> context);

    RendererOptions const& options() const;
    // Throws Error(InvalidArgument) when the colour options are unusable.
    ColorInfo color() const;
    // Devices this build and machine can create (not disabled by the
    // options), the CPU last.
    std::vector<DeviceInfo> devices() const;
    Capabilities capabilities() const;
    RendererStats stats() const;

    // A scene of this renderer; it keeps the renderer alive.
    Scene createScene(SceneOptions const& options = {}) const;
    // A session rendering `scene` (of this renderer); it creates the device
    // and keeps the scene and the renderer alive.
    Session createSession(Scene const& scene, SessionOptions const& options = {}) const;

    std::shared_ptr<RendererContext> const& context() const noexcept { return m_context; }

private:
    std::shared_ptr<RendererContext> m_context;
};

} // namespace prism
