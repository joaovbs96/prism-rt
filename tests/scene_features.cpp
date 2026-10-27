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
//   textured_emitter   textured_emitter.usda: a scene lit only by a textured
//                      emitter gets no default dome and is lit by it.
//   textured_emitter_consistency  MIS BSDF: the fixture with next-event
//                      estimation and with BSDF sampling only agrees on the floor.
//   sss_fallback       sss_fallback.usda: prism:sssFallback renders the
//                      furnace's subsurface-only sphere by its (black)
//                      diffuse lobe instead of tracing the walk.
//   light_aovs         light_aovs.usda (light_aovs.exr): diffuse + specular +
//                      emission is the colour in every pixel; each lobe shows
//                      where it should; Position holds world positions.
//   light_aovs_sum     any render with Diffuse, Specular and Emission layers:
//                      their sum is the colour.
//   light_aovs_albedo  light_aovs_albedo.usda: the albedo through the glass
//                      and the mirror sphere is the walls' colour, that of
//                      the diffuse sphere its own.
//   dome_orientation   NEGZ POSZ: dome_orientation.usda from its two
//                      cameras: the dome follows UsdLux's lat-long
//                      convention, and its sun lights the ball's +X side.
//   dome_orientation_consistency  MIS BSDF: CameraNegZ's render with the
//                      dome importance sampled and with BSDF sampling only
//                      agrees on the ball.
//   rect_light_texture rect_light_texture.usda: a camera-visible textured
//                      rect light shows its image upright and unmirrored.
//   exr_parts          LAYERS PARTS: the multi-part EXR PARTS holds a named
//                      part per layer of the single-part LAYERS, with the
//                      same channels and pixels.

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imageio.h>

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
    std::string path;
    std::vector<std::string> names;

    bool Load(std::string const& file)
    {
        path = file;
        OIIO::ImageBuf buffer(path);
        if (!buffer.read(0, 0, true, OIIO::TypeDesc::FLOAT)) return false;
        auto const& spec = buffer.spec();
        width = spec.width;
        height = spec.height;
        channels = spec.nchannels;
        names = spec.channelnames;
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

    // The index of the named channel, -1 when there is none.
    int Channel(std::string const& name) const
    {
        auto found = std::find(names.begin(), names.end(), name);
        return found == names.end() ? -1 : int(found - names.begin());
    }
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

// textured_emitter.usda, 64x64: the floor is lit by the textured emitter
// only. The sky stays black (no default dome: the scene's emitter counts as
// a light), the emitter's checker is seen above the floor.
void TexturedEmitter(Image const& image)
{
    float sky = 0.0f;
    for (int y = 0; y < 6; ++y)
        for (int x = 0; x < image.width; ++x)
            for (int c = 0; c < 3; ++c) sky = std::max(sky, image.At(x, y, c));
    Check(sky == 0.0f, Describe("the sky is black (no default dome)", sky));
    float emitter = 0.0f;
    for (int y = 6; y < image.height / 2; ++y)
        for (int x = 0; x < image.width; ++x) emitter = std::max(emitter, image.At(x, y, 1));
    Check(emitter > 1.0f, Describe("the emitter is seen", emitter));
    Check(image.Gray(32, 44, 3) > 0.01f, Describe("the emitter lights the floor", image.Gray(32, 44, 3)));
}

// Horizontal bands of the floor (rows 30 to 57 of 64) with next-event
// estimation and with BSDF sampling only (a high sample count): the light
// sample's emission and densities are unbiased when they agree.
void TexturedEmitterConsistency(std::vector<Image> const& images)
{
    Image const& mis = images[0];
    Image const& bsdf = images[1];
    for (int band = 0; band < 3; ++band) {
        const int y0 = mis.height * (30 + 9 * band) / 64, y1 = mis.height * (39 + 9 * band) / 64;
        double a = 0.0, b = 0.0;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < mis.width; ++x)
                for (int c = 0; c < 3; ++c) {
                    a += mis.At(x, y, c);
                    b += bsdf.At(x, y, c);
                }
        char text[256];
        std::snprintf(text, sizeof(text), "floor rows %d-%d: MIS %.6f, BSDF sampling %.6f", y0, y1, a, b);
        Check(std::abs(a - b) <= 0.02 * std::max(a, b) + 1e-6, text);
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

// dome_orientation.usda: the colour the 120-degree camera sees in the
// camera-space direction (x, y, z) (z < 0 ahead), as the mean of a 3x3 box.
void DomeColor(Image const& image, float x, float y, float z, float rgb[3])
{
    const float tanHalf = std::tan(60.0f * 3.14159265f / 180.0f);
    const int px = int(std::lround((x / -z / tanHalf + 1.0f) * 0.5f * image.width - 0.5f));
    const int py = int(std::lround((1.0f - y / -z / tanHalf) * 0.5f * image.height - 0.5f));
    for (int c = 0; c < 3; ++c) rgb[c] = image.Mean(px, py, c, 1);
}

// The dome band `name` with colour `expected` is seen in direction (x, y, z).
void DomeBand(Image const& image, char const* camera, char const* name, float x, float y, float z,
              float const (&expected)[3])
{
    float rgb[3];
    DomeColor(image, x, y, z, rgb);
    bool ok = true;
    for (int c = 0; c < 3; ++c) ok = ok && std::abs(rgb[c] - expected[c]) <= 0.1f;
    char text[256];
    std::snprintf(text, sizeof(text), "%s sees %s toward (%.2f, %.2f, %.2f): (%.3f, %.3f, %.3f)", camera, name,
                  x, y, z, rgb[0], rgb[1], rgb[2]);
    Check(ok, text);
}

// dome_orientation.usda, the renders of /World/CameraNegZ and
// /World/CameraPosZ: UsdLux's lat-long convention (-Z red, +X green, +Z
// blue, -X yellow, cyan above 45 degrees), and the sun toward +X lights the
// ball's +X side.
void DomeOrientation(std::vector<Image> const& images)
{
    static const float red[3] = {0.5f, 0, 0}, green[3] = {0, 0.5f, 0}, blue[3] = {0, 0, 0.5f},
                       yellow[3] = {0.5f, 0.5f, 0}, cyan[3] = {0, 0.5f, 0.5f};
    const float s = std::sin(0.96f), c = std::cos(0.96f); // 55 degrees off the view axis
    Image const& negZ = images[0];
    // CameraNegZ: camera space is world space.
    DomeBand(negZ, "CameraNegZ", "-Z (red)", 0, 0.15f, -1, red);
    DomeBand(negZ, "CameraNegZ", "+X (green) on its right", s, 0.15f, -c, green);
    DomeBand(negZ, "CameraNegZ", "-X (yellow) on its left", -s, 0.15f, -c, yellow);
    DomeBand(negZ, "CameraNegZ", "+Y (cyan) above", 0, 1.43f, -1, cyan);
    // CameraPosZ looks down +Z: its right is world -X.
    Image const& posZ = images[1];
    DomeBand(posZ, "CameraPosZ", "+Z (blue)", 0, 0.15f, -1, blue);
    DomeBand(posZ, "CameraPosZ", "-X (yellow) on its right", s, 0.15f, -c, yellow);
    DomeBand(posZ, "CameraPosZ", "+X (green) on its left", -s, 0.15f, -c, green);
    DomeBand(posZ, "CameraPosZ", "+Y (cyan) above", 0, 1.43f, -1, cyan);
    // The ball (centre (0, -1.2, -2), radius 0.6) seen from the origin: the
    // points with normals (+-0.75, 0, 0.66).
    float lit[3], unlit[3];
    DomeColor(negZ, 0.45f, -1.2f, -2 + 0.396f, lit);
    DomeColor(negZ, -0.45f, -1.2f, -2 + 0.396f, unlit);
    const float a = (lit[0] + lit[1] + lit[2]) / 3, b = (unlit[0] + unlit[1] + unlit[2]) / 3;
    char text[256];
    std::snprintf(text, sizeof(text), "the sun lights the ball's +X side: %.3f against %.3f on its -X side", a, b);
    Check(a > 3.0f * b && a > 0.3f, text);
}

// dome_orientation.usda from CameraNegZ with the dome importance sampled
// (MIS) and with BSDF sampling only: the ball, lit mostly by the small sun,
// is as bright in both. A sampled direction that disagreed with the lookup's
// texel would bias the first.
void DomeOrientationConsistency(std::vector<Image> const& images)
{
    Image const& mis = images[0];
    Image const& bsdf = images[1];
    // The ball's box: centre (0, -1.2, -2) seen 31 degrees below the axis.
    const int x0 = mis.width * 10 / 32, x1 = mis.width * 22 / 32;
    const int y0 = mis.height * 16 / 32, y1 = mis.height * 28 / 32;
    double a = 0.0, b = 0.0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c) {
                a += mis.At(x, y, c);
                b += bsdf.At(x, y, c);
            }
    char text[256];
    std::snprintf(text, sizeof(text), "the ball: MIS %.4f, BSDF sampling %.4f", a, b);
    Check(std::abs(a - b) <= 0.02 * std::max(a, b), text);
}

// rect_light_texture.usda, 64x64, linear Rec.709: the camera-visible rect
// light shows its texture upright and unmirrored, each quadrant's colour at
// its centre (the quadrant centres are 10.7 pixels from the image centre).
void RectLightTexture(Image const& image)
{
    struct Quadrant { const char* name; int x, y; float r, g, b; };
    const int lo = image.width * 21 / 64, hi = image.width * 42 / 64;
    const Quadrant quadrants[] = {
        {"top left (red)", lo, lo, 1, 0, 0},
        {"top right (green)", hi, lo, 0, 1, 0},
        {"bottom left (blue)", lo, hi, 0, 0, 1},
        {"bottom right (white)", hi, hi, 1, 1, 1},
    };
    for (auto const& q : quadrants) {
        const float r = image.Mean(q.x, q.y, 0), g = image.Mean(q.x, q.y, 1), b = image.Mean(q.x, q.y, 2);
        char text[256];
        std::snprintf(text, sizeof(text), "the texture's %s quadrant is seen there: (%.3f, %.3f, %.3f)", q.name, r,
                      g, b);
        Check(std::abs(r - q.r) < 0.05f && std::abs(g - q.g) < 0.05f && std::abs(b - q.b) < 0.05f, text);
    }
}

// The three channels <layer>.R/G/B (R, G, B for an empty layer); false if
// one is missing.
bool LayerChannels(Image const& image, std::string const& layer, int* indices, char const* const* suffixes = nullptr)
{
    static char const* const rgb[] = {"R", "G", "B"};
    if (!suffixes) suffixes = rgb;
    for (int c = 0; c < 3; ++c) {
        indices[c] = image.Channel(layer.empty() ? std::string(suffixes[c]) : layer + "." + suffixes[c]);
        if (indices[c] < 0) {
            Check(false, image.path + " has the channels of " + (layer.empty() ? std::string("the colour") : layer));
            return false;
        }
    }
    return true;
}

// Mean of a layer's channel c over the (2r+1)^2 box around (x, y).
float LayerMean(Image const& image, int const* channels, int x, int y, int c, int r)
{
    return image.Mean(x, y, channels[c], r);
}

// diffuse + specular + emission against the colour (relative to it, or to a
// hundredth of the mean where it is darker), and no negative AOV.
void LightAovSum(Image const& image, int const* color, int const* albedo, int const* diffuse,
                 int const* specular, int const* emission)
{
    double mean = 0.0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            for (int c = 0; c < 3; ++c) mean += image.At(x, y, color[c]);
    mean /= 3.0 * image.width * image.height;
    double worst = 0.0, negative = 0.0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
            for (int c = 0; c < 3; ++c) {
                const double value = image.At(x, y, color[c]);
                const double sum = double(image.At(x, y, diffuse[c])) + image.At(x, y, specular[c]) +
                    image.At(x, y, emission[c]);
                worst = std::max(worst, std::abs(sum - value) / std::max(std::abs(value), 0.01 * mean));
                for (int const* layer : {diffuse, specular, emission, albedo})
                    negative = std::min(negative, double(image.At(x, y, layer[c])));
            }
    Check(worst <= 1e-4, Describe("diffuse + specular + emission is the colour (largest relative difference)",
                                  float(worst)));
    Check(negative >= -1e-5, Describe("no AOV is negative (smallest value)", float(negative)));
}

void LightAovsSum(Image const& image)
{
    int color[3], albedo[3], diffuse[3], specular[3], emission[3];
    if (!LayerChannels(image, "", color) || !LayerChannels(image, "Albedo", albedo) ||
        !LayerChannels(image, "Diffuse", diffuse) || !LayerChannels(image, "Specular", specular) ||
        !LayerChannels(image, "Emission", emission))
        return;
    LightAovSum(image, color, albedo, diffuse, specular, emission);
}

// light_aovs.usda, 96x64.
void LightAovs(Image const& image)
{
    int color[3], albedo[3], diffuse[3], specular[3], emission[3], position[3];
    static char const* const xyz[] = {"X", "Y", "Z"};
    if (!LayerChannels(image, "", color) || !LayerChannels(image, "Albedo", albedo) ||
        !LayerChannels(image, "Diffuse", diffuse) || !LayerChannels(image, "Specular", specular) ||
        !LayerChannels(image, "Emission", emission) || !LayerChannels(image, "Position", position, xyz))
        return;
    LightAovSum(image, color, albedo, diffuse, specular, emission);
    auto gray = [&](int const* layer, int x, int y) {
        return (LayerMean(image, layer, x, y, 0, 1) + LayerMean(image, layer, x, y, 1, 1) +
                LayerMean(image, layer, x, y, 2, 1)) / 3.0f;
    };
    // The dome (top corner) and the panel are seen directly: emission only.
    Check(gray(emission, 3, 3) > 0.3f && gray(diffuse, 3, 3) == 0.0f && gray(specular, 3, 3) == 0.0f,
          Describe("the dome is emission", gray(emission, 3, 3)));
    // Floor (bottom centre): mostly diffuse, no emission.
    Check(gray(diffuse, 48, 60) > 0.05f && gray(emission, 48, 60) == 0.0f,
          Describe("the floor is diffuse", gray(diffuse, 48, 60)));
    Check(gray(specular, 48, 60) < 0.1f * gray(diffuse, 48, 60),
          Describe("the floor has no glossy lobe (specular / diffuse)",
                   gray(specular, 48, 60) / std::max(1e-6f, gray(diffuse, 48, 60))));
    // A Lambertian sample weighs its colour exactly (in the working space).
    float spread = 0.0f;
    for (int c = 0; c < 3; ++c)
        spread = std::max(spread, std::abs(image.At(47, 60, albedo[c]) - image.At(49, 59, albedo[c])));
    Check(spread < 1e-5f && LayerMean(image, albedo, 48, 60, 0, 1) > LayerMean(image, albedo, 48, 60, 1, 1) &&
              LayerMean(image, albedo, 48, 60, 1, 1) > LayerMean(image, albedo, 48, 60, 2, 1),
          Describe("the floor's albedo is its (orange) colour, the same in every pixel (spread)", spread));
    // Find the spheres' pixels from the world positions (centre rows).
    auto nearest = [&](float px, float py, float pz, float radius) {
        int best = -1;
        float distance = 1e9f;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x) {
                const float dx = image.At(x, y, position[0]) - px, dy = image.At(x, y, position[1]) - py,
                    dz = image.At(x, y, position[2]) - pz;
                const float d = std::abs(std::sqrt(dx * dx + dy * dy + dz * dz) - radius);
                // The point of the sphere closest to the camera, roughly.
                if (d < 1e-3f && image.At(x, y, position[2]) > pz + 0.5f * radius) {
                    const float off = std::abs(image.At(x, y, position[0]) - px) + std::abs(image.At(x, y, position[1]) - py);
                    if (off < distance) { distance = off; best = y * image.width + x; }
                }
            }
        return best;
    };
    const int metal = nearest(-1.6f, 0.6f, 0.0f, 0.6f);
    const int glass = nearest(0.0f, 0.6f, 0.0f, 0.6f);
    const int plastic = nearest(1.6f, 0.6f, 0.0f, 0.6f);
    Check(metal >= 0 && glass >= 0 && plastic >= 0, "the world positions put the spheres where they are");
    if (metal < 0 || glass < 0 || plastic < 0) return;
    auto at = [&](int pixel, int const* layer) { return gray(layer, pixel % image.width, pixel / image.width); };
    Check(at(metal, diffuse) < 0.02f * at(metal, specular), Describe("the metal is specular (diffuse / specular)",
          at(metal, diffuse) / std::max(1e-6f, at(metal, specular))));
    Check(at(glass, specular) > 0.05f && at(glass, diffuse) < 0.05f * at(glass, specular),
          Describe("the glass is specular (diffuse / specular)", at(glass, diffuse) / std::max(1e-6f, at(glass, specular))));
    Check(at(plastic, diffuse) > 0.02f && at(plastic, specular) > 0.002f,
          Describe("the plastic is diffuse and specular (diffuse)", at(plastic, diffuse)));
    // The metal's albedo is its colour: red above blue.
    Check(LayerMean(image, albedo, metal % image.width, metal / image.width, 0, 1) >
              1.5f * LayerMean(image, albedo, metal % image.width, metal / image.width, 2, 1),
          Describe("the metal's albedo is gold (red)", LayerMean(image, albedo, metal % image.width, metal / image.width, 0, 1)));
}

