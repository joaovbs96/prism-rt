#include "../src/materialx/loader.h"

#include <MaterialXCore/Node.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void write(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream out(path);
    out << contents;
}

int run()
{
    auto library = MaterialX::createDocument();
    auto definition = library->addNodeDef("ND_prism_test", "color3", "prism_test");
    definition->addInput("value", "color3")
        ->setValue(MaterialX::Color3(0.1f, 0.2f, 0.3f));
    DocumentLoader loader(library, false, false);

    const auto path = std::filesystem::current_path() / "library_fixture.mtlx";
    write(path, R"(<materialx version="1.38">
      <prism_test name="test" type="color3" nodedef="ND_prism_test" />
    </materialx>)");
    auto doc = loader.load(path.string());
    require(bool(doc), "data-library fixture failed to load");
    require(doc->hasDataLibrary(), "standard library was copied into the document");
    require(!doc->getChild("ND_prism_test"), "definition unexpectedly became local content");
    require(doc->getNodeDef("ND_prism_test") == definition,
            "definition did not resolve through the data library");
    require(doc->getNode("test")->getNodeDef() == definition,
            "node did not resolve its data-library definition");

    // importLibrary historically lets authored top-level definitions override
    // same-named library definitions.  A collision must retain that behavior.
    write(path, R"(<materialx version="1.38">
      <nodedef name="ND_prism_test" node="prism_test" type="color3">
        <input name="value" type="color3" value="0.8, 0.7, 0.6" />
      </nodedef>
      <prism_test name="test" type="color3" nodedef="ND_prism_test" />
    </materialx>)");
    auto overridden = loader.load(path.string());
    require(bool(overridden), "override fixture failed to load");
    require(!overridden->hasDataLibrary(), "collision did not use compatibility import");
    auto localDefinition = overridden->getNodeDef("ND_prism_test");
    require(localDefinition && localDefinition != definition,
            "authored definition did not override the library");
    require(localDefinition->getInput("value")->getValueString() == "0.8, 0.7, 0.6",
            "authored definition value changed");
    require(overridden->getNode("test")->getNodeDef() == localDefinition,
            "node did not resolve its authored definition");

    std::cout << "Data-library regression checks passed\n";
    return 0;
}

int main()
{
    try { return run(); }
    catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << std::endl;
        return 1;
    }
}
