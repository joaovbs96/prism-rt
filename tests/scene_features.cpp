// UsdLux and geometry features of both USD loaders (docs/hydra.md, "Scene
// features"), checked on renders of their fixtures in tests/hydra/fixtures.
// The parity harness renders each fixture with the CLI and the Hydra
// delegate; this checks that each render shows the feature.
//
// prism_scene_features_test FEATURE IMAGE.exr...
//
//   camera_visibility  light_camera_visibility.usda: the light with
//                      cameraVisibility = 1 is seen by the camera (and
//                      covers its pixels), the other is not; both light the floor.
//   dome_temperature   dome_color_temperature.usda: the dome's colour
//                      temperature tints it.
//   invisible_ids      pointinstancer_invisible_ids.usda: invisibleIds hide
//                      their instances.
//   display_color      display_color_interpolation.usda: displayColor and
//                      displayOpacity of every interpolation (and indexed).
//   light_lobes        FULL DIFFUSE SPECULAR: light_lobes.usda and its
//                      variants: inputs:diffuse / inputs:specular light
//                      multipliers split the image by BSDF lobe.
//   double_sided       double_sided.usda: with prism:honorDoubleSided,
//                      back faces of single-sided, non-transmissive meshes
//                      are not hit.
//   instance_primvars  instance_primvars.usda (Hydra only): per-instance
//                      primvars reach MDL geompropvalue lookups.
//   lobes_consistency  MIS BSDF: a light_lobes variant rendered with MIS and
//                      with BSDF sampling only agrees on every card.
//   sss_fallback       sss_fallback.usda: prism:sssFallback renders the
//                      furnace's subsurface-only sphere by its (black)
//                      diffuse lobe instead of tracing the walk.

#include <OpenImageIO/imagebuf.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool ok, std::string const& what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    } else {
        std::printf("ok: %s\n", what.c_str());
    }
}

struct Image {
    int width = 0, height = 0, channels = 0;
    std::vector<float> pixels;

    bool Load(std::string const& path)
    {
        OIIO::ImageBuf buffer(path);
        if (!buffer.read(0, 0, true, OIIO::TypeDesc::FLOAT)) return false;
        auto const& spec = buffer.spec();
        width = spec.width;
        height = spec.height;
        channels = spec.nchannels;
        pixels.resize(size_t(width) * height * channels);
        return buffer.get_pixels(OIIO::ROI(spec.x, spec.x + width, spec.y, spec.y + height, 0, 1, 0, channels),
                                 OIIO::TypeDesc::FLOAT, pixels.data());
    }

    float At(int x, int y, int c) const { return pixels[(size_t(y) * width + x) * channels + c]; }

    // Mean of channel c over the (2r+1)^2 box around (x, y).
    float Mean(int x, int y, int c, int r = 1) const
    {
        double sum = 0;
        int count = 0;
        for (int j = std::max(0, y - r); j <= std::min(height - 1, y + r); ++j)
            for (int i = std::max(0, x - r); i <= std::min(width - 1, x + r); ++i) {
                sum += At(i, j, c);
                ++count;
            }
        return count ? float(sum / count) : 0.0f;
    }

    // Mean of the RGB channels over the box.
    float Gray(int x, int y, int r = 1) const
    {
        return (Mean(x, y, 0, r) + Mean(x, y, 1, r) + Mean(x, y, 2, r)) / 3.0f;
    }

    bool HasAlpha() const { return channels >= 4; }
};

std::string Describe(char const* what, float value)
{
    char text[256];
    std::snprintf(text, sizeof(text), "%s (%g)", what, value);
    return text;
}

// light_camera_visibility.usda, 64x64: the visible light projects to about
// (15, 32), the hidden one to about (49, 32); nothing is behind them.
void CameraVisibility(Image const& image)
{
    Check(image.Gray(15, 32) > 1.0f, Describe("the camera-visible light is seen", image.Gray(15, 32)));
    Check(image.Gray(49, 32) < 1e-4f, Describe("the hidden light is not seen", image.Gray(49, 32)));
    Check(image.Gray(15, 55, 2) > 0.01f, Describe("the visible light lights the floor", image.Gray(15, 55, 2)));
    Check(image.Gray(49, 55, 2) > 0.01f, Describe("the hidden light lights the floor", image.Gray(49, 55, 2)));
    if (image.HasAlpha()) {
        Check(image.Mean(15, 32, 3, 0) > 0.99f, Describe("the visible light covers its pixel", image.Mean(15, 32, 3, 0)));
        Check(image.Mean(49, 32, 3, 0) < 0.01f, Describe("the hidden light leaves its pixel transparent",
                                                         image.Mean(49, 32, 3, 0)));
    }
}

