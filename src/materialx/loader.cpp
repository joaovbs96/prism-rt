#include "loader.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXCore/Traversal.h>
#include <MaterialXCore/Util.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <format>
#include <iostream>
#include <mutex>
#include <set>
#include <tuple>
#include <vector>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string_view>

namespace {
// The verbosity of the DocumentLoader::load running on this thread
// (DocumentLoader::setVerbose: the renderer's "mdl" debug output).
thread_local bool tVerbose = false;
bool VerboseMdl() { return tVerbose; }

struct VerboseScope {
    bool previous;
    explicit VerboseScope(bool verbose) : previous(tVerbose) { tVerbose = verbose; }
    ~VerboseScope() { tVerbose = previous; }
};

// What the DocumentLoader::load running on this thread repaired, for its
// one-line summary (the details are verbose output).
thread_local DocumentLoader::RepairCounts tRepairs;

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

// An integer input's value named by its declaration's enum (UsdPreviewSurface's
// opacityMode "transparent" / "presence", which USD authors as a token and
// MaterialX declares as an integer with enum names and enumvalues).
std::string EnumValueString(const MaterialX::InputPtr& declared, const std::string& name)
{
    const std::vector<std::string> names =
        MaterialX::splitString(declared->getAttribute(MaterialX::ValueElement::ENUM_ATTRIBUTE), ",");
    const std::vector<std::string> values =
        MaterialX::splitString(declared->getAttribute(MaterialX::ValueElement::ENUM_VALUES_ATTRIBUTE), ",");
    const std::string key = MaterialX::trimSpaces(name);
    for (size_t i = 0; i < names.size(); ++i) {
        if (MaterialX::trimSpaces(names[i]) != key) continue;
        if (values.empty()) return std::to_string(i);
        return i < values.size() ? MaterialX::trimSpaces(values[i]) : std::string();
    }
    return {};
}

// The definition a node without an explicit (or known) nodedef gets, as
// Node::getNodeDef picks it: its category and output type, and its version
// (the default version when it names none); empty when that is ambiguous
// (convert has one definition per input type for the same output type).
MaterialX::NodeDefPtr DeclaredNodeDef(const MaterialX::DocumentPtr& doc, const MaterialX::DocumentPtr& library,
                                      const MaterialX::NodePtr& node)
{
    if (node->hasNodeDefString()) {
        if (auto nodedef = library->getNodeDef(node->getNodeDefString())) return nodedef;
        if (auto nodedef = doc->getNodeDef(node->getNodeDefString())) return nodedef;
    }
    std::vector<MaterialX::NodeDefPtr> candidates = library->getMatchingNodeDefs(node->getCategory());
    auto local = doc->getMatchingNodeDefs(node->getCategory());
    candidates.insert(candidates.end(), local.begin(), local.end());
    MaterialX::NodeDefPtr nodedef;
    int matches = 0;
    for (auto const& candidate : candidates)
        if (candidate->getType() == node->getType() && candidate->isVersionCompatible(node->getVersionString())) {
            nodedef = candidate;
            ++matches;
        }
    return matches == 1 ? nodedef : nullptr;
}

// USD's UsdPrimvarReader_matrix has no MaterialX counterpart (MaterialX
// 1.39.5 leaves ND_UsdPrimvarReader_matrix44 commented out: "Getting primvar
// of matrix type is not supported"). hdMtlx then finds no definition for
// ND_UsdPrimvarReader_matrix and writes a node of another category and type
// ("surface", surfaceshader) that nothing can generate, and the material
// renders as the error material. Such a reader becomes a constant of its
// fallback (identity by default), as when the primvar is missing, with a
// warning.
void ReplaceMatrixPrimvarReaders(const MaterialX::DocumentPtr& doc, const MaterialX::DocumentPtr& library)
{
    if (!library) return;
    const std::string name = "ND_UsdPrimvarReader_matrix";
    if (library->getNodeDef(name) || doc->getNodeDef(name) || !library->getNodeDef("ND_constant_matrix44")) return;
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>(); node && node->getNodeDefString() == name)
            nodes.push_back(node);
    for (auto const& node : nodes) {
        std::string fallback = "1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1";
        // An authored fallback: a constant matrix (sixteen numbers).
        if (auto input = node->getInput("fallback"); input && input->hasValueString() && !input->hasNodeName() &&
            !input->hasNodeGraphString() && !input->hasInterfaceName()) {
            const std::string value = input->getValueString();
            if (std::count(value.begin(), value.end(), ',') == 15) fallback = value;
        }
        for (auto const& input : node->getInputs()) node->removeInput(input->getName());
        node->setCategory("constant");
        node->setType("matrix44");
        node->setNodeDefString("ND_constant_matrix44");
        node->addInput("value", "matrix44")->setValueString(fallback);
        std::cerr << "[MaterialX] WARNING: " << node->getNamePath()
                  << ": MaterialX cannot read matrix primvars (UsdPrimvarReader_matrix); using its fallback\n";
    }
}

