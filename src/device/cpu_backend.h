#pragma once

// The CPU backend of the shared path tracer (shaders/integrator): Embree
// intersection through SceneManager and the CPU Material / Environment
// classes, including native MDL code.

#include "hlsl_math.h"
#include "../../shaders/integrator/path_tracer.h"
#include "light_table.h"
#include "../camera_rays.h"
#include "../scene.h"

#include <optional>

namespace prism::device {

// The shared path tracer's types live in the global namespace; ::PathRay and
// ::RayFootprint are distinct from prism::PathRay and prism::RayFootprint.

struct CpuHit {
    SurfaceHit surface;
    ::Material* material = nullptr;
    const void* primvarMesh = nullptr;
    const void* instancePrimvars = nullptr;
};

struct CpuShading {
    std::optional<RayHit> hit;   // after Material::perturbNormal
    ::Material* material = nullptr;
    // prepare() results are only read through the shading context.
    mutable ::Material::PreparedShadingContext prepared;
};

inline Float3 ToFloat3(float3 v) { return Float3(v.x, v.y, v.z); }
inline float3 FromFloat3(Float3 const& v) { return float3(v.x(), v.y(), v.z()); }

struct CpuBackend {
    SceneManager* scene = nullptr;
    LightTable const* lights = nullptr;
    Rng rng = {0u};
    // Every material of the scene always blocks shadow rays (no cutouts).
    bool opaqueScene = false;
    // LightTable::lobes
    bool lobes = false;
    // CpuSceneState::culling
    bool culling = false;

    static prism::PathRay ToQuery(::PathRay const& ray)
    {
        prism::PathRay query;
        query.ray.org_x = ray.ray.origin.x;
        query.ray.org_y = ray.ray.origin.y;
        query.ray.org_z = ray.ray.origin.z;
        query.ray.dir_x = ray.ray.direction.x;
        query.ray.dir_y = ray.ray.direction.y;
        query.ray.dir_z = ray.ray.direction.z;
        query.ray.tnear = ray.ray.tnear;
        query.ray.tfar = ray.ray.tfar;
        query.ray.mask = -1;
        query.footprint = ToFootprint(ray.footprint);
        return query;
    }

    bool occludedByOpaque(::PathRay const& ray)
    {
        if (!opaqueScene) return false;
        RTCRay query = ToQuery(ray).ray;
        rtcOccluded1(scene->scene, &query);
        return query.tfar < 0.0f;
    }

    bool mayHaveCutouts() { return !opaqueScene; }
    CpuHit intersectNonOpaque(::PathRay const& ray) { return intersect(ray); }

    CpuHit intersect(::PathRay const& ray)
    {
        prism::PathRay query;
        query.ray.org_x = ray.ray.origin.x;
        query.ray.org_y = ray.ray.origin.y;
        query.ray.org_z = ray.ray.origin.z;
        query.ray.dir_x = ray.ray.direction.x;
        query.ray.dir_y = ray.ray.direction.y;
        query.ray.dir_z = ray.ray.direction.z;
        query.ray.tnear = ray.ray.tnear;
        query.ray.tfar = ray.ray.tfar;
        query.ray.mask = -1;
        query.footprint = ToFootprint(ray.footprint);
        const RayHit rh = scene->castRayCulled(query, culling);
        CpuHit hit;
        hit.surface = FromRayHit(rh);
        hit.primvarMesh = rh.primvarMesh;
        hit.instancePrimvars = rh.instancePrimvars;
        if (hit.surface.valid) hit.material = scene->getMaterial(rh);
        return hit;
    }

    CpuShading prepare(CpuHit const& hit)
    {
        CpuShading shading;
        shading.hit = ToRayHit(hit.surface);
        shading.hit->primvarMesh = hit.primvarMesh;
        shading.hit->instancePrimvars = hit.instancePrimvars;
        shading.material = hit.material;
        hit.material->perturbNormal(*shading.hit);
        hit.material->prepare(*shading.hit, shading.prepared);
        return shading;
    }

    BsdfSample sample(CpuShading const& shading, float4 xi)
    {
        shading.prepared.hasXi = true;
        shading.prepared.xi[0] = xi.x;
        shading.prepared.xi[1] = xi.y;
        shading.prepared.xi[2] = xi.z;
        shading.prepared.xi[3] = xi.w;
        const BRDFSample s = shading.material->samplePrepared(*shading.hit, rng.state, shading.prepared);
        const BsdfSample result = {FromFloat3(s.wi), FromFloat3(s.color), FromFloat3(s.emission), s.scattered,
                                   s.pdf, s.delta, s.null_event, s.transmitted};
        return result;
    }

