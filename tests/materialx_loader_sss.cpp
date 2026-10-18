#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const auto path = std::filesystem::current_path() / "sss_fixture.mtlx";
    const std::string xml = R"(<materialx version="1.38">
      <constant name="weight" type="float"><input name="value" type="float" value="0.8" /></constant>
      <standard_surface name="constant" type="surfaceshader">
        <input name="subsurface" type="float" value="0.7" />
        <input name="transmission" type="float" value="0.6" />
        <input name="base_color" type="color3" value="0.2, 0.3, 0.4" />
      </standard_surface>
      <standard_surface name="connected" type="surfaceshader">
        <input name="subsurface" type="float" nodename="weight" />
      </standard_surface>
      <standard_surface name="graph_connected" type="surfaceshader">
        <input name="subsurface" type="float" nodegraph="weights" output="out" />
      </standard_surface>
      <standard_surface name="default_weight" type="surfaceshader" />
      <nodegraph name="weights">
        <constant name="w" type="float"><input name="value" type="float" value="0.8" /></constant>
        <output name="out" type="float" nodename="w" />
      </nodegraph>
      <nodegraph name="nested">
        <input name="weight" type="float" value="0.9" />
        <open_pbr_surface name="openpbr" type="surfaceshader">
          <input name="subsurface_weight" type="float" interfacename="weight" />
        </open_pbr_surface>
      </nodegraph>
    </materialx>)";
    { std::ofstream out(path); out << xml; }
    auto library = MaterialX::createDocument();
    auto library_node = library->addNode("standard_surface", "library_surface", "surfaceshader");
    library_node->addInput("subsurface", "float")->setValue(0.8f);
    DocumentLoader loader(library, /*clamp=*/false, /*fallback_sss_to_diffuse=*/true);
    auto doc = loader.load(path.string());
    require(bool(doc), "load failed");
    for (const char* name : {"constant", "connected", "graph_connected", "default_weight", "nested/openpbr"}) {
        auto node = doc->getDescendant(name)->asA<MaterialX::Node>();
        auto input = node->getInput(node->getCategory() == "standard_surface" ? "subsurface" : "subsurface_weight");
        require(input && input->getValue()->asA<float>() == 0.0f, "SSS weight not zero");
        for (const char* attr : {"nodename", "nodegraph", "output", "interfacename"})
            require(!input->hasAttribute(attr), "connection survived fallback");
    }
    auto material = doc->getNode("constant");
    require(material->getInput("transmission")->getValue()->asA<float>() == 0.6f, "transmission changed");
    require(material->getInput("base_color")->getValueString() == "0.2, 0.3, 0.4", "base color changed");
    require(doc->getNode("library_surface")->getInput("subsurface")->getValue()->asA<float>() == 0.8f, "imported library changed");
    require(library_node->getInput("subsurface")->getValue()->asA<float>() == 0.8f, "shared library changed");
    DocumentLoader disabled(library, false, false);
    auto original = disabled.load(path.string());
    require(original->getNode("constant")->getInput("subsurface")->getValue()->asA<float>() == 0.7f, "opt-out ignored");
    require(original->getNode("connected")->getInput("subsurface")->hasAttribute("nodename"), "opt-out removed connection");
    std::ifstream file(path);
    std::string contents((std::istreambuf_iterator<char>(file)), {});
    require(contents == xml, "source asset modified");
    std::cout << "SSS fallback regression checks passed\n";
}
