// One MDL material program as a callable shader of the Vulkan ray tracing
// pipeline (src/device/vulkan/vulkan_material_calls.h). The host inserts the
// scene's link unit with the dispatch of this program only, so DXC emits just
// this program's code. The raygen shader (kernel.hlsl with
// PRISM_CALL_MATERIALS) calls callable 2 x program, and 2 x program + 1 when
// the first did not evaluate the BSDF.
//
// Each part runs the megakernel's own material code after the MDL init of the
// prepared shading point:
// - whole (PRISM_MATERIAL_CALL_WHOLE; both callable slots): GpuBackend::shade
//   (BSDF sample, evaluations, medium), answering kMaterialCallEvaluated;
// - sample (no define): shade's BSDF sample and medium;
// - evaluate (PRISM_MATERIAL_CALL_EVALUATE): shade's BSDF evaluations towards
//   the light sample and (light lobes) the sampled direction.
// Large programs are split into sample and evaluate (MaterialCallPart): the
// driver's compile memory grows with the square of a module's size, so the
// halves compile in much less memory than the whole (docs/gpu_devices.md,
// "Driver compile budget"). Sample and whole parts also answer the cutout
// opacity and the emission of a light sample's point (kMaterialCallEmitted).

#include "../../shared/prism_gpu_types.h"
#include "../../integrator/path_tracer.h"
#include "resources.hlsl"
#include "../accessors.hlsl"
#include "../geometry.hlsl"
#include "../mdl_types.hlsl"
#include "../mdl_runtime.hlsl"
//@PRISM_MDL_CODE@
#include "../gpu_backend.hlsl"

[shader("callable")]
void material(inout MaterialCall call)
{
#ifdef PRISM_SHADER_SALT
    if (call.material == PRISM_SHADER_SALT) return; // never: makes the code unique (cold-compile measurements)
#endif
    g_prism_feedback = (call.flags & kMaterialCallFeedback) != 0u ? 1u : 0u;
    g_prism_feedback_weight = (call.flags >> kMaterialCallFeedbackWeightShift) & kMaterialCallFeedbackWeightMask;
    const GpuMaterial material = gpu_material(call.material);
#ifdef PRISM_MATERIAL_CALL_EVALUATE
    GpuShading shading;
    shading.surface = materialCallSurface(call);
    shading.material = call.material;
    gpu_mdl_init_shading(shading, material);
    GpuBackend backend;
    backend.rng.state = call.rng; // evaluations draw no random numbers
    // shade's evaluation loop: one call site (DXC inlines each call). The
    // sample part's answer in the payload says what to evaluate.
    bool light, lobes;
    materialCallEvaluations(call.flags, light, lobes);
    BsdfEvaluation lightEvaluation = invalidBsdfEvaluation();
    float3 sampleDiffuse = (float3)0.0f;
    [loop] for (uint pass = light ? 0u : 1u; pass < (lobes ? 2u : 1u); ++pass) {
        const BsdfEvaluation e = backend.evaluate(shading, pass == 0u ? call.lightWi : call.wi);
        if (pass == 0u) lightEvaluation = e;
        else sampleDiffuse = diffuseFraction(e);
    }
    storeMaterialEvaluation(call, lightEvaluation, sampleDiffuse);
#else
    if ((call.flags & kMaterialCallModeMask) == kMaterialCallCutout) {
        call.pdf = gpu_mdl_cutout_opacity(materialCallSurface(call), material);
        return;
    }
    GpuShading shading;
    shading.surface = materialCallSurface(call);
    shading.material = call.material;
    gpu_mdl_init_shading(shading, material);
    if ((call.flags & kMaterialCallEmitted) != 0u) {
        call.emission = gpu_mdl_emitter_emission(shading.surface, material, shading.state);
        return;
    }
    GpuBackend backend;
    backend.rng.state = call.rng;
#ifdef PRISM_MATERIAL_CALL_WHOLE
    MaterialQuery query;
    query.xi = call.xi;
    query.lightWi = call.lightWi;
    query.evaluateLight = (call.flags & kMaterialCallEvaluateLight) != 0u;
    query.lobes = (call.flags & kMaterialCallLobes) != 0u;
    const MaterialResult result = backend.shade(shading, query);
    storeMaterialResult(call, result);
    call.flags |= kMaterialCallEvaluated;
#else
    // shade's sample and medium; the evaluations are the evaluate part's.
    MaterialResult result;
    result.sample = backend.sample(shading, call.xi);
    result.light = invalidBsdfEvaluation();
    result.medium = noMedium();
    if (entersMedium(result.sample, shading.surface.front_face)) result.medium = backend.medium(shading);
    result.sampleDiffuse = (float3)0.0f;
    storeMaterialResult(call, result);
#endif
    call.rng = backend.rng.state;
#endif
}
