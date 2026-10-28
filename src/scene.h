#pragma once

#include <filesystem>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include <embree4/rtcore.h>
#include <embree4/rtcore_common.h>

// project libraries
#include "float3.h"
#include "ray.h"
#include "sampling.h"
#include "material.h"
#include "geometry.h"
#include "environment.h"
#include "lights.h"
#include "render_profile.h"
#include "render_defaults.h"

// TODO: for motion blur, we need to add a new copy of the vertexbuffer per timestep
// so we need to have the vertexbuffer and apply some sort of transformation to the buffer
// The easiest way would be to make all meshes be objects, like our mesh loaders,
// and add functions that operate on top of the buffers - e.g. rotating, translating, scaling.
// We need to add these, regardless, so it's better to do the two things at once.
// We can, then, add the transforms at a certain timestep, which will create the additional
// buffers, if the timestep is different than 0.

class SceneManager
{
public:
    struct PlacementTransform {
        std::array<float, 9> normal{};
        std::array<float, 9> tangent{};
        bool flipOrientation = false;
        bool mirrored = false;
    };
    // `color`: the colour management the scene was loaded with (its lights,
    // materials and textures are in its working space); devices read it.
    SceneManager(RTCDevice &device_, std::shared_ptr<const prism::color::ColorManagement> color)
        : colorManagement(std::move(color))
    {
        if (!colorManagement) throw std::invalid_argument("SceneManager needs a colour management");
        device = &device_;
        scene = rtcNewScene(*device);
    }

    prism::color::ColorManagement const& color() const { return *colorManagement; }

    // Attaches a mesh's triangles (when it has any) as one placement. The
    // public scene's commit (src/scene/scene.cpp) builds SceneManagers; the
    // Hydra delegate builds its own.
    void attachMesh(Mesh* mesh) {
        if (!mesh->vertices.empty() && !mesh->indices.empty()) {
            const unsigned id = mesh->setupEmbree(*device, scene);
            geometry.emplace(id, mesh);
        }
    }

    RTCRayHit intersectRaw(prism::PathRay const &pathRay)
    {
        RTCRayHit e_rayhit{};
        e_rayhit.ray = pathRay.ray;
        // initialize_ray supplies the normal transport epsilon. Preserve an
        // explicit primary clipping interval, including a near plane of zero.
        e_rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        e_rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;
        for (auto& id : e_rayhit.hit.instID)
            id = RTC_INVALID_GEOMETRY_ID;

        // intersect a single ray with the scene
        prism::profiling::Scope intersectionTimer(prism::profiling::shadow
            ? prism::profiling::ShadowIntersection : prism::profiling::Intersection);
        rtcIntersect1(scene, &e_rayhit);
        intersectionTimer.stop();
        return e_rayhit;
    }

