#include "integration_fixture.h"

#include <algorithm>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

int RunMultisampledPresentationTest()
{
    using namespace prism::hydra::test;
    try {
        auto render = [](bool multiSampled) {
            Fixture fixture;
            const SdfPath path("/Plane");
            fixture.scene->AddPrims({
                {path, HdPrimTypeTokens->mesh, Plane()},
                {SdfPath("/Dome"), HdPrimTypeTokens->domeLight, Dome()}});
            Require(fixture.color.Allocate(
                        GfVec3i(Fixture::Size, Fixture::Size, 1),
                        HdFormatFloat32Vec4, multiSampled),
                    "Host-requested color AOV allocation failed");
            Require(fixture.color.IsMultiSampled() == multiSampled,
                    "Render buffer did not retain the host multisampled flag");
            fixture.Render();
            const std::vector<int> primIds = Read<int>(fixture.prim);
            Require(primIds.size() == size_t(Fixture::Size) * Fixture::Size &&
                        primIds[(Fixture::Size / 2) * Fixture::Size +
                                Fixture::Size / 2] >= 0,
                    "AOV presentation test did not render the fixture mesh");

            // Hydra calls Resolve before exposing a multisampled Bprim. Prism
            // stores one resolved image, so Resolve is intentionally a no-op.
            fixture.color.Resolve();
            void const *mapped = fixture.color.Map();
            Require(mapped != nullptr, "Resolved AOV could not be mapped");
            auto const *values = static_cast<float const *>(mapped);
            const size_t count = size_t(Fixture::Size) * Fixture::Size * 4;
            std::vector<float> pixels(values, values + count);
            fixture.color.Unmap();
            Require(std::any_of(pixels.begin(), pixels.end(),
                        [](float value) { return value > 0.0f; }),
                    "Rendered color AOV had no nonzero RGB values");
            return pixels;
        };

        const std::vector<float> ordinary = render(false);
        const std::vector<float> hostMultisampled = render(true);
        Require(!ordinary.empty() && ordinary == hostMultisampled,
                "Host-requested multisampled AOV changed resolved pixels");
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
