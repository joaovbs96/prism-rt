// The core's camera rays from a public CameraDesc (src/camera/
// camera_projection.h) are bit-identical to the ones built with Gf and
// CameraUtil (GfCamera::GetFrustum().ComputeProjectionMatrix(),
// CameraUtilConformedWindow, CameraUtilFraming), as the CLI built them before
// the session API: random cameras (perspective and orthographic, offsets,
// clipping), image sizes, data windows, pixel aspect ratios and every
// conform policy.
#include <prism/usd.h>

#include "../src/camera/camera_projection.h"
#include "../src/usd/gf_convert.h"

#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/cameraUtil/framing.h>

#include <cstring>
#include <iostream>
#include <random>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Same(prism::Matrix4d const& a, GfMatrix4d const& b)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::memcmp(&a[size_t(r)][c], &b[r][c], sizeof(double)) != 0) return false;
    return true;
}

} // namespace

int main()
{
    std::mt19937 random(1234);
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(random); };
    auto integer = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(random); };
    const CameraUtilConformWindowPolicy gfPolicies[] = {CameraUtilFit, CameraUtilCrop, CameraUtilMatchVertically,
                                                        CameraUtilMatchHorizontally, CameraUtilDontConform};
    const prism::ConformPolicy policies[] = {prism::ConformPolicy::ExpandAperture, prism::ConformPolicy::CropAperture,
                                             prism::ConformPolicy::AdjustApertureWidth,
                                             prism::ConformPolicy::AdjustApertureHeight,
                                             prism::ConformPolicy::AdjustPixelAspectRatio};
    int failures = 0, cases = 0;
    for (int i = 0; i < 20000; ++i) {
        GfCamera camera;
        GfMatrix4d transform;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) transform[r][c] = uniform(-3, 3);
        camera.SetTransform(transform);
        camera.SetProjection(i % 5 == 0 ? GfCamera::Orthographic : GfCamera::Perspective);
        camera.SetFocalLength(float(uniform(5, 200)));
        camera.SetHorizontalAperture(float(uniform(5, 50)));
        camera.SetVerticalAperture(float(uniform(5, 50)));
        if (i % 3 == 0) {
            camera.SetHorizontalApertureOffset(float(uniform(-5, 5)));
            camera.SetVerticalApertureOffset(float(uniform(-5, 5)));
        }
        camera.SetClippingRange(GfRange1f(float(uniform(0.001, 1)), float(uniform(10, 1e6))));
        camera.SetFStop(i % 2 ? float(uniform(1, 16)) : 0.0f);
        camera.SetFocusDistance(float(uniform(0.5, 50)));
        const int width = integer(1, 2000), height = integer(1, 2000);
        const int dataX = i % 4 ? integer(0, width - 1) : 0, dataY = i % 4 ? integer(0, height - 1) : 0;
        const int dataWidth = i % 4 ? integer(1, width - dataX) : width;
        const int dataHeight = i % 4 ? integer(1, height - dataY) : height;
        const float pixelAspectRatio = i % 7 == 0 ? float(uniform(0.5, 2)) : 1.0f;
        const int policy = i % 5;

        // What the CLI computed with Gf (its UsdCameraRays before the session).
        const GfMatrix4d projection = camera.GetFrustum().ComputeProjectionMatrix();
        GfMatrix4d expected;
        const bool full = dataX == 0 && dataY == 0 && dataWidth == width && dataHeight == height;
        if (full && pixelAspectRatio == 1.0f && gfPolicies[policy] == CameraUtilFit) {
            expected = CameraUtilConformedWindow(projection, CameraUtilFit, double(width) / height).GetInverse();
        } else {
            const CameraUtilFraming framing(GfRange2f(GfVec2f(0.0f), GfVec2f(float(width), float(height))),
                                            GfRect2i(GfVec2i(dataX, dataY), dataWidth, dataHeight), pixelAspectRatio);
            expected = framing.ApplyToProjectionMatrix(projection, gfPolicies[policy]).GetInverse();
        }

        prism::camera::Framing framing;
        framing.width = width;
        framing.height = height;
        framing.dataX = dataX;
        framing.dataY = dataY;
        framing.dataWidth = dataWidth;
        framing.dataHeight = dataHeight;
        framing.pixelAspectRatio = pixelAspectRatio;
        framing.policy = policies[policy];
        const prism::CameraDesc desc = prism::usd::CameraFromGf(camera);
        const prism::CameraRayParameters rays = prism::camera::RayParameters(desc, framing);
        ++cases;
        bool ok = Same(prism::camera::ProjectionMatrix(desc), projection) && Same(rays.inverseProjection, expected) &&
                  Same(rays.cameraToWorld, camera.GetTransform()) &&
                  rays.nearClip == camera.GetClippingRange().GetMin() &&
                  rays.farClip == camera.GetClippingRange().GetMax() &&
                  rays.orthographic == (camera.GetProjection() == GfCamera::Orthographic) &&
                  rays.apertureRadius == prism::CameraApertureRadius(camera.GetFocalLength() * GfCamera::FOCAL_LENGTH_UNIT,
                                                                     camera.GetFStop()) &&
                  rays.focusDistance == double(camera.GetFocusDistance());
        if (!ok) {
            if (failures < 10)
                std::cerr << "FAILED: case " << i << " (" << width << "x" << height << ", data " << dataX << "," << dataY
                          << " " << dataWidth << "x" << dataHeight << ", par " << pixelAspectRatio << ", policy "
                          << policy << ")\n";
            ++failures;
        }
    }
    if (failures) {
        std::cerr << failures << " of " << cases << " cameras differ from Gf\n";
        return 1;
    }
    std::cout << "prism_camera_projection_test: " << cases << " cameras bit-identical to Gf\n";
    return 0;
}
