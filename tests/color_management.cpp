// Colour management (src/color, docs/color_management.md).
//
// Without arguments: the working-space matrices and luminance weights, the
// views, texture conversion (and its cache identity), light colours, and the
// MaterialX colour transforms the MDL generator emits for each working space.
//
// --check-patches IMAGE.exr WORKING_SPACE: a render of
// tests/hydra/fixtures/color_management.usda holds every tagged colour
// converted exactly once (expected values from constants, not from OCIO).
//
// --check-view IMAGE.exr IMAGE.png WORKING_SPACE DISPLAY VIEW: the 8-bit
// output is the linear output through the view, applied once.

#include "../src/color/color_management.h"
#include "../src/lights.h"
#include "../src/materialx/mdl_generator.h"
#include "../src/texture.h"

#include <MaterialXFormat/XmlIo.h>
#include <OpenImageIO/imagebuf.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

int failures = 0;

void Check(bool ok, std::string const& what)
{
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance * std::max(1.0f, std::abs(b)); }
bool Near(Float3 a, Float3 b, float tolerance)
{
    return Near(a.x(), b.x(), tolerance) && Near(a.y(), b.y(), tolerance) && Near(a.z(), b.z(), tolerance);
}
std::string Text(Float3 c)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.6f, %.6f, %.6f)", c.x(), c.y(), c.z());
    return buffer;
}

// Linear Rec.709 to ACEScg (Bradford adaptation D65 to the ACES white), the
// matrix of the ACES studio config.
constexpr prism::color::Matrix3 kRec709ToAcescg{
    0.6130974f, 0.3395231f, 0.0473795f,
    0.0701937f, 0.9163539f, 0.0134524f,
    0.0206156f, 0.1095698f, 0.8698151f};