// dome_color_temperature.usda: a 3000 K dome is orange (red > green > blue)
// in any working space; an untinted dome would be grey.
void DomeTemperature(Image const& image)
{
    const float r = image.Mean(3, 3, 0), g = image.Mean(3, 3, 1), b = image.Mean(3, 3, 2);
    Check(r > 0.1f, Describe("the dome is seen", r));
    Check(r > 1.1f * g && g > 1.1f * b, Describe("the dome is orange (red / blue)", b > 0.0f ? r / b : r));
    Check(r > 2.0f * b, Describe("the tint is strong, red / blue", b > 0.0f ? r / b : r));
}

// pointinstancer_invisible_ids.usda: red cards at about x = 7, 24, 40, 57
// (row 32) over a white dome; invisibleIds hides the second and fourth.
void InvisibleIds(Image const& image)
{
    const int columns[4] = {7, 24, 40, 57};
    for (int i = 0; i < 4; ++i) {
        const bool hidden = i == 1 || i == 3;
        const float r = image.Mean(columns[i], 32, 0), g = image.Mean(columns[i], 32, 1);
        const std::string name = "instance " + std::to_string(i);
        if (hidden) {
            Check(g > 0.7f, Describe((name + " is hidden: the dome shows (green)").c_str(), g));
        } else {
            Check(r > 0.4f && g < 0.3f, Describe((name + " is drawn (green)").c_str(), g));
        }
    }
}

// display_color_interpolation.usda: cards at columns 11, 32, 53 and rows 21
// (top) and 43 (bottom); a two-face card's faces at its column -5 and +5.
void DisplayColor(Image const& image)
{
    struct Expected {
        char const* what;
        int x, y;
        float r, g, b;
        float tolerance;
    };
    // The vertex gradients 8 pixels from the centre are 9% of the way from
    // their end colour to the other.
    const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const float t = (0.35f - 8.0f / 28.0f) / 0.7f;
    const Expected expected[] = {
        {"constant", 11, 21, 0.8f, 0.1f, 0.1f, 0.06f},
        {"uniform, first face", 27, 21, 0.1f, 0.8f, 0.1f, 0.06f},
        {"uniform, second face", 37, 21, 0.1f, 0.1f, 0.8f, 0.06f},
        {"vertex, left", 45, 21, mix(0.8f, 0.1f, t), mix(0.1f, 0.8f, t), 0.1f, 0.08f},
        {"vertex, middle", 53, 21, 0.45f, 0.45f, 0.1f, 0.08f},
        {"vertex, right", 61, 21, mix(0.1f, 0.8f, t), mix(0.8f, 0.1f, t), 0.1f, 0.08f},
        {"faceVarying, first face", 6, 43, 0.8f, 0.1f, 0.1f, 0.06f},
        {"faceVarying, second face", 16, 43, 0.1f, 0.1f, 0.8f, 0.06f},
        {"indexed, left", 24, 43, mix(0.1f, 0.8f, t), mix(0.1f, 0.8f, t), mix(0.8f, 0.1f, t), 0.08f},
        {"indexed, right", 40, 43, mix(0.8f, 0.1f, t), mix(0.8f, 0.1f, t), mix(0.1f, 0.8f, t), 0.08f},
        {"opacity 1", 48, 43, 0.8f, 0.1f, 0.1f, 0.06f},
        {"opacity 0 (the dome)", 58, 43, 1.0f, 1.0f, 1.0f, 0.06f},
    };
    for (auto const& e : expected) {
        const float r = image.Mean(e.x, e.y, 0), g = image.Mean(e.x, e.y, 1), b = image.Mean(e.x, e.y, 2);
        const float error = std::max({std::abs(r - e.r), std::abs(g - e.g), std::abs(b - e.b)});
        char text[256];
        std::snprintf(text, sizeof(text), "%s: (%.3f, %.3f, %.3f), expected (%.3f, %.3f, %.3f)", e.what, r, g, b,
                      e.r, e.g, e.b);
        Check(error <= e.tolerance, text);
    }
}

