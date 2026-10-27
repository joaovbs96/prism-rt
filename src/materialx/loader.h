#pragma once

#include <MaterialXCore/Document.h>
#include <array>
#include <functional>
#include <string>

class DocumentLoader
{
public:
    // Takes the stdlib from MDLGenerator so both share the same instance
    // clamp: limit standard_surface base_color / specular_color to [0, 0.9999]
    // (upstream MaterialX conductor-IOR bug). Other shading models and inputs
    // are left untouched, so HDR emission etc. is preserved.
    //
    // fallback_sss_to_diffuse replaces the SSS weight with zero before code
    // generation, restoring the shader's diffuse lobe instead of the random
    // walk through the MDL volume (shaders/integrator/path_tracer.h). Off by
    // default; the renderer's material settings (prism:sssFallback,
    // PRISM_SSS_FALLBACK=1) turn it on. Independent of the color clamp.
    explicit DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp = false,
                            bool fallback_sss_to_diffuse = false);

    // Constant colours on uniform inputs (gltf_pbr's attenuation_color,
    // UsdUVTexture's scale and bias) in a colour space other than the working
    // space: the transform MaterialX generates for them is varying, which MDL
    // rejects for a uniform parameter (C219). load() converts such a value
    // with `convert` and tags it `target` (the working space's MaterialX name,
    // the generator's target), so no transform is generated. `convert` returns
    // false for a space it cannot convert; the value is then left as authored.
    // Without a conversion (the default) the values are left as authored.
    struct UniformColorConversion {
        std::string target;
        std::function<bool(const std::string& space, std::array<float, 3>& rgb)> convert;
    };
    void setUniformColorConversion(UniformColorConversion conversion) { m_uniformColors = std::move(conversion); }

    MaterialX::DocumentPtr load(const std::string& filepath);
    MaterialX::DocumentPtr create_empty();
    MaterialX::DocumentPtr get_stdlib() const { return m_stdlib; }

private:
    MaterialX::DocumentPtr m_stdlib;
    bool m_clamp;
    bool m_fallback_sss_to_diffuse;
    UniformColorConversion m_uniformColors;
};