float SrgbDecode(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
Float3 SrgbDecode(Float3 c) { return Float3(SrgbDecode(c.x()), SrgbDecode(c.y()), SrgbDecode(c.z())); }

prism::color::Options Legacy()
{
    return {"", "lin_rec709", "Gamma 2.2 Rec.709 - Display", "Un-tone-mapped"};
}

void TestMatrices()
{
    auto aces = prism::color::ColorManagement::Create({});
    Check(aces->workingSpace() == "ACEScg", "default working space is ACEScg, got " + aces->workingSpace());
    Check(aces->materialXSpace() == "acescg", "ACEScg's MaterialX name");
    Check(aces->display() == "sRGB - Display", "default display: " + aces->display());
    Check(aces->view() == "ACES 2.0 - SDR 100 nits (Rec.709)", "default view: " + aces->view());
    Check(!aces->rec709(), "ACEScg converts");
    auto const& m = aces->fromRec709();
    for (int i = 0; i < 9; ++i)
        Check(Near(m[i], kRec709ToAcescg[i], 2e-5f), "Rec.709 to ACEScg matrix entry " + std::to_string(i));
    for (int row = 0; row < 3; ++row)
        Check(Near(m[row * 3] + m[row * 3 + 1] + m[row * 3 + 2], 1.0f, 1e-5f), "white stays white");
    const Float3 probe(0.3f, 0.6f, 0.9f);
    Check(Near(prism::color::Apply(aces->toRec709(), aces->FromRec709(probe)), probe, 1e-5f), "inverse matrix");
    // The same colour has the same luminance in either space.
    for (Float3 c : {Float3(1, 0, 0), Float3(0, 1, 0), Float3(0, 0, 1), Float3(0.2f, 0.5f, 0.8f)}) {
        const float y709 = 0.2126f * c.x() + 0.7152f * c.y() + 0.0722f * c.z();
        Check(Near(aces->Luminance(aces->FromRec709(c)), y709, 1e-5f), "luminance of " + Text(c));
    }
    auto const& w = aces->luminanceWeights();
    Check(Near(w[0] + w[1] + w[2], 1.0f, 1e-5f), "ACEScg luminance weights sum to 1");
    Check(Near(w[0], 0.2676f, 1e-3f) && Near(w[1], 0.6744f, 1e-3f) && Near(w[2], 0.0580f, 2e-3f),
          "ACEScg luminance weights " + Text(Float3(w[0], w[1], w[2])));

    auto legacy = prism::color::ColorManagement::Create(Legacy());
    Check(legacy->rec709() && legacy->workingSpace() == "Linear Rec.709 (sRGB)", "lin_rec709 working space");
    Check(legacy->luminanceWeights() == prism::color::kRec709Luminance, "Rec.709 keeps its exact luminance weights");
    Check(legacy->materialXSpace() == "lin_rec709", "Rec.709's MaterialX name");
    const Float3 c(0.123f, 4.5f, -0.25f);
    const Float3 same = legacy->FromRec709(c);
    Check(same.x() == c.x() && same.y() == c.y() && same.z() == c.z(), "Rec.709 colours are unchanged");

    Check(aces->Resolve("acescg") == "ACEScg" && aces->Resolve("lin_rec709") == "Linear Rec.709 (sRGB)" &&
              aces->Resolve("srgb_texture") == "sRGB Encoded Rec.709 (sRGB)" && aces->Resolve("bogus").empty(),
          "colour space names resolve");
    Check(!aces->NeedsConversion("raw") && !aces->NeedsConversion("acescg") && !aces->NeedsConversion("") &&
              aces->NeedsConversion("lin_rec709") && aces->NeedsConversion("srgb_texture"),
          "which spaces convert into ACEScg");
    Check(!legacy->NeedsConversion("lin_rec709") && legacy->NeedsConversion("srgb_texture"),
          "which spaces convert into Rec.709");

    bool threw = false;
    try { prism::color::ColorManagement::Create({"", "srgb_texture", "", ""}); } catch (std::exception const&) { threw = true; }
    Check(threw, "a non-linear working space is refused");
    threw = false;
    try { prism::color::ColorManagement::Create({"", "", "", "No such view"}); } catch (std::exception const&) { threw = true; }
    Check(threw, "an unknown view is refused");
}

void TestViews()
{
    auto legacy = prism::color::ColorManagement::Create(Legacy());
    float pixels[] = {-0.5f, 0.0f, 0.0031f, 0.18f, 0.5f, 0.999f, 1.0f, 3.0f, 0.04f};
    float expected[9];
    for (int i = 0; i < 9; ++i) expected[i] = std::pow(std::clamp(pixels[i], 0.0f, 1.0f), 1.0f / 2.2f);
    legacy->ApplyView(pixels, 3, 3);
    for (int i = 0; i < 9; ++i)
        Check(Near(pixels[i], expected[i], 2e-6f), "gamma 2.2 view of value " + std::to_string(i) +
              ": " + std::to_string(pixels[i]) + " vs " + std::to_string(expected[i]));

    auto aces = prism::color::ColorManagement::Create({});
    float grey[] = {0.0f, 0.0f, 0.0f, 0.18f, 0.18f, 0.18f, 1.0f, 1.0f, 1.0f, 100.0f, 100.0f, 100.0f};
    aces->ApplyView(grey, 4, 3);
    Check(grey[0] == 0.0f, "ACES view of black");
    Check(Near(grey[3], 0.3492f, 2e-3f), "ACES 2.0 SDR view of 18% grey: " + std::to_string(grey[3]));
    Check(grey[3] < grey[6] && grey[6] < grey[9] && grey[9] <= 1.0f, "ACES view is monotonic and bounded");
    Check(grey[6] < 0.8f, "the ACES view tone maps (1.0 is not white): " + std::to_string(grey[6]));
}

void TestConversions()
{
    auto aces = prism::color::ColorManagement::Create({});
    float rgb[] = {0.5f, 0.2f, 0.1f};
    aces->ToWorkingSpace("srgb_texture", rgb, 1, 3);
    const Float3 expected = prism::color::Apply(kRec709ToAcescg, SrgbDecode(Float3(0.5f, 0.2f, 0.1f)));
    Check(Near(Float3(rgb[0], rgb[1], rgb[2]), expected, 2e-4f), "sRGB texture to ACEScg " + Text(Float3(rgb[0], rgb[1], rgb[2])));

    // Light colours (both loaders build lights with MakeSceneLight).
    prism::color::SetActive(aces);
    prism::LightParameters params;
    params.shape = prism::LightShape::Distant;
    params.color = Color3(1.0f, 0.0f, 0.0f);
    const auto light = prism::MakeSceneLight(params, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    Check(Near(light.radiance, Float3(kRec709ToAcescg[0], kRec709ToAcescg[3], kRec709ToAcescg[6]), 1e-5f),
          "light colour converts from Rec.709: " + Text(light.radiance));
    prism::color::SetActive(prism::color::ColorManagement::Create(Legacy()));
    const auto legacyLight = prism::MakeSceneLight(params, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    Check(legacyLight.radiance.x() == 1.0f && legacyLight.radiance.y() == 0.0f, "Rec.709 light colour unchanged");
}

void TestTextures(std::filesystem::path const& directory)
{
    std::filesystem::create_directories(directory);
    const auto file = directory / "color_management_patch.ppm";
    {
        std::ofstream out(file);
        out << "P3\n2 2\n255\n128 51 26 128 51 26\n128 51 26 128 51 26\n";
    }
    const Float3 texel(128.0f / 255.0f, 51.0f / 255.0f, 26.0f / 255.0f);
    auto sample = [](ImageTexture<Color3> const& texture) { return texture.sample(0.5f, 0.5f, Float3(0.0f)); };

    auto aces = prism::color::ColorManagement::Create({});
    prism::color::SetActive(aces);
    ImageTexture<Color3> raw(file);
    ImageTexture<Color3> converted(file, "srgb_texture");
    ImageTexture<Color3> data(file, "raw");
    Check(Near(sample(raw), texel, 1e-5f), "an untagged texture is not converted: " + Text(sample(raw)));
    Check(Near(sample(data), texel, 1e-5f), "a Raw texture is not converted");
    const Float3 expected = prism::color::Apply(kRec709ToAcescg, SrgbDecode(texel));
    Check(Near(sample(converted), expected, 2e-4f), "an sRGB texture converts into ACEScg once: " +
          Text(sample(converted)) + " expected " + Text(expected));
    Check(raw.resource->preparedPath() != converted.resource->preparedPath(), "converted textures cache apart");

    prism::color::SetActive(prism::color::ColorManagement::Create(Legacy()));
    ImageTexture<Color3> legacy(file, "srgb_texture");
    Check(Near(sample(legacy), SrgbDecode(texel), 2e-4f), "an sRGB texture decodes into Rec.709: " + Text(sample(legacy)));
    Check(legacy.resource->preparedPath() != converted.resource->preparedPath(),
          "the working space is part of the texture cache key");
    ImageTexture<Color3> legacyRaw(file, "lin_rec709");
    Check(legacyRaw.resource->preparedPath() == raw.resource->preparedPath(),
          "a texture already in the working space shares the unconverted entry");
}

// The MaterialX spike: MaterialX 1.39's default colour management only has
// transforms into lin_rec709. The generator's added library must produce one
// transform into the working space per tagged value, and none for values
// already in it or untagged.
void TestMaterialX(std::string const& libraries)
{
    auto document = [&](std::string const& attribute) {
        auto doc = MaterialX::createDocument();
        MaterialX::readFromXmlString(doc,
            "<?xml version=\"1.0\"?><materialx version=\"1.39\">"
            "<standard_surface name=\"surface\" type=\"surfaceshader\">"
            "<input name=\"base\" type=\"float\" value=\"0\"/>"
            "<input name=\"emission\" type=\"float\" value=\"1\"/>"
            "<input name=\"emission_color\" type=\"color3\" value=\"0.5, 0.2, 0.1\"" + attribute + "/>"
            "</standard_surface>"
            "<surfacematerial name=\"Patch\" type=\"material\">"
            "<input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"surface\"/>"
            "</surfacematerial></materialx>");
        return doc;
    };
    auto generate = [&](std::string const& attribute) {
        auto generator = MDLGenerator::shared(libraries);
        auto doc = document(attribute);
        doc->importLibrary(generator->get_stdlib());
        auto generated = generator->generate(doc, "Patch");
        Check(generated.success, "MDL generation: " + generated.error_message);
        return generated.mdl_source;
    };
    auto count = [](std::string const& text, std::string const& word) {
        size_t n = 0;
        for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) ++n;
        return n;
    };
    prism::color::SetActive(prism::color::ColorManagement::Create({}));
    const std::string srgb = generate(" colorspace=\"srgb_texture\"");
    Check(count(srgb, "srgb_texture_to_acescg") > 0, "ACEScg: an sRGB-tagged colour converts into ACEScg");
    const std::string acescg = generate(" colorspace=\"acescg\"");
    Check(count(acescg, "_to_acescg") == 0 && count(acescg, "_to_lin_rec709") == 0,
          "ACEScg: an ACEScg-tagged colour is not converted");
    const std::string untagged = generate("");
    Check(count(untagged, "_to_acescg") == 0 && count(untagged, "_to_lin_rec709") == 0,
          "ACEScg: an untagged colour is not converted");
    const std::string rec709 = generate(" colorspace=\"lin_rec709\"");
    Check(count(rec709, "lin_rec709_to_acescg") > 0, "ACEScg: a lin_rec709-tagged colour converts");

    prism::color::SetActive(prism::color::ColorManagement::Create(Legacy()));
    const std::string legacy = generate(" colorspace=\"srgb_texture\"");
    Check(count(legacy, "srgb_texture_to_lin_rec709") > 0 && count(legacy, "acescg") == 0,
          "Rec.709: an sRGB-tagged colour converts into lin_rec709 only");
    const std::string legacyRec709 = generate(" colorspace=\"lin_rec709\"");
    Check(count(legacyRec709, "_to_lin_rec709") == 0, "Rec.709: a lin_rec709-tagged colour is not converted");
}

Float3 Pixel(OIIO::ImageBuf const& image, float x, float y)
{
    // Sample of the pixel at fractional image coordinates (0..1).
    const int px = std::clamp(int(x * image.spec().width), 0, image.spec().width - 1);
    const int py = std::clamp(int(y * image.spec().height), 0, image.spec().height - 1);
    float value[4]{};
    image.getpixel(px, py, value, 4);
    return Float3(value[0], value[1], value[2]);
}

int CheckPatches(std::string const& path, std::string const& workingSpace)
{
    OIIO::ImageBuf image(path);
    if (!image.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::printf("FAIL: cannot read %s\n", path.c_str());
        return 1;
    }
    const bool aces = workingSpace == "acescg" || workingSpace == "ACEScg";
    auto toWorking = [&](Float3 c) { return aces ? prism::color::Apply(kRec709ToAcescg, c) : c; };
    const Float3 color(0.5f, 0.2f, 0.1f);
    const Float3 texel(128.0f / 255.0f, 51.0f / 255.0f, 26.0f / 255.0f);
    struct Patch { char const* name; float x; Float3 expected; };
    const Patch patches[] = {
        {"untagged", 0.2f, color},
        {"lin_rec709", 0.4f, toWorking(color)},
        {"srgb_texture", 0.6f, toWorking(SrgbDecode(color))},
        {"srgb_texture image", 0.8f, toWorking(SrgbDecode(texel))},
        {"dome", 0.5f, toWorking(Float3(0.2f, 0.5f, 0.8f))},
    };
    for (auto const& patch : patches) {
        const Float3 found = Pixel(image, patch.x, std::string(patch.name) == "dome" ? 0.05f : 0.5f);
        std::printf("%s: %s expected %s\n", patch.name, Text(found).c_str(), Text(patch.expected).c_str());
        Check(Near(found, patch.expected, 2e-3f), std::string(patch.name) + " patch in " + path);
    }
    return failures ? 1 : 0;
}

int CheckView(std::string const& linear, std::string const& encoded, prism::color::Options const& options)
{
    OIIO::ImageBuf exr(linear), png(encoded);
    if (!exr.read(0, 0, true, OIIO::TypeDesc::FLOAT) || !png.read(0, 0, true, OIIO::TypeDesc::FLOAT)) {
        std::printf("FAIL: cannot read %s or %s\n", linear.c_str(), encoded.c_str());
        return 1;
    }
    auto color = prism::color::ColorManagement::Create(options);
    const int width = exr.spec().width, height = exr.spec().height;
    if (png.spec().width != width || png.spec().height != height) {
        std::printf("FAIL: image sizes differ\n");
        return 1;
    }
    std::vector<float> pixels(size_t(width) * height * 3), expected;
    exr.get_pixels(OIIO::ROI(0, width, 0, height, 0, 1, 0, 3), OIIO::TypeDesc::FLOAT, pixels.data());
    expected = pixels;
    color->ApplyView(expected.data(), size_t(width) * height, 3);
    std::vector<float> found(pixels.size());
    png.get_pixels(OIIO::ROI(0, width, 0, height, 0, 1, 0, 3), OIIO::TypeDesc::FLOAT, found.data());
    double worst = 0.0;
    size_t bad = 0;
    for (size_t i = 0; i < found.size(); ++i) {
        const double error = std::abs(found[i] - expected[i]);
        worst = std::max(worst, error);
        if (error > 0.5 / 255.0 + 1e-4) ++bad;
    }
    std::printf("view %s / %s: largest difference %.6f, %zu values over half a code value\n",
                color->display().c_str(), color->view().c_str(), worst, bad);
    Check(bad == 0, "the 8-bit output is the linear output through the view, once");
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 4 && std::string(argv[1]) == "--check-patches") return CheckPatches(argv[2], argv[3]);
        if (argc == 7 && std::string(argv[1]) == "--check-view")
            return CheckView(argv[2], argv[3], {"", argv[4], argv[5], argv[6]});
        TestMatrices();
        TestViews();
        TestConversions();
        const char* cache = std::getenv("PRISM_CACHE_DIR");
        TestTextures(std::filesystem::path(cache && *cache ? cache : std::filesystem::temp_directory_path().string()) /
                     "color_management_test");
        if (const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH"); libraries && *libraries)
            TestMaterialX(libraries);
        else
            std::printf("PRISM_MATERIALX_LIBRARY_PATH unset: MaterialX colour transforms not checked\n");
    } catch (std::exception const& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("Colour management checks passed\n");
    return 0;
}