// light_lobes.usda (full), light_lobes_diffuse.usda, light_lobes_specular.usda:
// cards at columns 11 (matte), 32 (plastic), 53 (metal), rows 18 to 46. The
// matte card has no glossy lobe; the metal card's diffuse part is only MDL's
// multiscatter compensation of its glossy lobe (about 1%). On every card the
// diffuse-only and specular-only images add up to the full one.
void LightLobes(std::vector<Image> const& images)
{
    Image const& full = images[0];
    Image const& diffuse = images[1];
    Image const& specular = images[2];
    struct Card {
        char const* name;
        int x;
    };
    const Card cards[] = {{"matte", 11}, {"plastic", 32}, {"metal", 53}};
    for (auto const& card : cards) {
        for (int y : {23, 32, 41}) {
            const float f = full.Gray(card.x, y, 4), d = diffuse.Gray(card.x, y, 4), s = specular.Gray(card.x, y, 4);
            char text[256];
            std::snprintf(text, sizeof(text), "%s card, row %d: diffuse %.4f + specular %.4f = full %.4f", card.name,
                          y, d, s, f);
            Check(f > 0.01f && std::abs(d + s - f) <= 0.04f * f, text);
            if (std::string(card.name) == "matte")
                Check(s < 1e-5f, Describe("the matte card has no glossy lobe: specular-only image", s));
            if (std::string(card.name) == "metal")
                Check(d < 0.02f * f, Describe("the metal card has no diffuse lobe: diffuse-only image", d));
            if (std::string(card.name) == "plastic")
                Check(d > 0.05f * f && s > 0.05f * f, Describe("the plastic card has both lobes (specular)", s));
        }
    }
    // The camera sees the dome unscaled.
    Check(std::abs(diffuse.Gray(32, 3) - full.Gray(32, 3)) < 0.01f * full.Gray(32, 3) &&
              std::abs(specular.Gray(32, 3) - full.Gray(32, 3)) < 0.01f * full.Gray(32, 3),
          Describe("the camera sees the dome unscaled", full.Gray(32, 3)));
}

// double_sided.usda (prism:honorDoubleSided on): cards at columns 7, 19, 32,
// 45, 57 (row 32) in front of a blue backdrop.
void DoubleSided(Image const& image)
{
    const float backdropR = image.Mean(32, 6, 0), backdropB = image.Mean(32, 6, 2);
    Check(backdropB > 0.4f && backdropR < 0.2f, Describe("the backdrop is blue", backdropB));
    struct Card {
        char const* name;
        int x;
        bool drawn;
    };
    const Card cards[] = {{"single-sided card facing away is culled", 7, false},
                          {"doubleSided card facing away is drawn", 19, true},
                          {"single-sided card facing the camera is drawn", 32, true},
                          {"mirrored single-sided card is culled", 57, false}};
    for (auto const& card : cards) {
        const float r = image.Mean(card.x, 32, 0), b = image.Mean(card.x, 32, 2);
        Check(card.drawn ? (r > 0.4f && b < 0.2f) : (b > 0.7f * backdropB && r < 0.2f),
              Describe(card.name, card.drawn ? r : b));
    }
    const float glassB = image.Mean(45, 32, 2);
    Check(glassB < 0.6f * backdropB, Describe("the transmissive card facing away is drawn (tints the backdrop)", glassB));
}

// instance_primvars.usda: cards whose material reads the geompropvalue
// "tint" (default grey): PointInstancer cards at (15, 21), (32, 21), (49, 21),
// native instances at (24, 43) and (40, 43).
void InstancePrimvars(Image const& image)
{
    struct Expected {
        char const* what;
        int x, y;
        float r, g, b;
    };
    const Expected expected[] = {{"PointInstancer red", 15, 21, 0.8f, 0.1f, 0.1f},
                                 {"PointInstancer green", 32, 21, 0.1f, 0.8f, 0.1f},
                                 {"PointInstancer blue", 49, 21, 0.1f, 0.1f, 0.8f},
                                 {"native instance yellow", 24, 43, 0.8f, 0.8f, 0.1f},
                                 {"native instance cyan", 40, 43, 0.1f, 0.8f, 0.8f}};
    for (auto const& e : expected) {
        const float r = image.Mean(e.x, e.y, 0), g = image.Mean(e.x, e.y, 1), b = image.Mean(e.x, e.y, 2);
        const float error = std::max({std::abs(r - e.r), std::abs(g - e.g), std::abs(b - e.b)});
        char text[256];
        std::snprintf(text, sizeof(text), "%s: (%.3f, %.3f, %.3f)", e.what, r, g, b);
        Check(error <= 0.1f, text);
    }
}

// A light_lobes variant rendered with MIS (next-event estimation splits the
// BSDF evaluation by lobe) and with BSDF sampling only (light hits are scaled
// by the sampled direction's diffuse fraction): the card means agree (the
// two estimators of each lobe are unbiased). Any resolution.
void LobesConsistency(std::vector<Image> const& images)
{
    Image const& mis = images[0];
    Image const& bsdf = images[1];
    const float columns[] = {11.0f / 64.0f, 32.0f / 64.0f, 53.0f / 64.0f};
    char const* names[] = {"matte", "plastic", "metal"};
    for (int i = 0; i < 3; ++i) {
        const int x = int(columns[i] * float(mis.width)), y = mis.height / 2;
        const int r = std::max(1, mis.width * 5 / 64);
        const float a = mis.Gray(x, y, r), b = bsdf.Gray(x, y, r);
        char text[256];
        std::snprintf(text, sizeof(text), "%s card: MIS %.4f, BSDF sampling %.4f", names[i], a, b);
        Check(std::abs(a - b) <= 0.03f * std::max(a, b) + 1e-4f, text);
    }
}

