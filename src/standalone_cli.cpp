// The standalone CLI: argument parsing (flags, UsdRender settings and
// products) in front of the public API: a Renderer, a Scene the loaders fill,
// a Session that renders each view, and prism_io (through the products'
// writer) for the files.
#include "standalone_cli.h"
#include <prism/gltf.h>
#include <prism/io.h>
#include <prism/obj.h>
#include <prism/renderer.h>
#include <prism/session.h>
#include <prism/usd.h>
#include "render_defaults.h"
// Internal: option parsing helpers (device and denoise names), the loading
// progress model and the GPU program report (diagnostic hooks).
#include "scene/scene_internal.h"
#include "device/device.h"
#include "color/color_management.h"
#include "renderer/renderer_context.h"
#include "renderer/install_paths.h"
#include "output/render_products.h"
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/gf/camera.h>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <array>
#include <sstream>
#include <chrono>
#include <set>
#include <tuple>
#include <type_traits>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "progress.h"
#include "render_profile.h"
PXR_NAMESPACE_USING_DIRECTIVE

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
// usdview's default dome light texture (Hdx's StinsonBeach.hdr), or empty.
std::string DefaultDomeTexture()
{
    const PXR_NS::PlugPluginPtr hdx = PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hdx");
    return hdx ? hdx->FindPluginResource("textures/StinsonBeach.hdr", true) : std::string();
}

struct Options {
    std::string scene, camera, environment, output = "output.exr";
    // The material preview (--material, --material-name): a MaterialX
    // document on the built-in shaderball instead of a scene.
    std::string material, materialName;
    std::string environmentSampling = "mis", textureFiltering = "filtered";
    // UsdRender settings prim (empty: the stage's renderSettingsPrimPath) and
    // the directory its relative product names resolve against.
    std::string renderSettings, outputDirectory;
    prism::device::DevicePreference device = prism::device::DevicePreference::Auto;
    std::optional<Float3> cameraPosition, cameraTarget;
    std::optional<Color3> environmentColor;
    Float3 cameraUp{0, 1, 0};
    float verticalFov = 40, focusDistance = 10, defocusAngle = 0;
    bool explicitLens = false, widthSet = false, heightSet = false;
    bool honorDoubleSided = false; // prism:honorDoubleSided
    bool defaultDome = true; // light scenes without lights like usdview's default dome
    int width = 1280, height = 720, samples = prism::defaults::kSamplesPerPixel, depth = 32, roulette = 4;
    unsigned seed = 0;
    float exposure = 0, clamp = 10;
    float adaptiveThreshold = prism::defaults::kAdaptiveThreshold;
    int adaptiveMinSamples = prism::defaults::kAdaptiveMinSamples;
    double timeLimit = prism::defaults::kTimeLimitSeconds;
    bool textureStreaming = true;
    // Defaults from PRISM_GPU_TEXTURE_BUDGET_MIB and PRISM_SSS_FALLBACK.
    int gpuTextureBudgetMiB = prism::defaults::FromEnvironment().gpuTextureBudgetMiB;
    bool sssFallback = prism::defaults::FromEnvironment().sssFallback;
    // Subdivision surfaces and displacement (prism:subdivisionLevel etc.;
    // defaults from PRISM_SUBDIVISION_LEVEL etc.).
    int subdivisionLevel = prism::defaults::FromEnvironment().subdivisionLevel;
    int subdivisionMaxFaces = prism::defaults::FromEnvironment().subdivisionMaxFaces;
    int subdivisionBudgetFaces = prism::defaults::FromEnvironment().subdivisionBudgetFaces;
    int displacementLevel = prism::defaults::FromEnvironment().displacementLevel;
    int displacementMaxFaces = prism::defaults::FromEnvironment().displacementMaxFaces;
    // The USD time code scenes load at (--time; default: the default time).
    std::optional<double> time;
    bool progress = true; // --no-progress
    // --denoise: none, aov (the image also gets a denoised layer) or replace
    // (its colour is the denoised colour); prism:denoise true is replace.
    std::string denoise = "none";
    prism::device::DenoiseSettings denoiseSettings;
    unsigned contributions = 7;
    prism::device::NeeSettings nee;
    prism::color::Options color;
    // Options given on the command line (they take precedence over prism:*
    // render settings).
    std::set<std::string> given;
};

