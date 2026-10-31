#pragma once

// The internal renderer context (docs/sdk_design.md, section 3): what used to
// be process-wide state of one renderer. Creating one configures the process
// runtime (runtime.h) with its options. The front ends create one (the CLI
// per run, the Hydra delegate per render delegate) and pass it, or what they
// need of it, to the loaders, services and devices. It becomes the public
// prism::Renderer's implementation later (migration step 3).

#include "../internal_export.h"
#include "../progress.h"
#include "../varying_emitter_signal.h"
#include "logger.h"
#include "material_settings.h"
#include "renderer_options.h"
#include "runtime.h"

#include <embree4/rtcore_device.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace prism {

class PRISM_INTERNAL_API RendererContext {
public:
    explicit RendererContext(RendererOptions options);
    ~RendererContext();
    RendererContext(RendererContext const&) = delete;
    RendererContext& operator=(RendererContext const&) = delete;

    RendererOptions const& options() const noexcept { return m_options; }

    // The colour management everything this renderer loads converts into,
    // created from options().color on first use. Scenes keep the one they
    // were loaded with (SceneManager::colorManagement).
    std::shared_ptr<const color::ColorManagement> colorManagement() const;
    // Makes the colour management of `options` current, unless the current
    // one was made from the same options, and returns it. What was loaded
    // before keeps its colour management: callers reload it (the Hydra
    // delegate rebuilds its scene). Throws std::runtime_error for unusable
    // options and keeps the current one. Without `view` no view processor
    // is built (ColorManagement::Create).
    std::shared_ptr<const color::ColorManagement> setColor(color::Options const& options, bool view = true);
    // Like setColor, but never throws: unusable options warn through `warn`
    // and fall back (ColorManagement::CreateOrFallback). For hosts an
    // exception must not reach (the Hydra delegate).
    std::shared_ptr<const color::ColorManagement> setColorOrFallback(
        color::Options const& options, bool view, std::function<void(std::string const&)> const& warn);

    // Whether subsurface scattering renders its diffuse lobe (initially
    // options().materials.sssFallback; the Hydra delegate's prism:sssFallback
    // changes it). Materials compiled before keep theirs.
    bool sssFallback() const;
    void setSssFallback(bool enabled);

    // The current colour management and material options, for loaders and
    // the material service.
    MaterialSettings materialSettings() const;

    // This renderer's texture service (TextureService::Acquire of its
    // options: renderers with the same options share one), created on first
    // use and held while the context lives.
    std::shared_ptr<TextureService> textures() const;
    // options().devices with the texture service, the progress model, the
    // logger and the varying-emission note, for CreateDevice.
    DeviceResources deviceOptions() const;

    // The renderer's diagnostics (options().log, logLevel, debugCategories):
    // its warnings deduplicate per renderer. Bind it (logging::Bind) on the
    // threads that work for the renderer.
    Logger const& logger() const noexcept { return m_logger; }
    // The renderer's load and preparation progress (services and devices
    // report to it; hosts poll it).
    Progress& progress() noexcept { return m_progress; }
    // Notes of materials of varying emission (MaterialSettings, DeviceOptions).
    std::shared_ptr<VaryingEmitterSignal> const& varyingEmitters() const noexcept { return m_varyingEmitters; }
    // The renderer's Embree device, created on first use (its scenes'
    // acceleration structures). Throws std::runtime_error if Embree fails.
    RTCDevice& embreeDevice() const;

private:
    RendererOptions m_options;
    mutable std::mutex m_mutex;
    mutable std::shared_ptr<const color::ColorManagement> m_color;
    mutable std::shared_ptr<TextureService> m_textures;
    bool m_sssFallback = false;
    Logger m_logger;
    mutable Progress m_progress;
    std::shared_ptr<VaryingEmitterSignal> m_varyingEmitters = std::make_shared<VaryingEmitterSignal>();
    mutable RTCDevice m_embree = nullptr;
};

} // namespace prism
