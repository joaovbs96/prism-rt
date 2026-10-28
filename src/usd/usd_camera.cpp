// USD cameras as public CameraDescs (include/prism/usd.h).

#include <prism/usd.h>

#include "gf_convert.h"

#include <pxr/usd/usdGeom/camera.h>

#include <cmath>

namespace prism::usd {

CameraDesc CameraFromGf(pxr::GfCamera const& camera)
{
    CameraDesc desc;
    desc.cameraToWorld = Constant(ToMatrix4d(camera.GetTransform()));
    desc.projection = camera.GetProjection() == pxr::GfCamera::Orthographic ? CameraDesc::Projection::Orthographic
                                                                            : CameraDesc::Projection::Perspective;
    desc.focalLength = camera.GetFocalLength();
    desc.horizontalAperture = camera.GetHorizontalAperture();
    desc.verticalAperture = camera.GetVerticalAperture();
    desc.horizontalApertureOffset = camera.GetHorizontalApertureOffset();
    desc.verticalApertureOffset = camera.GetVerticalApertureOffset();
    const pxr::GfRange1f clipping = camera.GetClippingRange();
    desc.nearClip = clipping.GetMin();
    desc.farClip = clipping.GetMax();
    desc.fStop = camera.GetFStop();
    desc.focusDistance = camera.GetFocusDistance();
    return desc;
}

CameraDesc LoadCamera(pxr::UsdStageRefPtr const& stage, pxr::SdfPath const& path, pxr::UsdTimeCode time)
{
    const pxr::UsdGeomCamera authored(stage ? stage->GetPrimAtPath(path) : pxr::UsdPrim());
    if (!authored) throw Error(ErrorCode::NotFound, "Not a USD camera: " + path.GetString());
    CameraDesc desc = CameraFromGf(authored.GetCamera(time));
    const float scale = authored.ComputeLinearExposureScale(time);
    desc.exposureScale = std::isfinite(scale) ? scale : 1.0f;
    return desc;
}

} // namespace prism::usd
