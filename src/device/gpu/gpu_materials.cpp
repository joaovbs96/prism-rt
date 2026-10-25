#include "gpu_materials.h"

#include "../../mdl_material.h"
#include "../../mdl/context.h"
#include "../../mdl/texture_runtime.h"
#include "../../resource_limits.h"
#include "../../load_profile.h"
#include "gpu_device.h"

#include <tbb/parallel_for.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace prism::device::gpu {

namespace {

// Entry points generated for every program, as in the native backend.
struct EntryPoint { const char* path; const char* name; };
constexpr EntryPoint kEntryPoints[] = {
    {"init", "init"},
    {"surface.scattering", "bsdf"},
    {"surface.emission.emission", "edf"},
    {"surface.emission.intensity", "emission_intensity"},
    {"geometry.cutout_opacity", "cutout"},
    {"thin_walled", "thin_walled"},
    {"backface.scattering", "backface_bsdf"},
    {"backface.emission.emission", "backface_edf"},
    {"backface.emission.intensity", "backface_emission_intensity"},
    {"volume.absorption_coefficient", "volume_absorption"},
    {"volume.scattering_coefficient", "volume_scattering"},
    {"volume.scattering.directional_bias", "volume_anisotropy"},
};
constexpr size_t kEntryPointCount = std::size(kEntryPoints);
enum EntryIndex {
    E_INIT, E_BSDF, E_EDF, E_INTENSITY, E_CUTOUT, E_THIN_WALLED,
    E_BACK_BSDF, E_BACK_EDF, E_BACK_INTENSITY,
    E_VOLUME_ABSORPTION, E_VOLUME_SCATTERING, E_VOLUME_ANISOTROPY
};

std::string MessagesOf(mi::neuraylib::IMdl_execution_context* context)
{
    std::string text;
    for (mi::Size i = 0; i < context->get_messages_count(); ++i) {
        mi::base::Handle<const mi::neuraylib::IMessage> message(context->get_message(i));
        if (message->get_severity() <= mi::base::MESSAGE_SEVERITY_ERROR) {
            if (!text.empty()) text += "; ";
            text += message->get_string();
        }
    }
    return text;
}

// Applies `remap` to every resource index stored in an argument block.
template <class Remap>
void RemapResources(mi::neuraylib::ITarget_value_layout const* layout,
                    mi::neuraylib::Target_value_layout_state state,
                    char* block, size_t size, Remap const& remap)
{
    mi::neuraylib::IValue::Kind kind;
    mi::Size elementSize = 0;
    const mi::Size offset = layout->get_layout(kind, elementSize, state);
    using K = mi::neuraylib::IValue;
    if (kind == K::VK_TEXTURE || kind == K::VK_LIGHT_PROFILE ||
        kind == K::VK_BSDF_MEASUREMENT) {
        if (offset + sizeof(uint32_t) > size) return;
        uint32_t index = 0;
        std::memcpy(&index, block + offset, sizeof(index));
        index = remap(kind, index);
        std::memcpy(block + offset, &index, sizeof(index));
        return;
    }
    const mi::Size count = layout->get_num_elements(state);
    for (mi::Size i = 0; i < count; ++i)
        RemapResources(layout, layout->get_nested_state(i, state), block, size, remap);
}

// What a compiled class can be specialized on: entry points whose
// expression is constant need no generated code at all.
struct ProgramTraits {
    bool cutoutConstant = false;     // materials carry the constant opacity
    int thinWalled = -1;             // -1: varying, else the constant value
    bool emissionZero = false;
    bool backfaceEmissionZero = false;
    bool volumeZero = false;          // no medium behind the surface
    bool volumeAnisotropy = false;    // the phase function's anisotropy is addressable
};

mi::base::Handle<const mi::neuraylib::IValue> ConstantValue(
    mi::neuraylib::ICompiled_material const* material, const char* path)
{
    mi::base::Handle<const mi::neuraylib::IExpression> expression(material->lookup_sub_expression(path));
    if (!expression || expression->get_kind() != mi::neuraylib::IExpression::EK_CONSTANT) return {};
    mi::base::Handle<const mi::neuraylib::IExpression_constant> constant(
        expression->get_interface<mi::neuraylib::IExpression_constant>());
    return mi::base::Handle<const mi::neuraylib::IValue>(constant->get_value());
}

bool IsConstantBlack(mi::neuraylib::ICompiled_material const* material, const char* path)
{
    auto value = ConstantValue(material, path);
    mi::base::Handle<const mi::neuraylib::IValue_compound> compound(
        value ? value->get_interface<mi::neuraylib::IValue_compound>() : nullptr);
    if (!compound) return false;
    for (mi::Size i = 0; i < compound->get_size(); ++i) {
        mi::base::Handle<const mi::neuraylib::IValue_float> component(
            compound->get_value<mi::neuraylib::IValue_float>(i));
        if (!component || component->get_value() != 0.0f) return false;
    }
    return true;
}

ProgramTraits TraitsOf(mi::neuraylib::ICompiled_material const* material)
{
    ProgramTraits traits;
    if (auto value = ConstantValue(material, "geometry.cutout_opacity")) {
        mi::base::Handle<const mi::neuraylib::IValue_float> opacity(
            value->get_interface<mi::neuraylib::IValue_float>());
        traits.cutoutConstant = opacity && std::isfinite(opacity->get_value());
    }
    if (auto value = ConstantValue(material, "thin_walled")) {
        mi::base::Handle<const mi::neuraylib::IValue_bool> thin(
            value->get_interface<mi::neuraylib::IValue_bool>());
        if (thin) traits.thinWalled = thin->get_value() ? 1 : 0;
    }
    traits.emissionZero = IsConstantBlack(material, "surface.emission.intensity");
    traits.backfaceEmissionZero = IsConstantBlack(material, "backface.emission.intensity");
    traits.volumeZero = IsConstantBlack(material, "volume.absorption_coefficient") &&
        IsConstantBlack(material, "volume.scattering_coefficient");
    mi::base::Handle<const mi::neuraylib::IExpression> bias(
        material->lookup_sub_expression("volume.scattering.directional_bias"));
    traits.volumeAnisotropy = bias.is_valid_interface();
    return traits;
}

// Entry points a program needs, given its traits.
bool NeedsEntry(ProgramTraits const& t, size_t entry)
{
    switch (entry) {
    case E_EDF: case E_INTENSITY: return !t.emissionZero;
    case E_CUTOUT: return !t.cutoutConstant;
    case E_THIN_WALLED: return t.thinWalled < 0;
    case E_BACK_BSDF: return t.thinWalled != 0;
    case E_BACK_EDF: case E_BACK_INTENSITY: return t.thinWalled != 0 && !t.backfaceEmissionZero;
    case E_VOLUME_ABSORPTION: case E_VOLUME_SCATTERING: return !t.volumeZero && t.thinWalled != 1;
    case E_VOLUME_ANISOTROPY: return !t.volumeZero && t.thinWalled != 1 && t.volumeAnisotropy;
    default: return true;
    }
}

std::string ProgramFunction(uint32_t program, EntryIndex entry)
{
    return std::format("prism_p{}_{}", program, kEntryPoints[entry].name);
}

// The entry functions of a program translated alone (MdlProgramCode).
std::string SingleProgramFunction(EntryIndex entry)
{
    return std::format("prism_p_{}", kEntryPoints[entry].name);
}

void ConfigureHlslBackend(mi::neuraylib::IMdl_backend* backend)
{
    backend->set_option("num_texture_spaces", "1");
    backend->set_option("num_texture_results", std::to_string(kGpuTextureResults).c_str());
    backend->set_option("enable_auxiliary", "off");
    backend->set_option("texture_runtime_with_derivs", "on");
    backend->set_option("enable_ro_segment", "on");
    backend->set_option("max_const_data", "0");
    backend->set_option("df_handle_slot_mode", "none");
    backend->set_option("libbsdf_flags_in_bsdf_data", "off");
}

// Scene data ids (string constants of the link unit) naming the texture
// coordinates (mdl_runtime.hlsl).
std::string GenerateSceneData(mi::neuraylib::ITarget_code const* code)
{
    std::string s = "bool prism_scene_data_is_texcoord(int id)\n{\n";
    for (mi::Size i = 1; code && i < code->get_string_constant_count(); ++i)
        if (mdl::NativeTextureRuntime::IsTexcoordPrimvarName(code->get_string_constant(i)))
            s += std::format("    if (id == {}) return true;\n", i);
    s += "    return false;\n}\n\nbool prism_scene_data_is_camera_position(int id)\n{\n";
    for (mi::Size i = 1; code && i < code->get_string_constant_count(); ++i)
        if (mdl::NativeTextureRuntime::IsCameraPositionName(code->get_string_constant(i)))
            s += std::format("    if (id == {}) return true;\n", i);
    return s + "    return false;\n}\n\n";
}

// The prism_mdl_* dispatch functions. With `single`, of one program
// translated alone: its functions are prism_p_*, and the dispatch reaches
// them whatever the program index (the callable shader of that program).
//
// The prism_mdl_*_emitter functions (init, thin_walled, cutout,
// emission_intensity, edf_evaluate) dispatch to the programs `emitters`
// marks only: those of materials with varying emission, whose emission light
// samples evaluate (gpu_mdl_emitter_emission). Their code is inlined there a
// second time, the other programs' is not. With `single`, `emitters` is
// ignored: the program is marked if it has an EDF (its callable answers
// kMaterialCallEmitted whatever the scene).
std::string GenerateDispatch(std::vector<std::array<bool, kEntryPointCount>> const& present,
                             std::vector<ProgramTraits> const& traits, std::vector<bool> const& emitters,
                             bool single = false)
{
    const char* state = "Shading_state_material";
    const char* inoutState = "inout";
    const char* selector = single ? "    switch (0u) {\n" : "    switch (program) {\n";
    auto ProgramFunction = [single](uint32_t program, EntryIndex entry) {
        return single ? SingleProgramFunction(entry) : prism::device::gpu::ProgramFunction(program, entry);
    };
    // Programs of the function being generated (all, or the emitters).
    bool emitterOnly = false;
    auto included = [&](size_t p) {
        if (!emitterOnly) return true;
        if (single) return present[p][E_EDF] || present[p][E_BACK_EDF];
        return p < emitters.size() && emitters[p];
    };
    std::string s;
    auto cases = [&](EntryIndex entry, const char* call, const char* fallback) {
        std::string body = selector;
        for (size_t p = 0; p < present.size(); ++p) {
            if (!present[p][entry] || !included(p)) continue;
            const std::string function = ProgramFunction(uint32_t(p), entry);
            body += std::format("    case {}u: {}\n", p,
                std::vformat(call, std::make_format_args(function)));
        }
        body += std::format("    default: {}\n    }}\n", fallback);
        return body;
    };
    auto init = [&](const char* name) {
        s += std::format("void {}(uint program, {} {} state)\n{{\n", name, inoutState, state);
        s += cases(E_INIT, "{}(state); break;", "break;");
        s += "}\n\n";
    };
    auto thinWalled = [&](const char* name) {
        s += std::format("bool {}(uint program, {} {} state)\n{{\n", name, inoutState, state);
        std::string body = selector;
        for (size_t p = 0; p < present.size(); ++p) {
            if (!included(p)) continue;
            if (present[p][E_THIN_WALLED])
                body += std::format("    case {}u: return {}(state);\n", p, ProgramFunction(uint32_t(p), E_THIN_WALLED));
            else if (traits[p].thinWalled == 1)
                body += std::format("    case {}u: return true;\n", p);
        }
        s += body + "    default: return false;\n    }\n}\n\n";
    };
    auto cutout = [&](const char* name) {
        s += std::format("float {}(uint program, {} {} state)\n{{\n", name, inoutState, state);
        s += cases(E_CUTOUT, "return {}(state);", "return 1.0;");
        s += "}\n\n";
    };
    auto intensity = [&](const char* name) {
        s += std::format("float3 {}(uint program, bool backface, {} {} state)\n{{\n", name, inoutState, state);
        s += "    if (backface) {\n" + cases(E_BACK_INTENSITY, "return {}(state);", "break;") + "    }\n";
        s += "    else {\n" + cases(E_INTENSITY, "return {}(state);", "break;") + "    }\n";
        s += "    return float3(0.0, 0.0, 0.0);\n}\n\n";
    };
    // DF entry points report whether the selected side has a function.
    auto dfDispatch = [&](const char* name, const char* dataType, EntryIndex front,
                          EntryIndex back, const char* suffix) {
        std::string call = std::string("{}") + suffix + "(data, state); return true;";
        s += std::format("bool {}(uint program, bool backface, inout {} data, {} {} state)\n{{\n",
                         name, dataType, inoutState, state);
        s += "    if (backface) {\n" + cases(back, call.c_str(), "break;") + "    }\n";
        s += "    else {\n" + cases(front, call.c_str(), "break;") + "    }\n";
        s += "    return false;\n}\n\n";
    };
    init("prism_mdl_init");
    // The cutout path's own init: only programs with a varying cutout.
    s += std::format("void prism_mdl_init_cutout(uint program, {} {} state)\n{{\n", inoutState, state);
    {
        std::string body = selector;
        for (size_t p = 0; p < present.size(); ++p)
            if (present[p][E_CUTOUT])
                body += std::format("    case {}u: {}(state); break;\n", p, ProgramFunction(uint32_t(p), E_INIT));
        s += body + "    default: break;\n    }\n}\n\n";
    }
    thinWalled("prism_mdl_thin_walled");
    cutout("prism_mdl_cutout");
    intensity("prism_mdl_emission_intensity");
    s += std::format("float3 prism_mdl_volume_absorption(uint program, {} {} state)\n{{\n", inoutState, state);
    s += cases(E_VOLUME_ABSORPTION, "return {}(state);", "return float3(0.0, 0.0, 0.0);");
    s += "}\n\n";
    s += std::format("float3 prism_mdl_volume_scattering(uint program, {} {} state)\n{{\n", inoutState, state);
    s += cases(E_VOLUME_SCATTERING, "return {}(state);", "return float3(0.0, 0.0, 0.0);");
    s += "}\n\n";
    s += std::format("float prism_mdl_volume_anisotropy(uint program, {} {} state)\n{{\n", inoutState, state);
    s += cases(E_VOLUME_ANISOTROPY, "return {}(state);", "return 0.0;");
    s += "}\n\n";
    dfDispatch("prism_mdl_bsdf_sample", "Bsdf_sample_data", E_BSDF, E_BACK_BSDF, "_sample");
    dfDispatch("prism_mdl_bsdf_evaluate", "Bsdf_evaluate_data", E_BSDF, E_BACK_BSDF, "_evaluate");
    dfDispatch("prism_mdl_edf_evaluate", "Edf_evaluate_data", E_EDF, E_BACK_EDF, "_evaluate");
    // Light samples on triangles of varying emission.
    emitterOnly = true;
    init("prism_mdl_init_emitter");
    thinWalled("prism_mdl_thin_walled_emitter");
    cutout("prism_mdl_cutout_emitter");
    intensity("prism_mdl_emission_intensity_emitter");
    dfDispatch("prism_mdl_edf_evaluate_emitter", "Edf_evaluate_data", E_EDF, E_BACK_EDF, "_evaluate");
    return s;
}

// The code of one program translated alone, with its entry points named
// as program 0, plus its own resources and read-only data. Programs whose
// MDL classes differ only in names (MaterialX node names become parameter
// names) produce equal keys and can share one program in the link unit:
// their argument blocks then have the same layout.
std::string StructuralKey(mi::neuraylib::IMdl_backend* backend, mi::neuraylib::ITransaction* transaction,
                          mi::neuraylib::IMdl_execution_context* context,
                          mi::neuraylib::ICompiled_material const* compiled, ProgramTraits const& traits)
{
    mi::base::Handle<mi::neuraylib::ILink_unit> unit(backend->create_link_unit(transaction, context));
    if (!unit) return {};
    std::vector<std::string> names;
    std::vector<mi::neuraylib::Target_function_description> descs;
    names.reserve(kEntryPointCount);
    for (size_t e = 0; e < kEntryPointCount; ++e) {
        if (!NeedsEntry(traits, e)) continue;
        names.push_back(ProgramFunction(0, EntryIndex(e)));
        descs.emplace_back(kEntryPoints[e].path, names.back().c_str());
    }
    if (unit->add_material(compiled, descs.data(), descs.size(), context) != 0) return {};
    mi::base::Handle<const mi::neuraylib::ITarget_code> code(backend->translate_link_unit(unit.get(), context));
    if (!code) return {};
    std::string key = code->get_code();
    for (size_t e = 0; e < descs.size(); ++e) key += std::format("|{}", descs[e].function_index);
    // Image textures reached through arguments are rebound per material;
    // only images the code names by a literal index identify the program.
    static const std::regex literalTexture(
        R"(tex_[a-z0-9_]*_2d\(\s*\d)");
    const bool literalImages = std::regex_search(key, literalTexture);
    for (mi::Size i = 1; i < code->get_texture_count(); ++i) {
        const auto shape = code->get_texture_shape(i);
        const char* name = code->get_texture(i);
        const bool image = shape == mi::neuraylib::ITarget_code::Texture_shape_2d;
        key += std::format("|t{}:{}", int(shape), image && !literalImages ? "" : (name ? name : ""));
    }
    for (mi::Size i = 0; i < code->get_ro_data_segment_count(); ++i)
        key.append(code->get_ro_data_segment_data(i), code->get_ro_data_segment_size(i));
    mi::base::Handle<mi::neuraylib::ITarget_value_layout const> layout(code->get_argument_block_layout(0));
    key += std::format("|args{}", layout ? layout->get_size() : 0);
    return key;
}

// A 128-bit digest of a structural key; equal digests mean equal programs.
std::string Digest(std::string const& text)
{
    uint64_t a = 1469598103934665603ull, b = 0x9e3779b97f4a7c15ull;
    for (unsigned char c : text) {
        a = (a ^ c) * 1099511628211ull;
        b = (b + c) * 0x100000001b3ull ^ (b >> 29);
    }
    return std::format("{:016x}{:016x}{:x}", a, b, text.size());
}

// Digests persist across runs, keyed by the program's class identity (the
// context address prefix is per process).
std::filesystem::path StructuralKeyFile(std::string const& identity)
{
    const size_t colon = identity.find(':');
    const std::string stable = colon == std::string::npos ? identity : identity.substr(colon + 1);
    return prism::PersistentCacheRoot() / "gpu-structure" /
        (Digest(stable + "|hlsl" +
                "|mdl-" MI_NEURAYLIB_PRODUCT_VERSION_STRING "|v4") + ".key");
}

// Defined with the per-program layout below.
std::shared_ptr<const MdlProgramCode> ProgramCodeFor(mdl::CompiledProgram const& program);

std::mutex gStructuralKeysMutex;
// Program identity -> structural key. Programs are immutable.
auto& gStructuralKeys = *new std::map<std::pair<std::string, int>, std::string>();

} // namespace

