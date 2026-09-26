#include "standalone_cli.h"
#include "scene.h"
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <tbb/parallel_for.h>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <memory>

namespace {
struct Options {
    std::string scene, camera, environment, output = "output.exr";
    int width = 1280, height = 720, samples = 256, depth = 32, roulette = 4;
    unsigned seed = 0;
    float exposure = 0, clamp = 10;
};

void help() {
    std::cout << "PrismRT --scene FILE --camera /Camera [options]\n"
        "  --env FILE --env-exposure STOPS\n"
        "  --width 1280 --height 720 --spp 256\n"
        "  --max-depth 32 --rr-min-depth 4 --seed 0\n"
        "  --max-sample-radiance 10 --output output.exr\n"
        "EXR output is linear and unclamped; other formats use gamma 2.2.\n"
        "The environment option follows the original EnvironmentMap radiance convention.\n"
        "Without arguments, the original built-in scene is rendered.\n";
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

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (i + 1 == argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[++i];
        if (key == "--scene") o.scene = value;
        else if (key == "--camera") o.camera = value;
        else if (key == "--env") o.environment = value;
        else if (key == "--output") o.output = value;
        else if (key == "--width") o.width = integer(value);
        else if (key == "--height") o.height = integer(value);
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
    if (o.scene.empty() || o.camera.empty())
        throw std::runtime_error("--scene and --camera are required (see --help)");
    if (o.width <= 0 || o.height <= 0 || o.samples <= 0 || o.depth <= 0 || o.roulette < 0 || o.clamp < 0)
        throw std::runtime_error("Dimensions, spp and depth must be positive; roulette depth and clamp must be nonnegative");
    if (static_cast<uint64_t>(o.width) * o.height > std::numeric_limits<unsigned>::max() / 4)
        throw std::runtime_error("Image dimensions are too large");
    return o;
}

Float3 vector(const GfVec3d& v) { return Float3(float(v[0]), float(v[1]), float(v[2])); }

// Independent USD camera ingestion, deliberately separate from the Hydra adapter.
struct UsdCameraRays {
    GfMatrix4d inverseProjection, world;
    GfRange1f clipping;
    bool orthographic;
    double radius, focus;

    UsdCameraRays(const UsdGeomCamera& camera, int width, int height) {
        const GfCamera c = camera.GetCamera(UsdTimeCode::Default());
        inverseProjection = CameraUtilConformedWindow(
            c.GetFrustum().ComputeProjectionMatrix(), CameraUtilFit,
            double(width) / height).GetInverse();
        world = c.GetTransform();
        clipping = c.GetClippingRange();
        orthographic = c.GetProjection() == GfCamera::Orthographic;
        radius = c.GetFStop() > 0 ? c.GetFocalLength() * 0.1 / (2.0 * c.GetFStop()) : 0;
        focus = c.GetFocusDistance();
    }
    RTCRay ray(int x, int y, int width, int height, unsigned& seed) const {
        const double u = (x + rnd(seed)) / width;
        const double v = 1.0 - (y + rnd(seed)) / height;
        const GfVec3d nearPoint = inverseProjection.Transform(GfVec3d(2*u-1, 2*v-1, -1));
        GfVec3d origin = orthographic ? GfVec3d(nearPoint[0], nearPoint[1], 0) : GfVec3d(0);
        GfVec3d direction = orthographic ? GfVec3d(0, 0, -1) : nearPoint.GetNormalized();
        if (!orthographic && radius > 0 && focus > 0) {
            const Float3 disk = random_in_unit_disk(seed);
            const GfVec3d target = direction * (-focus / direction[2]);
            origin = GfVec3d(disk.x()*radius, disk.y()*radius, 0);
            direction = (target-origin).GetNormalized();
        }
        RTCRay r = initialize_ray(vector(world.Transform(origin)), vector(world.TransformDir(direction).GetNormalized()));
        const double denominator = std::max(1e-12, -direction[2]);
        r.tnear = float(std::max(0.0, double(clipping.GetMin())) / denominator);
        r.tfar = float(double(clipping.GetMax()) / denominator);
        return r;
    }
};

struct Device {
    RTCDevice value = rtcNewDevice(nullptr);
    ~Device() { if (value) rtcReleaseDevice(value); }
};
struct SceneRelease {
    SceneManager& manager;
    Mesh* mesh = nullptr;
    ~SceneRelease() {
        rtcReleaseScene(manager.scene);
        if (mesh) {
            rtcReleaseGeometry(mesh->geom);
            delete mesh->transform;
            mesh->transform = nullptr;
        }
    }
};
}

int runStandaloneCli(int argc, char** argv) {
    try {
        if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            help();
            return 0;
        }
        const Options o = parse(argc, argv);
        auto stage = UsdStage::Open(o.scene);
        if (!stage) throw std::runtime_error("Cannot open USD scene: " + o.scene);
        UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath(o.camera)));
        if (!camera) throw std::runtime_error("Not a USD camera: " + o.camera);
        const UsdCameraRays rays(camera, o.width, o.height);
        mdl::Context context;
        if (!context.is_valid()) throw std::runtime_error(context.get_error());
        Device device;
        if (!device.value) throw std::runtime_error("Cannot create Embree device");
        SceneManager scene(device.value);
        const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        const std::string libraryPath = libraries ? libraries : mx_libraries_path;
        auto mesh = std::make_unique<USDMesh>(o.scene, &context, libraryPath, libraryPath + "/mdl");
        SceneRelease release{scene};
        const auto id = mesh->setupEmbree(device.value, scene.scene);
        release.mesh = mesh.get();
        scene.geometry.emplace(id, mesh.get());
        scene.max_depth = o.depth;
        scene.rr_min_depth = o.roulette;
        ColorTexture black(Color3(0));
        ConstantColorBackground background(&black);
        std::unique_ptr<ImageTexture<Color3>> envImage;
        std::unique_ptr<EnvironmentMap> env;
        scene.environment = &background;
        if (!o.environment.empty()) {
            if (!std::filesystem::exists(o.environment)) throw std::runtime_error("Environment does not exist: " + o.environment);
            envImage = std::make_unique<ImageTexture<Color3>>(o.environment);
            envImage->adjustExposure(o.exposure);
            env = std::make_unique<EnvironmentMap>(envImage.get());
            scene.environment = env.get();
        }
        scene.commitScene();
        std::vector<float> pixels(size_t(o.width)*o.height*3);
        tbb::parallel_for(0, o.height, [&](int y) {
            for (int x = 0; x < o.width; ++x) {
                unsigned seed = tea<64>(unsigned(y*o.width) ^ o.seed, unsigned(x));
                Color3 sum(0);
                for (int sample = 0; sample < o.samples; ++sample) {
                    const RTCRay r = rays.ray(x, y, o.width, o.height, seed);
                    Color3 value = scene.trace(r, 0, Float3(1), seed);
                    const float peak = value.max_component();
                    if (o.clamp > 0 && peak > o.clamp) value *= o.clamp / peak;
                    sum += value;
                }
                sum /= float(o.samples);
                const size_t offset = (size_t(y)*o.width+x)*3;
                for (int c = 0; c < 3; ++c) pixels[offset+c] = sum[c];
            }
        });
        OIIO::ImageSpec spec(o.width, o.height, 3, OIIO::TypeDesc::FLOAT);
        OIIO::ImageBuf image(spec);
        image.set_pixels(OIIO::ROI(), OIIO::TypeDesc::FLOAT, pixels.data());
        if (std::filesystem::path(o.output).extension() != ".exr") {
            OIIO::ImageBufAlgo::clamp(image, image, 0.0f, 1.0f);
            OIIO::ImageBufAlgo::pow(image, image, 1.0f/2.2f);
        }
        if (!image.write(o.output)) throw std::runtime_error("Cannot write image: " + image.geterror());
        std::cout << "Rendered " << o.width << "x" << o.height << " at " << o.samples << " spp to " << o.output << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PrismRT: " << error.what() << '\n';
        return 1;
    }
}
