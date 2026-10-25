#pragma once

// UsdPreviewSurface networks and displacement terminals for the MaterialX
// conversion of USD materials, shared by the Hydra delegate's material service
// (src/hydra/materials.cpp) and the standalone loader (src/mesh/usd_mesh.h).

#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>
#include <pxr/base/tf/diagnostic.h>
#include <pxr/base/tf/token.h>

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>

namespace prism::materialx {

inline PXR_NS::TfToken SurfaceTerminalName(PXR_NS::HdMaterialNetworkInterface& net)
{
    const PXR_NS::TfTokenVector terminals = net.GetTerminalNames();
    for (const PXR_NS::TfToken& terminal : terminals) {
        if (terminal == PXR_NS::TfToken("surface") || terminal == PXR_NS::TfToken("surfaceshader"))
            return terminal;
    }
    for (const PXR_NS::TfToken& terminal : terminals) {
        const auto connection = net.GetTerminalConnection(terminal);
        if (connection.first &&
            PXR_NS::HdMtlxGetMxTerminalName(&net, connection.second.upstreamNodeName) == "surfaceshader")
            return terminal;
    }
    return {};
}

// hdMtlx converts one terminal; a displacement terminal is converted on its
// own and merged into the surface document, its shader bound to the
// material's displacementshader input.
inline void AddDisplacementShader(PXR_NS::HdMaterialNetworkInterface& net, const MaterialX::DocumentPtr& doc,
                                  const MaterialX::DocumentPtr& library)
{
    PXR_NS::TfToken terminal;
    for (const PXR_NS::TfToken& name : net.GetTerminalNames())
        if (name == PXR_NS::TfToken("displacement")) terminal = name;
    if (terminal.IsEmpty()) return;
    const auto connection = net.GetTerminalConnection(terminal);
    if (!connection.first) return;
    PXR_NS::HdMtlxTexturePrimvarData data;
    MaterialX::DocumentPtr displacement = PXR_NS::HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(
        &net, connection.second.upstreamNodeName,
        net.GetNodeInputConnectionNames(connection.second.upstreamNodeName), library, &data);
    if (!displacement) return;
    auto sourceMaterials = displacement->getMaterialNodes();
    auto materials = doc->getMaterialNodes();
    if (sourceMaterials.empty() || materials.empty()) return;
    auto shaderInput = sourceMaterials.front()->getInput("displacementshader");
    if (!shaderInput || !shaderInput->hasNodeName()) return;
    for (const auto& child : displacement->getChildren()) {
        if (child == sourceMaterials.front()) continue;
        auto existing = doc->getChild(child->getName());
        if (!existing) {
            doc->addChildOfCategory(child->getCategory(), child->getName())->copyContentFrom(child);
        } else if (auto graph = child->asA<MaterialX::NodeGraph>()) {
            auto target = existing->asA<MaterialX::NodeGraph>();
            if (!target) continue;
            for (const auto& node : graph->getChildren())
                if (!target->getChild(node->getName()))
                    target->addChildOfCategory(node->getCategory(), node->getName())->copyContentFrom(node);
        }
    }
    auto input = materials.front()->getInput("displacementshader");
    if (!input) input = materials.front()->addInput("displacementshader", "displacementshader");
    input->setNodeName(shaderInput->getNodeName());
}

inline bool IsPreviewSurfaceNetwork(PXR_NS::HdMaterialNetworkInterface& net)
{
    const PXR_NS::TfToken terminalName = SurfaceTerminalName(net);
    if (terminalName.IsEmpty()) return false;
    const auto terminal = net.GetTerminalConnection(terminalName);
    if (!terminal.first) return false;
    const PXR_NS::TfToken surfaceType = net.GetNodeType(terminal.second.upstreamNodeName);
    // The universal USD context is supported when it is a Preview Surface
    // graph. hdMtlx supplies the installed UsdPreviewSurface/UsdUVTexture
    // definitions and performs graph conversion, including texture outputs.
    if (surfaceType == PXR_NS::TfToken("UsdPreviewSurface") ||
        surfaceType == PXR_NS::TfToken("ND_UsdPreviewSurface_surfaceshader"))
        return true;
    return false;
}

inline bool RewritePreviewSurfaceNetwork(PXR_NS::HdMaterialNetworkInterface& net)
{
    const PXR_NS::TfToken terminalName = SurfaceTerminalName(net);
    if (terminalName.IsEmpty()) return false;
    const auto terminal = net.GetTerminalConnection(terminalName);
    if (!terminal.first ||
        (net.GetNodeType(terminal.second.upstreamNodeName) != PXR_NS::TfToken("UsdPreviewSurface") &&
         net.GetNodeType(terminal.second.upstreamNodeName) != PXR_NS::TfToken("ND_UsdPreviewSurface_surfaceshader")))
        return false;

    auto mappedType = [](const PXR_NS::TfToken& type) -> PXR_NS::TfToken {
        const std::string value = type.GetString();
        if (value == "UsdPreviewSurface") return PXR_NS::TfToken("ND_UsdPreviewSurface_surfaceshader");
        if (value == "UsdUVTexture") return PXR_NS::TfToken("ND_UsdUVTexture_23");
        if (value == "UsdTransform2d") return PXR_NS::TfToken("ND_UsdTransform2d");
        const std::string readerPrefix = "UsdPrimvarReader_";
        if (value.rfind(readerPrefix, 0) == 0) {
            std::string typeName = value.substr(readerPrefix.size());
            if (typeName == "float2") typeName = "vector2";
            else if (typeName == "float3") typeName = "vector3";
            else if (typeName == "float4") typeName = "vector4";
            return PXR_NS::TfToken("ND_UsdPrimvarReader_" + typeName);
        }
        return {};
    };
    auto mappedOutput = [](const PXR_NS::TfToken& type, const PXR_NS::TfToken& output) {
        if ((type == PXR_NS::TfToken("UsdPreviewSurface") ||
             type == PXR_NS::TfToken("ND_UsdPreviewSurface_surfaceshader") ||
             type == PXR_NS::TfToken("UsdTransform2d") ||
             type == PXR_NS::TfToken("ND_UsdTransform2d") ||
             type == PXR_NS::TfToken("UsdPrimvarReader_float") ||
             type == PXR_NS::TfToken("UsdPrimvarReader_float2") ||
             type == PXR_NS::TfToken("UsdPrimvarReader_float3") ||
             type == PXR_NS::TfToken("UsdPrimvarReader_float4") ||
             type == PXR_NS::TfToken("ND_UsdPrimvarReader_float") ||
             type == PXR_NS::TfToken("ND_UsdPrimvarReader_vector2") ||
             type == PXR_NS::TfToken("ND_UsdPrimvarReader_vector3") ||
             type == PXR_NS::TfToken("ND_UsdPrimvarReader_vector4")) &&
            (output == PXR_NS::TfToken("surface") || output == PXR_NS::TfToken("result")))
            return PXR_NS::TfToken("out");
        return output;
    };

    for (const PXR_NS::TfToken& node : net.GetNodeNames()) {
        const PXR_NS::TfToken sourceType = net.GetNodeType(node);
        const PXR_NS::TfToken translated = mappedType(sourceType);
        if (!translated.IsEmpty()) net.SetNodeType(node, translated);
        else if (sourceType.GetString().rfind("ND_", 0) != 0) {
            TF_WARN("[Hydra Material] Unsupported node '%s' in UsdPreviewSurface graph",
                    sourceType.GetText());
            return false;
        }

        for (const PXR_NS::TfToken& input : net.GetNodeInputConnectionNames(node)) {
            auto connections = net.GetNodeInputConnection(node, input);
            for (auto& connection : connections) {
                const PXR_NS::TfToken upstreamType = net.GetNodeType(connection.upstreamNodeName);
                connection.upstreamOutputName =
                    mappedOutput(upstreamType, connection.upstreamOutputName);
            }
            net.SetNodeInputConnection(node, input, connections);
        }
    }

    PXR_NS::HdMaterialNetworkInterface::InputConnection translatedTerminal = terminal.second;
    translatedTerminal.upstreamOutputName = mappedOutput(
        net.GetNodeType(translatedTerminal.upstreamNodeName),
        translatedTerminal.upstreamOutputName);
    net.SetTerminalConnection(terminalName, translatedTerminal);
    return true;
}

// UsdUVTexture's wrap mode "repeat" is MaterialX's "periodic" (the Hydra
// delegate converts it while it resolves textures).
inline void RewritePreviewSurfaceWrapModes(PXR_NS::HdMaterialNetworkInterface& net)
{
    for (const PXR_NS::TfToken& node : net.GetNodeNames())
        for (const PXR_NS::TfToken& param : net.GetAuthoredNodeParameterNames(node)) {
            if (param != PXR_NS::TfToken("wrapS") && param != PXR_NS::TfToken("wrapT")) continue;
            auto wrap = net.GetNodeParameterData(node, param);
            if (wrap.value.IsHolding<PXR_NS::TfToken>() &&
                wrap.value.UncheckedGet<PXR_NS::TfToken>() == PXR_NS::TfToken("repeat")) {
                wrap.value = PXR_NS::TfToken("periodic");
                net.SetNodeParameterData(node, param, wrap);
            }
        }
}

inline void FixPreviewSurfaceNormalOutput(const MaterialX::DocumentPtr& doc)
{
    if (!doc) return;
    for (const MaterialX::NodePtr& node : doc->getNodes()) {
        if (node->getCategory() == "UsdPreviewSurface") {
            if (MaterialX::InputPtr normalInput = node->getInput("normal"))
                normalInput->setType("vector3");
        }
    }
    for (const MaterialX::NodeGraphPtr& graph : doc->getNodeGraphs()) {
        MaterialX::OutputPtr normal = graph->getOutput("normal_rgb");
        if (!normal || normal->getType() != "color3" || !normal->hasNodeName())
            continue;
        const std::string upstreamName = normal->getNodeName();
        const std::string upstreamOutput = normal->getOutputString();
        MaterialX::NodePtr convert = graph->addNode(
            "convert", "normal_rgb_to_vector3", "vector3");
        MaterialX::InputPtr input = convert->addInput("in", "color3");
        input->setNodeName(upstreamName);
        input->setOutputString(upstreamOutput);
        normal->setNodeName(convert->getName());
        normal->removeAttribute("output");
        normal->setType("vector3");
    }
}

} // namespace prism::materialx
