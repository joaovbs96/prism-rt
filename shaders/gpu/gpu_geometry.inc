// GPU ray-query results and the analytic sphere test, shared by GLSL and HLSL.
// Written in the Prism shading dialect.

#ifndef PRISM_GPU_GEOMETRY_INC
#define PRISM_GPU_GEOMETRY_INC

// Committed result of a closest-hit ray query.
struct GpuRawHit {
    bool hit;
    uint instance;        // top-level instance index
    uint primitive;       // primitive index in the instance's prototype
    float t;
    float2 barycentrics;  // triangles: weights of the second and third vertex
    float3 objectOrigin;  // object-space ray (spheres)
    float3 objectDirection;
};

// Embree's sphere-point test: the nearest root within [tmin, tmax], else the
// farther one. Returns a negative distance on a miss.
PRISM_INLINE float gpu_intersect_sphere(float3 origin, float3 direction, float4 sphere,
                                        float tmin, float tmax)
{
    float3 oc = origin - float3(sphere.x, sphere.y, sphere.z);
    float a = dot(direction, direction);
    float b = dot(oc, direction);
    float c = dot(oc, oc) - sphere.w * sphere.w;
    float disc = b * b - a * c;
    if (disc < 0.0f || !(a > 0.0f)) return -1.0f;
    float s = sqrt(disc);
    float t0 = (-b - s) / a;
    float t1 = (-b + s) / a;
    if (t0 >= tmin && t0 <= tmax) return t0;
    if (t1 >= tmin && t1 <= tmax) return t1;
    return -1.0f;
}

#endif // PRISM_GPU_GEOMETRY_INC