// Inputs whose type contradicts their node definition (hdMtlx types a
// shader input after the graph output feeding it, and some exporters write
// vector parameters as strings, flags as integers or enum names for
// integers) get their declared type; values are converted, and a graph
// output feeding them gets the extract/convert node the connection implies.
//
// Inputs the definition does not declare make MaterialX find no definition
// for a node that does not name one (Node::getNodeDef matches the inputs),
// and the material fails. open_pbr_surface's geometry inputs
// written under standard_surface's names (normal, tangent, coat_normal,
// thin_walled) take OpenPBR's names (the Typhoon suite's bump test connects
// `normal`, and Typhoon renders its bump); other undeclared inputs are
// dropped with a warning.
void FixDeclaredInputTypes(const MaterialX::DocumentPtr& doc, const MaterialX::DocumentPtr& library)
{
    if (!library) return;
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>()) nodes.push_back(node);
    for (auto const& node : nodes) {
        MaterialX::NodeDefPtr nodedef = DeclaredNodeDef(doc, library, node);
        if (!nodedef) continue;
        for (auto input : node->getInputs()) {
            auto declared = nodedef->getActiveInput(input->getName());
            // (A node naming its definition gets it whatever its inputs.)
            if (!declared && node->hasNodeDefString()) continue;
            if (!declared) {
                const std::string name = input->getName();
                const std::string renamed = "geometry_" + name;
                auto geometry = node->getCategory() == "open_pbr_surface" &&
                        (name == "normal" || name == "tangent" || name == "coat_normal" || name == "thin_walled")
                    ? nodedef->getActiveInput(renamed) : nullptr;
                if (geometry && !node->getInput(renamed)) {
                    input->setName(renamed);
                    declared = geometry;
                    std::cerr << "[MaterialX] " << node->getNamePath() << ": input '" << name << "' read as '"
                              << renamed << "'\n";
                } else {
                    std::cerr << "[MaterialX] WARNING: " << node->getNamePath() << " has no input '" << name
                              << "' (" << nodedef->getName() << "); ignored\n";
                    node->removeInput(name);
                    continue;
                }
            }
            if (declared->getType() == input->getType()) continue;
            const std::string from = input->getType(), to = declared->getType();
            auto graphElement = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr;
            auto graph = input->hasNodeGraphString() ? doc->getNodeGraph(input->getNodeGraphString()) : nullptr;
            auto output = graph && input->hasOutputString() ? graph->getOutput(input->getOutputString()) : nullptr;
            if (output && output->getType() == from && output->hasNodeName()) {
                auto adapter = AddAdapter(graph, from, to, output->getNodeName(), output->getNodeName());
                if (!adapter) continue;
                output->setNodeName(adapter->getName());
                output->setType(to);
                ++tRepairs.connectionTypes;
            } else if (input->hasNodeName()) {
                // Connected within the graph: FixConnectionTypes then adapts
                // an upstream of another type.
                auto upstream = graphElement ? graphElement->getNode(input->getNodeName()) : nullptr;
                if (!upstream) continue;
            } else if (input->hasValueString()) {
                std::string value = ConvertValueString(input->getValueString(), to);
                if (value.empty() && to == "integer") value = EnumValueString(declared, input->getValueString());
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
        ++tRepairs.connectionTypes;
        if (VerboseMdl())
            std::cout << "[MaterialX] " << from << " output of " << fix.upstream->getNamePath() << " feeds "
                      << to << " input " << fix.input->getNamePath() << "; inserted " << adapter->getCategory()
                      << "\n";
    }
}

// MaterialX's MDL generator layers a BSDF over a base by handing the base to
// the top lobe; a mix as the top passes nothing on and the base is lost. A
// layer distributes over a mix of its top:
//   layer(mix(fg, bg, m), base) = mix(layer(fg, base), layer(bg, base), m).
void DistributeLayersOverMixes(const MaterialX::ElementPtr& root)
{
    std::vector<MaterialX::NodePtr> work;
    for (auto element : root->traverseTree())
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

// The constant an input of a node inside `instance`'s graph implementation
// takes: its value, or the instance's for an interface input (empty when
// bound to a node, or when the instance's input is).
std::string ResolvedConstant(const MaterialX::InputPtr& input, const MaterialX::NodePtr& instance)
{
    if (!input) return {};
    if (input->hasInterfaceName()) return instance ? ConstantOrDefault(instance, input->getInterfaceName()) : "";
    return IsBound(input) ? std::string() : input->getValueString();
}

// Repairs rewrite a private copy of a node's genmdl graph implementation
// instead of flattening the graph into the material. MaterialX's MDL
// generator emits a graph implementation as an MDL function whose internal
// constants are literals, but publishes every unconnected input of the
// material's own nodes as a material parameter: a flattened open_pbr_surface
// turned its ~100 internal constants into class-compilation arguments, the
// uniform scatter modes among them, so every dielectric_bsdf kept all three of
// its lobe variants (mx_dielectric_bsdf selects R, T or RT by scatter_mode)
// and the GPU programs of repaired glass grew 3.5x. The copy has a nodedef
// and category of its own, named by its content once rewritten, so materials
// with the same repair share one MDL function (and class program) and keep
// the surface's inputs as their parameters.
struct PrivateGraph {
    MaterialX::NodeGraphPtr graph;
    MaterialX::NodeDefPtr nodedef;
    std::string category, nodedefString; // the node's, before
    std::string source;                  // the library nodedef
    explicit operator bool() const { return bool(graph); }
};

// Make an element and its descendants the document's own: copies keep the
// library file's source URI, and MaterialX writes such elements as an
// XInclude of that file instead of their content (the generation cache keys
// and PublishPrivateGraph's name read the written document).
void ClearSourceUris(const MaterialX::ElementPtr& element)
{
    element->setSourceUri("");
    for (auto const& child : element->getChildren()) ClearSourceUris(child);
}

// Copy the node's genmdl graph implementation under a draft name and point
// the node at it (its inputs are unchanged).
PrivateGraph ClonePrivateGraph(const MaterialX::NodePtr& node)
{
    const std::string target = "genmdl";
    auto doc = node->getDocument();
    auto nodedef = node->getNodeDef(target);
    auto implementation = node->getImplementation(target);
    auto source = implementation ? implementation->asA<MaterialX::NodeGraph>() : nullptr;
    if (!doc || !nodedef || !source) return {};
    PrivateGraph clone;
    clone.category = node->getCategory();
    clone.nodedefString = node->getNodeDefString();
    clone.source = nodedef->getName();
    const std::string draft = doc->createValidChildName("ND_prism_" + clone.category + "_draft");
    const std::string category = draft.substr(3);
    clone.nodedef = doc->addNodeDef(draft, "", category);
    clone.nodedef->copyContentFrom(nodedef);
    clone.nodedef->setNodeString(category);
    clone.graph = doc->addNodeGraph(doc->createValidChildName("NG_" + category));
    clone.graph->copyContentFrom(source);
    clone.graph->setNodeDefString(draft);
    clone.graph->removeAttribute(MaterialX::InterfaceElement::TARGET_ATTRIBUTE);
    ClearSourceUris(clone.nodedef);
    ClearSourceUris(clone.graph);
    node->setCategory(category);
    node->setNodeDefString(draft);
    return clone;
}

// Undo ClonePrivateGraph: the node uses the library's graph again.
void DiscardPrivateGraph(const MaterialX::NodePtr& node, PrivateGraph& clone)
{
    auto doc = node->getDocument();
    doc->removeNodeGraph(clone.graph->getName());
    doc->removeNodeDef(clone.nodedef->getName());
    node->setCategory(clone.category);
    if (clone.nodedefString.empty()) node->removeAttribute(MaterialX::InterfaceElement::NODE_DEF_ATTRIBUTE);
    else node->setNodeDefString(clone.nodedefString);
    clone = {};
}

// The library category of a node category, also when that is already a
// published private copy's (prism_<category>_<16 hex digits>): a repair that
// rewrites a copy another repair made names its result after the library
// node too.
std::string LibraryCategory(const std::string& category)
{
    const std::string prefix = "prism_";
    if (category.size() > prefix.size() + 17 && category.rfind(prefix, 0) == 0 &&
        category[category.size() - 17] == '_' &&
        category.find_first_not_of("0123456789abcdef", category.size() - 16) == std::string::npos)
        return category.substr(prefix.size(), category.size() - prefix.size() - 17);
    return category;
}

// Name a rewritten copy by its content (FNV-1a of its XML): one definition
// per distinct repair, shared by every node that needs it.
void PublishPrivateGraph(const MaterialX::NodePtr& node, PrivateGraph& clone)
{
    auto doc = node->getDocument();
    auto scratch = MaterialX::createDocument();
    auto copy = scratch->addNodeGraph("graph");
    copy->copyContentFrom(clone.graph);
    copy->removeAttribute(MaterialX::InterfaceElement::NODE_DEF_ATTRIBUTE);
    MaterialX::XmlWriteOptions options;
    options.writeXIncludeEnable = false;
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : clone.source + "|" + MaterialX::writeToXmlString(scratch, &options)) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    const std::string category = std::format("prism_{}_{:016x}", LibraryCategory(clone.category), hash);
    const std::string nodedefName = "ND_" + category;
    if (doc->getNodeDef(nodedefName)) {
        doc->removeNodeGraph(clone.graph->getName());
        doc->removeNodeDef(clone.nodedef->getName());
    } else {
        clone.nodedef->setName(nodedefName);
        clone.nodedef->setNodeString(category);
        clone.graph->setName("NG_" + category);
        clone.graph->setNodeDefString(nodedefName);
    }
    node->setCategory(category);
    node->setNodeDefString(nodedefName);
    // A copy this one rewrote goes when no node uses it any more.
    const std::string previous = clone.nodedefString;
    if (previous != nodedefName && LibraryCategory(clone.category) != clone.category) {
        bool used = false;
        for (auto element : doc->traverseTree())
            if (auto other = element->asA<MaterialX::Node>(); other && other->getNodeDefString() == previous) {
                used = true;
                break;
            }
        if (!used) {
            for (auto const& graph : doc->getNodeGraphs())
                if (graph->getNodeDefString() == previous) doc->removeNodeGraph(graph->getName());
            doc->removeNodeDef(previous);
        }
    }
    clone = {};
}

// The ports reading a graph's result: its outputs.
std::vector<MaterialX::PortElementPtr> OutputPorts(const MaterialX::NodeGraphPtr& graph)
{
    std::vector<MaterialX::PortElementPtr> ports;
    for (auto const& output : graph->getOutputs()) ports.push_back(output);
    return ports;
}

// The surface constructors a graph's outputs read.
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
// the depth. Such nodes get a private copy of their genmdl graph
// (ClonePrivateGraph) whose constructor is thin-walled; a thin-walled MDL
// material scatters with its backface BSDF from the back, so the material gets
// the same shader there (whose scattering the backends then compile once).
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
// nodes get the same thin-walled graph when they transmit or scatter (an opaque one
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

    for (auto const& node : nodes) {
        if (node->getCategory() == "open_pbr_surface") {
            auto depth = node->getInput("transmission_depth");
            if (!depth) depth = node->addInput("transmission_depth", "float");
            for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"})
                depth->removeAttribute(attribute);
            depth->setValue(0.0f);
        }
        // A private copy of the MDL generator's own choice of implementation
        // (ClonePrivateGraph).
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) {
            std::cerr << "[MaterialX] WARNING: no graph for thin-walled " << node->getNamePath()
                      << "; it renders as a closed surface\n";
            continue;
        }
        const std::vector<MaterialX::NodePtr> surfaces = ExpandedSurfaces(clone.graph, OutputPorts(clone.graph));
        for (auto const& surface : surfaces) {
            auto thin = surface->getInput("thin_walled");
            if (!thin) thin = surface->addInput("thin_walled", "boolean");
            for (const char* attribute : {"nodename", "nodegraph", "output", "interfacename"})
                thin->removeAttribute(attribute);
            thin->setValue(true);
        }
        if (surfaces.empty()) {
            DiscardPrivateGraph(node, clone);
            std::cerr << "[MaterialX] WARNING: no surface constructor for thin-walled " << node->getNamePath()
                      << "; it renders as a closed surface\n";
            continue;
        }
        const std::string name = node->getName();
        PublishPrivateGraph(node, clone);
        for (auto const& material : doc->getMaterialNodes()) {
            auto front = material->getInput("surfaceshader");
            if (!front || front->getConnectedNode() != node) continue;
            auto back = material->getInput("backsurfaceshader");
            if (back && (back->hasNodeName() || back->hasNodeGraphString())) continue;
            if (!back) back = material->addInput("backsurfaceshader", "surfaceshader");
            back->copyContentFrom(front);
        }
        if (VerboseMdl())
            std::cout << "[MaterialX] thin-walled " << name << " as " << node->getCategory() << " ("
                      << surfaces.size() << " surface constructor)\n";
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
//
// prism_oren_nayar_diffuse_bsdf: oren_nayar_diffuse_bsdf with
//   energy_compensation, its multiple-scattering lobe tinted as the EON model
//   (OpenPBR; MaterialX's GLSL mx_oren_nayar_compensated_diffuse) has it:
//     rho_ms = rho^2 E_avg / (1 - rho (1 - E_avg)),
//     E_avg = A (1 + C2 r), A = 1 / (1 + C1 r),
//   C1 = 1/2 - 2/(3 pi), C2 = 2/3 - 28/(15 pi). MaterialX's MDL library
//   (pbrlib_1_10 mx_oren_nayar_diffuse_bsdf) passes multiscatter_tint
//   color(1.0), which libbsdf adds untinted (diffuse_sample/_evaluate:
//   tint * f_ss + multiscatter_tint * f_ms), so only white albedo was right.
//   libbsdf's lobe is the same fit (eval_oren_nayar: rho_avg = A + C2 B with
//   B = A r), and it saturates the tint and the roughness, so does rho_ms here.
const std::string kFresnelLayer = "prism_fresnel_layer";
const std::string kDielectricSplit = "prism_dielectric_split";
const std::string kOrenNayarCompensated = "prism_oren_nayar_diffuse_bsdf";

void EnsureHelperBsdf(const MaterialX::DocumentPtr& doc, const std::string& category)
{
    const std::string name = "ND_" + category;
    if (doc->getNodeDef(name)) return;
    auto nodedef = doc->addNodeDef(name, "BSDF", category);
    nodedef->setNodeGroup("pbr");
    std::string code;
    if (category == kOrenNayarCompensated) {
        nodedef->setInputValue("weight", 1.0f);
        nodedef->setInputValue("color", MaterialX::Color3(0.18f));
        nodedef->setInputValue("roughness", 0.0f);
        const std::string rho = "math::saturate({{color}})";
        const std::string r = "math::saturate({{roughness}})";
        const std::string average = "((1.0 + 0.07248831 * " + r + ") / (1.0 + 0.28779340 * " + r + "))";
        code = "material(surface: material_surface(scattering: df::weighted_layer(weight: {{weight}}, "
               "layer: df::diffuse_reflection_bsdf(tint: {{color}}, roughness: {{roughness}}, "
               "multiscatter_tint: " + rho + " * " + rho + " * " + average + " / (color(1.0) - " + rho +
               " * (1.0 - " + average + "))), normal: {{normal}})))";
    } else if (category == kFresnelLayer) {
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
                               const MaterialX::NodePtr& instance, bool split = false)
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
        if (!filmWeight || IsZero(ResolvedConstant(filmWeight, instance))) filmed = nullptr;
    } else if (auto film = top->getInput("thinfilm_thickness");
               film && !IsZero(ResolvedConstant(film, instance))) {
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

// The scatter_mode "T" dielectric_bsdf among a graph's nodes (but `before`).
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

// MaterialX 1.39.3's gltf_pbr graph writes its dielectric as a mix of two
// layers over one base (the plain and the iridescent reflection, "the base
// layer BSDF is duplicated" for the code generators of the time); 1.39.5's,
// which the coupling matches, layers the mix of the two reflections over
// the base. mix(layer(a, X), layer(b, X), m) becomes layer(mix(a, b, m), X)
// (the mix node becomes the layer, so its consumers are unchanged) where both
// layers feed only the mix. Graphs without the shape are unchanged.
void FactorSharedLayerBases(const MaterialX::GraphElementPtr& graph)
{
    for (auto const& mix : graph->getNodes()) {
        if (mix->getCategory() != "mix" || mix->getType() != "BSDF") continue;
        auto fg = Producer(mix, "fg"), bg = Producer(mix, "bg");
        if (!fg || !bg || fg == bg || fg->getCategory() != "layer" || bg->getCategory() != "layer") continue;
        auto base = Producer(fg, "base");
        if (!base || base != Producer(bg, "base") || SoleConsumer(fg).first != mix ||
            SoleConsumer(bg).first != mix)
            continue;
        auto tops = graph->addNode("mix", graph->createValidChildName(mix->getName() + "_tops"), "BSDF");
        tops->addInput("fg", "BSDF")->copyContentFrom(fg->getInput("top"));
        tops->addInput("bg", "BSDF")->copyContentFrom(bg->getInput("top"));
        if (auto weight = mix->getInput("mix")) tops->addInput("mix", "float")->copyContentFrom(weight);
        for (const char* input : {"fg", "bg", "mix"}) mix->removeInput(input);
        mix->setCategory("layer");
        if (mix->hasNodeDefString()) mix->setNodeDefString("ND_layer_bsdf");
        mix->addInput("top", "BSDF")->setNodeName(tops->getName());
        mix->addInput("base", "BSDF")->setNodeName(base->getName());
        graph->removeNode(fg->getName());
        graph->removeNode(bg->getName());
        if (VerboseMdl())
            std::cout << "[MaterialX] " << mix->getNamePath() << ": mix of layers over one base factored\n";
        return FactorSharedLayerBases(graph); // the node list changed
    }
}

// Couple (and replace the smooth coat of) a surface's private graph by its
// plan; false when the graph didn't have the expected shape. `instance` is
// the surface node (the values of the graph's interface inputs).
bool RepairExpandedDielectric(const MaterialX::GraphElementPtr& graph, const MaterialX::NodePtr& transmission,
                              const std::vector<MaterialX::NodePtr>& surfaces, const MaterialX::NodePtr& instance,
                              DielectricPlan& plan)
{
    if (!transmission || !CoupleTransmissionFresnel(graph, transmission, instance, plan.split)) return false;
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
    for (auto& [node, plan] : nodes) {
        const std::string name = node->getName();
        PrivateGraph clone = plan.couple ? ClonePrivateGraph(node) : PrivateGraph{};
        if (!clone) {
            ReportPartialRepair(name, plan);
            continue;
        }
        FactorSharedLayerBases(clone.graph);
        const bool coupled = RepairExpandedDielectric(clone.graph, AddedTransmission(clone.graph, {}),
                                                      ExpandedSurfaces(clone.graph, OutputPorts(clone.graph)), node,
                                                      plan);
        if (!coupled) {
            DiscardPrivateGraph(node, clone);
            std::cerr << "[MaterialX] WARNING: " << name << "'s graph has no layered transmission"
                      << " lobe; its glass may lose energy\n";
        } else {
            PruneExpansion(clone.graph, {});
            PublishPrivateGraph(node, clone);
        }
        if (coupled && VerboseMdl())
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
// the material's volume. gltf_pbr nodes with a finite attenuation distance
// get a private copy of their genmdl graph with that layer.
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
    for (auto const& node : nodes) {
        const std::string name = node->getName();
        DielectricPlan plan = PlanDielectric(node);
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) {
            std::cerr << "[MaterialX] WARNING: no graph for " << node->getNamePath()
                      << "; its volume attenuation is ignored\n";
            continue;
        }
        const MaterialX::NodeGraphPtr graph = clone.graph;
        if (plan.couple) FactorSharedLayerBases(graph);
        MaterialX::NodePtr transmission, volume;
        for (auto const& added : graph->getNodes()) {
            if (added->getCategory() == "anisotropic_vdf") volume = added;
            else if (added->getCategory() == "dielectric_bsdf") {
                auto mode = added->getInput("scatter_mode");
                if (mode && mode->getValueString() == "T") transmission = added;
            }
        }
        if (!transmission || !volume) {
            DiscardPrivateGraph(node, clone);
            std::cerr << "[MaterialX] WARNING: " << name << "'s graph has no transmission BSDF and"
                      << " volume; its volume attenuation is ignored\n";
            continue;
        }
        const std::vector<MaterialX::PortElementPtr> consumers = transmission->getDownstreamPorts();
        auto layer = graph->addNode("layer", graph->createValidChildName(transmission->getName() + "_volume"), "BSDF");
        layer->addInput("top", "BSDF")->setNodeName(transmission->getName());
        layer->addInput("base", "VDF")->setNodeName(volume->getName());
        for (auto const& port : consumers) port->setNodeName(layer->getName());
        if (VerboseMdl())
            std::cout << "[MaterialX] " << name << ": transmission layered over its volume ("
                      << consumers.size() << " consumers)\n";
        if (plan.couple &&
            !RepairExpandedDielectric(graph, transmission, ExpandedSurfaces(graph, OutputPorts(graph)), node, plan))
            std::cerr << "[MaterialX] WARNING: " << name << ": no layered transmission lobe; its glass may lose energy\n";
        PruneExpansion(graph, {});
        PublishPrivateGraph(node, clone);
        ReportPartialRepair(name, plan);
    }
}

// Finish a repair of a private graph: publish it when `repaired`, else
// return the node to the library's graph.
void FinishPrivateGraph(const MaterialX::NodePtr& node, PrivateGraph& clone, bool repaired)
{
    if (!repaired) {
        DiscardPrivateGraph(node, clone);
        return;
    }
    PruneExpansion(clone.graph, {});
    PublishPrivateGraph(node, clone);
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
        const std::set<std::string> before;
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) continue;
        const MaterialX::NodeGraphPtr graph = clone.graph;
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
        FinishPrivateGraph(node, clone, coupled);
        if (!coupled)
            std::cerr << "[MaterialX] WARNING: " << name << "'s graph has no layered transmission"
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
        const std::set<std::string> before;
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) continue;
        const MaterialX::NodeGraphPtr graph = clone.graph;
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
        FinishPrivateGraph(node, clone, coupled);
        if (!coupled)
            std::cerr << "[MaterialX] WARNING: " << name << "'s graph has no layered transmission"
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
        const std::set<std::string> before;
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) continue;
        const MaterialX::NodeGraphPtr graph = clone.graph;
        if (ior && LamaSchlickAsRt(graph, before, *ior)) {
            FinishPrivateGraph(node, clone, true);
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
        FinishPrivateGraph(node, clone, repaired);
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

bool IsCompensatedOrenNayar(const MaterialX::NodePtr& node)
{
    return node->getCategory() == "oren_nayar_diffuse_bsdf" && IsTrueConstant(node->getInput("energy_compensation"));
}

void UseCompensatedOrenNayar(const MaterialX::NodePtr& node)
{
    EnsureHelperBsdf(node->getDocument(), kOrenNayarCompensated);
    node->removeInput("energy_compensation");
    node->setCategory(kOrenNayarCompensated);
    node->setNodeDefString("ND_" + kOrenNayarCompensated);
}

// EON energy compensation (oren_nayar_diffuse_bsdf's energy_compensation)
// with its multiple-scattering lobe tinted by the albedo, as MaterialX's GLSL
// and OpenPBR define it; MaterialX's MDL library leaves that lobe white, so
// rough coloured diffuse surfaces rendered too bright (by up to 4x in a
// low-albedo channel at roughness 1). Compensated nodes become
// prism_oren_nayar_diffuse_bsdf (EnsureHelperBsdf): the document's own
// (top level or in authored graphs), and those in the genmdl graph of a node
// the document uses (open_pbr_surface's base, LamaDiffuse), through a private
// copy of that graph (ClonePrivateGraph). Runs after the other graph repairs,
// so it rewrites the graph they chose.
void CompensateOrenNayarEnergy(const MaterialX::DocumentPtr& doc)
{
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree()) {
        auto node = element->asA<MaterialX::Node>();
        if (!node) continue;
        // Nodes in implementation graphs (private copies) belong to the node
        // that uses the graph.
        auto parent = node->getParent() ? node->getParent()->asA<MaterialX::NodeGraph>() : nullptr;
        if (parent && parent->hasNodeDefString()) continue;
        nodes.push_back(node);
    }
    for (auto const& node : nodes) {
        if (IsCompensatedOrenNayar(node)) {
            UseCompensatedOrenNayar(node);
            if (VerboseMdl()) std::cout << "[MaterialX] tinted EON compensation: " << node->getNamePath() << "\n";
            continue;
        }
        // libbsdf adds the compensation lobe only to a rough lobe: an
        // open_pbr_surface with a constant zero base_diffuse_roughness (the
        // default) renders the same with the library's graph, and keeps it.
        if (LibraryCategory(node->getCategory()) == "open_pbr_surface" &&
            IsZero(ConstantOrDefault(node, "base_diffuse_roughness")))
            continue;
        auto implementation = node->getImplementation("genmdl");
        auto graph = implementation ? implementation->asA<MaterialX::NodeGraph>() : nullptr;
        if (!graph) continue;
        const auto inner = graph->getNodes();
        if (std::none_of(inner.begin(), inner.end(), IsCompensatedOrenNayar)) continue;
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) continue;
        int count = 0;
        for (auto const& lobe : clone.graph->getNodes())
            if (IsCompensatedOrenNayar(lobe)) {
                UseCompensatedOrenNayar(lobe);
                ++count;
            }
        const std::string name = node->getNamePath();
        PublishPrivateGraph(node, clone);
        if (VerboseMdl())
            std::cout << "[MaterialX] tinted EON compensation: " << name << " as " << node->getCategory() << " ("
                      << count << " lobe)\n";
    }
}