// sss_fallback.usda, 64x64: the sphere of sss_furnace.usda covers the centre.
// Traced, it looks like the white dome; with the fallback its base (weight 0)
// leaves it black against the dome.
void SssFallback(Image const& image)
{
    Check(image.Gray(32, 32, 2) < 0.05f, Describe("the sphere renders black", image.Gray(32, 32, 2)));
    Check(image.Gray(2, 2, 1) > 0.5f, Describe("the dome is seen", image.Gray(2, 2, 1)));
}

// glass_furnace.usda, 64x64: the glass sphere and the transparent card look
// like the white dome: no energy gained or lost, on average or in any pixel.
// (Before DocumentLoader coupled OpenPBR's reflection and transmission, MDL's
// custom-curve layer lost light leaving the sphere: mean 0.987, the sphere
// 3% dark; docs/materialx_compatibility.md.) The only loss left is paths
// still inside the sphere at the depth limit. That delta paths are not MIS
// weighted is checked by comparing MIS with BSDF-only renders
// (prism_scene_features_glass_furnace_sampling).
void GlassFurnace(Image const& image)
{
    double sum = 0.0, squares = 0.0, worst = 0.0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            for (int c = 0; c < 3; ++c) {
                const double v = image.At(x, y, c);
                sum += v;
                squares += (v - 1.0) * (v - 1.0);
                worst = std::max(worst, std::abs(v - 1.0));
            }
    const double count = 3.0 * image.width * image.height;
    const float mean = float(sum / count), rms = float(std::sqrt(squares / count));
    Check(std::abs(mean - 1.0f) <= 0.001f, Describe("the image is the dome on average", mean));
    Check(rms <= 0.002f, Describe("the RMS difference from the dome", rms));
    Check(worst <= 0.02, Describe("the largest pixel difference from the dome", float(worst)));
}

} // namespace

int main(int argc, char** argv)
{
    using Single = std::function<void(Image const&)>;
    auto single = [](Single check) {
        return std::make_pair(1, std::function<void(std::vector<Image> const&)>(
            [check](std::vector<Image> const& images) { check(images[0]); }));
    };
    // Each check and the number of images it takes.
    std::map<std::string, std::pair<int, std::function<void(std::vector<Image> const&)>>> const checks = {
        {"camera_visibility", single(CameraVisibility)},
        {"dome_temperature", single(DomeTemperature)},
        {"invisible_ids", single(InvisibleIds)},
        {"display_color", single(DisplayColor)},
        {"light_lobes", {3, LightLobes}},
        {"double_sided", single(DoubleSided)},
        {"instance_primvars", single(InstancePrimvars)},
        {"sss_fallback", single(SssFallback)},
        {"glass_furnace", single(GlassFurnace)},
        // Diagnostics: the pixels where two images differ most.
        {"diff", {2, [](std::vector<Image> const& images) {
            std::vector<std::pair<float, int>> d;
            for (int y = 0; y < images[0].height; ++y)
                for (int x = 0; x < images[0].width; ++x) {
                    float e = 0.0f;
                    for (int c = 0; c < 3; ++c) e += std::abs(images[0].At(x, y, c) - images[1].At(x, y, c));
                    d.push_back({e, y * images[0].width + x});
                }
            std::sort(d.rbegin(), d.rend());
            for (size_t i = 0; i < std::min<size_t>(12, d.size()); ++i)
                std::printf("(%d, %d): %g\n", d[i].second % images[0].width, d[i].second / images[0].width, d[i].first);
        }}},
        {"lobes_consistency", {2, LobesConsistency}},
    };
    if (argc < 3 || !checks.contains(argv[1]) || argc - 2 != checks.at(argv[1]).first) {
        std::printf("usage: prism_scene_features_test FEATURE IMAGE.exr...\nfeatures:");
        for (auto const& [name, check] : checks) std::printf(" %s (%d)", name.c_str(), check.first);
        std::printf("\n");
        return 2;
    }
    std::vector<Image> images(size_t(argc - 2));
    for (int i = 2; i < argc; ++i) {
        if (!images[size_t(i - 2)].Load(argv[i])) {
            std::printf("FAIL: cannot read %s\n", argv[i]);
            return 1;
        }
    }
    checks.at(argv[1]).second(images);
    return failures == 0 ? 0 : 1;
}
