#pragma once

#include "../geometry.h"
#include "../material.h"

#include <algorithm>
#include <cmath>

namespace prism::hydra {

inline float NormalizeFallbackOpacity(float opacity)
{
    return std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
}

// The material of geometry without one: USD displayColor and displayOpacity.
// Constant values are the texture and the opacity. With primvarColor or
// primvarOpacity, the hit mesh's interpolated `displayColor` / `displayOpacity`
// primvar (Mesh::primvars, any USD interpolation) replaces them where the mesh
// has it. Opaque samples use Diffuse unchanged; transparent samples continue
// the path as null events.
class FallbackMaterial final : public Diffuse {
public:
    FallbackMaterial(Texture<Float3> *color, float opacity, bool primvarColor = false,
                     bool primvarOpacity = false)
        : Diffuse(color), _opacity(NormalizeFallbackOpacity(opacity)), _primvarColor(primvarColor),
          _primvarOpacity(primvarOpacity) {}

    float cutoutOpacity(const RayHit &hit) override { return opacityAt(hit); }
    bool provenOpaque() const override { return !_primvarOpacity && _opacity >= 1.0f; }
    // The constant opacity (the value where a mesh lacks the primvar).
    float opacity() const { return _opacity; }
    bool primvarColor() const { return _primvarColor; }
    bool primvarOpacity() const { return _primvarOpacity; }

    BRDFSample sample(const RayHit hit, unsigned int &seed) override
    {
        const float opacity = opacityAt(hit);
        if (opacity < 1.0f && (opacity <= 0.0f || rnd(seed) >= opacity))
            return _NullSample(hit);
        BRDFSample s = Diffuse::sample(hit, seed);
        float color[3];
        if (_primvarColor && _Primvar(hit, "displayColor", color, 3)) s.color = Color3(color[0], color[1], color[2]);
        return s;
    }

    BRDFEvaluation evaluate(const RayHit &hit, const Float3 &wi) override
    {
        BRDFEvaluation e = Diffuse::evaluate(hit, wi);
        float color[3];
        if (_primvarColor && _Primvar(hit, "displayColor", color, 3))
            e.value = Color3(color[0], color[1], color[2]) * e.pdf; // pdf = cosine / pi
        return e;
    }

    float opacityAt(const RayHit &hit) const
    {
        float value = 0.0f;
        if (_primvarOpacity && _Primvar(hit, "displayOpacity", &value, 1))
            return NormalizeFallbackOpacity(value);
        return _opacity;
    }

private:
    static bool _Primvar(const RayHit &hit, const char *name, float *out, int count)
    {
        auto const *mesh = static_cast<Mesh const *>(hit.primvarMesh);
        return mesh && hit.primid >= 0 &&
            mesh->interpolatePrimvar(name, unsigned(hit.primid), hit.bary_u, hit.bary_v, out, count);
    }

    static BRDFSample _NullSample(const RayHit &hit)
    {
        BRDFSample sample{};
        sample.wi = hit.d;
        sample.color = Color3(1.0f);
        sample.emission = Color3(0.0f);
        sample.scattered = true;
        sample.pdf = 0.0f;
        sample.delta = true;
        sample.null_event = true;
        sample.nee_compatible = false;
        return sample;
    }

    float _opacity;
    bool _primvarColor;
    bool _primvarOpacity;
};

} // namespace prism::hydra
