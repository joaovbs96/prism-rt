#pragma once

#include <MaterialXCore/Document.h>
#include <string>

class DocumentLoader
{
public:
    // Takes the stdlib from MDLGenerator so both share the same instance
    // clamp: limit standard_surface base_color / specular_color to [0, 0.9999]
    // (upstream MaterialX conductor-IOR bug). Other shading models and inputs
    // are left untouched, so HDR emission etc. is preserved.
    //
    // Until the integrator supports volume transport, replace the SSS weight
    // with zero before code generation. This restores the shader's diffuse lobe;
    // ignoring MDL volume expressions alone leaves a transmitting surface.
    // Pass false only when testing real SSS support. Independent of color clamp.
    explicit DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp = false,
                            bool fallback_sss_to_diffuse = true);

    MaterialX::DocumentPtr load(const std::string& filepath);
    MaterialX::DocumentPtr create_empty();
    MaterialX::DocumentPtr get_stdlib() const { return m_stdlib; }

private:
    MaterialX::DocumentPtr m_stdlib;
    bool m_clamp;
    bool m_fallback_sss_to_diffuse;
};
