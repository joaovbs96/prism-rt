// DocumentLoader's repairs of layered dielectrics beyond the plain coupling
// (materialx_loader_fresnel): tinted glass through prism_dielectric_split,
// thin films on the RT lobe, smooth coats as prism_fresnel_layer, thin-walled
// standard_surface, transparent UsdPreviewSurface, LamaDielectric and
// LamaGeneralizedSchlick. Uses MaterialX's own library graphs (the shapes the
// repairs rely on); skipped without them.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/Util.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
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
    const std::string xml = "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n" + body +
                            "  <surfacematerial name=\"M\" type=\"material\">\n"
                            "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"S\" />\n"
                            "  </surfacematerial>\n</materialx>\n";
    { std::ofstream out(path); out << xml; }
    DocumentLoader loader(gLibrary, false, false);
    auto doc = loader.load(path.string());
    require(bool(doc), name + ": load failed");
    std::string errors;
    require(doc->validate(&errors), name + ": invalid after repair: " + errors);
    std::ifstream in(path);
    std::stringstream source;
    source << in.rdbuf();
    require(source.str() == xml, name + ": source file changed");
    in.close();
    std::filesystem::remove(path);
    return doc;
}

std::string Shader(const std::string& category, const std::string& inputs)
{
    return "  <" + category + " name=\"S\" type=\"surfaceshader\">\n" + inputs + "  </" + category + ">\n";
}

std::string Input(const std::string& name, const std::string& type, const std::string& value)
{
    return "    <input name=\"" + name + "\" type=\"" + type + "\" value=\"" + value + "\" />\n";
}

std::vector<MaterialX::NodePtr> Find(const MaterialX::DocumentPtr& doc,
                                     const std::function<bool(const MaterialX::NodePtr&)>& match)
{
    std::vector<MaterialX::NodePtr> found;
    for (auto const& node : doc->getNodes())
        if (match(node)) found.push_back(node);
    return found;
}

std::vector<MaterialX::NodePtr> Category(const MaterialX::DocumentPtr& doc, const std::string& category)
{
    return Find(doc, [&](const MaterialX::NodePtr& node) { return node->getCategory() == category; });
}

std::string Mode(const MaterialX::NodePtr& node)
{
    auto input = node->getInput("scatter_mode");
    return input ? input->getValueString() : "R";
}

std::vector<MaterialX::NodePtr> Dielectrics(const MaterialX::DocumentPtr& doc, const std::string& mode)
{
    return Find(doc, [&](const MaterialX::NodePtr& node) {
        return node->getCategory() == "dielectric_bsdf" && Mode(node) == mode && !node->getDownstreamPorts().empty();
    });
}

MaterialX::NodePtr Upstream(const MaterialX::NodePtr& node, const char* input)
{
    auto port = node ? node->getInput(input) : nullptr;
    return port ? port->getConnectedNode() : nullptr;
}

const std::string kGlass = Input("base_weight", "float", "0") + Input("specular_roughness", "float", "0") +
                           Input("transmission_weight", "float", "1");

void TintedGlass()
{
    auto doc = Load("tinted", Shader("open_pbr_surface", kGlass + Input("transmission_color", "color3", "0.5, 0.8, 0.9")));
    auto splits = Category(doc, "prism_dielectric_split");
    require(splits.size() == 1, "tinted glass: no prism_dielectric_split");
    require(doc->getNodeDef("ND_prism_dielectric_split") &&
                doc->getImplementation("IM_prism_dielectric_split_genmdl") &&
                doc->getImplementation("IM_prism_dielectric_split_genmdl")->getAttribute("sourcecode").find(
                    "df::fresnel_layer") != std::string::npos,
            "tinted glass: helper definition missing");
    auto normal = doc->getNodeDef("ND_prism_dielectric_split")->getInput("normal");
    require(normal && normal->getDefaultGeomPropString() == "Nworld", "tinted glass: helper normal has no default");
    auto split = splits.front();
    auto carrier = Upstream(split, "rt");
    require(carrier && carrier->getCategory() == "layer" && Upstream(carrier, "base") &&
                Upstream(carrier, "base")->getCategory() == "anisotropic_vdf",
            "tinted glass: the split's RT lobe lost its volume");
    auto rt = Upstream(carrier, "top");
    require(rt && Mode(rt) == "RT" && rt->getInput("tint") && rt->getInput("tint")->getValueString() == "1, 1, 1",
            "tinted glass: the RT lobe is not untinted RT");
    require(split->getInput("transmission_tint") && split->getInput("transmission_tint")->hasNodeName(),
            "tinted glass: the transmission tint was not passed on");
    require(Upstream(split, "reflection") && Upstream(split, "reflection")->getCategory() == "generalized_schlick_bsdf" &&
                Upstream(split, "transmission") && Mode(Upstream(split, "transmission")) == "T",
            "tinted glass: bare lobes missing");
    auto clear = Load("clear", Shader("open_pbr_surface", kGlass));
    require(Category(clear, "prism_dielectric_split").empty() && !clear->getNodeDef("ND_prism_dielectric_split"),
            "untinted glass took the split");
}

