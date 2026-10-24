#pragma once

#include "captured_material.h"
#include "hdmtlx_names.h"
#include "resolved_resource.h"
#include "udim_resource.h"
#include "../materialx/mdl_generator.h"
#include "../color/color_management.h"
#include "../mdl_material.h"
#include "../resource_limits.h"
#include <pxr/base/gf/vec3f.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <set>
#include <unordered_map>

namespace prism::materialx {

struct CompatibleBinding {
    PXR_NS::TfToken node, parameter;
    std::string uniform, type;
    // The uniform was folded into the prototype's program. The template then
    // serves only materials whose captured value equals foldedValue.
    bool folded = false;
    PXR_NS::VtValue foldedValue;
};
struct CompatibleMaterialTemplate {
    mdl::Context* context = nullptr;
    std::string libraries, mdlPath;
    CapturedMaterialGraph structure;
    std::shared_ptr<MDLMaterial> prototype;
    std::vector<CompatibleBinding> bindings;
};

inline bool IsCompatibleTemplateValue(const CapturedParameter& parameter) {
    const auto& value=parameter.data.value;
    const bool filename=parameter.name==PXR_NS::TfToken("file") ||
                        parameter.name==PXR_NS::TfToken("filename");
    return value.IsHolding<float>() || value.IsHolding<PXR_NS::GfVec3f>() ||
        (filename && (value.IsHolding<std::string>() ||
                      value.IsHolding<PXR_NS::SdfAssetPath>()));
}
inline bool IsCompatibleTemplateValue(const CapturedNode& node,
                                      const CapturedParameter& parameter) {
    // Authored defaults remain present in the captured USD graph even when a
    // connection supplies the input. Conversion removes those defaults, so
    // they cannot be generated arguments and must not participate in binding.
    if (std::any_of(node.connections.begin(), node.connections.end(),
        [&](const CapturedConnection& connection) {
            return connection.input == parameter.name && !connection.connections.empty();
        })) return false;
    const std::string type=node.type.GetString();
    const std::string name=parameter.name.GetString();
    if ((name=="file" || name=="filename") &&
        type.find("UsdUVTexture")==std::string::npos &&
        type.find("image_")==std::string::npos)
        return false;
    // DocumentLoader may force SSS to zero (PRISM_SSS_FALLBACK). Keeping the
    // authored value structural prevents a bound variant from bypassing that
    // preprocessing rule.
    if ((type.find("standard_surface")!=std::string::npos && name=="subsurface") ||
        (type.find("open_pbr_surface")!=std::string::npos && name=="subsurface_weight"))
        return false;
    // DocumentLoader gives an uncoated transmissive open_pbr_surface its
    // specular IOR as coat_ior, which becomes the MDL refraction IOR. The
    // values that decide and feed it stay structural, so a bound variant never
    // refracts with its seed's IOR.
    if (type.find("open_pbr_surface")!=std::string::npos &&
        (name=="specular_ior" || name=="coat_ior" || name=="coat_weight" || name=="transmission_weight")) {
        const PXR_NS::TfToken weight("transmission_weight");
        const bool connected=std::any_of(node.connections.begin(),node.connections.end(),
            [&](const CapturedConnection& c) { return c.input==weight && !c.connections.empty(); });
        const auto found=std::find_if(node.parameters.begin(),node.parameters.end(),
            [&](const CapturedParameter& p) { return p.name==weight; });
        const bool transmissive=connected || (found!=node.parameters.end() &&
            !(found->data.value.IsHolding<float>() && found->data.value.UncheckedGet<float>()==0.0f));
        if (transmissive) return false;
    }
    // DocumentLoader couples the reflection and transmission of transmissive
    // surfaces whose lobes agree (CoupleTransmissionFresnel in loader.cpp);
    // the weight and the inputs it checks stay structural, so a variant never
    // binds onto a prototype coupled (or not) for its seed's values.
    struct Coupling { const char* type; const char* weight; std::initializer_list<const char*> inputs; };
    static const Coupling couplings[] = {
        {"open_pbr_surface", "transmission_weight",
         {"specular_color", "thin_film_weight", "transmission_color", "transmission_depth"}},
        {"standard_surface", "transmission",
         {"specular", "specular_color", "transmission_color", "transmission_extra_roughness", "thin_film_thickness"}},
        {"gltf_pbr", "transmission", {"base_color", "specular", "specular_color", "iridescence"}},
    };
    for (auto const& coupling : couplings) {
        if (type.find(coupling.type)==std::string::npos) continue;
        if (name==coupling.weight) return false;
        if (std::find_if(coupling.inputs.begin(),coupling.inputs.end(),
                [&](const char* input) { return name==input; })==coupling.inputs.end()) break;
        const PXR_NS::TfToken weight(coupling.weight);
        const bool connected=std::any_of(node.connections.begin(),node.connections.end(),
            [&](const CapturedConnection& c) { return c.input==weight && !c.connections.empty(); });
        const auto found=std::find_if(node.parameters.begin(),node.parameters.end(),
            [&](const CapturedParameter& p) { return p.name==weight; });
        if (connected || (found!=node.parameters.end() &&
            !(found->data.value.IsHolding<float>() && found->data.value.UncheckedGet<float>()==0.0f)))
            return false;
        break;
    }
    return IsCompatibleTemplateValue(parameter);
}

inline bool CompatibleTemplateStructure(CapturedMaterialGraph graph,
                                         CapturedMaterialGraph* structure) {
    if (!structure || graph.nodes.empty() || graph.terminals.empty()) return false;
    size_t values=0;
    for (auto& node : graph.nodes) for (auto& parameter : node.parameters) {
        if (!IsCompatibleTemplateValue(node,parameter)) continue;
        parameter.data.value=PXR_NS::VtValue(); ++values;
    }
    if (!values) return false;
    *structure=std::move(graph); return true;
}

inline bool BuildCompatibleBindings(const CapturedMaterialGraph& graph,
                                    const std::vector<GeneratedUniform>& uniforms,
                                    std::vector<CompatibleBinding>* bindings) {
    if (!bindings) return false;
    bindings->clear();
    std::unordered_map<std::string,size_t> eligibleParameterCounts;
    for (const auto& node : graph.nodes) for (const auto& parameter : node.parameters)
        if (IsCompatibleTemplateValue(node,parameter))
            ++eligibleParameterCounts[parameter.name.GetString()];
    std::set<size_t> claimedUniforms;
    auto sourceOf=[](const CapturedNode& node) {
        return node.sourceName.IsEmpty() ? node.name : node.sourceName;
    };
    std::vector<PXR_NS::TfToken> sources;
    for (const auto& node : graph.nodes) sources.push_back(sourceOf(node));
    // The same naming the converted documents use (see hdmtlx_names.h).
    const auto shortNames=ShortHdMtlxNodeNames(sources);
    for (const auto& node : graph.nodes) for (const auto& parameter : node.parameters) {
        if (!IsCompatibleTemplateValue(node,parameter)) continue;
        const std::string parameterName=parameter.name.GetString();
        const std::string mxNode=HdMtlxNodeName(sourceOf(node).GetString(),shortNames);
        const std::string exactSuffix="/"+mxNode+"/"+parameterName;
        std::vector<size_t> matches;
        for (size_t i=0;i<uniforms.size();++i) {
            const auto& path=uniforms[i].path;
            if ((path==mxNode+"/"+parameterName ||
                 (path.size()>=exactSuffix.size() && path.ends_with(exactSuffix))) &&
                !claimedUniforms.contains(i))
                matches.push_back(i);
        }
        // Some generators expose a direct interface port without its source
        // node component. This is safe only when both graph and generated
        // parameter name are globally unique.
        if (matches.empty() && eligibleParameterCounts[parameterName]==1) {
            const std::string suffix="/"+parameterName;
            for (size_t i=0;i<uniforms.size();++i) {
                const auto& path=uniforms[i].path;
                if (!claimedUniforms.contains(i) &&
                    (path==parameterName ||
                     (path.size()>=suffix.size() && path.ends_with(suffix))))
                    matches.push_back(i);
            }
        }
        if (matches.size()!=1) return false;
        const size_t index=matches.front();
        const auto& match=uniforms[index];
        if (match.name.empty()) return false;
        claimedUniforms.insert(index);
        bindings->push_back({node.name,parameter.name,match.name,match.type});
    }
    return !bindings->empty();
}

inline const CapturedParameter* FindCapturedParameter(
    const CapturedMaterialGraph& graph, const CompatibleBinding& binding,
    const CapturedNode** owner, std::string* error) {
    const CapturedParameter* parameter=nullptr;
    for (const auto& node : graph.nodes) if (node.name==binding.node)
        for (const auto& candidate : node.parameters)
            if (candidate.name==binding.parameter) {
                if (parameter) { *error="ambiguous captured parameter"; return nullptr; }
                parameter=&candidate; if (owner) *owner=&node;
            }
    if (!parameter) *error="captured parameter is missing";
    return parameter;
}

// A folded program has baked in every generated uniform that is not one of its
// runtime parameters. Record the prototype's captured value for those bindings
// so a material with a different value can never reuse the program.
inline bool RecordFoldedBindings(const CapturedMaterialGraph& graph,
                                 const std::vector<mdl::CompiledProgram::Parameter>& parameters,
                                 std::vector<CompatibleBinding>* bindings) {
    if (!bindings) return false;
    for (auto& binding : *bindings) {
        if (std::any_of(parameters.begin(),parameters.end(),
                [&](const auto& parameter) { return parameter.name==binding.uniform; }))
            continue;
        std::string error;
        const CapturedParameter* parameter=FindCapturedParameter(graph,binding,nullptr,&error);
        if (!parameter || parameter->data.value.IsEmpty()) return false;
        binding.folded=true;
        binding.foldedValue=parameter->data.value;
    }
    return true;
}

struct CompatibleBindResult {
    std::shared_ptr<MDLMaterial> material;
    uint64_t bytesRead = 0;
    std::string error;
    // Every folded (node, parameter) whose captured value differs from the
    // template's; a recompile keeping them lets later edits bind again.
    std::vector<std::pair<PXR_NS::TfToken, PXR_NS::TfToken>> differingValues;
};

inline CompatibleBindResult BindCompatibleTemplate(
    const CompatibleMaterialTemplate& prepared,
    const CapturedMaterialGraph& graph, mdl::Context& context,
    const PXR_NS::ArResolverContext& resolverContext,
    const std::string& sourceVersion, const std::filesystem::path& cacheDir) {
    CompatibleBindResult result;
    if (!prepared.prototype || !prepared.prototype->is_valid()) {
        result.error="template prototype expired"; return result;
    }
    // The program computes with the prototype's folded values; binding any
    // other value would silently render a different material. Reject before
    // any SDK or resource work, reporting every differing value.
    for (const auto& binding : prepared.bindings) {
        if (!binding.folded) continue;
        const CapturedParameter* parameter=
            FindCapturedParameter(graph,binding,nullptr,&result.error);
        if (!parameter) { result.differingValues.clear(); return result; }
        if (parameter->data.value!=binding.foldedValue)
            result.differingValues.emplace_back(binding.node,binding.parameter);
    }
    if (!result.differingValues.empty()) {
        result.error="folded template parameters differ:";
        for (const auto& [node,parameter] : result.differingValues)
            result.error+=" "+node.GetString()+"."+parameter.GetString();
        return result;
    }
    prism::PreparationAdmission::Reservation admission(
        prism::PreparationAdmission::Shared(),prism::kMaterialBindReservationBytes);
    auto scope=mi::base::make_handle(context.get_database()->get_global_scope());
    auto transaction=mi::base::make_handle(scope ? scope->create_transaction() : nullptr);
    mi::base::Handle<mi::neuraylib::IValue_factory> values;
    if (transaction) values=context.get_factory()->create_value_factory(transaction.get());
    mi::base::Handle<mi::neuraylib::IValue_list> overrides;
    if (values) overrides=values->create_value_list();
    if (!transaction || !values || !overrides) {
        result.error="cannot create template binding transaction"; return result;
    }
    for (const auto& binding : prepared.bindings) {
        if (binding.folded) continue;
        const CapturedNode* capturedNode=nullptr;
        const CapturedParameter* parameter=
            FindCapturedParameter(graph,binding,&capturedNode,&result.error);
        if (!parameter) return result;
        mi::base::Handle<mi::neuraylib::IValue> value;
        const auto& source=parameter->data.value;
        if (source.IsHolding<float>()) value=values->create_float(source.UncheckedGet<float>());
        else if (source.IsHolding<int>()) value=values->create_int(source.UncheckedGet<int>());
        else if (source.IsHolding<bool>()) value=values->create_bool(source.UncheckedGet<bool>());
        else if (source.IsHolding<PXR_NS::GfVec3f>() && binding.type=="color3") {
            auto color=source.UncheckedGet<PXR_NS::GfVec3f>();
            const std::string type=capturedNode ? capturedNode->type.GetString() : "";
            const std::string name=parameter->name.GetString();
            const std::string space=parameter->data.colorSpace.GetString();
            if (type.find("standard_surface")!=std::string::npos &&
                (name=="base_color" || name=="specular_color") &&
                (space.empty() || space=="none" || space=="lin_rec709"))
                for (int i=0;i<3;++i) color[i]=std::clamp(color[i],0.0f,0.9999f);
            value=values->create_color(color[0],color[1],color[2]);
        } else if (source.IsHolding<PXR_NS::GfVec3f>() && binding.type=="vector3") {
            const auto vector=source.UncheckedGet<PXR_NS::GfVec3f>();
            mi::base::Handle<mi::neuraylib::IType_factory> types(
                values->get_type_factory());
            mi::base::Handle<const mi::neuraylib::IType_float> scalar(
                types ? types->create_float() : nullptr);
            mi::base::Handle<const mi::neuraylib::IType_vector> vectorType(
                types && scalar ? types->create_vector(scalar.get(),3) : nullptr);
            mi::base::Handle<mi::neuraylib::IValue_vector> vectorValue(
                vectorType ? values->create_vector(vectorType.get()) : nullptr);
            bool valid=bool(vectorValue);
            for (mi::Size i=0; valid && i<3; ++i) {
                mi::base::Handle<mi::neuraylib::IValue_float> component(
                    values->create_float(vector[static_cast<int>(i)]));
                valid=component && vectorValue->set_value(i,component.get())==0;
            }
            if (!valid) { result.error="cannot create vector3 template argument"; return result; }
            value=vectorValue;
        } else if ((source.IsHolding<std::string>() ||
                    source.IsHolding<PXR_NS::SdfAssetPath>()) && binding.type=="filename") {
            PXR_NS::ArResolverContextBinder binder(resolverContext);
            std::string path;
            if (source.IsHolding<std::string>()) path=source.UncheckedGet<std::string>();
            else {
                const auto& asset=source.UncheckedGet<PXR_NS::SdfAssetPath>();
                path=asset.GetResolvedPath().empty()?asset.GetAssetPath():asset.GetResolvedPath();
            }
            if (path.find("<UDIM>") != std::string::npos) {
                std::string authored=path, resolvedPattern;
                if (source.IsHolding<PXR_NS::SdfAssetPath>()) {
                    const auto& asset=source.UncheckedGet<PXR_NS::SdfAssetPath>();
                    authored=asset.GetAssetPath(); resolvedPattern=asset.GetResolvedPath();
                }
                std::string extension=std::filesystem::path(authored).extension().string();
                auto family=PrepareMaterialUdimResource(authored,resolvedPattern,
                    resolverContext,sourceVersion,cacheDir,extension);
                if (family.mdlPath.empty()) {
                    result.error="template UDIM family could not be enumerated"; return result;
                }
                result.bytesRead+=family.bytesRead;
                value=context.get_factory()->create_texture(transaction.get(),
                    family.mdlPath.c_str(),mi::neuraylib::IType_texture::TS_2D,
                    1.0f,nullptr,true,nullptr);
                if (!value || overrides->add_value(binding.uniform.c_str(),value.get())!=0) {
                    result.error="cannot create template UDIM argument override"; return result;
                }
                continue;
            }
            const auto resolved=PXR_NS::ArGetResolver().Resolve(path);
            if (resolved.empty()) { result.error="template texture did not resolve"; return result; }
            std::string extension=std::filesystem::path(path).extension().string();
            if (extension.empty()) extension=".bin";
            MaterialResourceResult resource;
            try { resource=PrepareMaterialResource(
                resolved,resolverContext,sourceVersion,cacheDir,extension); }
            catch (const std::exception& error) { result.error=error.what(); return result; }
            if (!resource.resource) { result.error="template texture preparation failed"; return result; }
            result.bytesRead+=resource.bytesRead;
            const std::string absolute="/"+
                resource.resource->path.filename().generic_string();
            value=context.get_factory()->create_texture(transaction.get(),absolute.c_str(),
                mi::neuraylib::IType_texture::TS_2D,1.0f,nullptr,true,nullptr);
        } else { result.error="unsupported template parameter type"; return result; }
        if (!value || overrides->add_value(binding.uniform.c_str(),value.get())!=0) {
            result.error="cannot create template argument override"; return result;
        }
    }
    mdl::BackendCompiler backend(context);
    auto bound=backend.bind_arguments(prepared.prototype->get_target_code(),
                                      overrides.get(),transaction.get());
    transaction->commit(); transaction.reset();
    if (!bound.success) { result.error=bound.error_message; return result; }
    result.material=std::make_shared<MDLMaterial>(std::move(bound));
    return result;
}

class CompatibleTemplateRegistry {
public:
    static CompatibleTemplateRegistry& Shared() {
        static auto* value = new CompatibleTemplateRegistry;
        return *value;
    }
    bool find(mdl::Context* context, const std::string& libraries,
              const std::string& mdlPath,
              const CapturedMaterialGraph& structure,
              CompatibleMaterialTemplate* result) const {
        // Compiled code holds the working space's colour transforms.
        const std::string color=prism::color::Active()->identity();
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            auto prototype=it->prototype.lock();
            if (it->context == context && it->libraries == libraries && it->color == color &&
                it->mdlPath == mdlPath && it->structure == structure &&
                prototype && prototype->is_valid()) {
                *result = {it->context,it->libraries,it->mdlPath,it->structure,
                           std::move(prototype),it->bindings};
                return true;
            }
        }
        return false;
    }
    // A template is only reusable if bind_arguments can write its bindings:
    // every bound uniform (passed to bind_arguments unchanged) must be a
    // parameter of the compiled program, which then needs a class argument
    // block. A folded binding is instead checked against its recorded value.
    static bool IsBindable(const CompatibleMaterialTemplate& entry) {
        if (!entry.prototype || !entry.prototype->is_valid()) return false;
        const auto& target=entry.prototype->get_target_code();
        if (!target.binding || !target.program || !target.program->target_code) return false;
        const bool bindsArguments=std::any_of(entry.bindings.begin(),entry.bindings.end(),
            [](const CompatibleBinding& binding) { return !binding.folded; });
        if (bindsArguments && (!target.binding->argument_block ||
            target.program->target_code->get_argument_block_count()==0)) return false;
        const auto& parameters=target.program->parameters;
        return std::all_of(entry.bindings.begin(),entry.bindings.end(),
            [&](const CompatibleBinding& binding) {
                if (binding.folded) return !binding.foldedValue.IsEmpty();
                return std::any_of(parameters.begin(),parameters.end(),
                    [&](const auto& parameter) { return parameter.name==binding.uniform; });
            });
    }
    void publish(CompatibleMaterialTemplate entry) {
        if (!IsBindable(entry)) return;
        const std::string color=prism::color::Active()->identity();
        std::lock_guard lock(m_mutex);
        std::erase_if(m_entries, [&](const auto& old) {
            return old.context == entry.context && old.libraries == entry.libraries && old.color == color &&
                old.mdlPath == entry.mdlPath && old.structure == entry.structure;
        });
        if (m_entries.size() >= 512) m_entries.erase(m_entries.begin());
        m_entries.push_back({entry.context,std::move(entry.libraries),
            std::move(entry.mdlPath),std::move(entry.structure),entry.prototype,
            std::move(entry.bindings),color});
    }
    void eraseContext(mdl::Context* context) {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_entries,
            [&](const auto& entry) { return entry.context == context; });
    }
    void clear() { std::lock_guard lock(m_mutex); m_entries.clear(); }
private:
    struct StoredTemplate {
        mdl::Context* context = nullptr;
        std::string libraries, mdlPath;
        CapturedMaterialGraph structure;
        std::weak_ptr<MDLMaterial> prototype;
        std::vector<CompatibleBinding> bindings;
        std::string color;
    };
    mutable std::mutex m_mutex;
    std::vector<StoredTemplate> m_entries;
};
}
