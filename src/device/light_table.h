#pragma once

// The light table of a scene (shaders/integrator/lights.h): its analytic
// lights, the triangles of emissive materials, and the dome, with a
// power-proportional alias table for next-event estimation. Built once per
// scene change by each device.
//
// Triangles of constant emission (Material::constantEmission) are selected by
// their power. Those of varying emission (Material::variesEmission: textures,
// primvars, cutouts, directional EDFs) by an estimate of it: their emission
// is evaluated through the material at stratified points (the CPU MDL code),
// and each triangle's average radiance times its area, with a floor of a
// fraction of its material's mean, is its weight. The light sample evaluates
// the true emission at the sampled point, so the estimate only shapes the
// noise; BSDF hits weight these triangles by their own density
// (LightTable::varyingEmitters).

#include "hlsl_math.h"
#include "../../shaders/integrator/lights.h"
#include "../scene.h"
#include "../color/color_management.h"

#include "../load_profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace prism::device {

struct LightTable {
    std::vector<LightRecord> records;      // analytic lights first
    uint32_t analyticCount = 0;
    // records.size() + 1 entries (the last: the dome); empty without records.
    // After them, the shaping blocks of kLightShaped lights and their IES
    // grids (LightShaping in shaders/integrator/lights.h).
    std::vector<float4> alias;
    std::vector<std::shared_ptr<Texture<Color3>>> textures; // LightRecord.info.z
    struct Emitter {
        float pdfArea = 0.0f;   // selection density per unit area
        bool twoSided = false;
    };
    std::unordered_map<const Material*, Emitter> emitters;
    size_t emitterTriangles = 0;
    bool emittersOverBudget = false;
    // Triangles of varying emission, by placement: the selection density per
    // unit area of each primitive, negative when only its front face emits,
    // 0 for primitives next-event estimation does not sample.
    std::unordered_map<int, std::vector<float>> varyingEmitters;
    size_t varyingEmitterTriangles = 0;   // their records (part of emitterTriangles)
    size_t emissionEvaluations = 0;       // emission evaluations of the estimate
    double emissionEstimateSeconds = 0.0;
    bool varyingEmittersOverBudget = false;
    // Some light or dome has UsdLux diffuse or specular multipliers other
    // than 1 (the integrator's light lobes).
    bool lobes = false;
};

// Emissive triangles beyond this many are left to BSDF sampling.
constexpr size_t kMaxEmitterTriangles = size_t(1) << 22;
// Triangles of varying emission beyond this many are left to BSDF sampling
// (a record is 112 bytes). PRISM_MAX_VARYING_EMITTER_TRIANGLES lowers the
// limit for tests of what counts against it.
inline size_t MaxVaryingEmitterTriangles()
{
    static const size_t limit = [] {
        size_t value = size_t(1) << 20;
        if (const char* text = std::getenv("PRISM_MAX_VARYING_EMITTER_TRIANGLES"); text && *text)
            if (const unsigned long long parsed = std::strtoull(text, nullptr, 10); parsed > 0)
                value = size_t(parsed);
        return value;
    }();
    return limit;
}
// Emission evaluations of the estimate: k x k stratified points per triangle,
// k from 4 down to 1 as the triangle count grows.
constexpr size_t kEmissionEvaluationBudget = size_t(1) << 22;
// A triangle whose estimate is not zero weighs at least this fraction of its
// material's mean radiance on that placement (dark-on-average texels, and
// bright ones the estimate missed, stay reachable).
constexpr float kVaryingEmitterFloor = 0.1f;
// Material groups whose emission is zero at the centres of this many of their
// triangles (spread over the group) are taken as not emitting.
constexpr size_t kEmissionProbeTriangles = 64;

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

// An emissive triangle of a placement: its world-space vertices in the
// light record's order (the second and third swapped on placements that flip
// orientation, so the record's normal is the front normal of the hits) and
// the geometric normal Embree reports for it (in the space of its vertex
// buffer, before the placement's transform).
struct EmitterTriangle {
    int placement = 0;
    unsigned primitive = 0;
    Material* material = nullptr;
    Float3 p[3];
    Float3 objectNormal;
    bool swapped = false;
    bool instanced = false;
};

