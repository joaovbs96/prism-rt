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
    // fallback_sss_to_diffuse replaces the SSS weight with zero before code
    // generation, restoring the shader's diffuse lobe instead of the random
    // walk through the MDL volume (shaders/integrator/path_tracer.h). Off by
    // default; the prism:sssFallback setting (SetSssFallback) or, without it,
    // PRISM_SSS_FALLBACK=1 turns it on (DefaultSssFallback). Independent of the
    // color clamp.
    explicit DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp = false,
                            bool fallback_sss_to_diffuse = DefaultSssFallback());

    static bool DefaultSssFallback();
    // Process-wide, like the colour management: the frontends set it from
    // their settings before loading materials. Without a call, the
    // environment variable decides.
    static void SetSssFallback(bool enabled);

    MaterialX::DocumentPtr load(const std::string& filepath);
    MaterialX::DocumentPtr create_empty();
    MaterialX::DocumentPtr get_stdlib() const { return m_stdlib; }

private:
    MaterialX::DocumentPtr m_stdlib;
    bool m_clamp;
    bool m_fallback_sss_to_diffuse;
};
