#include "material_compiler.h"

#include <cstdint>
#include <cstdlib>
#include <format>
#include <filesystem>
#include <fstream>
#include <set>
#include <array>
#include <mutex>
#include <string_view>
#include "../resource_limits.h"

namespace mdl {

namespace {

bool verbose_mdl() {
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string(value) != "0";
}

// Stable 64-bit FNV-1a, so module names (and MDL dump filenames) do not change
// between runs or standard-library implementations the way std::hash may.
std::string fnv1a_hex(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return std::format("{:016x}", hash);
}

// The generator names modules after the material alone, so two documents that
// both define e.g. "Gold" would collide in the MDL database -- and the SDK
// reports a duplicate module as success, silently reusing the first shader.
// Suffix the name with a hash of the source document path + material name.
std::string make_unique_module_name(const std::string& module_name,
                                    const std::string& mtlx_path) {
    if (mtlx_path.empty())
        return module_name;

    std::error_code error;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(mtlx_path, error);
    const std::string path_key = error ? mtlx_path : canonical.generic_string();

    const std::string simple_name = module_name.substr(module_name.rfind("::") + 2);
    return module_name + "_" + fnv1a_hex(path_key + "\n" + simple_name);
}

// Produces a structural identity for a generated material module by replacing
// only top-level material parameter default expressions. The complete body,
// annotations, imports, parameter order and types remain byte-for-byte in the
// key. Parentheses and strings inside defaults are handled explicitly.
std::string canonical_parameter_key(const std::string& source) {
    const size_t material = source.find("export material ");
    if (material == std::string::npos) return {};
    const size_t open = source.find('(', material);
    if (open == std::string::npos) return {};
    std::string key = source;
    size_t i = open + 1;
    int paren = 1, brace = 0, bracket = 0;
    bool quoted = false, escaped = false;
    while (i < key.size() && paren > 0) {
        const char c = key[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            ++i; continue;
        }
        if (c == '"') { quoted = true; ++i; continue; }
        if (c == '(') ++paren;
        else if (c == ')') { --paren; if (paren == 0) break; }
        else if (c == '{') ++brace;
        else if (c == '}') --brace;
        else if (c == '[') ++bracket;
        else if (c == ']') --bracket;
        else if (c == '=' && paren == 1 && brace == 0 && bracket == 0) {
            const size_t valueStart = i + 1;
            size_t j = valueStart;
            int vp = 0, vb = 0, vs = 0;
            bool vq = false, ve = false;
            for (; j < key.size(); ++j) {
                const char d = key[j];
                if (vq) {
                    if (ve) ve = false;
                    else if (d == '\\') ve = true;
                    else if (d == '"') vq = false;
                    continue;
                }
                if (d == '"') { vq = true; continue; }
                if (d == '(') ++vp;
                else if (d == ')') { if (vp == 0 && vb == 0 && vs == 0) break; --vp; }
                else if (d == '{') ++vb;
                else if (d == '}') --vb;
                else if (d == '[') {
                    if (vp == 0 && vb == 0 && vs == 0 && j + 1 < key.size() && key[j + 1] == '[')
                        break;
                    ++vs;
                } else if (d == ']') --vs;
                else if (d == ',' && vp == 0 && vb == 0 && vs == 0) break;
            }
            key.replace(valueStart, j - valueStart, " <parameter-default> ");
            i = valueStart + sizeof(" <parameter-default> ") - 1;
            continue;
        }
        ++i;
    }
    return key;
}

std::array<std::mutex, 64> gCanonicalModuleMutexes;

std::string canonical_module_name(const std::string& structuralKey) {
    return "::prism_material_classes::m_" + fnv1a_hex(structuralKey);
}

// Textures, light profiles and BSDF measurements, also inside compounds.
bool contains_resource(const mi::neuraylib::IValue* value) {
    if (!value) return false;
    mi::base::Handle<const mi::neuraylib::IValue_resource> resource(
        value->get_interface<mi::neuraylib::IValue_resource>());
    if (resource) return true;
    mi::base::Handle<const mi::neuraylib::IValue_compound> compound(
        value->get_interface<mi::neuraylib::IValue_compound>());
    for (mi::Size i = 0; compound && i < compound->get_size(); ++i) {
        mi::base::Handle<const mi::neuraylib::IValue> child(compound->get_value(i));
        if (contains_resource(child.get())) return true;
    }
    return false;
}

} // namespace

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
            if (verbose_mdl()) std::cout << "[OK] Registered MaterialX MDL path: " << mx_mdl_path << "\n";
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
        const std::string_view text = msg->get_string() ? msg->get_string() : "";
        if (!verbose_mdl()) {
            if (msg->get_severity() > mi::base::MESSAGE_SEVERITY_WARNING) continue;
            if (text.find("C183 unused parameter") != std::string_view::npos) continue;
        }
        
