#pragma once

#include "camera_rays.h"
#include "rng.h"

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace prism {

inline void DumpCameraRayDiagnostics(CameraRayParameters const &camera,
                                     unsigned int width,
                                     unsigned int height,
                                     unsigned int renderSeed,
                                     std::string_view frontend)
{
    if (!std::getenv("PRISM_DUMP_CAMERA_RAYS") || width == 0 || height == 0)
        return;

    std::cout << std::setprecision(17)
              << "[CameraRayDiagnostic] frontend=" << frontend
              << " width=" << width << " height=" << height
              << " near=" << camera.nearClip << " far=" << camera.farClip
              << " aperture=" << camera.apertureRadius
              << " focus=" << camera.focusDistance
              << " orthographic=" << camera.orthographic << '\n';
    for (int row = 0; row != 4; ++row) {
        std::cout << "[CameraRayDiagnostic] inverseProjection[" << row << "]=";
        for (int column = 0; column != 4; ++column)
            std::cout << (column ? "," : "") << camera.inverseProjection[row][column];
        std::cout << " cameraToWorld[" << row << "]=";
        for (int column = 0; column != 4; ++column)
            std::cout << (column ? "," : "") << camera.cameraToWorld[row][column];
        std::cout << '\n';
    }

    const unsigned int samples[][2] = {
        {0, 0}, {width / 2, height / 2}, {width - 1, height - 1},
        {width / 3, height / 3}, {2 * width / 3, 2 * height / 3}};
    for (auto const &xy : samples) {
        unsigned int seed = tea<64>((xy[1] * width) ^ renderSeed, xy[0]);
        const CameraRayPair pair = GenerateCameraRays(
            camera, xy[0], xy[1], width, height, seed);
        auto printRay = [](RTCRay const &ray) {
            std::cout << ray.org_x << ',' << ray.org_y << ',' << ray.org_z
                      << ';' << ray.dir_x << ',' << ray.dir_y << ',' << ray.dir_z
                      << ';' << ray.tnear << ',' << ray.tfar;
        };
        std::cout << "[CameraRayDiagnostic] pixel=" << xy[0] << ',' << xy[1]
                  << " sampled=";
        printRay(pair.sampled);
        std::cout << " center=";
        printRay(pair.center);
        std::cout << " seedAfter=" << seed << '\n';
    }
    std::cout.flush();
}

} // namespace prism
