#pragma once

// What a compiled material depends on besides its MaterialX document and
// libraries: the renderer's colour management (its working space: colour
// transforms, uniform colours, tagged textures) and its SSS fallback. The
// loaders and the material service take it from the renderer context
// (RendererContext::materialSettings) and pass it to every material they
// compile; shared caches of compiled materials key by Identity().

#include "../color/color_management.h"

#include <filesystem>
#include <memory>
#include <string>

namespace prism {

struct MaterialSettings {
    std::shared_ptr<const color::ColorManagement> color;
    // The root of the persistent caches; generated documents go to its
    // "materials" directory.
    std::filesystem::path cacheDirectory;

    // The colour identity: part of every shared cache key of compiled
    // materials.
    std::string Identity() const
    {
        return color ? color->identity() : std::string();
    }
};

} // namespace prism
