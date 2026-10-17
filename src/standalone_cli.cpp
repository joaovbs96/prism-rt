#include "standalone_cli.h"
#include "scene.h"
#include "camera_rays.h"
#include "camera_ray_diagnostics.h"
#include "device/device.h"
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdLux/domeLight.h>
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
        "  --environment-sampling mis|bsdf (default mis)\n"
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
        } else if (stage) {
            size_t domeCount = 0;
            UsdGeomXformCache transforms;
            for (const auto& prim : stage->Traverse()) {
                UsdLuxDomeLight dome(prim);
                if (!dome) continue;
                if (UsdGeomImageable(prim).ComputeVisibility() == UsdGeomTokens->invisible) continue;
                if (domeCount++) { std::cerr << "PrismRT: only the first dome light is supported\n"; continue; }
                GfVec3f color(1); float intensity = 1, exposure = 0;
                dome.GetColorAttr().Get(&color); dome.GetIntensityAttr().Get(&intensity); dome.GetExposureAttr().Get(&exposure);
                const Color3 multiplier = Float3(color[0], color[1], color[2]) * (intensity*std::exp2(exposure));
                SdfAssetPath asset; dome.GetTextureFileAttr().Get(&asset);
                const std::string texturePath = asset.GetResolvedPath();
                if (!asset.GetAssetPath().empty() && texturePath.empty())
                    throw std::runtime_error("Cannot resolve dome texture: " + asset.GetAssetPath());
                Texture<Color3>* texture;
                if (!texturePath.empty()) { envImage = std::make_unique<ImageTexture<Color3>>(texturePath); texture = envImage.get(); }
                else { domeColor = std::make_unique<ColorTexture>(Color3(1)); texture = domeColor.get(); }
                const auto inverse = transforms.GetLocalToWorldTransform(prim).GetInverse();
                std::array<float,9> linear;
                for (int row=0; row<3; ++row) for (int col=0; col<3; ++col) linear[row*3+col] = float(inverse[col][row]);
                env = std::make_unique<TransformedEnvironmentMap>(texture, multiplier, linear);
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
        std::cout << "Rendered " << o.width << "x" << o.height << " at " << o.samples << " spp to " << o.output << '\n';
        const auto textureStats = prism::TextureService::Acquire()->stats();
        std::cout << "[PrismProfile] {\"stageSeconds\":" << stageSeconds
                  << ",\"loadSeconds\":" << loadSeconds
                  << ",\"renderSeconds\":" << renderSeconds
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