// Visits the triangles whose material `emits` accepts, by placement id for a
// deterministic order. `visit` returns false to stop.
template <class Accept, class Visit>
void ForEachEmitterTriangle(SceneManager& scene, Accept const& emits, Visit const& visit)
{
    std::vector<int> placements;
    for (auto const& [id, geometry] : scene.geometry) {
        if (!geometry || scene.disabled_placements.contains(id)) continue;
        if (geometry->type != RTC_GEOMETRY_TYPE_TRIANGLE) continue;
        bool any = false;
        ForEachPlacementMaterial(scene, id, geometry, [&](Material* material) {
            if (material && emits(material)) any = true;
        });
        if (any) placements.push_back(id);
    }
    std::sort(placements.begin(), placements.end());
    for (int id : placements) {
        Geometry* geometry = scene.geometry.at(id);
        auto* mesh = dynamic_cast<Mesh*>(geometry);
        if (!mesh) continue;
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
        const size_t count = mesh->indices.size() / 3;
        for (size_t i = 0; i < count; ++i) {
            Material* material = PrimitiveMaterial(scene, id, geometry, unsigned(i), instanced);
            if (!material || !emits(material)) continue;
            const unsigned* idx = &mesh->indices[3 * i];
            float const* a = vertices + 3 * idx[0];
            float const* b = vertices + 3 * idx[1];
            float const* c = vertices + 3 * idx[2];
            EmitterTriangle t;
            t.placement = id;
            t.primitive = unsigned(i);
            t.material = material;
            t.p[0] = world(a);
            t.p[1] = world(b);
            t.p[2] = world(c);
            if (flip) std::swap(t.p[1], t.p[2]);
            t.swapped = flip;
            t.instanced = instanced;
            const Float3 va(a[0], a[1], a[2]);
            t.objectNormal = cross(Float3(b[0], b[1], b[2]) - va, Float3(c[0], c[1], c[2]) - va);
            if (!visit(t)) return;
        }
    }
}

// The hit of a ray from `origin` along `direction` (with `footprint`) that
// reaches the point of `triangle` with Embree barycentrics (u, v) at
// distance t: the hit SceneManager::prepareHit makes of that intersection,
// after Material::perturbNormal. An empty footprint looks textures up at
// their finest level.
inline RayHit EmitterHit(SceneManager& scene, EmitterTriangle const& triangle, float u, float v,
                         Float3 const& origin, Float3 const& direction, float t,
                         prism::RayFootprint const& footprint = {})
{
    RTCRayHit query{};
    query.ray.org_x = origin.x();
    query.ray.org_y = origin.y();
    query.ray.org_z = origin.z();
    query.ray.dir_x = direction.x();
    query.ray.dir_y = direction.y();
    query.ray.dir_z = direction.z();
    query.ray.tnear = 0.0f;
    query.ray.tfar = t;
    query.ray.mask = unsigned(-1);
    query.hit.Ng_x = triangle.objectNormal.x();
    query.hit.Ng_y = triangle.objectNormal.y();
    query.hit.Ng_z = triangle.objectNormal.z();
    query.hit.u = u;
    query.hit.v = v;
    query.hit.primID = triangle.primitive;
    for (auto& id : query.hit.instID) id = RTC_INVALID_GEOMETRY_ID;
    if (triangle.instanced) {
        query.hit.geomID = 0u;
        query.hit.instID[0] = unsigned(triangle.placement);
    } else {
        query.hit.geomID = unsigned(triangle.placement);
    }
    prism::PathRay ray;
    ray.ray = query.ray;
    ray.footprint = footprint;
    RayHit hit = scene.prepareHit(query, ray);
    triangle.material->perturbNormal(hit);
    return hit;
}

// Embree barycentrics of the point with weights (b1, b2) of the record's
// second and third vertex.
inline void EmbreeBarycentrics(bool swapped, float b1, float b2, float* u, float* v)
{
    *u = swapped ? b2 : b1;
    *v = swapped ? b1 : b2;
}

inline Color3 NonNegative(Color3 const& c)
{
    return Color3(std::isfinite(c.x()) ? std::max(0.0f, c.x()) : 0.0f,
                  std::isfinite(c.y()) ? std::max(0.0f, c.y()) : 0.0f,
                  std::isfinite(c.z()) ? std::max(0.0f, c.z()) : 0.0f);
}

