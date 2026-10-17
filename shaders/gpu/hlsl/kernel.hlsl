// Metal device kernel (HLSL, compiled by DXC and Metal shader converter).
// The host substitutes the generated MDL code at the marker below.

#define PRISM_HLSL 1
#define PRISM_MDL_TEXTURE_RESULTS 16
#include "integrator/prism_sl.h"
#include "shared/prism_gpu_types.h"
#define MaterialHandle uint
#include "integrator/integrator_types.inc"
#include "gpu/hlsl/resources.hlsl"
#include "gpu/gpu_geometry.inc"
#include "gpu/hlsl/trace.hlsl"
#include "gpu/hlsl/mdl_types.hlsl"
#include "gpu/mdl_runtime_common.inc"
#include "gpu/mdl_runtime.inc"
//@PRISM_MDL_CODE@
#include "gpu/gpu_backend.inc"
#include "integrator/path_tracer.inc"
#include "gpu/gpu_kernel.inc"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    prism_kernel(id.x);
}