    MediumCoefficients medium(CpuShading const& shading)
    {
        MediumCoefficients result = {float3(0.0f), float3(0.0f), 0.0f, false};
        const auto medium = shading.material->medium(*shading.hit, shading.prepared);
        if (!medium) return result;
        result.sigmaA = FromFloat3(medium->absorption);
        result.sigmaS = FromFloat3(medium->scattering);
        result.g = medium->anisotropy;
        result.valid = true;
        return result;
    }

    BsdfEvaluation evaluate(CpuShading const& shading, float3 wi)
    {
        const BRDFEvaluation e = shading.material->evaluatePrepared(*shading.hit, ToFloat3(wi), shading.prepared);
        const BsdfEvaluation result = {FromFloat3(e.value), e.pdf, e.valid, FromFloat3(e.diffuse)};
        return result;
    }

    // Texture streaming feedback weight (GPU devices only).
    void pathWeight(float3 const&) {}

    MaterialResult shade(CpuShading const& shading, MaterialQuery const& query)
    {
        MaterialResult result;
        result.sample = sample(shading, query.xi);
        result.light = evaluatesLight(query, result.sample) ? evaluate(shading, query.lightWi)
                                                             : invalidBsdfEvaluation();
        result.medium = entersMedium(result.sample, shading.hit->front_face) ? medium(shading) : noMedium();
        // The diffuse part of the BSDF towards the sample (light lobes).
        result.sampleDiffuse = float3(0.0f);
        if (query.lobes && result.sample.scattered && !result.sample.delta && !result.sample.null_event)
            result.sampleDiffuse = diffuseFraction(evaluate(shading, result.sample.wi));
        return result;
    }

    bool supportsNee(CpuShading const& shading) { return shading.material->supportsNee(*shading.hit); }
    bool provenOpaque(CpuHit const& hit) { return hit.material->provenOpaque(); }
    float cutoutOpacity(CpuHit const& hit)
    {
        RayHit rh = ToRayHit(hit.surface);
        rh.primvarMesh = hit.primvarMesh;
        rh.instancePrimvars = hit.instancePrimvars;
        return hit.material->cutoutOpacity(rh);
    }

    float3 environmentRadiance(float3 direction, float angular)
    {
        return FromFloat3(scene->environment->radiance(
            ToFloat3(direction), prism::DirectionFootprint{angular, scene->textureFiltering}));
    }

    EnvironmentLightSample sampleEnvironment(float4 u, float angular)
    {
        const float point[4] = {u.x, u.y, u.z, u.w};
        const ::EnvironmentSample s = scene->environment->sampleDirectionUniform(
            point, prism::DirectionFootprint{angular, scene->textureFiltering});
        const EnvironmentLightSample result = {FromFloat3(s.wi), FromFloat3(s.radiance), s.pdf};
        return result;
    }

    float environmentPdf(float3 direction) { return scene->environment->directionPdf(ToFloat3(direction)); }

    bool lightLobes() { return lobes; }
    EnvironmentLobes environmentLobes(float3 direction, float angular)
    {
        Color3 diffuse, specular;
        EnvironmentRadianceLobes(scene->environment, ToFloat3(direction),
                                 prism::DirectionFootprint{angular, scene->textureFiltering}, &diffuse, &specular);
        const EnvironmentLobes result = {FromFloat3(diffuse), FromFloat3(specular)};
        return result;
    }

    uint lightCount() { return lights ? uint(lights->records.size()) : 0u; }
    uint analyticLightCount() { return lights ? lights->analyticCount : 0u; }
    LightRecord light(uint i) { return lights->records[i]; }
    float4 lightAlias(uint i) { return lights->alias[i]; }
    float3 lightTexture(uint texture, float2 st)
    {
        if (!lights || texture >= lights->textures.size()) return float3(1.0f);
        return FromFloat3(lights->textures[texture]->sampleInterp(st.x, st.y, Float3(0.0f)));
    }
    uint lightLinkMask(CpuHit const& hit) { return linkMasks(hit).first; }
    uint shadowLinkMask(CpuHit const& hit) { return linkMasks(hit).second; }
    std::pair<uint32_t, uint32_t> linkMasks(CpuHit const& hit) const
    {
        if (!scene->lightLinking) return {~0u, ~0u};
        auto found = scene->placement_link_masks.find(int(hit.surface.instance));
        return found == scene->placement_link_masks.end() ? std::pair<uint32_t, uint32_t>{0u, 0u}
                                                          : found->second;
    }

