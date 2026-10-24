#include "standalone_cli.h"
#include "scene.h"
#include "render_defaults.h"
#include "materialx/loader.h"
#include "camera_rays.h"
#include "camera_ray_diagnostics.h"
#include "device/device.h"
#include "device/light_table.h"
#include "color/color_management.h"
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
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdRender/settings.h>
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
    std::string exportScene, environmentSampling = "mis", textureFiltering = "filtered";
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
    bool progress = true; // --no-progress
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
        "PrismRT --export-scene InOneWeekend|CornellBox|QuadTest --seed 0 --output scene.usda\n"
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
        "  --honor-double-sided true|false (default false: USD doubleSided is ignored; true: back faces of\n"
        "                 single-sided meshes are not hit, unless their material transmits)\n"
        "  --default-dome true|false (default true: a scene with no lights, emissive geometry or\n"
        "                 environment is lit by USD's default dome, as in usdview)\n"
        "  --device auto|cpu|cpu-reference|vulkan|metal (default auto: GPU if available, else CPU;\n"
        "                 cpu-reference runs the original CPU integrator)\n"
        "  --ocio-config FILE|URI (default $OCIO, else ocio://studio-config-latest)\n"
        "  --working-space NAME (default ACEScg; lin_rec709 renders in linear Rec.709 as before)\n"
        "  --display NAME --view NAME (8-bit outputs; default the config's: sRGB - Display and\n"
        "                 ACES 2.0 - SDR 100 nits (Rec.709). The earlier gamma 2.2 output is\n"
        "                 --display \"Gamma 2.2 Rec.709 - Display\" --view Un-tone-mapped)\n"
        "PrismRT --list-devices\n"
        "EXR output is linear in the working space after the sample clamp; other formats go\n"
        "through the display and view. Light colors are linear Rec.709.\n"
        "The environment option follows the original EnvironmentMap radiance convention.\n"
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
        else if (key == "--export-scene") o.exportScene = value;
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
            if (!prism::device::ParseDevicePreference(value, &o.device))
                throw std::runtime_error("--device must be auto, cpu, cpu-reference, vulkan or metal");
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
        else if (key == "--gpu-texture-budget-mib") o.gpuTextureBudgetMiB = integer(value);
        else if (key == "--sss-fallback") o.sssFallback = boolean(key, value);
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
    if (!o.exportScene.empty()) {
        if (!o.scene.empty() || !o.camera.empty() || o.cameraPosition || o.cameraTarget)
            throw std::runtime_error("--export-scene cannot be combined with scene or camera options");
        return o;
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
    if (!o.given.count("--time-limit"))
        if (auto const* v = value("timeLimitSeconds"); v && v->CanCast<double>())
            o.timeLimit = VtValue::Cast<double>(*v).UncheckedGet<double>();
    booleanSetting("textureStreaming", "--texture-streaming", &o.textureStreaming);
    integerSetting("gpuTextureBudgetMiB", "--gpu-texture-budget-mib", &o.gpuTextureBudgetMiB);
    booleanSetting("sssFallback", "--sss-fallback", &o.sssFallback);
    // Settings of the delegate's mesh conversion: the standalone loader has
    // no subdivision surfaces or displacement yet (docs/feature_roadmap.md).
    for (const char* name : {"subdivisionLevel", "subdivisionMaxFaces", "subdivisionBudgetFaces",
                             "displacementLevel", "displacementMaxFaces"})
        if (value(name))
            std::cerr << "PrismRT: prism:" << name << " does not apply to the standalone loader "
                         "(no subdivision surfaces or displacement); ignored\n";
    if (auto name = textSetting("device", "--device"); !name.empty() &&
        !prism::device::ParseDevicePreference(name, &o.device))
        throw std::runtime_error("prism:device must be auto, cpu, cpu-reference, vulkan or metal");
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
        if (argc == 2 && std::string(argv[1]) == "--list-devices") {
            for (auto const& info : prism::device::AvailableDevices())
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
        ProgressPrinter progress(o.progress && o.exportScene.empty());
        EmbreeDevice device;
        if (!device.value) throw std::runtime_error("Cannot create Embree device");
        if (!o.exportScene.empty()) {
            unsigned seed = o.seed;
            auto exportScene = [&](auto& scene) { scene.exportToUSDA(o.output); };
            if (o.exportScene == "InOneWeekend") { InOneWeekendFinal s(device.value, seed); exportScene(s); }
            else if (o.exportScene == "CornellBox") { CornellBox s(device.value, seed); exportScene(s); }
            else if (o.exportScene == "QuadTest") { QuadTest s(device.value, seed); exportScene(s); }
            else throw std::runtime_error("Unknown procedural scene: " + o.exportScene);
            std::cout << "Exported " << o.exportScene << " to " << o.output << '\n';
            return 0;
        }
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
            views.push_back(std::move(view));
        }
        if (job && !job->includedPurposes.empty()) {
            const auto& purposes = job->includedPurposes;
            const bool onlyDefaults = std::all_of(purposes.begin(), purposes.end(), [](TfToken const& p) {
                return p == UsdGeomTokens->default_ || p == UsdGeomTokens->render;
            });
            if (!onlyDefaults)
                std::cerr << "PrismRT: includedPurposes other than default and render are not supported by the "
                             "standalone loader yet; ignored\n";
        }
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
        auto cameraFor = [&](prism::output::RenderProductRequest const& view) {
            GfCamera camera;
            if (!view.camera.IsEmpty()) {
                if (!stage) throw std::runtime_error("--camera prim paths require a USD scene");
                UsdGeomCamera authored(stage->GetPrimAtPath(view.camera));
                if (!authored) throw std::runtime_error("Not a USD camera: " + view.camera.GetString());
                camera = authored.GetCamera(UsdTimeCode::Default());
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
            return UsdCameraRays(camera, view);
        };
        // Cameras are checked before the scene loads.
        std::vector<UsdCameraRays> viewRays;
        for (auto const& view : views) viewRays.push_back(cameraFor(view));
        prism::DumpCameraRayDiagnostics(
            viewRays.front().parameters, static_cast<unsigned int>(views.front().dataWidth),
            static_cast<unsigned int>(views.front().dataHeight), o.seed, "standalone");
        progress.Stage("Loading scene");
        mdl::Context context;
        if (!context.is_valid()) throw std::runtime_error(context.get_error());
        // Before any material loads.
        DocumentLoader::SetSssFallback(o.sssFallback);
        SceneManager scene(device.value);
        const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        const std::string libraryPath = libraries ? libraries : "";
        std::unique_ptr<Mesh> mesh;
        if (isUsd) {
            std::vector<TfToken> bindingPurposes;
            if (job) bindingPurposes.assign(job->materialBindingPurposes.begin(), job->materialBindingPurposes.end());
            mesh = std::make_unique<USDMesh>(o.scene, &context, libraryPath, libraryPath + "/mdl",
                                             std::move(bindingPurposes));
        }
        else if (extension == ".gltf" || extension == ".glb") {
            mesh = std::make_unique<glTFMesh>(o.scene, &context, libraryPath, libraryPath + "/mdl");
        }
        else if (extension == ".obj") mesh = std::make_unique<ObjMesh>(o.scene);
        else throw std::runtime_error("Unsupported scene format: " + extension);
        const auto usdMesh = dynamic_cast<USDMesh*>(mesh.get());
        if ((mesh->vertices.empty() || mesh->indices.empty()) &&
            (!usdMesh || usdMesh->sphereCount() == 0))
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
            env = std::make_unique<EnvironmentMap>(envImage.get());
            scene.environment = env.get();
        }
        std::vector<std::unique_ptr<Texture<Color3>>> domeTextures;
        std::vector<std::unique_ptr<Environment>> domes;
        if (stage) {
            UsdGeomXformCache transforms;
            const bool useDomes = !o.environmentColor && o.environment.empty();
            for (const auto& prim : stage->Traverse()) {
                if (!prim.HasAPI<UsdLuxLightAPI>()) continue;
                if (UsdGeomImageable(prim).ComputeVisibility() == UsdGeomTokens->invisible) continue;
                UsdLuxLightAPI light(prim);
                GfVec3f color(1); float intensity = 1, exposure = 0;
                light.GetColorAttr().Get(&color); light.GetIntensityAttr().Get(&intensity);
                light.GetExposureAttr().Get(&exposure);
                bool enableTemperature = false; float temperature = 6500.0f;
                light.GetEnableColorTemperatureAttr().Get(&enableTemperature);
                light.GetColorTemperatureAttr().Get(&temperature);
                if (enableTemperature) color = GfCompMult(color, UsdLuxBlackbodyTemperatureAsRgb(temperature));
                const GfMatrix4d world = transforms.GetLocalToWorldTransform(prim);
                const bool isDome = prim.IsA<UsdLuxDomeLight>() || prim.IsA<UsdLuxDomeLight_1>();
                if (isDome) {
                    if (!useDomes) continue;
                    const Color3 multiplier = colorManagement->FromRec709(Float3(color[0], color[1], color[2])) *
                        (intensity*std::exp2(exposure));
                    SdfAssetPath asset;
                    const UsdAttribute file = prim.GetAttribute(UsdLuxTokens->inputsTextureFile);
                    file.Get(&asset);
                    const std::string texturePath = asset.GetResolvedPath();
                    if (!asset.GetAssetPath().empty() && texturePath.empty())
                        throw std::runtime_error("Cannot resolve dome texture: " + asset.GetAssetPath());
                    if (!texturePath.empty())
                        domeTextures.push_back(std::make_unique<ImageTexture<Color3>>(texturePath, TextureColorSpace(file)));
                    else domeTextures.push_back(std::make_unique<ColorTexture>(Color3(1)));
                    const auto inverse = world.GetInverse();
                    std::array<float,9> linear;
                    for (int row=0; row<3; ++row) for (int col=0; col<3; ++col) linear[row*3+col] = float(inverse[col][row]);
                    domes.push_back(std::make_unique<TransformedEnvironmentMap>(domeTextures.back().get(), multiplier, linear));
                    light.GetDiffuseAttr().Get(&domes.back()->diffuseFactor);
                    light.GetSpecularAttr().Get(&domes.back()->specularFactor);
                    continue;
                }
                prism::LightParameters params;
                if (prim.IsA<UsdLuxSphereLight>()) {
                    params.shape = prism::LightShape::Sphere;
                    UsdLuxSphereLight(prim).GetRadiusAttr().Get(&params.radius);
                    UsdLuxSphereLight(prim).GetTreatAsPointAttr().Get(&params.treatAsPoint);
                } else if (prim.IsA<UsdLuxRectLight>()) {
                    params.shape = prism::LightShape::Rect;
                    UsdLuxRectLight(prim).GetWidthAttr().Get(&params.width);
                    UsdLuxRectLight(prim).GetHeightAttr().Get(&params.height);
                } else if (prim.IsA<UsdLuxDiskLight>()) {
                    params.shape = prism::LightShape::Disk;
                    UsdLuxDiskLight(prim).GetRadiusAttr().Get(&params.radius);
                } else if (prim.IsA<UsdLuxCylinderLight>()) {
                    params.shape = prism::LightShape::Cylinder;
                    UsdLuxCylinderLight(prim).GetRadiusAttr().Get(&params.radius);
                    UsdLuxCylinderLight(prim).GetLengthAttr().Get(&params.length);
                } else if (prim.IsA<UsdLuxDistantLight>()) {
                    params.shape = prism::LightShape::Distant;
                    UsdLuxDistantLight(prim).GetAngleAttr().Get(&params.angle);
                } else {
                    std::cerr << "PrismRT: light type '" << prim.GetTypeName() << "' is not supported; skipped\n";
                    continue;
                }
                params.color = Color3(color[0], color[1], color[2]);
                params.intensity = intensity;
                params.exposure = exposure;
                light.GetNormalizeAttr().Get(&params.normalize);
                light.GetDiffuseAttr().Get(&params.diffuse);
                light.GetSpecularAttr().Get(&params.specular);
                // Hydra's light:cameraVisibility, which UsdImaging reads from
                // the prim's `cameraVisibility` attribute.
                if (const UsdAttribute visibility = prim.GetAttribute(TfToken("cameraVisibility")))
                    visibility.Get(&params.cameraVisible);
                std::array<double, 16> matrix{};
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column) matrix[row * 4 + column] = world[row][column];
                prism::SceneLight sceneLight = prism::MakeSceneLight(params, matrix);
                if (params.shape == prism::LightShape::Rect) {
                    SdfAssetPath asset;
                    const UsdAttribute file = UsdLuxRectLight(prim).GetTextureFileAttr();
                    file.Get(&asset);
                    if (!asset.GetResolvedPath().empty())
                        sceneLight.texture = std::make_shared<ImageTexture<Color3>>(
                            asset.GetResolvedPath(), TextureColorSpace(file));
                }
                scene.lights.push_back(std::move(sceneLight));
            }
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
                // usdview's, not --env's EnvironmentMap with its factor of two.
                envImage = std::make_unique<ImageTexture<Color3>>(texture, prism::color::kLightColorSpace);
                env = std::make_unique<TransformedEnvironmentMap>(envImage.get(), Color3(1.0f),
                                                                  std::array<float, 9>{1, 0, 0, 0, 1, 0, 0, 0, 1});
                scene.environment = env.get();
                std::cerr << "PrismRT: the scene has no lights, emissive geometry or environment; lighting it "
                             "with USD's default dome (" << texture << "). --env, --env-color or "
                             "--default-dome false override this\n";
            }
        }
        progress.Stage("Creating the render device");
        std::string deviceNote;
        auto renderDevice = prism::device::CreateDevice(o.device, &deviceNote);
        progress.Stage("");
        if (!deviceNote.empty()) std::cerr << "PrismRT: " << deviceNote << '\n';
        const auto deviceInfo = renderDevice->Info();
        std::cout << "Rendering on " << prism::device::ToString(deviceInfo.kind) << " device: "
                  << deviceInfo.name << '\n';
        renderDevice->SceneChanged(scene);
        const double loadSeconds = seconds();
        using prism::output::AovKind;
        double renderSeconds = 0, meanSamples = o.samples, fractionAtMaxSamples = 1.0;
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
            std::vector<AovKind> geometric;
            for (auto const& var : view.vars) {
                auto const* info = prism::output::FindAov(var.kind);
                if (!info || renderedKind(var.kind)) continue;
                if (!info->standalone) {
                    std::cerr << "PrismRT: render var <" << var.path << "> ('" << var.sourceName
                              << "') is only available in the Hydra delegate; skipped\n";
                    continue;
                }
                if (var.kind == AovKind::Color) {
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
                    progress.BeginRender(&imageProgress, o.adaptiveThreshold > 0,
                        views.size() > 1 ? "view " + std::to_string(index + 1) + "/" + std::to_string(views.size()) : "",
                        imageRequest.timeLimitSeconds);
                    rendered.push_back({AovKind::Color, 3, renderDevice->RenderImage(scene, imageRequest)});
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
                tbb::parallel_for(0u, height, [&](unsigned y) {
                    for (unsigned x = 0; x < width; ++x) {
                        const RayHit hit = scene.castRayCulled(prism::MakeCameraRayWithLens(rays.parameters,
                            float(x) + 0.5f, float(y) + 0.5f, width, height, nullptr), culling);
                        for (auto& plane : planes)
                            prism::output::ResolveGeometricAov(plane.kind, hit, worldToView, projection,
                                plane.pixels.data() + (size_t(y) * width + x) * plane.components);
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
