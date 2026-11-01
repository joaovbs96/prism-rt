// src/color against the OpenColorIO that is linked, with $OCIO unset: the
// default config, the working space, linear Rec.709 and a usable view come
// from a built-in studio config. Written for OpenColorIO 2.2.x, which has no
// ocio://studio-config-latest (tests/ocio22/CMakeLists.txt); on newer versions
// the default config is the latest one and the same checks hold.

#include "../../src/color/color_management.h"

#include <OpenColorIO/OpenColorIO.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, std::string const &what)
{
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
} // namespace

int main()
{
    using prism::color::ColorManagement;
    _putenv_s("OCIO", "");
    std::printf("OpenColorIO %s\n", OCIO_NAMESPACE::GetVersion());
    const auto names = prism::color::BuiltinConfigNames();
    for (auto const &name : names) std::printf("  built-in: %s\n", name.c_str());
    const std::string studio = prism::color::SelectBuiltinStudioConfig(names);
    Check(!studio.empty(), "a built-in studio config is chosen: " + studio);

    std::vector<std::string> warnings;
    auto warn = [&](std::string const &message) {
        warnings.push_back(message);
        std::printf("  warning: %s\n", message.c_str());
    };
    // The CLI's defaults (empty config, ACES 2.0 view requested explicitly).
    auto color = ColorManagement::CreateOrFallback({"", "", "", "ACES 2.0 - SDR 100 nits (Rec.709)"}, true, warn);
    std::printf("  config %s (%s), working %s, display %s, view %s, rec709 %s\n", color->config().c_str(),
                color->configName().c_str(), color->workingSpace().c_str(), color->display().c_str(),
                color->view().c_str(), color->rec709Space().c_str());
    Check(!color->passThrough(), "not pass-through");
    Check(color->config().rfind("ocio://studio-config", 0) == 0, "the built-in studio config: " + color->config());
    Check(color->workingSpace() == "ACEScg", "ACEScg working space");
    Check(!color->rec709Space().empty() && !color->Resolve("lin_rec709").empty(), "lin_rec709 resolves");
    Check(!color->display().empty() && !color->view().empty(), "a display and view");
    float pixel[3] = {0.18f, 0.18f, 0.18f};
    color->ApplyView(pixel, 1, 3);
    Check(pixel[0] > 0.2f && pixel[0] < 0.8f && pixel[0] == pixel[1], "the view tone-maps grey: " + std::to_string(pixel[0]));
    float srgb[3] = {0.5f, 0.5f, 0.5f};
    Check(color->NeedsConversion("srgb_texture") || color->NeedsConversion("sRGB - Texture"),
          "sRGB textures convert through the config");
    color->ToWorkingSpace(color->NeedsConversion("srgb_texture") ? "srgb_texture" : "sRGB - Texture", srgb, 1, 3);
    Check(srgb[0] > 0.1f && srgb[0] < 0.3f, "an sRGB texel decodes to linear: " + std::to_string(srgb[0]));
    std::string warning;
    const std::string working = prism::color::RenderingColorSpaceWorkingSpace("lin_rec709_scene", "", &warning);
    Check(!working.empty(), "renderingColorSpace lin_rec709_scene resolves: " + working + " " + warning);
    prism::color::Matrix3 m{};
    std::string name, error;
    Check(color->OutputMatrix("lin_rec709", &m, &name, &error), "output matrix to lin_rec709 " + error);
    Check(!warnings.empty(), "the substitutions were reported");

    // The Hydra delegate's path: no view processor.
    warnings.clear();
    auto noView = ColorManagement::CreateOrFallback({}, false, warn);
    Check(!noView->passThrough() && noView->workingSpace() == "ACEScg", "no-view colour management works");
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