    RayHit prepareHit(RTCRayHit e_rayhit,
                      prism::PathRay const &pathRay)
    {
        prism::profiling::Scope hitTimer(prism::profiling::shadow
            ? prism::profiling::ShadowHitPreparation : prism::profiling::HitPreparation);

        if (e_rayhit.hit.geomID == RTC_INVALID_GEOMETRY_ID) {
            RayHit miss(e_rayhit);
            miss.ray_footprint = pathRay.footprint;
            return miss;
        }
        else
        {
            const unsigned int placementId =
                e_rayhit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID
                    ? e_rayhit.hit.instID[0] : e_rayhit.hit.geomID;
            Geometry *geom = geometry.at(placementId);
            RTCGeometry interpolation = geom->geom;
            auto leaf = intersection_geometry.find(placementId);
            if (leaf != intersection_geometry.end()) interpolation = leaf->second;
            auto placement = placement_transforms.find(placementId);
            Float3 localAnalyticNormal;
            Float3 const *analyticNormal = nullptr;
            if (placement != placement_transforms.end()) {
                Float3 ng(e_rayhit.hit.Ng_x, e_rayhit.hit.Ng_y,
                          e_rayhit.hit.Ng_z);
                if (geom->type == RTC_GEOMETRY_TYPE_SPHERE_POINT) {
                    localAnalyticNormal = ng;
                    analyticNormal = &localAnalyticNormal;
                }
                auto const &m = placement->second.normal;
                ng = Float3(ng.x()*m[0] + ng.y()*m[3] + ng.z()*m[6],
                            ng.x()*m[1] + ng.y()*m[4] + ng.z()*m[7],
                            ng.x()*m[2] + ng.y()*m[5] + ng.z()*m[8]);
                // Match the flattened baseline's transformed-edge winding.
                const bool flipOrientation = placement->second.flipOrientation ||
                    (placement->second.mirrored &&
                     geom->type == RTC_GEOMETRY_TYPE_TRIANGLE);
                if (flipOrientation) ng = -ng;
                e_rayhit.hit.Ng_x = ng.x(); e_rayhit.hit.Ng_y = ng.y();
                e_rayhit.hit.Ng_z = ng.z();
            }
            RayHit hit(e_rayhit, &interpolation, geom->type, analyticNormal);
            if (geom->type == RTC_GEOMETRY_TYPE_TRIANGLE) hit.primvarMesh = dynamic_cast<Mesh const*>(geom);
            if (!placement_primvars.empty())
                if (auto found = placement_primvars.find(int(placementId)); found != placement_primvars.end())
                    hit.instancePrimvars = found->second.get();
            hit.ray_footprint = pathRay.footprint;
            hit.geomid = static_cast<int>(placementId);
            if (placement != placement_transforms.end()) {
                auto transformDirection = [](Float3 const &v,
                                             std::array<float, 9> const &m) {
                    return Float3(v.x()*m[0] + v.y()*m[3] + v.z()*m[6],
                                  v.x()*m[1] + v.y()*m[4] + v.z()*m[7],
                                  v.x()*m[2] + v.y()*m[5] + v.z()*m[8]);
                };
                // Triangle shading normals are interpolated in prototype
                // space. Sphere-point normals already came from the
                // transformed geometric normal above.
                if (geom->type == RTC_GEOMETRY_TYPE_TRIANGLE)
                    hit.n = transformDirection(
                        hit.n, placement->second.normal).normalize();
                if (!hit.tangent.is_zero())
                    hit.tangent = transformDirection(
                        hit.tangent, placement->second.tangent).normalize();
                // Ng was transformed before constructing RayHit so its
                // front-face decision and the interpolated normal agree.
                const bool flipOrientation = placement->second.flipOrientation ||
                    (placement->second.mirrored &&
                     geom->type == RTC_GEOMETRY_TYPE_TRIANGLE);
                if (flipOrientation) {
                    // Authored normals in the flattened path receive the same
                    // orientation parity correction before face forwarding.
                    hit.n = -hit.n;
                }
                if (placement->second.mirrored)
                    hit.bitangent_sign *= -1.0f;
            }
            prepareFootprint(hit, geom, placement == placement_transforms.end()
                ? nullptr : &placement->second);
            return hit;
        }
    }

    RayHit cast_ray(prism::PathRay const &pathRay)
    {
        return prepareHit(intersectRaw(pathRay), pathRay);
    }

    RayHit cast_ray(RTCRay ray) { return cast_ray(prism::PathRay{ray, {}}); }

