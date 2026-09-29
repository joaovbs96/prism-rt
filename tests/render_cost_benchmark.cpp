#include "../src/camera_rays.h"
#include "../src/render_profile.h"
#include "../src/scene.h"

#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/domeLight.h>

#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/texture.h>
#include <tbb/blocked_range.h>
#include <tbb/info.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

struct Options {
    std::filesystem::path output = "render-cost-benchmark";
    int width = 256;
    int height = 144;
    int samples = 32;
    bool geometryExperiment = false;
    bool residentHdr = false;
    bool textureSystem = false;
    bool skipUsd = false;
    std::filesystem::path environmentFile;
};

Options Parse(int argc, char **argv)
{
    Options result;
    int positional = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string value = argv[i];
        if (value == "--geometry-experiment") {
            result.geometryExperiment = true;
        } else if (value == "--resident-hdr") {
            result.residentHdr = true;
        } else if (value == "--texture-system") {
            result.textureSystem = true;
        } else if (value == "--env-file") {
            if (++i >= argc)
                throw std::runtime_error("--env-file requires a path");
            result.environmentFile = argv[i];
        } else if (value == "--skip-usd") {
            result.skipUsd = true;
        } else if (positional == 0) {
            result.output = value; ++positional;
        } else if (positional == 1) {
            result.width = std::stoi(value); ++positional;
        } else if (positional == 2) {
            result.height = std::stoi(value); ++positional;
        } else if (positional == 3) {
            result.samples = std::stoi(value); ++positional;
        } else {
            throw std::runtime_error(
                "usage: render_cost_benchmark [output-dir] [width] [height] [spp] [--geometry-experiment] [--resident-hdr|--texture-system] [--env-file HDR] [--skip-usd]");
        }
    }
    if (result.width <= 0 || result.height <= 0 || result.samples <= 0)
        throw std::runtime_error("width, height and spp must be positive");
    if (result.residentHdr && result.textureSystem)
        throw std::runtime_error(
            "--resident-hdr and --texture-system are mutually exclusive");
    return result;
}

struct Device {
    RTCDevice value = rtcNewDevice(nullptr);
    ~Device() { if (value) rtcReleaseDevice(value); }
};

struct SceneHandle {
    SceneManager *scene = nullptr;
    bool releaseDirectGeometry = false;
    ~SceneHandle() {
        if (!scene) return;
        if (scene->scene) rtcReleaseScene(scene->scene);
        if (releaseDirectGeometry)
            for (auto const &[id, geometry] : scene->geometry)
                if (geometry && geometry->geom) rtcReleaseGeometry(geometry->geom);
    }
};

prism::CameraRayParameters ReadCamera(UsdStageRefPtr const &stage,
                                      int width, int height)
{
    UsdGeomCamera authored(stage->GetPrimAtPath(SdfPath("/World/Camera")));
    if (!authored) throw std::runtime_error("exported stage has no /World/Camera");
    const GfCamera camera = authored.GetCamera(UsdTimeCode::Default());
    prism::CameraRayParameters result;
    result.inverseProjection = CameraUtilConformedWindow(
        camera.GetFrustum().ComputeProjectionMatrix(), CameraUtilFit,
        static_cast<double>(width) / height).GetInverse();
    result.cameraToWorld = camera.GetTransform();
    const GfRange1f clipping = camera.GetClippingRange();
    result.nearClip = clipping.GetMin();
    result.farClip = clipping.GetMax();
    result.orthographic = camera.GetProjection() == GfCamera::Orthographic;
    result.apertureRadius = camera.GetFStop() > 0.0
        ? camera.GetFocalLength() * 0.1 / (2.0 * camera.GetFStop()) : 0.0;
    result.focusDistance = camera.GetFocusDistance();
    return result;
}

struct BakedEnvironment {
    std::unique_ptr<ImageTexture<Color3>> texture;
    std::unique_ptr<EnvironmentMap> environment;
    std::unique_ptr<EnvironmentMap> referenceEnvironment;
    double texturePreprocessSeconds = 0.0;
    double probeRmsError = 0.0;
    double probeMaxError = 0.0;
    int sourceWidth = 0;
    int sourceHeight = 0;
};

class ImageBufEnvironmentMap final : public TransformedEnvironmentMap {
public:
    ImageBufEnvironmentMap(ImageTexture<Color3> *image, Color3 multiplier,
        std::array<float, 9> const &worldToEnvironment)
        : TransformedEnvironmentMap(image, multiplier, worldToEnvironment),
          transform(worldToEnvironment) {}