        const char* severity_str = "INFO";
        switch (msg->get_severity()) {
            case mi::base::MESSAGE_SEVERITY_ERROR:   severity_str = "ERROR"; break;
            case mi::base::MESSAGE_SEVERITY_WARNING: severity_str = "WARNING"; break;
            case mi::base::MESSAGE_SEVERITY_INFO:    severity_str = "INFO"; break;
            case mi::base::MESSAGE_SEVERITY_VERBOSE: severity_str = "VERBOSE"; break;
            case mi::base::MESSAGE_SEVERITY_DEBUG:   severity_str = "DEBUG"; break;
            default: break;
        }
        
        std::cerr << std::format("  [MDL {}] {}\n", severity_str, msg->get_string());
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
        if (verbose_mdl()) std::cout << "  Messages from load_module_from_string:\n";
        print_context_messages(context);
    }

    if (result < 0) {
        std::cerr << std::format("[ERROR] load_module_from_string failed (code: {})\n", result);
        return false;
    }
    // 1 = a module of this name already exists; the SDK keeps the old one and
    // ignores the new source.
    if (result == 1)
        std::cerr << std::format("[WARN] Module '{}' was already loaded; reusing the existing definition\n", module_name);

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
    if (verbose_mdl()) std::cout << std::format("  Module has {} material definition(s)\n", mat_count);

    if (mat_count == 0) {
        // Fall back to function definitions (MaterialX sometimes generates
        // materials as functions returning material type)
        mi::Size func_count = module->get_function_count();
        if (verbose_mdl()) std::cout << std::format("  Module has {} function definition(s)\n", func_count);

        for (mi::Size i = 0; i < func_count; ++i) {
            const char* func_db_name = module->get_function(i);
            if (verbose_mdl()) std::cout << std::format("    function[{}]: {}\n", i, func_db_name);
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
        if (verbose_mdl()) std::cout << std::format("  Stored function instance as '{}'\n", instance_name);
        return true;
    }

    // Use the first material definition
    for (mi::Size i = 0; i < mat_count; ++i) {
        const char* mat_db_name = module->get_material(i);
        if (verbose_mdl()) std::cout << std::format("    material[{}]: {}\n", i, mat_db_name);
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
    if (verbose_mdl()) std::cout << std::format("  Stored material instance as '{}'\n", instance_name);
    return true;
}

// ========================================================================
// Step 4.4: Compile a material instance
// ========================================================================
mi::base::Handle<const mi::neuraylib::ICompiled_material>
MaterialCompiler::compile_instance(
    mi::neuraylib::ITransaction* transaction,
    const std::string& instance_name,
    mi::neuraylib::IMdl_execution_context* context,
    bool classCompilation)
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
        if (verbose_mdl()) std::cout << "  Messages after set_option(target_type):\n";
        print_context_messages(context);
        context->clear_messages();
    }

    // Compile the material instance
    // Instance compilation mode bakes all parameters into the compiled material
    // (which is what we want for path tracing — no runtime parameter changes)
    const mi::Uint32 flags = classCompilation
        ? mi::neuraylib::IMaterial_instance::CLASS_COMPILATION
        : mi::neuraylib::IMaterial_instance::DEFAULT_OPTIONS;

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

