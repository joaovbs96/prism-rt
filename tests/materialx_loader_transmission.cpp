// DocumentLoader expands thin-walled open_pbr_surface nodes into their genmdl
// graph and marks the surface constructor thin-walled (MaterialX's graphs drop
// geometry_thin_walled); the backface gets the same shader and the
// transmission depth is cleared so transmission_color tints the thin sheet.
// An uncoated transmissive surface gets its specular IOR as coat_ior, which
// MaterialX's MDL makes the refraction IOR.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int run() {
    const auto path = std::filesystem::current_path() / "transmission_fixture.mtlx";
    const std::string xml = R"(<materialx version="1.39">
      <constant name="depth_source" type="float"><input name="value" type="float" value="3" /></constant>
      <open_pbr_surface name="thin" type="surfaceshader">
        <input name="geometry_thin_walled" type="boolean" value="true" />
        <input name="transmission_depth" type="float" nodename="depth_source" />
        <input name="transmission_color" type="color3" value="0.5, 1, 0.05" />
      </open_pbr_surface>
      <surfacematerial name="thin_material" type="material">
        <input name="surfaceshader" type="surfaceshader" nodename="thin" />
      </surfacematerial>
      <open_pbr_surface name="solid" type="surfaceshader">
        <input name="transmission_depth" type="float" value="2" />
      </open_pbr_surface>
      <surfacematerial name="solid_material" type="material">
        <input name="surfaceshader" type="surfaceshader" nodename="solid" />
      </surfacematerial>
      <open_pbr_surface name="water" type="surfaceshader">
        <input name="transmission_weight" type="float" value="1" />
        <input name="specular_ior" type="float" value="1.33" />
      </open_pbr_surface>
      <open_pbr_surface name="default_glass" type="surfaceshader">
        <input name="transmission_weight" type="float" value="0.5" />
      </open_pbr_surface>
      <open_pbr_surface name="coated_glass" type="surfaceshader">
        <input name="transmission_weight" type="float" value="1" />
        <input name="specular_ior" type="float" value="1.33" />
        <input name="coat_weight" type="float" value="1" />
        <input name="coat_ior" type="float" value="1.7" />
      </open_pbr_surface>
      <open_pbr_surface name="opaque" type="surfaceshader">
        <input name="specular_ior" type="float" value="1.33" />
      </open_pbr_surface>
    </materialx>)";
    { std::ofstream out(path); out << xml; }

    auto library = MaterialX::createDocument();
    library->addTargetDef("genmdl");
    auto nodedef = library->addNodeDef("ND_open_pbr_surface_surfaceshader", "surfaceshader", "open_pbr_surface");
    nodedef->setInputValue("geometry_thin_walled", false);
    nodedef->setInputValue("transmission_depth", 0.0f);
    nodedef->setInputValue("transmission_color", MaterialX::Color3(1.0f));
    nodedef->setInputValue("transmission_weight", 0.0f);
    nodedef->setInputValue("specular_ior", 1.5f);
    nodedef->setInputValue("coat_weight", 0.0f);
    nodedef->setInputValue("coat_ior", 1.6f);
    auto surfaceDef = library->addNodeDef("ND_surface", "surfaceshader", "surface");
    surfaceDef->setInputValue("opacity", 1.0f);
    surfaceDef->setInputValue("thin_walled", false);
    // The generic graph comes first: the genmdl one must still be chosen.
    for (const char* target : {"", "genmdl"}) {
        auto graph = library->addNodeGraph(std::string("NG_open_pbr_") + (*target ? target : "generic"));
        graph->setNodeDefString(nodedef->getName());
        if (*target) graph->setTarget(target);
        auto use = graph->addNode("multiply", "depth_use", "float");
        use->addInput("in1", "float")->setInterfaceName("transmission_depth");
        auto tint = graph->addNode("multiply", "tint_use", "color3");
        tint->addInput("in1", "color3")->setInterfaceName("transmission_color");
        auto surface = graph->addNode("surface", *target ? "mdl_constructor" : "generic_constructor", "surfaceshader");
        surface->addInput("opacity", "float")->setNodeName("depth_use");
        graph->addOutput("out", "surfaceshader")->setNodeName(surface->getName());
    }

    DocumentLoader loader(library, false, false);
    auto doc = loader.load(path.string());
    require(bool(doc), "load failed");

    require(!doc->getNode("thin"), "thin-walled node not expanded");
    auto material = doc->getNode("thin_material");
    auto surface = material->getInput("surfaceshader")->getConnectedNode();
    require(surface && surface->getCategory() == "surface", "material not fed by the surface constructor");
    require(surface->getName().find("mdl_constructor") != std::string::npos, "genmdl graph not chosen");
    require(surface->getInput("thin_walled") && surface->getInput("thin_walled")->getValueString() == "true",
            "surface constructor not thin-walled");
    auto back = material->getInput("backsurfaceshader");
    require(back && back->getConnectedNode() == surface, "backface does not use the same shader");
    bool depthCleared = false, tintKept = false;
    for (auto const& node : doc->getNodes()) {
        if (node->getName().find("depth_use") != std::string::npos) {
            auto in = node->getInput("in1");
            depthCleared = in && !in->hasNodeName() && in->getValueString() == "0";
        }
        if (node->getName().find("tint_use") != std::string::npos)
            tintKept = node->getInput("in1")->getValueString() == "0.5, 1, 0.05";
    }
    require(depthCleared, "transmission depth not cleared on the thin wall");
    require(tintKept, "transmission color not carried into the graph");

    // Closed (not thin-walled) surfaces are left as they are.
    auto solid = doc->getNode("solid");
    require(solid && solid->getCategory() == "open_pbr_surface", "closed surface expanded");
    require(solid->getInput("transmission_depth")->getValueString() == "2", "closed surface depth changed");
    require(!doc->getNode("solid_material")->getInput("backsurfaceshader"), "closed surface got a backface");

    // Refraction IOR of uncoated transmission.
    auto coatIor = [&](const char* name) {
        auto input = doc->getNode(name)->getInput("coat_ior");
        return input ? input->getValueString() : std::string();
    };
    require(coatIor("water") == "1.33", "uncoated transmission does not refract with specular_ior");
    require(coatIor("default_glass") == "1.5", "declared specular_ior default not used");
    require(coatIor("coated_glass") == "1.7", "coated surface lost its coat IOR");
    require(coatIor("opaque").empty(), "opaque surface changed");

    std::filesystem::remove(path);
    std::cout << "materialx loader transmission ok\n";
    return 0;
}

int main() {
    try { return run(); } catch (std::exception const& e) { std::cerr << "FAILED: " << e.what() << "\n"; return 1; }
}
