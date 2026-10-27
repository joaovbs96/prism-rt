#include "../src/materialx/mdl_generator.h"
#include "../src/materialx/usd_network.h"
#include <pxr/imaging/hd/materialNetwork2Interface.h>
#include <MaterialXCore/Material.h>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <fstream>

namespace { const auto kColor = prism::color::ColorManagement::Create({}); }

namespace mx = MaterialX;
struct TestCache {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
        ("prism-material-generation-"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TestCache() {
        std::filesystem::create_directories(path);
#ifdef _WIN32
        _putenv_s("PRISM_CACHE_DIR",path.string().c_str());
#else
        setenv("PRISM_CACHE_DIR",path.string().c_str(),1);
#endif
    }
    ~TestCache(){std::error_code error;std::filesystem::remove_all(path,error);}
};
mx::DocumentPtr document(mx::DocumentPtr library, mx::Color3 color) {
    auto doc = mx::createDocument();
    doc->setDataLibrary(library);
    auto shader = doc->addNode("open_pbr_surface", "Surface", "surfaceshader");
    shader->setInputValue("base_color", color);
    shader->setInputValue("base_weight", 1.0f);
    doc->addMaterialNode("PrismMaterial", shader);
    return doc;
}
int main(int argc,char** argv) {
    try {
        const char* libraries = std::getenv("PRISM_MATERIALX_LIBRARY_PATH");
        if (!libraries || !*libraries) return 77;
        const std::string libraryPath(libraries);
        std::unique_ptr<TestCache> isolatedCache;
        if(!(argc==5&&std::string(argv[1])=="--probe"))
            isolatedCache=std::make_unique<TestCache>();
        MDLGenerator generator(libraryPath);
        auto library = generator.get_stdlib();
        if(argc==5&&std::string(argv[1])=="--probe") {
            const mx::Color3 color(std::stof(argv[2]),std::stof(argv[3]),std::stof(argv[4]));
            const auto generated=generator.generate(document(library,color), "PrismMaterial", *kColor);
            if(!generated.success)throw std::runtime_error(generated.error_message);
            uint64_t hash=14695981039346656037ull;
            for(unsigned char c:generated.mdl_source){hash^=c;hash*=1099511628211ull;}
            std::cout<<"persistentProbe="<<(generated.generationPersistentCacheHit?"hit":"miss")
                     <<" hash="<<std::hex<<hash<<'\n';
            return 0;
        }
        // Standalone USD network metadata must not force full library copies.
        // Color metadata is consumed through GetNodeParameterData, not emitted
        // as a MaterialX input or lost in the small-document conversion.
        {
            PXR_NAMESPACE_USING_DIRECTIVE
            HdMaterialNetwork2 network;
            const SdfPath surface("/Material/Surface");
            auto& node=network.nodes[surface];
            node.nodeTypeId=TfToken("ND_open_pbr_surface_surfaceshader");
            node.parameters[TfToken("base_color")]=VtValue(GfVec3f(.2f,.3f,.4f));
            node.parameters[TfToken("typeName:base_color")]=VtValue(TfToken("color3f"));
            node.parameters[TfToken("colorSpace:base_color")]=VtValue(TfToken("lin_rec709"));
            HdMaterialNetwork2Interface interface(SdfPath("/Material"), &network);
            const auto small=prism::materialx::SimpleOpenPbrDocument(interface, surface.GetToken(), library);
            if (!small || small->getDataLibrary()!=library || small->getNodes().size()!=2)
                throw std::runtime_error("Simple legacy USD network copied/fell back from standard library");
            const auto material=small->getMaterialNodes().at(0);
            const auto shaders=mx::getShaderNodes(material);
            if(shaders.size()!=1 || shaders.front()->getInput("base_color")->getColorSpace()!="lin_rec709" ||
                shaders.front()->getInput("base_color")->getValue()->asA<mx::Color3>()!=mx::Color3(.2f,.3f,.4f))
                throw std::runtime_error("Simple USD conversion lost color or metadata");
            node.inputConnections[TfToken("base_color")].push_back({SdfPath("/Texture"),TfToken("rgb")});
            if(prism::materialx::SimpleOpenPbrDocument(interface,surface.GetToken(),library))
                throw std::runtime_error("Connected USD graph incorrectly used parameter-only conversion");
        }

        auto other = std::async(std::launch::async, [=] {
            MDLGenerator worker(libraryPath);
            return worker.get_stdlib();
        }).get();
        if (library != other) throw std::runtime_error("workers duplicated the immutable library");
        auto a = generator.generate(document(library, mx::Color3(.2f,.3f,.4f)), "PrismMaterial", *kColor);
        auto bdoc = document(library, mx::Color3(.8f,.15f,.25f));
        auto b = generator.generate(bdoc, "PrismMaterial", *kColor);
        if (!a.success || !b.success || !b.generationCacheHit)
            throw std::runtime_error("color parameter template was not reused: " + a.error_message + b.error_message);
        if (a.mdl_source == b.mdl_source) throw std::runtime_error("template retained stale color");
        auto changedStructure=document(library, mx::Color3(.8f,.15f,.25f));
        changedStructure->getNode("Surface")->setInputValue("specular_weight", 0.0f);
        const auto changed=generator.generate(changedStructure, "PrismMaterial", *kColor);
        if(!changed.success || changed.generationCacheHit)
            throw std::runtime_error("non-parameterized scalar incorrectly reused color template");
#ifdef _WIN32
        _putenv_s("PRISM_DISABLE_GENERATION_CACHE", "1");
#else
        setenv("PRISM_DISABLE_GENERATION_CACHE", "1", 1);
#endif
        auto reference = generator.generate(bdoc, "PrismMaterial", *kColor);
        if (!reference.success || b.mdl_source != reference.mdl_source) {
            std::ofstream("template-cached.mdl") << b.mdl_source;
            std::ofstream("template-reference.mdl") << reference.mdl_source;
            throw std::runtime_error("template differs from independent MaterialX generation");
        }
        const auto changedReference=generator.generate(changedStructure, "PrismMaterial", *kColor);
        if(!changedReference.success || changed.mdl_source!=changedReference.mdl_source)
            throw std::runtime_error("scalar edit did not match independent generation");
        std::cout << "Shared libraries and color template generation match independent generation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
