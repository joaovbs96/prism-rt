#pragma once

// Since OpenUSD 26.08, HdMtlxCreateNameFromPath names every MaterialX node
// after the full USD path of its shader ("World_Looks_M0_Shader"); 26.05 used
// the prim name ("Shader"). Path-dependent names make the generated
// MaterialX and MDL of otherwise identical materials differ, which defeats the
// content-addressed material caches and the compatible-template bindings.
// PrismRT therefore renames hdMtlx's nodes back to their prim name whenever
// that name is unique within the material network (the collision upstream's
// change guards against keeps the full-path name).

#include "pxr/imaging/hdMtlx/hdMtlx.h"
#include "pxr/usd/sdf/path.h"

#include <MaterialXCore/Document.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace prism::materialx {

using HdMtlxNameMap = std::unordered_map<std::string, std::string>;

// HdMtlxCreateNameFromPath(node) -> prim name, for every network node whose
// prim name is unique and differs from the hdMtlx name.
template <class NodeNames>
inline HdMtlxNameMap ShortHdMtlxNodeNames(const NodeNames& nodeNames)
{
    std::vector<PXR_NS::SdfPath> paths;
    std::unordered_map<std::string, int> counts;
    for (const auto& name : nodeNames) {
        PXR_NS::SdfPath path{std::string(name.GetString())};
        if (!path.IsPrimPath()) continue;
        ++counts[path.GetName()];
        paths.push_back(std::move(path));
    }
    HdMtlxNameMap result;
    for (const auto& path : paths) {
        const std::string& shortName = path.GetName();
        if (shortName.empty() || counts[shortName] != 1) continue;
        std::string full = PXR_NS::HdMtlxCreateNameFromPath(path);
        if (full != shortName) result.emplace(std::move(full), shortName);
    }
    return result;
}

// The MaterialX node name PrismRT's documents use for a network node.
inline std::string HdMtlxNodeName(const std::string& nodePath, const HdMtlxNameMap& shortNames)
{
    std::string full = PXR_NS::HdMtlxCreateNameFromPath(PXR_NS::SdfPath(nodePath));
    const auto found = shortNames.find(full);
    return found == shortNames.end() ? full : found->second;
}

// Applies ShortHdMtlxNodeNames to a document built by hdMtlx. Only the
// document's own elements are touched, never the imported libraries.
inline void ShortenHdMtlxNodeNames(const MaterialX::DocumentPtr& doc, const HdMtlxNameMap& shortNames)
{
    if (!doc || shortNames.empty()) return;
    std::vector<MaterialX::ElementPtr> scopes{doc};
    for (const auto& graph : doc->getNodeGraphs())
        if (!graph->hasSourceUri()) scopes.push_back(graph);
    HdMtlxNameMap renamed;
    for (const auto& scope : scopes) {
        for (const auto& child : scope->getChildren()) {
            if (child->hasSourceUri() || !child->isA<MaterialX::Node>()) continue;
            const auto found = shortNames.find(child->getName());
            if (found == shortNames.end() || scope->getChild(found->second)) continue;
            child->setName(found->second);
            renamed.emplace(found->first, found->second);
        }
    }
    if (renamed.empty()) return;
    for (const auto& scope : scopes) {
        for (const auto& child : scope->getChildren()) {
            if (child->hasSourceUri()) continue;
            std::vector<MaterialX::ElementPtr> ports{child};
            for (const auto& grandchild : child->getChildren()) ports.push_back(grandchild);
            for (const auto& element : ports) {
                auto port = element->asA<MaterialX::PortElement>();
                if (!port) continue;
                const auto found = renamed.find(port->getNodeName());
                if (found != renamed.end()) port->setNodeName(found->second);
            }
        }
    }
}

// Same, for a document converted from an HdMaterialNetwork2 (node map keyed by path).
template <class NodeMap>
inline void ShortenHdMtlxNetworkNodeNames(const MaterialX::DocumentPtr& doc, const NodeMap& nodes)
{
    std::vector<PXR_NS::SdfPath> names;
    names.reserve(nodes.size());
    for (const auto& entry : nodes) names.push_back(entry.first);
    ShortenHdMtlxNodeNames(doc, ShortHdMtlxNodeNames(names));
}

} // namespace prism::materialx