void help() {
    std::cout << "PrismRT --scene FILE --camera /Camera [options]\n"
        "PrismRT --scene FILE --camera-position x,y,z --camera-target x,y,z [options]\n"
        "PrismRT --material FILE.mtlx [--material-name NAME] [options]\n"
        "  Supported scene formats: USD, glTF, GLB, OBJ\n"
        "  --material renders a MaterialX document on the built-in shaderball (the USD-WG\n"
        "                 StandardShaderBall in its own lit box, with its camera; default 512x512;\n"
        "                 docs/material_preview.md). --material-name picks one of the document's\n"
        "                 materials (default: the first)\n"
        "  --camera-up 0,1,0 --vfov 40 --focus-distance 10 --defocus-angle 0\n"
        "  --env FILE --env-exposure STOPS (or --env-color r,g,b)\n"
        "  --width 1280 --height 720 --spp 2048 (with adaptive sampling, the per-pixel maximum)\n"
        "  --max-depth 32 --rr-min-depth 4 --seed 0\n"
        "  --max-sample-radiance 10 --output output.exr\n"
        "  --render-settings /Render/Settings (default: the stage's renderSettingsPrimPath)\n"
        "  --output-dir DIR (where relative render product names are written; default .)\n"
        "                 USD scenes with RenderSettings take resolution, camera, pixel aspect\n"
        "                 ratio, data window, material binding purposes and prism:* settings from\n"
        "                 it; without --output or a camera option, each RenderProduct is written\n"
        "                 with its RenderVars (see docs/hydra.md)\n"
        "  --adaptive-threshold 0.01 (0 disables: every pixel gets --spp samples)\n"
        "  --adaptive-min-samples 64 (samples before a pixel may stop)\n"
        "  --time-limit SECONDS (default 0: none; sampling stops at the first sample past it,\n"
        "                 at least one sample per pixel; scene loading is not counted)\n"
        "                 Rendering stops at whichever comes first: the threshold, --spp, the limit\n"
        "  --texture-streaming true|false (default true: GPU devices that stream textures load\n"
        "                 the mip levels shading asks for; false uploads whole textures)\n"
        "  --gpu-texture-budget-mib N (default 0: 45% of device memory; textures beyond it upload at\n"
        "                 reduced resolution; default $PRISM_GPU_TEXTURE_BUDGET_MIB)\n"
        "  --no-progress (no progress line on stderr; on a terminal it updates in place, otherwise\n"
        "                 a line per 10%)\n"
        "  --sss-fallback true|false (default false: subsurface scattering is traced; true renders\n"
        "                 its diffuse lobe instead; default $PRISM_SSS_FALLBACK)\n"
        "  --time T (USD time code of animated attributes, transforms and skinning; default: the\n"
        "                 default time)\n"
        "  --subdivision-level 2 --subdivision-max-faces 65536 --subdivision-budget-faces 25165824\n"
        "                 (subdivision surfaces refine uniformly to the level, each mesh within the\n"
        "                 maximum faces and all of them within the budget; 0 renders control hulls)\n"
        "  --displacement-level 4 --displacement-max-faces 262144 (meshes their material displaces)\n"
        "                 Defaults: $PRISM_SUBDIVISION_LEVEL etc. (docs/hydra.md)\n"
        "  Options take precedence over the RenderSettings prim's prism:* settings, which take\n"
        "  precedence over PRISM_* environment variables (docs/hydra.md)\n"
        "  --debug-contributions all|visible|direct|indirect (noise analysis: keep only light seen\n"
        "                 directly, reaching the first hit, or bounced more often; comma-separated)\n"
        "  --environment-sampling mis|bsdf (default mis)\n"
        "  --nee-probability-min 0 --nee-from-depth 2 (from that depth on, lights are\n"
        "                 sampled with probability clamp(path weight, min, 1); min 0 always samples)\n"
        "  --light-selection spatial|power (default spatial: analytic lights by their estimated\n"
        "                 irradiance at the shading point, mixed with power; power: by power only)\n"
        "  --texture-filtering filtered|bilinear (default filtered)\n"
        "  --denoise none|aov|replace (default none; docs/denoising.md): aov adds a 'denoised' layer\n"
        "                 to the EXR output, replace writes the denoised colour instead of the raw one\n"
        "                 (render products: their colour vars). 'denoised' render vars are always\n"
        "                 denoised. prism:denoise true in the render settings means replace\n"
        "  --denoise-quality fast|balanced|high (default high)\n"
        "  --denoise-prefilter true|false (default true: denoise the albedo and normal guides first)\n"
        "  --honor-double-sided true|false (default false: USD doubleSided is ignored; true: back faces of\n"
        "                 single-sided meshes are not hit, unless their material transmits)\n"
        "  --default-dome true|false (default true: a scene with no lights, emissive geometry or\n"
        "                 environment is lit by USD's default dome, as in usdview)\n"
        "  --device auto|cpu|vulkan|metal (default auto: GPU if available, else CPU)\n"
        "  --ocio-config FILE|URI (default $OCIO, else ocio://studio-config-latest)\n"
        "  --working-space NAME (default $PRISM_WORKING_SPACE, else the render settings'\n"
        "                 renderingColorSpace, else ACEScg;\n"
        "                 lin_rec709 renders in linear Rec.709 as before)\n"
        "  --display NAME --view NAME (8-bit outputs; default the config's: sRGB - Display and\n"
        "                 ACES 2.0 - SDR 100 nits (Rec.709). The earlier gamma 2.2 output is\n"
        "                 --display \"Gamma 2.2 Rec.709 - Display\" --view Un-tone-mapped)\n"
        "PrismRT --list-devices\n"
        "EXR output is linear in the working space after the sample clamp; other formats go\n"
        "through the display and view. Light colors are linear Rec.709.\n"
        "--env lights like an untransformed USD DomeLight of FILE at intensity 1: radiance is the\n"
        "texel (linear Rec.709) times 2^--env-exposure. --env-color r,g,b is a constant dome of\n"
        "that radiance (also times 2^--env-exposure).\n"
        "Without --env, USD scenes use their authored dome lighting, and a scene with no lights,\n"
        "emissive geometry or environment gets USD's default dome (--default-dome false: black).\n";
}

int integer(const std::string& s) {
    int v;
    auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (error != std::errc() || end != s.data() + s.size())
        throw std::runtime_error("Invalid integer: " + s);
    return v;
}
float number(const std::string& s) {
    size_t used = 0;
    float v = std::stof(s, &used);
    if (used != s.size() || !std::isfinite(v))
        throw std::runtime_error("Invalid finite number: " + s);
    return v;
}

bool boolean(const std::string& key, const std::string& s) {
    if (s != "true" && s != "false") throw std::runtime_error(key + " takes true or false");
    return s == "true";
}

