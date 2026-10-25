// One MDL material program as a callable shader of the Vulkan ray tracing
// pipeline (src/device/vulkan/vulkan_material_calls.h). The host inserts the
// scene's link unit with the dispatch of this program only, so DXC emits just
// this program's code. The raygen shader (kernel.hlsl with
// PRISM_CALL_MATERIALS) calls it with the program index as the callable index.
//
// The program runs the megakernel's own material code: the MDL init of the
// prepared shading point, then GpuBackend::shade, or the cutout opacity, or
// the emission of a light sample's point (kMaterialCallEmitted).

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
    MaterialQuery query;
    query.xi = call.xi;
    query.lightWi = call.lightWi;
    query.evaluateLight = (call.flags & kMaterialCallEvaluateLight) != 0u;
    query.lobes = (call.flags & kMaterialCallLobes) != 0u;
    const MaterialResult result = backend.shade(shading, query);
    storeMaterialResult(call, result);
    call.rng = backend.rng.state;
}