    Color3 radiance(Float3 const &world) const override
    {
        auto *image = dynamic_cast<ImageTexture<Color3> *>(texture);
        if (!image || width <= 0 || height <= 0) return Color3(0.0f);
        const Float3 local(
            transform[0] * world.x() + transform[1] * world.y() + transform[2] * world.z(),
            transform[3] * world.x() + transform[4] * world.y() + transform[5] * world.z(),
            transform[6] * world.x() + transform[7] * world.y() + transform[8] * world.z());
        const Float3 d = local.normalize().normalize();
        float lon = std::atan2f(d.z(), d.x());
        if (lon < 0.0f) lon += 2.0f * PI;
        const float lat = std::atan2(std::hypot(d.x(), d.z()), d.y());
        const float x = lon / (2.0f * PI) * width - 0.5f;
        const float y = lat / PI * height - 0.5f;
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const float tx = x - ix, ty = y - iy;
        auto texel = [&](int px, int py) {
            px = (px % width + width) % width;
            py = std::clamp(py, 0, height - 1);
            float pixel[4]{};
            image->buffer.getpixel(px, py, pixel);
            return Color3(pixel[0], pixel[1], pixel[2]);
        };
        Color3 value = multiplier * (
            (1.0f - ty) * ((1.0f - tx) * texel(ix, iy) + tx * texel(ix + 1, iy)) +
            ty * ((1.0f - tx) * texel(ix, iy + 1) + tx * texel(ix + 1, iy + 1)));
        for (int c = 0; c < 3; ++c)
            if (!std::isfinite(value[c]) || value[c] < 0.0f) value[c] = 0.0f;
        return value;
    }

    Color3 radiance(Float3 const &world,
                    prism::DirectionFootprint const &) const override
    {
        return radiance(world);
    }

private:
    std::array<float, 9> transform;
};

class ResidentEnvironmentMap final : public TransformedEnvironmentMap {
public:
    ResidentEnvironmentMap(ImageTexture<Color3> *image, Color3 multiplier,
                           std::array<float, 9> const &worldToEnvironment)
        : TransformedEnvironmentMap(image, multiplier, worldToEnvironment),
          transform(worldToEnvironment), pixels(
              static_cast<size_t>(image->width) * image->height * 3)
    {
        const OIIO::ROI roi(0, image->width, 0, image->height, 0, 1, 0, 3);
        if (!image->buffer.get_pixels(
                roi, OIIO::TypeDesc::FLOAT, pixels.data()))
            throw std::runtime_error(
                "cannot bulk-copy baked HDR pixels: " + image->buffer.geterror());
    }

    Color3 radiance(Float3 const &world) const override
    {
        // Preserve TransformedEnvironmentMap's identity-transform normalization,
        // followed by EnvironmentMap's own normalization.  This experiment only
        // replaces ImageBuf::getpixel calls with resident-vector reads.
        const Float3 local(
            transform[0] * world.x() + transform[1] * world.y() +
                transform[2] * world.z(),
            transform[3] * world.x() + transform[4] * world.y() +
                transform[5] * world.z(),
            transform[6] * world.x() + transform[7] * world.y() +
                transform[8] * world.z());
        const Float3 transformed = local.normalize();
        const Float3 d = transformed.normalize();
        float lon = std::atan2f(d.z(), d.x());
        if (lon < 0.0f) lon += 2.0f * PI;
        const float lat = std::atan2(std::hypot(d.x(), d.z()), d.y());
        const float x = lon / (2.0f * PI) * width - 0.5f;
        const float y = lat / PI * height - 0.5f;
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        const float tx = x - ix, ty = y - iy;
        auto texel = [&](int px, int py) {
            px = (px % width + width) % width;
            py = std::clamp(py, 0, height - 1);
            const size_t offset = (static_cast<size_t>(py) * width + px) * 3;
            return Color3(pixels[offset], pixels[offset + 1],
                          pixels[offset + 2]);
        };
        Color3 result = multiplier * (
            (1 - ty) * ((1 - tx) * texel(ix, iy) +
                        tx * texel(ix + 1, iy)) +
            ty * ((1 - tx) * texel(ix, iy + 1) +
                  tx * texel(ix + 1, iy + 1)));
        for (int channel = 0; channel < 3; ++channel)
            if (!std::isfinite(result[channel]) || result[channel] < 0.0f)
                result[channel] = 0.0f;
        return result;
    }

    Color3 radiance(Float3 const &world,
                    prism::DirectionFootprint const &) const override
    {
        return radiance(world);
    }

private:
    std::array<float, 9> transform;
    std::vector<float> pixels;
};

