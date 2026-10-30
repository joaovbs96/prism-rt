#pragma once

// A material network interface whose name lists keep the network's own order
// after edits.
//
// HdDataSourceMaterialNetworkInterface lists the parameters, input
// connections, nodes and terminals of an edited node (or network) from an
// std::unordered_set<TfToken, TfHash>: TfToken hashes are their addresses, so
// the order changes from process to process with the order in which threads
// created the tokens. hdMtlx builds a node's inputs and the nodegraph's nodes
// in that order, so the converted document, its generated MDL and the
// compiled class (and with it floating-point evaluation order) changed between
// runs of the same scene. The material service edits every network it
// converts (document colour space, resolved textures, preview rewrites), so
// it reads the network through this interface instead: names still in the
// unedited network keep its order, and names the edits added follow, sorted.

#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>

#include <algorithm>
#include <unordered_set>

namespace prism::materialx {

// `names` (what the edited interface lists) in the order of `source` (what the
// unedited network lists); names only in `names` follow, sorted by text.
inline PXR_NS::TfTokenVector InSourceOrder(PXR_NS::TfTokenVector const& names,
                                           PXR_NS::TfTokenVector const& source)
{
    using PXR_NS::TfToken;
    std::unordered_set<TfToken, TfToken::HashFunctor> present(names.begin(), names.end());
    PXR_NS::TfTokenVector result;
    result.reserve(names.size());
    std::unordered_set<TfToken, TfToken::HashFunctor> listed;
    for (TfToken const& name : source)
        if (present.contains(name) && listed.insert(name).second) result.push_back(name);
    const size_t kept = result.size();
    for (TfToken const& name : names)
        if (listed.insert(name).second) result.push_back(name);
    std::sort(result.begin() + kept, result.end(), [](TfToken const& a, TfToken const& b) {
        return a.GetString() < b.GetString();
    });
    return result;
}

class OrderedMaterialNetworkInterface : public PXR_NS::HdDataSourceMaterialNetworkInterface {
public:
    using Base = PXR_NS::HdDataSourceMaterialNetworkInterface;

    OrderedMaterialNetworkInterface(PXR_NS::SdfPath const& materialPrimPath,
                                    PXR_NS::HdContainerDataSourceHandle const& networkContainer,
                                    PXR_NS::HdContainerDataSourceHandle const& primContainer)
        : Base(materialPrimPath, networkContainer, primContainer)
        , m_source(materialPrimPath, networkContainer, primContainer)
    {}

    PXR_NS::TfTokenVector GetNodeNames() const override {
        return InSourceOrder(Base::GetNodeNames(), m_source.GetNodeNames());
    }
    PXR_NS::TfTokenVector GetAuthoredNodeParameterNames(PXR_NS::TfToken const& node) const override {
        return InSourceOrder(Base::GetAuthoredNodeParameterNames(node),
                             m_source.GetAuthoredNodeParameterNames(node));
    }
    PXR_NS::TfTokenVector GetNodeInputConnectionNames(PXR_NS::TfToken const& node) const override {
        return InSourceOrder(Base::GetNodeInputConnectionNames(node),
                             m_source.GetNodeInputConnectionNames(node));
    }
    PXR_NS::TfTokenVector GetTerminalNames() const override {
        return InSourceOrder(Base::GetTerminalNames(), m_source.GetTerminalNames());
    }

private:
    // The same network, never edited: the reference order.
    Base m_source;
};

} // namespace prism::materialx