MdlShaderCode GenerateMdlShaderCode(std::vector<MDLMaterial const*> const& materials)
{
    MdlShaderCode result;
    result.materials.resize(materials.size());
    result.textures.resize(1); // invalid texture
    result.textureResults = kGpuTextureResults;

    // Distinct programs, in first-use order.
    std::vector<mdl::CompiledProgram const*> programs;
    std::unordered_map<mdl::CompiledProgram const*, uint32_t> programIndex;
    mdl::Context const* context = nullptr;
    for (size_t i = 0; i < materials.size(); ++i) {
        auto& record = result.materials[i];
        MDLMaterial const* material = materials[i];
        if (!material || !material->is_valid()) { record.error = "invalid MDL material"; continue; }
        auto const& target = material->get_target_code();
        auto const* program = target.program.get();
        if (!program || program->compiled_material_name.empty()) {
            record.error = "program has no compiled class";
            continue;
        }
        if (program->builtin_resources) {
            record.error = "program uses the SDK resource handler";
            continue;
        }
        if (context && program->owner_identity != context) {
            record.error = "materials of different MDL contexts";
            continue;
        }
        context = program->owner_identity;
        auto [found, inserted] = programIndex.emplace(program, uint32_t(programs.size()));
        if (inserted) programs.push_back(program);
        record.program = found->second;
        record.valid = true;
        record.constantCutout = material->constant_cutout();
    }
    result.programCount = uint32_t(programs.size());
    if (programs.empty()) {
        result.code = GenerateDispatch({}, {}, {}) + GenerateSceneData(nullptr);
        result.success = true;
        return result;
    }

    mi::base::Handle<mi::neuraylib::IScope> scope(context->get_database()->get_global_scope());
    mi::base::Handle<mi::neuraylib::ITransaction> transaction(scope->create_transaction());
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> executionContext(
        context->get_factory()->create_execution_context());
    mi::base::Handle<mi::neuraylib::IMdl_backend_api> backendApi(
        context->get_neuray()->get_api_component<mi::neuraylib::IMdl_backend_api>());
    mi::base::Handle<mi::neuraylib::IMdl_backend> backend(backendApi->get_backend(
        mi::neuraylib::IMdl_backend_api::MB_HLSL));
    auto fail = [&](std::string message) {
        result.success = false;
        result.error = std::move(message);
        // Shaders still need the dispatch functions; every material is invalid.
        result.code = GenerateDispatch({}, {}, {}) + GenerateSceneData(nullptr);
        transaction->abort();
        return result;
    };
    if (!backend) return fail("HLSL backend unavailable");
    ConfigureHlslBackend(backend.get());

    // Merge structurally identical programs.
    {
        std::map<std::string, uint32_t> canonical;
        std::vector<uint32_t> remap(programs.size());
        std::vector<mdl::CompiledProgram const*> unique;
        for (size_t p = 0; p < programs.size(); ++p) {
            const auto cacheKey = std::make_pair(programs[p]->identity + "|" +
                programs[p]->compiled_material_name, 0);
            std::string key;
            {
                std::lock_guard lock(gStructuralKeysMutex);
                if (auto found = gStructuralKeys.find(cacheKey); found != gStructuralKeys.end())
                    key = found->second;
            }
            const auto keyFile = StructuralKeyFile(programs[p]->identity);
            const bool persistent = programs[p]->identity.find(':') != std::string::npos;
            if (key.empty() && persistent) {
                std::ifstream file(keyFile);
                std::getline(file, key);
                if (!key.empty()) {
                    std::lock_guard lock(gStructuralKeysMutex);
                    gStructuralKeys.emplace(cacheKey, key);
                }
            }
            if (key.empty()) {
                // The program translated alone (usually early, while its
                // material compiled) gives the structural key.
                auto alone = ProgramCodeFor(*programs[p]);
                if (alone && alone->success) {
                    key = alone->digest;
                    if (const char* dir = std::getenv("PRISM_GPU_DUMP_PROGRAMS"); dir && *dir)
                        std::ofstream(std::filesystem::path(dir) / std::format("program{}.hlsl", p)) << alone->code;
                } else {
                    mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
                        transaction->access<mi::neuraylib::ICompiled_material>(
                            programs[p]->compiled_material_name.c_str()));
                    if (compiled)
                        key = StructuralKey(backend.get(), transaction.get(), executionContext.get(),
                                            compiled.get(), TraitsOf(compiled.get()));
                    if (!key.empty()) key = Digest(key);
                }
                if (key.empty()) key = "unique:" + programs[p]->compiled_material_name;
                else {
                    if (persistent) {
                        std::error_code ignored;
                        std::filesystem::create_directories(keyFile.parent_path(), ignored);
                        std::ofstream(keyFile) << key << '\n';
                    }
                }
                std::lock_guard lock(gStructuralKeysMutex);
                gStructuralKeys.emplace(cacheKey, key);
            }
            auto [found, inserted] = canonical.emplace(std::move(key), uint32_t(unique.size()));
            if (inserted) unique.push_back(programs[p]);
            remap[p] = found->second;
        }
        for (auto& record : result.materials)
            if (record.valid) record.program = remap[record.program];
        programs = std::move(unique);
        result.programCount = uint32_t(programs.size());
    }

    // Programs whose emission light samples evaluate (prism_mdl_*_emitter).
    std::vector<bool> emitters(programs.size(), false);
    for (size_t i = 0; i < materials.size(); ++i)
        if (result.materials[i].valid && materials[i]->variesEmission() &&
            result.materials[i].program < emitters.size()) {
            emitters[result.materials[i].program] = true;
            result.varyingEmitters = true;
        }

    mi::base::Handle<mi::neuraylib::ILink_unit> linkUnit(
        backend->create_link_unit(transaction.get(), executionContext.get()));
    if (!linkUnit) return fail("cannot create link unit: " + MessagesOf(executionContext.get()));

    std::vector<std::array<bool, kEntryPointCount>> present(programs.size());
    std::vector<ProgramTraits> traits(programs.size());
    std::vector<std::string> names(programs.size() * kEntryPointCount);
    for (size_t p = 0; p < programs.size(); ++p) {
        mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
            transaction->access<mi::neuraylib::ICompiled_material>(
                programs[p]->compiled_material_name.c_str()));
        if (!compiled) return fail("compiled class missing for program " + std::to_string(p));
        traits[p] = TraitsOf(compiled.get());
        std::vector<mi::neuraylib::Target_function_description> descs;
        std::vector<size_t> entries;
        for (size_t e = 0; e < kEntryPointCount; ++e) {
            present[p][e] = false;
            if (!NeedsEntry(traits[p], e)) continue;
            names[p * kEntryPointCount + e] = ProgramFunction(uint32_t(p), EntryIndex(e));
            descs.emplace_back(kEntryPoints[e].path, names[p * kEntryPointCount + e].c_str());
            entries.push_back(e);
        }
        if (linkUnit->add_material(compiled.get(), descs.data(),
                                   descs.size(), executionContext.get()) != 0)
            return fail(std::format("add_material failed for program {}: {}", p,
                                    MessagesOf(executionContext.get())));
        for (size_t i = 0; i < entries.size(); ++i)
            present[p][entries[i]] = descs[i].function_index != ~mi::Size(0);
    }

    const auto translateStart = std::chrono::steady_clock::now();
    mi::base::Handle<const mi::neuraylib::ITarget_code> code(
        backend->translate_link_unit(linkUnit.get(), executionContext.get()));
    result.translateSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - translateStart).count();
    linkUnit.reset();
    if (!code) return fail("translate_link_unit failed: " + MessagesOf(executionContext.get()));
    if (code->get_light_profile_count() > 1 || code->get_bsdf_measurement_count() > 1)
        return fail("light profiles and measured BSDFs are not supported on GPU devices");

    // Textures of the link unit itself come first, so its indices are used as is.
    for (mi::Size i = 1; i < code->get_texture_count(); ++i) {
        MdlTexture texture;
        const auto shape = code->get_texture_shape(i);
        if (shape == mi::neuraylib::ITarget_code::Texture_shape_bsdf_data) {
            mi::Size w = 0, h = 0, d = 0;
            const char* pixelType = nullptr;
            const float* data = code->get_texture_df_data(i, w, h, d, pixelType);
            const size_t components = pixelType && std::strcmp(pixelType, "Float32<3>") == 0 ? 3 : 1;
            auto values = std::make_shared<std::vector<float>>(
                data, data + (data ? size_t(w) * h * d * components : 0));
            texture.kind = MdlTexture::Kind::BsdfData;
            texture.values = values->data();
            texture.owner = values;
            texture.width = w; texture.height = h; texture.depth = d;
            texture.components = components;
        } else if (shape == mi::neuraylib::ITarget_code::Texture_shape_2d) {
            mdl::NativeTextureRuntime::ResourceView view;
            std::string error;
            if (!mdl::NativeTextureRuntime::PrepareDatabaseTexture(
                    code->get_texture(i), transaction.get(), &view, &error))
                return fail("texture " + std::to_string(i) + ": " + error);
            texture.kind = MdlTexture::Kind::Image;
            texture.tiled = view.tiled;
            texture.tiles = std::move(view.tiles);
        } else {
            return fail("unsupported texture shape " + std::to_string(int(shape)));
        }
        result.textures.push_back(std::move(texture));
    }

    // Argument textures, deduplicated by their prepared resources.
    std::map<std::vector<const void*>, uint32_t> textureIndex;
    auto addTexture = [&](mdl::NativeTextureRuntime::ResourceView const& view) -> uint32_t {
        std::vector<const void*> key;
        if (view.values) key.push_back(view.owner.get());
        for (auto const& [tile, resource] : view.tiles) key.push_back(resource.get());
        if (key.empty()) return 0;
        auto [found, inserted] = textureIndex.emplace(key, uint32_t(result.textures.size()));
        if (inserted) {
            MdlTexture texture;
            if (view.values) {
                texture.kind = MdlTexture::Kind::BsdfData;
                texture.owner = view.owner; texture.values = view.values;
                texture.width = view.width; texture.height = view.height;
                texture.depth = view.depth; texture.components = view.components;
            } else {
                texture.kind = MdlTexture::Kind::Image;
                texture.tiled = view.tiled;
                texture.tiles = view.tiles;
            }
            result.textures.push_back(std::move(texture));
        }
        return found->second;
    };

    for (size_t i = 0; i < materials.size(); ++i) {
        auto& record = result.materials[i];
        if (!record.valid) continue;
        auto const& target = materials[i]->get_target_code();
        // Argument blocks start 16-byte aligned so every element keeps its
        // native alignment relative to the block start.
        result.argumentBlocks.resize((result.argumentBlocks.size() + 15) & ~size_t(15));
        record.argBlockOffset = uint32_t(result.argumentBlocks.size());
        auto const* block = target.argument_data();
        if (!block || block->get_size() == 0) continue;
        std::vector<char> bytes(block->get_data(), block->get_data() + block->get_size());
        mi::base::Handle<mi::neuraylib::ITarget_value_layout const> layout(
            target.program->target_code->get_argument_block_layout(0));
        auto const* runtime = target.binding ? target.binding->texture_runtime.get() : nullptr;
        bool resourcesOk = true;
        if (layout) {
            RemapResources(layout.get(), mi::neuraylib::Target_value_layout_state(),
                bytes.data(), bytes.size(),
                [&](mi::neuraylib::IValue::Kind kind, uint32_t index) -> uint32_t {
                    if (index == 0) return 0;
                    if (kind != mi::neuraylib::IValue::VK_TEXTURE || !runtime ||
                        index >= runtime->resourceCount()) {
                        resourcesOk = false;
                        return 0;
                    }
                    const uint32_t mapped = addTexture(runtime->resource(index));
                    if (!mapped) resourcesOk = false;
                    return mapped;
                });
        }
        if (!resourcesOk) {
            record.valid = false;
            record.error = "argument resource without a prepared texture";
            continue;
        }
        result.argumentBlocks.insert(result.argumentBlocks.end(), bytes.begin(), bytes.end());
    }
    // Shaders read the blocks as 32-bit words.
    result.argumentBlocks.resize(std::max<size_t>(16, (result.argumentBlocks.size() + 15) & ~size_t(15)));

    for (mi::Size i = 0; i < code->get_ro_data_segment_count(); ++i) {
        if (i > 0) return fail("multiple read-only data segments");
        const char* data = code->get_ro_data_segment_data(i);
        result.roData.assign(data, data + code->get_ro_data_segment_size(i));
    }
    result.roData.resize(std::max<size_t>(16, (result.roData.size() + 15) & ~size_t(15)));

    result.targetCode = code->get_code();
    result.stringConstants.assign(code->get_string_constant_count(), std::string());
    for (mi::Size i = 1; i < code->get_string_constant_count(); ++i)
        if (const char* name = code->get_string_constant(i)) result.stringConstants[i] = name;
    result.sceneData = GenerateSceneData(code.get());
    result.code = result.targetCode + "\n// ---- Prism material dispatch ----\n" + GenerateDispatch(present, traits, emitters) +
        result.sceneData;
    if (result.varyingEmitters) result.code = "#define PRISM_VARYING_EMITTERS 1\n" + result.code;
    for (size_t p = 0; p < programs.size(); ++p) {
        std::vector<std::array<bool, kEntryPointCount>> only(programs.size());
        for (auto& row : only) row.fill(false);
        only[p] = present[p];
        // A callable answers light samples whatever the material set (as a
        // program translated alone): every program with an EDF.
        std::vector<bool> emits(programs.size(), false);
        emits[p] = present[p][E_EDF] || present[p][E_BACK_EDF];
        result.programDispatch.push_back(std::format("\n// ---- Prism material dispatch: program {} ----\n", p) +
                                         GenerateDispatch(only, traits, emits));
    }
    transaction->commit();
    result.success = true;
    return result;
}