class TextureSystemEnvironmentMap final : public TransformedEnvironmentMap {
public:
    TextureSystemEnvironmentMap(ImageTexture<Color3> *image, Color3 multiplier,
        std::array<float, 9> const &worldToEnvironment,
        std::filesystem::path const &texturePath)
        : TransformedEnvironmentMap(image, multiplier, worldToEnvironment),
          transform(worldToEnvironment),
          system(OIIO::TextureSystem::create(false))
    {
        if (!system) throw std::runtime_error("cannot create TextureSystem");
        if (!system->attribute("max_memory_MB", 128.0f))
            throw std::runtime_error("cannot set TextureSystem cache budget");
        handle = system->get_texture_handle(OIIO::ustring(texturePath.string()));
        if (!handle || !system->good(handle))
            throw std::runtime_error(
                "cannot open benchmark texture: " + texturePath.string());
    }

    Color3 radiance(Float3 const &world) const override
    {
        const Float3 local(
            transform[0] * world.x() + transform[1] * world.y() +
                transform[2] * world.z(),
            transform[3] * world.x() + transform[4] * world.y() +
                transform[5] * world.z(),
            transform[6] * world.x() + transform[7] * world.y() +
                transform[8] * world.z());
        const Float3 transformed = local.normalize();
        const Float3 d = transformed.normalize();
        float lon = std::atan2f(d.z(), d.x());
        if (lon < 0.0f) lon += 2.0f * PI;
        const float lat = std::atan2(std::hypot(d.x(), d.z()), d.y());
        OIIO::TextureOpt options;
        options.swrap = OIIO::TextureOpt::WrapPeriodic;
        options.twrap = OIIO::TextureOpt::WrapClamp;
        options.mipmode = OIIO::TextureOpt::MipModeNoMIP;
        options.interpmode = OIIO::TextureOpt::InterpBilinear;
        float pixel[3]{};
        const bool ok = system->texture(
            handle, system->get_perthread_info(), options,
            lon / (2.0f * PI), lat / PI,
            0.0f, 0.0f, 0.0f, 0.0f, 3, pixel);
        if (!ok) throw std::runtime_error("TextureSystem lookup failed: " + system->geterror());
        Color3 result = multiplier * Color3(pixel[0], pixel[1], pixel[2]);
        for (int channel = 0; channel < 3; ++channel)
            if (!std::isfinite(result[channel]) || result[channel] < 0.0f)
                result[channel] = 0.0f;
        return result;
    }

    Color3 radiance(Float3 const &world,
                    prism::DirectionFootprint const &) const override
    {
        return radiance(world);
    }

    std::string Statistics() const { return system->getstats(2, true); }

private:
    std::array<float, 9> transform;
    std::shared_ptr<OIIO::TextureSystem> system;
    OIIO::TextureSystem::TextureHandle *handle = nullptr;
};