Float3 triple(std::string s) {
    std::replace(s.begin(), s.end(), ',', ' ');
    std::istringstream input(s);
    std::string x, y, z, extra;
    if (!(input >> x >> y >> z) || (input >> extra))
        throw std::runtime_error("Expected three coordinates: " + s);
    return Float3(number(x), number(y), number(z));
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--no-progress") { o.progress = false; continue; }
        if (i + 1 == argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[++i];
        o.given.insert(key);
        if (key == "--scene") o.scene = value;
        else if (key == "--material") o.material = value;
        else if (key == "--material-name") o.materialName = value;
        else if (key == "--camera") o.camera = value;
        else if (key == "--camera-position") o.cameraPosition = triple(value);
        else if (key == "--camera-target") o.cameraTarget = triple(value);
        else if (key == "--camera-up") { o.cameraUp = triple(value); o.explicitLens = true; }
        else if (key == "--vfov") { o.verticalFov = number(value); o.explicitLens = true; }
        else if (key == "--focus-distance") { o.focusDistance = number(value); o.explicitLens = true; }
        else if (key == "--defocus-angle") { o.defocusAngle = number(value); o.explicitLens = true; }
        else if (key == "--environment-sampling") o.environmentSampling = value;
        else if (key == "--texture-filtering") o.textureFiltering = value;
        else if (key == "--honor-double-sided") {
            if (value != "true" && value != "false")
                throw std::runtime_error("--honor-double-sided takes true or false");
            o.honorDoubleSided = value == "true";
        }
        else if (key == "--default-dome") {
            if (value != "true" && value != "false")
                throw std::runtime_error("--default-dome takes true or false");
            o.defaultDome = value == "true";
        }
        else if (key == "--device") {
            if (!prism::device::ParseDevicePreference(value, &o.device)) {
                const std::string removed = prism::device::RemovedDeviceMessage(value);
                throw std::runtime_error(removed.empty() ? "--device must be auto, cpu, vulkan or metal"
                                                         : "--device: " + removed);
            }
        }
        else if (key == "--env") o.environment = value;
        else if (key == "--env-color") o.environmentColor = triple(value);
        else if (key == "--output") o.output = value;
        else if (key == "--render-settings") o.renderSettings = value;
        else if (key == "--output-dir") o.outputDirectory = value;
        else if (key == "--ocio-config") o.color.config = value;
        else if (key == "--working-space") o.color.workingSpace = value;
        else if (key == "--display") o.color.display = value;
        else if (key == "--view") o.color.view = value;
        else if (key == "--width") { o.width = integer(value); o.widthSet = true; }
        else if (key == "--height") { o.height = integer(value); o.heightSet = true; }
        else if (key == "--spp") o.samples = integer(value);
        else if (key == "--max-depth") o.depth = integer(value);
        else if (key == "--rr-min-depth") o.roulette = integer(value);
        else if (key == "--seed") {
            auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), o.seed);
            if (error != std::errc() || end != value.data() + value.size())
                throw std::runtime_error("Invalid seed: " + value);
        }
        else if (key == "--env-exposure") o.exposure = number(value);
        else if (key == "--max-sample-radiance") o.clamp = number(value);
        else if (key == "--adaptive-threshold") o.adaptiveThreshold = number(value);
        else if (key == "--adaptive-min-samples") o.adaptiveMinSamples = integer(value);
        else if (key == "--time-limit") o.timeLimit = number(value);
        else if (key == "--texture-streaming") o.textureStreaming = boolean(key, value);
        else if (key == "--denoise") {
            if (value != "none" && value != "aov" && value != "replace")
                throw std::runtime_error("--denoise takes none, aov or replace");
            o.denoise = value;
        }
        else if (key == "--denoise-quality") {
            if (!prism::device::ParseDenoiseQuality(value, &o.denoiseSettings.quality))
                throw std::runtime_error("--denoise-quality takes fast, balanced or high");
        }
        else if (key == "--denoise-prefilter") o.denoiseSettings.prefilter = boolean(key, value);
        else if (key == "--gpu-texture-budget-mib") o.gpuTextureBudgetMiB = integer(value);
        else if (key == "--sss-fallback") o.sssFallback = boolean(key, value);
        else if (key == "--subdivision-level") o.subdivisionLevel = integer(value);
        else if (key == "--subdivision-max-faces") o.subdivisionMaxFaces = integer(value);
        else if (key == "--subdivision-budget-faces") o.subdivisionBudgetFaces = integer(value);
        else if (key == "--displacement-level") o.displacementLevel = integer(value);
        else if (key == "--displacement-max-faces") o.displacementMaxFaces = integer(value);
        else if (key == "--time") {
            size_t used = 0;
            const double t = std::stod(value, &used);
            if (used != value.size() || !std::isfinite(t)) throw std::runtime_error("Invalid time code: " + value);
            o.time = t;
        }
        else if (key == "--nee-probability-min") o.nee.probabilityMin = number(value);
        else if (key == "--nee-from-depth") o.nee.fromDepth = integer(value);
        else if (key == "--light-selection") {
            if (value != "spatial" && value != "power")
                throw std::runtime_error("--light-selection takes spatial or power");
            o.nee.spatialLights = value == "spatial";
        }
        else if (key == "--debug-contributions") {
            o.contributions = 0;
            std::stringstream list(value);
            for (std::string part; std::getline(list, part, ',');) {
                if (part == "all") o.contributions |= 7;
                else if (part == "visible") o.contributions |= 1;
                else if (part == "direct") o.contributions |= 2;
                else if (part == "indirect") o.contributions |= 4;
                else throw std::runtime_error("--debug-contributions takes all, visible, direct or indirect");
            }
        }
        else throw std::runtime_error("Unknown option: " + key);
    }
    if (!o.materialName.empty() && o.material.empty())
        throw std::runtime_error("--material-name requires --material");
    if (!o.material.empty()) {
        // The preview has its scene, camera and settings.
        for (const char* flag : {"--scene", "--camera", "--camera-position", "--camera-target", "--camera-up",
                                 "--vfov", "--focus-distance", "--defocus-angle", "--render-settings", "--output-dir"})
            if (o.given.count(flag))
                throw std::runtime_error(std::string("--material renders the built-in preview scene; ") + flag +
                                         " does not apply");
        if (!o.widthSet) o.width = 512;
        if (!o.heightSet) o.height = 512;
        return o;
    }
    // The camera may also come from the stage's render settings.
    if (o.scene.empty())
        throw std::runtime_error("--scene and an explicit camera source, or --material, are required (see --help)");
    const bool explicitCamera = o.cameraPosition.has_value() && o.cameraTarget.has_value();
    if ((!o.camera.empty() && (o.cameraPosition || o.cameraTarget || o.explicitLens)) ||
        (o.cameraPosition.has_value() != o.cameraTarget.has_value()))
        throw std::runtime_error("Choose --camera or both --camera-position and --camera-target");
    if (explicitCamera && (o.verticalFov <= 0 || o.verticalFov >= 179 ||
        o.focusDistance <= 0 || o.defocusAngle < 0 || o.defocusAngle >= 179 ||
        (*o.cameraPosition - *o.cameraTarget).is_near_zero() ||
        cross(*o.cameraPosition - *o.cameraTarget, o.cameraUp).is_near_zero()))
        throw std::runtime_error("Invalid camera basis or lens parameters");
    return o;
}

// Range checks, after the render settings filled what the command line left.
void validate(Options const& o) {
    if (o.textureFiltering != "filtered" && o.textureFiltering != "bilinear")
        throw std::runtime_error("--texture-filtering must be filtered or bilinear");
    if (o.environmentSampling != "mis" && o.environmentSampling != "bsdf")
        throw std::runtime_error("--environment-sampling must be mis or bsdf");
    if (o.environmentColor && (!o.environment.empty() || o.environmentColor->x() < 0 ||
        o.environmentColor->y() < 0 || o.environmentColor->z() < 0))
        throw std::runtime_error("--env-color requires nonnegative linear RGB and conflicts with --env");
    if (o.width <= 0 || o.height <= 0 || o.samples <= 0 || o.depth <= 0 || o.roulette < 0 || o.clamp < 0)
        throw std::runtime_error("Dimensions, spp and depth must be positive; roulette depth and clamp must be nonnegative");
    if (!(o.nee.probabilityMin >= 0 && o.nee.probabilityMin <= 1) || o.nee.fromDepth < 0)
        throw std::runtime_error("--nee-probability-min must be in [0, 1] and --nee-from-depth nonnegative");
    if (o.adaptiveThreshold < 0 || o.adaptiveMinSamples < 1)
        throw std::runtime_error("--adaptive-threshold must be nonnegative and --adaptive-min-samples positive");
    if (!(o.timeLimit >= 0) || !std::isfinite(o.timeLimit))
        throw std::runtime_error("--time-limit must be a nonnegative number of seconds");
    if (o.gpuTextureBudgetMiB < 0)
        throw std::runtime_error("--gpu-texture-budget-mib must be nonnegative (0: automatic)");
    if (o.subdivisionLevel < 0 || o.displacementLevel < 0 || o.subdivisionMaxFaces < 1 ||
        o.subdivisionBudgetFaces < 1 || o.displacementMaxFaces < 1)
        throw std::runtime_error("Subdivision and displacement levels must be nonnegative and their face limits positive");
    if (static_cast<uint64_t>(o.width) * o.height > std::numeric_limits<unsigned>::max() / 4)
        throw std::runtime_error("Image dimensions are too large");
}