    void prepareFootprint(RayHit &hit, Geometry *geom,
                          PlacementTransform const *placement)
    {
        auto rayOrigin = hit.o;
        auto rayDirection = hit.d;
        const float predicted = std::max(1.0e-8f,
            hit.ray_footprint.width + std::max(0.0f, hit.t) *
                hit.ray_footprint.spread);
        auto planeDifferential = [&](Float3 const &dO, Float3 const &dD) {
            const Float3 o = rayOrigin + dO;
            const Float3 d = rayDirection + dD;
            const float denominator = dot(d, hit.n_g);
            if (std::abs(denominator) < 1.0e-6f) return Float3(0.0f);
            const float distance = dot(hit.p - o, hit.n_g) / denominator;
            const Float3 value = o + distance * d - hit.p;
            const float length = value.length();
            if (!std::isfinite(length) || length > predicted * 8.0f)
                return Float3(0.0f);
            return value;
        };
        if (hit.ray_footprint.exact) {
            hit.dpdx = planeDifferential(
                hit.ray_footprint.dOdx, hit.ray_footprint.dDdx);
            hit.dpdy = planeDifferential(
                hit.ray_footprint.dOdy, hit.ray_footprint.dDdy);
        }
        if (hit.dpdx.is_zero() || hit.dpdy.is_zero()) {
            Float3 axis = std::abs(hit.n_g.x()) < 0.8f
                ? Float3(1.0f,0.0f,0.0f) : Float3(0.0f,1.0f,0.0f);
            Float3 x = cross(axis, hit.n_g).normalize();
            Float3 y = cross(hit.n_g, x).normalize();
            hit.dpdx = predicted * x;
            hit.dpdy = predicted * y;
            hit.texture_footprint.fallback = true;
        }

        Float3 dpdu(0.0f), dpdv(0.0f);
        bool spherePole=false;
        if (geom->type == RTC_GEOMETRY_TYPE_TRIANGLE) {
            auto *mesh = dynamic_cast<Mesh *>(geom);
            const size_t base = static_cast<size_t>(hit.primid) * 3;
            if (mesh && base + 2 < mesh->indices.size()) {
                const unsigned i0=mesh->indices[base], i1=mesh->indices[base+1],
                               i2=mesh->indices[base+2];
                if (i0 < mesh->vertices.size() && i1 < mesh->vertices.size() &&
                    i2 < mesh->vertices.size() && i0 < mesh->texcoords.size() &&
                    i1 < mesh->texcoords.size() && i2 < mesh->texcoords.size()) {
                    const Float3 e1=mesh->vertices[i1]-mesh->vertices[i0];
                    const Float3 e2=mesh->vertices[i2]-mesh->vertices[i0];
                    const float du1=mesh->texcoords[i1].u()-mesh->texcoords[i0].u();
                    const float dv1=mesh->texcoords[i1].v()-mesh->texcoords[i0].v();
                    const float du2=mesh->texcoords[i2].u()-mesh->texcoords[i0].u();
                    const float dv2=mesh->texcoords[i2].v()-mesh->texcoords[i0].v();
                    const float determinant=du1*dv2-du2*dv1;
                    if (std::abs(determinant)>1.0e-12f) {
                        const float inverse=1.0f/determinant;
                        dpdu=inverse*(dv2*e1-dv1*e2);
                        dpdv=inverse*(-du2*e1+du1*e2);
                    }
                }
            }
        } else if (geom->type == RTC_GEOMETRY_TYPE_SPHERE_POINT) {
            Float3 sphereCenter;
            float radius = 0.0f;
            const bool validSphere = geom->analyticSphere(
                static_cast<unsigned int>(std::max(hit.primid, 0)),
                &sphereCenter, &radius);
            if (!validSphere || !(radius > 0.0f) || !std::isfinite(radius)) {
                spherePole = true;
                dpdu = Float3(0.0f);
                dpdv = Float3(0.0f);
            } else {
            const float theta = PI - 2.0f*PI*hit.u;
            const float phi = PI*hit.v;
            const float sinPhi=std::sin(phi), cosPhi=std::cos(phi);
            spherePole=std::abs(sinPhi)<1.0e-4f;
            dpdu=2.0f*PI*radius*Float3(
                sinPhi*std::sin(theta),0.0f,-sinPhi*std::cos(theta));
            dpdv=PI*radius*Float3(
                cosPhi*std::cos(theta),-sinPhi,cosPhi*std::sin(theta));
            }
        }
        if (placement) {
            auto transform = [&](Float3 const &v) {
                auto const &m=placement->tangent;
                return Float3(v.x()*m[0]+v.y()*m[3]+v.z()*m[6],
                              v.x()*m[1]+v.y()*m[4]+v.z()*m[7],
                              v.x()*m[2]+v.y()*m[5]+v.z()*m[8]); };
            dpdu=transform(dpdu); dpdv=transform(dpdv);
        }
        const float a=dot(dpdu,dpdu), b=dot(dpdu,dpdv), c=dot(dpdv,dpdv);
        const float determinant=a*c-b*b;
        hit.texture_footprint.filtering = textureFiltering;
        if (determinant > 1.0e-20f && std::isfinite(determinant)) {
            auto solve = [&](Float3 const &dp, float &du, float &dv) {
                const float x=dot(dp,dpdu), y=dot(dp,dpdv);
                du=(c*x-b*y)/determinant; dv=(a*y-b*x)/determinant; };
            solve(hit.dpdx,hit.texture_footprint.dudx,hit.texture_footprint.dvdx);
            solve(hit.dpdy,hit.texture_footprint.dudy,hit.texture_footprint.dvdy);
            hit.texture_footprint.valid =
                std::isfinite(hit.texture_footprint.dudx) &&
                std::isfinite(hit.texture_footprint.dvdx) &&
                std::isfinite(hit.texture_footprint.dudy) &&
                std::isfinite(hit.texture_footprint.dvdy);
        }
        if(spherePole) {
            const float meridianScale=std::sqrt(std::max(c,1.0e-12f));
            const float dv=std::min(1.0f,predicted/meridianScale);
            hit.texture_footprint={0.5f,dv,0.5f,dv,true,true,textureFiltering};
        }
        else if (!hit.texture_footprint.valid) {
            const float scale=std::sqrt(std::max({a,c,1.0e-12f}));
            const float rho=std::min(1.0f,predicted/scale);
            hit.texture_footprint={rho,0.0f,0.0f,rho,true,true,textureFiltering};
        }
    }

