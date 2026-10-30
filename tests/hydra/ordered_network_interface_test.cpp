// The material service's network interface (src/materialx/ordered_network_interface.h):
// once a node is edited (the document colour space tags its colours, a texture
// path is resolved), HdDataSourceMaterialNetworkInterface lists its parameters
// and connections in TfToken hash order, i.e. by token address, which changes
// from process to process. hdMtlx creates inputs and nodes in that order, so
// the MaterialEggs countertop compiled differently ordered documents, and
// rendered a different image, in about one run in five. The ordered interface
// keeps the network's own order; this test edits a network as the material
// service does and checks the names and the converted document keep it.

#include "../../src/materialx/ordered_network_interface.h"
#include "../../src/materialx/hdmtlx_document.h"
#include "../../src/usd/mtlx_color_space.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/dataSourceMaterialNetworkInterface.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/utils.h>
#include <pxr/imaging/hdMtlx/hdMtlx.h>

#include <MaterialXCore/Document.h>
#include <MaterialXFormat/XmlIo.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;

void Expect(bool condition, std::string const& what)
{
    if (condition) return;
    std::cerr << "FAILED: " << what << "\n";
    ++failures;
}

std::string Join(TfTokenVector const& names)
{
    std::string text;
    for (TfToken const& name : names) text += name.GetString() + " ";
    return text;
}

const SdfPath kMaterial("/Looks/M");
const SdfPath kSurface("/Looks/M/surface");

// A standard_surface with many colour and float values and a constant node
// on each of several inputs, as UsdImaging hands the CLI its networks.
HdContainerDataSourceHandle Network()
{
    HdMaterialNetwork network;
    HdMaterialNode surface;
    surface.path = kSurface;
    surface.identifier = TfToken("ND_standard_surface_surfaceshader");
    const char* colours[] = {"base_color", "specular_color", "subsurface_color", "subsurface_radius",
                             "sheen_color", "coat_color", "emission_color", "transmission_color",
                             "transmission_scatter"};
    float shade = 0.05f;
    for (const char* name : colours) {
        surface.parameters[TfToken(name)] = VtValue(GfVec3f(shade, 0.5f, 1.0f - shade));
        shade += 0.07f;
    }
    const char* floats[] = {"base", "specular", "subsurface", "sheen", "coat", "emission",
                            "transmission", "metalness", "diffuse_roughness", "coat_roughness"};
    for (const char* name : floats) surface.parameters[TfToken(name)] = VtValue(0.25f);
    network.nodes.push_back(surface);
    // Connections in a non-alphabetical order, which the network keeps.
    const char* inputs[] = {"specular_roughness", "base_color", "subsurface_scale", "coat_IOR",
                            "specular_IOR", "sheen_roughness"};
    for (const char* input : inputs) {
        HdMaterialNode constant;
        constant.path = kMaterial.AppendChild(TfToken(std::string("c_") + input));
        const bool colour = std::string(input) == "base_color";
        constant.identifier = TfToken(colour ? "ND_constant_color3" : "ND_constant_float");
        constant.parameters[TfToken("value")] = colour ? VtValue(GfVec3f(0.3f, 0.2f, 0.1f)) : VtValue(0.4f);
        network.nodes.insert(network.nodes.begin(), constant);
        network.relationships.push_back({constant.path, TfToken("out"), kSurface, TfToken(input)});
    }
    HdMaterialNetworkMap map;
    map.map[HdMaterialTerminalTokens->surface] = network;
    map.terminals.push_back(kSurface);
    return HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(map);
}

// Tags every colour, then edits the surface as texture resolution does:
// a parameter rewritten, one deleted, two added, a connection re-set.
template <class Interface>
void Edit(Interface& net, bool reverse)
{
    prism::usd::ApplyMtlxDocumentColorSpace(net, TfToken("srgb_texture"));
    TfTokenVector names = net.GetAuthoredNodeParameterNames(kSurface.GetToken());
    if (reverse) std::reverse(names.begin(), names.end());
    for (TfToken const& name : names)
        net.SetNodeParameterData(kSurface.GetToken(), name,
                                 net.GetNodeParameterData(kSurface.GetToken(), name));
    net.DeleteNodeParameter(kSurface.GetToken(), TfToken("base"));
    net.SetNodeParameterValue(kSurface.GetToken(), TfToken(reverse ? "zz_added" : "aa_added"), VtValue(0.5f));
    net.SetNodeParameterValue(kSurface.GetToken(), TfToken(reverse ? "aa_added" : "zz_added"), VtValue(0.5f));
    const TfToken input("subsurface_scale");
    net.SetNodeInputConnection(kSurface.GetToken(), input,
                               net.GetNodeInputConnection(kSurface.GetToken(), input));
}