// The radiance a triangle of varying emission sends along its normal from
// the point with record weights (b1, b2), from its front or back face.
inline Color3 EmittedAlongNormal(SceneManager& scene, EmitterTriangle const& triangle, float b1, float b2,
                                 bool back)
{
    const Float3 e1 = triangle.p[1] - triangle.p[0];
    const Float3 e2 = triangle.p[2] - triangle.p[0];
    const Float3 n = cross(e1, e2);
    const float length = n.length();
    if (!(length > 0.0f) || !std::isfinite(length)) return Color3(0.0f);
    const Float3 q = triangle.p[0] + b1 * e1 + b2 * e2;
    const Float3 direction = back ? n / length : -(n / length);
    float u, v;
    EmbreeBarycentrics(triangle.swapped, b1, b2, &u, &v);
    RayHit hit = EmitterHit(scene, triangle, u, v, q - direction, direction, 1.0f);
    return NonNegative(triangle.material->emittedRadiance(hit));
}

// The triangles of varying emission grouped by placement, then material.
struct VaryingEmitters {
    std::vector<EmitterTriangle> triangles;
    std::vector<size_t> groups;   // first triangle of each group; one more entry: the end
};

// False: more than `limit` triangles.
inline bool CollectVaryingEmitters(SceneManager& scene, size_t limit, VaryingEmitters* result)
{
    std::vector<EmitterTriangle> triangles;
    bool overBudget = false;
    ForEachEmitterTriangle(scene, [](Material* material) { return material->variesEmission(); },
                           [&](EmitterTriangle const& t) {
                               if (triangles.size() >= limit) {
                                   overBudget = true;
                                   return false;
                               }
                               triangles.push_back(t);
                               return true;
                           });
    result->triangles.clear();
    result->groups.clear();
    if (overBudget) return false;
    size_t start = 0;
    while (start < triangles.size()) {
        size_t end = start;
        while (end < triangles.size() && triangles[end].placement == triangles[start].placement) ++end;
        // The placement's materials in first-use order.
        std::vector<Material*> order;
        for (size_t i = start; i < end; ++i)
            if (std::find(order.begin(), order.end(), triangles[i].material) == order.end())
                order.push_back(triangles[i].material);
        for (Material* material : order) {
            result->groups.push_back(result->triangles.size());
            for (size_t i = start; i < end; ++i)
                if (triangles[i].material == material) result->triangles.push_back(triangles[i]);
        }
        start = end;
    }
    result->groups.push_back(result->triangles.size());
    return true;
}

// Whether a group's emission is nonzero at the centre of one of up to
// kEmissionProbeTriangles of its triangles, spread over it.
inline bool GroupEmits(SceneManager& scene, EmitterTriangle const* triangles, size_t count, size_t* evaluations)
{
    if (count == 0) return false;
    const size_t step = std::max<size_t>(1, count / kEmissionProbeTriangles);
    const bool backFace = triangles[0].material->mayEmitBackFace();
    for (size_t i = 0; i < count; i += step) {
        for (int side = 0; side < (backFace ? 2 : 1); ++side) {
            ++*evaluations;
            if (Luminance(EmittedAlongNormal(scene, triangles[i], 1.0f / 3.0f, 1.0f / 3.0f, side == 1)) > 0.0f)
                return true;
        }
    }
    return false;
}

} // namespace light_table_detail

// Whether some placement's material emits, so its own geometry lights the
// scene: a constant radiance, or varying emission that is not zero at the
// centres of some of its triangles (light_table_detail::GroupEmits).
inline bool SceneHasEmitters(SceneManager& scene)
{
    using namespace light_table_detail;
    bool varying = false;
    for (auto const& [id, geometry] : scene.geometry) {
        if (!geometry || scene.disabled_placements.contains(id)) continue;
        bool emits = false;
        ForEachPlacementMaterial(scene, id, geometry, [&](Material* material) {
            if (!material || emits) return;
            const auto emission = material->constantEmission();
            emits = emission && Luminance(emission->radiance) > 0.0f;
            varying = varying || (geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE && material->variesEmission());
        });
        if (emits) return true;
    }
    if (!varying) return false;
    VaryingEmitters emitters;
    if (!CollectVaryingEmitters(scene, MaxVaryingEmitterTriangles(), &emitters)) return true;
    size_t evaluations = 0;
    for (size_t g = 0; g + 1 < emitters.groups.size(); ++g)
        if (GroupEmits(scene, emitters.triangles.data() + emitters.groups[g],
                       emitters.groups[g + 1] - emitters.groups[g], &evaluations))
            return true;
    return false;
}

