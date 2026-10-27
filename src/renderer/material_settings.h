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
    // Subsurface scattering renders its diffuse lobe (DocumentLoader's
    // fallback_sss_to_diffuse; prism:sssFallback, PRISM_SSS_FALLBACK).
    bool sssFallback = false;
    // MaterialOptions::generationCache and mdlResourceMode.
    bool generationCache = true;
    std::string mdlResourceMode = "auto";

    // The colour identity, the SSS fallback and the resource mode: part of
    // every shared cache key of compiled materials.
    std::string Identity() const
    {
        return (color ? color->identity() : std::string()) + (sssFallback ? "|sss-fallback" : "") +
            (mdlResourceMode != "auto" ? "|resources=" + mdlResourceMode : std::string());
    }
};

} // namespace prism