// A layer whose top is a mix of two BSDFs (both sides connected).
bool LayersOverMix(const MaterialX::NodeGraphPtr& graph)
{
    for (auto const& node : graph->getNodes()) {
        if (node->getCategory() != "layer") continue;
        auto top = Producer(node, "top");
        if (top && top->getCategory() == "mix" && top->getType() == "BSDF" && Producer(top, "fg") &&
            Producer(top, "bg"))
            return true;
    }
    return false;
}

// MaterialX's MDL generator layers a BSDF over a base by handing the base to
// the top lobe (ClosureLayerNodeMdl); a mix of two lobes as the top has no
// base input, so the generator emits the top alone and drops the base
// ("MDL has no support for layering BSDF nodes without a base input"). The
// generator of 1.39.5 distributes such layers itself; 1.39.3's does not, and
// its open_pbr_surface graph layers the dielectric reflection (a mix of the
// thin-film and plain lobes) over the whole substrate: diffuse, subsurface
// and transmission were lost, and OpenPBR surfaces only reflected. Nodes
// whose genmdl graph (the library's or a repair's private copy) has such a
// layer get a private copy with the layers distributed over the mixes
// (DistributeLayersOverMixes, as for the document's own layers). Runs after
// the other graph repairs, which match the undistributed graphs, and only
// when the caller found the generator dropping the base
// (setDistributeLibraryLayers).
//
// MaterialX's MDL library before 1.39.5 (pbrlib_1_9 mx_dielectric_bsdf)
// gives a reflection-only dielectric a Schlick curve with grazing
// reflectivity 1 - roughness whatever its IOR, so a lobe of IOR 1 still
// reflects at grazing angles and dims its base there; 1.39.5's curve
// reflects nothing at IOR 1. open_pbr_surface's specular_weight 0 is such a
// lobe (its IOR is modulated to 1): the Cornell box's walls kept a grey
// grazing highlight and lost 4% of their diffuse light. Reflection-only
// dielectric_bsdf nodes get weight 0 where their IOR is 1, which is what
// 1.39.5's curve computes, when the caller found that library
// (setZeroUnitIorReflections).
//
// Both rewrite the document's own nodes in place and the genmdl graph of a
// node through a private copy.
bool IsUnitIorReflection(const MaterialX::NodePtr& node)
{
    return node->getCategory() == "dielectric_bsdf" && ConstantOrDefault(node, "scatter_mode") == "R";
}

