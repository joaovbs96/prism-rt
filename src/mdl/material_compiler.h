#pragma once

#include "../internal_export.h"
#include "context.h"
#include "../materialx/mdl_generator.h"

#include <mi/mdl_sdk.h>
#include <set>
#include <string>
#include <vector>
#include <iostream>

namespace mdl {

// Selects which class parameters remain runtime arguments. Folding a parameter
// bakes its value into the compiled material, so the SDK can drop BSDF lobes a
// zero weight disables; the resulting program then only suits that value.
// Resource-typed parameters (textures, light profiles, BSDF measurements)
// always remain arguments so resource preflight and the custom texture runtime
// keep binding them per argument block.
struct CompileSpecification {
    // false: every class parameter except strings is an argument (full class
    // compilation; strings are always folded, see fold_string_parameters).
    bool foldParameters = false;
    // With foldParameters, the generated-uniform names that stay arguments.
    std::set<std::string> keptUniforms;
    // A folded program may enter the process target-code cache only when other
    // materials are expected to share it (compatible template groups).
    bool shareProgram = true;
};

// Result of compiling a MaterialX-generated MDL module
struct CompiledMaterial {
    // The compiled material handle (ready for backend compilation in Phase 5)
    mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled_material;

    // The transaction that owns the compiled material's data.
    // MUST stay alive as long as compiled_material is in use.
    // Generated target code can retain references to module database elements,
    // so the transaction is committed when compilation finishes.
    mi::base::Handle<mi::neuraylib::ITransaction> transaction;

    // Database names for cleanup/reference
    std::string module_db_name;      // e.g. "mdl::materialx::test_material"
    std::string definition_db_name;  // e.g. "mdl::materialx::test_material::main_material(...)"
    std::string instance_db_name;    // e.g. "material_instance_test_material"

    bool success = false;
    bool allowTargetCodeReuse = false;
    // Class parameters baked into compiled_material as constants; empty for
    // full class compilation and for the instance-compilation fallback.
    std::vector<std::string> folded_parameters;
    std::string error_message;

    // Call this when you're done with the compiled material
    // (after backend compilation in Phase 5)
    void release() {
        compiled_material.reset();
        if (transaction) {
            transaction->commit();
            transaction.reset();
        }
    }
};

// Phase 4: Loads MaterialX-generated MDL code into the MDL SDK,
// creates a material instance, and compiles it.
class PRISM_INTERNAL_API MaterialCompiler {
public:
    // Takes a reference to an already-initialized MDL Context
    // mx_mdl_path: path to MaterialX's MDL support libraries
    //   e.g. "<MaterialX install>/libraries/mdl"
    //   This directory must contain a "materialx/" subfolder with
    //   core.mdl, sampling.mdl, stdlib_1_10.mdl, pbrlib_1_10.mdl, etc.
    MaterialCompiler(Context& ctx, const std::string& mx_mdl_path);

    // Full pipeline: load MDL string -> create instance -> compile
    // Takes the GeneratedMDL from MDLGenerator (Phase 3 output)
    // mtlx_path: path to the .mtlx file that was loaded. Used to resolve
    //            relative texture paths (the directory containing the .mtlx
    //            is registered as a resource search path).
    // specification: which class parameters stay runtime arguments (default:
    //            all of them).
    CompiledMaterial compile(const GeneratedMDL& generated_mdl,
                             const std::string& mtlx_path = "",
                             const CompileSpecification& specification = {});

    // Individual steps (if you need finer control)

    // Step 4.2: Load MDL source string into the SDK database
    bool load_module_from_string(
        mi::neuraylib::ITransaction* transaction,
        const std::string& module_name,     // e.g. "::materialx::test_material"
        const std::string& mdl_source,
        mi::neuraylib::IMdl_execution_context* context);

    // Step 4.3: Find the material definition in the loaded module
    //           and create an instance with default parameters
    bool create_material_instance(
        mi::neuraylib::ITransaction* transaction,
        const std::string& module_db_name,  // e.g. "mdl::materialx::test_material"
        const std::string& instance_name,   // DB name to store the instance under
        mi::neuraylib::IMdl_execution_context* context);

    // Step 4.4: Compile a material instance into a CompiledMaterial
    mi::base::Handle<const mi::neuraylib::ICompiled_material> compile_instance(
        mi::neuraylib::ITransaction* transaction,
        const std::string& instance_name,
        mi::neuraylib::IMdl_execution_context* context,
        bool classCompilation = true);

private:
    // Helper: print all messages from the execution context
    void print_context_messages(mi::neuraylib::IMdl_execution_context* context);

    // Recompiles a class-compiled result with every parameter folded except
    // resources and specification.keptUniforms. Leaves the unfolded class
    // result in place when nothing would fold or the folded compile fails.
    void fold_class_parameters(
        mi::neuraylib::ITransaction* transaction,
        const std::string& instance_name,
        const CompileSpecification& specification,
        mi::neuraylib::IMdl_execution_context* context,
        CompiledMaterial& result);

    // Recompiles a full class compilation with its string-valued parameters
    // folded. Native argument blocks carry strings as ids into the target
    // code's string table, which holds only the strings of the code itself:
    // a string argument reaches the program as id 0 (unknown), so scene data
    // lookups such as MaterialX geompropvalue would always return their
    // defaults. Folded, the names become code constants with valid ids.
    void fold_string_parameters(
        mi::neuraylib::ITransaction* transaction,
        const std::string& instance_name,
        mi::neuraylib::IMdl_execution_context* context,
        CompiledMaterial& result);

    Context& m_ctx;
    std::string m_mx_mdl_path;
};

} // namespace mdl
