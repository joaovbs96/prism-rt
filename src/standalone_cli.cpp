#include "standalone_cli.h"
#include "scene.h"
#include "render_defaults.h"
#include "materialx/loader.h"
#include "materialx/material_service.h"
#include "camera_rays.h"
#include "camera_ray_diagnostics.h"
#include "device/device.h"
#include "device/light_table.h"
#include "color/color_management.h"
#include "renderer/renderer_context.h"
#include "output/image_writer.h"
#include "output/render_products.h"
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/domeLight_1.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/rectLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/collectionMembershipQuery.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <tbb/parallel_for.h>
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
        "  Supported scene formats: USD, glTF, GLB, OBJ\n"
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
        "  --working-space NAME (default the render settings' renderingColorSpace, else ACEScg;\n"
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
    // The camera may also come from the stage's render settings.
    if (o.scene.empty())
        throw std::runtime_error("--scene and an explicit camera source are required (see --help)");
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

// The colour space of a light's texture: linear Rec.709, like the light's
// color. The Hydra delegate cannot see attribute colorSpace metadata, so
// neither loader reads it (docs/color_management.md).
std::string TextureColorSpace(UsdAttribute const&) {
    return prism::color::kLightColorSpace;
}

// A light's texture file, resolved as the Hydra delegate resolves it (the
// asset's resolved path, else the resolver on its authored path); empty when
// it does not exist.
std::string ResolvedLightTexture(SdfAssetPath const& asset) {
    std::string resolved = asset.GetResolvedPath();
    if (resolved.empty() && !asset.GetAssetPath().empty())
        resolved = ArGetResolver().Resolve(asset.GetAssetPath()).GetPathString();
    if (!resolved.empty() && !std::filesystem::exists(resolved)) resolved.clear();
    return resolved;
}

// UsdLuxShapingAPI inputs, as the Hydra delegate reads them
// (scene_bridge.cpp _ShapingParameters): the light is shaped when one of
// them has a value (the API applied, which gives every input its fallback,
// or an input authored); unauthored ones take the schema's fallbacks.
prism::LightShapingParameters ShapingParameters(UsdPrim const& prim, UsdTimeCode time)
{
    prism::LightShapingParameters s;
    const UsdLuxShapingAPI shaping(prim);
    auto read = [&](UsdAttribute const& attribute, auto* value) {
        if (attribute && attribute.HasValue() && attribute.Get(value, time)) s.active = true;
    };
    GfVec3f tint(0.0f);
    SdfAssetPath ies;
    read(shaping.GetShapingFocusAttr(), &s.focus);
    read(shaping.GetShapingFocusTintAttr(), &tint);
    read(shaping.GetShapingConeAngleAttr(), &s.coneAngle);
    read(shaping.GetShapingConeSoftnessAttr(), &s.coneSoftness);
    read(shaping.GetShapingIesFileAttr(), &ies);
    read(shaping.GetShapingIesAngleScaleAttr(), &s.iesAngleScale);
    read(shaping.GetShapingIesNormalizeAttr(), &s.iesNormalize);
    s.focusTint = Color3(tint[0], tint[1], tint[2]);
    if (s.active && !ies.GetAssetPath().empty()) {
        std::string resolved = ies.GetResolvedPath();
        if (resolved.empty()) resolved = ArGetResolver().Resolve(ies.GetAssetPath()).GetPathString();
        if (!resolved.empty() && std::filesystem::exists(resolved)) s.ies = prism::LoadIesProfile(resolved);
        else prism::WarnUnresolvedIesProfile(ies.GetAssetPath());
    }
    return s;
}

// Light and shadow linking as the Hydra delegate gets it from
// HdsiLightLinkingSceneIndex: a category bit per distinct collection that
// does not include everything (at most 32 of each kind), and on each
// placement the bits of the collections that include its prim.
class LightLinks {
public:
    // The category bit of a light's lightLink or shadowLink collection (0:
    // it links everything).
    uint32_t bit(UsdCollectionAPI const& collection, bool shadow)
    {
        if (!collection) return 0u;
        UsdCollectionMembershipQuery query = collection.ComputeMembershipQuery();
        std::string key;
        if (query.UsesPathExpansionRuleMap()) {
            auto const& rules = query.GetAsPathExpansionRuleMap();
            if (rules.size() == 1 && rules.begin()->first == SdfPath::AbsoluteRootPath() &&
                rules.begin()->second == UsdTokens->expandPrims) return 0u;
            for (auto const& [path, rule] : rules) key += path.GetString() + "=" + rule.GetString() + ";";
        } else {
            const SdfPathExpression expression = collection.ResolveCompleteMembershipExpression();
            if (expression == SdfPathExpression::Everything()) return 0u;
            key = "expression:" + expression.GetText();
        }
        auto& kind = shadow ? m_shadow : m_light;
        for (auto const& category : kind)
            if (category.key == key) return category.bit;
        if (kind.size() >= 32) {
            if (!m_warned) std::cerr << "PrismRT: more than 32 light or shadow link collections; further ones are ignored\n";
            m_warned = true;
            return 0u;
        }
        kind.push_back({key, std::move(query), 1u << kind.size()});
        return kind.back().bit;
    }
    bool active() const { return !m_light.empty() || !m_shadow.empty(); }
    // The light and shadow link masks of a prim.
    std::pair<uint32_t, uint32_t> masks(SdfPath const& path) const
    {
        std::pair<uint32_t, uint32_t> result{0u, 0u};
        for (auto const& category : m_light) if (category.query.IsPathIncluded(path)) result.first |= category.bit;
        for (auto const& category : m_shadow) if (category.query.IsPathIncluded(path)) result.second |= category.bit;
        return result;
    }

private:
    struct Category {
        std::string key;
        UsdCollectionMembershipQuery query;
        uint32_t bit = 0;
    };
    std::vector<Category> m_light, m_shadow;
    bool m_warned = false;
};