void ZeroUnitIorReflection(const MaterialX::GraphElementPtr& graph, const MaterialX::NodePtr& lobe)
{
    auto select = graph->addNode("ifequal", graph->createValidChildName(lobe->getName() + "_unit_ior_weight"),
                                 "float");
    auto ior = select->addInput("value1", "float");
    if (auto source = lobe->getInput("ior")) ior->copyContentFrom(source);
    else ior->setValueString(ConstantOrDefault(lobe, "ior"));
    select->setInputValue("value2", 1.0f);
    select->setInputValue("in1", 0.0f);
    auto weight = select->addInput("in2", "float");
    if (auto source = lobe->getInput("weight")) weight->copyContentFrom(source);
    else weight->setValue(1.0f);
    auto input = lobe->getInput("weight");
    if (!input) input = lobe->addInput("weight", "float");
    for (const char* attribute : {"value", "nodename", "nodegraph", "output", "interfacename"})
        input->removeAttribute(attribute);
    input->setNodeName(select->getName());
}

void RepairForOldMdlLibrary(const MaterialX::DocumentPtr& doc, bool distribute, bool unitIor)
{
    if (!distribute && !unitIor) return;
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree()) {
        auto node = element->asA<MaterialX::Node>();
        if (!node) continue;
        auto parent = node->getParent() ? node->getParent()->asA<MaterialX::NodeGraph>() : nullptr;
        if (parent && parent->hasNodeDefString()) continue;
        nodes.push_back(node);
    }
    for (auto const& node : nodes) {
        if (unitIor && IsUnitIorReflection(node)) {
            if (auto parent = node->getParent() ? node->getParent()->asA<MaterialX::GraphElement>() : nullptr)
                ZeroUnitIorReflection(parent, node);
            continue;
        }
        auto implementation = node->getImplementation("genmdl");
        auto graph = implementation ? implementation->asA<MaterialX::NodeGraph>() : nullptr;
        if (!graph) continue;
        const auto inner = graph->getNodes();
        const bool layers = distribute && LayersOverMix(graph);
        const bool reflections = unitIor && std::any_of(inner.begin(), inner.end(), IsUnitIorReflection);
        if (!layers && !reflections) continue;
        PrivateGraph clone = ClonePrivateGraph(node);
        if (!clone) continue;
        if (layers) DistributeLayersOverMixes(clone.graph);
        if (reflections)
            for (auto const& lobe : clone.graph->getNodes())
                if (IsUnitIorReflection(lobe)) ZeroUnitIorReflection(clone.graph, lobe);
        const std::string name = node->getNamePath();
        PublishPrivateGraph(node, clone);
        if (VerboseMdl())
            std::cout << "[MaterialX] " << name << ": " << (layers ? "layers distributed over mixes" : "")
                      << (layers && reflections ? ", " : "") << (reflections ? "no reflection at IOR 1" : "")
                      << " (an older MaterialX MDL library), as " << node->getCategory() << "\n";
    }
}