namespace {
// The key holds the bindings, so their addresses cannot be reused while cached.
using ShaderCodeKey = std::vector<std::shared_ptr<const mdl::MaterialBinding>>;
std::mutex gShaderCodeMutex;
std::vector<std::pair<ShaderCodeKey, std::shared_ptr<const MdlShaderCode>>> gShaderCodeCache;
}

std::shared_ptr<const MdlShaderCode> CachedMdlShaderCode(std::vector<MDLMaterial const*> const& materials)
{
    // The bindings own MDL interface handles, so the cache must be emptied before the
    // MDL SDK unloads; static destruction would run after libmdl_sdk is gone.
    static std::once_flag hookRegistered;
    std::call_once(hookRegistered, [] {
        mdl::Context::add_shutdown_hook([] {
            std::lock_guard lock(gShaderCodeMutex);
            gShaderCodeCache.clear();
        });
    });
    auto& mutex = gShaderCodeMutex;
    auto& cache = gShaderCodeCache;
    using Key = ShaderCodeKey;
    Key key;
    key.reserve(materials.size());
    for (MDLMaterial const* material : materials)
        key.push_back(material && material->is_valid() ? material->get_target_code().binding : nullptr);
    {
        std::lock_guard lock(mutex);
        for (auto const& [cachedKey, code] : cache)
            if (cachedKey == key) return code;
    }
    auto code = std::make_shared<const MdlShaderCode>(GenerateMdlShaderCode(materials));
    std::lock_guard lock(mutex);
    constexpr size_t maxEntries = 4;
    if (cache.size() >= maxEntries) cache.erase(cache.begin());
    cache.emplace_back(std::move(key), code);
    return code;
}

