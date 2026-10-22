#include "standalone_cli.h"
#include "scene.h"
#include "camera_rays.h"
#include "camera_ray_diagnostics.h"
#include "device/device.h"
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

namespace {
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
    int width = 1280, height = 720, samples = 256, depth = 32, roulette = 4;
    unsigned seed = 0;
    float exposure = 0, clamp = 10;
    float adaptiveThreshold = 0;
    int adaptiveMinSamples = 16;
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
        "  --width 1280 --height 720 --spp 256\n"
        "  --max-depth 32 --rr-min-depth 4 --seed 0\n"
        "  --max-sample-radiance 10 --output output.exr\n"
        "  --render-settings /Render/Settings (default: the stage's renderSettingsPrimPath)\n"
        "  --output-dir DIR (where relative render product names are written; default .)\n"
        "                 USD scenes with RenderSettings take resolution, camera, pixel aspect\n"
        "                 ratio, data window, material binding purposes and prism:* settings from\n"
        "                 it; without --output or a camera option, each RenderProduct is written\n"
        "                 with its RenderVars (see docs/hydra.md)\n"
        "  --adaptive-threshold 0 (e.g. 0.01; 0 disables) --adaptive-min-samples 16\n"
        "                 with adaptive sampling, --spp is the per-pixel maximum\n"
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
        "Without --env, USD scenes use their authored dome lighting.\n";
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
    if (!o.given.count("--honor-double-sided"))
        if (auto const* v = value("honorDoubleSided"); v && v->IsHolding<bool>())
            o.honorDoubleSided = v->UncheckedGet<bool>();
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
        mdl::Context context;
        if (!context.is_valid()) throw std::runtime_error(context.get_error());
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
            mesh = std::make_unique<glTFMesh>(o.scene);
            std::cerr << "PrismRT glTF: legacy materials retain BSDF-only sampling where no matching PDF is available\n";
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
        scene.commitScene();
        std::string deviceNote;
        auto renderDevice = prism::device::CreateDevice(o.device, &deviceNote);
        if (!deviceNote.empty()) std::cerr << "PrismRT: " << deviceNote << '\n';
        const auto deviceInfo = renderDevice->Info();
        std::cout << "Rendering on " << prism::device::ToString(deviceInfo.kind) << " device: "
                  << deviceInfo.name << '\n';
        renderDevice->SceneChanged(scene);
        const double loadSeconds = seconds();
        using prism::output::AovKind;
        double renderSeconds = 0, meanSamples = o.samples;
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
                    rendered.push_back({AovKind::Color, 3, renderDevice->RenderImage(scene, imageRequest)});
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
            if (o.adaptiveThreshold > 0) std::cout << " (adaptive, mean " << meanSamples << ")";
            std::cout << " to " << view.output << '\n';
        }
        const double outputSeconds = seconds() - loadSeconds - renderSeconds;
        const auto textureStats = prism::TextureService::Acquire()->stats();
        std::cout << "[PrismProfile] {\"stageSeconds\":" << stageSeconds
                  << ",\"loadSeconds\":" << loadSeconds
                  << ",\"renderSeconds\":" << renderSeconds
                  << ",\"meanSamples\":" << meanSamples
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