// The diffuse albedo a material's surface clearly has: a constant base
// colour times its weight, on a surface without metal, transmission,
// subsurface, coat or cutout (whose reflection is then at least that colour
// less its specular reflection). Empty when the surface is anything else or
// the albedo is under 0.05.
std::optional<std::array<float, 3>> ClearBaseAlbedo(const MaterialX::NodePtr& material)
{
    auto input = material->getInput("surfaceshader");
    auto surface = input ? input->getConnectedNode() : nullptr;
    if (!surface) return std::nullopt;
    auto value = [&](const char* name) { return ConstantOrDefault(surface, name); };
    auto zero = [&](std::initializer_list<const char*> names) {
        for (const char* name : names)
            if (!IsZero(value(name))) return false;
        return true;
    };
    std::string color, weight = "1";
    const std::string& category = surface->getCategory();
    if (category == "open_pbr_surface") {
        if (!zero({"base_metalness", "transmission_weight", "subsurface_weight", "coat_weight", "fuzz_weight"}) ||
            !IsOne(value("geometry_opacity")))
            return std::nullopt;
        color = value("base_color");
        weight = value("base_weight");
    } else if (category == "standard_surface") {
        if (!zero({"metalness", "transmission", "subsurface", "coat", "sheen"}) || !IsWhite(value("opacity")))
            return std::nullopt;
        color = value("base_color");
        weight = value("base");
    } else if (category == "gltf_pbr") {
        if (!zero({"metallic", "transmission", "clearcoat"}) || !IsBlack(value("sheen_color")) || !IsOne(value("alpha")))
            return std::nullopt;
        color = value("base_color");
    } else if (category == "UsdPreviewSurface") {
        if (!zero({"metallic", "useSpecularWorkflow", "clearcoat"}) || !IsOne(value("opacity"))) return std::nullopt;
        color = value("diffuseColor");
    } else {
        return std::nullopt;
    }
    try {
        const auto c = MaterialX::fromValueString<MaterialX::Color3>(color);
        const float w = std::stof(weight);
        std::array<float, 3> albedo{c[0] * w, c[1] * w, c[2] * w};
        if (std::max({albedo[0], albedo[1], albedo[2]}) < 0.05f) return std::nullopt;
        return albedo;
    } catch (...) {
        return std::nullopt;
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

// MaterialX 1.39.5's shader generator converts the colour output of a node
// whose filename inputs carry a colour space (the node is converted as a
// whole, not image by image), and it does so once per such input
// (ShaderGraph::applyInputTransforms). A node with several, such as
// triplanarprojection (filex, filey, filez), gets several transform nodes
// with one name; adding the second frees the first while the output still
// lists its input, and ShaderGraph::finalize crashes (use after free in
// ShaderInput::breakConnection). Upstream MaterialX keeps the first request
// for an output since then. Do the same: the first filename input keeps its
// colour space, the others are tagged "none", which the generator skips.
void KeepOneOutputColorTransform(const MaterialX::DocumentPtr& doc)
{
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>()) nodes.push_back(node);
    for (auto const& node : nodes) {
        const std::string& type = node->getType();
        if (type != "color3" && type != "color4" && type != MaterialX::MULTI_OUTPUT_TYPE_STRING) continue;
        std::vector<MaterialX::InputPtr> tagged;
        for (auto const& input : node->getInputs()) {
            if (input->getType() != MaterialX::FILENAME_TYPE_STRING) continue;
            if (!input->hasValueString() && !input->hasInterfaceName()) continue;
            const std::string space = input->getActiveColorSpace();
            if (!space.empty() && space != "none") tagged.push_back(input);
        }
        if (tagged.size() < 2) continue;
        const std::string kept = tagged.front()->getActiveColorSpace();
        for (size_t i = 1; i < tagged.size(); ++i) {
            const std::string space = tagged[i]->getActiveColorSpace();
            if (space != kept)
                std::cerr << "[MaterialX] WARNING: " << node->getNamePath() << " converts its output from '" << kept
                          << "' (its " << tagged.front()->getName() << "), not from " << tagged[i]->getName()
                          << "'s '" << space << "'\n";
            tagged[i]->setColorSpace("none");
        }
        if (VerboseMdl())
            std::cout << "[MaterialX] " << node->getNamePath() << ": one colour transform from '" << kept
                      << "' for its " << tagged.size() << " filename inputs\n";
    }
}

// A constant colour on a uniform input (by the node's definition) that the
// generator would transform into the working space: the generated transform
// is varying and cannot feed the uniform MDL parameter. Convert the value
// here and tag it with the working space instead. The generator transforms
// lin_rec709 and the spaces of the library's <space>_to_lin_rec709 nodes
// (ColorTransformLibrary in mdl_generator.cpp); others it leaves as they are,
// and so does this.
void ConvertUniformColors(const MaterialX::DocumentPtr& doc,
                          const DocumentLoader::UniformColorConversion& conversion)
{
    if (!conversion.convert || conversion.target.empty()) return;
    std::vector<MaterialX::NodePtr> nodes;
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>()) nodes.push_back(node);
    for (auto const& node : nodes) {
        const MaterialX::NodeDefPtr definition = node->getNodeDef();
        if (!definition) continue;
        for (auto const& input : node->getInputs()) {
            const std::string& type = input->getType();
            if ((type != "color3" && type != "color4") || IsBound(input) || !input->getValue()) continue;
            const MaterialX::InputPtr declaration = definition->getActiveInput(input->getName());
            if (!declaration || !declaration->getIsUniform()) continue;
            const std::string space = input->getActiveColorSpace();
            if (space.empty() || space == "none" || space == conversion.target) continue;
            if (space != "lin_rec709" &&
                doc->getMatchingNodeDefs(space + "_to_lin_rec709").empty())
                continue;
            std::array<float, 3> rgb{};
            float alpha = 1.0f;
            if (type == "color3") {
                const auto value = input->getValue()->asA<MaterialX::Color3>();
                rgb = {value[0], value[1], value[2]};
            } else {
                const auto value = input->getValue()->asA<MaterialX::Color4>();
                rgb = {value[0], value[1], value[2]};
                alpha = value[3];
            }
            const std::array<float, 3> authored = rgb;
            if (!conversion.convert(space, rgb)) {
                std::cerr << "[MaterialX] WARNING: cannot convert " << input->getNamePath() << " from '" << space
                          << "' to the working space; it is a uniform input, so its colour transform will "
                             "not compile\n";
                continue;
            }
            if (type == "color3") input->setValue(MaterialX::Color3(rgb[0], rgb[1], rgb[2]));
            else input->setValue(MaterialX::Color4(rgb[0], rgb[1], rgb[2], alpha));
            input->setColorSpace(conversion.target);
            if (VerboseMdl())
                std::cout << std::format("[MaterialX] uniform colour {} converted from {} to {}: "
                                         "({:.9g}, {:.9g}, {:.9g}) -> ({:.9g}, {:.9g}, {:.9g})\n",
                                         input->getNamePath(), space, conversion.target, authored[0],
                                         authored[1], authored[2], rgb[0], rgb[1], rgb[2]);
        }
    }
}
}