BakedEnvironment ReadEnvironment(UsdStageRefPtr const &stage,
                                 Options const &options)
{
    UsdLuxDomeLight dome(stage->GetPrimAtPath(SdfPath("/World/Environment")));
    if (!dome) throw std::runtime_error("exported stage has no baked dome");
    SdfAssetPath asset;
    dome.GetTextureFileAttr().Get(&asset);
    const std::string path = options.environmentFile.empty()
        ? asset.GetResolvedPath() : options.environmentFile.string();
    if (path.empty()) throw std::runtime_error("baked dome texture did not resolve");
    GfVec3f color(1.0f);
    float intensity = 1.0f, exposure = 0.0f;
    dome.GetColorAttr().Get(&color);
    dome.GetIntensityAttr().Get(&intensity);
    dome.GetExposureAttr().Get(&exposure);
    const float scale = intensity * std::exp2(exposure);
    UsdGeomXformCache cache;
    const GfMatrix4d inverse = cache.GetLocalToWorldTransform(
        dome.GetPrim()).GetInverse();
    std::array<float, 9> linear{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            linear[row * 3 + column] = float(inverse[column][row]);
    if (options.residentHdr || options.textureSystem) {
        const std::array<float, 9> identity{1,0,0, 0,1,0, 0,0,1};
        if (linear != identity)
            throw std::runtime_error(
                "resident/TextureSystem experiment requires an identity dome transform");
    }
    BakedEnvironment result;
    result.texture = std::make_unique<ImageTexture<Color3>>(path);
    result.sourceWidth = result.texture->width;
    result.sourceHeight = result.texture->height;
    const Color3 multiplier(color[0] * scale, color[1] * scale,
                            color[2] * scale);
    result.referenceEnvironment = std::make_unique<ImageBufEnvironmentMap>(
        result.texture.get(), multiplier, linear);
    if (options.residentHdr)
        result.environment = std::make_unique<ResidentEnvironmentMap>(
            result.texture.get(), multiplier, linear);
    else if (options.textureSystem) {
        const auto preprocessStart = std::chrono::steady_clock::now();
        const std::filesystem::path txPath =
            options.output / "benchmark-environment.tx";
        OIIO::ImageSpec config;
        config.format = OIIO::TypeDesc::FLOAT;
        config.tile_width = config.tile_height = 64;
        config.attribute("maketx:resize", 0);
        config.attribute("maketx:forcefloat", 1);
        config.attribute("maketx:fileformatname", "openexr");
        if (!OIIO::ImageBufAlgo::make_texture(
                OIIO::ImageBufAlgo::MakeTxTexture, result.texture->buffer,
                txPath.string(), config))
            throw std::runtime_error(
                "cannot create benchmark .tx: " + OIIO::geterror());
        result.texturePreprocessSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - preprocessStart).count();
        result.environment = std::make_unique<TextureSystemEnvironmentMap>(
            result.texture.get(), multiplier, linear, txPath);

        double sumSquared = 0.0;
        size_t values = 0;
        auto probe = [&](Float3 direction) {
            const Color3 expected = result.referenceEnvironment->radiance(direction);
            const Color3 actual = result.environment->radiance(direction);
            for (int channel = 0; channel < 3; ++channel) {
                const double error = double(actual[channel]) - expected[channel];
                sumSquared += error * error;
                result.probeMaxError = std::max(
                    result.probeMaxError, std::abs(error));
                ++values;
            }
        };
        probe(Float3(0.0f, 1.0f, 0.0f));
        probe(Float3(0.0f, -1.0f, 0.0f));
        probe(Float3(1.0f, 0.0f, 1.0e-7f));
        probe(Float3(1.0f, 0.0f, -1.0e-7f));
        unsigned probeSeed = 0x54455853u;
        for (unsigned i = 0; i < 1024; ++i) {
            const float y = 1.0f - 2.0f * rnd(probeSeed);
            const float phi = 2.0f * PI * rnd(probeSeed);
            const float radius = std::sqrt(std::max(0.0f, 1.0f - y * y));
            probe(Float3(radius * std::cos(phi), y,
                         radius * std::sin(phi)));
        }
        result.probeRmsError = std::sqrt(sumSquared / values);
    } else
        result.environment = std::make_unique<ImageBufEnvironmentMap>(
            result.texture.get(), multiplier, linear);
    return result;
}

struct ImageResult {
    std::vector<float> pixels;
    double seconds = 0.0;
    uint64_t checksum = 1469598103934665603ull;
};

void Accumulate(prism::profiling::Counters *destination,
                prism::profiling::Counters const &source)
{
    for (size_t i = 0; i < destination->calls.size(); ++i) {
        destination->calls[i] += source.calls[i];
        destination->nanoseconds[i] += source.nanoseconds[i];
    }
}

ImageResult Render(SceneManager &scene,
                   prism::CameraRayParameters const &camera,
                   int width, int height, int samples,
                   prism::profiling::Counters *profile = nullptr,
                   uint64_t *workerNanoseconds = nullptr)
{
    ImageResult result;
    result.pixels.resize(static_cast<size_t>(width) * height * 3);
    std::mutex profileMutex;
    uint64_t workerTotal = 0;
    const auto begin = std::chrono::steady_clock::now();
    tbb::parallel_for(tbb::blocked_range<int>(0, height, 4),
        [&](tbb::blocked_range<int> const &rows) {
            prism::profiling::Counters local{};
            const auto workerBegin = std::chrono::steady_clock::now();
            prism::profiling::active = profile ? &local : nullptr;
            for (int y = rows.begin(); y != rows.end(); ++y) {
                for (int x = 0; x < width; ++x) {
                    unsigned seed = tea<64>(unsigned(y * width), unsigned(x));
                    Color3 sum(0.0f);
                    for (int sample = 0; sample < samples; ++sample) {
                        const RTCRay ray = prism::GenerateCameraRays(
                            camera, unsigned(x), unsigned(y), unsigned(width),
                            unsigned(height), seed).sampled;
                        sum += scene.trace(ray, 0, Color3(1.0f), seed);
                    }
                    sum /= float(samples);
                    const size_t offset =
                        (static_cast<size_t>(y) * width + x) * 3;
                    result.pixels[offset] = sum.x();
                    result.pixels[offset + 1] = sum.y();
                    result.pixels[offset + 2] = sum.z();
                }
            }
            prism::profiling::active = nullptr;
            const uint64_t elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - workerBegin).count());
            if (profile) {
                std::lock_guard lock(profileMutex);
                Accumulate(profile, local);
                workerTotal += elapsed;
            }
        });
    result.seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    if (workerNanoseconds) *workerNanoseconds = workerTotal;
    for (float value : result.pixels) {
        if (!std::isfinite(value))
            throw std::runtime_error("render produced a non-finite pixel");
        result.checksum ^= std::bit_cast<uint32_t>(value);
        result.checksum *= 1099511628211ull;
    }
    return result;
}

