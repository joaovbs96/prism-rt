// DocumentLoader compatibility preprocessing of types and color spaces:
// connections between different types get an extract/convert node, inputs
// retyped by hdMtlx get their declared type back, and OCIO color space names
// become MaterialX ones.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    const auto path = std::filesystem::current_path() / "types_fixture.mtlx";
    const std::string xml = R"(<materialx version="1.39" colorspace="ACEScg">
      <image name="color_image" type="color3">
        <input name="file" type="filename" value="a.png" colorspace="srgb_tx" />
      </image>
      <surface_like name="direct" type="surfaceshader">
        <input name="opacity" type="float" nodename="color_image" />
      </surface_like>
      <surface_like name="retyped" type="surfaceshader">
        <input name="opacity" type="color3" nodegraph="graph" output="out" />
        <input name="tint" type="color3" value="0.5, 0.25, 1" />
      </surface_like>
      <oren_nayar_diffuse_bsdf name="base" type="BSDF" />
      <dielectric_bsdf name="coat_a" type="BSDF" />
      <dielectric_bsdf name="coat_b" type="BSDF" />
      <mix name="coats" type="BSDF">
        <input name="fg" type="BSDF" nodename="coat_a" />
        <input name="bg" type="BSDF" nodename="coat_b" />
        <input name="mix" type="float" value="0.25" />
      </mix>
      <layer name="glaze" type="BSDF">
        <input name="top" type="BSDF" nodename="coats" />
        <input name="base" type="BSDF" nodename="base" />
      </layer>
      <nodegraph name="graph">
        <constant name="c" type="color3"><input name="value" type="color3" value="0.5, 0.5, 0.5" /></constant>
        <output name="out" type="color3" nodename="c" />
      </nodegraph>
    </materialx>)";
    { std::ofstream out(path); out << xml; }
    auto library = MaterialX::createDocument();
    auto nodedef = library->addNodeDef("ND_surface_like", "surfaceshader", "surface_like");
    nodedef->addInput("opacity", "float");
    nodedef->addInput("tint", "color3");
    DocumentLoader loader(library, false, false);
    auto doc = loader.load(path.string());
    require(bool(doc), "load failed");

    require(doc->getColorSpace() == "acescg", "document color space not canonical");
    require(doc->getNode("color_image")->getInput("file")->getColorSpace() == "srgb_texture",
            "srgb_tx not mapped");

    auto direct = doc->getNode("direct")->getInput("opacity");
    auto adapter = doc->getNode(direct->getNodeName());
    require(adapter && adapter->getCategory() == "extract" && adapter->getType() == "float",
            "no extract between color3 and float");
    require(adapter->getInput("in")->getNodeName() == "color_image", "extract not fed by the image");

    auto retyped = doc->getNode("retyped")->getInput("opacity");
    require(retyped->getType() == "float", "input not retyped to its declaration");
    auto output = doc->getNodeGraph("graph")->getOutput("out");
    require(output->getType() == "float", "graph output not retyped");
    auto graphAdapter = doc->getNodeGraph("graph")->getNode(output->getNodeName());
    require(graphAdapter && graphAdapter->getCategory() == "extract", "no extract in the graph");
    require(doc->getNode("retyped")->getInput("tint")->getType() == "color3", "matching input changed");

    // A layer over a mix becomes a mix of layers.
    auto glaze = doc->getNode("glaze");
    require(glaze->getCategory() == "mix", "layer over mix not distributed");
    require(glaze->getInput("mix")->getValueString() == "0.25", "mix weight not kept");
    for (const char* side : {"fg", "bg"}) {
        auto layer = doc->getNode(glaze->getInput(side)->getNodeName());
        require(layer && layer->getCategory() == "layer", "mix side is not a layer");
        require(layer->getInput("base")->getNodeName() == "base", "layer lost its base");
        require(layer->getInput("top")->getNodeName() == (std::string(side) == "fg" ? "coat_a" : "coat_b"),
                "layer top is not the mix side");
    }

    std::filesystem::remove(path);
    std::cout << "materialx loader types ok\n";
    return 0;
}