// Independent USD camera ingestion, deliberately separate from the Hydra adapter.
struct UsdCameraRays {
    prism::CameraRayParameters parameters;

    // The view's display window, pixel aspect ratio and data window, framed
    // as Hydra frames a render pass (CameraUtilFraming).
    UsdCameraRays(const GfCamera& c, prism::output::RenderProductRequest const& view) {
        const GfMatrix4d projection = c.GetFrustum().ComputeProjectionMatrix();
        if (view.FullDataWindow() && view.pixelAspectRatio == 1.0f &&
            prism::output::ConformPolicy(view.aspectRatioConformPolicy) == CameraUtilFit) {
            parameters.inverseProjection = CameraUtilConformedWindow(
                projection, CameraUtilFit, double(view.width) / view.height).GetInverse();
        } else {
            const CameraUtilFraming framing(
                GfRange2f(GfVec2f(0.0f), GfVec2f(float(view.width), float(view.height))),
                GfRect2i(GfVec2i(view.dataX, view.dataY), view.dataWidth, view.dataHeight),
                view.pixelAspectRatio);
            parameters.inverseProjection = framing.ApplyToProjectionMatrix(
                projection, prism::output::ConformPolicy(view.aspectRatioConformPolicy)).GetInverse();
        }
        parameters.cameraToWorld = c.GetTransform();
        const GfRange1f clipping = c.GetClippingRange();
        parameters.nearClip = static_cast<float>(clipping.GetMin());
        parameters.farClip = static_cast<float>(clipping.GetMax());
        parameters.orthographic =
            c.GetProjection() == GfCamera::Orthographic;
        parameters.apertureRadius = prism::CameraApertureRadius(
            c.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT, c.GetFStop());
        parameters.focusDistance = c.GetFocusDistance();
    }
};

// The progress line on stderr: the loading stage, then a bar per colour
// render. A monitor thread polls the device's ImageProgress (relaxed atomics)
// and the process's prism::Progress model four times a second, so nothing in
// the render path waits on it. On a terminal it rewrites one line with \r;
// otherwise (logs, CTest) it prints a plain line per stage and per 10%.
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

    // An empty stage clears the line (before other output).
    void Stage(std::string text)
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        m_stage = std::move(text);
        m_render = nullptr;
        if (m_stage.empty() && m_terminal && m_width) {
            std::cerr << '\r' << std::string(m_width, ' ') << '\r' << std::flush;
            m_width = 0;
        }
    }
    // With a time limit, the progress is at least its elapsed fraction.
    void BeginRender(prism::device::ImageProgress* progress, bool adaptive, std::string label,
                     double timeLimit)
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        m_render = progress;
        m_adaptive = adaptive;
        m_timeLimit = timeLimit;
        m_rate = m_rateSamples = m_rateTime = 0.0;
        m_stage = std::move(label);
        m_renderStart = std::chrono::steady_clock::now();
        m_lastDecile = -1;
    }
    void EndRender()
    {
        if (!m_enabled) return;
        std::lock_guard lock(m_mutex);
        if (m_render) Print(Line(1.0), true);
        m_render = nullptr;
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
        double fraction = m_render->Fraction();
        if (m_timeLimit > 0.0)
            fraction = std::max(fraction, std::min(1.0, std::chrono::duration<double>(
                std::chrono::steady_clock::now() - m_renderStart).count() / m_timeLimit));
        return fraction;
    }
    // Called with m_mutex held. `fraction` overrides the polled one (1 at the end).
    std::string Line(double fraction = -1.0)
    {
        auto& p = *m_render;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_renderStart).count();
        // GPU devices compile and upload before their first sample.
        if (fraction < 0.0 && p.samples.load(std::memory_order_relaxed) == 0.0)
            return "Preparing the render device... " + Duration(elapsed);
        if (fraction < 0.0) fraction = Fraction();
        const int width = 20, filled = int(fraction * width + 1e-9);
        std::ostringstream line;
        line << '[' << std::string(filled, '#') << std::string(width - filled, '-') << "] "
             << int(fraction * 100.0) << "%  " << long(p.samples.load(std::memory_order_relaxed)) << '/'
             << p.target.load(std::memory_order_relaxed) << " spp";
        if (m_adaptive)
            line << "  " << int(p.activeFraction.load(std::memory_order_relaxed) * 100.0 + 0.5) << "% px active";
        line << "  " << Duration(elapsed) << " elapsed";
        // ETA: the samples left at the current rate (adaptive sampling may
        // stop the remaining pixels earlier), within the time limit.
        const double samples = p.samples.load(std::memory_order_relaxed);
        const double target = p.target.load(std::memory_order_relaxed);
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
            if (m_render) {
                const double fraction = Fraction();
                const int decile = int(fraction * 10.0);
                if (m_terminal) Print(Line(), false);
                else if (decile > m_lastDecile && decile < 10) { m_lastDecile = decile; Print(Line(), false); }
            } else if (!m_stage.empty()) {
                // Loading: the CLI's stage, with the material count while the
                // materials compile (prism::Progress, reported by the loaders).
                std::string text = m_stage;
                if (auto* model = prism::Progress::Current()) {
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

    bool m_enabled = false, m_terminal = false, m_stop = false, m_adaptive = false;
    double m_timeLimit = 0.0;
    // Sample rate for the ETA (samples per second, smoothed).
    double m_rate = 0.0, m_rateSamples = 0.0, m_rateTime = 0.0;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::string m_stage;
    prism::device::ImageProgress* m_render = nullptr;
    std::chrono::steady_clock::time_point m_renderStart;
    int m_lastDecile = -1;
    size_t m_width = 0;
};

struct EmbreeDevice {
    RTCDevice value = rtcNewDevice(nullptr);
    ~EmbreeDevice() { if (value) rtcReleaseDevice(value); }
};
struct SceneRelease {
    SceneManager& manager;
    Mesh* mesh = nullptr;
    ~SceneRelease() {
        rtcReleaseScene(manager.scene);
        if (mesh) {
            if (mesh->geom) rtcReleaseGeometry(mesh->geom);
            delete mesh->transform;
            mesh->transform = nullptr;
        }
    }
};
}

