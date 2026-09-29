#include "../src/camera_rays.h"

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

bool Check(bool condition, char const *message)
{
    if (!condition) std::cerr << "FAILED: " << message << '\n';
    return condition;
}

bool SameFloat(float a, float b)
{
    return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b);
}

bool SameRay(RTCRay const &a, RTCRay const &b)
{
    return SameFloat(a.org_x, b.org_x) && SameFloat(a.org_y, b.org_y) &&
        SameFloat(a.org_z, b.org_z) && SameFloat(a.dir_x, b.dir_x) &&
        SameFloat(a.dir_y, b.dir_y) && SameFloat(a.dir_z, b.dir_z) &&
        SameFloat(a.tnear, b.tnear) && SameFloat(a.tfar, b.tfar) &&
        SameFloat(a.time, b.time) && a.mask == b.mask && a.id == b.id &&
        a.flags == b.flags;
}

prism::CameraRayParameters Perspective(unsigned width, unsigned height,
                                        bool lens)
{
    GfFrustum frustum;
    frustum.SetPerspective(53.0, 1.0, 0.07, 321.0);
    const GfMatrix4d projection = CameraUtilConformedWindow(
        frustum.ComputeProjectionMatrix(), CameraUtilFit,
        static_cast<double>(width) / height);
    GfMatrix4d rotation(1.0), translation(1.0);
    rotation.SetRotate(GfRotation(GfVec3d(0.2, 1.0, -0.1), 37.0));
    translation.SetTranslate(GfVec3d(3.25, -1.5, 7.75));

    prism::CameraRayParameters result;
    result.inverseProjection = projection.GetInverse();
    result.cameraToWorld = rotation * translation;
    result.nearClip = 0.07;
    result.farClip = 321.0;
    result.apertureRadius = lens ? 0.035 : 0.0;
    result.focusDistance = 8.5;
    return result;
}

prism::CameraRayParameters Orthographic(unsigned width, unsigned height)
{
    GfFrustum frustum;
    frustum.SetOrthographic(-2.0, 2.0, -1.0, 1.0, 0.25, 90.0);
    const GfMatrix4d projection = CameraUtilConformedWindow(
        frustum.ComputeProjectionMatrix(), CameraUtilFit,
        static_cast<double>(width) / height);
    prism::CameraRayParameters result;
    result.inverseProjection = projection.GetInverse();
    result.cameraToWorld.SetRotate(
        GfRotation(GfVec3d(0.0, 1.0, 0.0), -23.0));
    result.cameraToWorld.SetTranslateOnly(GfVec3d(-4.0, 2.0, 6.0));
    result.nearClip = 0.25;
    result.farClip = 90.0;
    result.orthographic = true;
    return result;
}

bool Exercise(prism::CameraRayParameters const &first,
              prism::CameraRayParameters const &second,
              unsigned x, unsigned y, unsigned width, unsigned height,
              unsigned initialSeed, unsigned expectedDraws)
{
    unsigned aSeed = initialSeed;
    unsigned bSeed = initialSeed;
    const prism::CameraRayPair a = prism::GenerateCameraRays(
        first, x, y, width, height, aSeed);
    const prism::CameraRayPair b = prism::GenerateCameraRays(
        second, x, y, width, height, bSeed);
    bool ok = Check(SameRay(a.sampled, b.sampled) && SameRay(a.center, b.center),
                    "independently cached camera inputs produced different rays");
    ok &= Check(aSeed == bSeed, "camera adapters left different RNG states");

    unsigned expectedSeed = initialSeed;
    for (unsigned i = 0; i < expectedDraws; ++i) rnd(expectedSeed);
    ok &= Check(aSeed == expectedSeed,
                "camera ray generation consumed an unexpected RNG count");
    unsigned centerSeed = 0x12345678u;
    const unsigned savedCenterSeed = centerSeed;
    (void)prism::MakeCameraRay(first, static_cast<float>(x) + 0.5f,
        static_cast<float>(y) + 0.5f, width, height, false, centerSeed);
    ok &= Check(centerSeed == savedCenterSeed,
                "deterministic center ray consumed RNG state");
    ok &= Check(std::isfinite(a.sampled.tnear) &&
                    std::isfinite(a.sampled.tfar) &&
                    a.sampled.tnear >= 0.0f &&
                    a.sampled.tfar > a.sampled.tnear,
                "camera clipping interval is invalid");
    return ok;
}

} // namespace