    Material *getMaterial(RayHit const &hit)
    {
        auto binding = placement_materials.find(hit.geomid);
        if (binding != placement_materials.end()) {
            auto triangle = binding->second.triangles.find(hit.primid);
            if (triangle != binding->second.triangles.end()) return triangle->second;
            if (binding->second.fallback) return binding->second.fallback;
        }
        return geometry.at(hit.geomid)->getMaterial(hit.primid, hit.instid);
    }

    Material *getMaterial(RTCRayHit const &hit)
    {
        const unsigned int placementId =
            hit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID
                ? hit.hit.instID[0] : hit.hit.geomID;
        auto binding = placement_materials.find(placementId);
        if (binding != placement_materials.end()) {
            auto primitive = binding->second.triangles.find(hit.hit.primID);
            if (primitive != binding->second.triangles.end())
                return primitive->second;
            if (binding->second.fallback) return binding->second.fallback;
        }
        const unsigned int instanceId =
            hit.hit.instID[0] != RTC_INVALID_GEOMETRY_ID
                ? hit.hit.instID[0] : RTC_INVALID_GEOMETRY_ID;
        return geometry.at(placementId)->getMaterial(
            hit.hit.primID, instanceId);
    }

    void commitScene()
    {
        // FIXME: commented out, transforms don't seem to be working atm
        // setup scene geometry transformation
        for (auto it = geometry.begin(); it != geometry.end(); it++)
        {
            // TODO: we will need to turn the transforms into vectors, where each time entry will have its own transform
            it->second->applyTransform();
        }

        // finalize scene setup
        rtcCommitScene(scene);
    }