// ---------------------------------------------------------------------------
// Programs translated alone (MdlProgramCode) and the per-program layout.
// ---------------------------------------------------------------------------

namespace {

// Translates one compiled class alone: entry functions prism_p_*, its own
// texture indices, read-only data and string ids. Valid in any transaction
// that can access the material (and the module database elements it uses).
std::shared_ptr<MdlProgramCode> TranslateProgram(mdl::Context const& context,
                                                 mi::neuraylib::ITransaction* transaction,
                                                 mi::neuraylib::ICompiled_material const* compiled)
{
    auto result = std::make_shared<MdlProgramCode>();
    const auto start = std::chrono::steady_clock::now();
    auto fail = [&](std::string message) {
        result->success = false;
        result->error = std::move(message);
        return result;
    };
    mi::base::Handle<mi::neuraylib::IMdl_execution_context> executionContext(
        context.get_factory()->create_execution_context());
    mi::base::Handle<mi::neuraylib::IMdl_backend_api> backendApi(
        context.get_neuray()->get_api_component<mi::neuraylib::IMdl_backend_api>());
    mi::base::Handle<mi::neuraylib::IMdl_backend> backend(
        backendApi ? backendApi->get_backend(mi::neuraylib::IMdl_backend_api::MB_HLSL) : nullptr);
    if (!backend) return fail("HLSL backend unavailable");
    ConfigureHlslBackend(backend.get());
    const ProgramTraits traits = TraitsOf(compiled);
    std::vector<std::string> names;
    std::vector<mi::neuraylib::Target_function_description> descs;
    std::vector<size_t> entries;
    names.reserve(kEntryPointCount);
    for (size_t e = 0; e < kEntryPointCount; ++e) {
        if (!NeedsEntry(traits, e)) continue;
        names.push_back(SingleProgramFunction(EntryIndex(e)));
        descs.emplace_back(kEntryPoints[e].path, names.back().c_str());
        entries.push_back(e);
    }
    mi::base::Handle<const mi::neuraylib::ITarget_code> code;
    {
        mi::base::Handle<mi::neuraylib::ILink_unit> unit(backend->create_link_unit(transaction, executionContext.get()));
        if (!unit) return fail("cannot create link unit: " + MessagesOf(executionContext.get()));
        if (unit->add_material(compiled, descs.data(), descs.size(), executionContext.get()) != 0)
            return fail("add_material failed: " + MessagesOf(executionContext.get()));
        code = mi::base::Handle<const mi::neuraylib::ITarget_code>(
            backend->translate_link_unit(unit.get(), executionContext.get()));
    }
    if (!code) return fail("translate_link_unit failed: " + MessagesOf(executionContext.get()));
    if (code->get_light_profile_count() > 1 || code->get_bsdf_measurement_count() > 1)
        return fail("light profiles and measured BSDFs are not supported on GPU devices");
    if (code->get_ro_data_segment_count() > 1) return fail("multiple read-only data segments");
    std::vector<std::array<bool, kEntryPointCount>> present(1);
    present[0].fill(false);
    for (size_t i = 0; i < entries.size(); ++i) present[0][entries[i]] = descs[i].function_index != ~mi::Size(0);
    for (mi::Size i = 1; i < code->get_texture_count(); ++i) {
        MdlProgramCode::Texture texture;
        const auto shape = code->get_texture_shape(i);
        const char* name = code->get_texture(i);
        texture.name = name ? name : "";
        if (shape == mi::neuraylib::ITarget_code::Texture_shape_bsdf_data) {
            mi::Size w = 0, h = 0, d = 0;
            const char* pixelType = nullptr;
            const float* data = code->get_texture_df_data(i, w, h, d, pixelType);
            const size_t components = pixelType && std::strcmp(pixelType, "Float32<3>") == 0 ? 3 : 1;
            texture.image = false;
            texture.values = std::make_shared<const std::vector<float>>(
                data, data + (data ? size_t(w) * h * d * components : 0));
            texture.width = w; texture.height = h; texture.depth = d;
            texture.components = components;
        } else if (shape != mi::neuraylib::ITarget_code::Texture_shape_2d) {
            return fail("unsupported texture shape " + std::to_string(int(shape)));
        }
        result->textures.push_back(std::move(texture));
    }
    if (code->get_ro_data_segment_count() == 1) {
        const char* data = code->get_ro_data_segment_data(0);
        result->roData.assign(data, data + code->get_ro_data_segment_size(0));
    }
    result->stringConstants.assign(code->get_string_constant_count(), std::string());
    for (mi::Size i = 1; i < code->get_string_constant_count(); ++i)
        if (const char* name = code->get_string_constant(i)) result->stringConstants[i] = name;
    std::string dispatch = GenerateDispatch(present, std::vector<ProgramTraits>{traits}, {}, true);
    result->code = code->get_code();
    result->targetCodeBytes = result->code.size();
    result->code += "\n// ---- Prism material dispatch ----\n" + dispatch + GenerateSceneData(code.get());
    // Equal code, textures named by literal index, read-only data and argument
    // layout make interchangeable programs (StructuralKey).
    std::string key = result->code;
    static const std::regex literalTexture(R"(tex_[a-z0-9_]*_2d\(\s*\d)");
    const bool literalImages = std::regex_search(result->code, literalTexture);
    for (auto const& texture : result->textures)
        key += std::format("|t{}:{}", texture.image ? 1 : 0, texture.image && !literalImages ? "" : texture.name);
    key.append(result->roData.begin(), result->roData.end());
    mi::base::Handle<mi::neuraylib::ITarget_value_layout const> layout(code->get_argument_block_layout(0));
    key += std::format("|args{}", layout ? layout->get_size() : 0);
    result->digest = Digest(key);
    result->translateSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result->success = true;
    return result;
}

using ProgramFuture = std::shared_future<std::shared_ptr<const MdlProgramCode>>;
std::mutex gProgramsMutex;
// Context and class key -> translation (in flight or done). Translations
// hold no SDK objects, so they may outlive the SDK.
auto& gPrograms = *new std::unordered_map<std::string, ProgramFuture>();
auto& gProgramListeners = *new std::map<int, std::shared_ptr<const ProgramListener>>();
int gNextListener = 1;

std::string RegistryKey(mdl::Context const* context, std::string const& classKey)
{
    return std::format("{}|{}", static_cast<const void*>(context), classKey);
}

// Translations persist across runs, by class key (stable across processes):
// a warm load then translates nothing. Images keep their database names, which
// the scene that loads them resolves again (GenerateProgramShaderCode
// translates anew if one no longer resolves).
std::filesystem::path ProgramFile(std::string const& classKey)
{
    return prism::PersistentCacheRoot() / "gpu-programs" /
        (Digest(classKey + "|hlsl-alone|mdl-" MI_NEURAYLIB_PRODUCT_VERSION_STRING "|results-" +
                std::to_string(kGpuTextureResults) + "|v2") + ".prog");
}

constexpr char kProgramMagic[8] = {'P', 'R', 'S', 'M', 'P', 'R', 'G', '1'};

struct ProgramWriter {
    std::string bytes;
    void u64(uint64_t v) { bytes.append(reinterpret_cast<const char*>(&v), 8); }
    void str(std::string_view s) { u64(s.size()); bytes.append(s); }
};
struct ProgramReader {
    std::string_view bytes;
    bool ok = true;
    uint64_t u64()
    {
        if (bytes.size() < 8) { ok = false; return 0; }
        uint64_t v;
        std::memcpy(&v, bytes.data(), 8);
        bytes.remove_prefix(8);
        return v;
    }
    std::string str()
    {
        const uint64_t n = u64();
        if (!ok || n > bytes.size()) { ok = false; return {}; }
        std::string s(bytes.substr(0, n));
        bytes.remove_prefix(n);
        return s;
    }
};

uint64_t BytesDigest(std::string_view text)
{
    uint64_t a = 1469598103934665603ull;
    for (unsigned char c : text) a = (a ^ c) * 1099511628211ull;
    return a ^ (uint64_t(text.size()) * 0x9e3779b97f4a7c15ull);
}

void SaveProgram(std::string const& classKey, MdlProgramCode const& code)
{
    if (!code.success || classKey.empty()) return;
    ProgramWriter w;
    w.str(code.code);
    w.u64(code.targetCodeBytes);
    w.str(code.digest);
    w.str(std::string_view(code.roData.data(), code.roData.size()));
    w.u64(code.stringConstants.size());
    for (auto const& s : code.stringConstants) w.str(s);
    w.u64(code.textures.size());
    for (auto const& t : code.textures) {
        w.u64(t.image ? 1 : 0);
        w.str(t.name);
        w.u64(t.width); w.u64(t.height); w.u64(t.depth); w.u64(t.components);
        const size_t n = t.values ? t.values->size() : 0;
        w.str(std::string_view(n ? reinterpret_cast<const char*>(t.values->data()) : "", n * sizeof(float)));
    }
    std::string file(kProgramMagic, sizeof(kProgramMagic));
    const uint64_t digest = BytesDigest(w.bytes);
    file.append(reinterpret_cast<const char*>(&digest), 8);
    file += w.bytes;
    const auto path = ProgramFile(classKey);
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);
    static std::atomic<uint64_t> serial{0};
    const auto temporary = std::filesystem::path(path.string() + std::format(".{}.tmp", ++serial));
    {
        std::ofstream out(temporary, std::ios::binary);
        out.write(file.data(), std::streamsize(file.size()));
        if (!out) {
            out.close();
            std::filesystem::remove(temporary, ignored);
            return;
        }
    }
    std::filesystem::rename(temporary, path, ignored);
    if (ignored) std::filesystem::remove(temporary, ignored);
}

