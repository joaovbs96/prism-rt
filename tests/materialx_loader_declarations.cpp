// DocumentLoader repairs documents whose nodes do not match MaterialX's
// definitions, so MaterialX finds a definition for every node (the Typhoon
// suite's materials that rendered as the magenta error material: "Could not
// find a nodedef for node"), against MaterialX's own library (skipped
// without it):
// - a node of a category with several versions (standard_surface 1.0.0 and
//   1.0.1) is typed by its default version, so a vector3 graph output on
//   emission_color gets its convert node;
// - an integer input written as one of its enum names (UsdPreviewSurface's
//   opacityMode "transparent" / "presence", a token in USD) gets its value;
// - open_pbr_surface's normal written as standard_surface's `normal` is read
//   as geometry_normal; other undeclared inputs are dropped;
// - hdMtlx's ND_UsdPrimvarReader_matrix (MaterialX cannot read matrix
//   primvars) becomes a constant of its fallback, the identity.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXFormat/XmlIo.h>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    { std::ofstream out(path); out << "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n" << body << "</materialx>\n"; }
    DocumentLoader loader(gLibrary, false, false);
    auto doc = loader.load(path.string());
    std::filesystem::remove(path);
    require(bool(doc), name + ": load failed");
    return doc;
}

void RequireDefinitions(const MaterialX::DocumentPtr& doc, const std::string& name)
{
    for (auto element : doc->traverseTree())
        if (auto node = element->asA<MaterialX::Node>())
            require(bool(node->getNodeDef()), name + ": no definition for " + node->getNamePath());
}

// hdMtlx's document for a standard_surface whose emission_color reads a
// vector3 (dpdu_dpdv_debug, npr_nodes_sphere_gallery, subdiv_facevarying_test).
void Versions()
{
    auto doc = Load("versions", R"(
  <standard_surface name="Surface" type="surfaceshader">
    <input name="emission_color" type="vector3" output="emission_color_out" nodegraph="_" />
    <input name="emission" type="float" value="1" />
  </standard_surface>
  <surfacematerial name="M" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Surface" />
  </surfacematerial>
  <nodegraph name="_">
    <tangent name="T" type="vector3" nodedef="ND_tangent_vector3" />
    <output name="emission_color_out" type="vector3" nodename="T" />
  </nodegraph>
)");
    auto input = doc->getNode("Surface")->getInput("emission_color");
    require(input->getType() == "color3", "versions: emission_color keeps its declared color3");
    auto output = doc->getNodeGraph("_")->getOutput("emission_color_out");
    require(output->getType() == "color3", "versions: the graph output is color3");
    auto adapter = doc->getNodeGraph("_")->getNode(output->getNodeName());
    require(adapter && adapter->getCategory() == "convert", "versions: a convert node adapts the vector3");
    require(doc->getNode("Surface")->getNodeDef()->getName() == "ND_standard_surface_surfaceshader",
            "versions: the default version");
    RequireDefinitions(doc, "versions");
}

// transparency_material_gallery, usd_native_nodes_gallery.
void EnumNames()
{
    for (auto const& [mode, value] : {std::pair{"transparent", 0}, std::pair{"presence", 1}}) {
        auto doc = Load("enum", std::string(R"(
  <UsdPreviewSurface name="Surface" type="surfaceshader">
    <input name="opacity" type="float" value="0.5" />
    <input name="opacityMode" type="string" value=")") + mode + R"(" />
  </UsdPreviewSurface>
  <surfacematerial name="M" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Surface" />
  </surfacematerial>
)");
        auto input = doc->getNode("Surface")->getInput("opacityMode");
        require(input->getType() == "integer", "enum: opacityMode is an integer");
        require(input->getValueString() == std::to_string(value), std::string("enum: ") + mode);
        RequireDefinitions(doc, "enum");
    }
}

