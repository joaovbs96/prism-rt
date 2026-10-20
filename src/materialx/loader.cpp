#include "loader.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXCore/Traversal.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <vector>
#include <cstdlib>
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
        {"raw", "lin_rec709"}, {"utility - raw", "lin_rec709"}, {"non-color", "lin_rec709"},
        {"data", "lin_rec709"}, {"linear", "lin_rec709"}, {"lin_srgb", "lin_rec709"},
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

// MaterialX's open_pbr_surface graphs (1.39.4 and 1.39.5, genmdl variant
// included) never connect geometry_thin_walled to their <surface>
// constructor, so the MDL material is always closed: thin-walled glass
// refracts like a solid and fills with the transmission_depth medium. Expand
// such nodes into their genmdl graph and set the constructor's thin_walled
// flag; a thin-walled MDL material scatters with its backface BSDF from the
// back, so the material gets the same shader there.
//
// OpenPBR's thin wall is an infinitesimal sheet whose transmittance at normal
// incidence is transmission_color, whatever transmission_depth says. The
// graph uses transmission_color as the tint only when the depth is zero
// (otherwise it becomes the volume's absorption, which a thin-walled MDL
// material ignores), so the depth is cleared as well.
void ExpandThinWalledOpenPbr(const MaterialX::DocumentPtr& doc)
{
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>();
            node && node->getCategory() == "open_pbr_surface" && IsTrueConstant(node->getInput("geometry_thin_walled")))
            nodes.push_back(node);
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
        auto depth = node->getInput("transmission_depth");
        if (!depth) depth = node->addInput("transmission_depth", "float");
        for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"})
            depth->removeAttribute(attribute);
        depth->setValue(0.0f);

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

bool DocumentLoader::DefaultSssFallback()
{
    const char* value = std::getenv("PRISM_SSS_FALLBACK");
    return value && *value && std::string(value) != "0";
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
        // Expanding a node into its implementation needs the library.
        ExpandThinWalledOpenPbr(doc);
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
