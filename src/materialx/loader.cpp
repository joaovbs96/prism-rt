#include "loader.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXCore/Traversal.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <iostream>
#include <set>
#include <tuple>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace {
bool VerboseMdl() {
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string_view(value) != "0";
}

void AttachLibrary(const MaterialX::DocumentPtr& doc,
                   const MaterialX::DocumentPtr& library)
{
    if (!doc || !library) return;

    // A data-library reference avoids deep-copying the complete standard
    // library into every material.  importLibrary gives authored top-level
    // elements precedence, whereas data-library lookup checks the library
    // first, so retain the copying path for the uncommon override case.
    for (const auto& child : library->getChildren()) {
        const std::string name = child->getQualifiedName(child->getName());
        if (doc->getChild(name)) {
            doc->importLibrary(library);
            return;
        }
    }
    doc->setDataLibrary(library);
}

// MaterialX names for the color spaces assets spell after OCIO configs
// (ACES studio config roles, Houdini, RenderMan); unknown names would leave
// their textures unconverted.
std::string CanonicalColorSpace(std::string const& name)
{
    std::string key;
    for (char c : name) key += char(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> aliases[] = {
        {"srgb_tx", "srgb_texture"}, {"srgb", "srgb_texture"}, {"srgb - texture", "srgb_texture"},
        {"utility - srgb - texture", "srgb_texture"}, {"srgb texture", "srgb_texture"},
        {"srgb_rec709_scene", "srgb_texture"}, {"srgb encoded rec.709 (srgb)", "srgb_texture"},
        // Data is never converted: MaterialX has no transform from "none" into
        // any working space (lin_rec709 would convert into ACEScg).
        {"raw", "none"}, {"utility - raw", "none"}, {"non-color", "none"},
        {"data", "none"}, {"linear", "lin_rec709"}, {"lin_srgb", "lin_rec709"},
        {"lin_rec709_srgb", "lin_rec709"},
        {"linear rec.709 (srgb)", "lin_rec709"}, {"lin_rec709_scene", "lin_rec709"},
        {"utility - linear - srgb", "lin_rec709"}, {"scene-linear rec.709-srgb", "lin_rec709"},
        {"acescg", "acescg"}, {"aces - acescg", "acescg"}, {"lin_ap1", "acescg"},
        {"lin_ap1_scene", "acescg"}, {"acescg (lin_ap1)", "acescg"},
        {"g22_rec709", "g22_rec709"}, {"g18_rec709", "g18_rec709"},
        {"lin_displayp3", "lin_displayp3"}, {"srgb_displayp3", "srgb_displayp3"},
    };
    for (auto const& [alias, canonical] : aliases)
        if (key == alias) return canonical;
    return name;
}

// Inputs connected to a node output of another type (for example a color3
// image driving open_pbr_surface.geometry_opacity) fail validation; insert
// the extract/convert node the connection implies.
MaterialX::NodePtr AddAdapter(MaterialX::GraphElementPtr const& graph, std::string const& from,
                              std::string const& to, std::string const& upstream, std::string const& baseName)
{
    static const std::vector<std::string> vectors = {"color3", "color4", "vector2", "vector3", "vector4"};
    const bool fromVector = std::find(vectors.begin(), vectors.end(), from) != vectors.end();
    const bool extract = to == "float" && fromVector;
    const bool scalar = from == "float" || from == "integer" || from == "boolean";
    const bool convert = (std::find(vectors.begin(), vectors.end(), to) != vectors.end() &&
                          (scalar || fromVector)) ||
        (to == "float" && (from == "integer" || from == "boolean"));
    if (!extract && !convert) return nullptr;
    auto adapter = graph->addNode(extract ? "extract" : "convert",
                                  graph->createValidChildName(baseName + "_as_" + to), to);
    adapter->addInput("in", from)->setNodeName(upstream);
    if (extract) adapter->setInputValue("index", 0);
    return adapter;
}

int ComponentCount(std::string const& type)
{
    if (type == "float" || type == "integer" || type == "boolean") return 1;
    if (type == "vector2") return 2;
    if (type == "color3" || type == "vector3") return 3;
    if (type == "color4" || type == "vector4") return 4;
    return 0;
}

// A value written for another type, read as `type` (empty: not convertible).
std::string ConvertValueString(std::string const& value, std::string const& type)
{
    std::vector<std::string> parts;
    size_t begin = 0;
    for (;;) {
        const size_t comma = value.find(',', begin);
        std::string part = value.substr(begin, comma == std::string::npos ? std::string::npos : comma - begin);
        part.erase(0, part.find_first_not_of(" \t"));
        part.erase(part.find_last_not_of(" \t") + 1);
        parts.push_back(part);
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    if (type == "boolean") {
        if (parts.size() != 1) return {};
        if (parts[0] == "true" || parts[0] == "false") return parts[0];
        try { return std::stod(parts[0]) != 0.0 ? "true" : "false"; } catch (...) { return {}; }
    }
    if (type == "integer") {
        if (parts.size() != 1) return {};
        try { return std::to_string(int(std::stod(parts[0]))); } catch (...) { return {}; }
    }
    const int count = ComponentCount(type);
    if (!count) return {};
    for (auto const& part : parts) {
        try { (void)std::stod(part); } catch (...) { return {}; }
    }
    if (int(parts.size()) == count) return value;
    if (parts.size() == 1) {
        std::string result = parts[0];
        for (int i = 1; i < count; ++i) result += ", " + parts[0];
        return result;
    }
    if (count == 1) return parts[0];
    return {};
}

// Inputs whose type contradicts their node definition (hdMtlx types a
// shader input after the graph output feeding it, and some exporters write
// vector parameters as strings or flags as integers) get their declared
// type; values are converted, and a graph output feeding them gets the
// extract/convert node the connection implies.
void FixDeclaredInputTypes(const MaterialX::DocumentPtr& doc, const MaterialX::DocumentPtr& library)
{
    if (!library) return;
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>()) nodes.push_back(node);
    for (auto const& node : nodes) {
        MaterialX::NodeDefPtr nodedef;
        if (node->hasNodeDefString()) {
            nodedef = library->getNodeDef(node->getNodeDefString());
            if (!nodedef) nodedef = doc->getNodeDef(node->getNodeDefString());
        }
        if (!nodedef) {
            std::vector<MaterialX::NodeDefPtr> candidates = library->getMatchingNodeDefs(node->getCategory());
            auto local = doc->getMatchingNodeDefs(node->getCategory());
            candidates.insert(candidates.end(), local.begin(), local.end());
            // Only an unambiguous definition: nodes such as convert have one
            // per input type for the same output type.
            int matches = 0;
            for (auto const& candidate : candidates)
                if (candidate->getType() == node->getType()) {
                    nodedef = candidate;
                    ++matches;
                }
            if (matches != 1) nodedef = nullptr;
        }
        if (!nodedef) continue;
        for (auto input : node->getInputs()) {
            auto declared = nodedef->getActiveInput(input->getName());
            if (!declared || declared->getType() == input->getType()) continue;
            const std::string from = input->getType(), to = declared->getType();
            auto graphElement = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr;
            auto graph = input->hasNodeGraphString() ? doc->getNodeGraph(input->getNodeGraphString()) : nullptr;
            auto output = graph && input->hasOutputString() ? graph->getOutput(input->getOutputString()) : nullptr;
            if (output && output->getType() == from && output->hasNodeName()) {
                auto adapter = AddAdapter(graph, from, to, output->getNodeName(), output->getNodeName());
                if (!adapter) continue;
                output->setNodeName(adapter->getName());
                output->setType(to);
            } else if (input->hasNodeName()) {
                // Connected within the graph: FixConnectionTypes then adapts
                // an upstream of another type.
                auto upstream = graphElement ? graphElement->getNode(input->getNodeName()) : nullptr;
                if (!upstream) continue;
            } else if (input->hasValueString()) {
                const std::string value = ConvertValueString(input->getValueString(), to);
                if (value.empty()) continue;
                input->setValueString(value);
            } else if (input->hasInterfaceName()) {
                continue;
            }
            input->setType(to);
            if (VerboseMdl())
                std::cout << "[MaterialX] " << from << " input " << input->getNamePath()
                          << " retyped to its declared " << to << "\n";
        }
    }
}

void FixConnectionTypes(const MaterialX::DocumentPtr& doc)
{
    struct Fix {
        MaterialX::InputPtr input;
        MaterialX::NodePtr upstream;
        MaterialX::ElementPtr graph;
    };
    std::vector<Fix> fixes;
    for (auto element : doc->traverseTree()) {
        auto input = element->asA<MaterialX::Input>();
        if (!input || !input->hasNodeName() || input->hasOutputString()) continue;
        auto node = input->getParent() ? input->getParent()->asA<MaterialX::Node>() : nullptr;
        if (!node) continue;
        auto graph = node->getParent();
        auto graphElement = graph ? graph->asA<MaterialX::GraphElement>() : nullptr;
        auto upstream = graphElement ? graphElement->getNode(input->getNodeName()) : nullptr;
        if (!upstream || upstream->getType() == input->getType() ||
            upstream->getType() == MaterialX::MULTI_OUTPUT_TYPE_STRING) continue;
        fixes.push_back({input, upstream, graph});
    }
    for (auto const& fix : fixes) {
        const std::string from = fix.upstream->getType(), to = fix.input->getType();
        auto adapter = AddAdapter(fix.graph->asA<MaterialX::GraphElement>(), from, to, fix.upstream->getName(),
                                  fix.upstream->getName());
        if (!adapter) continue;
        fix.input->setNodeName(adapter->getName());
        std::cerr << "[MaterialX] " << from << " output of " << fix.upstream->getNamePath() << " feeds "
                  << to << " input " << fix.input->getNamePath() << "; inserted " << adapter->getCategory()
                  << "\n";
    }
}

// MaterialX's MDL generator layers a BSDF over a base by handing the base to
// the top lobe; a mix as the top passes nothing on and the base is lost. A
// layer distributes over a mix of its top:
//   layer(mix(fg, bg, m), base) = mix(layer(fg, base), layer(bg, base), m).
void DistributeLayersOverMixes(const MaterialX::DocumentPtr& doc)
{
    std::vector<MaterialX::NodePtr> work;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>(); node && node->getCategory() == "layer") work.push_back(node);
    size_t guard = 0;
    while (!work.empty() && ++guard < 4096) {
        auto layer = work.back();
        work.pop_back();
        auto graph = layer->getParent() ? layer->getParent()->asA<MaterialX::GraphElement>() : nullptr;
        auto topInput = layer->getInput("top");
        auto baseInput = layer->getInput("base");
        if (!graph || !topInput || !topInput->hasNodeName()) continue;
        auto mix = graph->getNode(topInput->getNodeName());
        // Layering is associative: layer(layer(T, M), B) = layer(T, layer(M, B)),
        // which brings a mix that is the inner layer's base to the top of a layer.
        if (mix && mix->getCategory() == "layer" && mix->getType() == "BSDF" && baseInput) {
            auto innerTop = mix->getInput("top");
            auto innerBase = mix->getInput("base");
            if (innerTop && innerTop->hasNodeName() && innerBase && innerBase->hasNodeName()) {
                auto lower = graph->addNode("layer", graph->createValidChildName(layer->getName() + "_lower"), "BSDF");
                lower->addInput("top", "BSDF")->setNodeName(innerBase->getNodeName());
                auto lowerBase = lower->addInput("base", "BSDF");
                if (baseInput->hasNodeName()) lowerBase->setNodeName(baseInput->getNodeName());
                topInput->setNodeName(innerTop->getNodeName());
                baseInput->setNodeName(lower->getName());
                work.push_back(layer);
                work.push_back(lower);
                if (VerboseMdl())
                    std::cout << "[MaterialX] layer " << layer->getNamePath() << " reassociated over "
                              << mix->getNamePath() << "\n";
                continue;
            }
        }
        if (!mix || mix->getCategory() != "mix" || mix->getType() != "BSDF") continue;
        auto makeLayer = [&](const char* side) -> MaterialX::NodePtr {
            auto source = mix->getInput(side);
            auto result = graph->addNode("layer", graph->createValidChildName(layer->getName() + "_" + side), "BSDF");
            auto top = result->addInput("top", "BSDF");
            if (source && source->hasNodeName()) top->setNodeName(source->getNodeName());
            if (baseInput) {
                auto base = result->addInput("base", "BSDF");
                if (baseInput->hasNodeName()) base->setNodeName(baseInput->getNodeName());
            }
            return result;
        };
        auto fg = makeLayer("fg");
        auto bg = makeLayer("bg");
        // The layer becomes the mix, so its consumers are unchanged.
        layer->removeInput("top");
        layer->removeInput("base");
        layer->setCategory("mix");
        if (layer->hasNodeDefString()) layer->setNodeDefString("ND_mix_bsdf");
        layer->addInput("fg", "BSDF")->setNodeName(fg->getName());
        layer->addInput("bg", "BSDF")->setNodeName(bg->getName());
        if (auto weight = mix->getInput("mix")) {
            auto copy = layer->addInput("mix", "float");
            copy->copyContentFrom(weight);
        }
        work.push_back(fg);
        work.push_back(bg);
        if (VerboseMdl())
            std::cout << "[MaterialX] layer " << layer->getNamePath() << " distributed over mix "
                      << mix->getNamePath() << "\n";
    }
}

