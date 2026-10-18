#include "gpu_materials.h"

#include "../../mdl_material.h"
#include "../../mdl/texture_runtime.h"
#include "../../resource_limits.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <unordered_map>

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
};
constexpr size_t kEntryPointCount = std::size(kEntryPoints);
enum EntryIndex {
    E_INIT, E_BSDF, E_EDF, E_INTENSITY, E_CUTOUT, E_THIN_WALLED,
    E_BACK_BSDF, E_BACK_EDF, E_BACK_INTENSITY
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
    default: return true;
    }
}

std::string ProgramFunction(uint32_t program, EntryIndex entry)
{
    return std::format("prism_p{}_{}", program, kEntryPoints[entry].name);
}

// The prism_mdl_* dispatch functions. Types are spelled the same in both
std::string GenerateDispatch(std::vector<std::array<bool, kEntryPointCount>> const& present,
                             std::vector<ProgramTraits> const& traits)
{
    const char* state = "Shading_state_material";
    const char* inoutState = "inout";
    std::string s;
    auto cases = [&](EntryIndex entry, const char* call, const char* fallback) {
        std::string body = "    switch (program) {\n";
        for (size_t p = 0; p < present.size(); ++p) {
            if (!present[p][entry]) continue;
            const std::string function = ProgramFunction(uint32_t(p), entry);
            body += std::format("    case {}u: {}\n", p,
                std::vformat(call, std::make_format_args(function)));
        }
        body += std::format("    default: {}\n    }}\n", fallback);
        return body;
    };
    s += std::format("void prism_mdl_init(uint program, {} {} state)\n{{\n", inoutState, state);
    s += cases(E_INIT, "{}(state); break;", "break;");
    s += "}\n\n";
    // The cutout path's own init: only programs with a varying cutout.
    s += std::format("void prism_mdl_init_cutout(uint program, {} {} state)\n{{\n", inoutState, state);
    {
        std::string body = "    switch (program) {\n";
        for (size_t p = 0; p < present.size(); ++p)
            if (present[p][E_CUTOUT])
                body += std::format("    case {}u: {}(state); break;\n", p, ProgramFunction(uint32_t(p), E_INIT));
        s += body + "    default: break;\n    }\n}\n\n";
    }
    s += std::format("bool prism_mdl_thin_walled(uint program, {} {} state)\n{{\n", inoutState, state);
    {
        std::string body = "    switch (program) {\n";
        for (size_t p = 0; p < present.size(); ++p) {
            if (present[p][E_THIN_WALLED])
                body += std::format("    case {}u: return {}(state);\n", p, ProgramFunction(uint32_t(p), E_THIN_WALLED));
            else if (traits[p].thinWalled == 1)
                body += std::format("    case {}u: return true;\n", p);
        }
        s += body + "    default: return false;\n    }\n}\n\n";
    }
    s += std::format("float prism_mdl_cutout(uint program, {} {} state)\n{{\n", inoutState, state);
    s += cases(E_CUTOUT, "return {}(state);", "return 1.0;");
    s += "}\n\n";
    s += std::format("float3 prism_mdl_emission_intensity(uint program, bool backface, {} {} state)\n{{\n",
                     inoutState, state);
    s += "    if (backface) {\n" + cases(E_BACK_INTENSITY, "return {}(state);", "break;") + "    }\n";
    s += "    else {\n" + cases(E_INTENSITY, "return {}(state);", "break;") + "    }\n";
    s += "    return float3(0.0, 0.0, 0.0);\n}\n\n";
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
    dfDispatch("prism_mdl_bsdf_sample", "Bsdf_sample_data", E_BSDF, E_BACK_BSDF, "_sample");
    dfDispatch("prism_mdl_bsdf_evaluate", "Bsdf_evaluate_data", E_BSDF, E_BACK_BSDF, "_evaluate");
    dfDispatch("prism_mdl_edf_evaluate", "Edf_evaluate_data", E_EDF, E_BACK_EDF, "_evaluate");
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
                "|mdl-" MI_NEURAYLIB_PRODUCT_VERSION_STRING "|v1") + ".key");
}

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
        result.code = GenerateDispatch({}, {});
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
        result.code = GenerateDispatch({}, {});
        transaction->abort();
        return result;
    };
    if (!backend) return fail("HLSL backend unavailable");
    backend->set_option("num_texture_spaces", "1");
    backend->set_option("num_texture_results", std::to_string(kGpuTextureResults).c_str());
    backend->set_option("enable_auxiliary", "off");
    backend->set_option("texture_runtime_with_derivs", "on");
    backend->set_option("enable_ro_segment", "on");
    backend->set_option("max_const_data", "0");
    backend->set_option("df_handle_slot_mode", "none");
    backend->set_option("libbsdf_flags_in_bsdf_data", "off");

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
                mi::base::Handle<const mi::neuraylib::ICompiled_material> compiled(
                    transaction->access<mi::neuraylib::ICompiled_material>(
                        programs[p]->compiled_material_name.c_str()));
                if (compiled)
                    key = StructuralKey(backend.get(), transaction.get(), executionContext.get(),
                                        compiled.get(), TraitsOf(compiled.get()));
                if (const char* dir = std::getenv("PRISM_GPU_DUMP_PROGRAMS"); dir && *dir)
                    std::ofstream(std::filesystem::path(dir) / std::format("program{}.hlsl", p)) << key;
                if (key.empty()) key = "unique:" + programs[p]->compiled_material_name;
                else {
                    key = Digest(key);
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

    result.code = code->get_code();
    result.code += "\n// ---- Prism material dispatch ----\n";
    result.code += GenerateDispatch(present, traits);
    transaction->commit();
    result.success = true;
    return result;
}

std::shared_ptr<const MdlShaderCode> CachedMdlShaderCode(std::vector<MDLMaterial const*> const& materials)
{
    // The key holds the bindings, so their addresses cannot be reused while cached.
    using Key = std::vector<std::shared_ptr<const mdl::MaterialBinding>>;
    Key key;
    key.reserve(materials.size());
    for (MDLMaterial const* material : materials)
        key.push_back(material && material->is_valid() ? material->get_target_code().binding : nullptr);
    static std::mutex mutex;
    static std::vector<std::pair<Key, std::shared_ptr<const MdlShaderCode>>> cache;
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

} // namespace prism::device::gpu
