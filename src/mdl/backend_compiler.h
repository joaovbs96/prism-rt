#pragma once

#include "context.h"
#include "material_compiler.h"

#include <mi/mdl_sdk.h>
#include <string>
#include <vector>
#include <iostream>

namespace mdl {

// Stores the function indices for the generated target code.
// These indices are used with ITarget_code::execute_*() methods.
//
// For distribution functions (BSDF/EDF), the stored index is the BASE index
// (pointing to the _sample function). The sub-functions are at offsets:
//   base + 0 = _sample   -> use with execute_bsdf_sample() / execute_edf_sample()
//   base + 1 = _evaluate -> use with execute_bsdf_evaluate() / execute_edf_evaluate()
//   base + 2 = _pdf      -> use with execute_bsdf_pdf() / execute_edf_pdf()
//   base + 3 = _auxiliary -> use with execute_bsdf_auxiliary() / execute_edf_auxiliary()
struct MaterialFunctions {
    // Common init function (single-init mode).
    // Must be called once per shading point before any other function.
    mi::Size init_function_index = ~mi::Size(0);

    // Surface BSDF base index (_sample). Add offsets for evaluate/pdf/auxiliary.
    mi::Size surface_bsdf_function_index = ~mi::Size(0);

    // Surface EDF base index (_sample). Add offsets for evaluate/pdf/auxiliary.
    mi::Size surface_edf_function_index = ~mi::Size(0);

    // Surface emission intensity (scalar expression, not a DF — no offsets)
    mi::Size surface_emission_intensity_function_index = ~mi::Size(0);

    // Backface BSDF base index (only relevant if thin_walled == true)
    mi::Size backface_bsdf_function_index = ~mi::Size(0);

    // Backface EDF base index
    mi::Size backface_edf_function_index = ~mi::Size(0);

    // Backface emission intensity (scalar, no offsets)
    mi::Size backface_emission_intensity_function_index = ~mi::Size(0);

    // geometry.cutout_opacity (scalar, no offsets)
    mi::Size cutout_opacity_function_index = ~mi::Size(0);

    // thin_walled (scalar bool, no offsets)
    mi::Size thin_walled_function_index = ~mi::Size(0);

    // Convenience: DF sub-function offset constants
    static constexpr mi::Size DF_SAMPLE    = 0;
    static constexpr mi::Size DF_EVALUATE  = 1;
    static constexpr mi::Size DF_PDF       = 2;
    static constexpr mi::Size DF_AUXILIARY = 3;
};

// Result of Phase 5: native CPU target code with callable functions
struct TargetCode {
    // The generated native target code object.
    // Owns the JIT-compiled machine code and provides execute_*() methods.
    mi::base::Handle<const mi::neuraylib::ITarget_code> target_code;

    // Function indices for calling the generated BSDF/EDF/expression functions
    MaterialFunctions functions;

    // Whether the material is thin-walled (affects which functions to use)
    bool is_thin_walled = false;

    // Read-only data segment (needed for some materials)
    const char* ro_data_segment = nullptr;
    mi::Size ro_data_segment_size = 0;

    // Argument block (for class-compiled materials; nullptr for instance compilation)
    const char* argument_block = nullptr;

    bool success = false;
    std::string error_message;
};

// Phase 5: Compiles a CompiledMaterial to native CPU code using the
// MDL SDK's LLVM-based native backend.
class BackendCompiler {
public:
    explicit BackendCompiler(Context& ctx);

    // Full pipeline: CompiledMaterial -> native target code with callable functions
    TargetCode generate_target_code(const CompiledMaterial& compiled);

private:
    void print_context_messages(mi::neuraylib::IMdl_execution_context* context);

    Context& m_ctx;
};

} // namespace mdl