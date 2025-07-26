#pragma once

#include <embree4/rtcore.h>

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

            // geometry specific interpolation
            if (geomid != RTC_INVALID_GEOMETRY_ID){
                if (type == RTC_GEOMETRY_TYPE_TRIANGLE) {
                    Float3 m_normal;
                    Float2 m_uv;
                    rtcInterpolate0(*geom, primid, u, v, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 0, reinterpret_cast<float*>(&m_normal), 3);
                    rtcInterpolate0(*geom, primid, u, v, RTC_BUFFER_TYPE_VERTEX_ATTRIBUTE, 1, reinterpret_cast<float*>(&m_uv), 2);

                    // use normal and uv parameters from the mesh
                    n = m_normal.normalize();
                    n = front_face ? n : -n;
                    u = m_uv.u();
                    v = m_uv.v();
                } else if (type == RTC_GEOMETRY_TYPE_SPHERE_POINT) {
                    float theta = std::atan2f(n.z(), n.x());
                    float phi = std::acosf(n.y());
                    
                    u = 1.0 - ((theta + PI) / (2.0f * PI));
                    v = phi / PI;
                }
            }
        }

        Float3 p;
        Float3 o;
        Float3 d;
        Float3 n;
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