bool IsTrueConstant(const MaterialX::InputPtr& input)
{
    if (!input || input->hasNodeName() || input->hasNodeGraphString() || input->hasOutputString() ||
        input->hasInterfaceName())
        return false;
    return input->getValueString() == "true";
}

bool IsBound(const MaterialX::InputPtr& input)
{
    return input->hasNodeName() || input->hasNodeGraphString() || input->hasOutputString() ||
           input->hasInterfaceName();
}

// The constant value of an input, or the declared default when it is absent
// (empty: bound or unknown).
std::string ConstantOrDefault(const MaterialX::NodePtr& node, const std::string& name)
{
    if (auto input = node->getInput(name)) return IsBound(input) ? std::string() : input->getValueString();
    auto nodedef = node->getNodeDef();
    auto declared = nodedef ? nodedef->getActiveInput(name) : nullptr;
    return declared ? declared->getValueString() : std::string();
}

bool IsZero(const std::string& value)
{
    try { return !value.empty() && std::stod(value) == 0.0; } catch (...) { return false; }
}

// MaterialX's MDL dielectric_bsdf hands its own IOR to the material
// (pbrlib_1_9 mx_dielectric_bsdf: `ior: color(mxp_ior)`), and OpenPBR's
// outermost dielectric is the coat: MDL refracts transmission with coat_ior
// (1.6 by default) instead of specular_ior, coat or not. Without a coat,
// coat_ior has no other effect, so a transmissive surface gets its specular
// IOR there. (A coated one keeps the mismatch: MDL has a single IOR.)
void RouteUncoatedTransmissionIor(const MaterialX::DocumentPtr& doc)
{
    // Collect first: adding an absent coat_ior mutates the tree.
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>(); node && node->getCategory() == "open_pbr_surface")
            nodes.push_back(node);
    for (auto const& node : nodes) {
        auto transmission = node->getInput("transmission_weight");
        if (!transmission || (!IsBound(transmission) && IsZero(transmission->getValueString()))) continue;
        if (!IsZero(ConstantOrDefault(node, "coat_weight"))) continue;
        const std::string ior = ConstantOrDefault(node, "specular_ior");
        if (ior.empty()) continue; // a varying IOR cannot become MDL's uniform one
        auto coatIor = node->getInput("coat_ior");
        if (!coatIor) coatIor = node->addInput("coat_ior", "float");
        if (!IsBound(coatIor) && coatIor->getValueString() == ior) continue;
        for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"})
            coatIor->removeAttribute(attribute);
        coatIor->setValueString(ior);
        if (VerboseMdl())
            std::cout << "[MaterialX] uncoated " << node->getNamePath() << ": coat_ior = specular_ior " << ior << "\n";
    }
}