// light_aovs_albedo.usda, 96x64: the spheres' centres project to (13, 32),
// (48, 32) and (83, 32); a corner shows a wall.
void LightAovsAlbedo(Image const& image)
{
    int albedo[3];
    if (!LayerChannels(image, "Albedo", albedo)) return;
    // The walls' colour in the working space, as a corner shows it.
    float wall[3], sphere[3];
    for (int c = 0; c < 3; ++c) {
        wall[c] = LayerMean(image, albedo, 3, 3, c, 1);
        sphere[c] = image.At(48, 32, albedo[c]);
    }
    Check(wall[0] > wall[1] && wall[1] > wall[2] && wall[2] > 0.0f, Describe("the walls are orange (red)", wall[0]));
    Check(sphere[1] > 1.5f * sphere[0] && sphere[1] > 1.5f * sphere[2],
          Describe("the diffuse sphere is green (green)", sphere[1]));
    auto expect = [&](char const* what, int x, int y, float const* colour, float tolerance) {
        float worst = 0.0f;
        for (int c = 0; c < 3; ++c)
            worst = std::max(worst, std::abs(LayerMean(image, albedo, x, y, c, 1) / colour[c] - 1.0f));
        Check(worst <= tolerance, Describe(what, worst));
        std::printf("  %s: albedo (%g, %g, %g)\n", what, LayerMean(image, albedo, x, y, 0, 1),
                    LayerMean(image, albedo, x, y, 1, 1), LayerMean(image, albedo, x, y, 2, 1));
    };
    expect("another wall's albedo is the same (largest relative difference)", 92, 60, wall, 1e-4f);
    expect("the diffuse sphere's albedo is its colour in every pixel", 48, 32, sphere, 1e-4f);
    expect("the albedo through the glass is the walls' colour", 13, 32, wall, 0.03f);
    expect("the albedo in the mirror is the walls' colour", 83, 32, wall, 0.03f);
}

