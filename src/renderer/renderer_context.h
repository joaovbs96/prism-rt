#pragma once

// The internal renderer context (docs/sdk_design.md, section 3): what used to
// be process-wide state of one renderer. The front ends create one (the CLI
// per run, the Hydra delegate per render delegate) and pass it, or what they
// need of it, to the loaders, services and devices. It becomes the public
// prism::Renderer's implementation later (migration step 3).

#include "renderer_options.h"

#include <memory>

namespace prism {

class RendererContext {
public:
    explicit RendererContext(RendererOptions options);
    RendererContext(RendererContext const&) = delete;
    RendererContext& operator=(RendererContext const&) = delete;

    RendererOptions const& options() const noexcept { return m_options; }

private:
    RendererOptions m_options;
};

} // namespace prism