namespace light_table_detail {

// Estimates the weights of the triangles of varying emission and appends
// their records to the table (BuildLightTable), and their selection weights
// (pi times the estimated power) to `weights`.
inline void AddVaryingEmitters(SceneManager& scene, LightTable& table, std::vector<double>& weights)
{
    constexpr float pi = 3.14159265358979323846f;
    bool any = false;
    for (auto const& [id, geometry] : scene.geometry) {
        if (!geometry || scene.disabled_placements.contains(id) || geometry->type != RTC_GEOMETRY_TYPE_TRIANGLE)
            continue;
        ForEachPlacementMaterial(scene, id, geometry, [&](Material* material) {
            any = any || (material && material->variesEmission());
        });
        if (any) break;
    }
    if (!any) return;
    prism::load_profile::Scope profile("light table: varying emitters");
    const auto start = std::chrono::steady_clock::now();
    VaryingEmitters emitters;
    if (!CollectVaryingEmitters(scene, MaxVaryingEmitterTriangles(), &emitters)) {
        table.varyingEmittersOverBudget = true;
        std::cerr << std::format("PrismRT: more than {} triangles have materials whose emission may vary; "
                                 "they are not sampled as lights\n", MaxVaryingEmitterTriangles());
        return;
    }
    // Groups that emit at all.
    const size_t groups = emitters.groups.size() - 1;
    std::vector<char> emitting(groups, 0);
    size_t probes = 0;
    std::vector<size_t> work;
    for (size_t g = 0; g < groups; ++g) {
        const size_t count = emitters.groups[g + 1] - emitters.groups[g];
        emitting[g] = GroupEmits(scene, emitters.triangles.data() + emitters.groups[g], count, &probes) ? 1 : 0;
        if (emitting[g])
            for (size_t i = emitters.groups[g]; i < emitters.groups[g + 1]; ++i) work.push_back(i);
    }
    table.emissionEvaluations += probes;
    if (!work.empty()) {
        size_t faces = 0;
        for (size_t i : work) faces += emitters.triangles[i].material->mayEmitBackFace() ? 2 : 1;
        const int strata = int(std::clamp(
            std::floor(std::sqrt(double(kEmissionEvaluationBudget) / double(faces))), 1.0, 4.0));
        // Per triangle: the mean luminance over its emitting faces (summed
        // over both faces where the back face may emit), and its area.
        std::vector<float> mean(emitters.triangles.size(), 0.0f);
        std::vector<float> area(emitters.triangles.size(), 0.0f);
        std::atomic<size_t> evaluations{0};
        tbb::parallel_for(tbb::blocked_range<size_t>(0, work.size(), 16),
                          [&](tbb::blocked_range<size_t> const& range) {
            size_t local = 0;
            for (size_t w = range.begin(); w != range.end(); ++w) {
                const size_t i = work[w];
                EmitterTriangle const& t = emitters.triangles[i];
                const float a = 0.5f * cross(t.p[1] - t.p[0], t.p[2] - t.p[0]).length();
                if (!(a > 0.0f) || !std::isfinite(a)) continue;
                area[i] = a;
                const bool back = t.material->mayEmitBackFace();
                double sum = 0.0;
                for (int sy = 0; sy < strata; ++sy) {
                    for (int sx = 0; sx < strata; ++sx) {
                        // The light sample's uniform warp (sampleLight, kLightTriangle).
                        const float ux = (float(sx) + 0.5f) / float(strata);
                        const float uy = (float(sy) + 0.5f) / float(strata);
                        const float su = std::sqrt(ux);
                        const float b1 = uy * su;
                        const float b2 = su - b1;
                        for (int side = 0; side < (back ? 2 : 1); ++side) {
                            sum += Luminance(EmittedAlongNormal(scene, t, b1, b2, side == 1));
                            ++local;
                        }
                    }
                }
                mean[i] = float(sum / double(strata * strata));
            }
            evaluations += local;
        });
        table.emissionEvaluations += evaluations.load();

        static const bool verbose = [] {
            const char* value = std::getenv("PRISM_VERBOSE_LIGHTS");
            return value && *value && std::string_view(value) != "0";
        }();
        for (size_t g = 0; g < groups; ++g) {
            if (!emitting[g]) continue;
            double power = 0.0;
            double groupArea = 0.0;
            for (size_t i = emitters.groups[g]; i < emitters.groups[g + 1]; ++i) {
                power += double(mean[i]) * area[i];
                groupArea += area[i];
            }
            const double floor = groupArea > 0.0 ? kVaryingEmitterFloor * power / groupArea : 0.0;
            if (verbose) {
                EmitterTriangle const& first = emitters.triangles[emitters.groups[g]];
                std::cerr << std::format("PrismRT lights: placement {} material {} triangles {} area {:.4g} "
                                         "mean luminance {:.4g} back face {}\n", first.placement,
                                         static_cast<const void*>(first.material),
                                         emitters.groups[g + 1] - emitters.groups[g], groupArea,
                                         groupArea > 0.0 ? power / groupArea : 0.0,
                                         first.material->mayEmitBackFace());
            }
            for (size_t i = emitters.groups[g]; i < emitters.groups[g + 1]; ++i) {
                EmitterTriangle const& t = emitters.triangles[i];
                if (!(mean[i] > 0.0f) || !(area[i] > 0.0f)) continue;
                const bool twoSided = t.material->mayEmitBackFace();
                const Float3 n = cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
                LightRecord record{};
                record.info.x = kLightTriangle;
                record.info.y = kLightVaryingEmission | (twoSided ? kLightTwoSided : 0u) |
                                (t.swapped ? kLightSwappedVertices : 0u);
                record.info.z = t.primitive;
                record.info.w = ~0u;
                record.p0 = F4(t.p[0], asfloat(uint32_t(t.placement)));
                record.u = F4(t.p[1], 0.0f);
                record.v = F4(t.p[2], area[i]);
                record.n = F4(n.normalize(), 0.0f);
                record.radiance = float4(0.0f, 0.0f, 0.0f, 0.0f);
                record.lobes = float4(1.0f, 1.0f, 0.0f, 0.0f);
                table.records.push_back(record);
                weights.push_back(double(pi) * std::max(double(mean[i]), floor) * double(area[i]));
                ++table.varyingEmitterTriangles;
            }
        }
        profile.detail(std::format("triangles={} records={} points={} evaluations={}", work.size(),
                                   table.varyingEmitterTriangles, strata * strata, table.emissionEvaluations));
    }
    table.emissionEstimateSeconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

} // namespace light_table_detail

// UsdLuxShapingAPI: the fraction of an analytic light's power its shaping
// leaves (the luminance of the cone, focus and IES factors over the
// directions it emits in, weighted as the shape emits: uniformly for spheres,
// points and cylinders, by the cosine to the emitting side for rect and disk
// lights), for its selection weight. 1 without shaping.
inline LightShaping SceneLightShaping(prism::SceneLight const& light)
{
    constexpr float pi = 3.14159265358979323846f;
    LightShapingParameters const& p = light.shaping;
    LightShaping s{};
    s.axis = light_table_detail::F3(light.shapingAxis);
    s.xAxis = light_table_detail::F3(light.shapingX);
    s.focus = std::max(0.0f, p.focus);
    s.tint = light_table_detail::F3(p.focusTint);
    const float cutoff = std::clamp(p.coneAngle, 0.0f, 180.0f) * pi / 180.0f;
    s.cutoff = p.coneAngle >= 180.0f ? 4.0f : cutoff;
    s.smoothStart = cutoff * (1.0f - std::clamp(p.coneSoftness, 0.0f, 1.0f));
    s.angleScale = p.iesAngleScale;
    s.iesScale = 0.0f;
    if (p.ies) {
        s.iesScale = p.ies->peak;
        if (p.iesNormalize) s.iesScale = p.ies->mean > 0.0f ? p.ies->peak / p.ies->mean : 0.0f;
        s.profileSize = uint(p.ies->width) | (uint(p.ies->height) << 16u);
    }
    return s;
}

inline double ShapingPowerFraction(prism::SceneLight const& light)
{
    if (!light.shaping.active || light.shape == prism::LightShape::Distant) return 1.0;
    const LightShaping s = SceneLightShaping(light);
    const bool planar = light.shape == prism::LightShape::Rect || light.shape == prism::LightShape::Disk;
    const float3 normal = light_table_detail::F3(light.normal);
    const float3 yAxis = cross(s.xAxis, s.axis);
    const float3 zAxis = -s.axis;
    constexpr int kTheta = 256, kPhi = 128;
    double shaped = 0.0, total = 0.0;
    for (int t = 0; t < kTheta; ++t) {
        // Uniform in cos(theta) about the local +Z: equal solid angles.
        const float cosTheta = 1.0f - 2.0f * (float(t) + 0.5f) / float(kTheta);
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        for (int f = 0; f < kPhi; ++f) {
            const float phi = 2.0f * 3.14159265358979f * (float(f) + 0.5f) / float(kPhi);
            const float3 e = s.xAxis * (sinTheta * std::cos(phi)) + yAxis * (sinTheta * std::sin(phi)) + zAxis * cosTheta;
            const float weight = planar ? std::max(0.0f, dot(e, normal)) : 1.0f;
            if (!(weight > 0.0f)) continue;
            float3 factor = shapingConeFocus(s, e);
            if (light.shaping.ies) {
                const float2 angles = iesAngles(s, e);
                factor = factor * (light.shaping.ies->sample(angles.x, angles.y) * s.iesScale);
            }
            shaped += double(weight) * light_table_detail::Luminance(Color3(factor.x, factor.y, factor.z));
            total += double(weight) * light_table_detail::Luminance(Color3(1.0f));
        }
    }
    return total > 0.0 ? shaped / total : 0.0;
}

// Vose's alias method over the selection probabilities `pmf`: x the
// probability of keeping the column, y its alias (as float bits), z the
// entry's probability (PathTracer::pickLight).
inline std::vector<float4> BuildAliasTable(std::vector<double> const& pmf)
{
    const size_t n = pmf.size();
    std::vector<float4> alias(n, float4(1.0f, 0.0f, 0.0f, 0.0f));
    std::vector<double> scaled(n);
    std::vector<uint32_t> smallBin, largeBin;
    for (size_t i = 0; i < n; ++i) {
        scaled[i] = pmf[i] * double(n);
        (scaled[i] < 1.0 ? smallBin : largeBin).push_back(uint32_t(i));
        alias[i].y = asfloat(uint32_t(i));
        alias[i].z = float(pmf[i]);
    }
    while (!smallBin.empty() && !largeBin.empty()) {
        const uint32_t s = smallBin.back();
        smallBin.pop_back();
        const uint32_t l = largeBin.back();
        largeBin.pop_back();
        alias[s].x = float(scaled[s]);
        alias[s].y = asfloat(l);
        scaled[l] -= 1.0 - scaled[s];
        (scaled[l] < 1.0 ? smallBin : largeBin).push_back(l);
    }
    for (uint32_t i : largeBin) alias[i].x = 1.0f;
    for (uint32_t i : smallBin) alias[i].x = 1.0f;
    return alias;
}

// The shaping blocks of the shaped lights, then their IES grids (shared
// between lights), after the alias entries (LightShaping).
inline void AppendShapingBlocks(LightTable& table, std::vector<prism::SceneLight const*> const& shaped)
{
    if (shaped.empty()) return;
    const size_t base = table.alias.size();
    table.alias.resize(base + 4 * shaped.size());
    std::vector<std::pair<prism::IesProfile const*, uint32_t>> profiles; // grid offsets from base
    for (size_t k = 0; k < shaped.size(); ++k) {
        prism::SceneLight const& light = *shaped[k];
        const LightShaping s = SceneLightShaping(light);
        uint32_t profile = 0u;
        if (light.shaping.ies) {
            auto found = std::find_if(profiles.begin(), profiles.end(),
                                      [&](auto const& p) { return p.first == light.shaping.ies.get(); });
            if (found != profiles.end()) {
                profile = found->second;
            } else {
                profile = uint32_t(table.alias.size() - base);
                profiles.emplace_back(light.shaping.ies.get(), profile);
                // The grid, then the maximum of each row.
                prism::IesProfile const& ies = *light.shaping.ies;
                std::vector<float> values = ies.grid;
                for (int j = 0; j < ies.height; ++j)
                    values.push_back(*std::max_element(ies.grid.begin() + size_t(j) * size_t(ies.width),
                                                       ies.grid.begin() + size_t(j + 1) * size_t(ies.width)));
                const size_t first = table.alias.size();
                table.alias.resize(first + (values.size() + 3) / 4, float4(0.0f));
                for (size_t i = 0; i < values.size(); ++i) {
                    float4& q = table.alias[first + i / 4];
                    (i % 4 == 0 ? q.x : i % 4 == 1 ? q.y : i % 4 == 2 ? q.z : q.w) = values[i];
                }
            }
        }
        float4* block = &table.alias[base + 4 * k];
        block[0] = float4(s.axis, s.focus);
        block[1] = float4(s.xAxis, s.cutoff);
        block[2] = float4(s.tint, s.smoothStart);
        block[3] = float4(s.angleScale, s.iesScale, asfloat(profile), asfloat(s.profileSize));
    }
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
    std::vector<prism::SceneLight const*> shaped; // kLightShaped records, by block
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
        double weight = double(prism::LightPower(light, sceneRadius));
        if (light.shaping.active && record.info.x != kLightDistant) {
            record.info.y |= kLightShaped;
            record.lobes.z = asfloat(uint32_t(shaped.size()));
            shaped.push_back(&light);
            weight *= ShapingPowerFraction(light);
        }
        table.records.push_back(record);
        weights.push_back(std::max(1e-12, weight));
    }
    table.analyticCount = uint32_t(table.records.size());

