#pragma once

#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/base/gf/vec3f.h>
#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace prism::materialx {
namespace mx = MaterialX;
PXR_NAMESPACE_USING_DIRECTIVE

// A single authored OpenPBR node has no graph conversion to perform. Build
// its small instance document against the immutable library instead of copying
// every standard definition into hundreds of color-only sphere materials.
inline mx::DocumentPtr SimpleOpenPbrDocument(HdMaterialNetworkInterface& net,
                                     TfToken const& terminal, const mx::DocumentPtr& library) {
    const auto names = net.GetNodeNames();
    if (names.size() != 1 || names.front() != terminal ||
        !net.GetNodeInputConnectionNames(terminal).empty()) return {};
    const auto definition = HdMtlxGetNodeDef(net.GetNodeType(terminal), library);
    if (!definition || definition->getNodeString() != "open_pbr_surface") return {};
    auto doc = mx::createDocument();
    doc->setDataLibrary(library);
    auto node = doc->addNode("open_pbr_surface", HdMtlxGetMxTerminalName(definition->getType()), definition->getType());
    for (const auto& name : net.GetAuthoredNodeParameterNames(terminal)) {
        // The legacy network interface lists metadata alongside values. Its
        // GetNodeParameterData folds these entries into the actual parameter.
        // Scene-index interfaces expose metadata separately.
        if (name.GetString().starts_with("typeName:") ||
            name.GetString().starts_with("colorSpace:")) continue;
        const auto data = net.GetNodeParameterData(terminal, name);
        const auto declaration = definition->getActiveInput(name.GetString());
        if (!declaration) return {};
        auto input = node->addInput(name.GetString(), declaration->getType());
        const auto& value = data.value;
        if (value.IsHolding<float>()) input->setValue(value.UncheckedGet<float>());
        else if (value.IsHolding<int>()) input->setValue(value.UncheckedGet<int>());
        else if (value.IsHolding<bool>()) input->setValue(value.UncheckedGet<bool>());
        else if (value.IsHolding<GfVec3f>()) {
            const auto v=value.UncheckedGet<GfVec3f>();
            if (declaration->getType()=="color3") input->setValue(mx::Color3(v[0],v[1],v[2]));
            else if (declaration->getType()=="vector3") input->setValue(mx::Vector3(v[0],v[1],v[2]));
            else return {};
        } else return {};
        if (!data.colorSpace.IsEmpty()) input->setColorSpace(data.colorSpace.GetString());
    }
    doc->addMaterialNode("PrismMaterial", node);
    return doc;
}

inline std::string SerializeDocument(const mx::DocumentPtr& doc,
                              const std::string& installedLibraries)
{
    mx::XmlWriteOptions options;
    std::string root = std::filesystem::path(installedLibraries).generic_string();
#ifdef _WIN32
    std::transform(root.begin(), root.end(), root.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    if (!root.empty() && root.back() != '/') root.push_back('/');
    options.elementPredicate = [root](mx::ConstElementPtr element) {
        if (root.empty()) return true;
        std::string source = std::filesystem::path(
            element->getSourceUri()).generic_string();
#ifdef _WIN32
        std::transform(source.begin(), source.end(), source.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
        // Worker-local generators already own this immutable standard library.
        // Keep user and hdMtlx includes whose source URIs are outside it.
        return source.empty() || source.rfind(root, 0) != 0;
    };
    return mx::writeToXmlString(doc, &options);
}

} // namespace prism::materialx