// MaterialX 1.39.4's open_pbr_surface graphs (genmdl variant included) never
// connect geometry_thin_walled to their <surface> constructor, so the MDL
// material is always closed: thin-walled glass refracts like a solid and
// fills with the transmission_depth medium. 1.39.5 (OpenPBR 1.1.1, a single
// graph) connects it, but the material still has no backface shader and keeps
// the depth. Expand such nodes into their genmdl graph and set the
// constructor's thin_walled flag; a thin-walled MDL material scatters with
// its backface BSDF from the back, so the material gets the same shader there.
//
// OpenPBR's thin wall is an infinitesimal sheet whose transmittance at normal
// incidence is transmission_color, whatever transmission_depth says. The
// graph uses transmission_color as the tint only when the depth is zero
// (otherwise it becomes the volume's absorption, which a thin-walled MDL
// material ignores), so the depth is cleared as well.
//
// standard_surface's graph only uses thin_walled to pick translucency over
// subsurface scattering and has no thin_walled on its constructor, so its
// thin-walled glass refracts like a solid (and loses energy leaving it, R + T
// 0.64 at 41 degrees inside). Autodesk Standard Surface defines a thin wall
// as an infinitely thin shell that transmits without refraction, so such
// nodes get the same expansion when they transmit or scatter (an opaque one
// renders the same either way); standard_surface has no transmission medium.
void ExpandThinWalledOpenPbr(const MaterialX::DocumentPtr& doc)
{
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree()) {
        auto node = element->asA<MaterialX::Node>();
        if (!node) continue;
        if (node->getCategory() == "open_pbr_surface" && IsTrueConstant(node->getInput("geometry_thin_walled")))
            nodes.push_back(node);
        else if (node->getCategory() == "standard_surface" && IsTrueConstant(node->getInput("thin_walled")) &&
                 (!IsZero(ConstantOrDefault(node, "transmission")) || !IsZero(ConstantOrDefault(node, "subsurface"))))
            nodes.push_back(node);
    }
    if (nodes.empty()) return;

    // The MDL generator's own choice of implementation (the library's genmdl
    // target definition selects the MDL variant of the graph).
    const std::string target = "genmdl";

    for (auto const& node : nodes) {
        auto graph = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr;
        auto implementation = node->getImplementation(target);
        if (!graph || !implementation || !implementation->isA<MaterialX::NodeGraph>()) {
            std::cerr << "[MaterialX] WARNING: no graph for thin-walled " << node->getNamePath()
                      << "; it renders as a closed surface\n";
            continue;
        }
        if (node->getCategory() == "open_pbr_surface") {
            auto depth = node->getInput("transmission_depth");
            if (!depth) depth = node->addInput("transmission_depth", "float");
            for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"})
                depth->removeAttribute(attribute);
            depth->setValue(0.0f);
        }

        const std::string name = node->getName();
        std::vector<MaterialX::PortElementPtr> downstream = node->getDownstreamPorts();
        graph->flattenSubgraphs(target, [&](MaterialX::NodePtr candidate) { return candidate == node; });
        if (graph->getNode(name) == node) continue; // not expanded

        std::vector<MaterialX::NodePtr> surfaces;
        for (auto const& port : downstream) {
            auto surface = graph->getNode(port->getNodeName());
            if (surface && surface->getCategory() == "surface" &&
                std::find(surfaces.begin(), surfaces.end(), surface) == surfaces.end())
                surfaces.push_back(surface);
        }
        for (auto const& surface : surfaces) {
            surface->setInputValue("thin_walled", true);
            for (auto const& material : doc->getMaterialNodes()) {
                auto front = material->getInput("surfaceshader");
                if (!front || front->getConnectedNode() != surface) continue;
                auto back = material->getInput("backsurfaceshader");
                if (back && (back->hasNodeName() || back->hasNodeGraphString())) continue;
                if (!back) back = material->addInput("backsurfaceshader", "surfaceshader");
                back->copyContentFrom(front);
            }
        }
        if (VerboseMdl())
            std::cout << "[MaterialX] thin-walled " << name << " expanded into its " << target
                      << " graph (" << surfaces.size() << " surface constructor)\n";
    }
}

bool IsWhite(const std::string& value)
{
    if (value.empty()) return false;
    size_t start = 0, count = 0;
    try {
        while (start <= value.size()) {
            const size_t end = std::min(value.find(',', start), value.size());
            if (std::stod(value.substr(start, end - start)) != 1.0) return false;
            ++count;
            start = end + 1;
        }
    } catch (...) { return false; }
    return count == 3;
}

bool IsOne(const std::string& value)
{
    try { return !value.empty() && std::stod(value) == 1.0; } catch (...) { return false; }
}

bool IsPositive(const std::string& value)
{
    try { return !value.empty() && std::stod(value) > 0.0; } catch (...) { return false; }
}

bool IsBlack(const std::string& value)
{
    if (value.empty()) return false;
    size_t start = 0, count = 0;
    try {
        while (start <= value.size()) {
            const size_t end = std::min(value.find(',', start), value.size());
            if (std::stod(value.substr(start, end - start)) != 0.0) return false;
            ++count;
            start = end + 1;
        }
    } catch (...) { return false; }
    return count == 3;
}

// Two helper BSDFs with inline genmdl implementations, added to a document
// that uses them (MaterialX has no node for an MDL Fresnel layer):
//
// prism_fresnel_layer: df::fresnel_layer(ior, weight, layer: coat, base:
//   substrate). libbsdf weights its base by 1 - weight * max(F(k1), F(k2'))
//   where k2' is k2 reflected about the half vector of the refraction, so a
//   base refracting with the same IOR keeps exactly 1 - F (no exit loss).
//   The coat is a bare reflection lobe (generalized_schlick with color0 =
//   color90 = 1). The material IOR follows the reflection-only
//   dielectric_bsdf it replaces (pbrlib's preserve_volume_ior: the base's when
//   above one, else the layer's): OpenPBR's coated glass refracts with
//   coat_ior, its base carrying specular_ior relative to the coat.
//
// prism_dielectric_split: a dielectric whose reflection (tint s) and
//   transmission (tint t) are tinted differently, as
//     min(s, t) * RT + (s - min) * F R + (t - min) * (1 - F) T
//   with RT the untinted reflect-and-transmit lobe, F R the reflection lobe
//   weighted by its Fresnel term, (1 - F) T the transmission lobe under an
//   empty Fresnel layer. Exact where s >= t (the last term vanishes); the
//   last term is exact for smooth lobes.
const std::string kFresnelLayer = "prism_fresnel_layer";
const std::string kDielectricSplit = "prism_dielectric_split";

void EnsureHelperBsdf(const MaterialX::DocumentPtr& doc, const std::string& category)
{
    const std::string name = "ND_" + category;
    if (doc->getNodeDef(name)) return;
    auto nodedef = doc->addNodeDef(name, "BSDF", category);
    nodedef->setNodeGroup("pbr");
    std::string code;
    if (category == kFresnelLayer) {
        nodedef->setInputValue("ior", 1.5f);
        nodedef->setInputValue("weight", 1.0f);
        nodedef->addInput("coat", "BSDF");
        nodedef->addInput("substrate", "BSDF");
        code = "material(surface: material_surface(scattering: df::fresnel_layer(ior: {{ior}}, weight: {{weight}}, "
               "layer: {{coat}}.surface.scattering, base: {{substrate}}.surface.scattering, normal: {{normal}})), "
               "ior: ({{weight}} <= 0.0 || math::max_value({{substrate}}.ior) > 1.0) ? {{substrate}}.ior : "
               "color({{ior}}), volume: {{substrate}}.volume)";
    } else {
        nodedef->setInputValue("ior", 1.5f);
        nodedef->setInputValue("reflection_tint", MaterialX::Color3(1.0f));
        nodedef->setInputValue("transmission_tint", MaterialX::Color3(1.0f));
        nodedef->addInput("rt", "BSDF");
        nodedef->addInput("reflection", "BSDF");
        nodedef->addInput("transmission", "BSDF");
        const std::string common = "math::min({{reflection_tint}}, {{transmission_tint}})";
        code = "material(surface: material_surface(scattering: df::color_unbounded_mix(components: "
               "df::color_bsdf_component[]("
               "df::color_bsdf_component(weight: " + common + ", component: {{rt}}.surface.scattering), "
               "df::color_bsdf_component(weight: {{reflection_tint}} - " + common +
               ", component: df::fresnel_layer(ior: {{ior}}, layer: {{reflection}}.surface.scattering, "
               "normal: {{normal}})), "
               "df::color_bsdf_component(weight: {{transmission_tint}} - " + common +
               ", component: df::fresnel_layer(ior: {{ior}}, base: {{transmission}}.surface.scattering, "
               "normal: {{normal}}))))), "
               "ior: {{rt}}.ior, volume: {{rt}}.volume)";
    }
    auto normal = nodedef->addInput("normal", "vector3");
    if (doc->getGeomPropDef("Nworld")) normal->setDefaultGeomPropString("Nworld");
    auto implementation = doc->addImplementation("IM_" + category + "_genmdl");
    implementation->setNodeDefString(name);
    implementation->setTarget("genmdl");
    implementation->setAttribute("sourcecode", code);
}

// Copy an input (value or connection) from one node to another.
void CopyInput(const MaterialX::NodePtr& to, const MaterialX::NodePtr& from, const std::string& name,
               const std::string& as = std::string())
{
    auto source = from ? from->getInput(name) : nullptr;
    if (!source) return;
    const std::string target = as.empty() ? name : as;
    auto input = to->getInput(target);
    if (!input) input = to->addInput(target, source->getType());
    input->copyContentFrom(source);
}

// A reflection lobe without Fresnel weighting (a constant Schlick curve of 1),
// with the roughness and frame of `lobe`.
MaterialX::NodePtr BareReflection(const MaterialX::GraphElementPtr& graph, const MaterialX::NodePtr& lobe,
                                  const std::string& name)
{
    auto bare = graph->addNode("generalized_schlick_bsdf", graph->createValidChildName(name), "BSDF");
    bare->setInputValue("color0", MaterialX::Color3(1.0f));
    bare->setInputValue("color90", MaterialX::Color3(1.0f));
    for (const char* input : {"roughness", "normal", "tangent", "distribution"}) CopyInput(bare, lobe, input);
    return bare;
}

// The node connected to an input.
MaterialX::NodePtr Producer(const MaterialX::NodePtr& node, const char* input)
{
    auto port = node ? node->getInput(input) : nullptr;
    return port ? port->getConnectedNode() : nullptr;
}

void RedirectConsumers(const std::vector<MaterialX::PortElementPtr>& consumers, const MaterialX::NodePtr& node)
{
    for (auto const& port : consumers) {
        port->setNodeName(node->getName());
        port->removeAttribute("output");
    }
}

