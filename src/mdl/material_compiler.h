#pragma once

#include "context.h"
#include "../materialx/mdl_generator.h"

#include <mi/mdl_sdk.h>
#include <string>
#include <vector>
#include <iostream>

namespace mdl {

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
class MaterialCompiler {
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
    CompiledMaterial compile(const GeneratedMDL& generated_mdl,
                             const std::string& mtlx_path = "");

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

    Context& m_ctx;
    std::string m_mx_mdl_path;
};

} // namespace mdl
