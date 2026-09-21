#include "material_compiler.h"

#include <format>
#include <filesystem>
#include <fstream>
#include <set>

namespace mdl {

MaterialCompiler::MaterialCompiler(Context& ctx, const std::string& mx_mdl_path)
    : m_ctx(ctx)
    , m_mx_mdl_path(mx_mdl_path)
{
    // Register the MaterialX MDL support libraries path with the SDK.
    // The generated MDL code imports modules like ::materialx::core,
    // ::materialx::sampling, ::materialx::pbrlib_1_10, etc.
    // These .mdl files live under <MaterialX install>/libraries/mdl/materialx/
    // so we add the parent ("libraries/mdl") as a search path.
    if (!mx_mdl_path.empty() && ctx.is_valid()) {
        if (ctx.add_mdl_path(mx_mdl_path)) {
            std::cout << "[OK] Registered MaterialX MDL path: " << mx_mdl_path << "\n";
        } else {
            std::cerr << "[WARN] Failed to register MaterialX MDL path: " << mx_mdl_path << "\n";
        }

        // Also add it as a resource path (for textures referenced by MaterialX MDL modules)
        ctx.add_resource_path(mx_mdl_path);
    }
}

void MaterialCompiler::print_context_messages(mi::neuraylib::IMdl_execution_context* context) {
    for (mi::Size i = 0; i < context->get_messages_count(); ++i) {
        mi::base::Handle<const mi::neuraylib::IMessage> msg(context->get_message(i));
        
        const char* severity_str = "INFO";
        switch (msg->get_severity()) {
            case mi::base::MESSAGE_SEVERITY_ERROR:   severity_str = "ERROR"; break;
            case mi::base::MESSAGE_SEVERITY_WARNING: severity_str = "WARNING"; break;
            case mi::base::MESSAGE_SEVERITY_INFO:    severity_str = "INFO"; break;
            case mi::base::MESSAGE_SEVERITY_VERBOSE: severity_str = "VERBOSE"; break;
            case mi::base::MESSAGE_SEVERITY_DEBUG:   severity_str = "DEBUG"; break;
            default: break;
        }
        
        std::cout << std::format("  [MDL {}] {}\n", severity_str, msg->get_string());
    }
}

// ========================================================================
// Step 4.2: Load MDL source string into the SDK database
// ========================================================================
bool MaterialCompiler::load_module_from_string(
    mi::neuraylib::ITransaction* transaction,
    const std::string& module_name,
    const std::string& mdl_source,
    mi::neuraylib::IMdl_execution_context* context)
{
    mi::neuraylib::IMdl_impexp_api* impexp = m_ctx.get_impexp();
    if (!impexp) {
        std::cerr << "[ERROR] IMdl_impexp_api not available\n";
        return false;
    }

    // load_module_from_string takes:
    //   transaction, module_name (e.g. "::materialx::my_mat"),
    //   mdl_source (the generated MDL code string), context
    mi::Sint32 result = impexp->load_module_from_string(
        transaction,
        module_name.c_str(),
        mdl_source.c_str(),
        context);

    if (context->get_messages_count() > 0) {
        std::cout << "  Messages from load_module_from_string:\n";
        print_context_messages(context);
    }

    if (result < 0) {
        std::cerr << std::format("[ERROR] load_module_from_string failed (code: {})\n", result);
        return false;
    }

    return true;
}

// ========================================================================
// Step 4.3: Create a material instance from the loaded module
// ========================================================================
bool MaterialCompiler::create_material_instance(
    mi::neuraylib::ITransaction* transaction,
    const std::string& module_db_name,
    const std::string& instance_name,
    mi::neuraylib::IMdl_execution_context* context)
{
    // Access the module from the database
    mi::base::Handle<const mi::neuraylib::IModule> module(
        transaction->access<mi::neuraylib::IModule>(module_db_name.c_str()));

    if (!module) {
        std::cerr << std::format("[ERROR] Module '{}' not found in database\n", module_db_name);
        return false;
    }

    // Enumerate material definitions in the module
    mi::Size mat_count = module->get_material_count();
    std::cout << std::format("  Module has {} material definition(s)\n", mat_count);

    if (mat_count == 0) {
        // Fall back to function definitions (MaterialX sometimes generates
        // materials as functions returning material type)
        mi::Size func_count = module->get_function_count();
        std::cout << std::format("  Module has {} function definition(s)\n", func_count);

        for (mi::Size i = 0; i < func_count; ++i) {
            const char* func_db_name = module->get_function(i);
            std::cout << std::format("    function[{}]: {}\n", i, func_db_name);
        }

        if (func_count == 0) {
            std::cerr << "[ERROR] No materials or functions found in module\n";
            return false;
        }

        // Use the first function definition
        const char* def_db_name = module->get_function(0);
        mi::base::Handle<const mi::neuraylib::IFunction_definition> func_def(
            transaction->access<mi::neuraylib::IFunction_definition>(def_db_name));

        if (!func_def) {
            std::cerr << std::format("[ERROR] Cannot access function definition '{}'\n", def_db_name);
            return false;
        }

        // Create an instance with default arguments
        mi::Sint32 result = 0;
        mi::base::Handle<mi::neuraylib::IFunction_call> func_call(
            func_def->create_function_call(nullptr, &result));

        if (result != 0 || !func_call) {
            std::cerr << std::format("[ERROR] create_function_call failed (code: {})\n", result);
            return false;
        }

        // Store the instance in the database
        transaction->store(func_call.get(), instance_name.c_str());
        std::cout << std::format("  Stored function instance as '{}'\n", instance_name);
        return true;
    }

    // Use the first material definition
    for (mi::Size i = 0; i < mat_count; ++i) {
        const char* mat_db_name = module->get_material(i);
        std::cout << std::format("    material[{}]: {}\n", i, mat_db_name);
    }

    const char* mat_def_db_name = module->get_material(0);
    mi::base::Handle<const mi::neuraylib::IFunction_definition> mat_def(
        transaction->access<mi::neuraylib::IFunction_definition>(mat_def_db_name));

    if (!mat_def) {
        std::cerr << std::format("[ERROR] Cannot access material definition '{}'\n", mat_def_db_name);
        return false;
    }

    // Create a material instance with default arguments
    mi::Sint32 result = 0;
    mi::base::Handle<mi::neuraylib::IFunction_call> mat_instance(
        mat_def->create_function_call(nullptr, &result));

    if (result != 0 || !mat_instance) {
        std::cerr << std::format("[ERROR] create_function_call failed (code: {})\n", result);
        return false;
    }

    // Store the instance in the database
    transaction->store(mat_instance.get(), instance_name.c_str());
    std::cout << std::format("  Stored material instance as '{}'\n", instance_name);
    return true;
}

// ========================================================================
// Step 4.4: Compile a material instance
// ========================================================================
mi::base::Handle<const mi::neuraylib::ICompiled_material>
MaterialCompiler::compile_instance(
    mi::neuraylib::ITransaction* transaction,
    const std::string& instance_name,
    mi::neuraylib::IMdl_execution_context* context)
{
    // Access the stored material instance
    mi::base::Handle<const mi::neuraylib::IFunction_call> mat_instance(
        transaction->access<mi::neuraylib::IFunction_call>(instance_name.c_str()));

    if (!mat_instance) {
        std::cerr << std::format("[ERROR] Material instance '{}' not found in DB\n", instance_name);
        return {};
    }

    // Set target type to SID_MATERIAL so MaterialX's material output is converted to MDL's
    // standard material struct. Required for conductor Fresnel to work with F0 > 1.0:
    // without this, the F0->IOR formula (1+sqrt(F0))/(1-sqrt(F0)) is singular for F0>=1.
    mi::base::Handle<mi::neuraylib::IType_factory> tf(
        m_ctx.get_factory()->create_type_factory(transaction));
    mi::base::Handle<const mi::neuraylib::IType> standard_material_type(
        tf->get_predefined_struct(mi::neuraylib::IType_struct::SID_MATERIAL));
    mi::Sint32 opt_result = context->set_option("target_type", standard_material_type.get());
    if (opt_result != 0) {
        std::cerr << std::format("[WARN] set_option(target_type) failed (code: {})\n", opt_result);
        std::cerr << "       Conductor Fresnel for F0 > 1 may not work correctly\n";
    }
    if (context->get_messages_count() > 0) {
        std::cout << "  Messages after set_option(target_type):\n";
        print_context_messages(context);
        context->clear_messages();
    }

    // Compile the material instance
    // Instance compilation mode bakes all parameters into the compiled material
    // (which is what we want for path tracing — no runtime parameter changes)
    mi::Uint32 flags = mi::neuraylib::IMaterial_instance::DEFAULT_OPTIONS;

    // We need to cast to IMaterial_instance to call create_compiled_material
    mi::base::Handle<const mi::neuraylib::IMaterial_instance> mi_instance(
        mat_instance->get_interface<const mi::neuraylib::IMaterial_instance>());

    if (!mi_instance) {
        // If direct cast fails, the function call IS the material instance
        // in newer SDK versions. Try creating compiled material directly.
        // In MDL SDK 2025, IFunction_call for materials supports compilation.
        
        // Alternative approach: use the function call directly
        std::cerr << "[WARN] Could not get IMaterial_instance interface\n";
        std::cerr << "       Trying alternative compilation approach...\n";

        // In newer SDK, we can use IMdl_factory to compile
        mi::neuraylib::IMdl_factory* factory = m_ctx.get_factory();
        if (!factory) {
            std::cerr << "[ERROR] IMdl_factory not available\n";
            return {};
        }

        // Create a compiled material via the material instance
        // The IFunction_call for a material IS the material instance
        mi::base::Handle<mi::neuraylib::IFunction_call> mutable_instance(
            transaction->edit<mi::neuraylib::IFunction_call>(instance_name.c_str()));

        if (!mutable_instance) {
            std::cerr << "[ERROR] Cannot edit material instance\n";
            return {};
        }

        mi::base::Handle<const mi::neuraylib::IMaterial_instance> mi2(
            mutable_instance->get_interface<const mi::neuraylib::IMaterial_instance>());

        if (!mi2) {
            std::cerr << "[ERROR] Still cannot get IMaterial_instance interface\n";
            return {};
        }

        mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
            mi2->create_compiled_material(flags, context));

        if (!compiled) {
            std::cerr << "[ERROR] create_compiled_material failed\n";
            print_context_messages(context);
            return {};
        }

        return compiled;
    }

    mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
        mi_instance->create_compiled_material(flags, context));

    if (!compiled) {
        std::cerr << "[ERROR] create_compiled_material failed\n";
        print_context_messages(context);
        return {};
    }

    return compiled;
}

// ========================================================================
// Full pipeline: GeneratedMDL -> CompiledMaterial
// ========================================================================
CompiledMaterial MaterialCompiler::compile(const GeneratedMDL& generated_mdl,
                                           const std::string& mtlx_path) {
    CompiledMaterial result;
    result.success = false;

    if (!generated_mdl.success) {
        result.error_message = "Input GeneratedMDL was not successful";
        return result;
    }

    if (!m_ctx.is_valid()) {
        result.error_message = "MDL Context is not valid";
        return result;
    }

    // Create execution context for error reporting
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> context(
        m_ctx.get_factory()->create_execution_context());

    if (!context) {
        result.error_message = "Failed to create execution context";
        return result;
    }

    // MDLGenerator inserts authored color-space conversions into the graph
    // before MDL compilation, targeting linear Rec.709.

    // Create a transaction for this compilation
    mi::base::Handle<mi::neuraylib::IScope> scope(
        m_ctx.get_database()->get_global_scope());

    mi::base::Handle<mi::neuraylib::ITransaction> transaction(
        scope->create_transaction());

    if (!transaction) {
        result.error_message = "Failed to create transaction";
        return result;
    }

    // --- Register texture resource paths ---
    // MaterialX generates texture paths in the MDL code that can be:
    //   1. Absolute paths: "C:/textures/diffuse.png"
    //   2. Root-relative paths: "/textures/diffuse.png" (relative to .mtlx location)
    //   3. Relative paths: "textures/diffuse.png"
    //
    // For root-relative paths (starting with /), the MDL SDK resolves them
    // relative to its configured resource search paths. So we register the
    // directory containing the .mtlx file as a resource path.

    // Register the .mtlx file's directory as a resource search path
    if (!mtlx_path.empty()) {
        std::filesystem::path doc_dir = std::filesystem::path(mtlx_path).parent_path();
        if (!doc_dir.empty() && std::filesystem::exists(doc_dir)) {
            if (m_ctx.add_resource_path(doc_dir.string())) {
                std::cout << "[OK] Added document resource path: " << doc_dir.string() << "\n";
            }
        }
    }

    // Also register parent directories of any absolute texture paths
    if (!generated_mdl.texture_paths.empty()) {
        std::set<std::string> registered_dirs;
        for (const auto& tex_path : generated_mdl.texture_paths) {
            if (tex_path.empty()) continue;
            std::filesystem::path p(tex_path);

            // Skip root-relative paths like "/textures/foo.png" —
            // these are resolved via the document resource path above
            if (tex_path.front() == '/' || tex_path.front() == '\\') {
                continue;
            }

            // For absolute paths, register the parent directory
            if (p.is_absolute()) {
                std::string parent = p.parent_path().string();
                if (!parent.empty() && registered_dirs.find(parent) == registered_dirs.end()) {
                    if (m_ctx.add_resource_path(parent)) {
                        std::cout << "[OK] Added texture resource path: " << parent << "\n";
                        registered_dirs.insert(parent);
                    }
                }
            }
        }
    }

    // --- Step 4.2: Load MDL from string ---
    std::cout << "\n--- Step 4.2: Loading MDL into SDK ---\n";

    // The module_name from GeneratedMDL is like "::materialx::test_material"
    std::string module_name = generated_mdl.module_name;

    // Sanitize texture paths in the generated MDL source.
    // MaterialX fileprefix="./" produces paths like "./texture.jpg" in the MDL code,
    // but the MDL SDK rejects strict relative paths ("./") in string-based modules.
    //
    // We resolve them to absolute paths using the .mtlx document directory.
    // This is more robust than converting to root-relative paths ("/texture.jpg")
    // because it doesn't depend on the SDK's resource search path ordering --
    // the texture location is unambiguous regardless of when paths were registered.
    std::string mdl_source = generated_mdl.mdl_source;
    {
        std::string doc_dir_prefix;
        if (!mtlx_path.empty()) {
            std::filesystem::path doc_dir = std::filesystem::path(mtlx_path).parent_path();
            if (!doc_dir.empty()) {
                // Normalize to forward slashes so the MDL SDK accepts it on all platforms
                std::string doc_dir_str = doc_dir.string();
                std::replace(doc_dir_str.begin(), doc_dir_str.end(), '\\', '/');
                doc_dir_prefix = doc_dir_str + "/";
            }
        }

        if (!doc_dir_prefix.empty()) {
            // Replace "./ with "<absolute_doc_dir>/ so paths become fully qualified
            std::string search = "\"./";
            std::string replace = "\"" + doc_dir_prefix;
            size_t pos = 0;
            while ((pos = mdl_source.find(search, pos)) != std::string::npos) {
                mdl_source.replace(pos, search.length(), replace);
                pos += replace.length();
            }
            std::cout << "[OK] Resolved relative texture paths to: " << doc_dir_prefix << "\n";
        } else {
            // Fallback: no .mtlx path available, use root-relative resolution
            std::cout << "[WARN] No .mtlx path provided; falling back to root-relative texture paths\n";
            std::string search = "\"./";
            std::string replace = "\"/";
            size_t pos = 0;
            while ((pos = mdl_source.find(search, pos)) != std::string::npos) {
                mdl_source.replace(pos, search.length(), replace);
                pos += replace.length();
            }
        }
    }

    // Dump the final MDL source next to the .mtlx file for inspection
    {
        std::string dump_name = module_name;
        if (dump_name.size() >= 2 && dump_name[0] == ':' && dump_name[1] == ':')
            dump_name = dump_name.substr(2);
        std::replace(dump_name.begin(), dump_name.end(), ':', '_');
        dump_name += ".mdl";

        std::filesystem::path dump_path;
        if (!mtlx_path.empty())
            dump_path = std::filesystem::path(mtlx_path).parent_path() / dump_name;
        else
            dump_path = std::filesystem::current_path() / dump_name;

        std::ofstream f(dump_path);
        if (f) {
            f << mdl_source;
            std::cout << "[DEBUG] Dumped generated MDL to: " << dump_path.string() << "\n";
        } else {
            std::cerr << "[WARN] Could not write MDL dump to: " << dump_path.string() << "\n";
        }
    }

    if (!load_module_from_string(
            transaction.get(), module_name, mdl_source, context.get())) {
        result.error_message = "Failed to load MDL module from string";
        transaction->commit();
        return result;
    }

    // Get the DB name for the module
    mi::base::Handle<const mi::IString> module_db_name_str(
        m_ctx.get_factory()->get_db_module_name(module_name.c_str()));

    if (!module_db_name_str) {
        result.error_message = "Failed to get DB module name for: " + module_name;
        transaction->commit();
        return result;
    }

    result.module_db_name = module_db_name_str->get_c_str();
    std::cout << std::format("[OK] Module loaded as '{}'\n", result.module_db_name);

    // --- Step 4.3: Create material instance ---
    std::cout << "\n--- Step 4.3: Creating Material Instance ---\n";

    result.instance_db_name = "material_instance_" +
        module_name.substr(module_name.rfind("::") + 2);  // extract simple name

    context->clear_messages();
    if (!create_material_instance(
            transaction.get(), result.module_db_name,
            result.instance_db_name, context.get())) {
        result.error_message = "Failed to create material instance";
        transaction->commit();
        return result;
    }
    std::cout << "[OK] Material instance created\n";

    // --- Step 4.4: Compile the material ---
    std::cout << "\n--- Step 4.4: Compiling Material ---\n";

    context->clear_messages();
    result.compiled_material = compile_instance(
        transaction.get(), result.instance_db_name, context.get());

    if (!result.compiled_material) {
        result.error_message = "Failed to compile material instance";
        transaction->commit();
        return result;
    }

    std::cout << "[OK] Material compiled successfully\n";

    // Print compiled material info
    mi::base::Uuid hash = result.compiled_material->get_hash();
    std::cout << std::format("  Hash: {:08x} {:08x} {:08x} {:08x}\n",
        hash.m_id1, hash.m_id2, hash.m_id3, hash.m_id4);
    std::cout << std::format("  Parameters: {}\n",
        result.compiled_material->get_parameter_count());
    std::cout << std::format("  Temporaries: {}\n",
        result.compiled_material->get_temporary_count());

    // Keep the transaction alive — the compiled material references DB data.
    // The caller must call result.release() when done (after Phase 5).
    result.transaction = transaction;

    result.success = true;
    return result;
}

} // namespace mdl