    // Constant-emission triangles, by placement id for a deterministic order.
    std::vector<int> placements;
    for (auto const& [id, geometry] : scene.geometry) {
        if (!geometry || scene.disabled_placements.contains(id)) continue;
        if (geometry->type != RTC_GEOMETRY_TYPE_TRIANGLE) continue;
        bool emits = false;
        ForEachPlacementMaterial(scene, id, geometry, [&](Material* material) {
            if (material && material->constantEmission()) emits = true;
        });
        if (emits) placements.push_back(id);
    }
    std::sort(placements.begin(), placements.end());

    struct ConstantTriangle {
        Float3 p[3];
        Material* material;
        ConstantEmission emission;
    };
    std::vector<ConstantTriangle> triangles;
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
            ConstantTriangle t{{world(a), world(b), world(c)}, material, *emission};
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
        }
        if (triangles.size() > maxEmitterTriangles) break;
    }
    if (triangles.size() > maxEmitterTriangles) {
        table.emittersOverBudget = true;
        triangles.clear();
    }
    for (ConstantTriangle const& t : triangles) {
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
    const size_t varyingFirst = table.records.size();
    AddVaryingEmitters(scene, table, weights);
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
    // Per-primitive densities of the varying emitters (negative: front face only).
    for (size_t i = varyingFirst; i < table.records.size(); ++i) {
        LightRecord const& record = table.records[i];
        auto& densities = table.varyingEmitters[int(asuint(record.p0.w))];
        if (densities.empty()) {
            Geometry* geometry = scene.geometry.at(int(asuint(record.p0.w)));
            auto* mesh = dynamic_cast<Mesh*>(geometry);
            densities.assign(mesh ? mesh->indices.size() / 3 : 0, 0.0f);
        }
        if (record.info.z >= densities.size()) continue;
        const float density = float(pmf[i] / double(record.v.w));
        densities[record.info.z] = (record.info.y & kLightTwoSided) != 0u ? density : -density;
    }

    table.alias = BuildAliasTable(pmf);
    AppendShapingBlocks(table, shaped);
    return table;
}

} // namespace prism::device
