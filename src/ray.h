#pragma once

#include <embree4/rtcore.h>

#include "math.h"
#include "float3.h"
#include "rng.h"

class RayHit {
    public:
        RayHit(RTCRayHit e_rayhit) {
            // make sure the normal vector is normalized
            n = Float3(e_rayhit.hit.Ng_x, e_rayhit.hit.Ng_y, e_rayhit.hit.Ng_z).normalize();
            o = Float3(e_rayhit.ray.org_x, e_rayhit.ray.org_y, e_rayhit.ray.org_z);
            d = Float3(e_rayhit.ray.dir_x, e_rayhit.ray.dir_y, e_rayhit.ray.dir_z);
            t = e_rayhit.ray.tfar;
            p = o + t * d;
            geomid = e_rayhit.hit.geomID;

            // check if normal is front facing
            front_face = dot(d, n) < 0;
            n = front_face ? n : -n;
        }

        Float3 p;
        Float3 o;
        Float3 d;
        Float3 n;
        bool front_face;
        float t;
        int geomid;
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