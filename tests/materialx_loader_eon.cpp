// DocumentLoader tints EON's energy compensation (MaterialX's MDL library
// leaves oren_nayar_diffuse_bsdf's multiple-scattering lobe white): nodes
// with energy_compensation true use prism_oren_nayar_diffuse_bsdf, whose
// genmdl code passes the EON multiscatter tint, directly or in a private copy
// of the graph that has them (open_pbr_surface's base, LamaDiffuse). Against
// MaterialX's own library; skipped without it.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

MaterialX::DocumentPtr gLibrary;

MaterialX::DocumentPtr Load(const std::string& name, const std::string& body)
{
    const auto path = std::filesystem::current_path() / (name + ".mtlx");
    { std::ofstream out(path); out << "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n" << body << "</materialx>\n"; }
    DocumentLoader loader(gLibrary, false, false);
    auto doc = loader.load(path.string());
    std::filesystem::remove(path);
    require(bool(doc), name + ": load failed");
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>())
            require(bool(node->getNodeDef()), name + ": no definition for " + node->getNamePath());
    return doc;
}

const std::string kHelper = "prism_oren_nayar_diffuse_bsdf";

// The nodes of a category in the genmdl graph `node` uses.
std::vector<MaterialX::NodePtr> GraphNodes(const MaterialX::NodePtr& node, const std::string& category)
{
    auto graph = node->getImplementation("genmdl");
    require(graph && graph->isA<MaterialX::NodeGraph>(), node->getName() + " has no genmdl graph");
    return graph->asA<MaterialX::NodeGraph>()->getNodes(category);
}

void Direct()
{
    auto doc = Load("direct", R"(
  <oren_nayar_diffuse_bsdf name="On" type="BSDF">
    <input name="color" type="color3" value="0.9, 0.5, 0.2" />
    <input name="roughness" type="float" value="1" />
    <input name="energy_compensation" type="boolean" value="true" />
  </oren_nayar_diffuse_bsdf>
  <oren_nayar_diffuse_bsdf name="Off" type="BSDF">
    <input name="roughness" type="float" value="1" />
    <input name="energy_compensation" type="boolean" value="false" />
  </oren_nayar_diffuse_bsdf>
  <oren_nayar_diffuse_bsdf name="Default" type="BSDF" />
)");
    auto on = doc->getNode("On");
    require(on->getCategory() == kHelper && !on->getInput("energy_compensation") &&
                on->getInput("color")->getValueString() == "0.9, 0.5, 0.2",
            "direct: the compensated node uses the helper with its inputs");
    require(doc->getNode("Off")->getCategory() == "oren_nayar_diffuse_bsdf" &&
                doc->getNode("Default")->getCategory() == "oren_nayar_diffuse_bsdf",
            "direct: uncompensated nodes are unchanged");
    auto implementation = doc->getImplementation("IM_" + kHelper + "_genmdl");
    require(implementation && implementation->getAttribute("sourcecode").find("multiscatter_tint") != std::string::npos,
            "direct: the helper's genmdl code sets the multiscatter tint");
}

void OpenPbr()
{
    auto doc = Load("open_pbr", R"(
  <open_pbr_surface name="Opaque" type="surfaceshader">
    <input name="base_diffuse_roughness" type="float" value="0.5" />
  </open_pbr_surface>
  <open_pbr_surface name="Glass" type="surfaceshader">
    <input name="base_diffuse_roughness" type="float" value="0.3" />
    <input name="transmission_weight" type="float" value="1" />
    <input name="specular_roughness" type="float" value="0" />
  </open_pbr_surface>
)");
    for (const char* name : {"Opaque", "Glass"}) {
        auto node = doc->getNode(name);
        require(node->getCategory().rfind("prism_open_pbr_surface_", 0) == 0,
                std::string("open_pbr: ") + name + " uses a private graph");
        require(GraphNodes(node, kHelper).size() == 1, std::string("open_pbr: ") + name + "'s base uses the helper");
        // The thin-walled subsurface reflection is not compensated.
        require(GraphNodes(node, "oren_nayar_diffuse_bsdf").size() == 1,
                std::string("open_pbr: ") + name + " keeps its uncompensated lobe");
    }
    // The glass keeps its coupled transmission (the repair before this one).
    require(doc->getNode("Opaque")->getCategory() != doc->getNode("Glass")->getCategory(),
            "open_pbr: the glass keeps its own (coupled) graph");
}

void Lama()
{
    auto doc = Load("lama", R"(
  <LamaDiffuse name="Diffuse" type="BSDF">
    <input name="roughness" type="float" value="0.7" />
  </LamaDiffuse>
)");
    auto node = doc->getNode("Diffuse");
    require(GraphNodes(node, kHelper).size() == 1, "lama: the compensated lobe uses the helper");
    require(GraphNodes(node, "oren_nayar_diffuse_bsdf").size() == 1, "lama: the uncompensated lobe stays");
}

void SharedGraph()
{
    // Materials with the same repair share one graph.
    auto doc = Load("shared", R"(
  <open_pbr_surface name="A" type="surfaceshader">
    <input name="base_diffuse_roughness" type="float" value="0.5" />
  </open_pbr_surface>
  <open_pbr_surface name="B" type="surfaceshader">
    <input name="base_color" type="color3" value="0.2, 0.3, 0.4" />
    <input name="base_diffuse_roughness" type="float" value="1" />
  </open_pbr_surface>
  <open_pbr_surface name="Smooth" type="surfaceshader">
    <input name="base_diffuse_roughness" type="float" value="0" />
  </open_pbr_surface>
  <open_pbr_surface name="Default" type="surfaceshader" />
)");
    require(doc->getNode("A")->getCategory() == doc->getNode("B")->getCategory() &&
                doc->getNode("A")->getCategory().rfind("prism_open_pbr_surface_", 0) == 0,
            "shared: one graph");
    // libbsdf compensates rough lobes only: smooth surfaces keep the library's graph.
    require(doc->getNode("Smooth")->getCategory() == "open_pbr_surface" &&
                doc->getNode("Default")->getCategory() == "open_pbr_surface",
            "smooth: the library's graph");
}
}

int main()
{
#ifndef PRISM_TEST_MATERIALX_ROOT
    std::cout << "no MaterialX libraries\n";
    return 77;
#else
    try {
        const std::filesystem::path root = PRISM_TEST_MATERIALX_ROOT;
        if (!std::filesystem::exists(root / "libraries" / "bxdf" / "open_pbr_surface.mtlx")) {
            std::cout << "no MaterialX libraries at " << root.string() << "\n";
            return 77;
        }
        gLibrary = MaterialX::createDocument();
        MaterialX::loadLibraries({"libraries"}, MaterialX::FileSearchPath(root.string()), gLibrary);
        Direct();
        OpenPbr();
        Lama();
        SharedGraph();
        std::cout << "materialx_loader_eon passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
#endif
}