    float emitterPdfArea(CpuHit const& hit)
    {
        if (!lights || lights->emitters.empty() || !hit.material) return 0.0f;
        auto found = lights->emitters.find(hit.material);
        if (found == lights->emitters.end()) return 0.0f;
        if (!found->second.twoSided && !hit.surface.front_face) return 0.0f;
        auto geometry = scene->geometry.find(int(hit.surface.instance));
        if (geometry == scene->geometry.end() || !geometry->second ||
            geometry->second->type != RTC_GEOMETRY_TYPE_TRIANGLE)
            return 0.0f;
        return found->second.pdfArea;
    }
    int maxDepth() { return scene->max_depth; }
    int rouletteDepth() { return scene->rr_min_depth; }
    bool environmentMis() { return scene->environmentSampling; }

    static prism::RayFootprint ToFootprint(::RayFootprint const& f)
    {
        prism::RayFootprint r;
        r.dOdx = ToFloat3(f.dOdx);
        r.dOdy = ToFloat3(f.dOdy);
        r.dDdx = ToFloat3(f.dDdx);
        r.dDdy = ToFloat3(f.dDdy);
        r.width = f.width;
        r.spread = f.spread;
        r.exact = f.exact;
        return r;
    }

    static ::RayFootprint FromFootprint(prism::RayFootprint const& f)
    {
        const ::RayFootprint r = {FromFloat3(f.dOdx), FromFloat3(f.dOdy), FromFloat3(f.dDdx), FromFloat3(f.dDdy),
                                f.width, f.spread, f.exact};
        return r;
    }

    static SurfaceHit FromRayHit(RayHit const& rh)
    {
        SurfaceHit hit;
        hit.valid = rh.geomid != int(RTC_INVALID_GEOMETRY_ID);
        hit.p = FromFloat3(rh.p);
        hit.o = FromFloat3(rh.o);
        hit.d = FromFloat3(rh.d);
        hit.n = FromFloat3(rh.n);
        hit.n_g = FromFloat3(rh.n_g);
        hit.tangent = FromFloat3(rh.tangent);
        hit.bitangent_sign = rh.bitangent_sign;
        hit.u = rh.u;
        hit.v = rh.v;
        hit.front_face = rh.front_face;
        hit.t = rh.t;
        hit.instance = uint(rh.geomid);
        hit.primitive = uint(rh.primid);
        hit.ray_footprint = FromFootprint(rh.ray_footprint);
        hit.texture_footprint = {rh.texture_footprint.dudx, rh.texture_footprint.dvdx, rh.texture_footprint.dudy,
                                 rh.texture_footprint.dvdy, rh.texture_footprint.valid,
                                 rh.texture_footprint.fallback,
                                 rh.texture_footprint.filtering == prism::TextureFiltering::Filtered};
        hit.dpdx = FromFloat3(rh.dpdx);
        hit.dpdy = FromFloat3(rh.dpdy);
        hit.bary_u = rh.bary_u;
        hit.bary_v = rh.bary_v;
        hit.vertex0 = hit.vertex1 = hit.vertex2 = 0u;
        hit.prototype = 0u;
        hit.instanceRecord = 0xffffffffu;
        return hit;
    }

    static RayHit ToRayHit(SurfaceHit const& hit)
    {
        RTCRayHit miss{};
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit rh(miss);
        rh.p = ToFloat3(hit.p);
        rh.o = ToFloat3(hit.o);
        rh.d = ToFloat3(hit.d);
        rh.n = ToFloat3(hit.n);
        rh.n_g = ToFloat3(hit.n_g);
        rh.n_s = Float3(0.0f);
        rh.tangent = ToFloat3(hit.tangent);
        rh.bitangent_sign = hit.bitangent_sign;
        rh.u = hit.u;
        rh.v = hit.v;
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
        rh.dpdx = ToFloat3(hit.dpdx);
        rh.dpdy = ToFloat3(hit.dpdy);
        rh.bary_u = hit.bary_u;
        rh.bary_v = hit.bary_v;
        return rh;
    }
};

using CpuPathTracer = PathTracer<CpuBackend, CpuHit, CpuShading>;

inline CameraParams ToCameraParams(prism::CameraRayParameters const& camera)
{
    CameraParams params;
    for (int r = 0; r < 4; ++r) {
        params.inverseProjection[r] = float4(float(camera.inverseProjection[r][0]),
            float(camera.inverseProjection[r][1]), float(camera.inverseProjection[r][2]),
            float(camera.inverseProjection[r][3]));
        params.cameraToWorld[r] = float4(float(camera.cameraToWorld[r][0]), float(camera.cameraToWorld[r][1]),
            float(camera.cameraToWorld[r][2]), float(camera.cameraToWorld[r][3]));
    }
    params.nearClip = camera.nearClip;
    params.farClip = camera.farClip;
    params.apertureRadius = float(camera.apertureRadius);
    params.focusDistance = float(camera.focusDistance);
    params.orthographic = camera.orthographic;
    return params;
}

} // namespace prism::device
