// Closest hit with an inline ray query. Triangles are opaque; sphere
// prototypes are procedural boxes resolved with the analytic sphere test.

#ifndef PRISM_TRACE_HLSL
#define PRISM_TRACE_HLSL

GpuRawHit gpu_trace(Ray ray)
{
    RaytracingAccelerationStructure scene = ResourceDescriptorHeap[PRISM_SLOT_ACCEL];
    RayDesc desc;
    desc.Origin = ray.origin;
    desc.Direction = ray.direction;
    desc.TMin = ray.tnear;
    desc.TMax = ray.tfar;
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, desc);
    while (query.Proceed()) {
        if (query.CandidateType() == CANDIDATE_PROCEDURAL_PRIMITIVE) {
            GpuInstance inst = gpu_instance(query.CandidateInstanceIndex());
            GpuPrototype proto = gpu_prototype(inst.info.x);
            float4 sphere = gpu_sphere(proto.info.z + query.CandidatePrimitiveIndex());
            float t = gpu_intersect_sphere(query.CandidateObjectRayOrigin(),
                                           query.CandidateObjectRayDirection(), sphere,
                                           query.RayTMin(), query.CommittedRayT());
            if (t >= 0.0f) query.CommitProceduralPrimitiveHit(t);
        } else {
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

#endif // PRISM_TRACE_HLSL
