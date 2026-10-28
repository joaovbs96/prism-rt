#pragma once

// The public renderer context (docs/sdk_design.md, section 3.4): colour
// management, the material system, textures, the Embree device, logging and
// progress of one renderer. Several renderers per process are allowed.
//
// This version exposes what scenes need; the renderer's options struct and
// sessions become public with the session API (migration step 4). Until
// then front ends in this repository build a Renderer from the internal
// RendererOptions (src/renderer/renderer_options.h).

#include "scene.h"

#include <memory>

namespace prism {

struct RendererOptions;
class RendererContext;

class Renderer {
public:
    // The front ends' defaults: options from the environment (OCIO, PRISM_*).
    Renderer();
    explicit Renderer(RendererOptions const& options);
    // A renderer around an existing internal context (front ends that create
    // one themselves).
    explicit Renderer(std::shared_ptr<RendererContext> context);

    // A scene of this renderer; it keeps the renderer alive.
    Scene createScene(SceneOptions const& options = {}) const;

    std::shared_ptr<RendererContext> const& context() const noexcept { return m_context; }

private:
    std::shared_ptr<RendererContext> m_context;
};

} // namespace prism
