#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <iostream>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int run() {
    const auto path = std::filesystem::current_path() / "clamp_fixture.mtlx";
    const std::string xml = R"(<materialx version="1.38" colorspace="lin_rec709">
      <image name="tex" type="color3" />
      <standard_surface name="gold" type="surfaceshader">
        <input name="base_color" type="color3" value="1.059, 0.773, 0.307" />
        <input name="specular_color" type="color3" value="1.001, 0.985, 0.523" />
        <input name="emission_color" type="color3" value="5, 5, 5" />
      </standard_surface>
      <standard_surface name="in_range" type="surfaceshader">
        <input name="base_color" type="color3" value="0.2, 0.3, 0.4" />
      </standard_surface>
      <standard_surface name="textured" type="surfaceshader">
        <input name="base_color" type="color3" nodename="tex" />
      </standard_surface>
      <standard_surface name="aces" type="surfaceshader">
        <input name="base_color" type="color3" value="0.9, 0.7, 0.3" colorspace="acescg" />
      </standard_surface>
      <open_pbr_surface name="openpbr" type="surfaceshader">
        <input name="base_color" type="color3" value="1.059, 0.773, 0.307" />
      </open_pbr_surface>
      <nodegraph name="nested">
        <input name="tint" type="color3" value="2, 2, 2" />
        <standard_surface name="inner" type="surfaceshader">
          <input name="base_color" type="color3" interfacename="tint" />
        </standard_surface>
      </nodegraph>
    </materialx>)";
    { std::ofstream out(path); out << xml; }
    auto library = MaterialX::createDocument();

    DocumentLoader loader(library, /*clamp=*/true);
    auto doc = loader.load(path.string());
    require(bool(doc), "load failed");
    // Each in-place clamp and clamp node is counted for the summary line.
    require(loader.repairs().clamps > 0, "clamps not counted");

    // Working-space constants are clamped in place.
    auto gold = doc->getNode("gold");
    require(gold->getInput("base_color")->getValueString() == "0.9999, 0.773, 0.307", "base_color not clamped");
    require(gold->getInput("specular_color")->getValueString() == "0.9999, 0.985, 0.523", "specular_color not clamped");
    require(gold->getInput("emission_color")->getValueString() == "5, 5, 5", "emission clamped");
    require(doc->getNode("in_range")->getInput("base_color")->getValueString() == "0.2, 0.3, 0.4", "in-range value changed");

    // Connections and constants awaiting a color transform get a clamp node
    // that takes over the original binding.
    auto check_clamp_node = [&](MaterialX::NodePtr surface, const char* attribute, const std::string& expected) {
        auto input = surface->getInput("base_color");
        // The surface input now only references the clamp node.
        require(input->getAttribute(attribute) != expected && !input->hasAttribute("value"), "binding left on surface input");
        auto clamp = input->getConnectedNode();
        require(clamp && clamp->getCategory() == "clamp" && clamp->getType() == "color3", "no clamp node");
        require(clamp->getInput("in")->getAttribute(attribute) == expected, "binding not moved");
        require(clamp->getInput("high")->getValueString() == "0.9999, 0.9999, 0.9999", "wrong limit");
    };
    check_clamp_node(doc->getNode("textured"), "nodename", "tex");
    check_clamp_node(doc->getNode("aces"), "value", "0.9, 0.7, 0.3");
    require(doc->getNode("aces")->getInput("base_color")->getConnectedNode()
                ->getInput("in")->getColorSpace() == "acescg", "colorspace not moved");
    check_clamp_node(doc->getDescendant("nested/inner")->asA<MaterialX::Node>(), "interfacename", "tint");

    // Other shading models are untouched.
    require(doc->getNode("openpbr")->getInput("base_color")->getValueString() == "1.059, 0.773, 0.307", "open_pbr clamped");

    // Disabled by default.
    DocumentLoader disabled(library);
    auto original = disabled.load(path.string());
    require(original->getNode("gold")->getInput("base_color")->getValueString() == "1.059, 0.773, 0.307", "clamp ran while disabled");
    require(!original->getNode("textured")->getInput("base_color")->getConnectedNode()
                || original->getNode("textured")->getInput("base_color")->getNodeName() == "tex", "clamp node inserted while disabled");

    std::cout << "Clamp regression checks passed\n";
    return 0;
}

int main() {
    try { return run(); }
    catch (const std::exception& e) { std::cerr << "FAILED: " << e.what() << std::endl; return 1; }
}
