// One MDL material program as a callable shader of the Vulkan ray tracing
// pipeline (src/device/vulkan/vulkan_material_calls.h). The host inserts the
// scene's link unit with the dispatch of this program only, so DXC emits just
// this program's code. The raygen shader (kernel.hlsl with
// PRISM_CALL_MATERIALS) calls it with the program index as the callable index.
//
// The program runs the megakernel's own material code: the MDL init of the
// prepared shading point, then GpuBackend::shade, or the cutout opacity.

// The BSDF table lookup stays out of line in callables (mdl_runtime.hlsl);
// the compute kernel inlines it (out of line, the driver's compile of the
// kernel took 3.5 times as long).
#define PRISM_NOINLINE [noinline]

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
    const GpuMaterial material = gpu_material(call.material);
    if ((call.flags & kMaterialCallModeMask) == kMaterialCallCutout) {
        call.pdf = gpu_mdl_cutout_opacity(materialCallSurface(call), material);
        return;
    }
    GpuShading shading;
    shading.surface = materialCallSurface(call);
    shading.material = call.material;
    gpu_mdl_init_shading(shading, material);
    GpuBackend backend;
    backend.rng.state = call.rng;
    MaterialQuery query;
    query.xi = call.xi;
    query.lightWi = call.lightWi;
    query.evaluateLight = (call.flags & kMaterialCallEvaluateLight) != 0u;
    const MaterialResult result = backend.shade(shading, query);
    storeMaterialResult(call, result);
    call.rng = backend.rng.state;
}
