#pragma once

// Camera rays from the public CameraDesc (docs/sdk_design.md, 3.5 and 3.7):
// the projection of a USD camera (GfCamera::GetFrustum().
// ComputeProjectionMatrix()), conformed to the image's aspect ratio
// (CameraUtilConformedWindow) and framed for a pixel aspect ratio and a data
// window (CameraUtilFraming::ApplyToProjectionMatrix). Std-only; the
// arithmetic repeats Gf's and CameraUtil's step by step (float and double
// where they use them), so the rays are bit-identical to the ones the CLI
// built with Gf (prism_camera_projection_test compares them).

#include "../../include/prism/scene.h"
#include "../../include/prism/settings.h"
#include "../camera_rays.h"
#include "../matrix4d.h"

namespace prism::camera {

// The image a camera renders: a width x height display window, of which the
// data window (dataX, dataY, dataWidth x dataHeight, top row first) is
// rendered.
struct Framing {
    int width = 0, height = 0;
    int dataX = 0, dataY = 0, dataWidth = 0, dataHeight = 0;
    float pixelAspectRatio = 1.0f;
    ConformPolicy policy = ConformPolicy::ExpandAperture;

    bool FullDataWindow() const
    {
        return dataX == 0 && dataY == 0 && dataWidth == width && dataHeight == height;
    }
};

// The camera's transform: the sample nearest to time 0 (one instant renders
// until motion blur exists).
Matrix4d CameraToWorld(CameraDesc const& camera);

// GfCamera::GetFrustum().ComputeProjectionMatrix().
Matrix4d ProjectionMatrix(CameraDesc const& camera);

// CameraUtilConformedWindow(projection, policy, targetAspect).
Matrix4d ConformedWindow(Matrix4d const& projection, ConformPolicy policy, double targetAspect);

// CameraUtilFraming(display window, data window, pixel aspect ratio)
// .ApplyToProjectionMatrix(projection, policy).
Matrix4d ApplyFraming(Matrix4d const& projection, Framing const& framing);

// The ray parameters of `camera` for `framing`: the projection conformed to
// the image (a full data window of square pixels expanding the aperture), or
// framed (crops, other pixel aspect ratios or policies), as the CLI did.
CameraRayParameters RayParameters(CameraDesc const& camera, Framing const& framing);

} // namespace prism::camera