std::shared_ptr<MdlProgramCode> LoadProgram(std::string const& classKey)
{
    if (classKey.empty()) return nullptr;
    std::ifstream in(ProgramFile(classKey), std::ios::binary);
    if (!in) return nullptr;
    const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.size() < 16 || file.compare(0, 8, std::string_view(kProgramMagic, 8)) != 0) return nullptr;
    uint64_t digest;
    std::memcpy(&digest, file.data() + 8, 8);
    ProgramReader r{std::string_view(file).substr(16)};
    if (BytesDigest(r.bytes) != digest) return nullptr;
    auto code = std::make_shared<MdlProgramCode>();
    code->code = r.str();
    code->targetCodeBytes = size_t(r.u64());
    code->digest = r.str();
    const std::string ro = r.str();
    code->roData.assign(ro.begin(), ro.end());
    const uint64_t strings = r.u64();
    for (uint64_t i = 0; r.ok && i < strings; ++i) code->stringConstants.push_back(r.str());
    const uint64_t textures = r.u64();
    for (uint64_t i = 0; r.ok && i < textures; ++i) {
        MdlProgramCode::Texture t;
        t.image = r.u64() != 0;
        t.name = r.str();
        t.width = size_t(r.u64()); t.height = size_t(r.u64());
        t.depth = size_t(r.u64()); t.components = size_t(r.u64());
        const std::string values = r.str();
        if (!t.image) {
            auto v = std::make_shared<std::vector<float>>(values.size() / sizeof(float));
            std::memcpy(v->data(), values.data(), v->size() * sizeof(float));
            t.values = v;
        }
        code->textures.push_back(std::move(t));
    }
    if (!r.ok || code->targetCodeBytes > code->code.size()) return nullptr;
    code->success = true;
    code->fromDisk = true;
    return code;
}

