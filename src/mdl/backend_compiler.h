#pragma once

#include "context.h"
#include "material_compiler.h"

#include <mi/mdl_sdk.h>
#include <string>
#include <vector>
#include <iostream>
#include <memory>
#include <optional>

namespace mdl {
class NativeTextureRuntime;

// Releases process cache entries before an MDL SDK context shuts down.
void ClearTargetCodeCache(const Context* context = nullptr);

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

    // volume.absorption_coefficient / volume.scattering_coefficient (colors,
    // per scene unit) and the anisotropy of the volume's phase function when
    // it is a single anisotropic_vdf (else isotropic).
    mi::Size volume_absorption_function_index = ~mi::Size(0);
    mi::Size volume_scattering_function_index = ~mi::Size(0);
    mi::Size volume_anisotropy_function_index = ~mi::Size(0);

    // geometry.displacement (float3 offset of the position; CPU only, applied
    // to geometry at load time).
    mi::Size displacement_function_index = ~mi::Size(0);

    // Convenience: DF sub-function offset constants
    static constexpr mi::Size DF_SAMPLE    = 0;
    static constexpr mi::Size DF_EVALUATE  = 1;
    static constexpr mi::Size DF_PDF       = 2;
    static constexpr mi::Size DF_AUXILIARY = 3;
};

// Published components are immutable. A binding always retains its program so
// resource handlers and argument bytes cannot outlive their native ABI.
struct CompiledProgram {
    struct Parameter { std::string name; std::string type_name; };
    mi::base::Handle<const mi::neuraylib::ITarget_code> target_code;
    MaterialFunctions functions;
    const char* ro_data_segment = nullptr;
    mi::Size ro_data_segment_size = 0;
    std::shared_ptr<const NativeTextureRuntime> resource_prefix;
    // Ordered resource capabilities of the compiled material arguments. This
    // deliberately excludes resource values, which belong to MaterialBinding,
    // but prevents a program compiled for one resource ABI from accepting a
    // differently shaped argument interface.
    std::string resource_interface;
    std::vector<Parameter> parameters;
    const Context* owner_identity = nullptr;
    std::string identity;
    bool builtin_resources = false;
    // Database name of the compiled class this program was generated from.
    // GPU devices compile it again for their shading language; argument
    // blocks share the layout. Empty if the class could not be stored.
    std::string compiled_material_name;
    // geometry.cutout_opacity depends only on the arguments (no state or
    // textures): each binding can evaluate it once.
    bool uniform_cutout = false;
    // surface.emission is a diffuse EDF and intensity depending only on the
    // arguments: the front-face radiance is one constant per binding.
    bool uniform_emission = false;
    // geometry.displacement is not the constant zero.
    bool has_displacement = false;
};

struct MaterialBinding {
    std::shared_ptr<const CompiledProgram> program;
    mi::base::Handle<const mi::neuraylib::ITarget_argument_block> argument_block;
    std::shared_ptr<const NativeTextureRuntime> texture_runtime;
    bool is_thin_walled = false;
    std::optional<float> constant_cutout_opacity;
};

struct TargetCode {
    std::shared_ptr<const CompiledProgram> program;
    std::shared_ptr<const MaterialBinding> binding;
    std::string textureFallbackReason;
    mi::neuraylib::Texture_handler_base* texture_handler() const;
    const mi::neuraylib::ITarget_argument_block* argument_data() const {
        return binding ? binding->argument_block.get() : nullptr;
    }
    bool success = false;
    bool cacheHit = false;
    bool cacheReuseRejected = false;
    // Actual SDK calls, including failed/custom discovery translations.
    uint64_t translationCalls = 0;
    uint64_t fallbackTranslations = 0;
    double programWaitSeconds = 0.0;
    double nativeJitSeconds = 0.0;
    std::string error_message;
};

// Phase 5: Compiles a CompiledMaterial to native CPU code using the
// MDL SDK's LLVM-based native backend.
class BackendCompiler {
public:
    explicit BackendCompiler(Context& ctx);

    // Full pipeline: CompiledMaterial -> native target code with callable functions
    TargetCode generate_target_code(const CompiledMaterial& compiled);
    TargetCode bind_arguments(const TargetCode& prototype,
        const mi::neuraylib::IValue_list* overrides,
        mi::neuraylib::ITransaction* transaction);

private:
    void print_context_messages(mi::neuraylib::IMdl_execution_context* context);

    Context& m_ctx;
};

} // namespace mdl
