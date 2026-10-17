// Closest hit with an inline ray query (GL_EXT_ray_query). Triangles are
// opaque; sphere prototypes are procedural boxes resolved with the analytic
// sphere test. Mirrors shaders/gpu/hlsl/trace.hlsl.

#ifndef PRISM_TRACE_GLSL
#define PRISM_TRACE_GLSL

GpuRawHit gpu_trace(Ray ray)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, g_scene, gl_RayFlagsNoneEXT, 0xFFu, ray.origin, ray.tnear,
                          ray.direction, ray.tfar);
    float closest = ray.tfar;
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) == gl_RayQueryCandidateIntersectionAABBEXT) {
            GpuInstance inst = gpu_instance(uint(rayQueryGetIntersectionInstanceIdEXT(query, false)));
            GpuPrototype proto = gpu_prototype(inst.info.x);
            float4 sphere = gpu_sphere(proto.info.z + uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false)));
            float t = gpu_intersect_sphere(rayQueryGetIntersectionObjectRayOriginEXT(query, false),
                                           rayQueryGetIntersectionObjectRayDirectionEXT(query, false), sphere,
                                           rayQueryGetRayTMinEXT(query), closest);
            if (t >= 0.0f) {
                rayQueryGenerateIntersectionEXT(query, t);
                closest = t;
            }
        } else {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    GpuRawHit hit;
    hit.hit = false;
    hit.instance = 0u;
    hit.primitive = 0u;
    hit.t = ray.tfar;
    hit.barycentrics = float2(0.0f, 0.0f);
    hit.objectOrigin = float3(0.0f, 0.0f, 0.0f);
    hit.objectDirection = float3(0.0f, 0.0f, 0.0f);
    uint committed = rayQueryGetIntersectionTypeEXT(query, true);
    if (committed == gl_RayQueryCommittedIntersectionTriangleEXT) {
        hit.hit = true;
        hit.instance = uint(rayQueryGetIntersectionInstanceIdEXT(query, true));
        hit.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
        hit.t = rayQueryGetIntersectionTEXT(query, true);
        hit.barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
    } else if (committed == gl_RayQueryCommittedIntersectionGeneratedEXT) {
        hit.hit = true;
        hit.instance = uint(rayQueryGetIntersectionInstanceIdEXT(query, true));
        hit.primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, true));
        hit.t = rayQueryGetIntersectionTEXT(query, true);
        hit.objectOrigin = rayQueryGetIntersectionObjectRayOriginEXT(query, true);
        hit.objectDirection = rayQueryGetIntersectionObjectRayDirectionEXT(query, true);
    }
    return hit;
}

#endif // PRISM_TRACE_GLSL