// The single node connected to a node's output: (consumer, input name).
std::pair<MaterialX::NodePtr, std::string> SoleConsumer(const MaterialX::NodePtr& node)
{
    const std::vector<MaterialX::PortElementPtr> ports = node->getDownstreamPorts();
    if (ports.size() != 1) return {};
    auto consumer = ports[0]->getParent() ? ports[0]->getParent()->asA<MaterialX::Node>() : nullptr;
    return {consumer, consumer ? ports[0]->getName() : std::string()};
}

// MaterialX's surface graphs (open_pbr_surface, standard_surface, gltf_pbr)
// build a dielectric as a reflection-only lobe layered over a mix of a
// transmission-only lobe and the opaque base:
//
//     layer(R, mix(T [over its VDF], opaque, transmission))
//
// MaterialX's MDL dielectric_bsdf turns the layer into df::custom_curve_layer
// (a Schlick curve). libbsdf weights a custom-curve base by
// 1 - max(curve(cos k1), curve(cos k2)) and, as the curve has no IOR, takes
// k2's cosine as is: for light leaving the medium k2 is the refracted
// direction, more grazing than k1, so the base loses curve(k2) - curve(k1).
// Smooth glass at 40 degrees inside keeps only R + T = 0.795, and a closed
// glass sphere in a white furnace renders 3% dark. The reflection-and-
// transmission dielectric (scatter_mode RT) splits by its exact Fresnel term
// and conserves energy; layering distributes over the mix, so the graph is
// rewritten as
//
//     mix(RT [over its VDF], layer(R, opaque), transmission)
//
// which is the same where R and T agree (callers check that the lobes have
// the same IOR and roughness). `transmission` is the expanded graph's
// scatter_mode "T" dielectric_bsdf (`mix` its substrate's port: "fg" in
// open_pbr_surface, standard_surface and gltf_pbr, "bg" in UsdPreviewSurface).
//
// Thin films: the reflection lobe (or, in open_pbr_surface and gltf_pbr, the
// thin-film side of a mix of reflection lobes) carries the film, and libbsdf
// applies a film around an RT lobe to its Fresnel term with R + T = 1, so the
// RT lobe gets the same film (a mix of filmed and plain RT lobes by the
// film's weight).
//
// Tints: with `split`, reflection and transmission are tinted differently
// (OpenPBR's transmission_color at depth 0, glTF's base_color, a tinted
// specular_color): RT stays untinted and prism_dielectric_split weights it by
// both tints. Callers don't combine a split with a thin film.
bool CoupleTransmissionFresnel(const MaterialX::GraphElementPtr& graph, const MaterialX::NodePtr& transmission,
                               bool split = false)
{
    auto mode = transmission->getInput("scatter_mode");
    if (!mode || IsBound(mode) || mode->getValueString() != "T") return false;
    MaterialX::NodePtr carrier = transmission; // T, or the layer of T over its VDF
    auto [consumer, port] = SoleConsumer(transmission);
    if (consumer && consumer->getCategory() == "layer" && port == "top") {
        carrier = consumer;
        std::tie(consumer, port) = SoleConsumer(carrier);
    }
    if (!consumer || consumer->getCategory() != "mix" || (port != "fg" && port != "bg")) return false;
    const MaterialX::NodePtr substrate = consumer;
    const std::string carrierSide = port, opaqueSide = port == "fg" ? "bg" : "fg";
    auto [layer, layerPort] = SoleConsumer(substrate);
    auto opaque = substrate->getInput(opaqueSide);
    if (!layer || layer->getCategory() != "layer" || layerPort != "base" || !opaque || !opaque->hasNodeName())
        return false;
    auto top = Producer(layer, "top");
    if (!top) return false;
    // The plain reflection lobe, and the thin-film one with its weight.
    MaterialX::NodePtr plain = top, filmed;
    MaterialX::InputPtr filmWeight;
    if (top->getCategory() == "mix") {
        filmed = Producer(top, "fg");
        plain = Producer(top, "bg");
        filmWeight = top->getInput("mix");
        if (!plain || !filmed) return false;
        if (!filmWeight || (!IsBound(filmWeight) && IsZero(filmWeight->getValueString()))) filmed = nullptr;
    } else if (auto film = top->getInput("thinfilm_thickness");
               film && (IsBound(film) || !IsZero(film->getValueString()))) {
        filmed = top;
    }

    const std::vector<MaterialX::PortElementPtr> consumers = layer->getDownstreamPorts();
    mode->setValueString("RT");
    if (split) {
        EnsureHelperBsdf(graph->getDocument(), kDielectricSplit);
        auto node = graph->addNode(kDielectricSplit, graph->createValidChildName(transmission->getName() + "_split"),
                                   "BSDF");
        CopyInput(node, transmission, "ior");
        CopyInput(node, transmission, "normal");
        if (plain->getCategory() == "dielectric_bsdf") CopyInput(node, plain, "tint", "reflection_tint");
        CopyInput(node, transmission, "tint", "transmission_tint");
        auto bareT = graph->addNode("dielectric_bsdf", graph->createValidChildName(transmission->getName() + "_bare"),
                                    "BSDF");
        for (const char* input : {"ior", "roughness", "normal", "tangent", "distribution"})
            CopyInput(bareT, transmission, input);
        bareT->setInputValue("scatter_mode", std::string("T"));
        auto tint = transmission->getInput("tint");
        if (!tint) tint = transmission->addInput("tint", "color3");
        for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"}) tint->removeAttribute(attribute);
        tint->setValueString("1, 1, 1");
        node->addInput("rt", "BSDF")->setNodeName(carrier->getName());
        node->addInput("reflection", "BSDF")->setNodeName(BareReflection(graph, plain, plain->getName() + "_bare")->getName());
        node->addInput("transmission", "BSDF")->setNodeName(bareT->getName());
        carrier = node;
    } else if (filmed == top) {
        for (const char* input : {"thinfilm_thickness", "thinfilm_ior"}) CopyInput(transmission, filmed, input);
    } else if (filmed) {
        auto filmedT = graph->addNode("dielectric_bsdf", graph->createValidChildName(transmission->getName() + "_tf"),
                                      "BSDF");
        filmedT->copyContentFrom(transmission);
        for (const char* input : {"thinfilm_thickness", "thinfilm_ior"}) CopyInput(filmedT, filmed, input);
        MaterialX::NodePtr filmedCarrier = filmedT;
        if (carrier != transmission) {
            filmedCarrier = graph->addNode("layer", graph->createValidChildName(carrier->getName() + "_tf"), "BSDF");
            filmedCarrier->copyContentFrom(carrier);
            filmedCarrier->getInput("top")->setNodeName(filmedT->getName());
        }
        auto films = graph->addNode("mix", graph->createValidChildName(carrier->getName() + "_films"), "BSDF");
        films->addInput("fg", "BSDF")->setNodeName(filmedCarrier->getName());
        films->addInput("bg", "BSDF")->setNodeName(carrier->getName());
        if (filmWeight) films->addInput("mix", "float")->copyContentFrom(filmWeight);
        carrier = films;
    }
    layer->getInput("base")->setNodeName(opaque->getNodeName());
    auto mix = graph->addNode("mix", graph->createValidChildName(layer->getName() + "_fresnel_coupled"), "BSDF");
    mix->addInput(carrierSide, "BSDF")->setNodeName(carrier->getName());
    mix->addInput(opaqueSide, "BSDF")->setNodeName(layer->getName());
    if (auto weight = substrate->getInput("mix")) mix->addInput("mix", "float")->copyContentFrom(weight);
    RedirectConsumers(consumers, mix);
    graph->removeNode(substrate->getName());
    return true;
}

// The coat of an expanded surface graph whose glass was coupled: the first
// layer under the surface constructor (below a sheen) with a reflection-only
// dielectric_bsdf on top. A smooth coat over smooth glass becomes a Fresnel
// layer (prism_fresnel_layer) with the coat's IOR and weight: MDL refracts
// the glass with the material IOR, and a Fresnel layer of the same IOR keeps
// exactly 1 - F for the refracted light (custom_curve_layer loses
// curve(k2) - curve(k1) of it leaving the medium, R + T 0.78 at 41 degrees
// inside). Rough coats or glass are left alone: the Fresnel layer's base
// weighting gains or loses more there than the curve layer does.
bool ReplaceSmoothCoat(const MaterialX::GraphElementPtr& graph, const MaterialX::NodePtr& surface)
{
    MaterialX::NodePtr node = Producer(surface, "bsdf");
    for (int depth = 0; node && node->getCategory() == "layer" && depth < 4; ++depth) {
        auto top = Producer(node, "top");
        if (top && top->getCategory() == "sheen_bsdf") {
            node = Producer(node, "base");
            continue;
        }
        if (!top || top->getCategory() != "dielectric_bsdf") return false;
        if (ConstantOrDefault(top, "scatter_mode") != "R") return false;
        auto tint = top->getInput("tint");
        if (tint && (IsBound(tint) || !IsWhite(tint->getValueString()))) return false;
        auto base = node->getInput("base");
        if (!base || !base->hasNodeName()) return false;
        EnsureHelperBsdf(graph->getDocument(), kFresnelLayer);
        const std::vector<MaterialX::PortElementPtr> consumers = node->getDownstreamPorts();
        auto layer = graph->addNode(kFresnelLayer, graph->createValidChildName(node->getName() + "_fresnel"), "BSDF");
        for (const char* input : {"ior", "weight", "normal"}) CopyInput(layer, top, input);
        layer->addInput("coat", "BSDF")->setNodeName(BareReflection(graph, top, top->getName() + "_bare")->getName());
        layer->addInput("substrate", "BSDF")->copyContentFrom(base);
        RedirectConsumers(consumers, layer);
        return true;
    }
    return false;
}

