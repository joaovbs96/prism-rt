// UsdRender settings, products and vars (docs/hydra.md, "Render settings and
// products"):
//   --unit FIXTURES SCRATCH   the shared resolver and image writer
//   --hydra FIXTURES          the delegate reads the active settings prim's
//                             prism:* settings and materialBindingPurposes,
//                             and follows a new active prim (as usdview's
//                             render settings menu sets it)
//   --check-products DIR      the two-product fixture's EXR channels
//   --check-crop FULL CROP    a data window crop matches its region of the
//                             full frame
#include "../../src/color/color_management.h"
#include "../../src/output/image_writer.h"
#include "../../src/output/render_products.h"

#include <OpenImageIO/imagebuf.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/attribute.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace prism::output;

namespace {

int failures = 0;

void Check(bool condition, std::string const &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool Contains(std::vector<std::string> const &messages, std::string const &text)
{
    for (auto const &message : messages)
        if (message.find(text) != std::string::npos) return true;
    return false;
}

void Unit(std::filesystem::path const &fixtures, std::filesystem::path const &scratch)
{
    // Data windows: a pixel is in when its centre is.
    int x, y, w, h;
    DataWindowPixels(GfRange2f(GfVec2f(0, 0), GfVec2f(1, 1)), 96, 64, &x, &y, &w, &h);
    Check(x == 0 && y == 0 && w == 96 && h == 64, "full data window");
    DataWindowPixels(GfRange2f(GfVec2f(0.25f, 0.2f), GfVec2f(0.75f, 0.9f)), 96, 64, &x, &y, &w, &h);
    Check(x == 24 && y == 6 && w == 48 && h == 45, "cropped data window (y up in NDC, down in pixels)");
    DataWindowPixels(GfRange2f(GfVec2f(-0.1f, -0.1f), GfVec2f(1.1f, 1.1f)), 100, 50, &x, &y, &w, &h);
    Check(x == -10 && y == -5 && w == 120 && h == 60, "overscan data window");
    DataWindowPixels(GfRange2f(GfVec2f(0.5f, 0.5f), GfVec2f(0.5f, 0.5f)), 10, 10, &x, &y, &w, &h);
    Check(w == 0 && h == 0, "empty data window");
    Check(ConformPolicy(UsdRenderTokens->expandAperture) == CameraUtilFit &&
          ConformPolicy(UsdRenderTokens->cropAperture) == CameraUtilCrop &&
          ConformPolicy(UsdRenderTokens->adjustApertureWidth) == CameraUtilMatchVertically &&
          ConformPolicy(UsdRenderTokens->adjustApertureHeight) == CameraUtilMatchHorizontally &&
          ConformPolicy(UsdRenderTokens->adjustPixelAspectRatio) == CameraUtilDontConform,
          "aspect ratio conform policies");
    // RenderVar sources.
    Check(RenderVarAov("color", TfToken("raw")) == AovKind::Color &&
          RenderVarAov("Ci", TfToken()) == AovKind::Color &&
          RenderVarAov("C.*", TfToken("lpe")) == AovKind::Color &&
          RenderVarAov("normal", TfToken("raw")) == AovKind::Normal &&
          RenderVarAov("primvars:st", TfToken("raw")) == AovKind::St &&
          RenderVarAov("st", TfToken("primvar")) == AovKind::St &&
          RenderVarAov("primId", TfToken("raw")) == AovKind::PrimId &&
          RenderVarAov("C<RD>.*L", TfToken("lpe")) == AovKind::Unknown &&
          RenderVarAov("albedo", TfToken("raw")) == AovKind::Albedo &&
          RenderVarAov("diffuse", TfToken()) == AovKind::Diffuse &&
          RenderVarAov("specular", TfToken("raw")) == AovKind::Specular &&
          RenderVarAov("emission", TfToken("raw")) == AovKind::Emission &&
          RenderVarAov("Pworld", TfToken("raw")) == AovKind::Pworld &&
          RenderVarAov("P", TfToken("raw")) == AovKind::Pworld &&
          RenderVarAov("transmission", TfToken("raw")) == AovKind::Unknown,
          "render var sources");

    // The two-product fixture.
    auto stage = UsdStage::Open((fixtures / "render_products.usda").string());
    Check(bool(stage), "open render_products.usda");
    if (!stage) return;
    std::vector<std::string> warnings;
    auto job = ResolveRenderJob(stage, "", "out", &warnings);
    Check(job.has_value(), "render settings found through renderSettingsPrimPath");
    if (!job) return;
    Check(job->settings == SdfPath("/Render/Settings"), "settings path");
    Check(job->products.size() == 3, "three products");
    Check(Contains(warnings, "/Render/Vars/Diffuse") && Contains(warnings, "not supported"),
          "unsupported var warned about");
    Check(job->namespacedSettings.count("prism:samplesPerPixel") == 1, "prism:* settings kept");
    Check(job->base.width == 64 && job->base.height == 64 &&
          job->base.camera == SdfPath("/World/Camera"), "settings resolution and camera");
    if (job->products.size() == 3) {
        auto const &beauty = job->products[0];
        Check(beauty.output == (std::filesystem::path("out") / "render_products_beauty.exr").string(),
              "product name under the output directory");
        Check(beauty.vars.size() == 3 && beauty.vars[0].kind == AovKind::Color &&
              beauty.vars[0].components == 4 && beauty.vars[1].kind == AovKind::CameraDepth &&
              beauty.vars[2].kind == AovKind::Normal && beauty.vars[2].name == "Normal",
              "beauty vars");
        Check(beauty.SameView(job->products[1]), "the PNG product shares the beauty view");
        auto const &side = job->products[2];
        Check(side.camera == SdfPath("/World/Side") && side.width == 48 && side.height == 32 &&
              !side.SameView(beauty), "side product camera and resolution");
        Check(side.vars.size() == 3 && side.vars[0].kind == AovKind::Color &&
              side.vars[0].components == 3 && side.vars[2].kind == AovKind::St,
              "side vars (lpe beauty, primvar st)");
    }
    Check(job->materialBindingPurposes.size() == 2 && job->materialBindingPurposes[0] == TfToken("full"),
          "default material binding purposes");
    bool threw = false;
    try { ResolveRenderJob(stage, "/World", "", &warnings); } catch (std::exception const &) { threw = true; }
    Check(threw, "a prim that is not RenderSettings is an error");

    auto crop = UsdStage::Open((fixtures / "render_settings_crop.usda").string());
    auto cropJob = crop ? ResolveRenderJob(crop, "", "", nullptr) : std::nullopt;
    Check(cropJob && cropJob->products.size() == 3, "crop fixture products");
    if (cropJob && cropJob->products.size() == 3) {
        auto const &window = cropJob->products[1];
        Check(window.dataX == 24 && window.dataY == 6 && window.dataWidth == 48 &&
              window.dataHeight == 45 && !window.FullDataWindow(), "crop product data window");
        Check(cropJob->products[2].pixelAspectRatio == 2.0f, "pixel aspect ratio");
    }
    auto parity = UsdStage::Open((fixtures / "parity.usda").string());
    Check(parity && !ResolveRenderJob(parity, "", "", nullptr), "no render settings: no job");

    // Channel names.
    Check(LayerChannels("color", AovKind::Color, 4, true) == std::vector<std::string>{"R", "G", "B", "A"} &&
          LayerChannels("Normal", AovKind::Normal, 3, false) ==
              std::vector<std::string>{"Normal.X", "Normal.Y", "Normal.Z"} &&
          LayerChannels("Depth", AovKind::CameraDepth, 1, false) == std::vector<std::string>{"Depth.Z"} &&
          LayerChannels("st", AovKind::St, 2, false) == std::vector<std::string>{"st.U", "st.V"} &&
          LayerChannels("id", AovKind::PrimId, 1, false) == std::vector<std::string>{"id.id"} &&
          LayerChannels("beauty2", AovKind::Color, 3, false) ==
              std::vector<std::string>{"beauty2.R", "beauty2.G", "beauty2.B"} &&
          LayerChannels("Diffuse", AovKind::Diffuse, 3, false) ==
              std::vector<std::string>{"Diffuse.R", "Diffuse.G", "Diffuse.B"} &&
          LayerChannels("P", AovKind::Pworld, 3, false) == std::vector<std::string>{"P.X", "P.Y", "P.Z"},
          "layer channel names");

    // The writer: every layer in EXR, with its data window; colour only in
    // 8-bit, through the view.
    auto color = prism::color::ColorManagement::Create({});
    Image image;
    image.width = 3; image.height = 2;
    image.x = 5; image.y = 7; image.fullWidth = 10; image.fullHeight = 12;
    ImageLayer beautyLayer{"color", {"R", "G", "B", "A"}, std::vector<float>(24, 0.25f), true};
    ImageLayer normalLayer{"Normal", {"Normal.X", "Normal.Y", "Normal.Z"}, std::vector<float>(18), false};
    for (size_t p = 0; p < 6; ++p) normalLayer.pixels[p * 3 + 2] = float(p);
    image.layers = {beautyLayer, normalLayer};
    std::filesystem::create_directories(scratch);
    const auto exr = (scratch / "writer.exr").string();
    WriteImage(exr, image, *color);
    OIIO::ImageBuf readBack(exr);
    Check(readBack.read(0, 0, true, OIIO::TypeDesc::FLOAT), "read the written EXR");
    auto const &spec = readBack.spec();
    Check(spec.nchannels == 7 && spec.channelindex("A") >= 0 && spec.channelindex("Normal.Z") >= 0,
          "EXR channels");
    Check(spec.x == 5 && spec.y == 7 && spec.width == 3 && spec.height == 2 &&
          spec.full_width == 10 && spec.full_height == 12, "EXR data and display windows");
    Check(spec.find_attribute("chromaticities") != nullptr &&
          !spec.get_string_attribute("oiio:ColorSpace").empty(),
          "EXR colour space tags (" + std::string(spec.get_string_attribute("oiio:ColorSpace")) + ")");
    std::vector<float> pixel(size_t(spec.nchannels));
    readBack.getpixel(5 + 2, 7 + 1, pixel.data());
    Check(pixel[spec.channelindex("Normal.Z")] == 5.0f && pixel[spec.channelindex("R")] == 0.25f,
          "EXR values");
    warnings.clear();
    const auto png = (scratch / "writer.png").string();
    WriteImage(png, image, *color, &warnings);
    Check(Contains(warnings, "Normal"), "8-bit output warns about the AOVs it drops");
    OIIO::ImageBuf readPng(png);
    Check(readPng.read(0, 0, true, OIIO::TypeDesc::FLOAT) && readPng.spec().nchannels == 4,
          "8-bit output keeps colour and alpha");
    // Premultiplied 0.25 at alpha 0.25 is colour 1: the view applies to it,
    // the file holds unassociated alpha, and reading associates it again.
    std::vector<float> viewed{1.0f, 1.0f, 1.0f, 0.25f};
    color->ApplyView(viewed.data(), 1, 4);
    readPng.getpixel(0, 0, pixel.data());
    Check(std::abs(pixel[0] - 0.25f * viewed[0]) < 2.0f / 255 && std::abs(pixel[3] - 0.25f) < 1.0f / 255,
          std::string("8-bit colour goes through the view once, alpha stays linear (") +
          std::to_string(pixel[0]) + " for " + std::to_string(0.25f * viewed[0]) + ")");

    // Product images take the vars from the rendered AOVs.
    RenderProductRequest product;
    product.width = product.dataWidth = 2;
    product.height = product.dataHeight = 1;
    product.vars = {{SdfPath("/V/C"), "C", "color", TfToken(), TfToken("color4f"), AovKind::Color, 4},
                    {SdfPath("/V/D"), "D", "depth", TfToken(), TfToken("float"), AovKind::Depth, 1}};
    warnings.clear();
    auto built = BuildProductImage(product, {{AovKind::Color, 3, {1, 2, 3, 4, 5, 6}}}, &warnings);
    Check(built.layers.size() == 1 && built.layers[0].pixels[3] == 1.0f &&
          built.layers[0].channels[0] == "R", "RGB render gets opaque alpha");
    Check(Contains(warnings, "/V/D"), "missing AOV warned about");
}

void Render(UsdImagingGLEngine &engine, UsdStageRefPtr const &stage)
{
    UsdImagingGLRenderParams params;
    params.enableSceneMaterials = true;
    params.enableSceneLights = true;
    params.showRender = true;
    params.showProxy = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    do {
        engine.Render(stage->GetPseudoRoot(), params);
        if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("Render timed out");
        if (!engine.IsConverged()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (!engine.IsConverged());
}

GfVec4f CenterColor(UsdImagingGLEngine &engine)
{
    auto *buffer = engine.GetAovRenderBuffer(HdAovTokens->color);
    if (!buffer || buffer->GetFormat() != HdFormatFloat32Vec4) throw std::runtime_error("No colour AOV");
    buffer->Resolve();
    auto const *data = static_cast<GfVec4f const *>(buffer->Map());
    const GfVec4f value = data[size_t(buffer->GetHeight() / 2) * buffer->GetWidth() + buffer->GetWidth() / 2];
    buffer->Unmap();
    return value;
}

std::string Stat(UsdImagingGLEngine &engine, char const *name)
{
    const auto stats = engine.GetRenderStats();
    const auto found = stats.find(name);
    return found != stats.end() && found->second.IsHolding<std::string>()
        ? found->second.UncheckedGet<std::string>() : std::string("<missing>");
}

void Hydra(std::filesystem::path const &fixtures)
{
    auto stage = UsdStage::Open((fixtures / "render_settings_binding.usda").string());
    if (!stage) throw std::runtime_error("Cannot open render_settings_binding.usda");
    UsdImagingGLEngine::Parameters parameters;
    parameters.gpuEnabled = false;
    parameters.rendererPluginId = TfToken("HdPrismRendererPlugin");
    UsdImagingGLEngine engine(parameters);
    if (engine.GetCurrentRendererId() != parameters.rendererPluginId)
        throw std::runtime_error("PrismRT plugin not loaded");
    engine.SetCameraPath(SdfPath("/World/Camera"));
    engine.SetRenderViewport(GfVec4d(0, 0, 32, 32));
    engine.SetRendererAovs({HdAovTokens->color});
    engine.SetEnablePresentation(false);
    // The stage's renderSettingsPrimPath: preview bindings first, 4 spp.
    Render(engine, stage);
    Check(Stat(engine, "activeRenderSettings") == "/Render/Preview",
          "the delegate reads the stage's active render settings prim (" +
          Stat(engine, "activeRenderSettings") + ")");
    const VtValue samples = engine.GetRendererSetting(TfToken("prism:samplesPerPixel"));
    Check(samples.IsHolding<int>() && samples.UncheckedGet<int>() == 4,
          "authored prism:samplesPerPixel applies");
    const GfVec4f preview = CenterColor(engine);
    Check(preview[1] > 2 * preview[0], "preview binding (green) with materialBindingPurposes [preview, \"\"]");
    // Another active prim (usdview's render settings menu): allPurpose only.
    engine.SetActiveRenderSettingsPrimPath(SdfPath("/Render/AllPurpose"));
    Render(engine, stage);
    Check(Stat(engine, "activeRenderSettings") == "/Render/AllPurpose", "a new active prim is followed");
    const VtValue samples2 = engine.GetRendererSetting(TfToken("prism:samplesPerPixel"));
    Check(samples2.IsHolding<int>() && samples2.UncheckedGet<int>() == 6, "its prism:* settings apply");
    const GfVec4f all = CenterColor(engine);
    Check(all[0] > 2 * all[1], "allPurpose binding (red) with materialBindingPurposes [\"\"]");
    // An edit of the active prim's purposes rebuilds the bindings.
    stage->GetPrimAtPath(SdfPath("/Render/AllPurpose"))
        .GetAttribute(TfToken("materialBindingPurposes"))
        .Set(VtArray<TfToken>{TfToken("preview"), TfToken()});
    Render(engine, stage);
    const GfVec4f edited = CenterColor(engine);
    Check(edited[1] > 2 * edited[0], "edited materialBindingPurposes rebuild the bindings");
}

std::vector<float> Channels(OIIO::ImageBuf const &image, std::vector<std::string> const &names, int x, int y)
{
    std::vector<float> pixel(size_t(image.spec().nchannels)), result;
    image.getpixel(x, y, pixel.data());
    for (auto const &name : names) {
        const int index = image.spec().channelindex(name);
        if (index < 0) throw std::runtime_error("Missing channel " + name);
        result.push_back(pixel[size_t(index)]);
    }
    return result;
}

void CheckProducts(std::filesystem::path const &directory)
{
    OIIO::ImageBuf beauty((directory / "render_products_beauty.exr").string());
    Check(beauty.read(0, 0, true, OIIO::TypeDesc::FLOAT), "read the beauty product");
    auto const &spec = beauty.spec();
    Check(spec.width == 64 && spec.height == 64, "beauty product resolution");
    for (auto const *name : {"R", "G", "B", "A", "Depth.Z", "Normal.X", "Normal.Y", "Normal.Z"})
        Check(spec.channelindex(name) >= 0, std::string("beauty channel ") + name);
    Check(spec.nchannels == 8, "beauty has no channel for the unsupported var");
    // The panel fills the centre: normal +Z at camera depth 4; colour sane.
    const auto centre = Channels(beauty, {"R", "G", "B", "A", "Depth.Z", "Normal.X", "Normal.Y", "Normal.Z"}, 32, 32);
    Check(std::abs(centre[4] - 4.0f) < 1e-3f, "centre camera depth 4");
    Check(std::abs(centre[7] - 1.0f) < 1e-4f && std::abs(centre[5]) < 1e-4f, "centre normal +Z");
    Check(centre[3] == 1.0f, "opaque alpha");
    bool finite = true, positive = false;
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x) {
            auto rgb = Channels(beauty, {"R", "G", "B"}, x, y);
            for (float v : rgb) { finite &= std::isfinite(v) && v >= 0; positive |= v > 0; }
        }
    Check(finite && positive, "beauty colour finite, non-negative and lit");
    OIIO::ImageBuf preview((directory / "render_products_beauty.png").string());
    Check(preview.read(0, 0, true, OIIO::TypeDesc::FLOAT) && preview.spec().width == 64 &&
          preview.nativespec().format == OIIO::TypeDesc::UINT8, "8-bit product of the same view");
    OIIO::ImageBuf side((directory / "render_products_side.exr").string());
    Check(side.read(0, 0, true, OIIO::TypeDesc::FLOAT), "read the side product");
    auto const &sideSpec = side.spec();
    Check(sideSpec.width == 48 && sideSpec.height == 32, "side product resolution");
    for (auto const *name : {"R", "G", "B", "Neye.X", "Neye.Y", "Neye.Z", "St.U", "St.V"})
        Check(sideSpec.channelindex(name) >= 0, std::string("side channel ") + name);
    Check(sideSpec.channelindex("A") < 0, "color3f beauty has no alpha");
    const auto eye = Channels(side, {"Neye.X", "Neye.Y", "Neye.Z"}, 24, 16);
    const float length = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    Check(std::abs(length - 1.0f) < 1e-3f && eye[2] > 0.5f, "side eye normal faces the camera");
}

void CheckCrop(std::string const &fullPath, std::string const &cropPath)
{
    OIIO::ImageBuf full(fullPath), crop(cropPath);
    Check(full.read(0, 0, true, OIIO::TypeDesc::FLOAT) && crop.read(0, 0, true, OIIO::TypeDesc::FLOAT),
          "read the full and cropped products");
    auto const &f = full.spec();
    auto const &c = crop.spec();
    Check(c.x == 24 && c.y == 6 && c.width == 48 && c.height == 45 &&
          c.full_width == f.width && c.full_height == f.height, "crop data window in the EXR");
    // Geometry: exactly the full frame's pixels. Colour: the same region
    // (other samples, since pixel seeds are local to the data window).
    bool normals = true;
    double error = 0, energy = 0, sumFull = 0, sumCrop = 0;
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x) {
            const auto a = Channels(full, {"Normal.X", "Normal.Y", "Normal.Z", "R", "G", "B"}, c.x + x, c.y + y);
            const auto b = Channels(crop, {"Normal.X", "Normal.Y", "Normal.Z", "R", "G", "B"}, c.x + x, c.y + y);
            for (int i = 0; i < 3; ++i) normals &= a[i] == b[i];
            for (int i = 3; i < 6; ++i) {
                error += double(a[i] - b[i]) * (a[i] - b[i]);
                energy += double(a[i]) * a[i];
                sumFull += a[i];
                sumCrop += b[i];
            }
        }
    Check(normals, "crop normals equal the full frame's region");
    const double nrmse = std::sqrt(error / std::max(energy, 1e-20));
    const double mean = std::abs(sumCrop - sumFull) / std::max(sumFull, 1e-20);
    std::cout << "Crop against the full frame: NRMSE " << nrmse << ", mean " << mean << '\n';
    Check(nrmse < 0.1 && mean < 0.02, "crop colour matches the full frame's region");
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "--unit" && argc == 4) Unit(argv[2], argv[3]);
        else if (mode == "--hydra" && argc == 3) Hydra(argv[2]);
        else if (mode == "--check-products" && argc == 3) CheckProducts(argv[2]);
        else if (mode == "--check-crop" && argc == 4) CheckCrop(argv[2], argv[3]);
        else {
            std::cerr << "usage: prism_render_settings_test --unit FIXTURES SCRATCH | --hydra FIXTURES |"
                         " --check-products DIR | --check-crop FULL.exr CROP.exr\n";
            return 2;
        }
    } catch (std::exception const &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "Render settings checks passed\n";
    return 0;
}
