// DocumentLoader rewrites the dielectric of transmissive surfaces,
// layer(R, mix(T over its VDF, opaque, w)), as
// mix(RT over its VDF, layer(R, opaque), w): MDL's custom-curve layer loses
// the energy of light leaving the medium, the RT dielectric does not.
// Untinted lobes share the RT lobe directly; tinted ones go through
// prism_dielectric_split.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

MaterialX::NodePtr Upstream(const MaterialX::NodePtr& node, const char* input) {
    auto port = node ? node->getInput(input) : nullptr;
    return port ? port->getConnectedNode() : nullptr;
}

int run() {
    const auto path = std::filesystem::current_path() / "fresnel_fixture.mtlx";
    const std::string xml = R"(<materialx version="1.39">
      <open_pbr_surface name="glass" type="surfaceshader">
        <input name="transmission_weight" type="float" value="1" />
      </open_pbr_surface>
      <surfacematerial name="glass_material" type="material">
        <input name="surfaceshader" type="surfaceshader" nodename="glass" />
      </surfacematerial>
      <open_pbr_surface name="deep" type="surfaceshader">
        <input name="transmission_weight" type="float" value="0.5" />
        <input name="transmission_color" type="color3" value="0.5, 0.8, 0.9" />
        <input name="transmission_depth" type="float" value="1" />
      </open_pbr_surface>
      <surfacematerial name="deep_material" type="material">
        <input name="surfaceshader" type="surfaceshader" nodename="deep" />
      </surfacematerial>
      <open_pbr_surface name="tinted" type="surfaceshader">
        <input name="transmission_weight" type="float" value="1" />
        <input name="transmission_color" type="color3" value="0.5, 0.8, 0.9" />
      </open_pbr_surface>
      <surfacematerial name="tinted_material" type="material">
        <input name="surfaceshader" type="surfaceshader" nodename="tinted" />
      </surfacematerial>
      <open_pbr_surface name="reflection_tinted" type="surfaceshader">
        <input name="transmission_weight" type="float" value="1" />
        <input name="specular_color" type="color3" value="1, 0.5, 0.5" />
      </open_pbr_surface>
      <surfacematerial name="reflection_tinted_material" type="material">
        <input name="surfaceshader" type="surfaceshader" nodename="reflection_tinted" />
      </surfacematerial>
      <open_pbr_surface name="opaque" type="surfaceshader" />
    </materialx>)";
    { std::ofstream out(path); out << xml; }

    auto library = MaterialX::createDocument();
    library->addTargetDef("genmdl");
    auto nodedef = library->addNodeDef("ND_open_pbr_surface_surfaceshader", "surfaceshader", "open_pbr_surface");
    nodedef->setInputValue("transmission_weight", 0.0f);
    nodedef->setInputValue("geometry_thin_walled", false);
    nodedef->setInputValue("specular_color", MaterialX::Color3(1.0f));
    nodedef->setInputValue("thin_film_weight", 0.0f);
    nodedef->setInputValue("transmission_color", MaterialX::Color3(1.0f));
    nodedef->setInputValue("transmission_depth", 0.0f);
    nodedef->setInputValue("specular_ior", 1.5f);
    nodedef->setInputValue("coat_weight", 0.0f);
    nodedef->setInputValue("coat_ior", 1.6f);
    // The shape of MaterialX 1.39.5's open_pbr_surface dielectric.
    auto graph = library->addNodeGraph("NG_open_pbr_genmdl");
    graph->setNodeDefString(nodedef->getName());
    graph->setTarget("genmdl");
    auto transmission = graph->addNode("dielectric_bsdf", "dielectric_transmission", "BSDF");
    transmission->addInput("tint", "color3")->setInterfaceName("transmission_color");
    transmission->setInputValue("scatter_mode", std::string("T"));
    auto volume = graph->addNode("anisotropic_vdf", "dielectric_volume", "VDF");
    volume->addInput("absorption", "vector3")->setValueString("0, 0, 0");
    auto volumeLayer = graph->addNode("layer", "dielectric_volume_transmission", "BSDF");
    volumeLayer->addInput("top", "BSDF")->setNodeName(transmission->getName());
    volumeLayer->addInput("base", "VDF")->setNodeName(volume->getName());
    auto opaque = graph->addNode("oren_nayar_diffuse_bsdf", "opaque_base", "BSDF");
    opaque->setInputValue("weight", 1.0f);
    auto substrate = graph->addNode("mix", "dielectric_substrate", "BSDF");
    substrate->addInput("fg", "BSDF")->setNodeName(volumeLayer->getName());
    substrate->addInput("bg", "BSDF")->setNodeName(opaque->getName());
    substrate->addInput("mix", "float")->setInterfaceName("transmission_weight");
    auto reflection = graph->addNode("dielectric_bsdf", "dielectric_reflection", "BSDF");
    reflection->addInput("tint", "color3")->setInterfaceName("specular_color");
    reflection->setInputValue("scatter_mode", std::string("R"));
    auto base = graph->addNode("layer", "dielectric_base", "BSDF");
    base->addInput("top", "BSDF")->setNodeName(reflection->getName());
    base->addInput("base", "BSDF")->setNodeName(substrate->getName());
    auto surface = graph->addNode("surface", "mdl_constructor", "surfaceshader");
    surface->addInput("bsdf", "BSDF")->setNodeName(base->getName());
    graph->addOutput("out", "surfaceshader")->setNodeName(surface->getName());

    DocumentLoader loader(library, false, false);
    auto doc = loader.load(path.string());
    require(bool(doc), "load failed");

    for (const char* name : {"glass_material", "deep_material"}) {
        auto constructor = Upstream(doc->getNode(name), "surfaceshader");
        require(constructor && constructor->getCategory() == "surface", "transmissive glass not expanded");
        auto coupled = Upstream(constructor, "bsdf");
        require(coupled && coupled->getCategory() == "mix", "dielectric not a mix of coupled and opaque lobes");
        auto rtLayer = Upstream(coupled, "fg");
        auto rt = Upstream(rtLayer, "top");
        require(rtLayer && rtLayer->getCategory() == "layer" && Upstream(rtLayer, "base") &&
                    Upstream(rtLayer, "base")->getCategory() == "anisotropic_vdf",
                "the coupled lobe lost its volume");
        require(rt && rt->getInputValue("scatter_mode")->getValueString() == "RT", "transmission not made RT");
        auto opaqueLayer = Upstream(coupled, "bg");
        require(opaqueLayer && opaqueLayer->getCategory() == "layer" &&
                    Upstream(opaqueLayer, "top")->getInputValue("scatter_mode")->getValueString() == "R" &&
                    Upstream(opaqueLayer, "base")->getCategory() == "oren_nayar_diffuse_bsdf",
                "the opaque base lost its reflection layer");
        const std::string weight = coupled->getInput("mix")->getValueString();
        require(weight == (std::string(name) == "glass_material" ? "1" : "0.5"), "transmission weight not kept");
        for (auto const& node : doc->getNodes())
            require(node->getName().find("dielectric_substrate") == std::string::npos ||
                        !node->getDownstreamPorts().empty(),
                    "the replaced substrate mix was left behind");
    }
    // Differently tinted lobes are coupled through prism_dielectric_split
    // (materialx_loader_dielectric checks its shape).
    for (const char* name : {"tinted", "reflection_tinted"})
        require(!doc->getNode(name), "a tinted transmissive surface was not expanded");
    size_t splits = 0;
    for (auto const& node : doc->getNodes()) splits += node->getCategory() == "prism_dielectric_split";
    require(splits == 2, "tinted surfaces were not split by their tints");
    auto opaqueNode = doc->getNode("opaque");
    require(opaqueNode && opaqueNode->getCategory() == "open_pbr_surface", "an opaque surface was rewritten");

    std::filesystem::remove(path);
    std::cout << "materialx loader fresnel ok\n";
    return 0;
}

int main() {
    try { return run(); } catch (std::exception const& e) { std::cerr << "FAILED: " << e.what() << "\n"; return 1; }
}