// What the repair does with a surface node's dielectric
// (CoupleTransmissionFresnel): couple its reflection and transmission lobes
// (they share IOR and roughness), through prism_dielectric_split when their
// tints differ; replace a smooth coat over smooth glass (ReplaceSmoothCoat);
// and the layers it leaves lossy (`partial`, logged once per material).
// Inputs that are bound count as non-zero (and non-white).
struct DielectricPlan {
    bool transmissive = false, couple = false, split = false, coat = false;
    std::vector<std::string> partial;
};

DielectricPlan PlanDielectric(const MaterialX::NodePtr& node)
{
    DielectricPlan plan;
    auto value = [&](const char* name) { return ConstantOrDefault(node, name); };
    auto declared = [&](const char* name) {
        auto nodedef = node->getNodeDef();
        return node->getInput(name) || (nodedef && nodedef->getActiveInput(name));
    };
    // Inputs the definition doesn't declare are zero.
    auto nonzero = [&](const char* name) { return declared(name) && !IsZero(value(name)); };
    auto transmissive = [&](const char* name) {
        auto input = node->getInput(name);
        return input && (IsBound(input) || !IsZero(input->getValueString()));
    };
    // A thin film needs untinted lobes; a coat is replaced when it and the
    // glass are smooth and share one IOR (MDL refracts with a single IOR; a
    // Fresnel layer of another IOR reflects totally inside at angles the
    // glass transmits, trapping light in closed glass).
    // Tints the definition doesn't declare leave the surface alone (a graph
    // other than MaterialX's).
    auto decide = [&](bool film, bool tinted, const char* coatWeight, const char* coatRoughness,
                      const char* roughness, const std::string& coatIor, const std::string& ior,
                      std::initializer_list<const char*> tints) {
        for (const char* tint : tints)
            if (!declared(tint)) return;
        if (film && tinted) {
            plan.partial.push_back("thin film over tinted glass");
            return;
        }
        bool sameIor = false;
        try { sameIor = !coatIor.empty() && !ior.empty() && std::stod(coatIor) == std::stod(ior); } catch (...) {}
        // The split's Fresnel layers take the glass's IOR, which a coat of
        // another IOR makes other than the material's.
        if (tinted && nonzero(coatWeight) && !sameIor) {
            plan.partial.push_back("tinted glass under a coat of another IOR");
            return;
        }
        plan.couple = true;
        plan.split = tinted;
        // The split's Fresnel layers weight rough lobes only approximately.
        if (tinted && nonzero(roughness)) plan.partial.push_back("rough tinted glass (approximately)");
        if (nonzero(coatWeight)) {
            if (nonzero(coatRoughness) || nonzero(roughness)) plan.partial.push_back("rough coat");
            else if (!sameIor) plan.partial.push_back("coat of another IOR");
            else plan.coat = true;
        }
    };
    const std::string& category = node->getCategory();
    if (category == "open_pbr_surface") {
        if (!transmissive("transmission_weight") || value("geometry_thin_walled") != "false") return plan;
        plan.transmissive = true;
        const bool tinted = !IsWhite(value("specular_color")) ||
                            !(IsWhite(value("transmission_color")) || IsPositive(value("transmission_depth")));
        decide(nonzero("thin_film_weight"), tinted, "coat_weight", "coat_roughness", "specular_roughness",
               value("coat_ior"), value("specular_ior"), {"specular_color", "transmission_color"});
        if (nonzero("coat_weight") && nonzero("coat_darkening")) plan.partial.push_back("coat darkening");
        if (nonzero("fuzz_weight")) plan.partial.push_back("fuzz");
    } else if (category == "standard_surface") {
        if (!transmissive("transmission") || value("thin_walled") != "false") return plan;
        plan.transmissive = true;
        if (!IsOne(value("specular")) || nonzero("transmission_extra_roughness")) {
            plan.partial.push_back("specular weight or transmission_extra_roughness");
            return plan;
        }
        const bool tinted = !IsWhite(value("specular_color")) || !IsWhite(value("transmission_color"));
        decide(nonzero("thin_film_thickness"), tinted, "coat", "coat_roughness", "specular_roughness", value("coat_IOR"),
               value("specular_IOR"), {"specular_color", "transmission_color"});
    } else if (category == "gltf_pbr") {
        if (!transmissive("transmission")) return plan;
        plan.transmissive = true;
        if (!IsOne(value("specular")) || !IsWhite(value("specular_color"))) {
            plan.partial.push_back("specular or specular_color");
            return plan;
        }
        decide(nonzero("iridescence"), !IsWhite(value("base_color")), "clearcoat", "clearcoat_roughness", "roughness",
               "1.5", value("ior"), {"base_color"}); // gltf_pbr's clearcoat has IOR 1.5
        if (!IsBlack(value("sheen_color"))) plan.partial.push_back("sheen");
    }
    return plan;
}

void ReportPartialRepair(const std::string& name, const DielectricPlan& plan)
{
    if (plan.partial.empty()) return;
    std::string reasons;
    for (auto const& reason : plan.partial) reasons += (reasons.empty() ? "" : ", ") + reason;
    std::cerr << "[MaterialX] WARNING: " << name << ": layered transmission only partly repaired (not: " << reasons
              << "); its glass may lose energy\n";
}

// The surface constructors an expanded node's consumers now read.
std::vector<MaterialX::NodePtr> ExpandedSurfaces(const MaterialX::GraphElementPtr& graph,
                                                 const std::vector<MaterialX::PortElementPtr>& downstream)
{
    std::vector<MaterialX::NodePtr> surfaces;
    for (auto const& port : downstream) {
        auto surface = graph->getNode(port->getNodeName());
        if (surface && surface->getCategory() == "surface" &&
            std::find(surfaces.begin(), surfaces.end(), surface) == surfaces.end())
            surfaces.push_back(surface);
    }
    return surfaces;
}

// Remove the nodes an expansion added that nothing reads (gltf_pbr's
// unconnected volume divides by an absent attenuation_distance and fails
// validation).
void PruneExpansion(const MaterialX::GraphElementPtr& graph, const std::set<std::string>& before)
{
    for (bool pruned = true; pruned;) {
        pruned = false;
        for (auto const& added : graph->getNodes())
            if (!before.contains(added->getName()) && added->getDownstreamPorts().empty()) {
                graph->removeNode(added->getName());
                pruned = true;
            }
    }
}

// The scatter_mode "T" dielectric_bsdf among the nodes a flattening added.
MaterialX::NodePtr AddedTransmission(const MaterialX::GraphElementPtr& graph, const std::set<std::string>& before)
{
    MaterialX::NodePtr found;
    for (auto const& added : graph->getNodes()) {
        if (before.contains(added->getName()) || added->getCategory() != "dielectric_bsdf") continue;
        auto mode = added->getInput("scatter_mode");
        if (!mode || mode->getValueString() != "T") continue;
        if (found) return nullptr; // ambiguous
        found = added;
    }
    return found;
}

// Expand the transmissive surfaces whose lobes agree into their genmdl graphs
// and couple reflection and transmission (CoupleTransmissionFresnel).
// Couple (and replace the smooth coat of) an expanded surface graph by its
// plan; false when the graph didn't have the expected shape.
bool RepairExpandedDielectric(const MaterialX::GraphElementPtr& graph, const MaterialX::NodePtr& transmission,
                              const std::vector<MaterialX::NodePtr>& surfaces, DielectricPlan& plan)
{
    if (!transmission || !CoupleTransmissionFresnel(graph, transmission, plan.split)) return false;
    if (plan.coat) {
        bool replaced = !surfaces.empty();
        for (auto const& surface : surfaces) replaced = ReplaceSmoothCoat(graph, surface) && replaced;
        if (!replaced) plan.partial.push_back("coat");
    }
    return true;
}

