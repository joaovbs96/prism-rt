#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/plug/registry.h>
#include <OpenImageIO/imagebuf.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void render(UsdImagingGLEngine& engine, const UsdStageRefPtr& stage,
            UsdTimeCode frame = UsdTimeCode::Default()) {
    UsdImagingGLRenderParams params;
    params.frame = frame;
    params.enableSceneMaterials = true;
    params.enableSceneLights = true;
    const auto start = std::chrono::steady_clock::now();
    do {
        engine.Render(stage->GetPseudoRoot(), params);
        if (std::chrono::steady_clock::now()-start > std::chrono::minutes(5))
            throw std::runtime_error("Hydra failed to converge within five minutes");
        if (!engine.IsConverged()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!engine.IsConverged());
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
}

int main(int argc, char** argv) {
    try {
        if (argc < 4) {
            std::cerr << "usage: prism_hydra_render scene.usda /Camera output.exr [width height spp [--exercise] [--animation-exercise] [--environment-sampling mis|bsdf] [--sample-clamp N] [--seed N] [--sphere-batching]]\n";
            return 2;
        }
        const int width = argc > 4 ? std::stoi(argv[4]) : 128;
        const int height = argc > 5 ? std::stoi(argv[5]) : 128;
        const int spp = argc > 6 ? std::stoi(argv[6]) : 64;
        bool exercise = false;
        bool animationExercise = false;
        bool sphereBatching = false;
        std::string sampling = "mis";
        std::string filtering = "filtered";
        float sampleClamp = 10.0f;
        int seed = 0;
        for (int i = 7; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--exercise") { exercise = true; continue; }
            if (option == "--animation-exercise") { animationExercise = true; continue; }
            if (option == "--sphere-batching") { sphereBatching = true; continue; }
            if (++i >= argc) throw std::runtime_error("Missing option value: " + option);
            if (option == "--environment-sampling") sampling = argv[i];
            else if (option == "--texture-filtering") filtering = argv[i];
            else if (option == "--sample-clamp") sampleClamp = std::stof(argv[i]);
            else if (option == "--seed") seed = std::stoi(argv[i]);
            else throw std::runtime_error("Unknown option: " + option);
        }
        if ((sampling != "mis" && sampling != "bsdf") ||
            (filtering != "filtered" && filtering != "bilinear") ||
            !std::isfinite(sampleClamp) || sampleClamp < 0)
            throw std::runtime_error("Invalid sampling mode or sample clamp");
        if (width <= 0 || height <= 0 || spp <= 0)
            throw std::runtime_error("Dimensions and spp must be positive");
        auto stage = UsdStage::Open(argv[1]);
        if (!stage) throw std::runtime_error("Cannot open USD scene");
        if (!UsdGeomCamera(stage->GetPrimAtPath(SdfPath(argv[2]))))
            throw std::runtime_error("Camera path does not identify a USD camera");
        UsdImagingGLEngine::Parameters parameters;
        parameters.gpuEnabled = false;
        parameters.rendererPluginId = TfToken("HdPrismRendererPlugin");
        UsdImagingGLEngine engine(parameters);
        if (engine.GetCurrentRendererId() != parameters.rendererPluginId)
            throw std::runtime_error("PrismRT plugin not loaded; check PXR_PLUGINPATH_NAME and DLL paths");
        engine.SetCameraPath(SdfPath(argv[2]));
        engine.SetRenderViewport(GfVec4d(0, 0, width, height));
        engine.SetRendererSetting(TfToken("prism:samplesPerPixel"), VtValue(spp));
        engine.SetRendererSetting(TfToken("prism:maxDepth"), VtValue(32));
        engine.SetRendererSetting(TfToken("prism:rrMinDepth"), VtValue(4));
        engine.SetRendererSetting(TfToken("prism:maxSampleRadiance"), VtValue(sampleClamp));
        engine.SetRendererSetting(TfToken("prism:environmentSampling"), VtValue(TfToken(sampling)));
        engine.SetRendererSetting(TfToken("prism:textureFiltering"), VtValue(TfToken(filtering)));
        engine.SetRendererSetting(TfToken("prism:seed"), VtValue(seed));
        engine.SetRendererSetting(TfToken("prism:sphereBatching"), VtValue(sphereBatching));
        engine.SetRendererAovs({HdAovTokens->color, HdAovTokens->depth,
            HdAovTokens->cameraDepth, HdAovTokens->normal, HdAovTokens->Neye,
            HdAovTokens->primId, HdAovTokens->instanceId, HdAovTokens->elementId});
        engine.SetEnablePresentation(false);
        if (animationExercise) {
            runAnimationExercise(argv[1], argv[2], width, height, spp);
            std::cout << "Hydra animation exercise passed\n";
            return 0;
        }
        render(engine, stage);
        for (const auto& [name,value] : engine.GetRenderStats())
            std::cout << "Render stat " << name << ": " << value << '\n';
        // Some SDK/plugin shutdown paths terminate the process without
        // flushing fully buffered redirected stdout.  Publish the structured
        // statistics before image encoding so automated validators can rely
        // on them even in those runtimes.
        std::cout.flush();
        const auto initial = pixels(engine, width, height);
        std::cout << "Center normal: "
                  << center<GfVec3f>(engine, HdAovTokens->normal, HdFormatFloat32Vec3)
                  << "; camera depth: "
                  << center<float>(engine, HdAovTokens->cameraDepth, HdFormatFloat32) << '\n';
        OIIO::ImageBuf image(OIIO::ImageSpec(width, height, 4, OIIO::TypeDesc::FLOAT));
        image.set_pixels(OIIO::ROI(), OIIO::TypeDesc::FLOAT, initial.data());
        if (!image.write(argv[3])) throw std::runtime_error(image.geterror());
        if (exercise) {
            checkFixtureAovs(engine);
            // Same scene + seed must remain stable when asked to render again.
            render(engine, stage);
            if (pixels(engine, width, height) != initial)
                throw std::runtime_error("Converged image changed without a scene edit");
            auto panel = UsdGeomImageable(stage->GetPrimAtPath(SdfPath("/World/Panel")));
            if (!panel) throw std::runtime_error("Exercise mode requires parity fixture /World/Panel");
            panel.MakeInvisible();
            render(engine, stage);
            if (pixels(engine, width, height) == initial)
                throw std::runtime_error("Visibility edit did not invalidate rendering");
            panel.MakeVisible();
            render(engine, stage);
            if (pixels(engine, width, height) != initial)
                throw std::runtime_error("Restoring visibility did not reproduce deterministic rendering");
            engine.SetRenderViewport(GfVec4d(0,0,32,24));
            render(engine, stage);
            pixels(engine,32,24);
            stage->RemovePrim(SdfPath("/World/Panel"));
            render(engine,stage);
            pixels(engine,32,24);
        }
        std::cout << "Hydra render converged and saved " << argv[3] << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Hydra validation: " << e.what() << '\n';
        return 1;
    }
}