void WriteExr(std::filesystem::path const &path, ImageResult const &image,
              int width, int height)
{
    OIIO::ImageBuf output(OIIO::ImageSpec(
        width, height, 3, OIIO::TypeDesc::FLOAT));
    output.set_pixels(OIIO::ROI(), OIIO::TypeDesc::FLOAT,
                      image.pixels.data());
    if (!output.write(path.string()))
        throw std::runtime_error("cannot write " + path.string() + ": " +
                                 output.geterror());
}

double Median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void Compare(char const *label, ImageResult const &a, ImageResult const &b)
{
    double squared = 0.0, reference = 0.0, mean = 0.0;
    for (size_t i = 0; i < a.pixels.size(); ++i) {
        const double d = double(b.pixels[i]) - a.pixels[i];
        squared += d * d;
        reference += double(a.pixels[i]) * a.pixels[i];
        mean += d;
    }
    const double rmse = std::sqrt(squared / a.pixels.size());
    const double nrmse = reference > 0.0
        ? std::sqrt(squared / reference) : 0.0;
    std::cout << "comparison " << label << " rmse=" << rmse
              << " nrmse=" << nrmse
              << " meanDifference=" << mean / a.pixels.size() << '\n';
}

char const *StageName(size_t index)
{
    static constexpr char const *names[] = {
        "intersection", "hitPreparation", "materialSample",
        "materialEvaluate", "shadowIntersection", "shadowHitPreparation",
        "cutout", "environment"};
    return index < std::size(names) ? names[index] : "unknown";
}

struct SpherePrototype {
    std::unique_ptr<Sphere> geometry;
    RTCScene scene = nullptr;
    SpherePrototype(RTCDevice device, float radius)
        : geometry(std::make_unique<Sphere>(Float3(0.0f), radius, nullptr)),
          scene(rtcNewScene(device))
    {
        geometry->setupEmbree(device, scene);
        rtcCommitScene(scene);
    }
    ~SpherePrototype()
    {
        if (scene) rtcReleaseScene(scene);
        if (geometry && geometry->geom) rtcReleaseGeometry(geometry->geom);
        if (geometry) { delete geometry->transform; geometry->transform = nullptr; }
    }
};

struct InstancedExperiment {
    std::unique_ptr<SceneManager> scene;
    std::map<float, std::unique_ptr<SpherePrototype>> prototypes;
    InstancedExperiment() = default;
    InstancedExperiment(InstancedExperiment &&) = default;
    InstancedExperiment &operator=(InstancedExperiment &&) = default;
    InstancedExperiment(InstancedExperiment const &) = delete;
    InstancedExperiment &operator=(InstancedExperiment const &) = delete;
    ~InstancedExperiment() {
        // Release the top-level instances before their prototype scenes.
        if (scene && scene->scene) {
            rtcReleaseScene(scene->scene);
            scene->scene = nullptr;
        }
    }
};

class BatchedSpheres final : public Geometry {
public:
    struct Entry { Float3 center; float radius; Material *material; };
    std::vector<Entry> entries;

    unsigned int setupEmbree(RTCDevice &device, RTCScene &scene) override
    {
        type = RTC_GEOMETRY_TYPE_SPHERE_POINT;
        geom = rtcNewGeometry(device, type);
        float *vertices = static_cast<float *>(rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT4,
            4 * sizeof(float), entries.size()));
        for (size_t i = 0; i < entries.size(); ++i) {
            vertices[4 * i] = entries[i].center.x();
            vertices[4 * i + 1] = entries[i].center.y();
            vertices[4 * i + 2] = entries[i].center.z();
            vertices[4 * i + 3] = entries[i].radius;
        }
        vertex_count = static_cast<int>(entries.size());
        updateAABB();
        rtcCommitGeometry(geom);
        geomid = rtcAttachGeometry(scene, geom);
        return geomid;
    }
    void applyTransform() override {}
    Material *getMaterial(unsigned int primid, unsigned int) override
    {
        return primid < entries.size() ? entries[primid].material : nullptr;
    }
    void updateAABB() override
    {
        std::vector<Float3> bounds;
        bounds.reserve(entries.size() * 2);
        for (Entry const &entry : entries)
        {
            bounds.push_back(entry.center - Float3(entry.radius));
            bounds.push_back(entry.center + Float3(entry.radius));
        }
        bounding_box.update(bounds);
    }
};

