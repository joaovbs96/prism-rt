#include "backend_compiler.h"

#include <format>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <mi/base/interface_implement.h>

namespace mdl {

namespace {
std::mutex gTargetCodeCacheMutex;
// Target-code handles depend on process-lifetime MDL SDK state in Hydra.
// Deliberately retain the cache so static destruction never releases SDK
// objects after INeuray shutdown in standalone clients.
auto& gTargetCodeCache =
    *new std::unordered_map<std::string, TargetCode>();
std::array<std::mutex, 64> gTargetCodeKeyMutexes;

bool VerboseMdl()
{
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string_view(value) != "0";
}

std::string UuidText(const mi::base::Uuid& hash)
{
    return std::format("{:08x}{:08x}{:08x}{:08x}",
        hash.m_id1, hash.m_id2, hash.m_id3, hash.m_id4);
}

std::string ClassProgramHash(
    const mi::neuraylib::ICompiled_material* material,
    mi::neuraylib::IMdl_factory* factory,
    mi::neuraylib::ITransaction* transaction)
{
    // Whole-material hashes retain the generated module identity. Combine all
    // executable slot hashes, whose SDK contract excludes argument values,
    // with the exact ordered argument interface used by the target block.
    std::string key = UuidText(material->get_hash());

    mi::base::Handle<mi::neuraylib::IType_factory> typeFactory(
        factory->create_type_factory(transaction));
    key += ":" + std::to_string(material->get_parameter_count());
    for (mi::Size i = 0; i < material->get_parameter_count(); ++i) {
        const char* name = material->get_parameter_name(i);
        mi::base::Handle<const mi::neuraylib::IValue> value(material->get_argument(i));
        mi::base::Handle<const mi::neuraylib::IType> type(
            value ? value->get_type() : nullptr);
        mi::base::Handle<const mi::IString> typeName(
            typeFactory && type ? typeFactory->get_mdl_type_name(type.get()) : nullptr);
        key += ":";
        key += name ? name : "";
        key += "=";
        key += typeName ? typeName->get_c_str() : "<unknown>";
    }
    return key;
}

class KnownResourceCallback final : public mi::base::Interface_implement<
    mi::neuraylib::ITarget_resource_callback> {
public:
    KnownResourceCallback(const mi::neuraylib::ITarget_code* code,
                          mi::neuraylib::ITransaction* transaction)
        : m_code(code), m_transaction(transaction) {}

    mi::Uint32 get_resource_index(
        const mi::neuraylib::IValue_resource* resource) override {
        return m_code->get_known_resource_index(m_transaction, resource);
    }
    mi::Uint32 get_string_index(
        const mi::neuraylib::IValue_string* value) override {
        const char* wanted = value ? value->get_value() : nullptr;
        if (!wanted) return 0;
        for (mi::Size i = 1; i < m_code->get_string_constant_count(); ++i) {
            const char* candidate = m_code->get_string_constant(i);
            if (candidate && std::string_view(candidate) == wanted)
                return static_cast<mi::Uint32>(i);
        }
        return 0;
    }
private:
    const mi::neuraylib::ITarget_code* m_code;
    mi::neuraylib::ITransaction* m_transaction;
};
}

void ClearTargetCodeCache()
{
    std::lock_guard<std::mutex> lock(gTargetCodeCacheMutex);
    gTargetCodeCache.clear();
}

BackendCompiler::BackendCompiler(Context& ctx)
    : m_ctx(ctx)
{}

void BackendCompiler::print_context_messages(mi::neuraylib::IMdl_execution_context* context) {
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

        if (VerboseMdl() || msg->get_severity() <= mi::base::MESSAGE_SEVERITY_WARNING)
            std::cerr << std::format("  [MDL {}] {}\n", severity_str, msg->get_string());
    }
}