// openPbr_bump_fractal.
void UndeclaredInputs()
{
    auto doc = Load("undeclared", R"(
  <open_pbr_surface name="Surface" type="surfaceshader">
    <input name="base_color" type="color3" value="0.5, 0.5, 0.5" />
    <input name="normal" type="vector3" output="normal_out" nodegraph="_" />
    <input name="not_an_input" type="float" value="1" />
  </open_pbr_surface>
  <surfacematerial name="M" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Surface" />
  </surfacematerial>
  <nodegraph name="_">
    <normal name="N" type="vector3" nodedef="ND_normal_vector3" />
    <output name="normal_out" type="vector3" nodename="N" />
  </nodegraph>
)");
    auto surface = doc->getNode("Surface");
    require(!surface->getInput("normal") && surface->getInput("geometry_normal") &&
                surface->getInput("geometry_normal")->getOutputString() == "normal_out",
            "undeclared: normal is read as geometry_normal");
    require(!surface->getInput("not_an_input"), "undeclared: an unknown input is dropped");
    require(bool(surface->getInput("base_color")), "undeclared: declared inputs stay");
    RequireDefinitions(doc, "undeclared");
}

// usd_native_nodes_gallery's MatrixMat, as hdMtlx writes it.
void PrimvarReaderAlias()
{
    auto doc = Load("matrix", R"(
  <UsdPreviewSurface name="Surface" type="surfaceshader">
    <input name="diffuseColor" type="color3" output="diffuseColor_out" nodegraph="_" />
  </UsdPreviewSurface>
  <surfacematerial name="M" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="Surface" />
  </surfacematerial>
  <nodegraph name="_">
    <convert name="ToColor" type="color3" nodedef="ND_convert_vector3_color3">
      <input name="in" type="vector3" nodename="ApplyMatrix" />
    </convert>
    <transformmatrix name="ApplyMatrix" type="vector3" nodedef="ND_transformmatrix_vector3M4">
      <input name="in" type="vector3" value="0.8, 0.45, 0.25" />
      <input name="mat" type="surfaceshader" nodename="ReadTintMatrix" />
    </transformmatrix>
    <surface name="ReadTintMatrix" type="surfaceshader" nodedef="ND_UsdPrimvarReader_matrix">
      <input name="varname" type="string" value="tintMatrix" />
    </surface>
    <output name="diffuseColor_out" type="color3" nodename="ToColor" />
  </nodegraph>
)");
    auto graph = doc->getNodeGraph("_");
    auto reader = graph->getNode("ReadTintMatrix");
    require(reader->getCategory() == "constant" && reader->getType() == "matrix44" &&
                reader->getNodeDefString() == "ND_constant_matrix44" && !reader->getInput("varname") &&
                reader->getInput("value")->getValueString() == "1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1",
            "matrix: the reader is the identity, its fallback");
    require(graph->getNode("ApplyMatrix")->getInput("mat")->getType() == "matrix44", "matrix: mat is a matrix44");
    RequireDefinitions(doc, "matrix");
}

// Documents that already match are not changed.
void Unchanged()
{
    auto doc = Load("unchanged", R"(
  <standard_surface name="Surface" type="surfaceshader" version="1.0.0">
    <input name="base_color" type="color3" value="0.5, 0.5, 0.5" />
  </standard_surface>
  <UsdPreviewSurface name="Preview" type="surfaceshader">
    <input name="opacityMode" type="integer" value="1" />
  </UsdPreviewSurface>
)");
    require(doc->getNode("Surface")->getNodeDef()->getName() == "ND_standard_surface_surfaceshader_100",
            "unchanged: an explicit version keeps its definition");
    require(doc->getNode("Preview")->getInput("opacityMode")->getValueString() == "1", "unchanged: integer mode");
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
        if (!std::filesystem::exists(root / "libraries" / "bxdf" / "usd_preview_surface.mtlx")) {
            std::cout << "no MaterialX libraries at " << root.string() << "\n";
            return 77;
        }
        gLibrary = MaterialX::createDocument();
        MaterialX::loadLibraries({"libraries"}, MaterialX::FileSearchPath(root.string()), gLibrary);
        Versions();
        EnumNames();
        UndeclaredInputs();
        PrimvarReaderAlias();
        Unchanged();
        std::cout << "materialx_loader_declarations passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
#endif
}
