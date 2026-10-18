#include "standalone_cli.h"
#include "scene.h"
#include "camera_rays.h"
#include "camera_ray_diagnostics.h"
#include "device/device.h"
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
#include <tbb/parallel_for.h>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <chrono>

namespace {
struct Options {
    std::string scene, camera, environment, output = "output.exr";
    std::string exportScene, environmentSampling = "mis", textureFiltering = "filtered";
    prism::device::DevicePreference device = prism::device::DevicePreference::Auto;
    std::optional<Float3> cameraPosition, cameraTarget;
    std::optional<Color3> environmentColor;
    Float3 cameraUp{0, 1, 0};
    float verticalFov = 40, focusDistance = 10, defocusAngle = 0;
    bool explicitLens = false, widthSet = false, heightSet = false;
    int width = 1280, height = 720, samples = 256, depth = 32, roulette = 4;
    unsigned seed = 0;
    float exposure = 0, clamp = 10;
    float adaptiveThreshold = 0;
    int adaptiveMinSamples = 16;
    unsigned contributions = 7;
    prism::device::NeeSettings nee;
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
        "  --adaptive-threshold 0 (e.g. 0.01; 0 disables) --adaptive-min-samples 16\n"
        "                 with adaptive sampling, --spp is the per-pixel maximum\n"
        "  --debug-contributions all|visible|direct|indirect (noise analysis: keep only light seen\n"
        "                 directly, reaching the first hit, or bounced more often; comma-separated)\n"
        "  --environment-sampling mis|bsdf (default mis)\n"
        "  --nee-probability-min 0.05 --nee-from-depth 2 (from that depth on, environment light is\n"
        "                 sampled with probability clamp(path weight, min, 1); min 0 always samples)\n"
        "  --texture-filtering filtered|bilinear (default filtered)\n"
        "  --device auto|cpu|cpu-reference|vulkan|metal (default auto: GPU if available, else CPU;\n"
        "                 cpu-reference runs the original CPU integrator)\n"
        "PrismRT --list-devices\n"
        "EXR output is linear after the sample clamp; other formats use gamma 2.2.\n"
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
        else if (key == "--device") {
            if (!prism::device::ParseDevicePreference(value, &o.device))
                throw std::runtime_error("--device must be auto, cpu, cpu-reference, vulkan or metal");
        }
        else if (key == "--env") o.environment = value;
        else if (key == "--env-color") o.environmentColor = triple(value);
        else if (key == "--output") o.output = value;
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
    const bool explicitCamera = o.cameraPosition.has_value() && o.cameraTarget.has_value();
    if (o.scene.empty() || (o.camera.empty() && !explicitCamera))
        throw std::runtime_error("--scene and an explicit camera source are required (see --help)");
    if ((!o.camera.empty() && (o.cameraPosition || o.cameraTarget || o.explicitLens)) ||
        (o.cameraPosition.has_value() != o.cameraTarget.has_value()))
        throw std::runtime_error("Choose --camera or both --camera-position and --camera-target");
    if (explicitCamera && (o.verticalFov <= 0 || o.verticalFov >= 179 ||
        o.focusDistance <= 0 || o.defocusAngle < 0 || o.defocusAngle >= 179 ||
        (*o.cameraPosition - *o.cameraTarget).is_near_zero() ||
        cross(*o.cameraPosition - *o.cameraTarget, o.cameraUp).is_near_zero()))
        throw std::runtime_error("Invalid camera basis or lens parameters");
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
    return o;
}

// Independent USD camera ingestion, deliberately separate from the Hydra adapter.
struct UsdCameraRays {
    prism::CameraRayParameters parameters;