TargetCode BackendCompiler::generate_target_code(const CompiledMaterial& compiled) {
    TargetCode result;
    result.success = false;

    if (!compiled.success || !compiled.compiled_material) {
        result.error_message = "Input CompiledMaterial is not valid";
        return result;
    }

    if (!compiled.transaction) {
        result.error_message = "CompiledMaterial transaction is null (already released?)";
        return result;
    }

    // ================================================================
    // Step 5.1: Get the native backend and configure it
    // ================================================================
    if (VerboseMdl()) std::cout << "\n--- Step 5.1: Acquiring Native Backend ---\n";

    mi::base::Handle<mi::neuraylib::IMdl_backend_api> backend_api(
        m_ctx.get_neuray()->get_api_component<mi::neuraylib::IMdl_backend_api>());

    if (!backend_api) {
        result.error_message = "Failed to get IMdl_backend_api";
        return result;
    }

    mi::base::Handle<mi::neuraylib::IMdl_backend> backend(
        backend_api->get_backend(mi::neuraylib::IMdl_backend_api::MB_NATIVE));

    if (!backend) {
        result.error_message = "Failed to get native backend";
        return result;
    }

    // Configure backend options
    backend->set_option("num_texture_spaces", "1");
    backend->set_option("num_texture_results", "128");
    backend->set_option("enable_auxiliary", "on");

    if (VerboseMdl()) std::cout << "[OK] Native backend acquired and configured\n";

    // ================================================================
    // Step 5.2: Create execution context and inspect compiled material
    // ================================================================
    if (VerboseMdl()) std::cout << "\n--- Step 5.2: Inspecting Compiled Material ---\n";

    mi::base::Handle<mi::neuraylib::IMdl_execution_context> context(
        m_ctx.get_factory()->create_execution_context());

    if (!context) {
        result.error_message = "Failed to create execution context";
        return result;
    }

    // Check if material is thin-walled (determines if backface expressions matter)
    // For instance compilation, we can look at the compiled material's thin_walled slot
    mi::base::Handle<const mi::neuraylib::IExpression> thin_walled_expr(
        compiled.compiled_material->lookup_sub_expression("thin_walled"));

    // For a simple check, we try to evaluate thin_walled as a constant
    result.is_thin_walled = false;
    if (thin_walled_expr) {
        mi::base::Handle<const mi::neuraylib::IExpression_constant> tw_const(
            thin_walled_expr->get_interface<const mi::neuraylib::IExpression_constant>());
        if (tw_const) {
            mi::base::Handle<const mi::neuraylib::IValue_bool> tw_val(
                tw_const->get_value<mi::neuraylib::IValue_bool>());
            if (tw_val) {
                result.is_thin_walled = tw_val->get_value();
            }
        }
    }

    if (VerboseMdl()) std::cout << std::format("  thin_walled: {}\n", result.is_thin_walled ? "true" : "false");

    const std::string materialHash = std::format("{}:{}",
        static_cast<const void*>(&m_ctx),
        ClassProgramHash(compiled.compiled_material.get(), m_ctx.get_factory(),
                         compiled.transaction.get()));
    if (VerboseMdl())
        std::cout << "  class cache key: " << materialHash
                  << " reusable=" << compiled.allowTargetCodeReuse << "\n";
    std::unique_lock<std::mutex> keyLock(
        gTargetCodeKeyMutexes[std::hash<std::string>{}(materialHash) %
                              gTargetCodeKeyMutexes.size()]);
    {
        std::lock_guard<std::mutex> lock(gTargetCodeCacheMutex);
        auto found = gTargetCodeCache.find(materialHash);
        if (found != gTargetCodeCache.end()) {
            TargetCode reused = found->second;
            if (VerboseMdl()) std::cout << "  class target-code cache hit\n";
            reused.cacheHit = true;
            reused.nativeJitSeconds = 0.0;
            reused.argument_block.reset();
            if (reused.target_code->get_argument_block_count() > 0) {
                mi::base::Handle<KnownResourceCallback> resources(
                    new KnownResourceCallback(reused.target_code.get(),
                                              compiled.transaction.get()));
                reused.argument_block = mi::base::Handle<const mi::neuraylib::ITarget_argument_block>(
                    reused.target_code->create_argument_block(
                        0, compiled.compiled_material.get(), resources.get()));
                if (!reused.argument_block) {
                    // Resource-valued arguments need a resource callback. Fall
                    // through and generate self-contained target code for them.
                    if (VerboseMdl()) std::cout << "  argument-block creation failed; regenerating\n";
                } else {
                    mi::neuraylib::Shading_state_material state{};
                    bool thin = false;
                    if (reused.target_code->execute(
                            reused.functions.thin_walled_function_index, state,
                            nullptr, reused.argument_data(), &thin) == 0)
                        reused.is_thin_walled = thin;
                    return reused;
                }
            } else {
                return reused;
            }
        }
    }

    // ================================================================
    // Step 5.3: Create link unit and add material sub-expressions
    // ================================================================
    if (VerboseMdl()) std::cout << "\n--- Step 5.3: Creating Link Unit ---\n";

    mi::base::Handle<mi::neuraylib::ILink_unit> link_unit(
        backend->create_link_unit(compiled.transaction.get(), context.get()));

    if (!link_unit) {
        result.error_message = "Failed to create link unit";
        print_context_messages(context.get());
        return result;
    }

    // Build the list of sub-expressions to compile.
    // Using single-init mode ("init" as first entry) so the init function
    // pre-computes shared values used by all subsequent functions.
    std::vector<mi::neuraylib::Target_function_description> descs;

    // Index 0: init (enables single-init mode)
    descs.push_back(mi::neuraylib::Target_function_description("init"));

    // Index 1: surface BSDF
    descs.push_back(mi::neuraylib::Target_function_description("surface.scattering"));

    // Index 2: surface EDF
    descs.push_back(mi::neuraylib::Target_function_description("surface.emission.emission"));

    // Index 3: surface emission intensity
    descs.push_back(mi::neuraylib::Target_function_description("surface.emission.intensity"));

    // Index 4: geometry.cutout_opacity
    descs.push_back(mi::neuraylib::Target_function_description("geometry.cutout_opacity"));

    // Index 5: thin_walled
    descs.push_back(mi::neuraylib::Target_function_description("thin_walled"));

    // Conditionally add backface expressions if the material is thin-walled
    // Class arguments may change thin_walled without changing the compiled
    // class hash. Generate both sides and select at runtime per argument block.
    bool has_backface = true;
    size_t backface_bsdf_idx = 0;
    size_t backface_edf_idx = 0;
    size_t backface_emission_intensity_idx = 0;

    if (has_backface) {
        backface_bsdf_idx = descs.size();
        descs.push_back(mi::neuraylib::Target_function_description("backface.scattering"));

        backface_edf_idx = descs.size();
        descs.push_back(mi::neuraylib::Target_function_description("backface.emission.emission"));

        backface_emission_intensity_idx = descs.size();
        descs.push_back(mi::neuraylib::Target_function_description("backface.emission.intensity"));
    }

    if (VerboseMdl()) std::cout << std::format("  Registering {} sub-expressions with link unit\n", descs.size());

    // Add the compiled material with all sub-expressions to the link unit
    mi::Sint32 add_result = link_unit->add_material(
        compiled.compiled_material.get(),
        descs.data(),
        descs.size(),
        context.get());

    if (add_result != 0) {
        result.error_message = std::format("add_material failed (code: {})", add_result);
        print_context_messages(context.get());

        // Print per-function return codes for debugging
        for (size_t i = 0; i < descs.size(); ++i) {
            if (descs[i].return_code != 0) {
                std::cerr << std::format("  desc[{}] '{}': return_code = {}\n",
                    i, descs[i].path, descs[i].return_code);
            }
        }
        return result;
    }

    // Store the function indices from the descs array
    result.functions.init_function_index = descs[0].function_index;
    result.functions.surface_bsdf_function_index = descs[1].function_index;
    result.functions.surface_edf_function_index = descs[2].function_index;
    result.functions.surface_emission_intensity_function_index = descs[3].function_index;
    result.functions.cutout_opacity_function_index = descs[4].function_index;
    result.functions.thin_walled_function_index = descs[5].function_index;

    if (has_backface) {
        result.functions.backface_bsdf_function_index = descs[backface_bsdf_idx].function_index;
        result.functions.backface_edf_function_index = descs[backface_edf_idx].function_index;
        result.functions.backface_emission_intensity_function_index =
            descs[backface_emission_intensity_idx].function_index;
    }

    if (VerboseMdl()) std::cout << "[OK] All sub-expressions added to link unit\n";

    // Print function indices
    if (VerboseMdl()) {
        std::cout << "  Function indices:\n";
        std::cout << std::format("    init:                  {}\n", static_cast<size_t>(result.functions.init_function_index));
        std::cout << std::format("    surface.scattering:    {}\n", static_cast<size_t>(result.functions.surface_bsdf_function_index));
        std::cout << std::format("    surface.emission:      {}\n", static_cast<size_t>(result.functions.surface_edf_function_index));
        std::cout << std::format("    emission intensity:    {}\n", static_cast<size_t>(result.functions.surface_emission_intensity_function_index));
        std::cout << std::format("    cutout_opacity:        {}\n", static_cast<size_t>(result.functions.cutout_opacity_function_index));
        std::cout << std::format("    thin_walled:           {}\n", static_cast<size_t>(result.functions.thin_walled_function_index));
    }

    // ================================================================
    // Step 5.4: Translate link unit to native target code
    // ================================================================
    if (VerboseMdl()) std::cout << "\n--- Step 5.4: Generating Native Code ---\n";

    context->clear_messages();
    const auto nativeJitStart = std::chrono::steady_clock::now();
    result.target_code = mi::base::Handle<const mi::neuraylib::ITarget_code>(
        backend->translate_link_unit(link_unit.get(), context.get()));
    result.nativeJitSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - nativeJitStart).count();

    if (!result.target_code) {
        result.error_message = "translate_link_unit failed";
        print_context_messages(context.get());
        return result;
    }

    if (result.target_code->get_argument_block_count() > 0)
        result.argument_block = mi::base::Handle<const mi::neuraylib::ITarget_argument_block>(
            result.target_code->get_argument_block(0));
    {
        mi::neuraylib::Shading_state_material state{};
        bool thin = result.is_thin_walled;
        if (result.target_code->execute(
                result.functions.thin_walled_function_index, state, nullptr,
                result.argument_data(), &thin) == 0)
            result.is_thin_walled = thin;
    }

    if (context->get_messages_count() > 0) {
        if (VerboseMdl()) std::cout << "  Messages from translate_link_unit:\n";
        print_context_messages(context.get());
    }

    if (VerboseMdl()) std::cout << "[OK] Native code generated\n";

    // Print target code info
    if (VerboseMdl()) {
        std::cout << std::format("  Code size:       {} bytes\n", static_cast<size_t>(result.target_code->get_code_size()));
        std::cout << std::format("  Callable functions: {}\n", static_cast<size_t>(result.target_code->get_callable_function_count()));
        std::cout << std::format("  Textures:        {}\n", static_cast<size_t>(result.target_code->get_texture_count()));
        std::cout << std::format("  RO data segments: {}\n", static_cast<size_t>(result.target_code->get_ro_data_segment_count()));
    }

    // List all callable functions
    for (mi::Size i = 0; i < result.target_code->get_callable_function_count(); ++i) {
        if (VerboseMdl()) std::cout << std::format("    function[{}]: {}\n",
            static_cast<size_t>(i), result.target_code->get_callable_function(i));
    }

    // Get read-only data segment if available
    if (result.target_code->get_ro_data_segment_count() > 0) {
        result.ro_data_segment = result.target_code->get_ro_data_segment_data(0);
        result.ro_data_segment_size = result.target_code->get_ro_data_segment_size(0);
        if (VerboseMdl()) std::cout << std::format("  RO data segment 0: {} bytes\n", static_cast<size_t>(result.ro_data_segment_size));
    }

    // List textures used by the material
    if (result.target_code->get_texture_count() > 1) {
        if (VerboseMdl()) std::cout << "  Textures used:\n";
        // Index 0 is always the invalid texture, start from 1
        for (mi::Size i = 1; i < result.target_code->get_texture_count(); ++i) {
            if (VerboseMdl()) std::cout << std::format("    texture[{}]: {}\n",
                static_cast<size_t>(i), result.target_code->get_texture(i));
        }
    }

    result.success = true;
    {
        std::lock_guard<std::mutex> lock(gTargetCodeCacheMutex);
        if (compiled.allowTargetCodeReuse) {
            TargetCode cached = result;
            cached.argument_block.reset();
            gTargetCodeCache.emplace(materialHash, std::move(cached));
            if (VerboseMdl()) std::cout << "  cached class target code\n";
        }
    }
    return result;
}

} // namespace mdl