// The Hydra delegate's prism:* render settings that the CLI honours, from
// the RenderSettings prim; command-line options take precedence.
void applyRenderSettings(VtDictionary const& settings, Options& o) {
    using prism::output::SettingText;
    auto value = [&](const char* name) -> VtValue const* {
        const auto found = settings.find(std::string("prism:") + name);
        return found == settings.end() ? nullptr : &found->second;
    };
    auto integerSetting = [&](const char* name, const char* flag, auto* field) {
        if (o.given.count(flag)) return;
        if (auto const* v = value(name); v && v->CanCast<int>())
            *field = static_cast<std::remove_pointer_t<decltype(field)>>(
                VtValue::Cast<int>(*v).UncheckedGet<int>());
    };
    auto floatSetting = [&](const char* name, const char* flag, float* field) {
        if (o.given.count(flag)) return;
        if (auto const* v = value(name); v && v->CanCast<float>())
            *field = VtValue::Cast<float>(*v).UncheckedGet<float>();
    };
    auto textSetting = [&](std::string const& name, const char* flag) -> std::string {
        return o.given.count(flag) ? std::string() : SettingText(settings, "prism:" + name);
    };
    integerSetting("samplesPerPixel", "--spp", &o.samples);
    integerSetting("maxDepth", "--max-depth", &o.depth);
    integerSetting("rrMinDepth", "--rr-min-depth", &o.roulette);
    integerSetting("seed", "--seed", &o.seed);
    floatSetting("maxSampleRadiance", "--max-sample-radiance", &o.clamp);
    floatSetting("adaptiveThreshold", "--adaptive-threshold", &o.adaptiveThreshold);
    integerSetting("adaptiveMinSamples", "--adaptive-min-samples", &o.adaptiveMinSamples);
    floatSetting("neeProbabilityMin", "--nee-probability-min", &o.nee.probabilityMin);
    integerSetting("neeFromDepth", "--nee-from-depth", &o.nee.fromDepth);
    if (auto mode = textSetting("lightSelection", "--light-selection"); !mode.empty()) {
        if (mode != "spatial" && mode != "power")
            throw std::runtime_error("prism:lightSelection takes spatial or power");
        o.nee.spatialLights = mode == "spatial";
    }
    if (auto mode = textSetting("environmentSampling", "--environment-sampling"); !mode.empty())
        o.environmentSampling = mode;
    if (auto mode = textSetting("textureFiltering", "--texture-filtering"); !mode.empty())
        o.textureFiltering = mode;
    auto booleanSetting = [&](const char* name, const char* flag, bool* field) {
        if (o.given.count(flag)) return;
        if (auto const* v = value(name); v && v->IsHolding<bool>()) *field = v->UncheckedGet<bool>();
    };
    booleanSetting("honorDoubleSided", "--honor-double-sided", &o.honorDoubleSided);
    // Denoising, as the delegate's: prism:denoise shows the denoised colour.
    bool denoise = false;
    booleanSetting("denoise", "--denoise", &denoise);
    if (denoise) o.denoise = "replace";
    if (auto quality = textSetting("denoiseQuality", "--denoise-quality"); !quality.empty() &&
        !prism::device::ParseDenoiseQuality(quality, &o.denoiseSettings.quality))
        throw std::runtime_error("prism:denoiseQuality takes fast, balanced or high");
    booleanSetting("denoisePrefilter", "--denoise-prefilter", &o.denoiseSettings.prefilter);
    if (!o.given.count("--time-limit"))
        if (auto const* v = value("timeLimitSeconds"); v && v->CanCast<double>())
            o.timeLimit = VtValue::Cast<double>(*v).UncheckedGet<double>();
    booleanSetting("textureStreaming", "--texture-streaming", &o.textureStreaming);
    integerSetting("gpuTextureBudgetMiB", "--gpu-texture-budget-mib", &o.gpuTextureBudgetMiB);
    booleanSetting("sssFallback", "--sss-fallback", &o.sssFallback);
    // Mesh conversion, as the delegate's.
    integerSetting("subdivisionLevel", "--subdivision-level", &o.subdivisionLevel);
    integerSetting("subdivisionMaxFaces", "--subdivision-max-faces", &o.subdivisionMaxFaces);
    integerSetting("subdivisionBudgetFaces", "--subdivision-budget-faces", &o.subdivisionBudgetFaces);
    integerSetting("displacementLevel", "--displacement-level", &o.displacementLevel);
    integerSetting("displacementMaxFaces", "--displacement-max-faces", &o.displacementMaxFaces);
    if (auto name = textSetting("device", "--device"); !name.empty() &&
        !prism::device::ParseDevicePreference(name, &o.device)) {
        const std::string removed = prism::device::RemovedDeviceMessage(name);
        throw std::runtime_error(removed.empty() ? "prism:device must be auto, cpu, vulkan or metal"
                                                 : "prism:device: " + removed);
    }
    for (auto [name, field, flag] : {std::tuple{"ocioConfig", &o.color.config, "--ocio-config"},
             std::tuple{"workingSpace", &o.color.workingSpace, "--working-space"},
             std::tuple{"display", &o.color.display, "--display"},
             std::tuple{"view", &o.color.view, "--view"}}) {
        if (auto text = textSetting(std::string("colorManagement:") + name, flag); !text.empty())
            *field = text;
    }
}

// The progress line on stderr: the loading stage, then a bar per colour
// render. A monitor thread prints four times a second what the renderer's
// prism::Progress model (loading) and the session's progress callback
// (rendering) last reported, so nothing in the render path waits on it. On a
// terminal it rewrites one line with \r; otherwise (logs, CTest) it prints a
// plain line per stage and per 10%.
class ProgressPrinter {
public:
    explicit ProgressPrinter(bool enabled) : m_enabled(enabled)
    {
        if (!m_enabled) return;
#ifdef _WIN32
        m_terminal = _isatty(_fileno(stderr)) != 0;
#else
        m_terminal = isatty(fileno(stderr)) != 0;
#endif
        m_thread = std::thread([this] { Run(); });
    }
    ~ProgressPrinter() { Stop(); }

    // The renderer's progress model (its material and texture counts).
    void SetModel(prism::Progress* model)
    {
        std::lock_guard lock(m_mutex);
        m_model = model;
    }
    // An empty stage clears the line (before other output).
    void Stage(std::string text)
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        m_stage = std::move(text);
        m_rendering = false;
        if (m_stage.empty() && m_terminal && m_width) {
            std::cerr << '\r' << std::string(m_width, ' ') << '\r' << std::flush;
            m_width = 0;
        }
    }
    // With a time limit, the progress is at least its elapsed fraction.
    void BeginRender(bool adaptive, std::string label, double timeLimit)
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        m_rendering = true;
        m_info = {};
        m_adaptive = adaptive;
        m_timeLimit = timeLimit;
        m_rate = m_rateSamples = m_rateTime = 0.0;
        m_stage = std::move(label);
        m_renderStart = std::chrono::steady_clock::now();
        m_lastDecile = -1;
    }
    // The session's progress callback (on its thread).
    void Update(prism::ProgressInfo const& info)
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        if (info.phase == "rendering") m_info = info;
    }
    void EndRender()
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        if (m_rendering) Print(Line(1.0), true);
        m_rendering = false;
        m_stage.clear();
    }
    void Stop()
    {
        if (!m_thread.joinable()) return;
        { std::lock_guard lock(m_mutex); m_stop = true; }
        m_wake.notify_all();
        m_thread.join();
        // A render cut short (an error) keeps its last line; end it so the
        // error starts on a line of its own.
        if (m_terminal && m_width) std::cerr << '\n' << std::flush;
        m_width = 0;
    }