DocumentLoader::DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp,
                               bool fallback_sss_to_diffuse)
    : m_stdlib(stdlib), m_clamp(clamp),
      m_fallback_sss_to_diffuse(fallback_sss_to_diffuse)
{}

std::string DocumentLoader::materialXVersionWarning(int major, int minor, int patch)
{
    using Version = std::tuple<int, int, int>;
    const Version version{major, minor, patch};
    const Version lo{kTestedMaterialXMin[0], kTestedMaterialXMin[1], kTestedMaterialXMin[2]};
    const Version hi{kTestedMaterialXMax[0], kTestedMaterialXMax[1], kTestedMaterialXMax[2]};
    if (version >= lo && version <= hi) return {};
    auto text = [](Version const& v) { return std::format("{}.{}.{}", std::get<0>(v), std::get<1>(v), std::get<2>(v)); };
    std::string range = text(lo);
    if (hi != lo) range += " to " + text(hi);
    return std::format("[MaterialX] WARNING: MaterialX {} is outside the tested range ({}); the compatibility "
                       "repairs target that version's graphs and may not apply or may apply wrongly\n",
                       text(version), range);
}

bool DocumentLoader::warnOnceIfUntestedMaterialX()
{
    static std::once_flag once;
    bool printed = false;
    std::call_once(once, [&] {
        const auto [major, minor, patch] = MaterialX::getVersionIntegers();
        const std::string warning = materialXVersionWarning(major, minor, patch);
        if (!warning.empty()) {
            std::cerr << warning;
            printed = true;
        }
    });
    return printed;
}