// Class keys whose stored translation named an image the scene could not
// resolve: translated anew from then on.
auto& gStaleProgramFiles = *new std::unordered_set<std::string>();

// The stored translation of a class, unless marked stale.
std::shared_ptr<MdlProgramCode> StoredProgram(std::string const& classKey)
{
    {
        std::lock_guard lock(gProgramsMutex);
        if (gStaleProgramFiles.contains(classKey)) return nullptr;
    }
    auto code = LoadProgram(classKey);
    if (code) code->classKey = classKey;
    return code;
}

void ObserveClass(mdl::Context const& context, std::string const& classKey, mi::neuraylib::ITransaction* transaction,
                  mi::neuraylib::ICompiled_material const* material)
{
    const std::string key = RegistryKey(&context, classKey);
    std::promise<std::shared_ptr<const MdlProgramCode>> promise;
    {
        std::lock_guard lock(gProgramsMutex);
        if (gPrograms.contains(key)) return;
        gPrograms.emplace(key, promise.get_future().share());
    }
    std::shared_ptr<const MdlProgramCode> code;
    try {
        code = StoredProgram(classKey);
        if (!code) {
            auto translated = TranslateProgram(context, transaction, material);
            translated->classKey = classKey;
            SaveProgram(classKey, *translated);
            code = translated;
        }
    } catch (std::exception const& e) {
        auto failed = std::make_shared<MdlProgramCode>();
        failed->error = e.what();
        code = failed;
    }
    promise.set_value(code);
    if (prism::load_profile::Enabled()) {
        size_t lookups = 0;
        for (size_t at = code->code.find("_3d("); at != std::string::npos; at = code->code.find("_3d(", at + 4))
            ++lookups;
        prism::load_profile::Mark("gpu.program",
            std::format("translate_ms={:.1f} code_kb={} ok={} early=1 disk={} lookups_3d={} digest={}",
                        code->translateSeconds * 1e3, code->code.size() >> 10, code->success ? 1 : 0,
                        code->fromDisk ? 1 : 0, lookups,
                        code->digest.substr(0, 12)));
    }
    if (!code->success) return;
    std::vector<std::shared_ptr<const ProgramListener>> listeners;
    {
        std::lock_guard lock(gProgramsMutex);
        for (auto const& [token, listener] : gProgramListeners) listeners.push_back(listener);
    }
    for (auto const& listener : listeners) (*listener)(code);
}

// The translation of `program`: the early one, or one made now from its
// stored class (kept for later scenes when the class is shareable).
std::shared_ptr<const MdlProgramCode> ProgramCodeFor(mdl::CompiledProgram const& program)
{
    const std::string key = program.class_key.empty() ? std::string()
                                                      : RegistryKey(program.owner_identity, program.class_key);
    std::promise<std::shared_ptr<const MdlProgramCode>> promise;
    if (!key.empty()) {
        ProgramFuture found;
        {
            std::lock_guard lock(gProgramsMutex);
            if (auto it = gPrograms.find(key); it != gPrograms.end()) found = it->second;
            else gPrograms.emplace(key, promise.get_future().share());
        }
        if (found.valid()) {
            auto code = found.get();
            if (code && code->success) return code;
            // A failed early translation is tried again from the stored class.
            std::lock_guard lock(gProgramsMutex);
            gPrograms.erase(key);
            return ProgramCodeFor(program);
        }
    }
    auto translate = [&]() -> std::shared_ptr<const MdlProgramCode> {
        auto failed = std::make_shared<MdlProgramCode>();
        mdl::Context const* context = program.owner_identity;
        if (!context || program.compiled_material_name.empty()) {
            failed->error = "program has no compiled class";
            return failed;
        }
        mi::base::Handle<mi::neuraylib::IScope> scope(context->get_database()->get_global_scope());
        mi::base::Handle<mi::neuraylib::ITransaction> transaction(scope->create_transaction());
        std::shared_ptr<const MdlProgramCode> code;
        {
            mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
                transaction->access<mi::neuraylib::ICompiled_material>(program.compiled_material_name.c_str()));
            if (!compiled) failed->error = "compiled class missing";
            else if (!(code = StoredProgram(program.class_key))) {
                auto translated = TranslateProgram(*context, transaction.get(), compiled.get());
                translated->classKey = program.class_key;
                SaveProgram(program.class_key, *translated);
                code = translated;
            }
        }
        transaction->commit();
        if (prism::load_profile::Enabled() && code)
            prism::load_profile::Mark("gpu.program", std::format("translate_ms={:.1f} code_kb={} ok={} early=0",
                                                                 code->translateSeconds * 1e3, code->code.size() >> 10,
                                                                 code->success ? 1 : 0));
        return code ? code : failed;
    };
    std::shared_ptr<const MdlProgramCode> code;
    try {
        code = translate();
    } catch (std::exception const& e) {
        auto failed = std::make_shared<MdlProgramCode>();
        failed->error = e.what();
        code = failed;
    }
    if (!key.empty()) {
        promise.set_value(code);
        if (!code->success) {
            std::lock_guard lock(gProgramsMutex);
            gPrograms.erase(key);
        }
    }
    return code;
}

