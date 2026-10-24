#pragma once

// The light table of a scene (shaders/integrator/lights.h): its analytic
// lights, the triangles of constant-emission materials, and the dome, with a
// power-proportional alias table for next-event estimation. Built once per
// scene change by each device.

#include "hlsl_math.h"
#include "../../shaders/integrator/lights.h"
#include "../scene.h"
#include "../color/color_management.h"

#include <algorithm>
#include <bit>
#include <memory>
#include <unordered_map>
#include <vector>

namespace prism::device {

struct LightTable {
    std::vector<LightRecord> records;      // analytic lights first
    uint32_t analyticCount = 0;
    // records.size() + 1 entries (the last: the dome); empty without records.
    std::vector<float4> alias;
    std::vector<std::shared_ptr<Texture<Color3>>> textures; // LightRecord.info.z
    struct Emitter {
        float pdfArea = 0.0f;   // selection density per unit area
        bool twoSided = false;
    };
    std::unordered_map<const Material*, Emitter> emitters;
    size_t emitterTriangles = 0;
    bool emittersOverBudget = false;
    // Some light or dome has UsdLux diffuse or specular multipliers other
    // than 1 (the integrator's light lobes).
    bool lobes = false;
};

// Emissive triangles beyond this many are left to BSDF sampling.
constexpr size_t kMaxEmitterTriangles = size_t(1) << 22;

namespace light_table_detail {

inline float Luminance(Color3 const& c)
{
    const float l = prism::color::Active()->Luminance(c);
    return std::isfinite(l) ? std::max(0.0f, l) : 0.0f;
}

inline float3 F3(Float3 const& v) { return float3(v.x(), v.y(), v.z()); }

inline float4 F4(Float3 const& v, float w) { return float4(v.x(), v.y(), v.z(), w); }

// The materials a placement can resolve to (SceneManager::getMaterial).
template <class F>
void ForEachPlacementMaterial(SceneManager& scene, int id, Geometry* geometry, F const& visit)
{
    if (auto binding = scene.placement_materials.find(id); binding != scene.placement_materials.end()) {
        if (binding->second.fallback) visit(binding->second.fallback);
        for (auto const& [primitive, material] : binding->second.triangles) visit(material);
    }
    if (auto* mesh = dynamic_cast<Mesh*>(geometry)) {
        if (mesh->material_override) visit(mesh->material_override);
        for (auto const& [index, material] : mesh->materials) visit(material);
    } else if (auto* quad = dynamic_cast<Quad*>(geometry)) {
        visit(quad->material);
    } else if (auto* box = dynamic_cast<Box*>(geometry)) {
        visit(box->material);
    }
}

inline Material* PrimitiveMaterial(SceneManager& scene, int id, Geometry* geometry, unsigned primitive,
                                   bool instanced)
{
    if (auto binding = scene.placement_materials.find(id); binding != scene.placement_materials.end()) {
        auto triangle = binding->second.triangles.find(int(primitive));
        if (triangle != binding->second.triangles.end()) return triangle->second;
        if (binding->second.fallback) return binding->second.fallback;
    }
    try {
        return geometry->getMaterial(int(primitive), instanced ? unsigned(id) : RTC_INVALID_GEOMETRY_ID);
    } catch (std::exception const&) {
        return nullptr;
    }
}

} // namespace light_table_detail

// Whether some placement's material emits a constant radiance, so its own
// geometry lights the scene (textured emitters are not detected).
inline bool SceneHasEmitters(SceneManager& scene)
{
    for (auto const& [id, geometry] : scene.geometry) {
        if (!geometry || scene.disabled_placements.contains(id)) continue;
        bool emits = false;
        light_table_detail::ForEachPlacementMaterial(scene, id, geometry, [&](Material* material) {
            if (!material || emits) return;
            const auto emission = material->constantEmission();
            emits = emission && light_table_detail::Luminance(emission->radiance) > 0.0f;
        });
        if (emits) return true;
    }
    return false;
}

inline LightTable BuildLightTable(SceneManager& scene, size_t maxEmitterTriangles = kMaxEmitterTriangles)
{
    using namespace light_table_detail;
    constexpr float pi = 3.14159265358979323846f;
    LightTable table;

    float sceneRadius = 1.0f;
    if (scene.scene) {
        RTCBounds bounds{};
        rtcGetSceneBounds(scene.scene, &bounds);
        const Float3 extent(bounds.upper_x - bounds.lower_x, bounds.upper_y - bounds.lower_y,
                            bounds.upper_z - bounds.lower_z);
        const float r = 0.5f * extent.length();
        if (std::isfinite(r) && r > 0.0f) sceneRadius = r;
    }

    table.lobes = EnvironmentHasLobes(scene.environment);
    std::vector<double> weights;
    for (prism::SceneLight const& light : scene.lights) {
        if (!(Luminance(light.radiance) > 0.0f)) continue;
        table.lobes |= light.diffuse != 1.0f || light.specular != 1.0f;
        LightRecord record{};
        record.lobes = float4(light.diffuse, light.specular, 0.0f, 0.0f);
        record.info.y = (light.cameraVisible ? kLightCameraVisible : 0u);
        record.info.z = kLightTextureNone;
        record.info.w = light.lightLinkBit;
        record.radiance = F4(light.radiance,
            light.shadowLinkBit ? float(std::countr_zero(light.shadowLinkBit) + 1) : 0.0f);
        switch (light.shape) {
        case prism::LightShape::Sphere:
            record.info.x = kLightSphere;
            record.p0 = F4(light.position, light.radius);
            break;
        case prism::LightShape::Point:
            record.info.x = kLightPoint;
            record.p0 = F4(light.position, 0.0f);
            break;
        case prism::LightShape::Rect:
        case prism::LightShape::Disk:
            record.info.x = light.shape == prism::LightShape::Rect ? kLightRect : kLightDisk;
            record.p0 = F4(light.position, 0.0f);
            record.u = F4(light.axisU, 0.0f);
            record.v = F4(light.axisV, light.area);
            record.n = F4(light.normal, 0.0f);
            if (light.texture && light.shape == prism::LightShape::Rect) {
                record.info.y |= kLightTextured;
                record.info.z = uint32_t(table.textures.size());
                table.textures.push_back(light.texture);
            }
            break;
        case prism::LightShape::Cylinder:
            record.info.x = kLightCylinder;
            record.p0 = F4(light.position, 0.0f);
            record.u = F4(light.axisU, 0.0f);
            record.v = F4(light.axisV, light.area);
            break;
        case prism::LightShape::Distant:
            record.info.x = kLightDistant;
            record.u = float4(0.0f, 0.0f, 0.0f, light.cosHalfAngle);
            record.n = F4(light.normal.normalize(), 0.0f);
            break;
        }
        if (record.info.x != kLightDistant && record.info.x != kLightPoint && !(light.area > 0.0f)) continue;
        table.records.push_back(record);
        weights.push_back(std::max(1e-12, double(prism::LightPower(light, sceneRadius))));
    }
    table.analyticCount = uint32_t(table.records.size());

    // Constant-emission triangles, by placement id for a deterministic order.
    std::vector<int> placements;
    for (auto const& [id, geometry] : scene.geometry) {
        if (!geometry || scene.disabled_placements.contains(id)) continue;
        if (geometry->type != RTC_GEOMETRY_TYPE_TRIANGLE && geometry->type != RTC_GEOMETRY_TYPE_QUAD) continue;
        bool emits = false;
        ForEachPlacementMaterial(scene, id, geometry, [&](Material* material) {
            if (material && material->constantEmission()) emits = true;
        });
        if (emits) placements.push_back(id);
    }
    std::sort(placements.begin(), placements.end());

    struct EmitterTriangle {
        Float3 p[3];
        Material* material;
        ConstantEmission emission;
    };
    std::vector<EmitterTriangle> triangles;
    for (int id : placements) {
        Geometry* geometry = scene.geometry.at(id);
        const bool instanced = scene.intersection_geometry.contains(id);
        std::array<float, 12> transform{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (instanced && scene.scene)
            if (RTCGeometry embree = rtcGetGeometry(scene.scene, unsigned(id)))
                rtcGetGeometryTransform(embree, 0.0f, RTC_FORMAT_FLOAT3X4_ROW_MAJOR, transform.data());
        auto world = [&](float const* v) {
            return Float3(transform[0] * v[0] + transform[1] * v[1] + transform[2] * v[2] + transform[3],
                          transform[4] * v[0] + transform[5] * v[1] + transform[6] * v[2] + transform[7],
                          transform[8] * v[0] + transform[9] * v[1] + transform[10] * v[2] + transform[11]);
        };
        bool flip = false;
        if (auto placement = scene.placement_transforms.find(id); placement != scene.placement_transforms.end())
            flip = placement->second.flipOrientation;
        float const* vertices = geometry->geom ? static_cast<float const*>(
            rtcGetGeometryBufferData(geometry->geom, RTC_BUFFER_TYPE_VERTEX, 0)) : nullptr;
        auto add = [&](unsigned primitive, float const* a, float const* b, float const* c) {
            Material* material = PrimitiveMaterial(scene, id, geometry, primitive, instanced);
            if (!material) return;
            const auto emission = material->constantEmission();
            if (!emission) return;
            EmitterTriangle t{{world(a), world(b), world(c)}, material, *emission};
            if (flip) std::swap(t.p[1], t.p[2]);
            triangles.push_back(t);
        };
        if (geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE) {
            auto* mesh = dynamic_cast<Mesh*>(geometry);
            if (!mesh) continue;
            const size_t count = mesh->indices.size() / 3;
            std::vector<float> local;
            if (!vertices) {
                local.resize(mesh->vertices.size() * 3);
                for (size_t v = 0; v < mesh->vertices.size(); ++v) {
                    local[3 * v] = mesh->vertices[v].x();
                    local[3 * v + 1] = mesh->vertices[v].y();
                    local[3 * v + 2] = mesh->vertices[v].z();
                }
                vertices = local.data();
            }
            for (size_t i = 0; i < count; ++i) {
                const unsigned* idx = &mesh->indices[3 * i];
                add(unsigned(i), vertices + 3 * idx[0], vertices + 3 * idx[1], vertices + 3 * idx[2]);
            }
        } else if (vertices) {
            uint32_t const* quads = static_cast<uint32_t const*>(
                rtcGetGeometryBufferData(geometry->geom, RTC_BUFFER_TYPE_INDEX, 0));
            const uint32_t quadCount = dynamic_cast<Box*>(geometry) ? 6u : 1u;
            for (uint32_t q = 0; quads && q < quadCount; ++q) {
                uint32_t const* i = quads + 4 * q;
                add(q, vertices + 3 * i[0], vertices + 3 * i[1], vertices + 3 * i[3]);
                add(q, vertices + 3 * i[2], vertices + 3 * i[3], vertices + 3 * i[1]);
            }
        }
        if (triangles.size() > maxEmitterTriangles) break;
    }
    if (triangles.size() > maxEmitterTriangles) {
        table.emittersOverBudget = true;
        triangles.clear();
    }
    for (EmitterTriangle const& t : triangles) {
        const Float3 n = cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        const float area = 0.5f * n.length();
        const float luminance = Luminance(t.emission.radiance);
        if (!(area > 0.0f) || !(luminance > 0.0f)) continue;
        LightRecord record{};
        record.info.x = kLightTriangle;
        record.info.y = t.emission.twoSided ? kLightTwoSided : 0u;
        record.info.z = kLightTextureNone;
        record.info.w = ~0u;
        record.p0 = F4(t.p[0], 0.0f);
        record.u = F4(t.p[1], 0.0f);
        record.v = F4(t.p[2], area);
        record.n = F4(n.normalize(), 0.0f);
        record.radiance = F4(t.emission.radiance, 0.0f);
        record.lobes = float4(1.0f, 1.0f, 0.0f, 0.0f);
        table.records.push_back(record);
        weights.push_back(double(pi) * luminance * area * (t.emission.twoSided ? 2.0 : 1.0));
        table.emitters[t.material].twoSided = t.emission.twoSided;
    }
    table.emitterTriangles = table.records.size() - table.analyticCount;
    if (table.records.empty()) return table;

    // The dome competes by its power through a disk of the scene's size.
    double domeWeight = 0.0;
    if (scene.environment) {
        const float integral = scene.environment->integratedLuminance();
        if (std::isfinite(integral) && integral > 0.0f)
            domeWeight = double(integral) * pi * sceneRadius * sceneRadius;
    }
    weights.push_back(domeWeight);

    double total = 0.0;
    for (double w : weights) total += w;
    const size_t n = weights.size();
    std::vector<double> pmf(n);
    for (size_t i = 0; i < n; ++i) pmf[i] = total > 0.0 ? weights[i] / total : 1.0 / double(n);
    for (size_t i = 0; i < table.records.size(); ++i) table.records[i].n.w = float(pmf[i]);
    for (auto& [material, emitter] : table.emitters) {
        const auto emission = material->constantEmission();
        const double perArea = double(pi) * Luminance(emission->radiance) * (emitter.twoSided ? 2.0 : 1.0);
        emitter.pdfArea = total > 0.0 ? float(perArea / total) : 0.0f;
    }

    // Vose's alias method.
    table.alias.assign(n, float4(1.0f, 0.0f, 0.0f, 0.0f));
    std::vector<double> scaled(n);
    std::vector<uint32_t> smallBin, largeBin;
    for (size_t i = 0; i < n; ++i) {
        scaled[i] = pmf[i] * double(n);
        (scaled[i] < 1.0 ? smallBin : largeBin).push_back(uint32_t(i));
        table.alias[i].y = asfloat(uint32_t(i));
        table.alias[i].z = float(pmf[i]);
    }
    while (!smallBin.empty() && !largeBin.empty()) {
        const uint32_t s = smallBin.back();
        smallBin.pop_back();
        const uint32_t l = largeBin.back();
        largeBin.pop_back();
        table.alias[s].x = float(scaled[s]);
        table.alias[s].y = asfloat(l);
        scaled[l] -= 1.0 - scaled[s];
        (scaled[l] < 1.0 ? smallBin : largeBin).push_back(l);
    }
    for (uint32_t i : largeBin) table.alias[i].x = 1.0f;
    for (uint32_t i : smallBin) table.alias[i].x = 1.0f;
    return table;
}

} // namespace prism::device