int runStandaloneCli(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h"))) {
            help();
            return 0;
        }
        // The renderer's options: the environment's (OCIO, PRISM_*), which
        // flags and render settings override below.
        prism::RendererOptions rendererOptions = prism::RendererOptions::FromEnvironment();
        if (argc == 2 && std::string(argv[1]) == "--list-devices") {
            for (auto const& info : prism::device::AvailableDevices(rendererOptions.devices))
                std::cout << prism::device::ToString(info.kind) << ": " << info.name
                          << (info.hardwareRayTracing ? " (hardware ray tracing)" : "") << '\n';
            return 0;
        }
        Options o = parse(argc, argv);
        // Loading phases (materials, textures) report to this model.
        prism::Progress progressModel;
        // Outlives the printer, whose thread reads it (also while an error
        // unwinds).
        prism::device::ImageProgress imageProgress;
        ProgressPrinter progress(o.progress);
        EmbreeDevice device;
        if (!device.value) throw std::runtime_error("Cannot create Embree device");
        auto start = std::chrono::steady_clock::now();
        auto seconds =[&] { return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); };
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
            if (o.camera.empty() && !explicitCamera) o.camera = view.camera.GetString();
            if (o.camera.empty() && !explicitCamera)
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
        // The denoiser is needed for --denoise replace and for denoised vars.
        bool anyDenoised = o.denoise == "replace";
        for (auto const& view : views)
            for (auto const& var : view.vars) anyDenoised |= var.kind == prism::output::AovKind::Denoised;
        if (anyDenoised && !prism::device::DenoisingAvailable()) {
            std::string reason;
            prism::device::SharedCpuDenoiser(&reason);
            throw std::runtime_error("denoising was asked for (--denoise, prism:denoise or a 'denoised' "
                                     "render var) but " + reason);
        }
        const UsdTimeCode time = o.time ? UsdTimeCode(*o.time) : UsdTimeCode::Default();
        // Colour options from the flags and render settings; the config
        // defaults to the renderer's ($OCIO).
        if (o.color.config.empty()) o.color.config = rendererOptions.color.config;
        rendererOptions.color = o.color;
        // Every loader takes its MDL modules from the MaterialX libraries'
        // mdl directory.
        rendererOptions.materials.materialxMdlPath = rendererOptions.materials.materialxLibraries + "/mdl";
        const prism::RendererContext renderer(rendererOptions);
        // Before anything loads: materials, lights and textures convert into
        // the working space as they are read.
        const auto colorManagement = prism::color::Activate(o.color);
        std::cout << "Color: working space " << colorManagement->workingSpace() << ", view " << colorManagement->view()
                  << " on " << colorManagement->display() << " (" << colorManagement->configName() << ")\n";
        const double stageSeconds = seconds();
        for (auto const& view : views) {
            if (view.width <= 0 || view.height <= 0 ||
                static_cast<uint64_t>(view.dataWidth) * view.dataHeight > std::numeric_limits<unsigned>::max() / 4)
                throw std::runtime_error("Authored render resolution is invalid or too large");
            if (view.camera.IsEmpty() && !explicitCamera)
                throw std::runtime_error("Render product <" + view.path.GetString() + "> has no camera");
        }
        // The cameras' exposure (UsdGeomCamera exposure, exposure:iso,
        // :time, :fStop and :responsivity), which scales the colour as the
        // Hydra delegate's exposure compensation does.
        std::vector<float> exposureScales;
        auto cameraFor = [&](prism::output::RenderProductRequest const& view) {
            GfCamera camera;
            float exposureScale = 1.0f;
            if (!view.camera.IsEmpty()) {
                if (!stage) throw std::runtime_error("--camera prim paths require a USD scene");
                UsdGeomCamera authored(stage->GetPrimAtPath(view.camera));
                if (!authored) throw std::runtime_error("Not a USD camera: " + view.camera.GetString());
                camera = authored.GetCamera(time);
                exposureScale = authored.ComputeLinearExposureScale(time);
            } else {
                auto asGf = [](Float3 v) { return GfVec3d(v.x(),v.y(),v.z()); };
                GfMatrix4d viewMatrix(1); viewMatrix.SetLookAt(asGf(*o.cameraPosition), asGf(*o.cameraTarget), asGf(o.cameraUp));
                camera.SetTransform(viewMatrix.GetInverse());
                camera.SetPerspectiveFromAspectRatioAndFieldOfView(double(view.width)/view.height, o.verticalFov, GfCamera::FOVVertical);
                camera.SetFocusDistance(o.focusDistance);
                const float radius = o.focusDistance * std::tan(deg2rad(o.defocusAngle/2));
                camera.SetFStop(radius > 0 ? camera.GetFocalLength()*0.1f/(2*radius) : 0);
                camera.SetClippingRange(GfRange1f(0.001f, 1e30f));
            }
            exposureScales.push_back(std::isfinite(exposureScale) ? exposureScale : 1.0f);
            return UsdCameraRays(camera, view);
        };
        // Cameras are checked before the scene loads.
        std::vector<UsdCameraRays> viewRays;
        for (auto const& view : views) viewRays.push_back(cameraFor(view));
        prism::DumpCameraRayDiagnostics(
            viewRays.front().parameters, static_cast<unsigned int>(views.front().dataWidth),
            static_cast<unsigned int>(views.front().dataHeight), o.seed, "standalone");
        progress.Stage("Loading scene");
        // The process-lifetime runtime the material service shares its caches
        // and templates through (as in the Hydra delegate).
        mdl::Context& context = prism::materials::SharedMdlContext();
        if (!context.is_valid()) throw std::runtime_error(context.get_error());
        // Before any material loads.
        DocumentLoader::SetSssFallback(o.sssFallback);
        SceneManager scene(device.value);
        const std::string libraryPath = renderer.options().materials.materialxLibraries;
        const std::string mdlPath = renderer.options().materials.materialxMdlPath;
        std::unique_ptr<Mesh> mesh;
        // Subdivision surfaces and load-time displacement (USD meshes; OBJ
        // faces their MTL disp displaces).
        prism::mesh::SubdivisionSettings subdivision;
        subdivision.level = prism::defaults::ClampSubdivisionLevel(o.subdivisionLevel);
        subdivision.maxFaces = size_t(prism::defaults::ClampFaces(o.subdivisionMaxFaces));
        subdivision.budgetFaces = size_t(prism::defaults::ClampFaces(o.subdivisionBudgetFaces));
        subdivision.displacementLevel = prism::defaults::ClampSubdivisionLevel(o.displacementLevel);
        subdivision.displacementMaxFaces = size_t(prism::defaults::ClampFaces(o.displacementMaxFaces));
        if (isUsd) {
            USDMesh::LoadOptions load;
            if (job) {
                load.bindingPurposes.assign(job->materialBindingPurposes.begin(), job->materialBindingPurposes.end());
                // includedPurposes: default geometry always renders; without
                // render settings, render-purpose geometry does too.
                load.includedPurposes.assign(job->includedPurposes.begin(), job->includedPurposes.end());
                if (load.includedPurposes.empty()) load.includedPurposes.push_back(UsdGeomTokens->default_);
            }
            load.time = time;
            load.subdivision = subdivision;
            load.renderer = renderer.options();
            mesh = std::make_unique<USDMesh>(stage, &context, libraryPath, mdlPath, std::move(load));
        }
        else if (extension == ".gltf" || extension == ".glb") {
            mesh = std::make_unique<glTFMesh>(o.scene, &context, libraryPath, mdlPath);
        }
        else if (extension == ".obj")
            mesh = std::make_unique<ObjMesh>(o.scene, &context, libraryPath, mdlPath, subdivision);
        else throw std::runtime_error("Unsupported scene format: " + extension);
        const auto usdMesh = dynamic_cast<USDMesh*>(mesh.get());
        if (usdMesh ? usdMesh->renderablePlacementCount() == 0
                    : (mesh->vertices.empty() || mesh->indices.empty()))
            throw std::runtime_error("Scene contains no renderable geometry");
        SceneRelease release{scene};
        release.mesh = mesh.get();
        scene.attachImportedMesh(mesh.get());
        scene.max_depth = o.depth;
        scene.rr_min_depth = o.roulette;
        scene.environmentSampling = o.environmentSampling == "mis";
        scene.textureFiltering = o.textureFiltering == "bilinear" ? prism::TextureFiltering::Bilinear : prism::TextureFiltering::Filtered;
        scene.honorDoubleSided = o.honorDoubleSided;
        scene.textureStreaming = o.textureStreaming;
        scene.gpuTextureBudgetMiB = prism::defaults::ClampGpuTextureBudgetMiB(o.gpuTextureBudgetMiB);
        ColorTexture black(Color3(0));
        ConstantColorBackground background(&black);
        std::unique_ptr<ImageTexture<Color3>> envImage;
        std::unique_ptr<Environment> env;
        std::unique_ptr<ColorTexture> domeColor;
        scene.environment = &background;
        if (o.environmentColor) {
            domeColor = std::make_unique<ColorTexture>(colorManagement->FromRec709(*o.environmentColor) * std::exp2(o.exposure));
            env = std::make_unique<ConstantColorBackground>(domeColor.get());
            scene.environment = env.get();
        } else if (!o.environment.empty()) {
            if (!std::filesystem::exists(o.environment)) throw std::runtime_error("Environment does not exist: " + o.environment);
            envImage = std::make_unique<ImageTexture<Color3>>(o.environment, prism::color::kLightColorSpace);
            envImage->adjustExposure(o.exposure);
            // As an untransformed USD dome of the file at intensity 1: the
            // texels times 2^--env-exposure.
            env = std::make_unique<EnvironmentMap>(envImage.get(), Color3(1.0f));
            scene.environment = env.get();
        }
        std::vector<std::unique_ptr<Texture<Color3>>> domeTextures;
        std::vector<std::unique_ptr<Environment>> domes;
        LightLinks links;
        if (stage) {
            UsdGeomXformCache transforms(time);
            const bool useDomes = !o.environmentColor && o.environment.empty();
            for (const auto& prim : stage->Traverse()) {
                if (!prim.HasAPI<UsdLuxLightAPI>()) continue;
                if (UsdGeomImageable(prim).ComputeVisibility(time) == UsdGeomTokens->invisible) continue;
                UsdLuxLightAPI light(prim);
                GfVec3f color(1); float intensity = 1, exposure = 0;
                light.GetColorAttr().Get(&color, time); light.GetIntensityAttr().Get(&intensity, time);
                light.GetExposureAttr().Get(&exposure, time);
                bool enableTemperature = false; float temperature = 6500.0f;
                light.GetEnableColorTemperatureAttr().Get(&enableTemperature, time);
                light.GetColorTemperatureAttr().Get(&temperature, time);
                if (enableTemperature) color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(temperature));
                const GfMatrix4d world = transforms.GetLocalToWorldTransform(prim);
                const bool isDome = prim.IsA<UsdLuxDomeLight>() || prim.IsA<UsdLuxDomeLight_1>();
                if (isDome) {
                    if (!useDomes) continue;
                    const Color3 multiplier = colorManagement->FromRec709(Float3(color[0], color[1], color[2])) *
                        (intensity*std::exp2(exposure));
                    SdfAssetPath asset;
                    const UsdAttribute file = prim.GetAttribute(UsdLuxTokens->inputsTextureFile);
                    file.Get(&asset, time);
                    const std::string texturePath = ResolvedLightTexture(asset);
                    // As in the Hydra delegate: a white dome.
                    if (!asset.GetAssetPath().empty() && texturePath.empty())
                        std::cerr << "PrismRT: could not resolve dome texture '" << asset.GetAssetPath()
                                  << "'; the dome is white\n";
                    if (!texturePath.empty())
                        domeTextures.push_back(std::make_unique<ImageTexture<Color3>>(texturePath, TextureColorSpace(file)));
                    else domeTextures.push_back(std::make_unique<ColorTexture>(Color3(1)));
                    const auto inverse = world.GetInverse();
                    std::array<float,9> linear;
                    for (int row=0; row<3; ++row) for (int col=0; col<3; ++col) linear[row*3+col] = float(inverse[col][row]);
                    domes.push_back(std::make_unique<TransformedEnvironmentMap>(domeTextures.back().get(), multiplier, linear));
                    light.GetDiffuseAttr().Get(&domes.back()->diffuseFactor, time);
                    light.GetSpecularAttr().Get(&domes.back()->specularFactor, time);
                    continue;
                }
                prism::LightParameters params;
                if (prim.IsA<UsdLuxSphereLight>()) {
                    params.shape = prism::LightShape::Sphere;
                    UsdLuxSphereLight(prim).GetRadiusAttr().Get(&params.radius, time);
                    UsdLuxSphereLight(prim).GetTreatAsPointAttr().Get(&params.treatAsPoint, time);
                } else if (prim.IsA<UsdLuxRectLight>()) {
                    params.shape = prism::LightShape::Rect;
                    UsdLuxRectLight(prim).GetWidthAttr().Get(&params.width, time);
                    UsdLuxRectLight(prim).GetHeightAttr().Get(&params.height, time);
                } else if (prim.IsA<UsdLuxDiskLight>()) {
                    params.shape = prism::LightShape::Disk;
                    UsdLuxDiskLight(prim).GetRadiusAttr().Get(&params.radius, time);
                } else if (prim.IsA<UsdLuxCylinderLight>()) {
                    params.shape = prism::LightShape::Cylinder;
                    UsdLuxCylinderLight(prim).GetRadiusAttr().Get(&params.radius, time);
                    UsdLuxCylinderLight(prim).GetLengthAttr().Get(&params.length, time);
                } else if (prim.IsA<UsdLuxDistantLight>()) {
                    params.shape = prism::LightShape::Distant;
                    UsdLuxDistantLight(prim).GetAngleAttr().Get(&params.angle, time);
                } else {
                    std::cerr << "PrismRT: light type '" << prim.GetTypeName() << "' is not supported; skipped\n";
                    continue;
                }
                params.color = Color3(color[0], color[1], color[2]);
                params.intensity = intensity;
                params.exposure = exposure;
                light.GetNormalizeAttr().Get(&params.normalize, time);
                light.GetDiffuseAttr().Get(&params.diffuse, time);
                light.GetSpecularAttr().Get(&params.specular, time);
                params.shaping = ShapingParameters(prim, time);
                // Hydra's light:cameraVisibility, which UsdImaging reads from
                // the prim's `cameraVisibility` attribute.
                if (const UsdAttribute visibility = prim.GetAttribute(TfToken("cameraVisibility")))
                    visibility.Get(&params.cameraVisible, time);
                std::array<double, 16> matrix{};
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column) matrix[row * 4 + column] = world[row][column];
                prism::SceneLight sceneLight = prism::MakeSceneLight(params, matrix);
                sceneLight.lightLinkBit = links.bit(light.GetLightLinkCollectionAPI(), false);
                sceneLight.shadowLinkBit = links.bit(light.GetShadowLinkCollectionAPI(), true);
                if (params.shape == prism::LightShape::Rect) {
                    SdfAssetPath asset;
                    const UsdAttribute file = UsdLuxRectLight(prim).GetTextureFileAttr();
                    file.Get(&asset, time);
                    if (const std::string texture = ResolvedLightTexture(asset); !texture.empty())
                        sceneLight.texture = std::make_shared<ImageTexture<Color3>>(
                            texture, TextureColorSpace(file));
                }
                scene.lights.push_back(std::move(sceneLight));
            }
            // Link masks of the placements, by the prims they place.
            scene.lightLinking = links.active();
            if (links.active() && usdMesh)
                for (auto const& [id, ids] : usdMesh->attachedIds)
                    scene.placement_link_masks[int(id)] = links.masks(ids.path);
            if (domes.size() == 1) {
                scene.environment = domes.front().get();
            } else if (domes.size() > 1) {
                std::vector<const Environment*> parts;
                for (auto const& dome : domes) parts.push_back(dome.get());
                env = std::make_unique<CompositeEnvironment>(std::move(parts));
                scene.environment = env.get();
            }
        }
        progress.Stage("Building the scene");
        scene.commitScene();
        progress.Stage("");
        // A scene with no lights, emissive geometry or environment would render
        // black. Light it with usdview's default dome, which the Hydra delegate
        // applies by the same rule (scene_bridge.cpp _ApplyDefaultDome).
        if (o.defaultDome && scene.environment == &background && scene.lights.empty() &&
            !prism::device::SceneHasEmitters(scene)) {
            if (const std::string texture = DefaultDomeTexture(); texture.empty()) {
                std::cerr << "PrismRT: the scene has no lights, emissive geometry or environment, and USD's "
                             "default dome texture was not found; the image will be black (pass --env or "
                             "--env-color)\n";
            } else {
                // As a USD dome (intensity 1, exposure 0, untransformed), like
                // usdview's.
                envImage = std::make_unique<ImageTexture<Color3>>(texture, prism::color::kLightColorSpace);
                env = std::make_unique<TransformedEnvironmentMap>(envImage.get(), Color3(1.0f),
                                                                  std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1});
                scene.environment = env.get();
                std::cerr << "PrismRT: the scene has no lights, emissive geometry or environment; lighting it "
                             "with USD's default dome (" << texture << "). --env, --env-color or "
                             "--default-dome false override this\n";
            }
        }
        if (const char* report = std::getenv("PRISM_GPU_PROGRAM_REPORT"); report && *report == '1') {
            if (!prism::device::ReportGpuPrograms(scene, std::cout))
                std::cerr << "PrismRT: this build has no Vulkan device\n";
            return 0;
        }
        progress.Stage("Creating the render device");
        std::string deviceNote;
        auto renderDevice = prism::device::CreateDevice(o.device, renderer.options().devices, &deviceNote);
        progress.Stage("");
        if (!deviceNote.empty()) std::cerr << "PrismRT: " << deviceNote << '\n';
        const auto deviceInfo = renderDevice->Info();
        std::cout << "Rendering on " << prism::device::ToString(deviceInfo.kind) << " device: "
                  << deviceInfo.name << '\n';
        renderDevice->SceneChanged(scene);
        const double loadSeconds = seconds();
        using prism::output::AovKind;
        double renderSeconds = 0, meanSamples = o.samples, fractionAtMaxSamples = 1.0;
        double denoiseMilliseconds = 0;
        std::string denoiserName; // the last denoise's (for the profile)
        bool timeLimitReached = false;
        std::vector<prism::output::RenderedAov> rendered;
        for (size_t index = 0; index < views.size(); ++index) {
            auto const& view = views[index];
            const auto& rays = viewRays[index];
            const unsigned width = static_cast<unsigned>(view.dataWidth);
            const unsigned height = static_cast<unsigned>(view.dataHeight);
            // Products that differ only in their vars share one render.
            if (index == 0 || !view.SameView(views[index - 1])) rendered.clear();
            const double renderStart = seconds();
            auto renderedKind = [&](AovKind kind) {
                return std::any_of(rendered.begin(), rendered.end(),
                                   [&](auto const& aov) { return aov.kind == kind; });
            };
            // The light AOVs render with the colour: ask for them if a
            // product sharing this view has one.
            bool lightAovs = false;
            // The denoised colour, likewise (and for --denoise replace).
            bool denoise = o.denoise == "replace";
            for (size_t other = index; other < views.size() && views[other].SameView(view); ++other)
                for (auto const& var : views[other].vars) {
                    lightAovs |= prism::output::LightAov(var.kind);
                    denoise |= var.kind == AovKind::Denoised;
                }
            std::vector<AovKind> geometric;
            for (auto const& var : view.vars) {
                auto const* info = prism::output::FindAov(var.kind);
                if (!info || renderedKind(var.kind)) continue;
                if (!info->standalone) {
                    std::cerr << "PrismRT: render var <" << var.path << "> ('" << var.sourceName
                              << "') is only available in the Hydra delegate; skipped\n";
                    continue;
                }
                if (prism::output::ColorAov(var.kind) || prism::output::LightAov(var.kind)) {
                    prism::device::ImageRequest imageRequest;
                    imageRequest.camera = rays.parameters;
                    imageRequest.width = width;
                    imageRequest.height = height;
                    imageRequest.samples = o.samples;
                    imageRequest.seed = o.seed;
                    imageRequest.sampleClamp = o.clamp;
                    imageRequest.adaptive.threshold = o.adaptiveThreshold;
                    imageRequest.adaptive.minSamples = o.adaptiveMinSamples;
                    imageRequest.contributions = o.contributions;
                    imageRequest.nee = o.nee;
                    imageRequest.meanSamples = &meanSamples;
                    imageRequest.fractionAtMaxSamples = &fractionAtMaxSamples;
                    // The limit covers all of the views together.
                    imageRequest.progress = &imageProgress;
                    bool stopped = false;
                    if (o.timeLimit > 0) {
                        imageRequest.timeLimitSeconds = std::max(1e-6, o.timeLimit - renderSeconds);
                        imageRequest.timeLimitReached = &stopped;
                    }
                    std::vector<prism::device::LightAovPixel> lightPixels;
                    if (lightAovs) imageRequest.lightAovs = &lightPixels;
                    std::vector<float> denoisedPixels;
                    double viewDenoiseMilliseconds = 0;
                    if (denoise) {
                        imageRequest.denoise = o.denoiseSettings;
                        imageRequest.denoised = &denoisedPixels;
                        imageRequest.denoiseMilliseconds = &viewDenoiseMilliseconds;
                        imageRequest.denoiser = &denoiserName;
                    }
                    progress.BeginRender(&imageProgress, o.adaptiveThreshold > 0,
                        views.size() > 1 ? "view " + std::to_string(index + 1) + "/" + std::to_string(views.size()) : "",
                        imageRequest.timeLimitSeconds);
                    rendered.push_back({AovKind::Color, 3, renderDevice->RenderImage(scene, imageRequest)});
                    // Applied after the sample clamp, as in every render loop;
                    // adaptive sampling measures its error relative to the
                    // image's brightness, so scaling afterwards is the same.
                    const float scale = exposureScales[index];
                    if (scale != 1.0f)
                        for (float& value : rendered.back().pixels) value *= scale;
                    if (denoise) {
                        // HDR denoising is scale invariant (OIDN scales its input
                        // to its training range), so the exposure applies after.
                        if (scale != 1.0f)
                            for (float& value : denoisedPixels) value *= scale;
                        denoiseMilliseconds += viewDenoiseMilliseconds;
                        std::cout << "Denoised " << width << "x" << height << " in " << viewDenoiseMilliseconds
                                  << " ms with " << denoiserName << " ("
                                  << prism::device::ToString(o.denoiseSettings.quality) << " quality"
                                  << (o.denoiseSettings.prefilter ? ", prefiltered guides" : "") << ")\n";
                        // --denoise replace: the colour outputs are the denoised colour.
                        if (o.denoise == "replace") rendered.back().pixels = denoisedPixels;
                        rendered.push_back({AovKind::Denoised, 3, std::move(denoisedPixels)});
                    }
                    if (lightAovs) {
                        // Radiance AOVs take the colour's exposure; the albedo does not.
                        for (AovKind kind : {AovKind::Albedo, AovKind::Diffuse, AovKind::Specular, AovKind::Emission}) {
                            prism::output::RenderedAov plane{kind, 3, std::vector<float>(lightPixels.size() * 3)};
                            const float planeScale = kind == AovKind::Albedo ? 1.0f : scale;
                            for (size_t p = 0; p < lightPixels.size(); ++p) {
                                auto const& a = lightPixels[p];
                                const Color3& value = kind == AovKind::Albedo ? a.albedo
                                    : kind == AovKind::Diffuse ? a.diffuse
                                    : kind == AovKind::Specular ? a.specular : a.emission;
                                for (int c = 0; c < 3; ++c) plane.pixels[3 * p + c] = value[c] * planeScale;
                            }
                            rendered.push_back(std::move(plane));
                        }
                    }
                    progress.EndRender();
                    timeLimitReached |= stopped;
                } else if (std::find(geometric.begin(), geometric.end(), var.kind) == geometric.end()) {
                    geometric.push_back(var.kind);
                }
            }
            if (!geometric.empty()) {
                // Pixel-centre primary hits, as the Hydra delegate resolves
                // its geometric AOVs.
                const GfMatrix4d worldToView = rays.parameters.cameraToWorld.GetInverse();
                const GfMatrix4d projection = rays.parameters.inverseProjection.GetInverse();
                std::vector<prism::output::RenderedAov> planes;
                for (AovKind kind : geometric)
                    planes.push_back({kind, prism::output::AovComponents(kind),
                        std::vector<float>(size_t(width) * height * prism::output::AovComponents(kind))});
                const bool culling = scene.cullsBackFaces();
                // Prim, element and instance ids (-1 on a miss): USD scenes
                // number their placements as the Hydra delegate does; glTF
                // and OBJ scenes are one prim of triangles.
                auto resolveId = [&](AovKind kind, RayHit const& hit) -> float {
                    if (hit.geomid == int(RTC_INVALID_GEOMETRY_ID)) return -1.0f;
                    if (!usdMesh)
                        return kind == AovKind::PrimId ? 0.0f : kind == AovKind::ElementId ? float(hit.primid) : -1.0f;
                    auto found = usdMesh->attachedIds.find(unsigned(hit.geomid));
                    if (found == usdMesh->attachedIds.end()) return -1.0f;
                    auto const& ids = found->second;
                    if (kind == AovKind::PrimId) return float(ids.primId);
                    if (kind == AovKind::InstanceId) return float(ids.instanceId);
                    return hit.primid >= 0 && size_t(hit.primid) < ids.coarseFaces.size()
                        ? float(ids.coarseFaces[size_t(hit.primid)]) : -1.0f;
                };
                tbb::parallel_for(0u, height, [&](unsigned y) {
                    for (unsigned x = 0; x < width; ++x) {
                        const RayHit hit = scene.castRayCulled(prism::MakeCameraRayWithLens(rays.parameters,
                            float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr), culling);
                        for (auto& plane : planes) {
                            float* values = plane.pixels.data() + (size_t(y) * width + x) * plane.components;
                            if (prism::output::IntegerAov(plane.kind)) values[0] = resolveId(plane.kind, hit);
                            else prism::output::ResolveGeometricAov(plane.kind, hit, worldToView, projection, values);
                        }
                    }
                });
                for (auto& plane : planes) rendered.push_back(std::move(plane));
            }
            renderSeconds += seconds() - renderStart;
            std::vector<std::string> warnings;
            const prism::output::Image image = prism::output::BuildProductImage(view, rendered, &warnings);
            if (image.layers.empty()) {
                for (auto const& warning : warnings) std::cerr << "PrismRT: " << warning << '\n';
                std::cerr << "PrismRT: nothing to write to " << view.output << '\n';
                continue;
            }
            // EXR: every layer, linear working space, tagged. Other formats:
            // the colour display-encoded through the view, exactly once.
            prism::output::WriteImage(view.output, image, *colorManagement, &warnings);
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
        const auto textureStats = prism::TextureService::Acquire()->stats();
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
                  << ",\"texturePrepareCalls\":" << textureStats.prepareCalls
                  << ",\"textureMemoryCacheHits\":" << textureStats.memoryCacheHits
                  << ",\"textureDiskCacheHits\":" << textureStats.diskCacheHits
                  << ",\"textureSourceBytesHashed\":" << textureStats.sourceBytesHashed
                  << ",\"texturePreparationWorkerSeconds\":" << textureStats.preparationNanoseconds / 1e9
                  << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PrismRT: " << error.what() << '\n';
        return 1;
    }
}
