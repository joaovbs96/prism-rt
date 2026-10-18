#pragma once

// Analytic lights of a scene (UsdLux sphere, rect, disk, cylinder and distant
// lights), in world space. Both frontends convert their lights with
// MakeSceneLight; the devices turn them into light-table records
// (src/device/light_table.h, shaders/integrator/lights.h).

#include "float3.h"
#include "texture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

namespace prism {

enum class LightShape { Sphere, Rect, Disk, Cylinder, Distant, Point };

struct SceneLight {
    LightShape shape = LightShape::Sphere;
    Float3 position = Float3(0.0f);
    // Rect: half extents along local X and Y. Disk: radius vectors along
    // local X and Y. Cylinder: half length along local X, radius along local Y.
    Float3 axisU = Float3(0.0f);
    Float3 axisV = Float3(0.0f);
    // Rect and disk: emitting side (local -Z). Distant: direction the light travels.
    Float3 normal = Float3(0.0f, 0.0f, -1.0f);
    float radius = 0.0f;          // sphere
    float cosHalfAngle = 1.0f;    // distant; 1: parallel
    float area = 0.0f;            // world-space surface area
    Color3 radiance = Color3(0.0f); // point light: intensity; parallel distant light: irradiance
    bool cameraVisible = false;
    std::shared_ptr<Texture<Color3>> texture; // rect lights
    uint32_t linkMask = ~0u;
};

// The UsdLux inputs of one light. `color` already includes any color
// temperature (UsdLuxBlackbodyTemperatureAsRgb).
struct LightParameters {
    LightShape shape = LightShape::Sphere;
    Color3 color = Color3(1.0f);
    float intensity = 1.0f;
    float exposure = 0.0f;
    bool normalize = false;
    float radius = 0.5f;
    float width = 1.0f;
    float height = 1.0f;
    float length = 1.0f;
    float angle = 0.53f;          // degrees, distant lights
    bool treatAsPoint = false;
    bool cameraVisible = false;
};

// localToWorld in GfMatrix4d layout: row-vector convention, m[row * 4 + column],
// translation in row 3.
inline SceneLight MakeSceneLight(LightParameters const& params, std::array<double, 16> const& m)
{
    auto point = [&](float x, float y, float z) {
        return Float3(float(x * m[0] + y * m[4] + z * m[8] + m[12]),
                      float(x * m[1] + y * m[5] + z * m[9] + m[13]),
                      float(x * m[2] + y * m[6] + z * m[10] + m[14]));
    };
    auto vector = [&](float x, float y, float z) {
        return Float3(float(x * m[0] + y * m[4] + z * m[8]),
                      float(x * m[1] + y * m[5] + z * m[9]),
                      float(x * m[2] + y * m[6] + z * m[10]));
    };
    constexpr float pi = 3.14159265358979323846f;
    SceneLight light;
    light.shape = params.shape;
    light.cameraVisible = params.cameraVisible;
    light.position = point(0.0f, 0.0f, 0.0f);
    const Float3 x = vector(1.0f, 0.0f, 0.0f), y = vector(0.0f, 1.0f, 0.0f), z = vector(0.0f, 0.0f, 1.0f);
    // Emission side of planar lights: the transformed -Z, kept perpendicular
    // to the (possibly sheared) plane.
    auto planeNormal = [&](Float3 const& u, Float3 const& v) {
        Float3 n = cross(v, u);
        if (n.length() <= 0.0f) return (-z).normalize();
        n = n.normalize();
        return dot(n, -z) < 0.0f ? -n : n;
    };
    switch (params.shape) {
    case LightShape::Sphere:
    case LightShape::Point: {
        // Uniform scale of the radius (the mean of the axis scales).
        const float scale = (x.length() + y.length() + z.length()) / 3.0f;
        light.radius = std::max(0.0f, params.radius) * scale;
        if (params.treatAsPoint || light.radius <= 0.0f) light.shape = LightShape::Point;
        light.area = 4.0f * pi * light.radius * light.radius;
        break;
    }
    case LightShape::Rect:
        light.axisU = x * (0.5f * params.width);
        light.axisV = y * (0.5f * params.height);
        light.normal = planeNormal(light.axisU, light.axisV);
        light.area = 4.0f * cross(light.axisU, light.axisV).length();
        break;
    case LightShape::Disk:
        light.axisU = x * params.radius;
        light.axisV = y * params.radius;
        light.normal = planeNormal(light.axisU, light.axisV);
        light.area = pi * cross(light.axisU, light.axisV).length();
        break;
    case LightShape::Cylinder: {
        light.axisU = x * (0.5f * params.length);
        const float radius = params.radius * 0.5f * (y.length() + z.length());
        Float3 radial = y - x.normalize() * dot(y, x.normalize());
        if (radial.length() <= 0.0f) radial = y;
        light.axisV = radial.normalize() * radius;
        light.area = 2.0f * pi * radius * 2.0f * light.axisU.length();
        break;
    }
    case LightShape::Distant: {
        light.normal = (-z).normalize();
        const float halfAngle = std::clamp(params.angle * pi / 360.0f, 0.0f, pi);
        light.cosHalfAngle = halfAngle > 0.0f ? std::cos(halfAngle) : 1.0f;
        break;
    }
    }

    // UsdLuxLightAPI: L = color * intensity * 2^exposure / sizeFactor.
    float scale = params.intensity * std::exp2(params.exposure);
    if (params.normalize) {
        if (light.shape == LightShape::Distant) {
            const float halfAngle = std::clamp(params.angle * pi / 360.0f, 0.0f, pi);
            const float s2 = std::sin(halfAngle) * std::sin(halfAngle);
            if (halfAngle > 0.0f) scale /= halfAngle <= pi / 2.0f ? s2 * pi : (2.0f - s2) * pi;
        } else if (light.shape == LightShape::Point) {
            // The limit of a normalized sphere light: intensity L * area / (4 pi) ... * pi.
            scale /= 4.0f;
        } else if (light.area > 0.0f) {
            scale /= light.area;
        }
    } else if (light.shape == LightShape::Point) {
        // A sphere light shrunk to a point keeps the intensity of its radius.
        scale *= pi * light.radius * light.radius;
    }
    light.radiance = params.color * scale;
    return light;
}

// Emitted power (watts in Prism's units), for light selection. Distant
// lights use the radius of the scene's bounds.
inline float LightPower(SceneLight const& light, float sceneRadius)
{
    constexpr float pi = 3.14159265358979323846f;
    const float luminance = std::max(0.0f, 0.2126f * light.radiance.x() + 0.7152f * light.radiance.y() +
                                     0.0722f * light.radiance.z());
    switch (light.shape) {
    case LightShape::Point: return 4.0f * pi * luminance;
    case LightShape::Distant: {
        const float disk = pi * sceneRadius * sceneRadius;
        if (light.cosHalfAngle >= 1.0f) return luminance * disk;
        return luminance * 2.0f * pi * (1.0f - light.cosHalfAngle) * disk;
    }
    default: return pi * luminance * light.area;
    }
}

} // namespace prism
