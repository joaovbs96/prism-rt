#pragma once

// Materials of a public prism::Scene (docs/sdk_design.md, section 3.6).
// MaterialX stays out of the API: documents come as files or strings, and
// compile through MDL at Scene::commit() (load time, never while rendering).

#include "types.h"

#include <string>

namespace prism {

struct MaterialDesc {
    enum class Kind {
        MaterialXFile,    // `path`: a .mtlx document
        MaterialXString,  // `document`: .mtlx XML text
        Diffuse,          // Lambertian `color` (the fallback of geometry without a material)
        Emissive,         // constant emission of `color`
    };
    Kind kind = Kind::Diffuse;
    std::string path;          // MaterialXFile
    std::string document;      // MaterialXString
    std::string materialName;  // empty: the first material of the document
    // An MDL resource search path for the document's textures (added to the
    // MDL SDK's search paths before it compiles); empty: none.
    std::string searchPath;
    // Diffuse / Emissive, in the scene's working space.
    Color3 color{0.18f};
    float opacity = 1.0f;      // Diffuse: below 1 makes it a cutout
    float intensity = 1.0f;    // Emissive
    // Diffuse: the colour and opacity come from the hit geometry's
    // displayColor / displayOpacity primvars (USD's varying display colours);
    // `color` and `opacity` are their fallbacks.
    bool displayColorPrimvar = false, displayOpacityPrimvar = false;
    // A name for diagnostics (the front end's material path or index).
    std::string name;
};

} // namespace prism
