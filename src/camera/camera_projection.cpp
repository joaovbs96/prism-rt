#include "camera_projection.h"

#include <cmath>
#include <limits>

namespace prism::camera {
namespace {

// GfCamera's units: apertures in tenths of a scene unit, focal length too.
constexpr double kApertureUnit = 0.1;
constexpr double kFocalLengthUnit = 0.1;

// CameraUtil's resolved policies.
enum class Resolved { MatchVertically, MatchHorizontally };

double SafeDivOne(double a, double b) { return b != 0.0 ? a / b : 1.0; }
double SafeDiv(double a, double b) { return b != 0.0 ? a / b : a; }
double Sign(double x) { return x < 0.0 ? -1.0 : 1.0; }

// _ResolveConformWindowPolicy (cameraUtil/conformWindow.cpp).
Resolved Resolve(double width, double height, ConformPolicy policy, double targetAspect)
{
    if (policy == ConformPolicy::AdjustApertureWidth) return Resolved::MatchVertically;
    if (policy == ConformPolicy::AdjustApertureHeight) return Resolved::MatchHorizontally;
    const double aspect = SafeDivOne(width, height);
    if ((policy == ConformPolicy::ExpandAperture) ^ (aspect > targetAspect)) return Resolved::MatchVertically;
    return Resolved::MatchHorizontally;
}

// A float pair with GfVec2f's arithmetic (float components; scaling by a
// double rounds the double product to float).
struct Vec2f {
    float x = 0.0f, y = 0.0f;
};
Vec2f Scaled(Vec2f v, double s)
{
    v.x = float(double(v.x) * s);
    v.y = float(double(v.y) * s);
    return v;
}
Vec2f operator+(Vec2f a, Vec2f b) { return {a.x + b.x, a.y + b.y}; }
Vec2f operator-(Vec2f a, Vec2f b) { return {a.x - b.x, a.y - b.y}; }

Matrix4d Diagonal(double x, double y)
{
    Matrix4d m(0.0);
    m[0][0] = x;
    m[1][1] = y;
    m[2][2] = 1.0;
    m[3][3] = 1.0;
    return m;
}

} // namespace

Matrix4d CameraToWorld(CameraDesc const& camera)
{
    if (camera.cameraToWorld.empty()) return Matrix4d();
    auto const* nearest = &camera.cameraToWorld.front();
    for (auto const& sample : camera.cameraToWorld)
        if (std::abs(sample.time) < std::abs(nearest->time)) nearest = &sample;
    return nearest->value;
}

Matrix4d ProjectionMatrix(CameraDesc const& camera)
{
    // GfCamera::GetFrustum: the window in millimetres (float halves of the
    // float apertures), offset, in scene units, over the focal length.
    const double halfWidth = double(camera.horizontalAperture / 2);
    const double halfHeight = double(camera.verticalAperture / 2);
    double minX = -halfWidth, minY = -halfHeight, maxX = halfWidth, maxY = halfHeight;
    const double offsetX = double(camera.horizontalApertureOffset);
    const double offsetY = double(camera.verticalApertureOffset);
    minX += offsetX; minY += offsetY;
    maxX += offsetX; maxY += offsetY;
    // GfRange2d::operator*=(double): a negative factor swaps the corners.
    auto scale = [&](double m) {
        if (m > 0) {
            minX *= m; minY *= m; maxX *= m; maxY *= m;
        } else {
            const double x = minX, y = minY;
            minX = maxX * m; minY = maxY * m;
            maxX = x * m; maxY = y * m;
        }
    };
    scale(kApertureUnit);
    const bool orthographic = camera.projection == CameraDesc::Projection::Orthographic;
    if (!orthographic && camera.focalLength != 0) scale(1.0 / (double(camera.focalLength) * kFocalLengthUnit));
    const double n = double(camera.nearClip);
    const double f = double(camera.farClip);

    // GfFrustum::ComputeProjectionMatrix.
    Matrix4d matrix;
    const double l = minX, r = maxX, b = minY, t = maxY;
    const double rl = r - l;
    const double tb = t - b;
    const double fn = f - n;
    if (orthographic) {
        matrix[0][0] = 2.0 / rl;
        matrix[1][1] = 2.0 / tb;
        matrix[2][2] = -2.0 / fn;
        matrix[3][0] = -(r + l) / rl;
        matrix[3][1] = -(t + b) / tb;
        matrix[3][2] = -(f + n) / fn;
    } else {
        matrix[0][0] = 2.0 / rl;
        matrix[1][1] = 2.0 / tb;
        matrix[2][2] = -(f + n) / fn;
        matrix[2][0] = (r + l) / rl;
        matrix[2][1] = (t + b) / tb;
        matrix[3][2] = -2.0 * n * f / fn;
        matrix[2][3] = -1.0;
        matrix[3][3] = 0.0;
    }
    return matrix;
}

Matrix4d ConformedWindow(Matrix4d const& projection, ConformPolicy policy, double targetAspect)
{
    if (policy == ConformPolicy::AdjustPixelAspectRatio) return projection;
    Matrix4d result = projection;
    const double windowX = std::fabs(projection[1][1]);
    const double windowY = std::fabs(projection[0][0]);
    if (Resolve(windowX, windowY, policy, targetAspect) == Resolved::MatchHorizontally) {
        result[1][1] = Sign(projection[1][1]) * windowY * targetAspect;
        const double scaleFactor = SafeDiv(result[1][1], projection[1][1]);
        result[2][1] *= scaleFactor;
        result[3][1] *= scaleFactor;
    } else {
        result[0][0] = Sign(projection[0][0]) * SafeDiv(windowX, targetAspect);
        const double scaleFactor = SafeDiv(result[0][0], projection[0][0]);
        result[2][0] *= scaleFactor;
        result[3][0] *= scaleFactor;
    }
    return result;
}

Matrix4d ApplyFraming(Matrix4d const& projection, Framing const& framing)
{
    // The display window is GfRange2f((0, 0), (width, height)); the data
    // window a GfRect2i. Sizes and centres are GfVec2f.
    const Vec2f dispMin{0.0f, 0.0f};
    const Vec2f dispSize = Vec2f{float(framing.width), float(framing.height)} - dispMin;
    const Vec2f dataMin{float(framing.dataX), float(framing.dataY)};
    const Vec2f dataSize{float(framing.dataWidth), float(framing.dataHeight)};
    const double aspect = framing.pixelAspectRatio * SafeDivOne(double(dispSize.x), double(dispSize.y));
    const Vec2f dispCenter = dispMin + Scaled(dispSize, 0.5);
    const Vec2f dataCenter = dataMin + Scaled(dataSize, 0.5);
    const Vec2f t = Scaled(dispCenter - dataCenter, double(2.0f));
    Matrix4d translate;
    translate[3][0] = double(t.x);
    translate[3][1] = double(-t.y);
    translate[3][2] = 0.0;
    return ConformedWindow(projection, framing.policy, aspect) * Diagonal(double(dispSize.x), double(dispSize.y)) *
           translate * Diagonal(1.0 / double(dataSize.x), 1.0 / double(dataSize.y));
}

CameraRayParameters RayParameters(CameraDesc const& camera, Framing const& framing)
{
    CameraRayParameters parameters;
    const Matrix4d projection = ProjectionMatrix(camera);
    if (framing.FullDataWindow() && framing.pixelAspectRatio == 1.0f &&
        framing.policy == ConformPolicy::ExpandAperture) {
        parameters.inverseProjection =
            ConformedWindow(projection, ConformPolicy::ExpandAperture, double(framing.width) / framing.height).inverse();
    } else {
        parameters.inverseProjection = ApplyFraming(projection, framing).inverse();
    }
    parameters.cameraToWorld = CameraToWorld(camera);
    parameters.nearClip = camera.nearClip;
    parameters.farClip = camera.farClip;
    parameters.orthographic = camera.projection == CameraDesc::Projection::Orthographic;
    parameters.apertureRadius = CameraApertureRadius(double(camera.focalLength) * kFocalLengthUnit, camera.fStop);
    parameters.focusDistance = camera.focusDistance;
    return parameters;
}

} // namespace prism::camera
