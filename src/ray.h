#pragma once

#include <embree4/rtcore.h>

#include <cmath>

#include "math.h"
#include "float3.h"
#include "float2.h"
#include "rng.h"

class RayHit {
    public:
        RayHit(RTCRayHit e_rayhit, RTCGeometry* geom = nullptr, RTCGeometryType type = RTC_GEOMETRY_TYPE_TRIANGLE) {
            // make sure the normal vector is normalized
            n = Float3(e_rayhit.hit.Ng_x, e_rayhit.hit.Ng_y, e_rayhit.hit.Ng_z).normalize();
            o = Float3(e_rayhit.ray.org_x, e_rayhit.ray.org_y, e_rayhit.ray.org_z);
            d = Float3(e_rayhit.ray.dir_x, e_rayhit.ray.dir_y, e_rayhit.ray.dir_z);
            t = e_rayhit.ray.tfar;
            p = o + t * d;
            geomid = e_rayhit.hit.geomID;
            primid = e_rayhit.hit.primID;
            u = e_rayhit.hit.u;
            v = e_rayhit.hit.v;

            // check if normal is front facing
            front_face = dot(d, n) < 0;
            n = front_face ? n : -n;

            // keep the geometric normal before n becomes the shading normal
            n_g = n;

            // geometry specific interpolation
            if (geomid != RTC_INVALID_GEOMETRY_ID){
                if (type == RTC_GEOMETRY_TYPE_TRIANGLE) {
                    Float3 m_normal;
                    Float2 m_uv;
                    float m_tangent[4]; // xyz = dP/du, w = handedness

                    // u, v are still barycentrics here — used by all three
                    // interpolations, then overwritten with texture UVs.
                    // Triangle meshes always register slots 0, 1 and 2
                    // (see Mesh::setupEmbree / Mesh::computeTangents).
                    rtcInterpolate0(*geom, primid, u, v, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, reinterpret_cast<float*>(&m_normal), 3);
                    rtcInterpolate0(*geom, primid, u, v, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, reinterpret_cast<float*>(&m_uv), 2);
                    rtcInterpolate0(*geom, primid, u, v, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 2, m_tangent, 4);

                    n = m_normal.normalize();
                    n = front_face ? n : -n;
                    u = m_uv.u();
                    v = m_uv.v();
                    tangent = Float3(m_tangent[0], m_tangent[1], m_tangent[2]);
                    bitangent_sign = m_tangent[3] < 0.0f ? -1.0f : 1.0f;
                } else if (type == RTC_GEOMETRY_TYPE_SPHERE_POINT) {
                    // Texture coordinates must not change when viewed from inside.
                    Float3 outward = front_face ? n : -n;
                    float theta = std::atan2f(outward.z(), outward.x());
                    float phi = std::acosf(outward.y());

                    u = 1.0 - ((theta + PI) / (2.0f * PI));
                    v = phi / PI;

                    // dP/du for this uv mapping is (z, 0, -x), taken on the
                    // outward normal. Zero at the poles -> fallback frame.
                    tangent = Float3(outward.z(), 0.0f, -outward.x());
                    bitangent_sign = -1.0f; // v grows downward from +Y pole
                }
            }
        }

        Float3 p;
        Float3 o;
        Float3 d;
        Float3 n;
        Float3 n_g = Float3(0.0f);
        Float3 n_s;
        Float3 tangent = Float3(0.0f); // dP/du; zero = none, use fallback
        float bitangent_sign = 1.0f;   // uv handedness (glTF TANGENT.w)
        float u, v;
        bool front_face;
        float t;
        int geomid, primid, instid;
};

RTCRay initialize_ray(Float3 o, Float3 d){
    RTCRay ray;
    ray.org_x  = o.x(); ray.org_y = o.y(); ray.org_z = o.z();
    ray.dir_x  = d.x(); ray.dir_y = d.y(); ray.dir_z = d.z();
    ray.tnear  = 0.f;
    ray.tfar   = std::numeric_limits<float>::infinity();
    ray.mask = -1;
    return ray;
}