void CoupleSurfaceTransmissionFresnel(const MaterialX::DocumentPtr& doc)
{
    std::vector<std::pair<MaterialX::NodePtr, DielectricPlan>> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>()) {
            DielectricPlan plan = PlanDielectric(node);
            if (plan.couple || !plan.partial.empty()) nodes.emplace_back(node, plan);
        }
    const std::string target = "genmdl";
    for (auto& [node, plan] : nodes) {
        const std::string name = node->getName();
        auto graph = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr;
        auto implementation = node->getImplementation(target);
        if (!plan.couple || !graph || !implementation || !implementation->isA<MaterialX::NodeGraph>()) {
            ReportPartialRepair(name, plan);
            continue;
        }
        std::set<std::string> before;
        for (auto const& existing : graph->getNodes()) before.insert(existing->getName());
        const std::vector<MaterialX::PortElementPtr> downstream = node->getDownstreamPorts();
        graph->flattenSubgraphs(target, [&](MaterialX::NodePtr candidate) { return candidate == node; });
        if (graph->getNode(name) == node) continue; // not expanded
        const bool coupled = RepairExpandedDielectric(graph, AddedTransmission(graph, before),
                                                      ExpandedSurfaces(graph, downstream), plan);
        PruneExpansion(graph, before);
        if (!coupled)
            std::cerr << "[MaterialX] WARNING: " << name << " expanded without a layered transmission"
                      << " lobe; its glass may lose energy\n";
        else if (VerboseMdl())
            std::cout << "[MaterialX] " << name << ": reflection and transmission share one Fresnel"
                      << " dielectric" << (plan.split ? " (split by tint)" : "")
                      << (plan.coat ? ", smooth coat as a Fresnel layer" : "") << "\n";
        ReportPartialRepair(name, plan);
    }
}

// MaterialX 1.39.5's gltf_pbr graph builds the KHR_materials_volume medium
// (an anisotropic_vdf absorbing attenuation_coeff) but never connects it, so
// attenuation is dropped and thick glTF glass renders clear. open_pbr_surface
// layers its transmission over its VDF, which the MDL generator turns into
// the material's volume. Expand gltf_pbr nodes with a finite attenuation
// distance into their genmdl graph and add that layer.
void LayerGltfPbrVolume(const MaterialX::DocumentPtr& doc)
{
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree()) {
        auto node = element->asA<MaterialX::Node>();
        if (!node || node->getCategory() != "gltf_pbr") continue;
        auto distance = node->getInput("attenuation_distance");
        if (!distance) continue;
        if (!IsBound(distance)) {
            try {
                const float value = std::stof(distance->getValueString());
                if (!(value > 0.0f && value < 1e29f)) continue; // no attenuation
            } catch (...) { continue; }
        }
        nodes.push_back(node);
    }
    const std::string target = "genmdl";
    for (auto const& node : nodes) {
        auto graph = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr;
        auto implementation = node->getImplementation(target);
        if (!graph || !implementation || !implementation->isA<MaterialX::NodeGraph>()) {
            std::cerr << "[MaterialX] WARNING: no graph for " << node->getNamePath()
                      << "; its volume attenuation is ignored\n";
            continue;
        }
        std::set<std::string> before;
        for (auto const& existing : graph->getNodes()) before.insert(existing->getName());
        const std::string name = node->getName();
        DielectricPlan plan = PlanDielectric(node);
        const std::vector<MaterialX::PortElementPtr> downstream = node->getDownstreamPorts();
        graph->flattenSubgraphs(target, [&](MaterialX::NodePtr candidate) { return candidate == node; });
        if (graph->getNode(name) == node) continue; // not expanded

        MaterialX::NodePtr transmission, volume;
        for (auto const& added : graph->getNodes()) {
            if (before.contains(added->getName())) continue;
            if (added->getCategory() == "anisotropic_vdf") volume = added;
            else if (added->getCategory() == "dielectric_bsdf") {
                auto mode = added->getInput("scatter_mode");
                if (mode && mode->getValueString() == "T") transmission = added;
            }
        }
        if (!transmission || !volume) {
            std::cerr << "[MaterialX] WARNING: " << name << " expanded without a transmission BSDF and"
                      << " volume; its volume attenuation is ignored\n";
            continue;
        }
        const std::vector<MaterialX::PortElementPtr> consumers = transmission->getDownstreamPorts();
        auto layer = graph->addNode("layer", graph->createValidChildName(name + "_volume_transmission"), "BSDF");
        layer->addInput("top", "BSDF")->setNodeName(transmission->getName());
        layer->addInput("base", "VDF")->setNodeName(volume->getName());
        for (auto const& port : consumers) port->setNodeName(layer->getName());
        if (VerboseMdl())
            std::cout << "[MaterialX] " << name << ": transmission layered over its volume ("
                      << consumers.size() << " consumers)\n";
        if (plan.couple && !RepairExpandedDielectric(graph, transmission, ExpandedSurfaces(graph, downstream), plan))
            std::cerr << "[MaterialX] WARNING: " << name << ": no layered transmission lobe; its glass may lose energy\n";
        ReportPartialRepair(name, plan);
    }
}

// Expand one node into its genmdl graph; the names of the graph's nodes
// before, or nullptr when it has no graph or wasn't expanded.
MaterialX::GraphElementPtr ExpandNode(const MaterialX::NodePtr& node, std::set<std::string>& before)
{
    const std::string target = "genmdl";
    auto graph = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr;
    auto implementation = node->getImplementation(target);
    if (!graph || !implementation || !implementation->isA<MaterialX::NodeGraph>()) return nullptr;
    for (auto const& existing : graph->getNodes()) before.insert(existing->getName());
    const std::string name = node->getName();
    graph->flattenSubgraphs(target, [&](MaterialX::NodePtr candidate) { return candidate == node; });
    return graph->getNode(name) == node ? nullptr : graph;
}

std::vector<MaterialX::NodePtr> AddedNodes(const MaterialX::GraphElementPtr& graph, const std::set<std::string>& before,
                                           const std::string& category, const char* mode = nullptr)
{
    std::vector<MaterialX::NodePtr> found;
    for (auto const& added : graph->getNodes()) {
        if (before.contains(added->getName()) || added->getCategory() != category) continue;
        if (mode) {
            auto input = added->getInput("scatter_mode");
            if (!input || input->getValueString() != mode) continue;
        }
        found.push_back(added);
    }
    return found;
}

std::vector<MaterialX::NodePtr> NodesOf(const MaterialX::DocumentPtr& doc, const std::string& category)
{
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>(); node && node->getCategory() == category)
            nodes.push_back(node);
    return nodes;
}

bool SameColor(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty()) return false;
    try {
        MaterialX::Color3 x = MaterialX::fromValueString<MaterialX::Color3>(a);
        MaterialX::Color3 y = MaterialX::fromValueString<MaterialX::Color3>(b);
        return x == y;
    } catch (...) { return false; }
}

// UsdPreviewSurface with opacity < 1 in "transparent" mode (opacityMode 0,
// no opacityThreshold) refracts: MaterialX's graph (the one USD ships) is
//
//     layer(generalized_schlick R (F0 from ior), mix(diffuse, T (ior, smooth), opacity))
//
// and loses energy leaving the medium like the other surfaces (R + T 0.64 at
// 41 degrees inside). Its transmission is always smooth, so only a smooth
// specular lobe (roughness 0) agrees with it: such surfaces are rewritten as
//
//     mix(layer(R, diffuse), RT, opacity)
//
// (the opaque part keeps its Schlick reflection; the transparent part
// reflects with Fresnel's). A rough specular over the smooth transmission has
// no exact form (an RT lobe would blur the refraction), and neither do the
// metallic and specular workflows' tinted reflections or the clearcoat.
void CoupleUsdPreviewTransmission(const MaterialX::DocumentPtr& doc)
{
    for (auto const& node : NodesOf(doc, "UsdPreviewSurface")) {
        auto opacity = node->getInput("opacity");
        if (!opacity) continue;
        if (!IsBound(opacity)) {
            try { if (!(std::stod(opacity->getValueString()) < 1.0)) continue; } catch (...) { continue; }
        }
        auto value = [&](const char* name) { return ConstantOrDefault(node, name); };
        // A threshold makes opacity a cutout; presence mode a coverage.
        if (!IsZero(value("opacityThreshold")) || (!value("opacityMode").empty() && value("opacityMode") != "0"))
            continue;
        DielectricPlan plan;
        plan.transmissive = true;
        if (!IsZero(value("roughness"))) plan.partial.push_back("rough specular over smooth transmission");
        if (!IsZero(value("metallic"))) plan.partial.push_back("metallic");
        if (value("useSpecularWorkflow") != "0") plan.partial.push_back("specular workflow");
        if (!IsZero(value("clearcoat"))) plan.partial.push_back("clearcoat");
        const std::string name = node->getName();
        if (!plan.partial.empty()) {
            ReportPartialRepair(name, plan);
            continue;
        }
        std::set<std::string> before;
        auto graph = ExpandNode(node, before);
        if (!graph) continue;
        auto transmissions = AddedNodes(graph, before, "dielectric_bsdf", "T");
        bool coupled = false;
        if (transmissions.size() == 1) {
            auto transmission = transmissions.front();
            auto [mixture, port] = SoleConsumer(transmission);
            auto diffuse = mixture ? mixture->getInput("fg") : nullptr;
            if (mixture && mixture->getCategory() == "mix" && port == "bg" && diffuse && diffuse->hasNodeName()) {
                transmission->setInputValue("scatter_mode", std::string("RT"));
                for (auto const& use : mixture->getDownstreamPorts()) {
                    auto layer = use->getParent() ? use->getParent()->asA<MaterialX::Node>() : nullptr;
                    if (!layer || layer->getCategory() != "layer" || use->getName() != "base") continue;
                    const std::vector<MaterialX::PortElementPtr> consumers = layer->getDownstreamPorts();
                    auto opaque = graph->addNode("layer", graph->createValidChildName(layer->getName() + "_opaque"), "BSDF");
                    opaque->addInput("top", "BSDF")->copyContentFrom(layer->getInput("top"));
                    opaque->addInput("base", "BSDF")->setNodeName(diffuse->getNodeName());
                    auto coupledMix =
                        graph->addNode("mix", graph->createValidChildName(layer->getName() + "_fresnel_coupled"), "BSDF");
                    coupledMix->addInput("fg", "BSDF")->setNodeName(opaque->getName());
                    coupledMix->addInput("bg", "BSDF")->setNodeName(transmission->getName());
                    if (auto weight = mixture->getInput("mix")) coupledMix->addInput("mix", "float")->copyContentFrom(weight);
                    RedirectConsumers(consumers, coupledMix);
                    coupled = true;
                }
            }
        }
        PruneExpansion(graph, before);
        if (!coupled)
            std::cerr << "[MaterialX] WARNING: " << name << " expanded without a layered transmission"
                      << " lobe; its glass may lose energy\n";
        else if (VerboseMdl())
            std::cout << "[MaterialX] " << name << ": transparent UsdPreviewSurface as an RT dielectric\n";
    }
}

