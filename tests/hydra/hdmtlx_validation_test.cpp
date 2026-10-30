// hdMtlx's validation warning (src/materialx/hdmtlx_document.h): a network
// authored with types its MaterialX definitions do not declare (a vector3
// multiply into a color3 base_color, an input authored as float where the
// definition declares vector3) converts to the same document as before, but
// its "Validation warnings for generated MaterialX file" warning is handed to
// the caller instead of being posted; DocumentLoader repairs the document
// the same way. Other warnings hdMtlx posts are posted as before.

#include "../../src/materialx/hdmtlx_document.h"
#include "../../src/materialx/loader.h"

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/diagnosticTrap.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialNetwork2Interface.h>

#include <MaterialXFormat/XmlIo.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
int failures = 0;

void Expect(bool condition, std::string const& what)
{
    if (condition) return;
    std::cerr << "FAILED: " << what << "\n";
    ++failures;
}

size_t ValidationWarnings(TfDiagnosticTrap const& trap)
{
    return trap.CountMatching([](TfWarning const& warning) {
        return std::string_view(warning.GetCommentary()).starts_with(prism::materialx::kHdMtlxValidationWarning);
    });
}

// standard_surface whose base_color is a color3 constant through a vector3
// multiply (in1 typed after the color3 node feeding it, in2 authored as a
// float), as Houdini networks for older MaterialX versions are. `unknown`
// adds a node without a definition.
HdMaterialNetwork2 MismatchedNetwork(bool unknown = false)
{
    HdMaterialNetwork2 network;
    const SdfPath color("/Looks/M/color");
    const SdfPath multiply("/Looks/M/multiply");
    const SdfPath convert("/Looks/M/convert");
    const SdfPath surface("/Looks/M/surface");
    HdMaterialNode2& constant = network.nodes[color];
    constant.nodeTypeId = TfToken("ND_constant_color3");
    constant.parameters[TfToken("value")] = VtValue(GfVec3f(0.5f, 0.25f, 0.75f));
    HdMaterialNode2& mult = network.nodes[multiply];
    mult.nodeTypeId = TfToken("ND_multiply_vector3");
    mult.parameters[TfToken("in2")] = VtValue(0.8f);
    mult.parameters[TfToken("typeName:in2")] = VtValue(TfToken("float"));
    mult.inputConnections[TfToken("in1")] = {{color, TfToken("out")}};
    HdMaterialNode2& conv = network.nodes[convert];
    conv.nodeTypeId = TfToken("ND_convert_vector3_color3");
    conv.inputConnections[TfToken("in")] = {{multiply, TfToken("out")}};
    HdMaterialNode2& shader = network.nodes[surface];
    shader.nodeTypeId = TfToken("ND_standard_surface_surfaceshader");
    shader.parameters[TfToken("specular_roughness")] = VtValue(0.3f);
    shader.inputConnections[TfToken("base_color")] = {{convert, TfToken("out")}};
    if (unknown) {
        const SdfPath custom("/Looks/M/custom");
        network.nodes[custom].nodeTypeId = TfToken("ND_prism_no_such_node_color3");
        shader.inputConnections[TfToken("specular_color")] = {{custom, TfToken("out")}};
    }
    network.terminals[TfToken("surface")] = {surface, TfToken("out")};
    return network;
}

MaterialX::DocumentPtr Convert(HdMaterialNetwork2& network, bool capture,
                               prism::materialx::HdMtlxValidationReport* report)
{
    HdMaterialNetwork2Interface net(SdfPath("/Looks/M"), &network);
    const auto terminal = net.GetTerminalConnection(TfToken("surface"));
    HdMtlxTexturePrimvarData data;
    const auto connections = net.GetNodeInputConnectionNames(terminal.second.upstreamNodeName);
    if (capture)
        return prism::materialx::CreateHdMtlxDocument(&net, terminal.second.upstreamNodeName, connections,
                                                      HdMtlxStdLibraries(), &data, report);
    return HdMtlxCreateMtlxDocumentFromHdMaterialNetworkInterface(&net, terminal.second.upstreamNodeName,
                                                                   connections, HdMtlxStdLibraries(), &data);
}

std::string Repaired(MaterialX::DocumentPtr const& doc, std::filesystem::path const& path,
                     DocumentLoader::RepairCounts* repairs)
{
    { std::ofstream(path) << MaterialX::writeToXmlString(doc); }
    DocumentLoader loader(HdMtlxStdLibraries(), true);
    loader.setLabel("/Looks/M");
    auto repaired = loader.load(path.string());
    if (repairs) *repairs = loader.repairs();
    return repaired ? MaterialX::writeToXmlString(repaired) : std::string();
}
} // namespace

int main()
{
    const auto scratch = std::filesystem::temp_directory_path() /
        ("prism-hdmtlx-validation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch);

    // As hdMtlx reports it: the validation warning is posted.
    std::string stockXml;
    MaterialX::DocumentPtr stock;
    {
        TfDiagnosticTrap trap;
        HdMaterialNetwork2 network = MismatchedNetwork();
        stock = Convert(network, false, nullptr);
        Expect(bool(stock), "hdMtlx converts the mismatched network");
        Expect(ValidationWarnings(trap) == 1, "hdMtlx posts its validation warning (the fixture is mismatched)");
        trap.Clear();
        if (stock) stockXml = MaterialX::writeToXmlString(stock);
    }

    // Captured: nothing reaches the enclosing trap, the report lists the issues,
    // and the document is the same.
    MaterialX::DocumentPtr captured;
    {
        TfDiagnosticTrap outer;
        prism::materialx::HdMtlxValidationReport report;
        HdMaterialNetwork2 network = MismatchedNetwork();
        captured = Convert(network, true, &report);
        Expect(bool(captured), "the captured conversion converts");
        Expect(outer.IsClean(), "no diagnostic escapes the captured conversion");
        Expect(report.issues >= 1, "the report counts the validation issues");
        Expect(report.text.find("doesn't match declaration") != std::string::npos,
               "the report keeps MaterialX's findings: " + report.text);
        Expect(captured && MaterialX::writeToXmlString(captured) == stockXml, "the same document as hdMtlx's");
    }

    // DocumentLoader repairs both the same way.
    if (stock && captured) {
        DocumentLoader::RepairCounts repairs;
        const std::string fromStock = Repaired(stock, scratch / "stock.mtlx", nullptr);
        const std::string fromCaptured = Repaired(captured, scratch / "captured.mtlx", &repairs);
        Expect(!fromStock.empty() && fromStock == fromCaptured, "the same repaired document");
        Expect(repairs.connectionTypes >= 1, "the vector3 into color3 connection is repaired");
    }

    // Other warnings are posted as before.
    {
        TfDiagnosticTrap outer;
        prism::materialx::HdMtlxValidationReport report;
        HdMaterialNetwork2 network = MismatchedNetwork(true);
        auto doc = Convert(network, true, &report);
        Expect(bool(doc), "a network with an unknown node converts");
        Expect(outer.HasAnyMatching([](TfWarning const& warning) {
                   return warning.GetCommentary().find("NodeDef not found") != std::string::npos;
               }), "hdMtlx's missing-definition warning is posted");
        Expect(ValidationWarnings(outer) == 0, "the validation warning is still captured");
        outer.Clear();
    }

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    if (failures) return 1;
    std::cout << "hdMtlx validation capture: OK\n";
    return 0;
}
