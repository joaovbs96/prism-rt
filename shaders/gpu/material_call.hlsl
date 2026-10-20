// The payload of a material program called as a separate shader (Vulkan:
// callable shaders of the ray tracing pipeline, see
// src/device/vulkan/vulkan_material_calls.h). The caller packs the prepared
// shading point and the material query; the program answers with
// backend.shade's MaterialResult, or with the cutout opacity of a hit.
//
// Only what the MDL state and the BSDF read travels: 47 words in, 21 out
// (272 bytes). The payload goes through the callable's stack, so every word
// costs memory traffic on each call.

#pragma once

static const uint kMaterialCallShade = 0u;   // MaterialResult for the query
static const uint kMaterialCallCutout = 1u;  // the hit's cutout opacity

// flags: inputs
static const uint kMaterialCallModeMask = 1u;
static const uint kMaterialCallFrontFace = 2u;
static const uint kMaterialCallFiltered = 4u;
static const uint kMaterialCallEvaluateLight = 8u;
static const uint kMaterialCallFeedbackShift = 10u; // 2 bits: the caller's texture feedback mode
static const uint kMaterialCallFeedbackMask = 3072u;
// flags: outputs
static const uint kMaterialCallScattered = 16u;
static const uint kMaterialCallDelta = 32u;
static const uint kMaterialCallNullEvent = 64u;
static const uint kMaterialCallTransmitted = 128u;
static const uint kMaterialCallLightValid = 256u;
static const uint kMaterialCallMediumValid = 512u;

struct MaterialCall {
    // In: the shading point (SurfaceHit fields the MDL state and BSDF read).
    float3 p;
    float3 o;
    float3 d;
    float3 n;
    float3 ng;
    float3 tangent;
    float3 dpdx;
    float3 dpdy;
    float4 uvFootprint;   // dudx, dvdx, dudy, dvdy
    float2 uv;
    float2 bary;
    float bitangentSign;
    uint vertex0;
    uint vertex1;
    uint vertex2;
    uint prototype;
    uint material;
    uint flags;
    uint rng;             // in and out: backend.rng
    // In: the query.
    float4 xi;
    float3 lightWi;
    // Out: MaterialResult (or the opacity in `pdf` for kMaterialCallCutout).
    float3 wi;
    float3 color;
    float3 emission;
    float pdf;
    float3 lightValue;
    float lightPdf;
    float3 sigmaA;
    float3 sigmaS;
    float g;
};

MaterialCall materialCall(SurfaceHit hit, uint material, uint mode)
{
    MaterialCall call;
    call.p = hit.p;
    call.o = hit.o;
    call.d = hit.d;
    call.n = hit.n;
    call.ng = hit.n_g;
    call.tangent = hit.tangent;
    call.dpdx = hit.dpdx;
    call.dpdy = hit.dpdy;
    call.uvFootprint = float4(hit.texture_footprint.dudx, hit.texture_footprint.dvdx, hit.texture_footprint.dudy,
                              hit.texture_footprint.dvdy);
    call.uv = float2(hit.u, hit.v);
    call.bary = float2(hit.bary_u, hit.bary_v);
    call.bitangentSign = hit.bitangent_sign;
    call.vertex0 = hit.vertex0;
    call.vertex1 = hit.vertex1;
    call.vertex2 = hit.vertex2;
    call.prototype = hit.prototype;
    call.material = material;
    call.flags = mode | (hit.front_face ? kMaterialCallFrontFace : 0u) |
                 (hit.texture_footprint.filtered ? kMaterialCallFiltered : 0u);
#ifdef PRISM_TEXTURE_STREAMING
    call.flags |= g_prism_feedback << kMaterialCallFeedbackShift;
#endif
    call.rng = 0u;
    call.xi = (float4)0.0f;
    call.lightWi = (float3)0.0f;
    call.wi = (float3)0.0f;
    call.color = (float3)0.0f;
    call.emission = (float3)0.0f;
    call.pdf = 0.0f;
    call.lightValue = (float3)0.0f;
    call.lightPdf = 0.0f;
    call.sigmaA = (float3)0.0f;
    call.sigmaS = (float3)0.0f;
    call.g = 0.0f;
    return call;
}

