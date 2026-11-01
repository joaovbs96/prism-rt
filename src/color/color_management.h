#pragma once

// Colour management (docs/color_management.md). The renderer works in one
// scene-linear working space (ACEScg by default) taken from an OpenColorIO
// config (the renderer's, by default $OCIO, else the built-in studio config;
// RendererOptions::FromEnvironment). Everything that enters the
// renderer converts into it once: MaterialX colour-space tags (the MDL
// generator's colour transforms), light colours and blackbody temperatures
// (authored as linear Rec.709), and textures prepared with a colour space.
// Linear outputs (EXR, Hydra AOVs) stay in the working space; 8-bit outputs
// go through the configured display and view (ACES 2.0 SDR on sRGB).

#include "../internal_export.h"
#include "../float3.h"
#include "../../include/prism/options.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace prism::color {

// Row-major 3x3 matrix applied to column vectors: out = M * in.
using Matrix3 = std::array<float, 9>;
inline constexpr Matrix3 kIdentity{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
// Rec.709 / sRGB luminance (the weights the renderer used before colour management).
inline constexpr std::array<float, 3> kRec709Luminance{0.2126f, 0.7152f, 0.0722f};
inline constexpr const char *kDefaultConfig = "ocio://studio-config-latest";
inline constexpr const char *kDefaultWorkingSpace = "ACEScg";

inline Float3 Apply(Matrix3 const &m, Float3 const &c)
{
    return Float3(m[0] * c.x() + m[1] * c.y() + m[2] * c.z(),
                  m[3] * c.x() + m[4] * c.y() + m[5] * c.z(),
                  m[6] * c.x() + m[7] * c.y() + m[8] * c.z());
}

// The public colour options (include/prism/options.h); an empty config is
// kDefaultConfig, an empty working space the renderingColorSpace's
// (RenderingColorSpaceWorkingSpace), else ACEScg.
using Options = prism::ColorOptions;

class PRISM_INTERNAL_API ColorManagement {
public:
    // Throws std::runtime_error for an unreadable config, an unknown or
    // non-linear working space, a config without linear Rec.709 (looked up
    // by name and its usual aliases). kDefaultConfig where the linked
    // OpenColorIO lacks it loads its newest built-in studio config, and with
    // `view` a display or view the config lacks becomes its default; both
    // are reported to standard error. Without `view` no view processor is built: display() and
    // view() are the requested names and ApplyView only clamps (hosts that
    // apply their own view, the Hydra delegate).
    static std::shared_ptr<const ColorManagement> Create(Options const &options, bool view = true);
    // Never throws: Create(options, view), else with the built-in config
    // (kDefaultConfig) and the same working space, else the built-in config's
    // defaults, else PassThrough(options). Each failure goes to `warn`.
    // requested() is `options` whichever succeeded.
    static std::shared_ptr<const ColorManagement> CreateOrFallback(
        Options const &options, bool view, std::function<void(std::string const &)> const &warn);
    // Linear Rec.709 working space without a config: nothing converts,
    // every colour space name resolves to itself, ApplyView only clamps.
    static std::shared_ptr<const ColorManagement> PassThrough(Options const &requested);
    bool passThrough() const noexcept;

    // The matrix from the working space to the scene-linear colour space
    // `space` (an OCIO name or alias, a renderingColorSpace token such as
    // lin_rec709_scene; data, raw and identity are linear Rec.709), with the
    // config's name of that space in *name. False, with a message in *error,
    // when the config has no such space or it is not scene-linear.
    bool OutputMatrix(std::string const &space, Matrix3 *matrix, std::string *name, std::string *error) const;
    // The config's name of linear Rec.709.
    std::string const &rec709Space() const noexcept;

    Options const &requested() const noexcept;
    std::string const &config() const noexcept;       // the config loaded (path or URI)
    std::string const &configName() const noexcept;   // its name
    std::string const &workingSpace() const noexcept; // canonical OCIO name
    // The working space's MaterialX colour-space name (acescg, lin_rec709,
    // ...), or "prism_working_<hash>" when MaterialX has none for it.
    std::string const &materialXSpace() const noexcept;
    std::string const &display() const noexcept;
    std::string const &view() const noexcept;
    // Config, working space and matrices: part of every cache key whose
    // contents depend on the working space.
    std::string const &identity() const noexcept;
    // The working space is linear Rec.709: nothing converts.
    bool rec709() const noexcept;
    Matrix3 const &fromRec709() const noexcept;
    Matrix3 const &toRec709() const noexcept;
    std::array<float, 3> const &luminanceWeights() const noexcept;

    Float3 FromRec709(Float3 const &c) const;
    float Luminance(Float3 const &c) const;

    // The config's name for `space` (OCIO names, aliases and the MaterialX
    // names its aliases cover), or empty when the config has none.
    std::string Resolve(std::string const &space) const;
    // Pixels in `space` differ from the working space: `space` resolves, is
    // not data (Raw) and is not the working space itself.
    bool NeedsConversion(std::string const &space) const;
    // Converts the first three of `channels` floats of `count` pixels from
    // `space` into the working space; other channels are kept. Throws for an
    // unknown space.
    void ToWorkingSpace(std::string const &space, float *pixels, size_t count, int channels) const;
    // Working space to the display/view encoding, clamped to [0, 1], for
    // 8-bit outputs. The first three of `channels` floats are converted.
    void ApplyView(float *pixels, size_t count, int channels) const;

    struct Impl;
    explicit ColorManagement(std::shared_ptr<Impl> impl);

private:
    std::shared_ptr<Impl> m_impl;
};

// There is no process-wide colour management: each renderer context owns one
// (renderer/renderer_context.h) and hands it to its loaders, materials,
// textures and scenes (SceneManager::colorManagement), which devices read.

// The newest built-in studio config of `names` (OpenColorIO's built-in config
// names, e.g. studio-config-v4.0.0_aces-v2.0_ocio-v2.5), or empty when there is
// none. kDefaultConfig falls back to it where an OpenColorIO has no
// studio-config-latest (2.2).
PRISM_INTERNAL_API std::string SelectBuiltinStudioConfig(std::vector<std::string> const &names);
// The names in the linked OpenColorIO's built-in config registry.
PRISM_INTERNAL_API std::vector<std::string> BuiltinConfigNames();
// Makes kDefaultConfig fail to load as on an OpenColorIO without it (tests).
PRISM_INTERNAL_API void SetDefaultConfigUnavailableForTesting(bool unavailable);

// The working space that a UsdRenderSettings.renderingColorSpace token asks
// for in the OpenColorIO config `config` (empty: the default config), as a
// name of that config; empty for the default working space.
// USD's names (GfColorSpaceNames: lin_rec709_scene, lin_ap1_scene, ...)
// resolve through the config's aliases. data, raw and identity ("no source
// colour transforms") take linear Rec.709, the space USD authors untagged
// colours and light colours in. unknown and an empty token take the default;
// so do names the config lacks and spaces that are not scene-linear, with a
// message in *warning.
PRISM_INTERNAL_API std::string RenderingColorSpaceWorkingSpace(std::string const &token, std::string const &config,
                                            std::string *warning = nullptr);

// Colour space names of the colour-space metadata authored on USD attributes
// (UsdColorSpaceAPI and ASWF interop names such as lin_rec709_scene) resolve
// through Resolve() as well; this is the default for authored light colours.
inline constexpr const char *kLightColorSpace = "lin_rec709";

} // namespace prism::color
