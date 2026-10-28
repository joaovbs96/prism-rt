#include "../../src/hydra/pass_camera.h"
#include "../../src/camera_rays.h"
#include "../../src/usd/gf_convert.h"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/usdGeom/camera.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
class TestCamera : public HdCamera {
public:
    TestCamera() : HdCamera(SdfPath("/Camera")) {
        _transform.SetIdentity();
        _projection = Perspective;
        _horizontalAperture = 0.02f;
        _verticalAperture = 0.02f;
        _focalLength = 0.035f;
        _clippingRange = GfRange1f(0.1f, 100.0f);
        _linearExposureScale = 4.0f;
    }
};
bool Check(bool value, char const *message) {
    if (!value) std::cerr << message << '\n';
    return value;
}
prism::CameraRayParameters Rays(prism::hydra::CameraState const &camera) {
    prism::CameraRayParameters parameters;
    parameters.cameraToWorld = prism::usd::ToMatrix4d(camera.worldToView.GetInverse());
    parameters.inverseProjection = prism::usd::ToMatrix4d(camera.projection.GetInverse());
    parameters.nearClip = camera.nearClip;
    parameters.farClip = camera.farClip;
    return parameters;
}

void Render(UsdImagingGLEngine &engine, UsdStageRefPtr const &stage) {
    UsdImagingGLRenderParams params;
    params.enableSceneMaterials = true;
    params.enableSceneLights = true;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    do {
        engine.Render(stage->GetPseudoRoot(), params);
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Framing integration render timed out");
        if (!engine.IsConverged()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (!engine.IsConverged());
}
std::vector<float> Pixels(UsdImagingGLEngine &engine, TfToken const &name,
                         unsigned width, unsigned height, size_t components) {
    auto *buffer = engine.GetAovRenderBuffer(name);
    if (!buffer || buffer->GetWidth() != width || buffer->GetHeight() != height)
        throw std::runtime_error("Framed AOV has incorrect dimensions");
    auto const *pixels = static_cast<float const *>(buffer->Map());
    std::vector<float> result(pixels, pixels + width * height * components);
    buffer->Unmap();
    return result;
}

// A host-supplied (free) camera must keep accumulating while it is unchanged,
// whatever its far plane: the camera counts as changed only when the host's
// matrices change. "projection" is the host's projection matrix.
bool FreeCameraKeepsAccumulating(UsdStageRefPtr const &stage, GfMatrix4d const &view,
                                 GfMatrix4d const &projection, char const *label) {
    UsdImagingGLEngine::Parameters setup;
    setup.gpuEnabled = false;
    setup.rendererPluginId = TfToken("HdPrismRendererPlugin");
    UsdImagingGLEngine engine(setup);
    engine.SetRendererSetting(TfToken("prism:samplesPerPixel"), VtValue(8));
    engine.SetRendererSetting(TfToken("prism:adaptiveThreshold"), VtValue(0.0f));
    engine.SetRendererSetting(TfToken("prism:seed"), VtValue(0));
    engine.SetRendererAovs({HdAovTokens->color});
    engine.SetRenderBufferSize(GfVec2i(48, 32));
    engine.SetCameraState(view, projection);
    UsdImagingGLRenderParams params;
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int64_t firstRevision = -1, lastRevision = -1;
    do {
        engine.Render(stage->GetPseudoRoot(), params);
        auto const stats = engine.GetRenderStats();
        auto const found = stats.find("requestedCameraRevision");
        if (found != stats.end() && found->second.IsHolding<int64_t>()) {
            lastRevision = found->second.UncheckedGet<int64_t>();
            if (firstRevision < 0) firstRevision = lastRevision;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            std::cerr << label << ": free camera did not converge (camera revision "
                      << lastRevision << ")\n";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (!engine.IsConverged());
    if (lastRevision > firstRevision + 1) {
        std::cerr << label << ": an unchanged free camera restarted accumulation (revision "
                  << firstRevision << " -> " << lastRevision << ")\n";
        return false;
    }
    auto const color = Pixels(engine, HdAovTokens->color, 48, 32, 4);
    for (float value : color)
        if (!std::isfinite(value)) {
            std::cerr << label << ": non-finite pixel with this free camera\n";
            return false;
        }
    return true;
}
}

int main(int argc, char **argv) {
    TestCamera camera;
    HdRenderPassState state;
    state.SetCamera(&camera);
    state.SetOverrideWindowPolicy(CameraUtilFit);
    state.SetFraming(CameraUtilFraming(
        GfRange2f(GfVec2f(0,0), GfVec2f(96,64)),
        GfRect2i(GfVec2i(0,0), GfVec2i(95,63)), 1.0f));
    auto full = prism::hydra::CameraFromPass(state);
    state.SetFraming(CameraUtilFraming(
        GfRange2f(GfVec2f(0,0), GfVec2f(96,64)),
        GfRect2i(GfVec2i(32,8), GfVec2i(63,39)), 1.0f));
    auto cropped = prism::hydra::CameraFromPass(state);
    unsigned width = 0, height = 0;
    bool ok = Check(prism::hydra::PassDimensions(state, {GfVec2i(32,32)},
        &width, &height) && width == 32 && height == 32,
        "Data-window dimensions were not used");
    ok &= Check(!prism::hydra::PassDimensions(state, {GfVec2i(96,64)},
        &width, &height), "Display-sized AOV accepted for cropped data window");
    ok &= Check(!prism::hydra::PassDimensions(state,
        {GfVec2i(32,32), GfVec2i(16,16)}, &width, &height),
        "Mismatched secondary AOV accepted");
    for (unsigned y : {0u, 15u, 31u}) {
        for (unsigned x : {0u, 15u, 31u}) {
            unsigned seedFull = 0, seedCrop = 0;
            auto a = prism::GenerateCameraRays(Rays(full), x + 32, y + 8,
                96, 64, seedFull).center;
            auto b = prism::GenerateCameraRays(Rays(cropped), x, y,
                32, 32, seedCrop).center;
            const Float3 difference(a.dir_x-b.dir_x, a.dir_y-b.dir_y, a.dir_z-b.dir_z);
            ok &= Check(difference.length() < 1e-6f,
                "Offset crop ray differs from corresponding display pixel");
        }
    }
    state.SetEnableExposureCompensation(false);
    ok &= Check(prism::hydra::CameraFromPass(state).linearExposureScale == 1.0f,
        "Disabled exposure compensation affected RGB scale");
    state.SetEnableExposureCompensation(true);
    ok &= Check(prism::hydra::CameraFromPass(state).linearExposureScale == 4.0f,
        "Camera linear exposure scale was not forwarded");
    try {
        if (argc != 2) throw std::runtime_error("Expected parity fixture path");
        auto stage = UsdStage::Open(argv[1]);
        if (!stage) throw std::runtime_error("Cannot open framing fixture");
        UsdImagingGLEngine::Parameters setup;
        setup.gpuEnabled = false;
        setup.rendererPluginId = TfToken("HdPrismRendererPlugin");
        UsdImagingGLEngine engine(setup);
        engine.SetCameraPath(SdfPath("/World/Camera"));
        engine.SetRendererSetting(TfToken("prism:samplesPerPixel"), VtValue(4));
        engine.SetRendererSetting(TfToken("prism:seed"), VtValue(0));
        engine.SetRendererAovs({HdAovTokens->color, HdAovTokens->normal});
        auto const display = GfRange2f(GfVec2f(0,0), GfVec2f(96,64));
        engine.SetFraming(CameraUtilFraming(display,
            GfRect2i(GfVec2i(0,0), GfVec2i(95,63)), 1.0f));
        engine.SetRenderBufferSize(GfVec2i(96,64));
        Render(engine, stage);
        auto const fullNormals = Pixels(engine, HdAovTokens->normal, 96,64,3);
        engine.SetFraming(CameraUtilFraming(display,
            GfRect2i(GfVec2i(32,8), GfVec2i(63,39)), 1.0f));
        engine.SetRenderBufferSize(GfVec2i(32,32));
        Render(engine, stage);
        auto const croppedNormals = Pixels(engine, HdAovTokens->normal,32,32,3);
        bool cropMatches = true;
        for (size_t y=0; y<32; ++y)
            for (size_t x=0; x<32; ++x)
                for (size_t component=0; component<3; ++component)
                    cropMatches &= croppedNormals[(y*32+x)*3+component] ==
                        fullNormals[((y+24)*96+x+32)*3+component];
        ok &= Check(cropMatches,
            "Cropped fixture AOV differs from display window region");
        auto const unexposed = Pixels(engine,HdAovTokens->color,32,32,4);
        // Keep the framing and camera unchanged while presenting an invalid
        // allocation, then return to the previous valid dimensions.
        engine.SetRenderBufferSize(GfVec2i(96,64));
        UsdImagingGLRenderParams mismatchedParams;
        engine.Render(stage->GetPseudoRoot(), mismatchedParams);
        ok &= Check(!engine.IsConverged(), "Invalid framing was reported complete");
        engine.SetRenderBufferSize(GfVec2i(32,32));
        Render(engine,stage);
        ok &= Check(Pixels(engine,HdAovTokens->color,32,32,4) == unexposed,
            "Returning to valid dimensions failed to recover the original image");
        UsdGeomCamera(stage->GetPrimAtPath(SdfPath("/World/Camera")))
            .CreateExposureAttr(VtValue(2.0f));
        Render(engine,stage);
        auto const exposed = Pixels(engine,HdAovTokens->color,32,32,4);
        bool exposureMatches = true, alphaMatches = true;
        for (size_t pixel=0; pixel<32*32; ++pixel) {
            for (size_t component=0; component<3; ++component)
                exposureMatches &= exposed[pixel*4+component] ==
                    4.0f*unexposed[pixel*4+component];
            alphaMatches &= exposed[pixel*4+3] == unexposed[pixel*4+3];
        }
        ok &= Check(exposureMatches, "Camera exposure did not scale rendered RGB by four");
        ok &= Check(alphaMatches, "Camera exposure changed alpha");
        {
            GfFrustum frustum = UsdGeomCamera(stage->GetPrimAtPath(SdfPath("/World/Camera")))
                .GetCamera(UsdTimeCode::Default()).GetFrustum();
            auto const view = frustum.ComputeViewMatrix();
            frustum.SetNearFar(GfRange1d(0.001, 1.0e30));
            ok &= Check(FreeCameraKeepsAccumulating(stage, view,
                frustum.ComputeProjectionMatrix(), "far clip 1e30"),
                "Free camera with a far clip of 1e30 failed");
            // The limit of an infinite far plane (Gf cannot express it).
            GfMatrix4d infinite = frustum.ComputeProjectionMatrix();
            infinite[2][2] = -1.0;
            infinite[3][2] = -2.0 * 0.001;
            ok &= Check(FreeCameraKeepsAccumulating(stage, view, infinite,
                "infinite far clip"), "Free camera with an infinite far plane failed");
        }
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return ok ? 0 : 1;
}