// LamaDielectric layers its reflection over its transmission (over the
// interior VDF), both dielectric_bsdf with the same IOR, roughness and frame:
// layer(R, layer(T, VDF)) loses the energy of light leaving the medium. With
// equal tints (Lama's reflectionTint and transmissionTint are meant for
// external reflection and inward transmission; MaterialX applies each to its
// lobe on both sides) it is layer(RT, VDF).
void CoupleLamaDielectric(const MaterialX::DocumentPtr& doc)
{
    for (auto const& node : NodesOf(doc, "LamaDielectric")) {
        const std::string name = node->getName();
        if (!SameColor(ConstantOrDefault(node, "reflectionTint"), ConstantOrDefault(node, "transmissionTint"))) {
            DielectricPlan plan;
            plan.partial.push_back("reflectionTint other than transmissionTint");
            ReportPartialRepair(name, plan);
            continue;
        }
        std::set<std::string> before;
        auto graph = ExpandNode(node, before);
        if (!graph) continue;
        auto transmissions = AddedNodes(graph, before, "dielectric_bsdf", "T");
        auto reflections = AddedNodes(graph, before, "dielectric_bsdf", "R");
        bool coupled = false;
        if (transmissions.size() == 1 && reflections.size() == 1) {
            auto [interior, port] = SoleConsumer(transmissions.front());
            auto [layer, layerPort] = SoleConsumer(reflections.front());
            if (interior && interior->getCategory() == "layer" && port == "top" && layer &&
                layer->getCategory() == "layer" && layerPort == "top" && Producer(layer, "base") == interior) {
                transmissions.front()->setInputValue("scatter_mode", std::string("RT"));
                RedirectConsumers(layer->getDownstreamPorts(), interior);
                coupled = true;
            }
        }
        PruneExpansion(graph, before);
        if (!coupled)
            std::cerr << "[MaterialX] WARNING: " << name << " expanded without a layered transmission"
                      << " lobe; its glass may lose energy\n";
        else if (VerboseMdl())
            std::cout << "[MaterialX] " << name << ": LamaDielectric as an RT dielectric\n";
    }
}

// LamaGeneralizedSchlick layers multiply(R, reflectionTint) over its
// transmission; MaterialX's MDL generator threads a layer's base only into a
// top BSDF with a base input ("TODO handle mix, add, and multiply" in
// ClosureLayerNodeMdl), so the transmission is dropped and the surface only
// reflects. A Schlick curve is linear in its colors, so the tint moves into
// the reflection lobe's color0 and color90 and the lobe becomes the top. The
// Schlick layer over the refracting base keeps its exit loss (a Schlick curve
// has no IOR), and its transmission lobe carries a Schlick weight of its own
// on top of the layer's (T 0.92 at normal incidence), which is logged.
//
// When the curve is Fresnel's Schlick approximation (reflectivity90 white,
// reflectivityProfile 0.2, a gray constant reflectivity or IOR) and both
// tints agree, the node is instead the RT dielectric of that IOR over its
// interior, which conserves energy (its reflection becomes Fresnel's).
std::optional<float> LamaSchlickIor(const MaterialX::NodePtr& node)
{
    auto value = [&](const char* name) { return ConstantOrDefault(node, name); };
    if (!IsWhite(value("reflectivity90")) || !SameColor(value("reflectionTint"), value("transmissionTint")))
        return std::nullopt;
    try {
        if (std::abs(std::stod(value("reflectivityProfile")) - 0.2) > 1e-6) return std::nullopt;
        const std::string mode = value("fresnelMode");
        const MaterialX::Color3 c = MaterialX::fromValueString<MaterialX::Color3>(
            value(mode == "1" ? "IOR" : "reflectivity"));
        if (c[0] != c[1] || c[1] != c[2]) return std::nullopt;
        if (mode == "1") return c[0] > 1.0f ? std::optional<float>(c[0]) : std::nullopt;
        if (mode != "0" || !(c[0] > 0.0f && c[0] < 1.0f)) return std::nullopt;
        const float root = std::sqrt(c[0]);
        return (1.0f + root) / (1.0f - root);
    } catch (...) { return std::nullopt; }
}

bool LamaSchlickAsRt(const MaterialX::GraphElementPtr& graph, const std::set<std::string>& before, float ior)
{
    auto transmissions = AddedNodes(graph, before, "generalized_schlick_bsdf", "T");
    if (transmissions.size() != 1) return false;
    auto transmission = transmissions.front();
    auto [interior, port] = SoleConsumer(transmission);
    if (!interior || interior->getCategory() != "layer" || port != "top") return false;
    auto [tinted, tintPort] = SoleConsumer(interior);
    if (!tinted || tinted->getCategory() != "multiply" || tintPort != "in1") return false;
    auto [layer, layerPort] = SoleConsumer(tinted);
    if (!layer || layer->getCategory() != "layer" || layerPort != "base") return false;
    auto rt = graph->addNode("dielectric_bsdf", graph->createValidChildName(transmission->getName() + "_rt"), "BSDF");
    rt->setInputValue("ior", ior);
    CopyInput(rt, tinted, "in2", "tint");
    for (const char* input : {"roughness", "normal", "tangent", "distribution"}) CopyInput(rt, transmission, input);
    rt->setInputValue("scatter_mode", std::string("RT"));
    auto volume = graph->addNode("layer", graph->createValidChildName(interior->getName() + "_rt"), "BSDF");
    volume->addInput("top", "BSDF")->setNodeName(rt->getName());
    volume->addInput("base", "VDF")->copyContentFrom(interior->getInput("base"));
    RedirectConsumers(layer->getDownstreamPorts(), volume);
    return true;
}

void RepairLamaGeneralizedSchlick(const MaterialX::DocumentPtr& doc)
{
    for (auto const& node : NodesOf(doc, "LamaGeneralizedSchlick")) {
        const std::string name = node->getName();
        const std::optional<float> ior = LamaSchlickIor(node);
        std::set<std::string> before;
        auto graph = ExpandNode(node, before);
        if (!graph) continue;
        if (ior && LamaSchlickAsRt(graph, before, *ior)) {
            PruneExpansion(graph, before);
            if (VerboseMdl())
                std::cout << "[MaterialX] " << name << ": LamaGeneralizedSchlick as an RT dielectric of IOR " << *ior
                          << "\n";
            continue;
        }
        bool repaired = false;
        for (auto const& multiply : AddedNodes(graph, before, "multiply")) {
            if (multiply->getType() != "BSDF") continue;
            auto lobe = Producer(multiply, "in1");
            auto [layer, port] = SoleConsumer(multiply);
            auto tint = multiply->getInput("in2");
            if (!lobe || lobe->getCategory() != "generalized_schlick_bsdf" || !tint || !layer ||
                layer->getCategory() != "layer" || port != "top")
                continue;
            for (const char* color : {"color0", "color90"}) {
                auto tinted = graph->addNode("multiply", graph->createValidChildName(lobe->getName() + "_" + color),
                                             "color3");
                auto in1 = tinted->addInput("in1", "color3");
                if (auto source = lobe->getInput(color)) in1->copyContentFrom(source);
                else in1->setValueString("1, 1, 1");
                tinted->addInput("in2", "color3")->copyContentFrom(tint);
                auto input = lobe->getInput(color);
                if (!input) input = lobe->addInput(color, "color3");
                for (const char* attribute : {"value", "nodename", "nodegraph", "output", "interfacename"})
                    input->removeAttribute(attribute);
                input->setNodeName(tinted->getName());
            }
            layer->getInput("top")->setNodeName(lobe->getName());
            repaired = true;
        }
        PruneExpansion(graph, before);
        if (!repaired) {
            std::cerr << "[MaterialX] WARNING: " << name << ": no tinted reflection over its transmission; "
                      << "MDL renders its reflection only\n";
            continue;
        }
        DielectricPlan plan;
        plan.partial.push_back("Schlick reflection over refraction");
        ReportPartialRepair(name, plan);
    }
}

