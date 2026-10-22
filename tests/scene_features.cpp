// UsdLux and geometry features of both USD loaders (docs/hydra.md, "Scene
// features"), checked on renders of their fixtures in tests/hydra/fixtures.
// The parity harness renders each fixture with the CLI and the Hydra
// delegate; this checks that each render shows the feature.
//
// prism_scene_features_test FEATURE IMAGE.exr
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

} // namespace

int main(int argc, char** argv)
{
    std::map<std::string, std::function<void(Image const&)>> const checks = {
        {"camera_visibility", CameraVisibility},
        {"dome_temperature", DomeTemperature},
        {"invisible_ids", InvisibleIds},
        {"display_color", DisplayColor},
    };
    if (argc != 3 || !checks.contains(argv[1])) {
        std::printf("usage: prism_scene_features_test FEATURE IMAGE.exr\nfeatures:");
        for (auto const& [name, check] : checks) std::printf(" %s", name.c_str());
        std::printf("\n");
        return 2;
    }
    Image image;
    if (!image.Load(argv[2])) {
        std::printf("FAIL: cannot read %s\n", argv[2]);
        return 1;
    }
    checks.at(argv[1])(image);
    return failures == 0 ? 0 : 1;
}