void MaterialCompiler::fold_class_parameters(
    mi::neuraylib::ITransaction* transaction,
    const std::string& instance_name,
    const CompileSpecification& specification,
    mi::neuraylib::IMdl_execution_context* context,
    CompiledMaterial& result)
{
    const auto& unfolded = result.compiled_material;
    mi::base::Handle<mi::IDynamic_array> names(
        transaction->create<mi::IDynamic_array>("String[]"));
    if (!unfolded || !names) return;
    std::vector<std::string> folded;
    // "fold_parameters" takes the names the unfolded class compile reports.
    for (mi::Size i = 0; i < unfolded->get_parameter_count(); ++i) {
        const char* name = unfolded->get_parameter_name(i);
        mi::base::Handle<const mi::neuraylib::IValue> value(unfolded->get_argument(i));
        if (!name || contains_resource(value.get()) ||
            specification.keptUniforms.contains(name))
            continue;
        mi::base::Handle<mi::IString> entry(transaction->create<mi::IString>("String"));
        if (!entry) return;
        entry->set_c_str(name);
        names->push_back(entry.get());
        folded.push_back(name);
    }
    if (folded.empty()) return;

    context->clear_messages();
    if (context->set_option("fold_parameters", names.get()) != 0 ||
        context->set_option("fold_ternary_on_df", true) != 0 ||
        context->set_option("fold_transparent_layers", true) != 0) {
        std::cerr << "[WARN] Cannot set MDL parameter folding options; keeping class compilation\n";
        print_context_messages(context);
        return;
    }
    mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
        compile_instance(transaction, instance_name, context, true));
    if (!compiled) {
        // The unfolded class result is still a correct program for this material.
        std::cerr << "[WARN] Folded class compilation failed; keeping class compilation\n";
        return;
    }
    if (verbose_mdl())
        std::cout << std::format("  Folded {} of {} class parameters; {} remain arguments\n",
            folded.size(), unfolded->get_parameter_count(), compiled->get_parameter_count());
    result.compiled_material = compiled;
    result.folded_parameters = std::move(folded);
}

