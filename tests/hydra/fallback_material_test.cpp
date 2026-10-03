#include "../../src/hydra/fallback_material.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

RayHit makeHit()
{
    RTCRayHit raw{};
    raw.ray = initialize_ray(Float3(0, 0, 1), Float3(0, 0, -1));
    raw.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    RayHit hit(raw);
    hit.d = Float3(0, 0, -1);
    hit.n = Float3(0, 0, 1);
    hit.p = Float3(0);
    return hit;
}
}

int main()
{
    try {
        ValueTexture<Float3> color(Float3(0.25f, 0.5f, 0.75f));
        Diffuse diffuse(&color);
        prism::hydra::FallbackMaterial opaque(&color, 1.0f);
        RayHit hit = makeHit();

        unsigned int expectedSeed = 193;
        unsigned int actualSeed = expectedSeed;
        const BRDFSample expected = diffuse.sample(hit, expectedSeed);
        const BRDFSample actual = opaque.sample(hit, actualSeed);
        require(actualSeed == expectedSeed, "Opacity 1 changed the Diffuse RNG sequence");
        require(actual.color.x() == expected.color.x() &&
                actual.color.y() == expected.color.y() &&
                actual.color.z() == expected.color.z() &&
                actual.wi.x() == expected.wi.x() &&
                actual.wi.y() == expected.wi.y() &&
                actual.wi.z() == expected.wi.z() &&
                actual.pdf == expected.pdf && !actual.null_event,
                "Opacity 1 did not preserve Diffuse sampling");
        require(opaque.provenOpaque() && opaque.cutoutOpacity(hit) == 1.0f,
                "Opacity 1 shadow behavior is not opaque");

        prism::hydra::FallbackMaterial transparent(&color, 0.0f);
        actualSeed = 193;
        const BRDFSample nullSample = transparent.sample(hit, actualSeed);
        require(actualSeed == 193, "Opacity 0 consumed RNG state");
        require(nullSample.scattered && nullSample.null_event && nullSample.delta &&
                nullSample.pdf == 0.0f && nullSample.color.x() == 1.0f &&
                nullSample.color.y() == 1.0f && nullSample.color.z() == 1.0f &&
                nullSample.wi.z() == hit.d.z(),
                "Opacity 0 did not produce a unit-throughput null event");
        require(!transparent.provenOpaque() &&
                transparent.cutoutOpacity(hit) == 0.0f,
                "Opacity 0 shadow behavior is not transparent");

        constexpr unsigned int sampleCount = 4096;
        constexpr float opacity = 0.35f;
        prism::hydra::FallbackMaterial fractional(&color, opacity);
        unsigned int accepted = 0;
        for (unsigned int initial = 1; initial <= sampleCount; ++initial) {
            unsigned int seed = initial;
            const BRDFSample sample = fractional.sample(hit, seed);
            accepted += !sample.null_event;
        }
        require(accepted > 1300 && accepted < 1600,
                "Fractional opacity did not stochastically accept its expected coverage");
        require(std::abs(fractional.cutoutOpacity(hit) - opacity) < 1e-6f,
                "Fractional opacity was not exposed to shadow rays");

        require(prism::hydra::NormalizeFallbackOpacity(-1.0f) == 0.0f &&
                prism::hydra::NormalizeFallbackOpacity(2.0f) == 1.0f &&
                prism::hydra::NormalizeFallbackOpacity(
                    std::numeric_limits<float>::quiet_NaN()) == 1.0f,
                "Opacity normalization did not clamp or default non-finite values");
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
