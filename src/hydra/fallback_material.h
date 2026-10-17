#pragma once

#include "../material.h"

#include <algorithm>
#include <cmath>

namespace prism::hydra {

inline float NormalizeFallbackOpacity(float opacity)
{
    return std::isfinite(opacity) ? std::clamp(opacity, 0.0f, 1.0f) : 1.0f;
}

// Constant USD displayOpacity for the displayColor fallback. Opaque samples
// use Diffuse unchanged; transparent samples continue the path as null events.
class FallbackMaterial final : public Diffuse {
public:
    FallbackMaterial(Texture<Float3> *color, float opacity)
        : Diffuse(color), _opacity(NormalizeFallbackOpacity(opacity)) {}

    float cutoutOpacity(const RayHit &) override { return _opacity; }
    bool provenOpaque() const override { return _opacity >= 1.0f; }
    float opacity() const { return _opacity; }

    BRDFSample sample(const RayHit hit, unsigned int &seed) override
    {
        if (_opacity >= 1.0f)
            return Diffuse::sample(hit, seed);

        if (_opacity <= 0.0f || rnd(seed) >= _opacity)
            return _NullSample(hit);

        return Diffuse::sample(hit, seed);
    }

private:
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
};

} // namespace prism::hydra
