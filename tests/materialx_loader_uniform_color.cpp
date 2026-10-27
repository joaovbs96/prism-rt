// DocumentLoader converts constant colours on uniform inputs (gltf_pbr's
// attenuation_color, UsdUVTexture's scale and bias) that carry a colour space
// into the working space and tags them with it: the transform the generator
// would emit is varying and cannot feed a uniform MDL parameter (C219). Uses
// MaterialX's own library definitions (which inputs are uniform); skipped
// without them.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

MaterialX::DocumentPtr gLibrary;
std::vector<std::string> gCalls;

// A stand-in for the working space: scales by (2, 3, 4), so a converted
// value is recognisable; "fails" cannot be converted.
DocumentLoader::UniformColorConversion Conversion()
{
    return {"acescg", [](const std::string& space, std::array<float, 3>& rgb) {
                gCalls.push_back(space);
                if (space == "g22_rec709") return false;
                rgb = {rgb[0] * 2, rgb[1] * 3, rgb[2] * 4};
                return true;
            }};
}

MaterialX::DocumentPtr Load(const std::string& name, const std::string& body, bool convert = true,
                            const std::string& rootAttributes = "")
{
    const auto path = std::filesystem::current_path() / (name + ".mtlx");
    const std::string xml = "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\"" + rootAttributes + ">\n" +
                            body + "</materialx>\n";
    { std::ofstream out(path); out << xml; }
    DocumentLoader loader(gLibrary, false, false);
    if (convert) loader.setUniformColorConversion(Conversion());
    gCalls.clear();
    auto doc = loader.load(path.string());
    require(bool(doc), name + ": load failed");
    std::ifstream in(path);
    std::stringstream source;
    source << in.rdbuf();
    require(source.str() == xml, name + ": source file changed");
    in.close();
    std::filesystem::remove(path);
    return doc;
}

std::string Gltf(const std::string& inputs)
{
    return "  <gltf_pbr name=\"S\" type=\"surfaceshader\">\n" + inputs +
           "    <input name=\"transmission\" type=\"float\" value=\"1\" />\n"
           "  </gltf_pbr>\n"
           "  <surfacematerial name=\"M\" type=\"material\">\n"
           "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"S\" />\n"
           "  </surfacematerial>\n";
}

MaterialX::InputPtr Input(const MaterialX::DocumentPtr& doc, const std::string& node, const std::string& name)
{
    auto found = doc->getNode(node);
    require(bool(found), "no node " + node);
    auto input = found->getInput(name);
    require(bool(input), "no input " + node + "." + name);
    return input;
}

void Expect(const MaterialX::InputPtr& input, const std::string& value, const std::string& space,
            const std::string& what)
{
    require(input->getValueString() == value,
            what + ": value " + input->getValueString() + ", expected " + value);
    require(input->getColorSpace() == space,
            what + ": colour space '" + input->getColorSpace() + "', expected '" + space + "'");
}

void Uniform()
{
    // The repro: gltf_pbr's uniform attenuation_color tagged lin_rec709.
    auto doc = Load("uniform_tagged", Gltf(
        "    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\" colorspace=\"lin_rec709\" />\n"
        "    <input name=\"attenuation_distance\" type=\"float\" value=\"0.5\" />\n"
        "    <input name=\"base_color\" type=\"color3\" value=\"0.5, 0.5, 0.5\" colorspace=\"lin_rec709\" />\n"));
    Expect(Input(doc, "S", "attenuation_color"), "1, 0.75, 0.5", "acescg", "tagged attenuation_color");
    // A varying input keeps its tag: the generator transforms it.
    Expect(Input(doc, "S", "base_color"), "0.5, 0.5, 0.5", "lin_rec709", "varying base_color");
    require(gCalls == std::vector<std::string>{"lin_rec709"}, "uniform_tagged: one conversion expected");
    std::string errors;
    require(doc->validate(&errors), "uniform_tagged: invalid after conversion: " + errors);

    // The document's colour space applies to untagged inputs.
    doc = Load("uniform_document_space", Gltf(
        "    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\" />\n"),
        true, " colorspace=\"srgb_texture\"");
    Expect(Input(doc, "S", "attenuation_color"), "1, 0.75, 0.5", "acescg", "inherited srgb_texture");
    require(gCalls == std::vector<std::string>{"srgb_texture"}, "uniform_document_space: srgb_texture expected");

    // Colour-space aliases are canonical (NormalizeColorSpaces) before this.
    doc = Load("uniform_alias", Gltf(
        "    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\" colorspace=\"lin_srgb\" />\n"));
    Expect(Input(doc, "S", "attenuation_color"), "1, 0.75, 0.5", "acescg", "lin_srgb alias");

    // UsdUVTexture's uniform color4 scale: RGB converted, alpha kept.
    doc = Load("uniform_color4",
        "  <UsdUVTexture name=\"T\" type=\"multioutput\">\n"
        "    <input name=\"file\" type=\"filename\" value=\"missing.png\" />\n"
        "    <input name=\"scale\" type=\"color4\" value=\"0.5, 0.25, 0.125, 0.5\" colorspace=\"lin_rec709\" />\n"
        "  </UsdUVTexture>\n");
    Expect(Input(doc, "T", "scale"), "1, 0.75, 0.5, 0.5", "acescg", "color4 scale");
}

