#pragma once

#include "scene_bridge.h"
#include <pxr/base/gf/range1f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <algorithm>
#include <vector>

namespace prism::hydra {

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