struct BatchedExperiment {
    std::unique_ptr<SceneManager> scene;
    std::unique_ptr<BatchedSpheres> geometry;
    BatchedExperiment() = default;
    BatchedExperiment(BatchedExperiment &&) = default;
    BatchedExperiment &operator=(BatchedExperiment &&) = default;
    BatchedExperiment(BatchedExperiment const &) = delete;
    BatchedExperiment &operator=(BatchedExperiment const &) = delete;
    ~BatchedExperiment()
    {
        if (scene && scene->scene) {
            rtcReleaseScene(scene->scene);
            scene->scene = nullptr;
        }
        if (geometry && geometry->geom) rtcReleaseGeometry(geometry->geom);
        if (geometry) { delete geometry->transform; geometry->transform = nullptr; }
    }
};

BatchedExperiment MakeBatchedExperiment(RTCDevice &device,
                                        SceneManager const &source)
{
    BatchedExperiment result;
    result.scene = std::make_unique<SceneManager>(device);
    result.geometry = std::make_unique<BatchedSpheres>();
    const std::map<int, Geometry *> ordered(
        source.geometry.begin(), source.geometry.end());
    for (auto const &[sourceId, base] : ordered) {
        (void)sourceId;
        Sphere *sphere = dynamic_cast<Sphere *>(base);
        if (!sphere) throw std::runtime_error(
            "batch experiment currently requires an all-sphere scene");
        result.geometry->entries.push_back(
            {sphere->pos, sphere->r, sphere->material});
    }
    const unsigned id = result.geometry->setupEmbree(
        device, result.scene->scene);
    result.scene->geometry[id] = result.geometry.get();
    result.scene->commitScene();
    return result;
}

InstancedExperiment MakeInstancedExperiment(RTCDevice &device,
                                            SceneManager const &source)
{
    InstancedExperiment result;
    result.scene = std::make_unique<SceneManager>(device);
    const std::map<int, Geometry *> ordered(
        source.geometry.begin(), source.geometry.end());
    for (auto const &[sourceId, base] : ordered) {
        (void)sourceId;
        Sphere *sphere = dynamic_cast<Sphere *>(base);
        if (!sphere) throw std::runtime_error(
            "geometry experiment currently requires an all-sphere scene");
        auto &prototype = result.prototypes[sphere->r];
        if (!prototype)
            prototype = std::make_unique<SpherePrototype>(device, sphere->r);
        RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
        rtcSetGeometryInstancedScene(instance, prototype->scene);
        GfMatrix4d world(1.0);
        world.SetTranslate(GfVec3d(sphere->pos.x(), sphere->pos.y(), sphere->pos.z()));
        float matrix[16];
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                matrix[column * 4 + row] = float(world[column][row]);
        rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT4X4_COLUMN_MAJOR,
                                matrix);
        rtcCommitGeometry(instance);
        const unsigned id = rtcAttachGeometry(result.scene->scene, instance);
        rtcReleaseGeometry(instance);
        result.scene->geometry[id] = prototype->geometry.get();
        result.scene->intersection_geometry[id] = prototype->geometry->geom;
        SceneManager::PlacementTransform placement;
        placement.normal = {1,0,0, 0,1,0, 0,0,1};
        placement.tangent = placement.normal;
        result.scene->placement_transforms[id] = placement;
        result.scene->placement_materials[id].fallback = sphere->material;
    }
    result.scene->commitScene();
    return result;
}