private:
    static std::string Duration(double seconds)
    {
        const long total = long(std::max(0.0, seconds) + 0.5);
        return std::to_string(total / 60) + "m" + (total % 60 < 10 ? "0" : "") + std::to_string(total % 60) + "s";
    }
    // Called with m_mutex held.
    double Fraction() const
    {
        double fraction = m_info.fraction;
        if (m_timeLimit > 0.0)
            fraction = std::max(fraction, std::min(1.0, std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_renderStart).count() / m_timeLimit));
        return fraction;
    }
    // Called with m_mutex held. `fraction` overrides the reported one (1 at the end).
    std::string Line(double fraction = -1.0)
    {
        auto const& p = m_info;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_renderStart).count();
        // GPU devices compile and upload before their first sample.
        if (fraction < 0.0 && p.samples == 0.0)
            return "Preparing the render device... " + Duration(elapsed);
        if (fraction < 0.0) fraction = Fraction();
        const int width = 20, filled = int(fraction * width + 1e-9);
        std::ostringstream line;
        line << '[' << std::string(filled, '#') << std::string(width - filled, '-') << "] "
             << int(fraction * 100.0) << "%  " << long(p.samples) << '/' << std::max(1, p.targetSamples) << " spp";
        if (m_adaptive) line << "  " << int(p.activeFraction * 100.0 + 0.5) << "% px active";
        line << "  " << Duration(elapsed) << " elapsed";
        // ETA: the samples left at the current rate (adaptive sampling may
        // stop the remaining pixels earlier), within the time limit.
        const double samples = p.samples;
        const double target = std::max(1, p.targetSamples);
        if (samples > m_rateSamples && elapsed > m_rateTime + 0.5) {
            const double rate = (samples - m_rateSamples) / (elapsed - m_rateTime);
            m_rate = m_rate > 0.0 ? 0.7 * m_rate + 0.3 * rate : rate;
            m_rateSamples = samples;
            m_rateTime = elapsed;
        }
        if (fraction < 1.0 && m_rate > 0.0) {
            double eta = (target - samples) / m_rate;
            if (m_timeLimit > 0.0) eta = std::min(eta, std::max(0.0, m_timeLimit - elapsed));
            line << ", ETA " << Duration(eta);
        }
        if (!m_stage.empty()) line << "  " << m_stage;
        return line.str();
    }
    void Print(std::string const& text, bool final)
    {
        if (m_terminal) {
            std::cerr << '\r' << text;
            if (text.size() < m_width) std::cerr << std::string(m_width - text.size(), ' ');
            m_width = final ? 0 : text.size();
            if (final) std::cerr << '\n';
            std::cerr << std::flush;
        } else {
            std::cerr << text << '\n' << std::flush;
        }
    }
    void Run()
    {
        std::string lastStage;
        std::unique_lock lock(m_mutex);
        while (!m_stop) {
            if (m_rendering) {
                const double fraction = Fraction();
                const int decile = int(fraction * 10.0);
                if (m_terminal) Print(Line(), false);
                else if (decile > m_lastDecile && decile < 10) { m_lastDecile = decile; Print(Line(), false); }
            } else if (!m_stage.empty()) {
                // Loading: the CLI's stage, with the material count while the
                // materials compile (prism::Progress, reported by the loaders).
                std::string text = m_stage;
                if (auto* model = m_model) {
                    const auto snapshot = model->Read();
                    auto const& materials = snapshot.phases[size_t(prism::Phase::Materials)];
                    auto const& textures = snapshot.phases[size_t(prism::Phase::Textures)];
                    if (materials.active && materials.total)
                        text += " (materials " + std::to_string(materials.done) + "/" + std::to_string(materials.total) + ")";
                    else if (textures.active && textures.total)
                        text += " (textures " + std::to_string(textures.done) + "/" + std::to_string(textures.total) + ")";
                }
                if (m_terminal) Print(text + "...", false);
                else if (m_stage != lastStage) Print(m_stage + "...", false);
                lastStage = m_stage;
            }
            m_wake.wait_for(lock, std::chrono::milliseconds(250), [this] { return m_stop; });
        }
    }

    bool m_enabled = false, m_terminal = false, m_stop = false, m_adaptive = false, m_rendering = false;
    double m_timeLimit = 0.0;
    // Sample rate for the ETA (samples per second, smoothed).
    double m_rate = 0.0, m_rateSamples = 0.0, m_rateTime = 0.0;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::string m_stage;
    prism::ProgressInfo m_info;
    prism::Progress* m_model = nullptr;
    std::chrono::steady_clock::time_point m_renderStart;
    int m_lastDecile = -1;
    size_t m_width = 0;
};

const char* DeviceName(prism::DeviceKind kind)
{
    switch (kind) {
    case prism::DeviceKind::Cpu: return "cpu";
    case prism::DeviceKind::Vulkan: return "vulkan";
    case prism::DeviceKind::Metal: return "metal";
    }
    return "unknown";
}

}

