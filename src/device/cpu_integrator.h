#pragma once

// The shared integrator (shaders/integrator/path_tracer.inc) compiled as C++
// on top of a CPU backend: Embree intersection through SceneManager and the
// CPU Material / Environment classes, including native MDL code.

#include "../../shaders/integrator/prism_sl.h"
#include "../camera_rays.h"
#include "../scene.h"

#include <optional>

namespace prism::device::cpu {

using namespace prism::sl;

using MaterialHandle = ::Material*;

#include "../../shaders/integrator/integrator_types.inc"

struct MaterialContext {
    std::optional<RayHit> hit;
    ::Material::PreparedShadingContext prepared;
};

inline Float3 ToFloat3(float3 v) { return Float3(v.x, v.y, v.z); }
inline float3 FromFloat3(Float3 const& v) { return float3(v.x(), v.y(), v.z()); }

inline prism::RayFootprint ToFootprint(RayFootprint const& f)
{
    prism::RayFootprint r;
    r.dOdx = ToFloat3(f.dOdx); r.dOdy = ToFloat3(f.dOdy);
    r.dDdx = ToFloat3(f.dDdx); r.dDdy = ToFloat3(f.dDdy);
    r.width = f.width; r.spread = f.spread; r.exact = f.exact;
    return r;
}

inline RayFootprint FromFootprint(prism::RayFootprint const& f)
{
    RayFootprint r;
    r.dOdx = FromFloat3(f.dOdx); r.dOdy = FromFloat3(f.dOdy);
    r.dDdx = FromFloat3(f.dDdx); r.dDdy = FromFloat3(f.dDdy);
    r.width = f.width; r.spread = f.spread; r.exact = f.exact;
    return r;
}

class CpuBackend {
public:
    explicit CpuBackend(SceneManager& scene) : m_scene(&scene) {}

    Hit backend_intersect(IN(PathRay) ray)
    {
        prism::PathRay query;
        query.ray.org_x = ray.ray.origin.x; query.ray.org_y = ray.ray.origin.y;
        query.ray.org_z = ray.ray.origin.z;
        query.ray.dir_x = ray.ray.direction.x; query.ray.dir_y = ray.ray.direction.y;
        query.ray.dir_z = ray.ray.direction.z;
        query.ray.tnear = ray.ray.tnear;
        query.ray.tfar = ray.ray.tfar;
        query.ray.mask = -1;
        query.footprint = ToFootprint(ray.footprint);
        const RayHit rh = m_scene->cast_ray(query);
        Hit hit = FromRayHit(rh);
        hit.material = hit.hit ? m_scene->getMaterial(rh) : nullptr;
        return hit;
    }

    void backend_prepare_material(INOUT(Hit) hit, OUT(MaterialContext) context)
    {
        context.hit = ToRayHit(hit);
        hit.material->perturbNormal(*context.hit);
        hit.material->prepare(*context.hit, context.prepared);
        hit.n = FromFloat3(context.hit->n);
        hit.n_g = FromFloat3(context.hit->n_g);
        hit.n_s = FromFloat3(context.hit->n_s);
    }

    BsdfSample backend_sample_material(IN(Hit) hit, INOUT(MaterialContext) context, INOUT(uint) seed)
    {
        const BRDFSample s = hit.material->samplePrepared(*context.hit, seed, context.prepared);
        BsdfSample result;
        result.wi = FromFloat3(s.wi);
        result.color = FromFloat3(s.color);
        result.emission = FromFloat3(s.emission);
        result.scattered = s.scattered;
        result.pdf = s.pdf;
        result.delta = s.delta;
        result.null_event = s.null_event;
        return result;
    }

    BsdfEvaluation backend_evaluate_material(IN(Hit) hit, float3 wi, INOUT(MaterialContext) context)
    {
        const BRDFEvaluation e = hit.material->evaluatePrepared(*context.hit, ToFloat3(wi), context.prepared);
        BsdfEvaluation result;
        result.value = FromFloat3(e.value);
        result.pdf = e.pdf;
        result.valid = e.valid;
        return result;
    }

    bool backend_supports_nee(IN(Hit) hit, INOUT(MaterialContext) context)
    {
        return hit.material->supportsNee(*context.hit);
    }

    bool backend_proven_opaque(IN(Hit) hit) { return hit.material->provenOpaque(); }

    float backend_cutout_opacity(IN(Hit) hit) { return hit.material->cutoutOpacity(ToRayHit(hit)); }

    float3 backend_environment_radiance(float3 direction, float angularRadius)
    {
        return FromFloat3(m_scene->environment->radiance(ToFloat3(direction),
            prism::DirectionFootprint{angularRadius, m_scene->textureFiltering}));
    }

    EnvironmentSample backend_environment_sample(INOUT(uint) seed, float angularRadius)
    {
        const ::EnvironmentSample s = m_scene->environment->sampleDirection(
            seed, prism::DirectionFootprint{angularRadius, m_scene->textureFiltering});
        EnvironmentSample result;
        result.wi = FromFloat3(s.wi);
        result.radiance = FromFloat3(s.radiance);
        result.pdf = s.pdf;
        return result;
    }