bool IdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The top-level declarations of generated HLSL, as [begin, end) ranges: a
// function definition ends with its body, anything else with a semicolon at
// depth zero; preprocessor lines stand alone (as LinkUnitTrimmer splits).
std::vector<std::pair<size_t, size_t>> TopLevelDeclarations(std::string_view code)
{
    std::vector<std::pair<size_t, size_t>> chunks;
    size_t start = 0, depth = 0, parens = 0;
    bool sawAssign = false, sawParen = false;
    for (size_t i = 0; i < code.size();) {
        const char c = code[i];
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            const size_t end = code.find('\n', i);
            i = end == std::string_view::npos ? code.size() : end;
            continue;
        }
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            const size_t end = code.find("*/", i + 2);
            i = end == std::string_view::npos ? code.size() : end + 2;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < code.size() && code[j] != c) j += code[j] == '\\' ? 2 : 1;
            i = j + 1;
            continue;
        }
        if (c == '#' && depth == 0 && parens == 0) {
            if (i > start) chunks.emplace_back(start, i);
            const size_t end = code.find('\n', i);
            const size_t next = end == std::string_view::npos ? code.size() : end + 1;
            chunks.emplace_back(i, next);
            i = start = next;
            sawAssign = sawParen = false;
            continue;
        }
        if (IdentifierChar(c)) {
            while (i < code.size() && IdentifierChar(code[i])) ++i;
            continue;
        }
        if (depth == 0) {
            if (c == '(') { if (parens++ == 0 && !sawAssign) sawParen = true; }
            else if (c == ')') { if (parens > 0) --parens; }
            else if (c == '=' && parens == 0) sawAssign = true;
            else if (c == ';' && parens == 0) {
                chunks.emplace_back(start, ++i);
                start = i;
                sawAssign = sawParen = false;
                continue;
            }
        }
        if (c == '{') ++depth;
        else if (c == '}' && depth > 0 && --depth == 0 && sawParen && !sawAssign) {
            chunks.emplace_back(start, ++i);
            start = i;
            sawAssign = sawParen = false;
            continue;
        }
        ++i;
    }
    if (start < code.size()) chunks.emplace_back(start, code.size());
    return chunks;
}

uint64_t TextHash(std::string_view text)
{
    uint64_t a = 1469598103934665603ull;
    for (unsigned char c : text) a = (a ^ c) * 1099511628211ull;
    return a ^ (uint64_t(text.size()) * 0x9e3779b97f4a7c15ull);
}

} // namespace

int AddProgramListener(ProgramListener listener)
{
    std::lock_guard lock(gProgramsMutex);
    const int token = gNextListener++;
    gProgramListeners.emplace(token, std::make_shared<const ProgramListener>(
        listener ? std::move(listener) : ProgramListener([](auto const&) {})));
    if (gProgramListeners.size() == 1) mdl::SetClassObserver(ObserveClass);
    return token;
}

void RemoveProgramListener(int token)
{
    std::lock_guard lock(gProgramsMutex);
    if (gProgramListeners.erase(token) && gProgramListeners.empty()) mdl::SetClassObserver(nullptr);
}

void LinkUnitEstimate::add(MdlProgramCode const& program)
{
    if (!program.success || !m_programs.insert(program.digest).second) return;
    std::string_view target(program.code.data(), program.targetCodeBytes);
    for (auto const& [begin, end] : TopLevelDeclarations(target))
        if (m_declarations.insert(TextHash(target.substr(begin, end - begin))).second) m_bytes += end - begin;
    m_bytes += program.code.size() - program.targetCodeBytes;
}

size_t EstimatedLinkUnitBytes(std::vector<std::shared_ptr<const MdlProgramCode>> const& programs)
{
    LinkUnitEstimate estimate;
    for (auto const& program : programs)
        if (program) estimate.add(*program);
    return estimate.bytes();
}

std::vector<std::shared_ptr<const MdlProgramCode>> ProgramCodes(std::vector<MDLMaterial const*> const& materials)
{
    std::vector<std::shared_ptr<const MdlProgramCode>> result(materials.size());
    std::vector<mdl::CompiledProgram const*> programs;
    std::unordered_map<mdl::CompiledProgram const*, size_t> index;
    std::vector<size_t> programOf(materials.size(), ~size_t(0));
    for (size_t i = 0; i < materials.size(); ++i) {
        MDLMaterial const* material = materials[i];
        if (!material || !material->is_valid()) continue;
        auto const* program = material->get_target_code().program.get();
        if (!program || program->compiled_material_name.empty() || program->builtin_resources) continue;
        auto [found, inserted] = index.emplace(program, programs.size());
        if (inserted) programs.push_back(program);
        programOf[i] = found->second;
    }
    std::vector<std::shared_ptr<const MdlProgramCode>> codes(programs.size());
    tbb::parallel_for(size_t(0), programs.size(), [&](size_t p) { codes[p] = ProgramCodeFor(*programs[p]); });
    for (size_t i = 0; i < materials.size(); ++i)
        if (programOf[i] != ~size_t(0)) result[i] = codes[programOf[i]];
    return result;
}

