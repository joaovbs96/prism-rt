#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/plug/registry.h>
#include <OpenImageIO/imagebuf.h>
#include "../../src/load_profile.h"
#include "../../src/color/color_management.h"
#include "../../src/output/render_products.h"
#include "../../src/render_defaults.h"
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/types.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
// The purposes rendered besides default geometry (UsdRenderSettings
// includedPurposes). Final frames: render-purpose geometry instead of the
// viewport's proxies.
struct Purposes {
    bool render = true, proxy = false, guide = false;
};

void render(UsdImagingGLEngine& engine, const UsdStageRefPtr& stage,
            UsdTimeCode frame = UsdTimeCode::Default(),
            bool domeLightCameraVisibility = true, Purposes purposes = {}) {
    UsdImagingGLRenderParams params;
    params.frame = frame;
    params.enableSceneMaterials = true;
    params.enableSceneLights = true;
    params.showRender = purposes.render;
    params.showProxy = purposes.proxy;
    params.showGuides = purposes.guide;
    engine.SetRendererSetting(TfToken("domeLightCameraVisibility"),
                              VtValue(domeLightCameraVisibility));
    // A scene that cannot be prepared (for example out of memory) is not
    // retried by the delegate, which then reports itself converged; report
    // the failure instead of saving an empty image.
    auto checkPreparation = [&engine] {
        const VtDictionary stats = engine.GetRenderStats();
        if (const auto found = stats.find("snapshotError");
            found != stats.end() && found->second.IsHolding<std::string>() &&
            !found->second.UncheckedGet<std::string>().empty())
            throw std::runtime_error("Scene preparation failed: " +
                found->second.UncheckedGet<std::string>());
    };
    const auto start = std::chrono::steady_clock::now();
    auto lastErrorCheck = start;
    bool firstRender = true;
    do {
        {
            std::optional<prism::load_profile::Scope> renderProfile;
            if (firstRender) renderProfile.emplace("cli.first_render_call");
            firstRender = false;
            engine.Render(stage->GetPseudoRoot(), params);
        }
        // Statistics scan the scene, so poll them about once a second.
        if (std::chrono::steady_clock::now() - lastErrorCheck > std::chrono::seconds(1)) {
            lastErrorCheck = std::chrono::steady_clock::now();
            checkPreparation();
        }
        // No limit by default (prism:timeLimitSeconds bounds a render);
        // PRISM_RENDER_TIMEOUT_MINUTES makes a render that never converges
        // an error.
        static const int minutes = [] {
            const char* value = std::getenv("PRISM_RENDER_TIMEOUT_MINUTES");
            return value && *value ? std::max(1, std::atoi(value)) : 0;
        }();
        if (minutes > 0 && std::chrono::steady_clock::now()-start > std::chrono::minutes(minutes))
            throw std::runtime_error("Hydra failed to converge within the time limit (PRISM_RENDER_TIMEOUT_MINUTES)");
        if (!engine.IsConverged()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!engine.IsConverged());
    prism::load_profile::Mark("cli.converged");
    checkPreparation();
}

std::vector<float> pixels(UsdImagingGLEngine& engine, int width, int height);

std::unique_ptr<UsdImagingGLEngine> makeAnimationEngine(
    const UsdStageRefPtr& stage, const std::string& cameraPath,
    int width, int height, int spp) {
    UsdImagingGLEngine::Parameters parameters;
    parameters.gpuEnabled = false;
    parameters.rendererPluginId = TfToken("HdPrismRendererPlugin");
    auto engine = std::make_unique<UsdImagingGLEngine>(parameters);
    if (engine->GetCurrentRendererId() != parameters.rendererPluginId)
        throw std::runtime_error("PrismRT plugin not loaded; check PXR_PLUGINPATH_NAME and DLL paths");
    engine->SetCameraPath(SdfPath(cameraPath));
    engine->SetRenderViewport(GfVec4d(0, 0, width, height));
    engine->SetRendererSetting(TfToken("prism:samplesPerPixel"), VtValue(spp));
    engine->SetRendererSetting(TfToken("prism:maxDepth"), VtValue(8));
    engine->SetRendererSetting(TfToken("prism:rrMinDepth"), VtValue(2));
    engine->SetRendererSetting(TfToken("prism:seed"), VtValue(17));
    engine->SetRendererSetting(TfToken("prism:adaptiveThreshold"), VtValue(0.0f));
    engine->SetRendererSetting(TfToken("prism:textureStreamingPrepass"), VtValue(true));
    engine->SetEnablePresentation(false);
    return engine;
}

int64_t statInt64(UsdImagingGLEngine& engine, const char* name) {
    const VtDictionary stats = engine.GetRenderStats();
    const auto found = stats.find(name);
    if (found == stats.end() || !found->second.IsHolding<int64_t>())
        throw std::runtime_error(std::string("Missing integer render stat: ") + name);
    return found->second.UncheckedGet<int64_t>();
}

std::vector<float> renderFreshAt(
    const std::string& scenePath, const std::string& cameraPath,
    int width, int height, int spp, double frame) {
    auto stage = UsdStage::Open(scenePath);
    if (!stage) throw std::runtime_error("Cannot reopen animated USD scene");
    auto engine = makeAnimationEngine(stage, cameraPath, width, height, spp);
    render(*engine, stage, UsdTimeCode(frame));
    return pixels(*engine, width, height);
}

void runAnimationExercise(const std::string& scenePath,
                          const std::string& cameraPath,
                          int width, int height, int spp) {
    const int animationSpp = std::min(spp, 16);
    auto stage = UsdStage::Open(scenePath);
    if (!stage) throw std::runtime_error("Cannot open animated USD scene");
    auto engine = makeAnimationEngine(
        stage, cameraPath, width, height, animationSpp);
    render(*engine, stage, UsdTimeCode(1.0));
    const auto frame1 = pixels(*engine, width, height);
    const int64_t buildsAfterFrame1 = statInt64(*engine, "snapshotBuilds");
    const int64_t conservativeAfterFrame1 =
        statInt64(*engine, "conservativeRebuilds");
    render(*engine, stage, UsdTimeCode(2.0));
    const auto frame2 = pixels(*engine, width, height);
    if (frame1 == frame2)
        throw std::runtime_error("Animated fixture did not change the rendered image");
    if (frame2 != renderFreshAt(scenePath, cameraPath, width, height,
                                animationSpp, 2.0))
        throw std::runtime_error("Frame 2 differs from a fresh renderer at frame 2");
    if (frame1 != renderFreshAt(scenePath, cameraPath, width, height,
                                animationSpp, 1.0))
        throw std::runtime_error("Frame 1 differs from a fresh renderer at frame 1");

    const int64_t buildsAfterFrame2 = statInt64(*engine, "snapshotBuilds");
    const int64_t conservativeAfterFrame2 =
        statInt64(*engine, "conservativeRebuilds");
    const bool animatedRebuilt = buildsAfterFrame2 != buildsAfterFrame1 ||
        conservativeAfterFrame2 != conservativeAfterFrame1;
    std::cout << "Animated counters: snapshotBuilds " << buildsAfterFrame1
              << " -> " << buildsAfterFrame2 << ", conservativeRebuilds "
              << conservativeAfterFrame1 << " -> "
              << conservativeAfterFrame2 << '\n';

    const auto xformPath = std::filesystem::path(scenePath).parent_path() /
        "animated_xform.usda";
    auto xformStage = UsdStage::Open(xformPath.string());
    if (!xformStage) throw std::runtime_error("Cannot open xform-only fixture");
    auto xformEngine = makeAnimationEngine(
        xformStage, cameraPath, width, height, animationSpp);
    render(*xformEngine, xformStage, UsdTimeCode(1.0));
    const auto xformFrame1 = pixels(*xformEngine, width, height);
    const int64_t xformBuilds = statInt64(*xformEngine, "snapshotBuilds");
    const int64_t xformConservative =
        statInt64(*xformEngine, "conservativeRebuilds");
    const int64_t placementsBefore = statInt64(*xformEngine, "placementInvalidations");
    const int64_t prototypesBefore = statInt64(*xformEngine, "prototypeInvalidations");
    const int64_t materialsBefore = statInt64(*xformEngine, "materialInvalidations");
    const int64_t environmentsBefore = statInt64(*xformEngine, "environmentInvalidations");
    render(*xformEngine, xformStage, UsdTimeCode(2.0));
    if (pixels(*xformEngine, width, height) == xformFrame1)
        throw std::runtime_error("Xform-only animation did not change the image");
    const auto xformFrame2 = pixels(*xformEngine, width, height);
    if (xformFrame2 != renderFreshAt(xformPath.string(), cameraPath, width,
                                     height, animationSpp, 2.0) ||
        xformFrame1 != renderFreshAt(xformPath.string(), cameraPath, width,
                                     height, animationSpp, 1.0))
        throw std::runtime_error(
            "Xform-only frame differs from its fresh-render reference");
    const int64_t xformBuildsAfter = statInt64(*xformEngine, "snapshotBuilds");
    const int64_t xformConservativeAfter =
        statInt64(*xformEngine, "conservativeRebuilds");
    const int64_t placementsAfter = statInt64(*xformEngine, "placementInvalidations");
    const int64_t prototypesAfter = statInt64(*xformEngine, "prototypeInvalidations");
    const int64_t materialsAfter = statInt64(*xformEngine, "materialInvalidations");
    const int64_t environmentsAfter = statInt64(*xformEngine, "environmentInvalidations");
    std::cout << "Xform-only counters: snapshotBuilds " << xformBuilds
              << " -> " << xformBuildsAfter << ", conservativeRebuilds "
              << xformConservative << " -> " << xformConservativeAfter << '\n';
    std::cout << "Xform-only invalidations: placement " << placementsBefore
              << " -> " << placementsAfter << ", prototype " << prototypesBefore
              << " -> " << prototypesAfter << ", material " << materialsBefore
              << " -> " << materialsAfter << ", environment "
              << environmentsBefore << " -> " << environmentsAfter << '\n';
    if (animatedRebuilt || xformBuildsAfter != xformBuilds ||
        xformConservativeAfter != xformConservative ||
        placementsAfter <= placementsBefore ||
        prototypesAfter != prototypesBefore || materialsAfter != materialsBefore ||
        environmentsAfter != environmentsBefore)
        throw std::runtime_error(
            "Timeline change caused a conservative scene rebuild");
}

std::vector<float> pixels(UsdImagingGLEngine& engine, int width, int height) {
    auto* buffer = engine.GetAovRenderBuffer(HdAovTokens->color);
    if (!buffer || buffer->GetFormat() != HdFormatFloat32Vec4 ||
        buffer->GetWidth() != unsigned(width) || buffer->GetHeight() != unsigned(height))
        throw std::runtime_error("Missing or invalid float4 color AOV");
    buffer->Resolve();
    const auto* data = static_cast<const float*>(buffer->Map());
    if (!data) { buffer->Unmap(); throw std::runtime_error("Cannot map color AOV"); }
    std::vector<float> result(size_t(width)*height*4);
    // Hydra's raster origin is bottom-left; image files use top-left.
    for (int y = 0; y < height; ++y)
        std::copy_n(data+size_t(height-1-y)*width*4, size_t(width)*4,
                    result.data()+size_t(y)*width*4);
    buffer->Unmap();
    for (float v : result) if (!std::isfinite(v))
        throw std::runtime_error("Non-finite color AOV");
    return result;
}

template<class T> T center(UsdImagingGLEngine& engine, TfToken const& aov, HdFormat format) {
    auto* buffer = engine.GetAovRenderBuffer(aov);
    if (!buffer || buffer->GetFormat() != format)
        throw std::runtime_error("Invalid AOV: " + aov.GetString());
    const auto* data = static_cast<const T*>(buffer->Map());
    if (!data) { buffer->Unmap(); throw std::runtime_error("Cannot map " + aov.GetString()); }
    const T value = data[size_t(buffer->GetHeight()/2)*buffer->GetWidth()+buffer->GetWidth()/2];
    buffer->Unmap();
    return value;
}

void checkFixtureAovs(UsdImagingGLEngine& engine) {
    const float depth = center<float>(engine, HdAovTokens->depth, HdFormatFloat32);
    const float cameraDepth = center<float>(engine, HdAovTokens->cameraDepth, HdFormatFloat32);
    if (std::abs(depth - float(100.0/99.9-10.0/(99.9*4.0))) > 1e-5f ||
        std::abs(cameraDepth-4.0f) > 1e-4f)
        throw std::runtime_error("Incorrect projected or camera-space depth AOV");
    for (const auto& aov : {HdAovTokens->normal, HdAovTokens->Neye}) {
        const auto n = center<GfVec3f>(engine, aov, HdFormatFloat32Vec3);
        if ((n-GfVec3f(0,0,1)).GetLength() > 1e-4f)
            throw std::runtime_error("Incorrect normal AOV: " + aov.GetString());
    }
    if (center<int>(engine,HdAovTokens->primId,HdFormatInt32) < 0 ||
        center<int>(engine,HdAovTokens->elementId,HdFormatInt32) != 0 ||
        center<int>(engine,HdAovTokens->instanceId,HdFormatInt32) != -1)
        throw std::runtime_error("Incorrect prim/instance/coarse-face AOV");
}

// A rendered AOV of the engine, top row first, as floats (integer AOVs
// converted exactly below 2^24).
prism::output::RenderedAov readAov(UsdImagingGLEngine& engine, prism::output::AovKind kind,
                                   int width, int height) {
    auto const* info = prism::output::FindAov(kind);
    if (!info) throw std::runtime_error("Unknown AOV");
    const TfToken name(std::string(info->name));
    auto* buffer = engine.GetAovRenderBuffer(name);
    if (!buffer || buffer->GetWidth() != unsigned(width) || buffer->GetHeight() != unsigned(height))
        throw std::runtime_error("Missing or invalid AOV: " + name.GetString());
    const HdFormat format = buffer->GetFormat();
    const HdFormat component = HdGetComponentFormat(format);
    const size_t components = HdGetComponentCount(format);
    if (component != HdFormatFloat32 && component != HdFormatInt32)
        throw std::runtime_error("Unsupported AOV format: " + name.GetString());
    buffer->Resolve();
    const void* data = buffer->Map();
    if (!data) { buffer->Unmap(); throw std::runtime_error("Cannot map " + name.GetString()); }
    prism::output::RenderedAov aov{kind, int(components),
        std::vector<float>(size_t(width) * height * components)};
    // Hydra's raster origin is bottom-left; image files use top-left.
    for (int y = 0; y < height; ++y) {
        const size_t source = size_t(height - 1 - y) * width * components;
        float* row = aov.pixels.data() + size_t(y) * width * components;
        for (size_t i = 0; i < size_t(width) * components; ++i)
            row[i] = component == HdFormatFloat32
                ? static_cast<const float*>(data)[source + i]
                : float(static_cast<const int32_t*>(data)[source + i]);
    }
    buffer->Unmap();
    if (kind == prism::output::AovKind::Color)
        for (float v : aov.pixels) if (!std::isfinite(v))
            throw std::runtime_error("Non-finite color AOV");
    return aov;
}

void printWarnings(std::vector<std::string> const& warnings) {
    for (auto const& warning : warnings) std::cerr << "prism_hydra_render: " << warning << '\n';
}

// Command-line options that are also prism:* render settings: authored values
// on the settings prim would win in the delegate, so the options override
// them in the session layer (the CLI's precedence).
void overrideAuthoredSettings(const UsdStageRefPtr& stage, SdfPath const& settings,
                              std::vector<std::pair<std::string, VtValue>> const& given) {
    UsdPrim prim = stage->GetPrimAtPath(settings);
    if (!prim) return;
    UsdEditContext session(stage, stage->GetSessionLayer());
    for (auto const& [name, value] : given) {
        UsdAttribute attribute = prim.GetAttribute(TfToken("prism:" + name));
        if (!attribute || !attribute.HasAuthoredValue()) continue;
        const SdfValueTypeName type = attribute.GetTypeName();
        VtValue converted = value;
        if (type == SdfValueTypeNames->Token && value.IsHolding<std::string>())
            converted = VtValue(TfToken(value.UncheckedGet<std::string>()));
        else if (type == SdfValueTypeNames->Asset && value.IsHolding<std::string>())
            converted = VtValue(SdfAssetPath(value.UncheckedGet<std::string>()));
        else converted = VtValue::CastToTypeOf(value, type.GetDefaultValue());
        if (converted.IsEmpty() || !attribute.Set(converted))
            throw std::runtime_error("Cannot override " + attribute.GetPath().GetString());
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--check-transparent-alpha") {
            OIIO::ImageBuf saved(argv[2]);
            if (!saved.read(0, 0, true, OIIO::TypeDesc::FLOAT))
                throw std::runtime_error("Cannot read export: " + saved.geterror());
            const auto &spec = saved.spec();
            if (spec.nchannels < 4 || spec.width <= 0 || spec.height <= 0)
                throw std::runtime_error("Export has no RGBA channel data");
            std::vector<float> pixel(size_t(spec.nchannels));
            int transparentCorners = 0;
            for (const auto &corner : {std::pair<int,int>{0,0}, {spec.width-1,0},
                                       {0,spec.height-1}, {spec.width-1,spec.height-1}}) {
                saved.getpixel(corner.first, corner.second, pixel.data(), spec.nchannels);
                if (pixel[3] <= 0.001f) ++transparentCorners;
            }
            if (transparentCorners != 4)
                throw std::runtime_error("Saved export corner pixels are not transparent");
            saved.getpixel(spec.width/2, spec.height/2, pixel.data(), spec.nchannels);
            if (pixel[3] < .99f)
                throw std::runtime_error("Saved export geometry pixel is not opaque");
            std::cout << "Transparent export alpha verified: " << argv[2] << '\n';
            return 0;
        }
        // prism_hydra_render scene.usda --render-settings PATH|default OUTPUT_DIR [options]
        // renders the RenderProducts of a RenderSettings prim.
        const bool productMode = argc >= 3 && std::string(argv[2]) == "--render-settings";
        if (argc < 4 || (productMode && (argc < 5 || std::string(argv[4]).rfind("--", 0) == 0))) {
            std::cerr << "usage: prism_hydra_render scene.usda /Camera output.exr [width height spp [--exercise] [--animation-exercise] [--hide-dome-background] [--environment-sampling mis|bsdf] [--light-selection spatial|power] [--sample-clamp N] [--seed N] [--adaptive-threshold T] [--adaptive-min-samples N] [--time-limit SECONDS] [--gpu-texture-budget-mib N] [--sss-fallback true|false] [--subdivision-level N] [--displacement-level N] [--sphere-batching] [--no-texture-streaming] [--progressive-textures] [--render-settings /Render/Settings]]\n"
                         "       prism_hydra_render scene.usda --render-settings /Render/Settings|default OUTPUT_DIR [--spp N] [options]\n";
            return 2;
        }
        const int width = !productMode && argc > 4 ? std::stoi(argv[4]) : 128;
        const int height = !productMode && argc > 5 ? std::stoi(argv[5]) : 128;
        int spp = !productMode && argc > 6 ? std::stoi(argv[6]) : prism::defaults::kSamplesPerPixel;
        bool exercise = false;
        bool animationExercise = false;
        bool sphereBatching = false;
        bool hideDomeBackground = false;
        // GPU texture streaming: settle the levels the view asks for before
        // the first sample (deterministic final images), unless the
        // interactive restart policy is asked for; or no streaming at all.
        bool textureStreaming = true;
        bool textureStreamingPrepass = true;
        std::string sampling = "mis";
        std::string filtering = "filtered";
        float sampleClamp = 10.0f;
        float adaptiveThreshold = prism::defaults::kAdaptiveThreshold;
        int adaptiveMinSamples = prism::defaults::kAdaptiveMinSamples;
        double timeLimit = prism::defaults::kTimeLimitSeconds;
        // Settings the tool sets only when given (else authored or defaults).
        std::vector<std::pair<std::string, VtValue>> optional;
        int maxDepth = 32;
        int changeDepth = 0;
        float neeProbabilityMin = 0.0f;
        std::string lightSelection = "spatial";
        int previewSequence = 0;
        int editSequence = 0;
        UsdTimeCode renderTime = UsdTimeCode::Default();
        std::string editPrim;
        int seed = 0;
        // --denoise none|aov|replace, as the CLI's (docs/denoising.md): with an
        // explicit camera, aov adds the denoised layer, replace writes the
        // denoised colour; render products take their 'denoised' vars.
        std::string denoise = "none";
        // prism:colorManagement:* render settings, set only when given.
        std::vector<std::pair<std::string, std::string>> colorSettings;
        // Options given (product mode sets only these renderer settings).
        std::set<std::string> given;
        // The RenderSettings prim (empty: the stage's renderSettingsPrimPath).
        std::string settingsPath = productMode && std::string(argv[3]) != "default"
            ? std::string(argv[3]) : std::string();
        for (int i = productMode ? 5 : 7; i < argc; ++i) {
            const std::string option(argv[i]);
            given.insert(option);
            if (option == "--exercise") { exercise = true; continue; }
            if (option == "--animation-exercise") { animationExercise = true; continue; }
            if (option == "--sphere-batching") { sphereBatching = true; continue; }
            if (option == "--hide-dome-background") { hideDomeBackground = true; continue; }
            if (option == "--no-texture-streaming") { textureStreaming = false; continue; }
            if (option == "--progressive-textures") { textureStreamingPrepass = false; continue; }
            if (++i >= argc) throw std::runtime_error("Missing option value: " + option);
            if (option == "--working-space") colorSettings.emplace_back("workingSpace", argv[i]);
            else if (option == "--ocio-config") colorSettings.emplace_back("ocioConfig", argv[i]);
            else if (option == "--display") colorSettings.emplace_back("display", argv[i]);
            else if (option == "--view") colorSettings.emplace_back("view", argv[i]);
            else if (option == "--spp" && productMode) spp = std::stoi(argv[i]);
            else if (option == "--render-settings" && !productMode) settingsPath = argv[i];
            else if (option == "--environment-sampling") sampling = argv[i];
            else if (option == "--texture-filtering") filtering = argv[i];
            else if (option == "--sample-clamp") sampleClamp = std::stof(argv[i]);
            else if (option == "--seed") seed = std::stoi(argv[i]);
            else if (option == "--adaptive-threshold") adaptiveThreshold = std::stof(argv[i]);
            else if (option == "--adaptive-min-samples") adaptiveMinSamples = std::stoi(argv[i]);
            else if (option == "--time-limit") timeLimit = std::stod(argv[i]);
            else if (option == "--gpu-texture-budget-mib")
                optional.emplace_back("gpuTextureBudgetMiB", VtValue(std::stoi(argv[i])));
            else if (option == "--sss-fallback")
                optional.emplace_back("sssFallback", VtValue(std::string(argv[i]) == "true"));
            else if (option == "--subdivision-level")
                optional.emplace_back("subdivisionLevel", VtValue(std::stoi(argv[i])));
            else if (option == "--subdivision-max-faces")
                optional.emplace_back("subdivisionMaxFaces", VtValue(std::stoi(argv[i])));
            else if (option == "--subdivision-budget-faces")
                optional.emplace_back("subdivisionBudgetFaces", VtValue(std::stoi(argv[i])));
            else if (option == "--displacement-level")
                optional.emplace_back("displacementLevel", VtValue(std::stoi(argv[i])));
            else if (option == "--displacement-max-faces")
                optional.emplace_back("displacementMaxFaces", VtValue(std::stoi(argv[i])));
            else if (option == "--max-depth") maxDepth = std::stoi(argv[i]);
            else if (option == "--time") renderTime = UsdTimeCode(std::stod(argv[i]));
            // Moves the camera this many times after the first render and
            // reports each preview's size and cost (adaptive preview).
            else if (option == "--preview-sequence") previewSequence = std::stoi(argv[i]);
            // Moves the given prim this many times after the first render and
            // reports how long each edit takes to show (scene updates).
            else if (option == "--edit-sequence") {
                editSequence = std::stoi(argv[i]);
                if (++i >= argc) throw std::runtime_error("--edit-sequence needs a prim path");
                editPrim = argv[i];
            }
            else if (option == "--nee-probability-min") neeProbabilityMin = std::stof(argv[i]);
            else if (option == "--denoise") {
                denoise = argv[i];
                if (denoise != "none" && denoise != "aov" && denoise != "replace")
                    throw std::runtime_error("--denoise takes none, aov or replace");
            }
            else if (option == "--denoise-quality")
                optional.emplace_back("denoiseQuality", VtValue(std::string(argv[i])));
            else if (option == "--denoise-interval")
                optional.emplace_back("denoiseInterval", VtValue(std::stoi(argv[i])));
            else if (option == "--denoise-prefilter")
                optional.emplace_back("denoisePrefilter", VtValue(std::string(argv[i]) == "true"));
            else if (option == "--light-selection") lightSelection = argv[i];
            // Render, then change the maximum depth and render again: the saved
            // image must match a render made at the new depth from the start.
            else if (option == "--change-depth") changeDepth = std::stoi(argv[i]);
            else throw std::runtime_error("Unknown option: " + option);
        }
        if ((sampling != "mis" && sampling != "bsdf") ||
            (filtering != "filtered" && filtering != "bilinear") ||
            !std::isfinite(sampleClamp) || sampleClamp < 0)
            throw std::runtime_error("Invalid sampling mode or sample clamp");
        if (width <= 0 || height <= 0 || spp <= 0)
            throw std::runtime_error("Dimensions and spp must be positive");
        if (productMode && denoise != "none")
            throw std::runtime_error("--denoise takes an explicit camera and output (render products "
                                     "denoise their 'denoised' vars)");
        if (productMode && (exercise || animationExercise || previewSequence || editSequence ||
                            changeDepth || hideDomeBackground))
            throw std::runtime_error("Validation exercises take an explicit camera and output");
        prism::load_profile::Mark("cli.start");
        std::optional<prism::load_profile::Scope> openProfile(std::in_place, "cli.stage_open");
        auto stage = UsdStage::Open(argv[1]);
        openProfile.reset();
        if (!stage) throw std::runtime_error("Cannot open USD scene");
        // RenderSettings, products and vars, as the CLI resolves them
        // (src/output/render_products.h).
        std::vector<std::string> warnings;
        const auto job = prism::output::ResolveRenderJob(stage, settingsPath,
            productMode ? argv[4] : "", &warnings);
        printWarnings(warnings);
        if (productMode && (!job || job->products.empty()))
            throw std::runtime_error("The render settings have no render products to write");
        std::vector<prism::output::RenderProductRequest> views;
        if (productMode) {
            views = job->products;
        } else {
            if (!UsdGeomCamera(stage->GetPrimAtPath(SdfPath(argv[2]))))
                throw std::runtime_error("Camera path does not identify a USD camera");
            // The settings' pixel aspect ratio, conform policy and data
            // window apply at the requested size.
            prism::output::RenderProductRequest view;
            if (job) view = job->base;
            view.camera = SdfPath(argv[2]);
            view.width = width;
            view.height = height;
            if (!job) view.dataWindowNDC = GfRange2f(GfVec2f(0.0f), GfVec2f(1.0f));
            prism::output::DataWindowPixels(view.dataWindowNDC, width, height,
                &view.dataX, &view.dataY, &view.dataWidth, &view.dataHeight);
            if (view.dataWidth <= 0 || view.dataHeight <= 0)
                throw std::runtime_error("The render settings' data window is empty");
            view.output = argv[3];
            prism::output::RenderVarRequest color;
            color.name = "color";
            color.kind = prism::output::AovKind::Color;
            color.components = 4;
            view.vars.push_back(color);
            views.push_back(std::move(view));
        }
        for (auto const& view : views)
            if (!UsdGeomCamera(stage->GetPrimAtPath(view.camera)))
                throw std::runtime_error("Render product <" + view.path.GetString() + "> has no USD camera");
        // includedPurposes: default geometry always renders.
        Purposes purposes;
        if (job) {
            auto has = [&](TfToken const& purpose) {
                return std::find(job->includedPurposes.begin(), job->includedPurposes.end(), purpose) !=
                    job->includedPurposes.end();
            };
            purposes.render = has(UsdGeomTokens->render);
            purposes.proxy = has(UsdGeomTokens->proxy);
            purposes.guide = has(UsdGeomTokens->guide);
        }
        if (job) {
            // The renderer settings this tool sets: all of them with an
            // explicit camera, the given options in product mode.
            std::vector<std::pair<std::string, VtValue>> overrides;
            auto add = [&](const char* option, const char* name, VtValue value) {
                if (!productMode || given.count(option)) overrides.emplace_back(name, std::move(value));
            };
            add("--spp", "samplesPerPixel", VtValue(spp));
            add("--max-depth", "maxDepth", VtValue(maxDepth));
            add("--sample-clamp", "maxSampleRadiance", VtValue(sampleClamp));
            add("--seed", "seed", VtValue(seed));
            add("--adaptive-threshold", "adaptiveThreshold", VtValue(adaptiveThreshold));
            add("--adaptive-min-samples", "adaptiveMinSamples", VtValue(adaptiveMinSamples));
            add("--time-limit", "timeLimitSeconds", VtValue(timeLimit));
            for (auto const& [name, value] : optional) overrides.emplace_back(name, value);
            add("--nee-probability-min", "neeProbabilityMin", VtValue(neeProbabilityMin));
            add("--environment-sampling", "environmentSampling", VtValue(sampling));
            add("--texture-filtering", "textureFiltering", VtValue(filtering));
            add("--light-selection", "lightSelection", VtValue(lightSelection));
            for (auto const& [name, value] : colorSettings)
                overrides.emplace_back("colorManagement:" + name, VtValue(value));
            overrideAuthoredSettings(stage, job->settings, overrides);
        }
        UsdImagingGLEngine::Parameters parameters;
        parameters.gpuEnabled = false;
        parameters.rendererPluginId = TfToken("HdPrismRendererPlugin");
        std::optional<prism::load_profile::Scope> engineProfile(std::in_place, "cli.engine_create");
        UsdImagingGLEngine engine(parameters);
        engineProfile.reset();
        if (engine.GetCurrentRendererId() != parameters.rendererPluginId)
            throw std::runtime_error("PrismRT plugin not loaded; check PXR_PLUGINPATH_NAME and DLL paths");
        // The delegate reads prism:* settings and materialBindingPurposes
        // from the active settings prim.
        if (job) engine.SetActiveRenderSettingsPrimPath(job->settings);
        // Product mode sets only the given options: the rest come from the
        // settings prim's prism:* attributes or the delegate's defaults.
        auto setting = [&](const char* option, const char* name, VtValue value) {
            if (!productMode || given.count(option))
                engine.SetRendererSetting(TfToken(name), std::move(value));
        };
        setting("--spp", "prism:samplesPerPixel", VtValue(spp));
        setting("--max-depth", "prism:maxDepth", VtValue(maxDepth));
        setting("--rr-min-depth", "prism:rrMinDepth", VtValue(4));
        setting("--sample-clamp", "prism:maxSampleRadiance", VtValue(sampleClamp));
        setting("--environment-sampling", "prism:environmentSampling", VtValue(TfToken(sampling)));
        setting("--texture-filtering", "prism:textureFiltering", VtValue(TfToken(filtering)));
        setting("--seed", "prism:seed", VtValue(seed));
        setting("--adaptive-threshold", "prism:adaptiveThreshold", VtValue(adaptiveThreshold));
        setting("--adaptive-min-samples", "prism:adaptiveMinSamples", VtValue(adaptiveMinSamples));
        setting("--time-limit", "prism:timeLimitSeconds", VtValue(timeLimit));
        for (auto const& [name, value] : optional)
            engine.SetRendererSetting(TfToken("prism:" + name), value);
        setting("--nee-probability-min", "prism:neeProbabilityMin", VtValue(neeProbabilityMin));
        setting("--light-selection", "prism:lightSelection", VtValue(TfToken(lightSelection)));
        engine.SetRendererSetting(TfToken("prism:sphereBatching"), VtValue(sphereBatching));
        engine.SetRendererSetting(TfToken("prism:textureStreaming"), VtValue(textureStreaming));
        engine.SetRendererSetting(TfToken("prism:textureStreamingPrepass"), VtValue(textureStreamingPrepass));
        for (auto const& [name, value] : colorSettings)
            engine.SetRendererSetting(TfToken("prism:colorManagement:" + name), VtValue(value));
        engine.SetEnablePresentation(false);
        // Frames a view: a plain viewport for a full window at pixel aspect
        // ratio 1 (as before render settings), else the display and data
        // windows with the conform policy (CameraUtilFraming, as the CLI).
        auto frame = [&](prism::output::RenderProductRequest const& view) {
            engine.SetCameraPath(view.camera);
            if (view.FullDataWindow() && view.pixelAspectRatio == 1.0f &&
                prism::output::ConformPolicy(view.aspectRatioConformPolicy) == CameraUtilFit) {
                engine.SetRenderViewport(GfVec4d(0, 0, view.width, view.height));
                return;
            }
            engine.SetFraming(CameraUtilFraming(
                GfRange2f(GfVec2f(0.0f), GfVec2f(float(view.width), float(view.height))),
                GfRect2i(GfVec2i(view.dataX, view.dataY), view.dataWidth, view.dataHeight),
                view.pixelAspectRatio));
            engine.SetOverrideWindowPolicy(prism::output::ConformPolicy(view.aspectRatioConformPolicy));
            engine.SetRenderBufferSize(GfVec2i(view.dataWidth, view.dataHeight));
        };
        if (productMode) {
            std::vector<prism::output::RenderedAov> rendered;
            for (size_t index = 0; index < views.size(); ++index) {
                auto const& view = views[index];
                if (index == 0 || !view.SameView(views[index - 1])) {
                    // Every AOV of the products sharing this view, colour first.
                    std::vector<prism::output::AovKind> kinds{prism::output::AovKind::Color};
                    for (size_t other = index; other < views.size() && views[other].SameView(view); ++other)
                        for (auto const& var : views[other].vars)
                            if (std::find(kinds.begin(), kinds.end(), var.kind) == kinds.end())
                                kinds.push_back(var.kind);
                    TfTokenVector aovs;
                    for (auto kind : kinds)
                        aovs.emplace_back(std::string(prism::output::FindAov(kind)->name));
                    frame(view);
                    engine.SetRendererAovs(aovs);
                    render(engine, stage, renderTime, true, purposes);
                    rendered.clear();
                    for (auto kind : kinds)
                        rendered.push_back(readAov(engine, kind, view.dataWidth, view.dataHeight));
                }
                std::vector<std::string> productWarnings;
                const auto image = prism::output::BuildProductImage(view, rendered, &productWarnings);
                if (!image.layers.empty())
                    prism::output::WriteImage(view.output, image, *prism::color::Active(), &productWarnings);
                printWarnings(productWarnings);
                if (image.layers.empty()) {
                    std::cerr << "prism_hydra_render: nothing to write to " << view.output << '\n';
                    continue;
                }
                std::cout << "Hydra render converged and saved " << view.output << '\n';
            }
            return 0;
        }
        auto const& view = views.front();
        frame(view);
        {
            TfTokenVector aovs{HdAovTokens->color, HdAovTokens->depth,
                HdAovTokens->cameraDepth, HdAovTokens->normal, HdAovTokens->Neye,
                HdAovTokens->primId, HdAovTokens->instanceId, HdAovTokens->elementId};
            if (denoise != "none") aovs.emplace_back("denoised");
            engine.SetRendererAovs(aovs);
        }
        if (animationExercise) {
            runAnimationExercise(argv[1], argv[2], width, height, spp);
            std::cout << "Hydra animation exercise passed\n";
            return 0;
        }
        render(engine, stage, renderTime, !hideDomeBackground, purposes);
        for (int step = 1; step <= previewSequence; ++step) {
            UsdGeomXformable camera(stage->GetPrimAtPath(SdfPath(argv[2])));
            bool resets = false;
            GfMatrix4d transform;
            camera.GetLocalTransformation(&transform, &resets);
            transform.SetTranslateOnly(transform.ExtractTranslation() + GfVec3d(0.002, 0.0, 0.0));
            camera.ClearXformOpOrder();
            camera.AddTransformOp().Set(transform);
            render(engine, stage, renderTime, !hideDomeBackground, purposes);
            const auto stats = engine.GetRenderStats();
            const auto stat = [&](const char* name) {
                auto found = stats.find(name);
                return found == stats.end() ? std::string("?") : TfStringify(found->second);
            };
            std::cout << "Preview " << step << ": " << stat("previewWidth") << "x" << stat("previewHeight")
                      << ", cost " << stat("previewCostMilliseconds") << " ms\n";
        }
        for (int step = 1; step <= editSequence; ++step) {
            UsdGeomXformable prim(stage->GetPrimAtPath(SdfPath(editPrim)));
            if (!prim) throw std::runtime_error("--edit-sequence prim is not transformable");
            bool resets = false;
            GfMatrix4d transform;
            prim.GetLocalTransformation(&transform, &resets);
            transform.SetTranslateOnly(transform.ExtractTranslation() + GfVec3d(0.02, 0.0, 0.0));
            const auto start = std::chrono::steady_clock::now();
            prim.ClearXformOpOrder();
            prim.AddTransformOp().Set(transform);
            render(engine, stage, renderTime, !hideDomeBackground, purposes);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const auto stats = engine.GetRenderStats();
            const auto stat = [&](const char* name) {
                auto found = stats.find(name);
                return found == stats.end() ? std::string("?") : TfStringify(found->second);
            };
            std::cout << "Edit " << step << ": converged after " << seconds << " s, snapshot "
                      << stat("snapshotBuildSeconds") << " s, preview " << stat("previewMilliseconds") << " ms\n";
        }
        if (changeDepth > 0) {
            engine.SetRendererSetting(TfToken("prism:maxDepth"), VtValue(changeDepth));
            render(engine, stage, renderTime, !hideDomeBackground, purposes);
        }
        for (const auto& [name,value] : engine.GetRenderStats())
            std::cout << "Render stat " << name << ": " << value << '\n';
        // Some SDK/plugin shutdown paths terminate the process without
        // flushing fully buffered redirected stdout.  Publish the structured
        // statistics before image encoding so automated validators can rely
        // on them even in those runtimes.
        std::cout.flush();
        const int outputWidth = view.dataWidth, outputHeight = view.dataHeight;
        const auto initial = pixels(engine, outputWidth, outputHeight);
        const size_t backgroundAlpha = 3;
        const size_t centerAlpha = (size_t(outputHeight / 2) * outputWidth + outputWidth / 2) * 4 + 3;
        if (hideDomeBackground) {
            if (initial[backgroundAlpha] != 0.0f || initial[centerAlpha] < 0.99f)
                throw std::runtime_error(
                    "Hidden-dome export must keep background transparent and geometry opaque");
        }
        std::cout << "Center normal: "
                  << center<GfVec3f>(engine, HdAovTokens->normal, HdFormatFloat32Vec3)
                  << "; camera depth: "
                  << center<float>(engine, HdAovTokens->cameraDepth, HdFormatFloat32) << '\n';
        {
            std::vector<std::string> outputWarnings;
            std::vector<prism::output::RenderedAov> outputs{{prism::output::AovKind::Color, 4, initial}};
            auto outputView = view;
            if (denoise != "none") {
                auto denoised = readAov(engine, prism::output::AovKind::Denoised, outputWidth, outputHeight);
                if (denoise == "replace") {
                    outputs.front().pixels = std::move(denoised.pixels);
                } else {
                    auto var = outputView.vars.front();
                    var.name = "denoised";
                    var.kind = prism::output::AovKind::Denoised;
                    outputView.vars.push_back(var);
                    outputs.push_back(std::move(denoised));
                }
            }
            const auto image = prism::output::BuildProductImage(outputView, outputs, &outputWarnings);
            prism::output::WriteImage(argv[3], image, *prism::color::Active(), &outputWarnings);
            printWarnings(outputWarnings);
        }
        if (hideDomeBackground) {
            OIIO::ImageBuf saved(argv[3]);
            if (!saved.read(0, 0, true, OIIO::TypeDesc::FLOAT))
                throw std::runtime_error("Cannot read back transparent export: " + saved.geterror());
            if (saved.spec().nchannels != 4)
                throw std::runtime_error("Transparent export did not preserve four color channels");
            std::vector<float> exported(size_t(outputWidth) * outputHeight * 4);
            if (!saved.get_pixels(OIIO::ROI(), OIIO::TypeDesc::FLOAT, exported.data()))
                throw std::runtime_error("Cannot inspect exported color channels: " + saved.geterror());
            if (exported[backgroundAlpha] != 0.0f || exported[centerAlpha] < 0.99f)
                throw std::runtime_error(
                    "Saved export must retain transparent background and opaque geometry");
        }
        if (exercise) {
            checkFixtureAovs(engine);
            // Same scene + seed must remain stable when asked to render again.
            render(engine, stage, UsdTimeCode::Default(), true, purposes);
            if (pixels(engine, outputWidth, outputHeight) != initial)
                throw std::runtime_error("Converged image changed without a scene edit");
            auto panel = UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Panel")));
            if (!panel) throw std::runtime_error("Exercise mode requires parity fixture /World/Panel");
            panel.MakeInvisible();
            render(engine, stage, UsdTimeCode::Default(), true, purposes);
            if (pixels(engine, outputWidth, outputHeight) == initial)
                throw std::runtime_error("Visibility edit did not invalidate rendering");
            panel.MakeVisible();
            render(engine, stage, UsdTimeCode::Default(), true, purposes);
            if (pixels(engine, outputWidth, outputHeight) != initial)
                throw std::runtime_error("Restoring visibility did not reproduce deterministic rendering");
            engine.SetRenderViewport(GfVec4d(0,0,32,24));
            render(engine, stage, UsdTimeCode::Default(), true, purposes);
            pixels(engine,32,24);
            stage->RemovePrim(SdfPath("/World/Panel"));
            render(engine, stage, UsdTimeCode::Default(), true, purposes);
            pixels(engine,32,24);
        }
        std::cout << "Hydra render converged and saved " << argv[3] << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Hydra validation: " << e.what() << '\n';
        return 1;
    }
}
