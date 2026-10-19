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
      <constant name="rgb" type="color3"><input name="value" type="color3" value="0.5, 0.5, 1" /></constant>
      <convert name="to_vector" type="vector3">
        <input name="in" type="color3" nodename="rgb" />
      </convert>
      <sheen_bsdf name="sheen" type="BSDF" />
      <oren_nayar_diffuse_bsdf name="dust" type="BSDF" />
      <dielectric_bsdf name="clear" type="BSDF"><input name="weight" type="float" value="0" /></dielectric_bsdf>
      <mix name="dust_or_clear" type="BSDF">
        <input name="fg" type="BSDF" nodename="dust" />
        <input name="bg" type="BSDF" nodename="clear" />
        <input name="mix" type="float" value="0.5" />
      </mix>
      <layer name="sheen_dust" type="BSDF">
        <input name="top" type="BSDF" nodename="sheen" />
        <input name="base" type="BSDF" nodename="dust_or_clear" />
      </layer>
      <layer name="dusty" type="BSDF">
        <input name="top" type="BSDF" nodename="sheen_dust" />
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
    library->addNodeDef("ND_convert_float_vector3", "vector3", "convert")->addInput("in", "float");
    library->addNodeDef("ND_convert_color3_vector3", "vector3", "convert")->addInput("in", "color3");
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

    // Ambiguous definitions (convert per input type) are left alone.
    require(doc->getNode("to_vector")->getInput("in")->getType() == "color3", "convert input retyped");

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

    // A layer over a layer is reassociated, and the mix it exposes distributed:
    // dusty = layer(sheen, mix(layer(dust, base), layer(clear, base))).
    auto dusty = doc->getNode("dusty");
    require(dusty->getCategory() == "layer" && dusty->getInput("top")->getNodeName() == "sheen",
            "layer over layer not reassociated");
    auto lower = doc->getNode(dusty->getInput("base")->getNodeName());
    require(lower && lower->getCategory() == "mix", "exposed mix not distributed");
    for (const char* side : {"fg", "bg"}) {
        auto sideLayer = doc->getNode(lower->getInput(side)->getNodeName());
        require(sideLayer && sideLayer->getCategory() == "layer" &&
                sideLayer->getInput("base")->getNodeName() == "base", "base lost under the dust layer");
    }

    std::filesystem::remove(path);
    std::cout << "materialx loader types ok\n";
    return 0;
}