void NormalizeColorSpaces(const MaterialX::DocumentPtr& doc)
{
    const std::string& attribute = MaterialX::Element::COLOR_SPACE_ATTRIBUTE;
    if (doc->hasAttribute(attribute))
        doc->setAttribute(attribute, CanonicalColorSpace(doc->getAttribute(attribute)));
    for (auto element : doc->traverseTree()) {
        if (!element->hasAttribute(attribute)) continue;
        const std::string value = element->getAttribute(attribute);
        const std::string canonical = CanonicalColorSpace(value);
        if (canonical != value) {
            element->setAttribute(attribute, canonical);
            if (VerboseMdl())
                std::cout << "[MaterialX] color space '" << value << "' read as '" << canonical
                          << "' at " << element->getNamePath() << "\n";
        }
    }
}
}

namespace {
// -1: not set (the environment variable decides), else 0 or 1.
std::atomic<int> sssFallbackSetting{-1};
}

bool DocumentLoader::DefaultSssFallback()
{
    if (const int set = sssFallbackSetting.load(); set >= 0) return set != 0;
    const char* value = std::getenv("PRISM_SSS_FALLBACK");
    return value && *value && std::string(value) != "0";
}

void DocumentLoader::SetSssFallback(bool enabled)
{
    sssFallbackSetting.store(enabled ? 1 : 0);
}

DocumentLoader::DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp,
                               bool fallback_sss_to_diffuse)
    : m_stdlib(stdlib), m_clamp(clamp),
      m_fallback_sss_to_diffuse(fallback_sss_to_diffuse)
{}

MaterialX::DocumentPtr DocumentLoader::load(const std::string& filepath)
{
    MaterialX::DocumentPtr doc = MaterialX::createDocument();

    try {
        MaterialX::readFromXmlFile(doc, filepath);

        // Renderer compatibility preprocessing, alongside the color workaround
        // below. Run before importing stdlib so library implementations are not
        // rewritten. Only this in-memory document changes, never the asset file.
        NormalizeColorSpaces(doc);
        FixDeclaredInputTypes(doc, m_stdlib);
        FixConnectionTypes(doc);
        DistributeLayersOverMixes(doc);
        if (m_fallback_sss_to_diffuse) {
            // Collect first: adding an absent weight input mutates the tree.
            std::vector<MaterialX::NodePtr> surface_nodes;
            for (auto element : doc->traverseTree()) {
                auto node = element->asA<MaterialX::Node>();
                if (node && (node->getCategory() == "standard_surface" ||
                             node->getCategory() == "open_pbr_surface"))
                    surface_nodes.push_back(node);
            }
            for (auto node : surface_nodes) {
                const std::string weight = node->getCategory() == "standard_surface"
                    ? "subsurface" : "subsurface_weight";
                auto input = node->getInput(weight);
                bool authoredUnsupportedSss = false;
                if (input) {
                    authoredUnsupportedSss = input->hasAttribute("nodename") ||
                        input->hasAttribute("nodegraph") ||
                        input->hasAttribute("output") ||
                        input->hasAttribute("interfacename");
                    if (!authoredUnsupportedSss && !input->getValueString().empty()) {
                        try {
                            authoredUnsupportedSss = std::stof(input->getValueString()) != 0.0f;
                        } catch (...) {
                            authoredUnsupportedSss = true;
                        }
                    }
                }
                if (!input) input = node->addInput(weight, "float");

                // A value does not override a connection. Remove every binding
                // form, including graph outputs and inherited graph interfaces.
                for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"})
                    input->removeAttribute(attribute);
                input->setValue(0.0f);
                if (authoredUnsupportedSss)
                    std::cerr << "[MaterialX] WARNING: unsupported authored SSS disabled at "
                              << node->getNamePath() << "." << weight << "\n";
                else if (VerboseMdl())
                    std::cout << "[MaterialX] SSS-to-diffuse fallback: "
                              << node->getNamePath() << "." << weight << " = 0\n";
            }
        }

        // Clamp the standard_surface inputs that feed artistic_ior (base_color ->
        // reflectivity, specular_color -> edge_color) to [0, 0.9999]. Upstream
        // MaterialX bug: values above one (e.g. physicallybased.info gold,
        // specular_color.R = 1.001) extrapolate the conductor IOR below zero in
        // the MDL translation, which renders as a green disk at normal incidence.
        // open_pbr_surface uses a Schlick conductor and does not need this.
        if (m_clamp) {
            std::vector<MaterialX::NodePtr> surface_nodes;
            for (auto element : doc->traverseTree()) {
                auto node = element->asA<MaterialX::Node>();
                if (node && node->getCategory() == "standard_surface")
                    surface_nodes.push_back(node);
            }
            for (auto node : surface_nodes) {
                for (const char* name : {"base_color", "specular_color"}) {
                    auto input = node->getInput(name);
                    if (!input) continue; // library defaults are in range

                    const bool connected = input->hasAttribute("nodename") ||
                        input->hasAttribute("nodegraph") || input->hasAttribute("interfacename");
                    const std::string& space = input->getActiveColorSpace();
                    const bool working_space = space.empty() || space == "none" || space == "lin_rec709";

                    if (!connected && working_space) {
                        // Constant already in the working space: clamp in place.
                        if (!input->getValue() || !input->getValue()->isA<MaterialX::Color3>())
                            continue;
                        const MaterialX::Color3 orig = input->getValue()->asA<MaterialX::Color3>();
                        MaterialX::Color3 c = orig;
                        for (size_t i = 0; i < 3; ++i)
                            c[i] = std::clamp(c[i], 0.0f, 0.9999f);
                        if (c != orig) {
                            input->setValue(c);
                            std::cout << std::format(
                                "[MaterialX] Clamped '{}.{}': ({:.4f},{:.4f},{:.4f}) -> ({:.4f},{:.4f},{:.4f})\n",
                                node->getNamePath(), name, orig[0], orig[1], orig[2], c[0], c[1], c[2]);
                        }
                        continue;
                    }

                    // Textures, graph connections, and constants that color
                    // management will still transform (e.g. acescg) are only
                    // known at shading time: move the binding onto a clamp
                    // node so the limit applies after the conversion.
                    auto parent = node->getParent()->asA<MaterialX::GraphElement>();
                    if (!parent) continue;
                    auto clamp = parent->addNode("clamp",
                        parent->createValidChildName(node->getName() + "_" + name + "_clamp"), "color3");
                    auto in = clamp->addInput("in", "color3");
                    for (const char* attribute : {"value", "colorspace", "nodename", "nodegraph", "output", "interfacename"}) {
                        if (input->hasAttribute(attribute)) {
                            in->setAttribute(attribute, input->getAttribute(attribute));
                            input->removeAttribute(attribute);
                        }
                    }
                    clamp->setInputValue("high", MaterialX::Color3(0.9999f));
                    input->setNodeName(clamp->getName());
                    std::cout << "[MaterialX] Inserted clamp node on "
                              << node->getNamePath() << "." << name << "\n";
                }
            }
        }

        // Attach after preprocessing so shared standard-library graphs retain
        // their original behavior. Validate the final document used for generation.
        AttachLibrary(doc, m_stdlib);
        // Both need the library (declared defaults, graph implementations).
        RouteUncoatedTransmissionIor(doc);
        ExpandThinWalledOpenPbr(doc);
        LayerGltfPbrVolume(doc);
        CoupleSurfaceTransmissionFresnel(doc);
        CoupleUsdPreviewTransmission(doc);
        CoupleLamaDielectric(doc);
        RepairLamaGeneralizedSchlick(doc);
        std::string validation_errors;
        if (!doc->validate(&validation_errors)) {
            std::cerr << "[WARN] Document validation issues: " << validation_errors << "\n";
        }
        return doc;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load '" << filepath << "': " << e.what() << "\n";
        return nullptr;
    }
}

MaterialX::DocumentPtr DocumentLoader::create_empty()
{
    MaterialX::DocumentPtr doc = MaterialX::createDocument();
    AttachLibrary(doc, m_stdlib);
    return doc;
}