    UsdCameraRays(const GfCamera& c, int width, int height) {
        parameters.inverseProjection = CameraUtilConformedWindow(
            c.GetFrustum().ComputeProjectionMatrix(), CameraUtilFit,
            double(width) / height).GetInverse();
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
        auto seconds = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); };
        const auto extension = std::filesystem::path(o.scene).extension().string();
        const bool isUsd = extension == ".usd" || extension == ".usda" || extension == ".usdc" || extension == ".usdz";
        UsdStageRefPtr stage;
        if (isUsd) {
            stage = UsdStage::Open(o.scene);
            if (!stage) throw std::runtime_error("Cannot open USD scene: " + o.scene);
            std::string settingsPath;
            stage->GetMetadata(TfToken("renderSettingsPrimPath"), &settingsPath);
            if (!settingsPath.empty()) {
                UsdRenderSettings settings(stage->GetPrimAtPath(SdfPath(settingsPath)));
                GfVec2i resolution;
                if (settings && settings.GetResolutionAttr().Get(&resolution)) {
                    if (!o.widthSet) o.width = resolution[0];
                    if (!o.heightSet) o.height = resolution[1];
                }
            }
        }
        const double stageSeconds = seconds();
        if (o.width <= 0 || o.height <= 0 ||
            static_cast<uint64_t>(o.width) * o.height > std::numeric_limits<unsigned>::max() / 4)
            throw std::runtime_error("Authored render resolution is invalid or too large");
        GfCamera camera;
        if (!o.camera.empty()) {
            if (!stage) throw std::runtime_error("--camera prim paths require a USD scene");
            UsdGeomCamera authored(stage->GetPrimAtPath(SdfPath(o.camera)));
            if (!authored) throw std::runtime_error("Not a USD camera: " + o.camera);
            camera = authored.GetCamera(UsdTimeCode::Default());
        } else {
            auto asGf = [](Float3 v) { return GfVec3d(v.x(),v.y(),v.z()); };
            GfMatrix4d view(1); view.SetLookAt(asGf(*o.cameraPosition), asGf(*o.cameraTarget), asGf(o.cameraUp));
            camera.SetTransform(view.GetInverse());
            camera.SetPerspectiveFromAspectRatioAndFieldOfView(double(o.width)/o.height, o.verticalFov, GfCamera::FOVVertical);
            camera.SetFocusDistance(o.focusDistance);
            const float radius = o.focusDistance * std::tan(deg2rad(o.defocusAngle/2));
            camera.SetFStop(radius > 0 ? camera.GetFocalLength()*0.1f/(2*radius) : 0);
            camera.SetClippingRange(GfRange1f(0.001f, 1e30f));
        }
        if (o.width <= 0 || o.height <= 0 || uint64_t(o.width)*o.height > std::numeric_limits<unsigned>::max()/4)
            throw std::runtime_error("Invalid authored image dimensions");
        const UsdCameraRays rays(camera, o.width, o.height);
        prism::DumpCameraRayDiagnostics(
            rays.parameters, static_cast<unsigned int>(o.width),
            static_cast<unsigned int>(o.height), o.seed, "standalone");
        mdl::Context context;
        if (!context.is_valid()) throw std::runtime_error(context.get_error());
        SceneManager scene(device.value);
        const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        const std::string libraryPath = libraries ? libraries : "";
        std::unique_ptr<Mesh> mesh;
        if (isUsd) mesh = std::make_unique<USDMesh>(o.scene, &context, libraryPath, libraryPath + "/mdl");
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
        ColorTexture black(Color3(0));
        ConstantColorBackground background(&black);
        std::unique_ptr<ImageTexture<Color3>> envImage;
        std::unique_ptr<Environment> env;
        std::unique_ptr<ColorTexture> domeColor;
        scene.environment = &background;
        if (o.environmentColor) {
            domeColor = std::make_unique<ColorTexture>(*o.environmentColor * std::exp2(o.exposure));
            env = std::make_unique<ConstantColorBackground>(domeColor.get());
            scene.environment = env.get();
        } else if (!o.environment.empty()) {
            if (!std::filesystem::exists(o.environment)) throw std::runtime_error("Environment does not exist: " + o.environment);
            envImage = std::make_unique<ImageTexture<Color3>>(o.environment);
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
                    const Color3 multiplier = Float3(color[0], color[1], color[2]) * (intensity*std::exp2(exposure));
                    SdfAssetPath asset;
                    prim.GetAttribute(UsdLuxTokens->inputsTextureFile).Get(&asset);
                    const std::string texturePath = asset.GetResolvedPath();
                    if (!asset.GetAssetPath().empty() && texturePath.empty())
                        throw std::runtime_error("Cannot resolve dome texture: " + asset.GetAssetPath());
                    if (!texturePath.empty()) domeTextures.push_back(std::make_unique<ImageTexture<Color3>>(texturePath));
                    else domeTextures.push_back(std::make_unique<ColorTexture>(Color3(1)));
                    const auto inverse = world.GetInverse();
                    std::array<float,9> linear;
                    for (int row=0; row<3; ++row) for (int col=0; col<3; ++col) linear[row*3+col] = float(inverse[col][row]);
                    domes.push_back(std::make_unique<TransformedEnvironmentMap>(domeTextures.back().get(), multiplier, linear));
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
                std::array<double, 16> matrix{};
                for (int row = 0; row < 4; ++row)
                    for (int column = 0; column < 4; ++column) matrix[row * 4 + column] = world[row][column];
                prism::SceneLight sceneLight = prism::MakeSceneLight(params, matrix);
                if (params.shape == prism::LightShape::Rect) {
                    SdfAssetPath asset;
                    UsdLuxRectLight(prim).GetTextureFileAttr().Get(&asset);
                    if (!asset.GetResolvedPath().empty())
                        sceneLight.texture = std::make_shared<ImageTexture<Color3>>(asset.GetResolvedPath());
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
        prism::device::ImageRequest imageRequest;
        imageRequest.camera = rays.parameters;
        imageRequest.width = static_cast<unsigned>(o.width);
        imageRequest.height = static_cast<unsigned>(o.height);
        imageRequest.samples = o.samples;
        imageRequest.seed = o.seed;
        imageRequest.sampleClamp = o.clamp;
        imageRequest.adaptive.threshold = o.adaptiveThreshold;
        imageRequest.adaptive.minSamples = o.adaptiveMinSamples;
        imageRequest.contributions = o.contributions;
        imageRequest.nee = o.nee;
        double meanSamples = o.samples;
        imageRequest.meanSamples = &meanSamples;
        const std::vector<float> pixels = renderDevice->RenderImage(scene, imageRequest);
        const double renderSeconds = seconds() - loadSeconds;
        OIIO::ImageSpec spec(o.width, o.height, 3, OIIO::TypeDesc::FLOAT);
        OIIO::ImageBuf image(spec);
        image.set_pixels(OIIO::ROI(), OIIO::TypeDesc::FLOAT, pixels.data());
        if (std::filesystem::path(o.output).extension() != ".exr") {
            OIIO::ImageBufAlgo::clamp(image, image, 0.0f, 1.0f);
            OIIO::ImageBufAlgo::pow(image, image, 1.0f/2.2f);
        }
        if (!image.write(o.output)) throw std::runtime_error("Cannot write image: " + image.geterror());
        std::cout << "Rendered " << o.width << "x" << o.height << " at " << o.samples << " spp";
        if (imageRequest.adaptive.Enabled()) std::cout << " (adaptive, mean " << meanSamples << ")";
        std::cout << " to " << o.output << '\n';
        const auto textureStats = prism::TextureService::Acquire()->stats();
        std::cout << "[PrismProfile] {\"stageSeconds\":" << stageSeconds
                  << ",\"loadSeconds\":" << loadSeconds
                  << ",\"renderSeconds\":" << renderSeconds
                  << ",\"meanSamples\":" << meanSamples
                  << ",\"outputSeconds\":" << seconds()-loadSeconds-renderSeconds
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
