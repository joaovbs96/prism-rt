#pragma once

#include "scene_bridge.h"
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace prism::hydra {

// A host that sets its camera from a view and projection matrix (usdview's
// free camera, UsdImagingGLEngine::SetCameraState) gets a GfCamera derived
// from them. With a huge far clip such as 1e30 the matrix's depth terms
// (-1 and -2 * near) no longer determine the far plane, and the derived
// clipping range comes back with a far of -inf. Hydra then builds a
// projection whose depth entries are NaN, which would also be a different
// camera on every frame. The far plane is then infinite for our purposes:
// repair the depth entries with the largest far plane the rays support.
// Only the depth terms depend on the clipping range, so the image plane
// stays as the host gave it.
inline void RepairUnboundedFarClip(CameraState &camera)
{
    if (std::isfinite(camera.farClip) && camera.farClip > camera.nearClip)
        return;
    constexpr float kUnboundedFar = 1.0e30f;
    camera.farClip = kUnboundedFar;
    double const n = camera.nearClip, f = kUnboundedFar;
    if (camera.orthographic) {
        camera.projection[2][2] = -2.0 / (f - n);
        camera.projection[3][2] = -(f + n) / (f - n);
    } else {
        camera.projection[2][2] = -(f + n) / (f - n);
        camera.projection[3][2] = -2.0 * f * n / (f - n);
    }
}

inline CameraState CameraFromPass(PXR_NS::HdRenderPassState const &state)
{
    CameraState camera;
    camera.worldToView = state.GetWorldToViewMatrix();
    // Stock Hydra already applies framing and window policy here. Applying
    // CameraUtilFraming again would crop and shift the camera twice.
    camera.projection = state.GetProjectionMatrix();
    if (auto const *source = state.GetCamera()) {
        camera.orthographic = source->GetProjection() == PXR_NS::HdCamera::Orthographic;
        camera.nearClip = std::max(0.0f, source->GetClippingRange().GetMin());
        camera.farClip = source->GetClippingRange().GetMax();
        camera.fStop = source->GetFStop();
        camera.focalLength = source->GetFocalLength();
        camera.focusDistance = source->GetFocusDistance();
        if (state.GetEnableExposureCompensation())
            camera.linearExposureScale = source->GetLinearExposureScale();
    }
    RepairUnboundedFarClip(camera);
    return camera;
}

// A framed projection maps the data window onto local buffer coordinates;
// its absolute origin is already represented in the projection matrix.
inline bool PassDimensions(PXR_NS::HdRenderPassState const &state,
    std::vector<PXR_NS::GfVec2i> const &bufferDimensions,
    unsigned int *width, unsigned int *height)
{
    auto const &framing = state.GetFraming();
    if (framing.IsValid()) {
        auto const size = framing.dataWindow.GetSize();
        if (size[0] <= 0 || size[1] <= 0) return false;
        for (auto const &dimensions : bufferDimensions)
            if (dimensions != size) return false;
        *width = static_cast<unsigned int>(size[0]);
        *height = static_cast<unsigned int>(size[1]);
        return true;
    }
    for (auto const &dimensions : bufferDimensions) {
        if (dimensions[0] > 0 && dimensions[1] > 0) {
            *width = static_cast<unsigned int>(dimensions[0]);
            *height = static_cast<unsigned int>(dimensions[1]);
            return true;
        }
    }
    auto const viewport = state.GetViewport();
    *width = static_cast<unsigned int>(std::max(0.0f, viewport[2]));
    *height = static_cast<unsigned int>(std::max(0.0f, viewport[3]));
    return true;
}

} // namespace prism::hydra