    float backend_environment_pdf(float3 direction)
    {
        return m_scene->environment->directionPdf(ToFloat3(direction));
    }

    int backend_max_depth() { return m_scene->max_depth; }
    int backend_roulette_depth() { return m_scene->rr_min_depth; }
    bool backend_environment_mis() { return m_scene->environmentSampling; }

private:
    static Hit FromRayHit(RayHit const& rh)
    {
        Hit hit;
        hit.hit = rh.geomid != int(RTC_INVALID_GEOMETRY_ID);
        hit.p = FromFloat3(rh.p); hit.o = FromFloat3(rh.o); hit.d = FromFloat3(rh.d);
        hit.n = FromFloat3(rh.n); hit.n_g = FromFloat3(rh.n_g); hit.n_s = FromFloat3(rh.n_s);
        hit.tangent = FromFloat3(rh.tangent);
        hit.bitangent_sign = rh.bitangent_sign;
        hit.u = rh.u; hit.v = rh.v;
        hit.front_face = rh.front_face;
        hit.t = rh.t;
        hit.instance = uint(rh.geomid);
        hit.primitive = uint(rh.primid);
        hit.material = nullptr;
        hit.ray_footprint = FromFootprint(rh.ray_footprint);
        hit.texture_footprint.dudx = rh.texture_footprint.dudx;
        hit.texture_footprint.dvdx = rh.texture_footprint.dvdx;
        hit.texture_footprint.dudy = rh.texture_footprint.dudy;
        hit.texture_footprint.dvdy = rh.texture_footprint.dvdy;
        hit.texture_footprint.valid = rh.texture_footprint.valid;
        hit.texture_footprint.fallback = rh.texture_footprint.fallback;
        hit.texture_footprint.filtered =
            rh.texture_footprint.filtering == prism::TextureFiltering::Filtered;
        hit.dpdx = FromFloat3(rh.dpdx); hit.dpdy = FromFloat3(rh.dpdy);
        return hit;
    }

    RayHit ToRayHit(Hit const& hit) const
    {
        RTCRayHit miss{};
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit rh(miss);
        rh.p = ToFloat3(hit.p); rh.o = ToFloat3(hit.o); rh.d = ToFloat3(hit.d);
        rh.n = ToFloat3(hit.n); rh.n_g = ToFloat3(hit.n_g); rh.n_s = ToFloat3(hit.n_s);
        rh.tangent = ToFloat3(hit.tangent);
        rh.bitangent_sign = hit.bitangent_sign;
        rh.u = hit.u; rh.v = hit.v;
        rh.front_face = hit.front_face;
        rh.t = hit.t;
        rh.geomid = int(hit.instance);
        rh.prototypeid = int(hit.instance);
        rh.primid = int(hit.primitive);
        rh.instid = int(RTC_INVALID_GEOMETRY_ID);
        rh.ray_footprint = ToFootprint(hit.ray_footprint);
        rh.texture_footprint.dudx = hit.texture_footprint.dudx;
        rh.texture_footprint.dvdx = hit.texture_footprint.dvdx;
        rh.texture_footprint.dudy = hit.texture_footprint.dudy;
        rh.texture_footprint.dvdy = hit.texture_footprint.dvdy;
        rh.texture_footprint.valid = hit.texture_footprint.valid;
        rh.texture_footprint.fallback = hit.texture_footprint.fallback;
        rh.texture_footprint.filtering = hit.texture_footprint.filtered
            ? prism::TextureFiltering::Filtered : prism::TextureFiltering::Bilinear;
        rh.dpdx = ToFloat3(hit.dpdx); rh.dpdy = ToFloat3(hit.dpdy);
        return rh;
    }

    SceneManager* m_scene;
};

// The integrator, compiled against the CPU backend.
struct Integrator : CpuBackend {
    using CpuBackend::CpuBackend;
#include "../../shaders/integrator/path_tracer.inc"
};

inline Integrator::CameraParams ToCameraParams(prism::CameraRayParameters const& camera)
{
    Integrator::CameraParams params;
    for (int r = 0; r < 4; ++r) {
        params.inverseProjection[r] = float4(float(camera.inverseProjection[r][0]),
            float(camera.inverseProjection[r][1]), float(camera.inverseProjection[r][2]),
            float(camera.inverseProjection[r][3]));
        params.cameraToWorld[r] = float4(float(camera.cameraToWorld[r][0]),
            float(camera.cameraToWorld[r][1]), float(camera.cameraToWorld[r][2]),
            float(camera.cameraToWorld[r][3]));
    }
    params.nearClip = camera.nearClip;
    params.farClip = camera.farClip;
    params.apertureRadius = float(camera.apertureRadius);
    params.focusDistance = float(camera.focusDistance);
    params.orthographic = camera.orthographic;
    return params;
}

} // namespace prism::device::cpu