struct Variant {
    std::string name;
    SceneManager *scene = nullptr;
    Environment *environment = nullptr;
};

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = Parse(argc, argv);
        std::filesystem::create_directories(options.output);
        Device device;
        if (!device.value) throw std::runtime_error("cannot create Embree device");

        const auto benchmarkStart = std::chrono::steady_clock::now();
        unsigned proceduralSeed = 0;
        InOneWeekendFinal procedural(device.value, proceduralSeed);
        const auto proceduralBuilt = std::chrono::steady_clock::now();
        SceneHandle proceduralRelease{&procedural, true};
        Environment *const originalGradient = procedural.environment;
        const std::filesystem::path usdPath =
            options.output / "InOneWeekend-benchmark.usda";
        procedural.exportToUSDA(usdPath);
        const auto stageExported = std::chrono::steady_clock::now();
        UsdStageRefPtr stage = UsdStage::Open(usdPath.string());
        if (!stage) throw std::runtime_error("cannot reopen benchmark export");
        const prism::CameraRayParameters camera = ReadCamera(
            stage, options.width, options.height);
        BakedEnvironment environment = ReadEnvironment(stage, options);
        const auto stageLoaded = std::chrono::steady_clock::now();
        procedural.environment = environment.environment.get();
        procedural.max_depth = 32;
        procedural.rr_min_depth = 4;
        procedural.commitScene();

        std::unique_ptr<mdl::Context> context;
        std::unique_ptr<USDMesh> imported;
        std::unique_ptr<SceneManager> usd;
        std::unique_ptr<SceneHandle> usdRelease;
        auto usdCompiled = stageLoaded;
        auto usdAttached = stageLoaded;
        if (!options.skipUsd) {
            const char *libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
            if (!libraries || !*libraries)
                throw std::runtime_error(
                    "PRISM_MATERIALX_LIBRARY_PATH is required");
            context = std::make_unique<mdl::Context>();
            if (!context->is_valid())
                throw std::runtime_error(context->get_error());
            imported = std::make_unique<USDMesh>(
                usdPath, context.get(), libraries,
                std::string(libraries) + "/mdl");
            usdCompiled = std::chrono::steady_clock::now();
            usd = std::make_unique<SceneManager>(device.value);
            usdRelease = std::make_unique<SceneHandle>();
            usdRelease->scene = usd.get();
            usd->attachImportedMesh(imported.get());
            usd->environment = environment.environment.get();
            usd->max_depth = 32;
            usd->rr_min_depth = 4;
            usd->commitScene();
            usdAttached = std::chrono::steady_clock::now();
        }

        auto elapsed = [](auto begin, auto end) {
            return std::chrono::duration<double>(end - begin).count();
        };
        std::cout << "setup tbbConcurrency=" << tbb::info::default_concurrency()
                  << " proceduralBuildSeconds="
                  << elapsed(benchmarkStart, proceduralBuilt)
                  << " exportSeconds=" << elapsed(proceduralBuilt, stageExported)
                  << " stageCameraEnvironmentLoadSeconds="
                  << elapsed(stageExported, stageLoaded)
                  << " usdMaterialGeometryCompileSeconds="
                  << elapsed(stageLoaded, usdCompiled)
                  << " usdAttachCommitSeconds="
                  << elapsed(usdCompiled, usdAttached)
                  << " residentHdr=" << options.residentHdr
                  << " textureSystem=" << options.textureSystem
                  << " skipUsd=" << options.skipUsd << '\n';
        const uint64_t sourceRgbBytes = uint64_t(environment.sourceWidth) *
            environment.sourceHeight * 3 * sizeof(float);
        std::cout << "environment sourceWidth=" << environment.sourceWidth
                  << " sourceHeight=" << environment.sourceHeight
                  << " sourceRgbFloatBytes=" << sourceRgbBytes
                  << " residentExtraBytes="
                  << (options.residentHdr ? sourceRgbBytes : 0)
                  << " textureSystemCacheBudgetMB="
                  << (options.textureSystem ? 128 : 0)
                  << " texturePreprocessSeconds="
                  << environment.texturePreprocessSeconds
                  << " probeRmsError=" << environment.probeRmsError
                  << " probeMaxError=" << environment.probeMaxError << '\n';
        std::cout.flush();

        std::optional<InstancedExperiment> instanced;
        std::optional<BatchedExperiment> batched;
        std::vector<Variant> variants{
            {"procedural-direct", &procedural, environment.environment.get()}};
        if (usd)
            variants.push_back(
                {"exported-usd", usd.get(), environment.environment.get()});
        variants.push_back(
            {"procedural-gradient", &procedural, originalGradient});
        if (options.geometryExperiment) {
            instanced.emplace(MakeInstancedExperiment(device.value, procedural));
            instanced->scene->environment = environment.environment.get();
            instanced->scene->max_depth = 32;
            instanced->scene->rr_min_depth = 4;
            variants.push_back({"procedural-instanced", instanced->scene.get(),
                                environment.environment.get()});
            batched.emplace(MakeBatchedExperiment(device.value, procedural));
            batched->scene->environment = environment.environment.get();
            batched->scene->max_depth = 32;
            batched->scene->rr_min_depth = 4;
            variants.push_back({"procedural-batched", batched->scene.get(),
                                environment.environment.get()});
        }

        for (bool mis : {true, false}) {
            const std::string mode = mis ? "mis" : "bsdf";
            std::map<std::string, ImageResult> images;
            for (Variant const &variant : variants) {
                variant.scene->environment = variant.environment;
                variant.scene->environmentSampling = mis;
                const ImageResult warmup = Render(
                    *variant.scene, camera, options.width,
                    options.height, options.samples);
                std::cout << "warmup variant=" << variant.name
                          << " mode=" << mode
                          << " seconds=" << warmup.seconds
                          << " checksum=0x" << std::hex << warmup.checksum
                          << std::dec << '\n';
                std::cout.flush();
                std::vector<double> timings;
                ImageResult last;
                std::optional<ImageResult> repeatReference;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    last = Render(*variant.scene, camera, options.width,
                                  options.height, options.samples);
                    if (!repeatReference) repeatReference = last;
                    else if (last.checksum != repeatReference->checksum ||
                             last.pixels != repeatReference->pixels)
                        throw std::runtime_error(
                            "timed repeats were not deterministic for " +
                            variant.name + " " + mode);
                    timings.push_back(last.seconds);
                }
                std::cout << "result variant=" << variant.name
                          << " mode=" << mode
                          << " medianSeconds=" << Median(timings)
                          << " repeats=" << timings[0] << ',' << timings[1]
                          << ',' << timings[2]
                          << " checksum=0x" << std::hex << last.checksum
                          << std::dec << '\n';
                std::cout.flush();
                WriteExr(options.output /
                    (variant.name + "-" + mode + ".exr"), last,
                    options.width, options.height);
                images.emplace(variant.name, std::move(last));

                const ImageResult unprofiled = Render(
                    *variant.scene, camera, options.width, options.height, 8);
                prism::profiling::Counters profile{};
                uint64_t workerNanoseconds = 0;
                const ImageResult profiled = Render(
                    *variant.scene, camera, options.width,
                    options.height, 8, &profile, &workerNanoseconds);
                if (profiled.pixels != unprofiled.pixels ||
                    profiled.checksum != unprofiled.checksum)
                    throw std::runtime_error(
                        "profiling instrumentation changed rendered pixels for " +
                        variant.name + " " + mode);
                std::cout << "profile variant=" << variant.name
                          << " mode=" << mode
                          << " summedWorkerElapsedMs="
                          << workerNanoseconds / 1.0e6 << '\n';
                for (size_t stageIndex = 0;
                     stageIndex < profile.calls.size(); ++stageIndex) {
                    std::cout << "  " << StageName(stageIndex)
                              << " calls=" << profile.calls[stageIndex]
                              << " summedScopeElapsedMs="
                              << profile.nanoseconds[stageIndex] / 1.0e6
                              << '\n';
                }
                std::cout.flush();
            }
            if (usd) {
                Compare(("procedural-vs-usd-" + mode).c_str(),
                        images.at("procedural-direct"),
                        images.at("exported-usd"));
                std::cout << "note: procedural-vs-USD compares handwritten "
                             "BSDFs with exported OpenPBR, so image equality "
                             "is not expected\n";
            }
            if (options.geometryExperiment)
                Compare(("direct-vs-instanced-" + mode).c_str(),
                        images.at("procedural-direct"),
                        images.at("procedural-instanced"));
            if (options.geometryExperiment)
                Compare(("direct-vs-batched-" + mode).c_str(),
                        images.at("procedural-direct"),
                        images.at("procedural-batched"));
            std::cout.flush();
        }
        if (auto *textureSystem = dynamic_cast<TextureSystemEnvironmentMap *>(
                environment.environment.get())) {
            std::cout << "textureSystemStatsBegin\n"
                      << textureSystem->Statistics()
                      << "\ntextureSystemStatsEnd\n";
            std::cout.flush();
        }
        if (environment.texture && environment.texture->service) {
            const auto serviceStats = environment.texture->service->stats();
            std::cout << "textureServiceStats prepareCalls=" << serviceStats.prepareCalls
                      << " memoryCacheHits=" << serviceStats.memoryCacheHits
                      << " diskCacheHits=" << serviceStats.diskCacheHits
                      << " sourceBytesHashed=" << serviceStats.sourceBytesHashed
                      << " preparationNanoseconds=" << serviceStats.preparationNanoseconds
                      << "\ntextureServiceOIIOStatsBegin\n"
                      << environment.texture->service->textureStatistics()
                      << "\ntextureServiceOIIOStatsEnd\n";
            std::cout.flush();
        }
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "render_cost_benchmark: " << error.what() << '\n';
        return 1;
    }
}