// ========================================================================
// Full pipeline: GeneratedMDL -> CompiledMaterial
// ========================================================================
CompiledMaterial MaterialCompiler::compile(const GeneratedMDL& generated_mdl,
                                           const std::string& mtlx_path,
                                           const CompileSpecification& specification) {
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

    // Resolve the global scope now. The per-material transaction is created
    // after the canonical module is committed so it observes that definition.
    mi::base::Handle<mi::neuraylib::IScope> scope(
        m_ctx.get_database()->get_global_scope());
    if (!scope) { result.error_message = "Failed to get global scope"; return result; }

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
                if (verbose_mdl()) std::cout << "[OK] Added document resource path: " << doc_dir.string() << "\n";
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
                        if (verbose_mdl()) std::cout << "[OK] Added texture resource path: " << parent << "\n";
                        registered_dirs.insert(parent);
                    }
                }
            }
        }
    }

    // --- Step 4.2: Load MDL from string ---
    if (verbose_mdl()) std::cout << "\n--- Step 4.2: Loading MDL into SDK ---\n";

    // The module_name from GeneratedMDL is like "::materialx::test_material";
    // make it unique per source document.
    std::string module_name = make_unique_module_name(generated_mdl.module_name, mtlx_path);

    // Sanitize texture paths in the generated MDL source.
    // MaterialX fileprefix="./" produces paths like "./texture.jpg" in the MDL code,
    // but the MDL SDK rejects strict relative paths ("./") in string-based modules.
    //
    // We resolve them to absolute paths using the .mtlx document directory.
    // This is more robust than converting to root-relative paths ("/texture.jpg")
    // because it doesn't depend on the SDK's resource search path ordering --
    // the texture location is unambiguous regardless of when paths were registered.
    std::string mdl_source = generated_mdl.mdl_source;
    std::string doc_dir_prefix;
    if (!mtlx_path.empty()) {
        std::filesystem::path doc_dir = std::filesystem::path(mtlx_path).parent_path();
        if (!doc_dir.empty()) {
            std::string doc_dir_str = doc_dir.string();
            std::replace(doc_dir_str.begin(), doc_dir_str.end(), '\\', '/');
            doc_dir_prefix = doc_dir_str + "/";
        }
    }

    {
        std::string search = "\"./";
        std::string replace = doc_dir_prefix.empty() ? "\"/" : "\"" + doc_dir_prefix;
        size_t pos = 0;
        while ((pos = mdl_source.find(search, pos)) != std::string::npos) {
            mdl_source.replace(pos, search.length(), replace);
            pos += replace.length();
        }
        if (verbose_mdl() && !doc_dir_prefix.empty())
            std::cout << "[OK] Resolved relative texture paths to: " << doc_dir_prefix << "\n";
    }

    // Resource-free variants of the same generated graph share one committed
    // module definition. Each variant module is still loaded below solely to
    // obtain its authored defaults; its call is then recreated against this
    // canonical definition. The SDK consequently produces the same genuine
    // compiled-material hash with different argument values.
    std::string canonicalModuleName;
    const std::string structuralKey = generated_mdl.texture_paths.empty()
        ? canonical_parameter_key(mdl_source) : std::string{};
    if (!structuralKey.empty()) {
        canonicalModuleName = canonical_module_name(structuralKey);
        std::mutex& keyMutex = gCanonicalModuleMutexes[
            std::hash<std::string>{}(canonicalModuleName) % gCanonicalModuleMutexes.size()];
        std::lock_guard<std::mutex> keyLock(keyMutex);
        mi::base::Handle<mi::neuraylib::ITransaction> canonicalTransaction(
            scope->create_transaction());
        mi::base::Handle<mi::neuraylib::IMdl_execution_context> canonicalContext(
            m_ctx.get_factory()->create_execution_context());
        mi::base::Handle<const mi::IString> canonicalDbName(
            m_ctx.get_factory()->get_db_module_name(canonicalModuleName.c_str()));
        mi::base::Handle<const mi::neuraylib::IModule> existing(
            canonicalTransaction && canonicalDbName
                ? canonicalTransaction->access<mi::neuraylib::IModule>(
                    canonicalDbName->get_c_str()) : nullptr);
        if (!canonicalTransaction || !canonicalContext ||
            (!existing && !load_module_from_string(
                canonicalTransaction.get(), canonicalModuleName,
                mdl_source, canonicalContext.get()))) {
            if (canonicalTransaction) canonicalTransaction->abort();
            canonicalModuleName.clear();
        } else {
            existing.reset();
            canonicalDbName.reset();
            canonicalContext.reset();
            canonicalTransaction->commit();
        }
    }

    mi::base::Handle<mi::neuraylib::ITransaction> transaction(
        scope->create_transaction());
    if (!transaction) {
        result.error_message = "Failed to create transaction";
        return result;
    }

    // Optional diagnostics belong in the renderer cache, never beside source
    // assets where compilation would mutate a project checkout.
    if (verbose_mdl()) {
        std::string dump_name = module_name;
        if (dump_name.size() >= 2 && dump_name[0] == ':' && dump_name[1] == ':')
            dump_name = dump_name.substr(2);
        std::replace(dump_name.begin(), dump_name.end(), ':', '_');
        dump_name += ".mdl";

        const std::filesystem::path dump_path =
            prism::PersistentCacheRoot() / "generated-mdl" / "debug" / dump_name;
        std::error_code directoryError;
        std::filesystem::create_directories(dump_path.parent_path(),directoryError);

        std::ofstream f(dump_path);
        if (f) {
            f << mdl_source;
            if (verbose_mdl()) std::cout << "[DEBUG] Dumped generated MDL to: " << dump_path.string() << "\n";
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
    if (verbose_mdl()) std::cout << std::format("[OK] Module loaded as '{}'\n", result.module_db_name);

    // --- Step 4.3: Create material instance ---
    if (verbose_mdl()) std::cout << "\n--- Step 4.3: Creating Material Instance ---\n";

    result.instance_db_name = "material_instance_" +
        module_name.substr(module_name.rfind("::") + 2);  // extract simple name

    context->clear_messages();
    bool instanceCreated = false;
    if (!canonicalModuleName.empty()) {
        mi::base::Handle<const mi::IString> canonicalDbName(
            m_ctx.get_factory()->get_db_module_name(canonicalModuleName.c_str()));
        mi::base::Handle<const mi::neuraylib::IModule> variantModule(
            transaction->access<mi::neuraylib::IModule>(result.module_db_name.c_str()));
        mi::base::Handle<const mi::neuraylib::IModule> canonicalModule(
            canonicalDbName ? transaction->access<mi::neuraylib::IModule>(
                canonicalDbName->get_c_str()) : nullptr);
        if (variantModule && canonicalModule && variantModule->get_material_count() > 0 &&
            canonicalModule->get_material_count() > 0) {
            mi::base::Handle<const mi::neuraylib::IFunction_definition> variantDefinition(
                transaction->access<mi::neuraylib::IFunction_definition>(
                    variantModule->get_material(0)));
            mi::base::Handle<const mi::neuraylib::IFunction_definition> canonicalDefinition(
                transaction->access<mi::neuraylib::IFunction_definition>(
                    canonicalModule->get_material(0)));
            mi::base::Handle<mi::neuraylib::IExpression_factory> expressions(
                m_ctx.get_factory()->create_expression_factory(transaction.get()));
            mi::base::Handle<const mi::neuraylib::IExpression_list> variantDefaults(
                variantDefinition ? variantDefinition->get_defaults() : nullptr);
            mi::base::Handle<mi::neuraylib::IExpression_list> arguments(
                expressions && variantDefaults
                    ? expressions->clone(variantDefaults.get()) : nullptr);
            mi::Sint32 callResult = -1;
            mi::base::Handle<mi::neuraylib::IFunction_call> call(
                canonicalDefinition && arguments
                    ? canonicalDefinition->create_function_call(arguments.get(), &callResult)
                    : nullptr);
            if (call && callResult == 0 &&
                transaction->store(call.get(), result.instance_db_name.c_str()) == 0) {
                instanceCreated = true;
                result.module_db_name = canonicalDbName->get_c_str();
                result.definition_db_name = canonicalModule->get_material(0);
            }
        }
    }
    if (!instanceCreated)
        instanceCreated = create_material_instance(
            transaction.get(), result.module_db_name,
            result.instance_db_name, context.get());
    if (!instanceCreated) {
        result.error_message = "Failed to create material instance";
        transaction->commit();
        return result;
    }
    if (verbose_mdl()) std::cout << "[OK] Material instance created\n";

    // --- Step 4.4: Compile the material ---
    if (verbose_mdl()) std::cout << "\n--- Step 4.4: Compiling Material ---\n";

    context->clear_messages();
    result.compiled_material = compile_instance(
        transaction.get(), result.instance_db_name, context.get(), true);
    if (!result.compiled_material) {
        context->clear_messages();
        result.compiled_material = compile_instance(
            transaction.get(), result.instance_db_name, context.get(), false);
    } else if (specification.foldParameters) {
        // Folding needs the unfolded class compile above to enumerate the
        // parameter names and types it may fold.
        fold_class_parameters(transaction.get(), result.instance_db_name,
                              specification, context.get(), result);
    }

    if (!result.compiled_material) {
        result.error_message = "Failed to compile material instance";
        transaction->commit();
        return result;
    }

    if (verbose_mdl()) std::cout << "[OK] Material compiled successfully\n";

    // Print compiled material info
    mi::base::Uuid hash = result.compiled_material->get_hash();
    if (verbose_mdl()) {
        std::cout << std::format("  Hash: {:08x} {:08x} {:08x} {:08x}\n",
            hash.m_id1, hash.m_id2, hash.m_id3, hash.m_id4);
        std::cout << std::format("  Parameters: {}\n",
            result.compiled_material->get_parameter_count());
        std::cout << std::format("  Temporaries: {}\n",
            result.compiled_material->get_temporary_count());
    }

    // Keep the transaction alive — the compiled material references DB data.
    // The caller must call result.release() when done (after Phase 5).
    result.transaction = transaction;

    result.success = true;
    // Target code is keyed by the compiled class program. Resource-valued
    // arguments are rebound into an immutable per-argument-block runtime by
    // BackendCompiler, so textured instances can safely share native code.
    // A folded program serves only materials with the same folded constants;
    // keep one nobody else will request out of the bounded cache.
    result.allowTargetCodeReuse =
        result.folded_parameters.empty() || specification.shareProgram;
    return result;
}

} // namespace mdl