// The shading point as the callee sees it; fields the payload does not
// carry are neutral (no MDL code or BSDF reads them).
SurfaceHit materialCallSurface(MaterialCall call)
{
    SurfaceHit hit;
    hit.valid = true;
    hit.p = call.p;
    hit.o = call.o;
    hit.d = call.d;
    hit.n = call.n;
    hit.n_g = call.ng;
    hit.tangent = call.tangent;
    hit.bitangent_sign = call.bitangentSign;
    hit.u = call.uv.x;
    hit.v = call.uv.y;
    hit.front_face = (call.flags & kMaterialCallFrontFace) != 0u;
    hit.t = 0.0f;
    hit.instance = 0u;
    hit.primitive = 0u;
    hit.ray_footprint = emptyFootprint();
    hit.texture_footprint.dudx = call.uvFootprint.x;
    hit.texture_footprint.dvdx = call.uvFootprint.y;
    hit.texture_footprint.dudy = call.uvFootprint.z;
    hit.texture_footprint.dvdy = call.uvFootprint.w;
    hit.texture_footprint.valid = true;
    hit.texture_footprint.fallback = false;
    hit.texture_footprint.filtered = (call.flags & kMaterialCallFiltered) != 0u;
    hit.dpdx = call.dpdx;
    hit.dpdy = call.dpdy;
    hit.bary_u = call.bary.x;
    hit.bary_v = call.bary.y;
    hit.vertex0 = call.vertex0;
    hit.vertex1 = call.vertex1;
    hit.vertex2 = call.vertex2;
    hit.prototype = call.prototype;
    return hit;
}

void storeMaterialResult(inout MaterialCall call, MaterialResult result)
{
    call.wi = result.sample.wi;
    call.color = result.sample.color;
    call.emission = result.sample.emission;
    call.pdf = result.sample.pdf;
    call.lightValue = result.light.value;
    call.lightPdf = result.light.pdf;
    call.sigmaA = result.medium.sigmaA;
    call.sigmaS = result.medium.sigmaS;
    call.g = result.medium.g;
    uint flags = call.flags & (kMaterialCallModeMask | kMaterialCallFrontFace | kMaterialCallFiltered |
                               kMaterialCallEvaluateLight);
    if (result.sample.scattered) flags |= kMaterialCallScattered;
    if (result.sample.delta) flags |= kMaterialCallDelta;
    if (result.sample.null_event) flags |= kMaterialCallNullEvent;
    if (result.sample.transmitted) flags |= kMaterialCallTransmitted;
    if (result.light.valid) flags |= kMaterialCallLightValid;
    if (result.medium.valid) flags |= kMaterialCallMediumValid;
    call.flags = flags;
}

MaterialResult loadMaterialResult(MaterialCall call)
{
    MaterialResult result;
    result.sample.wi = call.wi;
    result.sample.color = call.color;
    result.sample.emission = call.emission;
    result.sample.pdf = call.pdf;
    result.sample.scattered = (call.flags & kMaterialCallScattered) != 0u;
    result.sample.delta = (call.flags & kMaterialCallDelta) != 0u;
    result.sample.null_event = (call.flags & kMaterialCallNullEvent) != 0u;
    result.sample.transmitted = (call.flags & kMaterialCallTransmitted) != 0u;
    result.light.value = call.lightValue;
    result.light.pdf = call.lightPdf;
    result.light.valid = (call.flags & kMaterialCallLightValid) != 0u;
    result.medium.sigmaA = call.sigmaA;
    result.medium.sigmaS = call.sigmaS;
    result.medium.g = call.g;
    result.medium.valid = (call.flags & kMaterialCallMediumValid) != 0u;
    return result;
}