int main()
{
    constexpr unsigned width = 853, height = 479;
    bool ok = true;
    constexpr double authoredFocalLength = 50.0;
    constexpr double fStop = 47.746048;
    const double standaloneRadius = prism::CameraApertureRadius(
        authoredFocalLength * GfCamera::FOCAL_LENGTH_UNIT, fStop);
    // UsdImaging's camera adapter applies FOCAL_LENGTH_UNIT before exposing
    // HdCamera::GetFocalLength(). Model that independently converted input.
    const double hydraFocalLength =
        authoredFocalLength * GfCamera::FOCAL_LENGTH_UNIT;
    const double hydraRadius =
        prism::CameraApertureRadius(hydraFocalLength, fStop);
    ok &= Check(standaloneRadius == hydraRadius &&
                    std::abs(standaloneRadius - 0.0523603545) < 1e-10,
                "GfCamera and preconverted HdCamera lens units diverged");
    for (bool lens : {false, true}) {
        const prism::CameraRayParameters first =
            Perspective(width, height, lens);
        // Model the two frontends caching the same independently ingested
        // inverse projection and camera-to-world convention.
        const prism::CameraRayParameters second =
            Perspective(width, height, lens);
        ok &= Exercise(first, second, 0, 0, width, height, 17u,
                       lens ? 4u : 2u);
        ok &= Exercise(first, second, width - 1, height - 1,
                       width, height, 0xf00dcafeu, lens ? 4u : 2u);
        ok &= Exercise(first, second, 347, 211, width, height,
                       0x10203040u, lens ? 4u : 2u);
    }
    const prism::CameraRayParameters orthoA = Orthographic(width, height);
    const prism::CameraRayParameters orthoB = Orthographic(width, height);
    ok &= Exercise(orthoA, orthoB, 421, 73, width, height,
                   0xabcdef01u, 2u);
    unsigned pinholeSeed = 99u, lensSeed = 99u;
    const prism::CameraRayPair pinhole = prism::GenerateCameraRays(
        Perspective(width, height, false), 347, 211,
        width, height, pinholeSeed);
    const prism::CameraRayPair lens = prism::GenerateCameraRays(
        Perspective(width, height, true), 347, 211,
        width, height, lensSeed);
    ok &= Check(SameRay(pinhole.center, lens.center),
                "lens sampling changed the deterministic center ray");
    ok &= Check(!SameFloat(pinhole.sampled.org_x, lens.sampled.org_x) ||
                    !SameFloat(pinhole.sampled.org_y, lens.sampled.org_y) ||
                    !SameFloat(pinhole.sampled.org_z, lens.sampled.org_z),
                "perspective lens sampling did not move the ray origin");
    unsigned orthoSeed = 7u;
    const prism::CameraRayPair ortho = prism::GenerateCameraRays(
        orthoA, 421, 73, width, height, orthoSeed);
    const GfVec3d expectedDirection = orthoA.cameraToWorld.TransformDir(
        GfVec3d(0, 0, -1)).GetNormalized();
    ok &= Check(std::abs(ortho.sampled.dir_x - expectedDirection[0]) < 1e-7 &&
                    std::abs(ortho.sampled.dir_y - expectedDirection[1]) < 1e-7 &&
                    std::abs(ortho.sampled.dir_z - expectedDirection[2]) < 1e-7,
                "orthographic camera orientation was not applied");
    return ok ? 0 : 1;
}
