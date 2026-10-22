// GPU ray-query results and the analytic sphere test, shared by GLSL and HLSL.

#pragma once

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
float gpu_intersect_sphere(float3 origin, float3 direction, float4 sphere,
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



// prism:honorDoubleSided: whether a candidate triangle is a culled back face:
// its instance and primitive are marked (kInstanceCullBackFaces,
// kPrimitiveCullBackFace) and the ray does not see its front face, as
// gpu_intersect decides front_face (the geometric normal in world space,
// flipped with the instance's orientation).
bool gpu_culled_triangle(uint instanceIndex, uint primitive, float3 direction)
{
    const GpuInstance inst = gpu_instance(instanceIndex);
    if ((inst.info.y & kInstanceCullBackFaces) == 0u) return false;
    if ((gpu_primitive_material(inst.info.z + primitive) & kPrimitiveCullBackFace) == 0u) return false;
    const GpuPrototype proto = gpu_prototype(inst.info.x);
    if (proto.info.x != kProtoTriangles) return false;
    const uint base = 3u * (proto.info.z + primitive);
    const float3 p0 = gpu_position(proto.info.y + gpu_index(base));
    const float3 p1 = gpu_position(proto.info.y + gpu_index(base + 1u));
    const float3 p2 = gpu_position(proto.info.y + gpu_index(base + 2u));
    float3 ng = cross(p1 - p0, p2 - p0);
    if ((inst.info.y & kInstancePlacement) != 0u) {
        ng = float3(dot(ng, inst.normalColumns[0].xyz), dot(ng, inst.normalColumns[1].xyz),
                    dot(ng, inst.normalColumns[2].xyz));
        if ((inst.info.y & kInstanceFlip) != 0u) ng = -ng;
    }
    return !(dot(direction, normalize(ng)) < 0.0f);
}

// Closest hit among the instances selected by `mask`.
GpuRawHit gpu_trace(Ray ray, uint mask)
{
    RaytracingAccelerationStructure scene = sceneStructure();
    RayDesc desc;
    desc.Origin = ray.origin;
    desc.Direction = ray.direction;
    desc.TMin = ray.tnear;
    // An empty interval (a medium scatter distance below the transport
    // epsilon) misses, as in Embree; ray queries require TMin <= TMax.
    const bool empty = !(ray.tnear <= ray.tfar);
    desc.TMax = empty ? ray.tnear : ray.tfar;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, empty ? 0u : mask, desc);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            GpuInstance inst = gpu_instance(query.CandidateInstanceIndex());
            GpuPrototype proto = gpu_prototype(inst.info.x);
            float4 sphere = gpu_sphere(proto.info.z + query.CandidatePrimitiveIndex());
            float t = gpu_intersect_sphere(query.CandidateObjectRayOrigin(),
                                           query.CandidateObjectRayDirection(), sphere,
                                           query.RayTMin(), query.CommittedRayT());
            if (t >= 0.0f) query.CommitProceduralPrimitiveHit(t);
        } else if (!gpu_culled_triangle(query.CandidateInstanceIndex(), query.CandidatePrimitiveIndex(),
                                        ray.direction)) {
            query.CommitNonOpaqueTriangleHit();
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
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
        hit.hit = true;
        hit.instance = query.CommittedInstanceIndex();
        hit.primitive = query.CommittedPrimitiveIndex();
        hit.t = query.CommittedRayT();
        hit.barycentrics = query.CommittedTriangleBarycentrics();
    } else if (query.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT) {
        hit.hit = true;
        hit.instance = query.CommittedInstanceIndex();
        hit.primitive = query.CommittedPrimitiveIndex();
        hit.t = query.CommittedRayT();
        hit.objectOrigin = query.CommittedObjectRayOrigin();
        hit.objectDirection = query.CommittedObjectRayDirection();
    }
    return hit;
}


// Any hit among the opaque instances: the query ends at the first one.
bool gpu_occluded_by_opaque(Ray ray)
{
    RayDesc desc;
    desc.Origin = ray.origin;
    desc.Direction = ray.direction;
    desc.TMin = ray.tnear;
    const bool empty = !(ray.tnear <= ray.tfar);
    desc.TMax = empty ? ray.tnear : ray.tfar;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(sceneStructure(), RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         empty ? 0u : kMaskOpaque, desc);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            GpuInstance inst = gpu_instance(query.CandidateInstanceIndex());
            GpuPrototype proto = gpu_prototype(inst.info.x);
            float4 sphere = gpu_sphere(proto.info.z + query.CandidatePrimitiveIndex());
            float t = gpu_intersect_sphere(query.CandidateObjectRayOrigin(), query.CandidateObjectRayDirection(),
                                           sphere, query.RayTMin(), query.CommittedRayT());
            if (t >= 0.0f) query.CommitProceduralPrimitiveHit(t);
        } else if (!gpu_culled_triangle(query.CandidateInstanceIndex(), query.CandidatePrimitiveIndex(),
                                        ray.direction)) {
            query.CommitNonOpaqueTriangleHit();
        }
    }
    return query.CommittedStatus() != COMMITTED_NOTHING;
}
