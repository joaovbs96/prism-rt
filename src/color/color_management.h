#pragma once

// Colour management (docs/color_management.md). The renderer works in one
// scene-linear working space (ACEScg by default) taken from an OpenColorIO
// config ($OCIO, else the built-in studio config). Everything that enters the
// renderer converts into it once: MaterialX colour-space tags (the MDL
// generator's colour transforms), light colours and blackbody temperatures
// (authored as linear Rec.709), and textures prepared with a colour space.
// Linear outputs (EXR, Hydra AOVs) stay in the working space; 8-bit outputs
// go through the configured display and view (ACES 2.0 SDR on sRGB).

#include "../float3.h"

#include <array>
#include <cstddef>
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

struct Options {
    std::string config;       // empty: $OCIO, else kDefaultConfig
    std::string workingSpace; // empty: ACEScg (the config's scene_linear role if it has none)
    std::string display;      // empty: the config's default display
    std::string view;         // empty: the display's default view
    bool operator==(Options const &) const = default;
};

class ColorManagement {
public:
    // Throws std::runtime_error for an unreadable config, an unknown or
    // non-linear working space, or an unknown display/view.
    static std::shared_ptr<const ColorManagement> Create(Options const &options);

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

// The process-wide colour management every loader and device reads. Created
// from default Options on first use. Changing it does not reconvert loaded
// scene data: callers reload what depends on it (the Hydra delegate rebuilds
// the scene).
std::shared_ptr<const ColorManagement> Active();
void SetActive(std::shared_ptr<const ColorManagement> colorManagement);
// Makes a colour management for `options` active unless the active one was
// made from the same options; returns the active one.
std::shared_ptr<const ColorManagement> Activate(Options const &options);

// Colour space names of the colour-space metadata authored on USD attributes
// (UsdColorSpaceAPI and ASWF interop names such as lin_rec709_scene) resolve
// through Resolve() as well; this is the default for authored light colours.
inline constexpr const char *kLightColorSpace = "lin_rec709";

} // namespace prism::color