void ThinFilm()
{
    auto doc = Load("film", Shader("open_pbr_surface", kGlass + Input("thin_film_weight", "float", "1") +
                                                         Input("thin_film_thickness", "float", "0.5")));
    auto rts = Dielectrics(doc, "RT");
    require(rts.size() == 2, "thin film: expected a filmed and a plain RT lobe");
    int filmed = 0;
    for (auto const& rt : rts) filmed += rt->getInput("thinfilm_thickness") ? 1 : 0;
    require(filmed >= 1, "thin film: no RT lobe carries the film");
    auto standard = Load("film_ss", Shader("standard_surface", Input("base", "float", "0") +
                                                               Input("transmission", "float", "1") +
                                                               Input("thin_film_thickness", "float", "500")));
    auto ssRt = Dielectrics(standard, "RT");
    require(ssRt.size() == 1 && ssRt.front()->getInput("thinfilm_thickness"),
            "standard_surface thin film: the RT lobe has no film");
    auto tinted = Load("film_tint", Shader("open_pbr_surface", kGlass + Input("thin_film_weight", "float", "1") +
                                                                 Input("transmission_color", "color3", "0.5, 0.8, 0.9")));
    require(Category(tinted, "open_pbr_surface").size() == 1, "tinted thin film was rewritten");
}

void Coats()
{
    const std::string smooth = Input("coat_weight", "float", "1") + Input("coat_roughness", "float", "0") +
                               Input("coat_ior", "float", "1.5") + Input("specular_ior", "float", "1.5");
    auto doc = Load("coat", Shader("open_pbr_surface", kGlass + smooth));
    auto layers = Category(doc, "prism_fresnel_layer");
    require(layers.size() == 1, "smooth coat: no prism_fresnel_layer");
    auto layer = layers.front();
    require(layer->getInput("ior") && layer->getInput("weight") && Upstream(layer, "coat") && Upstream(layer, "substrate"),
            "smooth coat: Fresnel layer not connected");
    require(!layer->getDownstreamPorts().empty(), "smooth coat: Fresnel layer unused");
    // The default coat_ior (1.6) differs from the glass's: not replaced.
    auto otherIor = Load("coat_ior", Shader("open_pbr_surface", kGlass + Input("coat_weight", "float", "1") +
                                                                   Input("coat_roughness", "float", "0")));
    require(Category(otherIor, "prism_fresnel_layer").empty(), "a coat of another IOR became a Fresnel layer");
    auto rough = Load("coat_rough", Shader("open_pbr_surface", kGlass + Input("coat_weight", "float", "1") +
                                                                 Input("coat_roughness", "float", "0.3")));
    require(Category(rough, "prism_fresnel_layer").empty() && Dielectrics(rough, "RT").size() == 1,
            "rough coat: expected the coupling without a Fresnel layer");
    auto clearcoat = Load("clearcoat", Shader("gltf_pbr", Input("roughness", "float", "0") +
                                                              Input("transmission", "float", "1") +
                                                              Input("clearcoat", "float", "1")));
    require(Category(clearcoat, "prism_fresnel_layer").size() == 1, "smooth glTF clearcoat: no Fresnel layer");
}

void ThinWalledStandardSurface()
{
    auto doc = Load("thin_ss", Shader("standard_surface", Input("base", "float", "0") +
                                                              Input("transmission", "float", "1") +
                                                              Input("thin_walled", "boolean", "true")));
    auto surfaces = Category(doc, "surface");
    require(surfaces.size() == 1 && surfaces.front()->getInputValue("thin_walled") &&
                surfaces.front()->getInputValue("thin_walled")->getValueString() == "true",
            "thin-walled standard_surface: constructor not thin-walled");
    auto material = doc->getNode("M");
    require(material && material->getInput("backsurfaceshader") &&
                material->getInput("backsurfaceshader")->getConnectedNode() == surfaces.front(),
            "thin-walled standard_surface: no backface shader");
    auto opaque = Load("thin_ss_opaque", Shader("standard_surface", Input("thin_walled", "boolean", "true")));
    require(Category(opaque, "standard_surface").size() == 1, "opaque thin-walled standard_surface was expanded");
}