MdlShaderCode GenerateProgramShaderCode(std::vector<MDLMaterial const*> const& materials)
{
    MdlShaderCode result;
    result.perProgram = true;
    result.materials.resize(materials.size());
    result.textures.resize(1); // invalid texture
    result.textureResults = kGpuTextureResults;
    result.stringConstants.assign(1, std::string());
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::shared_ptr<const MdlProgramCode>> codes = ProgramCodes(materials);
    mdl::Context const* context = nullptr;
    // Distinct programs by digest, in first-use order.
    std::map<std::string, uint32_t> programIndex;
    for (size_t i = 0; i < materials.size(); ++i) {
        auto& record = result.materials[i];
        MDLMaterial const* material = materials[i];
        if (!material || !material->is_valid()) { record.error = "invalid MDL material"; continue; }
        auto const* program = material->get_target_code().program.get();
        if (!program || program->compiled_material_name.empty()) { record.error = "program has no compiled class"; continue; }
        if (program->builtin_resources) { record.error = "program uses the SDK resource handler"; continue; }
        if (context && program->owner_identity != context) { record.error = "materials of different MDL contexts"; continue; }
        if (!codes[i] || !codes[i]->success) {
            record.error = codes[i] ? codes[i]->error : std::string("no program code");
            continue;
        }
        context = program->owner_identity;
        static const bool merge = [] {
            const char* value = std::getenv("PRISM_GPU_MERGE_PROGRAMS");
            return !value || std::string_view(value) != "0";
        }();
        auto [found, inserted] = programIndex.emplace(
            merge ? codes[i]->digest : std::format("{}", static_cast<const void*>(codes[i].get())),
            uint32_t(result.programs.size()));
        if (inserted) result.programs.push_back(codes[i]);
        record.program = found->second;
        record.valid = true;
        record.constantCutout = material->constant_cutout();
        result.varyingEmitters = result.varyingEmitters || material->variesEmission();
    }
    result.programCount = uint32_t(result.programs.size());
    result.success = true;
    if (result.programs.empty()) {
        result.argumentBlocks.resize(16);
        result.roData.resize(16);
        return result;
    }

    // Global texture indices of each program's own textures.
    std::map<std::vector<const void*>, uint32_t> imageIndex;
    std::map<std::string, uint32_t> dataIndex;
    auto addView = [&](mdl::NativeTextureRuntime::ResourceView const& view) -> uint32_t {
        std::vector<const void*> key;
        if (view.values) key.push_back(view.owner.get());
        for (auto const& [tile, resource] : view.tiles) key.push_back(resource.get());
        if (key.empty()) return 0;
        auto [found, inserted] = imageIndex.emplace(key, uint32_t(result.textures.size()));
        if (inserted) {
            MdlTexture texture;
            if (view.values) {
                texture.kind = MdlTexture::Kind::BsdfData;
                texture.owner = view.owner; texture.values = view.values;
                texture.width = view.width; texture.height = view.height;
                texture.depth = view.depth; texture.components = view.components;
            } else {
                texture.kind = MdlTexture::Kind::Image;
                texture.tiled = view.tiled;
                texture.tiles = view.tiles;
            }
            result.textures.push_back(std::move(texture));
        }
        return found->second;
    };
    std::vector<std::vector<uint32_t>> programTextures(result.programs.size());
    bool stale = false;
    {
        mi::base::Handle<mi::neuraylib::IScope> scope(context->get_database()->get_global_scope());
        mi::base::Handle<mi::neuraylib::ITransaction> transaction(scope->create_transaction());
        for (size_t p = 0; p < result.programs.size(); ++p) {
            for (auto const& texture : result.programs[p]->textures) {
                uint32_t global = 0;
                if (texture.image) {
                    mdl::NativeTextureRuntime::ResourceView view;
                    std::string error;
                    if (mdl::NativeTextureRuntime::PrepareDatabaseTexture(texture.name.c_str(), transaction.get(),
                                                                          &view, &error))
                        global = addView(view);
                    else if (result.programs[p]->fromDisk) {
                        // A stored translation names an image this process
                        // does not have: translate the class anew.
                        std::lock_guard lock(gProgramsMutex);
                        gStaleProgramFiles.insert(result.programs[p]->classKey);
                        gPrograms.erase(RegistryKey(context, result.programs[p]->classKey));
                        stale = true;
                    } else if (GpuVerbose())
                        std::cerr << "PrismRT GPU: texture " << texture.name << " of an MDL program: " << error << '\n';
                } else if (texture.values) {
                    const std::string key = std::format("{}|{}x{}x{}x{}", texture.name, texture.width, texture.height,
                                                        texture.depth, texture.components);
                    auto [found, inserted] = dataIndex.emplace(key, uint32_t(result.textures.size()));
                    if (inserted) {
                        MdlTexture data;
                        data.kind = MdlTexture::Kind::BsdfData;
                        data.owner = texture.values;
                        data.values = texture.values->data();
                        data.width = texture.width; data.height = texture.height;
                        data.depth = texture.depth; data.components = texture.components;
                        result.textures.push_back(std::move(data));
                    }
                    global = found->second;
                }
                programTextures[p].push_back(global);
            }
        }
        transaction->commit();
    }
    // Once more with those classes translated anew (they are not read again).
    if (stale) return GenerateProgramShaderCode(materials);

    // Read-only data: a zero header, then each program's segment.
    result.roData.assign(16, 0);
    std::vector<uint32_t> roOffset(result.programs.size());
    for (size_t p = 0; p < result.programs.size(); ++p) {
        result.roData.resize((result.roData.size() + 15) & ~size_t(15));
        roOffset[p] = uint32_t(result.roData.size());
        result.roData.insert(result.roData.end(), result.programs[p]->roData.begin(), result.programs[p]->roData.end());
    }
    result.roData.resize((result.roData.size() + 15) & ~size_t(15));
    // Tables follow as 32-bit words: each program's string ids, each
    // material's texture indices.
    std::vector<uint32_t> tables;
    const uint32_t tableBase = uint32_t(result.roData.size() / 4);
    std::map<std::string, uint32_t> stringIds;
    std::vector<uint32_t> stringTable(result.programs.size());
    for (size_t p = 0; p < result.programs.size(); ++p) {
        auto const& names = result.programs[p]->stringConstants;
        stringTable[p] = tableBase + uint32_t(tables.size());
        tables.push_back(0);
        for (size_t s = 1; s < names.size(); ++s) {
            uint32_t id = 0;
            if (!names[s].empty()) {
                auto [found, inserted] = stringIds.emplace(names[s], uint32_t(result.stringConstants.size()));
                if (inserted) result.stringConstants.push_back(names[s]);
                id = found->second;
            }
            tables.push_back(id);
        }
    }

    for (size_t i = 0; i < materials.size(); ++i) {
        auto& record = result.materials[i];
        if (!record.valid) continue;
        auto const& target = materials[i]->get_target_code();
        const uint32_t p = record.program;
        record.roDataOffset = roOffset[p];
        record.stringTable = stringTable[p];
        // The material's texture table: the program's own textures (indices
        // 1..n of its code), then its argument textures, which the argument
        // block names by their index in this table.
        std::vector<uint32_t> table(1, 0u);
        table.insert(table.end(), programTextures[p].begin(), programTextures[p].end());
        result.argumentBlocks.resize((result.argumentBlocks.size() + 15) & ~size_t(15));
        record.argBlockOffset = uint32_t(result.argumentBlocks.size());
        auto const* block = target.argument_data();
        if (block && block->get_size() > 0) {
            std::vector<char> bytes(block->get_data(), block->get_data() + block->get_size());
            mi::base::Handle<mi::neuraylib::ITarget_value_layout const> layout(
                target.program->target_code->get_argument_block_layout(0));
            auto const* runtime = target.binding ? target.binding->texture_runtime.get() : nullptr;
            bool resourcesOk = true;
            std::map<uint32_t, uint32_t> local;
            if (layout) {
                RemapResources(layout.get(), mi::neuraylib::Target_value_layout_state(), bytes.data(), bytes.size(),
                    [&](mi::neuraylib::IValue::Kind kind, uint32_t index) -> uint32_t {
                        if (index == 0) return 0;
                        if (kind != mi::neuraylib::IValue::VK_TEXTURE || !runtime ||
                            index >= runtime->resourceCount()) {
                            resourcesOk = false;
                            return 0;
                        }
                        if (auto found = local.find(index); found != local.end()) return found->second;
                        const uint32_t global = addView(runtime->resource(index));
                        if (!global) resourcesOk = false;
                        const uint32_t slot = uint32_t(table.size());
                        table.push_back(global);
                        local.emplace(index, slot);
                        return slot;
                    });
            }
            if (!resourcesOk) {
                record.valid = false;
                record.error = "argument resource without a prepared texture";
                continue;
            }
            result.argumentBlocks.insert(result.argumentBlocks.end(), bytes.begin(), bytes.end());
        }
        record.textureTable = tableBase + uint32_t(tables.size());
        tables.insert(tables.end(), table.begin(), table.end());
    }
    result.argumentBlocks.resize(std::max<size_t>(16, (result.argumentBlocks.size() + 15) & ~size_t(15)));
    const size_t tableBytes = tables.size() * 4;
    result.roData.resize(result.roData.size() + tableBytes);
    std::memcpy(result.roData.data() + size_t(tableBase) * 4, tables.data(), tableBytes);
    result.roData.resize((result.roData.size() + 15) & ~size_t(15));
    for (auto const& program : result.programs) result.translateSeconds += program->translateSeconds;
    if (prism::load_profile::Enabled())
        prism::load_profile::Mark("gpu.program_layout",
            std::format("programs={} textures={} ro_kb={} assemble_ms={:.1f}", result.programs.size(),
                        result.textures.size() - 1, result.roData.size() >> 10,
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
    return result;
}

std::shared_ptr<const MdlShaderCode> CachedProgramShaderCode(std::vector<MDLMaterial const*> const& materials)
{
    static std::mutex mutex;
    static auto& cache = *new std::vector<std::pair<ShaderCodeKey, std::shared_ptr<const MdlShaderCode>>>();
    static std::once_flag hookRegistered;
    std::call_once(hookRegistered, [] {
        mdl::Context::add_shutdown_hook([] {
            std::lock_guard lock(mutex);
            cache.clear();
        });
    });
    ShaderCodeKey key;
    key.reserve(materials.size());
    for (MDLMaterial const* material : materials)
        key.push_back(material && material->is_valid() ? material->get_target_code().binding : nullptr);
    {
        std::lock_guard lock(mutex);
        for (auto const& [cachedKey, code] : cache)
            if (cachedKey == key) return code;
    }
    auto code = std::make_shared<const MdlShaderCode>(GenerateProgramShaderCode(materials));
    std::lock_guard lock(mutex);
    constexpr size_t maxEntries = 4;
    if (cache.size() >= maxEntries) cache.erase(cache.begin());
    cache.emplace_back(std::move(key), code);
    return code;
}

} // namespace prism::device::gpu