// The parts of a multi-part EXR against the layers of a single-part one.
void ExrParts(std::vector<Image> const& images)
{
    Image const& layers = images[0];
    std::string path = images[1].path;
    auto input = OIIO::ImageInput::open(path);
    if (!input) {
        Check(false, "cannot open " + path);
        return;
    }
    int parts = 0;
    size_t channels = 0;
    bool same = true;
    std::vector<std::string> partNames;
    for (int part = 0; input->seek_subimage(part, 0); ++part) {
        OIIO::ImageSpec const& spec = input->spec();
        ++parts;
        partNames.push_back(spec.get_string_attribute("oiio:subimagename"));
        std::vector<float> pixels(size_t(spec.width) * spec.height * spec.nchannels);
        if (spec.width != layers.width || spec.height != layers.height ||
            !input->read_image(part, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, pixels.data())) {
            same = false;
            continue;
        }
        for (int c = 0; c < spec.nchannels; ++c) {
            const int source = layers.Channel(spec.channelnames[c]);
            if (source < 0) { same = false; continue; }
            ++channels;
            for (int y = 0; y < spec.height; ++y)
                for (int x = 0; x < spec.width; ++x)
                    same = same && pixels[(size_t(y) * spec.width + x) * spec.nchannels + c] == layers.At(x, y, source);
        }
    }
    std::string names;
    for (auto const& name : partNames) names += (names.empty() ? "" : ", ") + name;
    std::printf("  parts: %s\n", names.c_str());
    Check(parts == 6 && partNames.front() == "Color" && partNames.back() == "Position",
          Describe("a named part per var", float(parts)));
    Check(channels == size_t(layers.channels), Describe("the parts hold every channel", float(channels)));
    Check(same, "the parts' pixels are the layers'");
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
        {"dome_orientation", {2, DomeOrientation}},
        {"dome_orientation_consistency", {2, DomeOrientationConsistency}},
        {"rect_light_texture", single(RectLightTexture)},
        {"textured_emitter", single(TexturedEmitter)},
        {"textured_emitter_consistency", {2, TexturedEmitterConsistency}},
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
        {"light_aovs", single(LightAovs)},
        {"light_aovs_sum", single(LightAovsSum)},
        {"light_aovs_albedo", single(LightAovsAlbedo)},
        {"exr_parts", {2, ExrParts}},
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