void UsdPreviewSurface()
{
    auto doc = Load("preview", Shader("UsdPreviewSurface", Input("opacity", "float", "0.5") +
                                                               Input("roughness", "float", "0")));
    auto rts = Dielectrics(doc, "RT");
    require(rts.size() == 1, "transparent UsdPreviewSurface: no RT lobe");
    bool coupled = false;
    for (auto const& port : rts.front()->getDownstreamPorts()) {
        auto mix = port->getParent()->asA<MaterialX::Node>();
        auto opaque = Upstream(mix, "fg");
        coupled = coupled || (mix && mix->getCategory() == "mix" && port->getName() == "bg" && opaque &&
                              opaque->getCategory() == "layer" && Upstream(opaque, "base") &&
                              Upstream(opaque, "base")->getCategory() == "oren_nayar_diffuse_bsdf" &&
                              mix->getInput("mix"));
    }
    require(coupled, "transparent UsdPreviewSurface: not mix(layer(R, diffuse), RT, opacity)");
    require(Dielectrics(doc, "T").empty(), "transparent UsdPreviewSurface: the layered transmission is still used");
    for (auto const& [name, inputs] :
         std::vector<std::pair<std::string, std::string>>{
             {"preview_rough", Input("opacity", "float", "0.5")},
             {"preview_cutout", Input("opacity", "float", "0.5") + Input("roughness", "float", "0") +
                                    Input("opacityThreshold", "float", "0.5")},
             {"preview_opaque", Input("roughness", "float", "0")}}) {
        auto other = Load(name, Shader("UsdPreviewSurface", inputs));
        require(Category(other, "UsdPreviewSurface").size() == 1, name + " was rewritten");
    }
}

void Lama()
{
    const std::string bsdf = "  <surface name=\"S\" type=\"surfaceshader\">\n"
                             "    <input name=\"bsdf\" type=\"BSDF\" nodename=\"L\" />\n  </surface>\n";
    auto doc = Load("lama", "  <LamaDielectric name=\"L\" type=\"BSDF\">\n" + Input("roughness", "float", "0.2") +
                                "  </LamaDielectric>\n" + bsdf);
    auto rts = Dielectrics(doc, "RT");
    require(rts.size() == 1 && Dielectrics(doc, "R").empty(), "LamaDielectric: not a single RT lobe");
    const auto ports = rts.front()->getDownstreamPorts();
    auto layer = ports.size() == 1 ? ports[0]->getParent()->asA<MaterialX::Node>() : nullptr;
    require(layer && layer->getCategory() == "layer" && Upstream(layer, "base") &&
                Upstream(layer, "base")->getCategory() == "anisotropic_vdf",
            "LamaDielectric: the RT lobe lost its interior");
    auto tinted = Load("lama_tinted", "  <LamaDielectric name=\"L\" type=\"BSDF\">\n" +
                                          Input("reflectionTint", "color3", "1, 0.5, 0.5") + "  </LamaDielectric>\n" + bsdf);
    require(Category(tinted, "LamaDielectric").size() == 1, "LamaDielectric with differing tints was rewritten");

    auto schlick = Load("lama_schlick", "  <LamaGeneralizedSchlick name=\"L\" type=\"BSDF\">\n" +
                                            Input("reflectionTint", "color3", "1, 0.5, 0.5") +
                                            "  </LamaGeneralizedSchlick>\n" + bsdf);
    bool layered = false;
    for (auto const& node : Category(schlick, "layer")) {
        auto top = Upstream(node, "top");
        if (top && top->getCategory() == "generalized_schlick_bsdf" && Mode(top) == "R") {
            auto color0 = Upstream(top, "color0");
            layered = color0 && color0->getCategory() == "multiply";
        }
    }
    require(layered, "LamaGeneralizedSchlick: the tinted reflection is not the layer's top");

    // Fresnel's Schlick curve with equal tints: the RT dielectric of its IOR.
    auto fresnel = Load("lama_schlick_rt", "  <LamaGeneralizedSchlick name=\"L\" type=\"BSDF\">\n" +
                                               Input("reflectivity", "color3", "0.04, 0.04, 0.04") +
                                               "  </LamaGeneralizedSchlick>\n" + bsdf);
    auto schlickRt = Dielectrics(fresnel, "RT");
    require(schlickRt.size() == 1 && schlickRt.front()->getInputValue("ior") &&
                std::abs(schlickRt.front()->getInputValue("ior")->asA<float>() - 1.5f) < 1e-4f,
            "LamaGeneralizedSchlick: not the RT dielectric of IOR 1.5");
}
} // namespace

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
        TintedGlass();
        ThinFilm();
        Coats();
        ThinWalledStandardSurface();
        UsdPreviewSurface();
        Lama();
    } catch (std::exception const& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "materialx loader dielectric ok\n";
    return 0;
#endif
}