    std::shared_ptr<const prism::color::ColorManagement> colorManagement;
    RTCDevice *device;
    RTCScene scene;
    std::unordered_map<int, Geometry *> geometry;
    // Top-level placement id -> leaf geometry used for interpolation.  For
    // ordinary geometry the Geometry::geom handle is used directly.
    std::unordered_map<int, RTCGeometry> intersection_geometry;
    std::unordered_map<int, PlacementTransform> placement_transforms;
    struct PlacementMaterialBinding {
        Material *fallback = nullptr;
        std::unordered_map<int, Material *> triangles;
    };
    std::unordered_map<int, PlacementMaterialBinding> placement_materials;
    // Placements disabled in the Embree scene (Hydra visibility). Embree
    // cannot be queried for this; GPU devices mirror the scene from here.
    std::unordered_set<int> disabled_placements;
    Environment *environment;
    // Analytic lights (UsdLux shapes); domes are the environment.
    std::vector<prism::SceneLight> lights;
    // Light/shadow-link categories of placements (bits, see SceneLight);
    // empty without linking. Placements missing here belong to none.
    std::unordered_map<int, std::pair<uint32_t, uint32_t>> placement_link_masks;
    bool lightLinking = false;
    int rr_min_depth = 4;
    int max_depth = 32;
    bool environmentSampling = true;
    prism::TextureFiltering textureFiltering =
        prism::TextureFiltering::Filtered;
    // GPU devices that stream textures (Vulkan) load the mip levels shading
    // asks for instead of whole textures (prism:textureStreaming).
    bool textureStreaming = true;
    // GPU texture budget in MiB (prism:gpuTextureBudgetMiB); 0 is 45% of the
    // device's memory. Textures beyond it upload at reduced resolution. The
    // front ends set it (their default is PRISM_GPU_TEXTURE_BUDGET_MIB).
    int gpuTextureBudgetMiB = 0;
    // USD doubleSided (prism:honorDoubleSided, off by default): with it, the
    // back faces of single-sided geometry are not hit, unless their material
    // transmits. Single-sided are the placements listed here (Hydra) and the
    // triangles Mesh::single_sided marks (the CLI's merged mesh).
    bool honorDoubleSided = false;
    std::unordered_set<int> single_sided_placements;
    // Instance-rate primvars by placement (InstancePrimvars).
    std::unordered_map<int, std::shared_ptr<const InstancePrimvars>> placement_primvars;

    // Whether some geometry is single-sided while doubleSided is honoured.
    bool cullsBackFaces() const
    {
        if (!honorDoubleSided) return false;
        if (!single_sided_placements.empty()) return true;
        for (auto const& [id, g] : geometry)
            if (auto const* mesh = dynamic_cast<Mesh const*>(g);
                mesh && (mesh->single_sided_all || !mesh->single_sided.empty())) return true;
        return false;
    }

    // Whether a back-face hit on this primitive of this placement is culled.
    bool culledBackFace(int placement, int primitive)
    {
        if (!honorDoubleSided || primitive < 0) return false;
        bool single = single_sided_placements.contains(placement);
        auto found = geometry.find(placement);
        if (found == geometry.end() || !found->second || found->second->type != RTC_GEOMETRY_TYPE_TRIANGLE)
            return false;
        if (!single) {
            auto const* mesh = dynamic_cast<Mesh const*>(found->second);
            single = mesh && (mesh->single_sided_all ||
                (size_t(primitive) < mesh->single_sided.size() && mesh->single_sided[size_t(primitive)]));
        }
        if (!single) return false;
        Material* material = nullptr;
        auto binding = placement_materials.find(placement);
        if (binding != placement_materials.end()) {
            auto triangle = binding->second.triangles.find(primitive);
            material = triangle != binding->second.triangles.end() ? triangle->second : binding->second.fallback;
        }
        if (!material) {
            const unsigned instance = intersection_geometry.contains(placement) ? unsigned(placement)
                                                                                 : RTC_INVALID_GEOMETRY_ID;
            try { material = found->second->getMaterial(primitive, instance); }
            catch (std::exception const&) { material = nullptr; }
        }
        return !(material && material->transmissive());
    }

    // cast_ray that passes through culled back faces (culledBackFace), for
    // the integrator's rays and the primary hits.
    RayHit castRayCulled(prism::PathRay pathRay, bool culling)
    {
        RayHit hit = cast_ray(pathRay);
        if (!culling) return hit;
        for (int crossings = 0; crossings < 64; ++crossings) {
            if (hit.geomid == int(RTC_INVALID_GEOMETRY_ID) || hit.front_face || !culledBackFace(hit.geomid, hit.primid))
                return hit;
            pathRay.ray.tnear = std::nextafter(hit.t, std::numeric_limits<float>::infinity());
            if (!(pathRay.ray.tnear <= pathRay.ray.tfar)) break;
            hit = cast_ray(pathRay);
        }
        RTCRayHit miss{};
        miss.ray = pathRay.ray;
        miss.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        RayHit result(miss);
        result.ray_footprint = pathRay.footprint;
        return result;
    }
    RayHit castRayCulled(RTCRay ray, bool culling) { return castRayCulled(prism::PathRay{ray, {}}, culling); }
};