MaterialX::DocumentPtr DocumentLoader::load(const std::string& filepath)
{
    warnOnceIfUntestedMaterialX();
    VerboseScope verbose(m_verbose);
    tRepairs = {};
    m_repairs = {};
    m_baseAlbedos.clear();
    MaterialX::DocumentPtr doc = MaterialX::createDocument();

    try {
        MaterialX::readFromXmlFile(doc, filepath);

        // Renderer compatibility preprocessing, alongside the color workaround
        // below. Run before importing stdlib so library implementations are not
        // rewritten. Only this in-memory document changes, never the asset file.
        NormalizeColorSpaces(doc);
        KeepOneOutputColorTransform(doc);
        ReplaceMatrixPrimvarReaders(doc, m_stdlib);
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
                            ++tRepairs.clamps;
                            if (VerboseMdl())
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
                    ++tRepairs.clamps;
                    if (VerboseMdl())
                        std::cout << "[MaterialX] Inserted clamp node on "
                                  << node->getNamePath() << "." << name << "\n";
                }
            }
        }

        // Attach after preprocessing so shared standard-library graphs retain
        // their original behavior. Validate the final document used for generation.
        AttachLibrary(doc, m_stdlib);
        // Before the repairs rename the surfaces' categories.
        for (auto const& material : doc->getMaterialNodes())
            if (auto albedo = ClearBaseAlbedo(material)) m_baseAlbedos.emplace_back(material->getName(), *albedo);
        // These need the library (declared defaults, graph implementations).
        ConvertUniformColors(doc, m_uniformColors);
        RouteUncoatedTransmissionIor(doc);
        ExpandThinWalledOpenPbr(doc);
        LayerGltfPbrVolume(doc);
        CoupleSurfaceTransmissionFresnel(doc);
        CoupleUsdPreviewTransmission(doc);
        CoupleLamaDielectric(doc);
        RepairLamaGeneralizedSchlick(doc);
        CompensateOrenNayarEnergy(doc);
        RepairForOldMdlLibrary(doc, m_distributeLibraryLayers, m_zeroUnitIorReflections);
        std::string validation_errors;
        if (!doc->validate(&validation_errors)) {
            std::cerr << "[WARN] Document validation issues: " << validation_errors << "\n";
        }
        m_repairs = tRepairs;
        if (m_repairs.connectionTypes || m_repairs.clamps) {
            std::string summary;
            if (m_repairs.connectionTypes)
                summary = std::to_string(m_repairs.connectionTypes) + " type repair" +
                    (m_repairs.connectionTypes == 1 ? "" : "s");
            if (m_repairs.clamps)
                summary += (summary.empty() ? "" : ", ") + std::to_string(m_repairs.clamps) + " clamp" +
                    (m_repairs.clamps == 1 ? "" : "s");
            std::cout << "[MaterialX] " << (m_label.empty() ? filepath : m_label) << ": " << summary
                      << (m_verbose ? "\n" : " (details: PRISM_MDL_VERBOSE=1)\n");
        }
        return doc;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load '" << filepath << "': " << e.what() << "\n";
        return nullptr;
    }
}

std::optional<std::array<float, 3>> DocumentLoader::baseAlbedo(const std::string& material) const
{
    for (auto const& [name, albedo] : m_baseAlbedos)
        if (name == material) return albedo;
    return std::nullopt;
}

MaterialX::DocumentPtr DocumentLoader::create_empty()
{
    MaterialX::DocumentPtr doc = MaterialX::createDocument();
    AttachLibrary(doc, m_stdlib);
    return doc;
}