std::string Convert(HdMaterialNetworkInterface& net)
{
    const auto terminal = net.GetTerminalConnection(HdMaterialTerminalTokens->surface);
    HdMtlxTexturePrimvarData data;
    auto doc = prism::materialx::CreateHdMtlxDocument(
        &net, terminal.second.upstreamNodeName,
        net.GetNodeInputConnectionNames(terminal.second.upstreamNodeName),
        HdMtlxStdLibraries(), &data, nullptr);
    return doc ? MaterialX::writeToXmlString(doc) : std::string();
}

// The order of the surface's inputs in a converted document.
std::vector<std::string> SurfaceInputs(std::string const& xml)
{
    std::vector<std::string> names;
    auto doc = MaterialX::createDocument();
    MaterialX::readFromXmlString(doc, xml);
    for (auto const& node : doc->getNodes())
        if (node->getCategory() == "standard_surface")
            for (auto const& input : node->getInputs()) names.push_back(input->getName());
    return names;
}
} // namespace

int main()
{
    const HdContainerDataSourceHandle network = Network();
    const TfToken surface = kSurface.GetToken();
    HdDataSourceMaterialNetworkInterface pristine(kMaterial, network, nullptr);
    const TfTokenVector sourceParameters = pristine.GetAuthoredNodeParameterNames(surface);
    const TfTokenVector sourceConnections = pristine.GetNodeInputConnectionNames(surface);
    const TfTokenVector sourceNodes = pristine.GetNodeNames();
    Expect(sourceParameters.size() == 19 && sourceConnections.size() == 6 && sourceNodes.size() == 7,
           "the fixture network has its parameters, connections and nodes");

    // Edited, the names keep the network's order; additions follow, sorted.
    TfTokenVector expected;
    for (TfToken const& name : sourceParameters)
        if (name != TfToken("base")) expected.push_back(name);
    expected.push_back(TfToken("aa_added"));
    expected.push_back(TfToken("zz_added"));
    for (bool reverse : {false, true}) {
        prism::materialx::OrderedMaterialNetworkInterface ordered(kMaterial, network, nullptr);
        Edit(ordered, reverse);
        const std::string label = reverse ? " (edited in reverse)" : "";
        Expect(ordered.GetAuthoredNodeParameterNames(surface) == expected,
               "edited parameters keep the network's order" + label + ": " +
               Join(ordered.GetAuthoredNodeParameterNames(surface)));
        Expect(ordered.GetNodeInputConnectionNames(surface) == sourceConnections,
               "edited connections keep the network's order" + label + ": " +
               Join(ordered.GetNodeInputConnectionNames(surface)));
        Expect(ordered.GetNodeNames() == sourceNodes, "nodes keep the network's order" + label);
        Expect(ordered.GetTerminalNames() == pristine.GetTerminalNames(), "terminals keep their order" + label);
    }

    // The converted document lists the surface's inputs in the network's
    // order however the edits went, as the unedited network converts.
    const std::string unedited = Convert(pristine);
    Expect(!unedited.empty(), "the unedited network converts");
    std::vector<std::string> uneditedInputs = SurfaceInputs(unedited);
    std::erase(uneditedInputs, "base");
    uneditedInputs.push_back("aa_added");
    uneditedInputs.push_back("zz_added");
    std::string documents[2];
    for (bool reverse : {false, true}) {
        prism::materialx::OrderedMaterialNetworkInterface ordered(kMaterial, network, nullptr);
        Edit(ordered, reverse);
        documents[reverse] = Convert(ordered);
        Expect(SurfaceInputs(documents[reverse]) == uneditedInputs,
               std::string("the edited document keeps the network's input order") +
               (reverse ? " (edited in reverse)" : ""));
    }
    Expect(!documents[0].empty() && documents[0] == documents[1],
           "the converted document does not depend on the order of the edits");

    // For the record: the stock interface's order after the same edits.
    HdDataSourceMaterialNetworkInterface stock(kMaterial, network, nullptr);
    Edit(stock, false);
    std::cout << "stock interface after edits: "
              << (stock.GetAuthoredNodeParameterNames(surface) == expected ? "network order" : "hash order")
              << " (" << Join(stock.GetAuthoredNodeParameterNames(surface)) << ")\n";

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "ordered network interface: names and documents keep the network's order\n";
    return 0;
}