int runStandaloneCli(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h"))) {
            help();
            return 0;
        }
        // An install path past Windows' 260-character limit: one clear message
        // instead of material loading failures (docs/deployment.md).
        if (std::string problem = prism::CheckInstallPathLength(); !problem.empty())
            throw std::runtime_error(problem);
        // The renderer's options: the environment's (OCIO, PRISM_*), which
        // flags and render settings override below.
        prism::RendererOptions rendererOptions = prism::RendererOptions::FromEnvironment();
        if (argc == 2 && std::string(argv[1]) == "--list-devices") {
            for (auto const& info : prism::Renderer(rendererOptions).devices())
                std::cout << DeviceName(info.kind) << ": " << info.name
                          << (info.hardwareRayTracing ? " (hardware ray tracing)" : "") << '\n';
            return 0;
        }
        Options o = parse(argc, argv);
        ProgressPrinter progress(o.progress);
        auto start = std::chrono::steady_clock::now();
        auto seconds =[&] { return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); };
        // --material: the built-in preview scene (prism::usd::LoadMaterialPreview).
        const bool preview = !o.material.empty();
        const auto extension = std::filesystem::path(o.scene).extension().string();
        const bool isUsd = extension == ".usd" || extension == ".usda" || extension == ".usdc" || extension == ".usdz";
        UsdStageRefPtr stage;
        std::optional<prism::output::RenderJob> job;
        if (isUsd) {
            stage = UsdStage::Open(o.scene);
            if (!stage) throw std::runtime_error("Cannot open USD scene: " + o.scene);
            // RenderSettings, products and vars, as prism_hydra_render
            // resolves them (src/output/render_products.h).
            std::vector<std::string> warnings;
            job = prism::output::ResolveRenderJob(stage, o.renderSettings, o.outputDirectory, &warnings);
            for (auto const& warning : warnings) std::cerr << "PrismRT: " << warning << '\n';
            if (job) {
                applyRenderSettings(job->namespacedSettings, o);
                // The working space when neither --working-space nor
                // prism:colorManagement:workingSpace names one.
                o.color.renderingColorSpace = job->renderingColorSpace;
                if (!o.widthSet && job->base.width > 0) o.width = job->base.width;
                if (!o.heightSet && job->base.height > 0) o.height = job->base.height;
            }
        } else if (!o.renderSettings.empty()) {
            throw std::runtime_error("--render-settings requires a USD scene");
        }
        validate(o);
        const bool explicitCamera = o.cameraPosition.has_value() && o.cameraTarget.has_value();
        // Without --output or a camera option, the render settings' products
        // are the outputs; otherwise one colour image as before.
        const bool productMode = job && !job->products.empty() && !o.given.count("--output") &&
            o.camera.empty() && !explicitCamera;
        std::vector<prism::output::RenderProductRequest> views;
        if (productMode) {
            views = job->products;
            if (o.widthSet || o.heightSet)
                std::cerr << "PrismRT: --width/--height do not apply to render products; ignored\n";
            if (o.denoise == "aov")
                std::cerr << "PrismRT: --denoise aov does not apply to render products (add a 'denoised' "
                             "RenderVar); ignored\n";
        } else {
            prism::output::RenderProductRequest view;
            if (job) view = job->base;
            if (!preview && o.camera.empty() && !explicitCamera) o.camera = view.camera.GetString();
            if (!preview && o.camera.empty() && !explicitCamera)
                throw std::runtime_error("--scene and an explicit camera source are required (see --help)");
            view.camera = o.camera.empty() ? SdfPath() : SdfPath(o.camera);
            // The authored data window applies to the requested size.
            view.width = o.width;
            view.height = o.height;
            if (!job) view.dataWindowNDC = GfRange2f(GfVec2f(0.0f), GfVec2f(1.0f));
            prism::output::DataWindowPixels(view.dataWindowNDC, view.width, view.height,
                &view.dataX, &view.dataY, &view.dataWidth, &view.dataHeight);
            if (view.dataWidth <= 0 || view.dataHeight <= 0)
                throw std::runtime_error("The render settings' data window is empty");
            view.output = o.output;
            prism::output::RenderVarRequest color;
            color.name = "color";
            color.kind = prism::output::AovKind::Color;
            color.components = 3;
            view.vars.push_back(color);
            if (o.denoise == "aov") {
                if (!prism::output::IsMultiChannelFormat(view.output))
                    std::cerr << "PrismRT: --denoise aov adds a layer only EXR holds; " << view.output
                              << " gets the raw colour (--denoise replace writes the denoised one)\n";
                prism::output::RenderVarRequest denoised = color;
                denoised.name = "denoised";
                denoised.kind = prism::output::AovKind::Denoised;
                view.vars.push_back(denoised);
            }
            views.push_back(std::move(view));
        }
        const UsdTimeCode time = o.time ? UsdTimeCode(*o.time) : UsdTimeCode::Default();
        // Colour options from the flags and render settings; the config
        // defaults to the renderer's ($OCIO).
        if (o.color.config.empty()) o.color.config = rendererOptions.color.config;
        // PRISM_WORKING_SPACE: below --working-space and prism:colorManagement:workingSpace.
        if (o.color.workingSpace.empty()) o.color.workingSpace = rendererOptions.color.workingSpace;
        rendererOptions.color = o.color;
        // Every loader takes its MDL modules from the MaterialX libraries'
        // mdl directory.
        rendererOptions.materials.materialxMdlPath = rendererOptions.materials.materialxLibraries + "/mdl";
        // --sss-fallback and prism:sssFallback (default PRISM_SSS_FALLBACK).
        rendererOptions.materials.sssFallback = o.sssFallback;
        // The renderer: colour, materials, textures, Embree, logging and
        // progress.
        const prism::Renderer renderer(rendererOptions);
        // The denoiser is needed for --denoise replace and for denoised vars.
        bool anyDenoised = o.denoise == "replace";
        for (auto const& view : views)
            for (auto const& var : view.vars) anyDenoised |= var.kind == prism::output::AovKind::Denoised;
        if (anyDenoised) {
            const prism::Capabilities capabilities = renderer.capabilities();
            if (!capabilities.denoising)
                throw std::runtime_error("denoising was asked for (--denoise, prism:denoise or a 'denoised' "
                                         "render var) but " + capabilities.denoisingNote);
        }
        // Loading phases (materials, textures) report to the renderer's model,
        // and its diagnostics go to standard error, also from this thread.
        prism::RendererContext& context = *renderer.context();
        progress.SetModel(&context.progress());
        // The printer outlives the renderer: it stops reading the model first.
        struct ModelRelease {
            ProgressPrinter& printer;
            ~ModelRelease() { printer.SetModel(nullptr); }
        } modelRelease{progress};
        prism::progress::Bind progressBinding(&context.progress());
        prism::logging::Bind logBinding(&context.logger());
        // Before anything loads: materials, lights and textures convert into
        // the working space as they are read.
        const prism::ColorInfo color = renderer.color();
        std::cout << "Color: working space " << color.workingSpace << ", view " << color.view
                  << " on " << color.display << " (" << color.configName << ")\n";
        const double stageSeconds = seconds();
        for (auto const& view : views) {
            if (view.width <= 0 || view.height <= 0 ||
                static_cast<uint64_t>(view.dataWidth) * view.dataHeight > std::numeric_limits<unsigned>::max() / 4)
                throw std::runtime_error("Authored render resolution is invalid or too large");
            if (view.camera.IsEmpty() && !explicitCamera && !preview)
                throw std::runtime_error("Render product <" + view.path.GetString() + "> has no camera");
        }
        // The cameras, checked before the scene loads: USD cameras with their
        // exposure (UsdGeomCamera exposure, exposure:iso, :time, :fStop and
        // :responsivity, which scales the colour as the Hydra delegate's
        // exposure compensation does), or the explicit look-at camera.
        auto cameraFor = [&](prism::output::RenderProductRequest const& view) {
            if (!view.camera.IsEmpty()) {
                if (!stage) throw std::runtime_error("--camera prim paths require a USD scene");
                return prism::usd::LoadCamera(stage, view.camera, time);
            }
            GfCamera camera;
            auto asGf = [](Float3 v) { return GfVec3d(v.x(),v.y(),v.z()); };
            GfMatrix4d viewMatrix(1); viewMatrix.SetLookAt(asGf(*o.cameraPosition), asGf(*o.cameraTarget), asGf(o.cameraUp));
            camera.SetTransform(viewMatrix.GetInverse());
            camera.SetPerspectiveFromAspectRatioAndFieldOfView(double(view.width)/view.height, o.verticalFov, GfCamera::FOVVertical);
            camera.SetFocusDistance(o.focusDistance);
            const float radius = o.focusDistance * std::tan(deg2rad(o.defocusAngle/2));
            camera.SetFStop(radius > 0 ? camera.GetFocalLength()*0.1f/(2*radius) : 0);
            camera.SetClippingRange(GfRange1f(0.001f, 1e30f));
            return prism::usd::CameraFromGf(camera);
        };
        // The preview's camera comes with its scene, below.
        std::vector<prism::CameraDesc> cameras;
        if (!preview)
            for (auto const& view : views) cameras.push_back(cameraFor(view));
        progress.Stage("Loading scene");
        // The scene, through the public API: the loaders add to it and its
        // commit converts meshes, compiles materials and builds the
        // acceleration structure.
        prism::SceneOptions sceneOptions;
        // Subdivision surfaces and load-time displacement (USD meshes; OBJ
        // faces their MTL disp displaces).
        sceneOptions.subdivisionLevel = prism::defaults::ClampSubdivisionLevel(o.subdivisionLevel);
        sceneOptions.subdivisionMaxFaces = uint64_t(prism::defaults::ClampFaces(o.subdivisionMaxFaces));
        sceneOptions.subdivisionBudgetFaces = uint64_t(prism::defaults::ClampFaces(o.subdivisionBudgetFaces));
        sceneOptions.displacementLevel = prism::defaults::ClampSubdivisionLevel(o.displacementLevel);
        sceneOptions.displacementMaxFaces = uint64_t(prism::defaults::ClampFaces(o.displacementMaxFaces));
        // A scene with no lights, emissive geometry or environment would render
        // black: usdview's default dome, which the Hydra delegate applies by
        // the same rule (scene_bridge.cpp _ApplyDefaultDome).
        sceneOptions.defaultDome = o.defaultDome;
        if (o.defaultDome) sceneOptions.defaultDomeTexture = DefaultDomeTexture();
        prism::Scene scene = renderer.createScene(sceneOptions);
        std::string previewNote;
        if (preview) {
            prism::MaterialDesc material;
            material.kind = prism::MaterialDesc::Kind::MaterialXFile;
            material.path = o.material;
            material.materialName = o.materialName;
            prism::usd::MaterialPreviewOptions previewOptions;
            previewOptions.time = time;
            const prism::usd::MaterialPreview loaded = prism::usd::LoadMaterialPreview(scene, material, previewOptions);
            cameras.assign(views.size(), loaded.camera);
            previewNote = "Material: " + loaded.materialName + " of " + o.material;
            if (o.materialName.empty() && loaded.documentMaterials.size() > 1) {
                previewNote += " (the first of " + std::to_string(loaded.documentMaterials.size()) + ":";
                for (auto const& name : loaded.documentMaterials) previewNote += " " + name;
                previewNote += "; --material-name picks another)";
            }
        } else if (isUsd) {
            prism::usd::LoadOptions load;
            if (job) {
                load.bindingPurposes.assign(job->materialBindingPurposes.begin(), job->materialBindingPurposes.end());
                // includedPurposes: default geometry always renders; without
                // render settings, render-purpose geometry does too.
                load.includedPurposes.assign(job->includedPurposes.begin(), job->includedPurposes.end());
                if (load.includedPurposes.empty()) load.includedPurposes.push_back(UsdGeomTokens->default_);
            }
            load.time = time;
            // --env and --env-color replace the scene's domes.
            load.domes = !o.environmentColor && o.environment.empty();
            if (prism::usd::Load(scene, stage, load).renderablePlacements() == 0)
                throw std::runtime_error("Scene contains no renderable geometry");
        }
        else if (extension == ".gltf" || extension == ".glb")
            prism::gltf::Load(scene, o.scene);
        else if (extension == ".obj")
            prism::obj::Load(scene, o.scene);
        else throw std::runtime_error("Unsupported scene format: " + extension);
        if (o.environmentColor) {
            // A constant dome of that radiance (linear Rec.709), times 2^--env-exposure.
            prism::EnvironmentDesc environment;
            environment.kind = prism::EnvironmentDesc::Kind::Constant;
            environment.color = *o.environmentColor;
            environment.exposure = o.exposure;
            scene.addEnvironment(environment);
        } else if (!o.environment.empty()) {
            if (!std::filesystem::exists(o.environment)) throw std::runtime_error("Environment does not exist: " + o.environment);
            // As an untransformed USD dome of the file at intensity 1: the
            // texels times 2^--env-exposure.
            prism::EnvironmentDesc environment;
            environment.kind = prism::EnvironmentDesc::Kind::LatLong;
            environment.texture = o.environment;
            environment.textureColorSpace = prism::color::kLightColorSpace;
            environment.colorSpace.clear(); // the multiplier 1, in the working space
            environment.exposure = o.exposure;
            scene.addEnvironment(environment);
        }
        progress.Stage("Building the scene");
        scene.commit();
        progress.Stage("");
        if (!previewNote.empty()) std::cout << previewNote << '\n';
        if (const char* report = std::getenv("PRISM_GPU_PROGRAM_REPORT"); report && *report == '1') {
            // Diagnostics without a GPU device (an internal hook).
            if (!prism::device::ReportGpuPrograms(prism::detail::SceneAccess::committed(scene), std::cout,
                                                  context.varyingEmitters().get()))
                std::cerr << "PrismRT: this build has no Vulkan device\n";
            return 0;
        }

        // The session: the device, the render settings and the camera of
        // each view.
        prism::SessionOptions sessionOptions;
        sessionOptions.device = o.device;
        prism::RenderSettings& settings = sessionOptions.settings;
        settings.samples = o.samples;
        settings.adaptiveThreshold = o.adaptiveThreshold;
        settings.adaptiveMinSamples = o.adaptiveMinSamples;
        settings.maxDepth = o.depth;
        settings.rouletteDepth = o.roulette;
        settings.seed = o.seed;
        settings.sampleClamp = o.clamp;
        settings.environmentMis = o.environmentSampling == "mis";
        settings.textureFiltering = o.textureFiltering == "bilinear" ? prism::TextureFiltering::Bilinear
                                                                    : prism::TextureFiltering::Filtered;
        settings.honorDoubleSided = o.honorDoubleSided;
        settings.nee = o.nee;
        settings.textureStreaming = o.textureStreaming;
        settings.gpuTextureBudgetMiB = o.gpuTextureBudgetMiB;
        settings.debugContributions = o.contributions;
        sessionOptions.cameraDesc = cameras.front();
        progress.Stage("Creating the render device");
        prism::Session session = renderer.createSession(scene, sessionOptions);
        progress.Stage("");
        if (!session.deviceNote().empty()) std::cerr << "PrismRT: " << session.deviceNote() << '\n';
        const prism::DeviceInfo deviceInfo = session.device();
        std::cout << "Rendering on " << DeviceName(deviceInfo.kind) << " device: " << deviceInfo.name << '\n';
        const double loadSeconds = seconds();
        using prism::output::AovKind;
        double renderSeconds = 0, meanSamples = o.samples, fractionAtMaxSamples = 1.0;
        double denoiseMilliseconds = 0;
        std::string denoiserName; // the last denoise's (for the profile)
        bool timeLimitReached = false;
        prism::RenderResult result;
        for (size_t index = 0; index < views.size(); ++index) {
            auto const& view = views[index];
            // Products that differ only in their vars share one render: the
            // first renders what all of them ask for.
            const bool newView = index == 0 || !view.SameView(views[index - 1]);
            std::vector<prism::Aov> aovs;
            bool denoise = o.denoise == "replace";
            bool traced = false;
            if (newView) {
                for (size_t other = index; other < views.size() && views[other].SameView(view); ++other)
                    for (auto const& var : views[other].vars) {
                        auto const* info = prism::output::FindAov(var.kind);
                        const auto aov = prism::output::PublicAov(var.kind);
                        if (!info || !aov) continue;
                        if (!info->standalone) {
                            std::cerr << "PrismRT: render var <" << var.path << "> ('" << var.sourceName
                                      << "') is only available in the Hydra delegate; skipped\n";
                            continue;
                        }
                        denoise |= var.kind == AovKind::Denoised;
                        traced |= prism::output::ColorAov(var.kind) || prism::output::LightAov(var.kind);
                        if (std::find(aovs.begin(), aovs.end(), *aov) == aovs.end()) aovs.push_back(*aov);
                    }
                result = prism::RenderResult{};
            }
            if (newView && !aovs.empty()) {
                session.setAovs(aovs);
                if (denoise) session.setDenoise(o.denoiseSettings, o.denoise == "replace");
                else session.setDenoise(std::nullopt);
                session.setCamera(cameras[index]);
                std::optional<prism::Window> window;
                if (!view.FullDataWindow())
                    window = prism::Window{unsigned(view.dataX), unsigned(view.dataY), unsigned(view.dataWidth),
                                           unsigned(view.dataHeight)};
                session.resize(unsigned(view.width), unsigned(view.height), window, view.pixelAspectRatio,
                               prism::output::PublicConformPolicy(view.aspectRatioConformPolicy));
                // The limit covers all of the views together.
                prism::RenderSettings viewSettings = settings;
                viewSettings.timeLimitSeconds = o.timeLimit > 0 ? std::max(1e-6, o.timeLimit - renderSeconds) : 0.0;
                session.setSettings(viewSettings);
                if (traced)
                    progress.BeginRender(o.adaptiveThreshold > 0,
                        views.size() > 1 ? "view " + std::to_string(index + 1) + "/" + std::to_string(views.size()) : "",
                        viewSettings.timeLimitSeconds);
                result = session.render([&](prism::ProgressInfo const& info) {
                    progress.Update(info);
                    return true;
                });
                if (traced) progress.EndRender();
                renderSeconds += result.renderSeconds;
                if (result.traced) {
                    meanSamples = result.meanSamples;
                    fractionAtMaxSamples = result.fractionAtMaxSamples;
                    timeLimitReached |= result.timeLimitReached;
                }
                if (!result.denoiser.empty()) {
                    denoiserName = result.denoiser;
                    denoiseMilliseconds += result.denoiseMilliseconds;
                    std::cout << "Denoised " << view.dataWidth << "x" << view.dataHeight << " in "
                              << result.denoiseMilliseconds << " ms with " << denoiserName << " ("
                              << prism::device::ToString(o.denoiseSettings.quality) << " quality"
                              << (o.denoiseSettings.prefilter ? ", prefiltered guides" : "") << ")\n";
                }
            }
            // EXR: every layer, linear working space, tagged. Other formats:
            // the colour display-encoded through the view, exactly once.
            std::vector<std::string> warnings;
            if (!prism::output::WriteProduct(view, result, renderer, &warnings)) {
                for (auto const& warning : warnings) std::cerr << "PrismRT: " << warning << '\n';
                std::cerr << "PrismRT: nothing to write to " << view.output << '\n';
                continue;
            }
            for (auto const& warning : warnings) std::cerr << "PrismRT: " << warning << '\n';
            std::cout << "Rendered " << view.dataWidth << "x" << view.dataHeight;
            if (!view.FullDataWindow())
                std::cout << " (data window of " << view.width << "x" << view.height << ")";
            std::cout << " at " << o.samples << " spp";
            if (o.adaptiveThreshold > 0 || timeLimitReached)
                std::cout << " (" << (o.adaptiveThreshold > 0 ? "adaptive, " : "") << "mean " << meanSamples
                          << ", " << 100.0 * fractionAtMaxSamples << "% of pixels at the maximum"
                          << (timeLimitReached ? ", stopped by the time limit" : "") << ")";
            std::cout << " to " << view.output << '\n';
        }
        const double outputSeconds = seconds() - loadSeconds - renderSeconds;
        const prism::RendererStats stats = renderer.stats();
        std::cout << "[PrismProfile] {\"stageSeconds\":" << stageSeconds
                  << ",\"loadSeconds\":" << loadSeconds
                  << ",\"renderSeconds\":" << renderSeconds
                  << ",\"meanSamples\":" << meanSamples
                  << ",\"fractionAtMaxSamples\":" << fractionAtMaxSamples
                  << ",\"timeLimitReached\":" << (timeLimitReached ? "true" : "false")
                  << ",\"outputSeconds\":" << outputSeconds
                  << ",\"denoiseMilliseconds\":" << denoiseMilliseconds
                  << ",\"denoiser\":\"" << denoiserName << "\""
                  << ",\"textureFiltering\":\"" << o.textureFiltering << "\""
                  << ",\"texturePrepareCalls\":" << stats.texturePrepareCalls
                  << ",\"textureMemoryCacheHits\":" << stats.textureMemoryCacheHits
                  << ",\"textureDiskCacheHits\":" << stats.textureDiskCacheHits
                  << ",\"textureSourceBytesHashed\":" << stats.textureSourceBytesHashed
                  << ",\"texturePreparationWorkerSeconds\":" << stats.texturePreparationNanoseconds / 1e9
                  << "}\n";
        // PRISM_RENDER_PROFILE: where the render's time went.
        if (const std::string report = prism::profiling::Report(renderSeconds); !report.empty())
            std::cout << report;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PrismRT: " << error.what() << '\n';
        return 1;
    }
}
