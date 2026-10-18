#include "backend_compiler.h"
#include "texture_runtime.h"

#include <format>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <mi/base/interface_implement.h>

namespace mdl {

mi::neuraylib::Texture_handler_base* TargetCode::texture_handler() const
{ return binding && binding->texture_runtime ? binding->texture_runtime->handler() : nullptr; }

namespace {
std::mutex gTargetCodeCacheMutex;
// Target-code handles depend on process-lifetime MDL SDK state in Hydra.
// Deliberately retain the cache so static destruction never releases SDK
// objects after INeuray shutdown in standalone clients.
auto& gTargetCodeCache =
    *new std::unordered_map<std::string, TargetCode>();
struct ProgramJob {
    std::condition_variable ready;
    bool done = false;
    std::string binding_signature;
    std::string failure;
};
auto& gProgramJobs = *new std::unordered_map<std::string, std::shared_ptr<ProgramJob>>;
auto& gCapabilities = *new std::unordered_map<std::string, std::string>;
// Wakes waiters on every exit, including SDK failures and exceptions. No SDK
// call or resource preparation executes while the cache mutex is held.
struct ProgramJobOwner {
    std::string key;
    std::shared_ptr<ProgramJob> job;
    TargetCode* result = nullptr;
    ~ProgramJobOwner() {
        if (!job) return;
        std::lock_guard lock(gTargetCodeCacheMutex);
        if (!result || !result->success)
            job->failure = result && !result->error_message.empty()
                ? result->error_message : "native program preparation failed";
        job->done = true;
        gProgramJobs.erase(key);
        job->ready.notify_all();
    }
};

struct ResourceRequirements {
    std::string signature, capabilities, unsupported;
    std::vector<mi::base::Handle<const mi::neuraylib::IValue_resource>> arguments;
};
void CaptureResources(const mi::neuraylib::IValue* value,
    mi::neuraylib::IValue_factory* factory, ResourceRequirements& result)
{
    if (!value) return;
    mi::base::Handle<const mi::neuraylib::IValue_resource> resource(
        value->get_interface<mi::neuraylib::IValue_resource>());
    if (resource) {
        result.arguments.push_back(resource);
        mi::base::Handle<const mi::IString> text(factory->dump(value, nullptr));
        const std::string item = text ? text->get_c_str() : "";
        result.signature += std::to_string(item.size()) + ":" + item;
        mi::base::Handle<const mi::neuraylib::IValue_texture> texture(
            value->get_interface<mi::neuraylib::IValue_texture>());
        if (texture) {
            mi::base::Handle<const mi::neuraylib::IType_texture> type(texture->get_type());
            const char* selector = texture->get_selector();
            if (!type) {
                result.unsupported = "texture argument has no texture type";
                return;
            }
            result.capabilities += ":texture:" + std::to_string(type->get_shape());
            result.capabilities += selector && *selector ? ":selector" : ":plain";
            if (selector && *selector) result.unsupported = "texture selectors require SDK resource handler";
            if (type->get_shape() != mi::neuraylib::IType_texture::TS_2D)
                result.unsupported = "authored non-2D texture requires SDK resource handler";
        } else {
            result.capabilities += ":resource:" + std::to_string(value->get_kind());
            result.unsupported = "light profile or measured BSDF requires SDK resource handler";
        }
        return;
    }
    // String table indices are fixed by the program as well.
    if (value->get_kind() == mi::neuraylib::IValue::VK_STRING) {
        mi::base::Handle<const mi::IString> text(factory->dump(value, nullptr));
        const std::string item = text ? text->get_c_str() : "";
        result.signature += std::to_string(item.size()) + ":" + item;
        // Unlike texture arguments, native string constants cannot be
        // appended by the custom runtime. Make the ordered string table part
        // of the program interface so a changed value never reaches the
        // rejection path after a cache hit (which would also bypass the
        // singleflight owner for the replacement program).
        result.capabilities += ":string:" + std::to_string(item.size()) + ":" + item;
    }
    mi::base::Handle<const mi::neuraylib::IValue_compound> compound(
        value->get_interface<mi::neuraylib::IValue_compound>());
    if (compound) for (mi::Size i=0; i<compound->get_size(); ++i) {
        mi::base::Handle<const mi::neuraylib::IValue> child(compound->get_value(i));
        CaptureResources(child.get(), factory, result);
    }
}

// Only target-layout limitations are reusable across bindings. Failures while
// opening or preparing an argument resource are value-specific and must never
// force later bindings of the same class onto the SDK handler.
bool IsProgramCapabilityFailure(std::string_view reason)
{
    return reason.starts_with("unsupported texture shape") ||
        reason.starts_with("light-profile resource") ||
        reason.starts_with("measured-BSDF resource") ||
        reason.starts_with("BSDF data resource");
}

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
    // The compiled class hash retains generated-module identity and excludes
    // argument values. Combine it with the exact ordered argument interface
    // used by the target block.
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

// True if an expression of a compiled material depends only on its arguments:
// constants, parameters and calls of uniform functions (state and texture
// functions are varying, as are functions using them).
bool IsUniformExpression(const mi::neuraylib::IExpression* expression,
                         const mi::neuraylib::ICompiled_material* material,
                         mi::neuraylib::ITransaction* transaction, int depth = 0)
{
    if (!expression || depth > 256) return false;
    using E = mi::neuraylib::IExpression;
    switch (expression->get_kind()) {
    case E::EK_CONSTANT:
    case E::EK_PARAMETER:
        return true;
    case E::EK_TEMPORARY: {
        mi::base::Handle<const mi::neuraylib::IExpression_temporary> temporary(
            expression->get_interface<mi::neuraylib::IExpression_temporary>());
        mi::base::Handle<const E> value(material->get_temporary(temporary->get_index()));
        return IsUniformExpression(value.get(), material, transaction, depth + 1);
    }
    case E::EK_DIRECT_CALL: {
        mi::base::Handle<const mi::neuraylib::IExpression_direct_call> call(
            expression->get_interface<mi::neuraylib::IExpression_direct_call>());
        mi::base::Handle<const mi::neuraylib::IFunction_definition> definition(
            transaction->access<mi::neuraylib::IFunction_definition>(call->get_definition()));
        if (!definition || !definition->is_uniform()) return false;
        mi::base::Handle<const mi::neuraylib::IExpression_list> arguments(call->get_arguments());
        for (mi::Size i = 0; arguments && i < arguments->get_size(); ++i) {
            mi::base::Handle<const E> argument(arguments->get_expression(i));
            if (!IsUniformExpression(argument.get(), material, transaction, depth + 1)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

// A diffuse EDF with uniform arguments (or none at all): its radiance does
// not depend on the shading point or the direction.
bool IsUniformDiffuseEdf(const mi::neuraylib::IExpression* expression,
                         const mi::neuraylib::ICompiled_material* material,
                         mi::neuraylib::ITransaction* transaction, int depth = 0)
{
    if (!expression || depth > 256) return false;
    using E = mi::neuraylib::IExpression;
    switch (expression->get_kind()) {
    case E::EK_CONSTANT:
        return true; // no EDF
    case E::EK_TEMPORARY: {
        mi::base::Handle<const mi::neuraylib::IExpression_temporary> temporary(
            expression->get_interface<mi::neuraylib::IExpression_temporary>());
        mi::base::Handle<const E> value(material->get_temporary(temporary->get_index()));
        return IsUniformDiffuseEdf(value.get(), material, transaction, depth + 1);
    }
    case E::EK_DIRECT_CALL: {
        mi::base::Handle<const mi::neuraylib::IExpression_direct_call> call(
            expression->get_interface<mi::neuraylib::IExpression_direct_call>());
        mi::base::Handle<const mi::neuraylib::IFunction_definition> definition(
            transaction->access<mi::neuraylib::IFunction_definition>(call->get_definition()));
        if (!definition ||
            definition->get_semantic() != mi::neuraylib::IFunction_definition::DS_INTRINSIC_DF_DIFFUSE_EDF)
            return false;
        mi::base::Handle<const mi::neuraylib::IExpression_list> arguments(call->get_arguments());
        for (mi::Size i = 0; arguments && i < arguments->get_size(); ++i) {
            mi::base::Handle<const E> argument(arguments->get_expression(i));
            if (!IsUniformExpression(argument.get(), material, transaction, depth + 1)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

class KnownResourceCallback final : public mi::base::Interface_implement<
    mi::neuraylib::ITarget_resource_callback> {
public:
    KnownResourceCallback(const mi::neuraylib::ITarget_code* code,
                          mi::neuraylib::ITransaction* transaction,
                          std::shared_ptr<NativeTextureRuntime> runtime = {})
        : m_code(code), m_transaction(transaction), m_runtime(std::move(runtime)) {}

    mi::Uint32 get_resource_index(
        const mi::neuraylib::IValue_resource* resource) override {
        auto index=m_code->get_known_resource_index(m_transaction, resource);
        if(index) return index;
        if(!m_runtime) {
            if(resource && resource->get_value()) m_failed=true;
            return 0;
        }
        index=m_runtime->registerTexture(resource,m_transaction);
        if(!index && resource && resource->get_value()) m_failed=true;
        return index;
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
        if (*wanted) m_failed=true;
        return 0;
    }
    bool failed() const { return m_failed; }
private:
    const mi::neuraylib::ITarget_code* m_code;
    mi::neuraylib::ITransaction* m_transaction;
    std::shared_ptr<NativeTextureRuntime> m_runtime;
    bool m_failed=false;
};

// Keep the argument bytes in an application-owned interface, decoupled from
// the lifetime of the SDK interface returned while compiling this variant.
class OwnedArgumentBlock final : public mi::base::Interface_implement<
    mi::neuraylib::ITarget_argument_block> {
public:
    explicit OwnedArgumentBlock(
        mi::neuraylib::ITarget_argument_block const *source)
        : m_data(source ? source->get_size() : 0)
    {
        if (source && !m_data.empty())
            std::memcpy(m_data.data(), source->get_data(), m_data.size());
    }
    char const *get_data() const override { return m_data.data(); }
    char *get_data() override { return m_data.data(); }
    mi::Size get_size() const override { return m_data.size(); }
    mi::neuraylib::ITarget_argument_block *clone() const override {
        auto *copy = new OwnedArgumentBlock(nullptr);
        copy->m_data = m_data;
        return copy;
    }
private:
    std::vector<char> m_data;
};
}

void ClearTargetCodeCache(const Context* context)
{
    std::lock_guard<std::mutex> lock(gTargetCodeCacheMutex);
    if (!context) {
        gTargetCodeCache.clear();
        gCapabilities.clear();
        return;
    }
    const std::string prefix = std::format("{}:", static_cast<const void*>(context));
    for (auto it = gCapabilities.begin(); it != gCapabilities.end();) {
        if (it->first.starts_with(prefix)) it = gCapabilities.erase(it);
        else ++it;
    }
    for (auto it = gTargetCodeCache.begin(); it != gTargetCodeCache.end();) {
        if (it->first.starts_with(prefix)) it = gTargetCodeCache.erase(it);
        else ++it;
    }
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
    auto program = std::make_shared<CompiledProgram>();
    program->owner_identity=&m_ctx;
    auto binding = std::make_shared<MaterialBinding>();
    binding->program = program;
    result.program = program;
    result.binding = binding;
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
    backend->set_option("enable_auxiliary", "off");
    backend->set_option("texture_runtime_with_derivs", "on");
    backend->set_option("use_builtin_resource_handler", "off");

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
    binding->is_thin_walled = false;
    if (thin_walled_expr) {
        mi::base::Handle<const mi::neuraylib::IExpression_constant> tw_const(
            thin_walled_expr->get_interface<const mi::neuraylib::IExpression_constant>());
        if (tw_const) {
            mi::base::Handle<const mi::neuraylib::IValue_bool> tw_val(
                tw_const->get_value<mi::neuraylib::IValue_bool>());
            if (tw_val) {
                binding->is_thin_walled = tw_val->get_value();
            }
        }
    }
    mi::base::Handle<const mi::neuraylib::IExpression> cutoutExpr(
        compiled.compiled_material->lookup_sub_expression("geometry.cutout_opacity"));
    if (cutoutExpr) {
        mi::base::Handle<const mi::neuraylib::IExpression_constant> constant(
            cutoutExpr->get_interface<const mi::neuraylib::IExpression_constant>());
        if (constant) {
            mi::base::Handle<const mi::neuraylib::IValue_float> value(
                constant->get_value<mi::neuraylib::IValue_float>());
            if (value && std::isfinite(value->get_value()))
                binding->constant_cutout_opacity=std::clamp(value->get_value(),0.0f,1.0f);
        }
    }

    if (VerboseMdl()) std::cout << std::format("  thin_walled: {}\n", binding->is_thin_walled ? "true" : "false");
    const bool uniformCutout = cutoutExpr && !binding->constant_cutout_opacity &&
        IsUniformExpression(cutoutExpr.get(), compiled.compiled_material.get(), compiled.transaction.get());

    // Emission that is the same everywhere: emissive triangles become lights.
    mi::base::Handle<const mi::neuraylib::IExpression> emissionIntensity(
        compiled.compiled_material->lookup_sub_expression("surface.emission.intensity"));
    mi::base::Handle<const mi::neuraylib::IExpression> emissionEdf(
        compiled.compiled_material->lookup_sub_expression("surface.emission.emission"));
    mi::base::Handle<const mi::neuraylib::IExpression> emissionMode(
        compiled.compiled_material->lookup_sub_expression("surface.emission.mode"));
    const bool uniformEmission = emissionIntensity && emissionEdf &&
        IsUniformExpression(emissionIntensity.get(), compiled.compiled_material.get(), compiled.transaction.get()) &&
        IsUniformDiffuseEdf(emissionEdf.get(), compiled.compiled_material.get(), compiled.transaction.get()) &&
        (!emissionMode || IsUniformExpression(emissionMode.get(), compiled.compiled_material.get(),
                                              compiled.transaction.get()));

    const std::string materialHash = std::format("{}:{}",
        static_cast<const void*>(&m_ctx),
        ClassProgramHash(compiled.compiled_material.get(), m_ctx.get_factory(),
                         compiled.transaction.get())) +
        ":native-deriv-textures-v3:mdl-" MI_NEURAYLIB_PRODUCT_VERSION_STRING
        ":callables-init-bsdf-edf-cutout-thin-v2:spaces-1:results-128";
    if (VerboseMdl())
        std::cout << "  class cache key: " << materialHash
                  << " reusable=" << compiled.allowTargetCodeReuse << "\n";
    ResourceRequirements requirements;
    mi::base::Handle<mi::neuraylib::IValue_factory> values(
        m_ctx.get_factory()->create_value_factory(compiled.transaction.get()));
    for (mi::Size i=0; i<compiled.compiled_material->get_parameter_count(); ++i) {
        mi::base::Handle<const mi::neuraylib::IValue> value(
            compiled.compiled_material->get_argument(i));
        CaptureResources(value.get(), values.get(), requirements);
    }
    {
        mi::base::Handle<mi::neuraylib::IType_factory> types(
            m_ctx.get_factory()->create_type_factory(compiled.transaction.get()));
        program->parameters.reserve(compiled.compiled_material->get_parameter_count());
        for(mi::Size i=0;i<compiled.compiled_material->get_parameter_count();++i) {
            const char* name=compiled.compiled_material->get_parameter_name(i);
            mi::base::Handle<const mi::neuraylib::IValue> value(
                compiled.compiled_material->get_argument(i));
            mi::base::Handle<const mi::neuraylib::IType> type(value?value->get_type():nullptr);
            mi::base::Handle<const mi::IString> typeName(
                types&&type?types->get_mdl_type_name(type.get()):nullptr);
            program->parameters.push_back({name?name:"",typeName?typeName->get_c_str():""});
        }
    }
    const std::string capabilityKey = materialHash + requirements.capabilities;
    const char* modeValue = std::getenv("PRISM_MDL_RESOURCE_MODE");
    const std::string mode = modeValue ? modeValue : "auto";
    if (mode != "auto" && mode != "custom" && mode != "builtin") {
        result.error_message = "PRISM_MDL_RESOURCE_MODE must be auto, custom, or builtin";
        return result;
    }
    bool builtin = mode == "builtin";
    NativeTextureRuntime::ArgumentResourcePreflight preflight;
    std::string argumentResourceFailure;
    if (!builtin && requirements.unsupported.empty()) {
        for (const auto& resource : requirements.arguments) {
            if (!NativeTextureRuntime::PreflightArgumentResource(
                    resource.get(), compiled.transaction.get(), preflight,
                    &argumentResourceFailure))
                break;
        }
        if (!argumentResourceFailure.empty() && mode == "custom") {
            result.textureFallbackReason = argumentResourceFailure;
            result.error_message = "forced custom runtime unsupported: " + argumentResourceFailure;
            return result;
        }
    }
    std::string cacheKey;
    std::optional<TargetCode> cachedTarget;
    ProgramJobOwner jobOwner;
    jobOwner.result = &result;
    for (;;) {
        std::unique_lock lock(gTargetCodeCacheMutex);
        auto capability = gCapabilities.find(capabilityKey);
        if (mode == "auto") {
            result.textureFallbackReason = !requirements.unsupported.empty()
                ? requirements.unsupported : capability != gCapabilities.end()
                ? capability->second : argumentResourceFailure;
            builtin = !result.textureFallbackReason.empty();
        }
        cacheKey = materialHash + (builtin ? ":builtin:" + requirements.signature
                                           : ":custom:" + requirements.capabilities +
                                             (mode=="custom" ? ":"+requirements.signature : ""));
        if (!compiled.allowTargetCodeReuse) break;
        auto found = gTargetCodeCache.find(cacheKey);
        if (found != gTargetCodeCache.end()) { cachedTarget = found->second; break; }
        auto pending = gProgramJobs.find(cacheKey);
        if (pending == gProgramJobs.end()) {
            jobOwner.key = cacheKey;
            jobOwner.job = std::make_shared<ProgramJob>();
            jobOwner.job->binding_signature=requirements.signature;
            gProgramJobs.emplace(cacheKey, jobOwner.job);
            break;
        }
        auto job = pending->second;
        const auto start = std::chrono::steady_clock::now();
        job->ready.wait(lock, [&] { return job->done; });
        result.programWaitSeconds += std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        if (!job->failure.empty() && job->binding_signature==requirements.signature) {
            result.error_message = job->failure; return result;
        }
    }
    backend->set_option("use_builtin_resource_handler", builtin ? "on" : "off");
    if(cachedTarget) {
            TargetCode reused = std::move(*cachedTarget);
            auto reusedBinding = std::make_shared<MaterialBinding>();
            reusedBinding->program = reused.program;
            reusedBinding->constant_cutout_opacity = binding->constant_cutout_opacity;
            reusedBinding->is_thin_walled = binding->is_thin_walled;
            std::shared_ptr<NativeTextureRuntime> mutableRuntime;
            if (reused.program->resource_interface != requirements.capabilities) {
                // Defensive guard for cache entries produced by an older or
                // colliding resource interface. The key normally prevents it.
                result.cacheReuseRejected = true;
                cachedTarget.reset();
            } else {
                if (reused.program->resource_prefix)
                    mutableRuntime = reused.program->resource_prefix->clone();
                reusedBinding->texture_runtime = mutableRuntime;
            }
            if (!cachedTarget) {
                // Continue below and publish a program with the exact current
                // resource prefix rather than binding against a mismatched ABI.
            } else {
            reused.binding = reusedBinding;
            reused.translationCalls = reused.fallbackTranslations = 0;
            // Each argument block gets its own immutable extension of the
            // cached target's resource table. Published materials therefore
            // share code without sharing mutable texture-index state.
            if (VerboseMdl()) std::cout << "  class target-code cache hit\n";
            reused.cacheHit = true;
            reused.nativeJitSeconds = 0.0;
            reused.programWaitSeconds = result.programWaitSeconds;
            reusedBinding->argument_block.reset();
            if (reused.program->target_code->get_argument_block_count() > 0) {
                mi::base::Handle<KnownResourceCallback> resources(
                    new KnownResourceCallback(reused.program->target_code.get(),
                                              compiled.transaction.get(),mutableRuntime));
                mi::base::Handle<const mi::neuraylib::ITarget_argument_block> sdkBlock(
                    reused.program->target_code->create_argument_block(
                        0, compiled.compiled_material.get(), resources.get()));
                if (!sdkBlock || resources->failed()) {
                    // Resource-valued arguments need a resource callback. Fall
                    // through and generate self-contained target code for them.
                    result.cacheReuseRejected = true;
                    if (VerboseMdl()) std::cout << "  argument-block creation failed; regenerating\n";
                } else {
                    reusedBinding->argument_block = mi::base::Handle<
                        const mi::neuraylib::ITarget_argument_block>(
                            new OwnedArgumentBlock(sdkBlock.get()));
                    sdkBlock.reset();
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
    program->functions.init_function_index = descs[0].function_index;
    program->functions.surface_bsdf_function_index = descs[1].function_index;
    program->functions.surface_edf_function_index = descs[2].function_index;
    program->functions.surface_emission_intensity_function_index = descs[3].function_index;
    program->functions.cutout_opacity_function_index = descs[4].function_index;
    program->functions.thin_walled_function_index = descs[5].function_index;

    if (has_backface) {
        program->functions.backface_bsdf_function_index = descs[backface_bsdf_idx].function_index;
        program->functions.backface_edf_function_index = descs[backface_edf_idx].function_index;
        program->functions.backface_emission_intensity_function_index =
            descs[backface_emission_intensity_idx].function_index;
    }

    if (VerboseMdl()) std::cout << "[OK] All sub-expressions added to link unit\n";

    // Print function indices
    if (VerboseMdl()) {
        std::cout << "  Function indices:\n";
        std::cout << std::format("    init:                  {}\n", static_cast<size_t>(program->functions.init_function_index));
        std::cout << std::format("    surface.scattering:    {}\n", static_cast<size_t>(program->functions.surface_bsdf_function_index));
        std::cout << std::format("    surface.emission:      {}\n", static_cast<size_t>(program->functions.surface_edf_function_index));
        std::cout << std::format("    emission intensity:    {}\n", static_cast<size_t>(program->functions.surface_emission_intensity_function_index));
        std::cout << std::format("    cutout_opacity:        {}\n", static_cast<size_t>(program->functions.cutout_opacity_function_index));
        std::cout << std::format("    thin_walled:           {}\n", static_cast<size_t>(program->functions.thin_walled_function_index));
    }

    // ================================================================
    // Step 5.4: Translate link unit to native target code
    // ================================================================
    if (VerboseMdl()) std::cout << "\n--- Step 5.4: Generating Native Code ---\n";

    context->clear_messages();
    const auto nativeJitStart = std::chrono::steady_clock::now();
    ++result.translationCalls;
    if (builtin) ++result.fallbackTranslations;
    program->target_code = mi::base::Handle<const mi::neuraylib::ITarget_code>(
        backend->translate_link_unit(link_unit.get(), context.get()));
    result.nativeJitSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - nativeJitStart).count();

    if (!program->target_code) {
        result.error_message = "translate_link_unit failed";
        print_context_messages(context.get());
        return result;
    }

    // Translation is complete. Release the link unit before resolving target
    // resources through the same transaction; the native backend may retain
    // its transaction/database lock for the link unit lifetime.
    link_unit.reset();

    std::shared_ptr<NativeTextureRuntime> mutableRuntime;
    if (!builtin) try {
        mutableRuntime = NativeTextureRuntime::Create(
            program->target_code.get(), compiled.transaction.get(),
            &result.textureFallbackReason);
        binding->texture_runtime = mutableRuntime;
    } catch (std::exception const &error) {
        binding->texture_runtime.reset();
        result.textureFallbackReason=error.what();
    }
    if (!builtin && !binding->texture_runtime) {
        if (mode == "custom") {
            result.error_message = "forced custom runtime unsupported: " + result.textureFallbackReason;
            return result;
        }
        {
            std::lock_guard lock(gTargetCodeCacheMutex);
            if (IsProgramCapabilityFailure(result.textureFallbackReason))
                gCapabilities[capabilityKey] = result.textureFallbackReason;
        }
        cacheKey = materialHash + ":builtin:" + requirements.signature;
        std::cerr << "PrismRT: MDL target uses SDK texture fallback: "
                  << result.textureFallbackReason << '\n';
        // Unsupported resources retain SDK behavior as one coherent target;
        // custom and built-in resource handling cannot be mixed per lookup.
        backend->set_option("use_builtin_resource_handler", "on");
        link_unit = mi::base::Handle<mi::neuraylib::ILink_unit>(
            backend->create_link_unit(compiled.transaction.get(), context.get()));
        if (!link_unit || link_unit->add_material(compiled.compiled_material.get(),
                descs.data(), descs.size(), context.get()) != 0) {
            result.error_message = "builtin-resource fallback link unit failed";
            return result;
        }
        const auto fallbackStart=std::chrono::steady_clock::now();
        ++result.translationCalls;
        ++result.fallbackTranslations;
        program->target_code = mi::base::Handle<const mi::neuraylib::ITarget_code>(
            backend->translate_link_unit(link_unit.get(), context.get()));
        result.nativeJitSeconds+=std::chrono::duration<double>(
            std::chrono::steady_clock::now()-fallbackStart).count();
        if (!program->target_code) {
            result.error_message = "builtin-resource fallback translation failed";
            return result;
        }
    }

    link_unit.reset();
    if (program->target_code->get_argument_block_count() > 0)
        binding->argument_block = mi::base::Handle<const mi::neuraylib::ITarget_argument_block>(
            program->target_code->get_argument_block(0));

    if (context->get_messages_count() > 0) {
        if (VerboseMdl()) std::cout << "  Messages from translate_link_unit:\n";
        print_context_messages(context.get());
    }

    if (VerboseMdl()) std::cout << "[OK] Native code generated\n";

    // Print target code info
    if (VerboseMdl()) {
        std::cout << std::format("  Code size:       {} bytes\n", static_cast<size_t>(program->target_code->get_code_size()));
        std::cout << std::format("  Callable functions: {}\n", static_cast<size_t>(program->target_code->get_callable_function_count()));
        std::cout << std::format("  Textures:        {}\n", static_cast<size_t>(program->target_code->get_texture_count()));
        std::cout << std::format("  RO data segments: {}\n", static_cast<size_t>(program->target_code->get_ro_data_segment_count()));
    }

    // List all callable functions
    for (mi::Size i = 0; i < program->target_code->get_callable_function_count(); ++i) {
        if (VerboseMdl()) std::cout << std::format("    function[{}]: {}\n",
            static_cast<size_t>(i), program->target_code->get_callable_function(i));
    }

    // Get read-only data segment if available
    if (program->target_code->get_ro_data_segment_count() > 0) {
        program->ro_data_segment = program->target_code->get_ro_data_segment_data(0);
        program->ro_data_segment_size = program->target_code->get_ro_data_segment_size(0);
        if (VerboseMdl()) std::cout << std::format("  RO data segment 0: {} bytes\n", static_cast<size_t>(program->ro_data_segment_size));
    }

    // List textures used by the material
    if (program->target_code->get_texture_count() > 1) {
        if (VerboseMdl()) std::cout << "  Textures used:\n";
        // Index 0 is always the invalid texture, start from 1
        for (mi::Size i = 1; i < program->target_code->get_texture_count(); ++i) {
            if (VerboseMdl()) std::cout << std::format("    texture[{}]: {}\n",
                static_cast<size_t>(i), program->target_code->get_texture(i));
        }
    }

    program->resource_prefix = binding->texture_runtime;
    program->resource_interface = requirements.capabilities;
    {
        // Store the class for GPU devices. The caller releases its handle and
        // commits right after target generation, so it is not used again.
        static std::atomic<uint64_t> storedClasses{0};
        std::string name = std::format("prism::gpu_class::{}", ++storedClasses);
        if (compiled.transaction->store(const_cast<mi::neuraylib::ICompiled_material*>(
                compiled.compiled_material.get()), name.c_str()) == 0)
            program->compiled_material_name = std::move(name);
    }
    program->uniform_cutout = uniformCutout;
    program->uniform_emission = uniformEmission;
    program->builtin_resources = !binding->texture_runtime;
    program->identity = cacheKey;
    result.success = true;
    {
        std::lock_guard<std::mutex> lock(gTargetCodeCacheMutex);
        if (compiled.allowTargetCodeReuse) {
            TargetCode cached = result;
            cached.binding.reset();
            cached.cacheReuseRejected = false;
            constexpr size_t maxCachedPrograms=128;
            if (gTargetCodeCache.size()>=maxCachedPrograms &&
                !gTargetCodeCache.contains(cacheKey))
                gTargetCodeCache.erase(gTargetCodeCache.begin());
            // TargetCode's argument block and texture handler refer into its
            // target code. Replacing an existing mapped value via assignment
            // releases members in declaration order (target code first),
            // violating that lifetime dependency. Erase uses destruction
            // order, then construct the replacement in an empty slot.
            gTargetCodeCache.erase(cacheKey);
            gTargetCodeCache.emplace(cacheKey,std::move(cached));
            if (VerboseMdl()) std::cout << "  cached class target code\n";
        }
    }
    return result;
}

TargetCode BackendCompiler::bind_arguments(const TargetCode& prototype,
    const mi::neuraylib::IValue_list* overrides,
    mi::neuraylib::ITransaction* transaction)
{
    TargetCode result;
    if(!prototype.success || !prototype.program || !prototype.binding || !transaction ||
       prototype.program->owner_identity!=&m_ctx) {
        result.error_message="invalid prototype binding or transaction";return result;
    }
    result.program=prototype.program;
    result.textureFallbackReason=prototype.textureFallbackReason;
    auto binding=std::make_shared<MaterialBinding>();
    binding->program=prototype.program;
    binding->is_thin_walled=prototype.binding->is_thin_walled;
    binding->constant_cutout_opacity=prototype.binding->constant_cutout_opacity;
    if(!overrides || overrides->get_size()==0) {
        binding->argument_block=prototype.binding->argument_block;
        binding->texture_runtime=prototype.binding->texture_runtime;
        result.binding=std::move(binding);result.success=true;result.cacheHit=true;return result;
    }
    if(!prototype.binding->argument_block ||
       prototype.program->target_code->get_argument_block_count()==0) {
        result.error_message="prototype has no class argument block";return result;
    }
    mi::base::Handle<const mi::neuraylib::ITarget_value_layout> layout(
        prototype.program->target_code->get_argument_block_layout(0));
    if(!layout || layout->get_num_elements()!=prototype.program->parameters.size()) {
        result.error_message="prototype argument layout does not match published parameters";
        return result;
    }
    mi::base::Handle<OwnedArgumentBlock> block(
        new OwnedArgumentBlock(prototype.binding->argument_block.get()));
    std::shared_ptr<NativeTextureRuntime> mutableRuntime;
    if(prototype.binding->texture_runtime)
        mutableRuntime=prototype.binding->texture_runtime->clone();
    mi::base::Handle<KnownResourceCallback> resources(new KnownResourceCallback(
        prototype.program->target_code.get(),transaction,mutableRuntime));
    mi::base::Handle<mi::neuraylib::IType_factory> types(
        m_ctx.get_factory()->create_type_factory(transaction));
    for(mi::Size i=0;i<overrides->get_size();++i) {
        const char* name=overrides->get_name(i);
        mi::base::Handle<const mi::neuraylib::IValue> value(overrides->get_value(i));
        if(!name||!value){result.error_message="null argument override";return result;}
        auto found=std::find_if(prototype.program->parameters.begin(),
            prototype.program->parameters.end(),[&](auto const& p){return p.name==name;});
        if(found==prototype.program->parameters.end()) {
            result.error_message="unknown argument override: "+std::string(name);return result;
        }
        mi::base::Handle<const mi::neuraylib::IType> type(value->get_type());
        mi::base::Handle<const mi::IString> typeName(
            types&&type?types->get_mdl_type_name(type.get()):nullptr);
        if(!typeName || found->type_name!=typeName->get_c_str()) {
            result.error_message="argument override type mismatch: "+std::string(name);return result;
        }
        const mi::Size index=static_cast<mi::Size>(found-prototype.program->parameters.begin());
        if(layout->set_value(block->get_data(),value.get(),resources.get(),
                             layout->get_nested_state(index))!=0 || resources->failed()) {
            result.error_message="argument override cannot use prototype resource table: "+
                std::string(name);return result;
        }
    }
    binding->argument_block=std::move(block);
    binding->texture_runtime=std::move(mutableRuntime);
    result.binding=binding;
    result.success=true;result.cacheHit=true;return result;
}

} // namespace mdl
