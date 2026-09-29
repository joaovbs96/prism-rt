#pragma once

#include "captured_material.h"
#include "resolved_resource.h"
#include "udim_resource.h"
#include "../materialx/mdl_generator.h"
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
    // DocumentLoader deliberately forces unsupported SSS to zero. Keeping the
    // authored value structural prevents a bound variant from bypassing that
    // preprocessing rule.
    if ((type.find("standard_surface")!=std::string::npos && name=="subsurface") ||
        (type.find("open_pbr_surface")!=std::string::npos && name=="subsurface_weight"))
        return false;
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
    for (const auto& node : graph.nodes) for (const auto& parameter : node.parameters) {
        if (!IsCompatibleTemplateValue(node,parameter)) continue;
        const std::string parameterName=parameter.name.GetString();
        const std::string source=node.sourceName.IsEmpty()
            ? node.name.GetString() : node.sourceName.GetString();
        const std::string mxNode=PXR_NS::HdMtlxCreateNameFromPath(
            PXR_NS::SdfPath(source));
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

struct CompatibleBindResult {
    std::shared_ptr<MDLMaterial> material;
    uint64_t bytesRead = 0;
    std::string error;
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
    prism::PreparationAdmission::Reservation admission(
        prism::PreparationAdmission::Shared(),512ull*1024*1024);
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
        const CapturedParameter* parameter=nullptr;
        const CapturedNode* capturedNode=nullptr;
        for (const auto& node : graph.nodes) if (node.name==binding.node)
            for (const auto& candidate : node.parameters)
                if (candidate.name==binding.parameter) {
                    if (parameter) { result.error="ambiguous captured parameter"; return result; }
                    parameter=&candidate; capturedNode=&node;
                }
        if (!parameter) { result.error="captured parameter is missing"; return result; }
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
        std::lock_guard lock(m_mutex);
        for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
            auto prototype=it->prototype.lock();
            if (it->context == context && it->libraries == libraries &&
                it->mdlPath == mdlPath && it->structure == structure &&
                prototype && prototype->is_valid()) {
                *result = {it->context,it->libraries,it->mdlPath,it->structure,
                           std::move(prototype),it->bindings};
                return true;
            }
        }
        return false;
    }
    void publish(CompatibleMaterialTemplate entry) {
        std::lock_guard lock(m_mutex);
        std::erase_if(m_entries, [&](const auto& old) {
            return old.context == entry.context && old.libraries == entry.libraries &&
                old.mdlPath == entry.mdlPath && old.structure == entry.structure;
        });
        if (m_entries.size() >= 512) m_entries.erase(m_entries.begin());
        m_entries.push_back({entry.context,std::move(entry.libraries),
            std::move(entry.mdlPath),std::move(entry.structure),entry.prototype,
            std::move(entry.bindings)});
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
    };
    mutable std::mutex m_mutex;
    std::vector<StoredTemplate> m_entries;
};
}
