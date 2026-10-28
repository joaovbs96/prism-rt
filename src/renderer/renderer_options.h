#pragma once

// The renderer's options are public (include/prism/options.h); this header
// adds what the renderer context hands its devices besides them.

#include "../../include/prism/options.h"
#include "../color/color_management.h"
#include "logger.h"

#include <memory>

namespace prism {

class Progress;
class TextureService;
class VaryingEmitterSignal;

// DeviceOptions with the renderer's services (RendererContext::
// deviceResources), for CreateDevice and AvailableDevices.
struct DeviceResources : DeviceOptions {
    DeviceResources() = default;
    // Implicit: callers with only the public options get no services.
    DeviceResources(DeviceOptions const& options) : DeviceOptions(options) {}

    // The renderer's texture service; null: the runtime's
    // (TextureService::Acquire()).
    std::shared_ptr<TextureService> textures;
    // The renderer's progress model and logger, and its note of materials of
    // varying emission (the Vulkan device prewarms their raygen variant);
    // null: none, and the calling thread's logger.
    Progress* progress = nullptr;
    Logger const* logger = nullptr;
    std::shared_ptr<VaryingEmitterSignal> varyingEmitters;
};

} // namespace prism
