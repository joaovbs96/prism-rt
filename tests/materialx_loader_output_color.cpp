// DocumentLoader keeps one output colour transform per node: MaterialX
// 1.39.5's generator converts the colour output of a node whose filename
// inputs carry a colour space once per such input, and a node with several
// (triplanarprojection's filex, filey and filez) crashed generation (use after
// free in ShaderGraph::finalize). The first tagged filename input keeps its
// space, the others are tagged "none", as upstream MaterialX now does.

#include "../src/materialx/loader.h"
#include <MaterialXCore/Node.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Space(MaterialX::NodePtr const& node, const char* input)
{
    require(bool(node), "node missing");
    auto port = node->getInput(input);
    require(bool(port), std::string("input missing: ") + input);
    return port->getColorSpace();
}
}

int main()
{
    try {
        const auto path = std::filesystem::current_path() / "output_color_fixture.mtlx";
        const std::string xml = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="graph">
    <triplanarprojection name="tri" type="color3">
      <input name="filex" type="filename" value="x.png" colorspace="srgb_rec709_scene" />
      <input name="filey" type="filename" value="y.png" colorspace="srgb_rec709_scene" />
      <input name="filez" type="filename" value="z.png" colorspace="srgb_rec709_scene" />
    </triplanarprojection>
    <triplanarprojection name="mixed" type="color3">
      <input name="filex" type="filename" value="x.png" colorspace="lin_rec709" />
      <input name="filey" type="filename" value="y.png" colorspace="srgb_texture" />
      <input name="filez" type="filename" value="z.png" />
    </triplanarprojection>
    <triplanarprojection name="one" type="color3">
      <input name="filex" type="filename" value="x.png" colorspace="srgb_texture" />
      <input name="filey" type="filename" value="y.png" />
    </triplanarprojection>
    <triplanarprojection name="vector" type="vector3">
      <input name="filex" type="filename" value="x.png" colorspace="srgb_texture" />
      <input name="filey" type="filename" value="y.png" colorspace="srgb_texture" />
    </triplanarprojection>
    <image name="plain" type="color3">
      <input name="file" type="filename" value="a.png" colorspace="srgb_texture" />
    </image>
    <output name="out" type="color3" nodename="tri" />
  </nodegraph>
  <nodegraph name="inherited" colorspace="srgb_texture">
    <triplanarprojection name="tri" type="color3">
      <input name="filex" type="filename" value="x.png" />
      <input name="filey" type="filename" value="y.png" />
    </triplanarprojection>
  </nodegraph>
</materialx>
)";
        { std::ofstream out(path); out << xml; }
        DocumentLoader loader(MaterialX::createDocument(), false, false);
        auto doc = loader.load(path.string());
        require(bool(doc), "load failed");
        {
            std::ifstream in(path);
            std::stringstream source;
            source << in.rdbuf();
            require(source.str() == xml, "the source file changed");
        }
        std::filesystem::remove(path);

        auto graph = doc->getNodeGraph("graph");
        auto tri = graph->getNode("tri");
        require(Space(tri, "filex") == "srgb_texture", "the first file keeps its (canonical) space");
        require(Space(tri, "filey") == "none" && Space(tri, "filez") == "none",
                "the other files of a colour node are not converted again");
        auto mixed = graph->getNode("mixed");
        require(Space(mixed, "filex") == "lin_rec709", "differing spaces: the first is kept");
        require(Space(mixed, "filey") == "none", "differing spaces: the second is dropped");
        require(Space(mixed, "filez").empty(), "an untagged file stays untagged");
        auto one = graph->getNode("one");
        require(Space(one, "filex") == "srgb_texture" && Space(one, "filey").empty(),
                "a node with one tagged file is unchanged");
        auto vector = graph->getNode("vector");
        require(Space(vector, "filex") == "srgb_texture" && Space(vector, "filey") == "srgb_texture",
                "a node without a colour output is unchanged");
        require(Space(graph->getNode("plain"), "file") == "srgb_texture", "an image is unchanged");
        auto inherited = doc->getNodeGraph("inherited")->getNode("tri");
        require(inherited->getInput("filex")->getActiveColorSpace() == "srgb_texture" &&
                    Space(inherited, "filey") == "none",
                "a space inherited from the graph counts too");
        std::cout << "materialx_loader_output_color passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