// The material service keeps tagged uniform colours out of its templates by
// name (IsCompatibleTemplateValue in src/materialx/prepared_template.h): a
// library with other uniform color3 inputs needs them listed there too.
void LibraryUniformColors()
{
    std::vector<std::string> found;
    for (auto const& definition : gLibrary->getNodeDefs())
        for (auto const& input : definition->getActiveInputs())
            if (input->getType() == "color3" && input->getIsUniform())
                found.push_back(definition->getNodeString() + "." + input->getName());
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    std::string list;
    for (auto const& name : found) list += " " + name;
    require(found == std::vector<std::string>{"gltf_pbr.attenuation_color"},
            "uniform color3 inputs of the library:" + list +
                "; update IsCompatibleTemplateValue in src/materialx/prepared_template.h");
}

void Untouched()
{
    // Already in the working space, data, or a space the generator has no
    // transform for: no conversion, the tag stays.
    for (auto const& [space, name] : std::vector<std::pair<std::string, std::string>>{
             {"acescg", "uniform_target"}, {"none", "uniform_none"}, {"unknown_space", "uniform_unknown"}}) {
        auto doc = Load(name, Gltf("    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\""
                                   " colorspace=\"" + space + "\" />\n"));
        Expect(Input(doc, "S", "attenuation_color"), "0.5, 0.25, 0.125", space, name);
        require(gCalls.empty(), name + ": no conversion expected");
    }
    // Untagged, without a document colour space.
    auto doc = Load("uniform_untagged", Gltf(
        "    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\" />\n"));
    Expect(Input(doc, "S", "attenuation_color"), "0.5, 0.25, 0.125", "", "untagged");
    require(gCalls.empty(), "uniform_untagged: no conversion expected");
    // A space the conversion cannot handle: left as authored.
    doc = Load("uniform_failed", Gltf(
        "    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\" colorspace=\"g22_rec709\" />\n"));
    Expect(Input(doc, "S", "attenuation_color"), "0.5, 0.25, 0.125", "g22_rec709", "failed conversion");
    require(gCalls == std::vector<std::string>{"g22_rec709"}, "uniform_failed: one attempt expected");
    // Without a conversion (the loader's default) nothing changes.
    doc = Load("uniform_no_conversion", Gltf(
        "    <input name=\"attenuation_color\" type=\"color3\" value=\"0.5, 0.25, 0.125\" colorspace=\"lin_rec709\" />\n"),
        false);
    Expect(Input(doc, "S", "attenuation_color"), "0.5, 0.25, 0.125", "lin_rec709", "no conversion");
}
}

int main()
{
#ifndef PRISM_TEST_MATERIALX_ROOT
    std::cout << "no MaterialX libraries\n";
    return 77;
#else
    try {
        const std::filesystem::path root = PRISM_TEST_MATERIALX_ROOT;
        if (!std::filesystem::exists(root / "libraries" / "bxdf" / "gltf_pbr.mtlx")) {
            std::cout << "no MaterialX libraries at " << root.string() << "\n";
            return 77;
        }
        gLibrary = MaterialX::createDocument();
        MaterialX::loadLibraries({"libraries"}, MaterialX::FileSearchPath(root.string()), gLibrary);
        Uniform();
        Untouched();
        LibraryUniformColors();
    } catch (std::exception const& e) {
        std::cerr << "FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "materialx loader uniform colour ok\n";
    return 0;
#endif
}
