#pragma once

#include "ray.h"
#include "sampling.h"

#include "matrix4d.h"

#include <algorithm>
#include <cmath>

namespace prism {

struct CameraRayParameters {
    prism::Matrix4d inverseProjection{1.0};
    prism::Matrix4d cameraToWorld{1.0};
    float nearClip = 0.001f;
    float farClip = 1.0e30f;
    double apertureRadius = 0.0;
    double focusDistance = 1.0;
    bool orthographic = false;
};

struct CameraRayPair {
    RTCRay sampled{};
    RTCRay center{};
    RayFootprint footprint{};
};

// Hydra camera adapters report focal length in world units. GfCamera keeps
// the authored USD value in tenths of a world unit, so callers using GfCamera
// must apply GfCamera::FOCAL_LENGTH_UNIT before calling this helper.
inline double CameraApertureRadius(double focalLengthWorld, double fStop)
{
    return fStop > 0.0 ? focalLengthWorld / (2.0 * fStop) : 0.0;
}

inline RTCRay MakeCameraRayWithLens(CameraRayParameters const &camera,
                                    float sx, float sy,
                                    unsigned int width, unsigned int height,
                                    Float3 const *lensPoint)
{
    const double nx = 2.0 * static_cast<double>(sx) /
        static_cast<double>(width) - 1.0;
    const double ny = 1.0 - 2.0 * static_cast<double>(sy) /
        static_cast<double>(height);
    prism::Vec4d p(nx, ny, -1.0, 1.0);
    p = p * camera.inverseProjection;
    if (std::abs(p[3]) > 1e-12) p /= p[3];

    prism::Vec3d originCamera(0.0);
    prism::Vec3d directionCamera;
    if (camera.orthographic) {
        originCamera = prism::Vec3d(p[0], p[1], 0.0);
        directionCamera = prism::Vec3d(0.0, 0.0, -1.0);
    } else {
        directionCamera = prism::Vec3d(p[0], p[1], p[2]).normalized();
    }

    if (lensPoint && !camera.orthographic && camera.apertureRadius > 0.0 &&
        camera.focusDistance > 0.0) {
        const double focusT = camera.focusDistance /
            std::max(1e-8, -directionCamera[2]);
        const prism::Vec3d focus = directionCamera * focusT;
        originCamera = prism::Vec3d(
            lensPoint->x() * camera.apertureRadius,
            lensPoint->y() * camera.apertureRadius, 0.0);
        directionCamera = (focus - originCamera).normalized();
    }

    const prism::Vec3d origin = camera.cameraToWorld.transform(originCamera);
    const prism::Vec3d direction = camera.cameraToWorld
        .transformDir(directionCamera).normalized();
    RTCRay ray = initialize_ray(
        Float3(static_cast<float>(origin[0]), static_cast<float>(origin[1]),
               static_cast<float>(origin[2])),
        Float3(static_cast<float>(direction[0]),
               static_cast<float>(direction[1]),
               static_cast<float>(direction[2])));
    const float dz = std::max(
        1e-8f, static_cast<float>(-directionCamera[2]));
    ray.tnear = std::max(0.0f, camera.nearClip) / dz;
    ray.tfar = camera.farClip / dz;
    return ray;
}

inline RTCRay MakeCameraRay(CameraRayParameters const &camera,
                            float sx, float sy,
                            unsigned int width, unsigned int height,
                            bool sampleLens, unsigned int &seed)
{
    Float3 disk;
    Float3 const *lens = nullptr;
    if (sampleLens && !camera.orthographic && camera.apertureRadius > 0.0 &&
        camera.focusDistance > 0.0) {
        disk = random_in_unit_disk(seed);
        lens = &disk;
    }
    return MakeCameraRayWithLens(camera, sx, sy, width, height, lens);
}

inline CameraRayPair GenerateCameraRays(CameraRayParameters const &camera,
                                        unsigned int x, unsigned int y,
                                        unsigned int width,
                                        unsigned int height,
                                        unsigned int &seed)
{
    const float sx = static_cast<float>(x) + rnd(seed);
    const float sy = static_cast<float>(y) + rnd(seed);
    Float3 lensPoint;
    Float3 const *lens = nullptr;
    if (!camera.orthographic && camera.apertureRadius > 0.0 &&
        camera.focusDistance > 0.0) {
        lensPoint = random_in_unit_disk(seed);
        lens = &lensPoint;
    }
    CameraRayPair result;
    result.sampled = MakeCameraRayWithLens(
        camera, sx, sy, width, height, lens);
    const RTCRay rx = MakeCameraRayWithLens(
        camera, sx + 1.0f, sy, width, height, lens);
    const RTCRay ry = MakeCameraRayWithLens(
        camera, sx, sy + 1.0f, width, height, lens);
    result.center = MakeCameraRayWithLens(
        camera, static_cast<float>(x) + 0.5f,
        static_cast<float>(y) + 0.5f, width, height, nullptr);
    auto origin = [](RTCRay const &r) { return Float3(r.org_x,r.org_y,r.org_z); };
    auto direction = [](RTCRay const &r) { return Float3(r.dir_x,r.dir_y,r.dir_z); };
    result.footprint.dOdx = origin(rx) - origin(result.sampled);
    result.footprint.dOdy = origin(ry) - origin(result.sampled);
    result.footprint.dDdx = direction(rx) - direction(result.sampled);
    result.footprint.dDdy = direction(ry) - direction(result.sampled);
    result.footprint.spread = std::max(
        result.footprint.dDdx.length(), result.footprint.dDdy.length());
    result.footprint.width = std::max(
        result.footprint.dOdx.length(), result.footprint.dOdy.length());
    result.footprint.exact = true;
    return result;
}

} // namespace prism
