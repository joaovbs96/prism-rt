#include "mdl_generator.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXGenShader/GenContext.h>
#include <MaterialXGenShader/DefaultColorManagementSystem.h>
#include <MaterialXGenShader/Shader.h>
#include <MaterialXGenShader/ShaderStage.h>
#include <MaterialXGenShader/Util.h>
#include <MaterialXGenMdl/MdlShaderGenerator.h>
#include <iostream>
#include <map>
#include <cstdlib>

namespace {
bool VerboseMdl() {
    const char* value = std::getenv("PRISM_MDL_VERBOSE");
    return value && *value && std::string(value) != "0";
}
}
#include <mutex>

class MDLGenerator::Impl {
public:
    Impl(const std::string& mx_libraries_path)
        : m_generator(MaterialX::MdlShaderGenerator::create())
        , m_context(m_generator)
    {
        // mx_libraries_path = ".../MaterialX/install/libraries"
        MaterialX::FilePath librariesPath(mx_libraries_path);
        MaterialX::FilePath installRoot = librariesPath.getParentPath();

        // loadLibraries needs the libraries folder as the search root
        MaterialX::FileSearchPath librariesSearchPath;
        librariesSearchPath.append(librariesPath);

        m_stdlib = MaterialX::createDocument();
        MaterialX::loadLibraries({}, librariesSearchPath, m_stdlib);

        // Decode authored color spaces in the graph, leaving scalar/vector data
        // textures (roughness, metallic, normals) in their original linear form.
        auto color_management = MaterialX::DefaultColorManagementSystem::create(
            m_generator->getTarget());
        color_management->loadLibrary(m_stdlib);
        m_generator->setColorManagementSystem(color_management);
        m_context.getOptions().targetColorSpaceOverride = "lin_rec709";

        if (VerboseMdl()) std::cout << "[DEBUG] stdlib elements loaded: "
                << m_stdlib->getChildren().size() << "\n";

        // registerSourceCodeSearchPath needs the libraries folder itself
        // so the generator can resolve e.g. stdlib/genmdl/mx_stdlib.mdl
        // Register source code search paths for MDL implementation files
        MaterialX::FileSearchPath sourceSearchPath;
        sourceSearchPath.append(librariesPath);
        sourceSearchPath.append(librariesPath / MaterialX::FilePath("mdl/materialx"));
        m_context.registerSourceCodeSearchPath(sourceSearchPath);

        if (VerboseMdl()) std::cout << "[DEBUG] Source search root: " << installRoot.asString() << "\n";
    }

    MaterialX::DocumentPtr get_stdlib() const { return m_stdlib; }

GeneratedMDL generate(MaterialX::DocumentPtr doc, const std::string& material_name) {
    GeneratedMDL result;
    result.success = false;

    if (!doc) {
        result.error_message = "Null document";
        return result;
    }

    // Debug: print all nodes in the document
    if (VerboseMdl()) { std::cout << "[DEBUG] All nodes in document:\n";
    for (auto& node : doc->getNodes()) {
        std::cout << "  node: '" << node->getName()
                  << "' category: '" << node->getCategory()
                  << "' type: '" << node->getType() << "'\n";
        for (auto& input : node->getInputs()) {
            std::cout << "    input: '" << input->getName()
                      << "' type: '" << input->getType()
                      << "' nodename: '" << input->getNodeName() << "'\n";
        }
    } }

    // 1. Try a top-level output directly
    MaterialX::OutputPtr output = doc->getOutput(material_name);
    if (output)
        return generate_from_output(output, material_name);

    // 2. Try a surfacematerial node
    MaterialX::NodePtr material_node = doc->getNode(material_name);
    if (material_node) {
        // Generate directly from the material node — the MDL generator
        // handles surfacematerial as the top-level entry point
        return generate_from_node(material_node, material_name);
    }

    result.error_message = "Material/output not found: " + material_name;
    return result;
}

    GeneratedMDL generate_from_output(MaterialX::OutputPtr output,
                                      const std::string& name) {
        GeneratedMDL result;
        result.success = false;

        try {
            MaterialX::ShaderPtr shader = m_generator->generate(name, output, m_context);

            if (!shader) {
                result.error_message = "Failed to generate shader from output";
                return result;
            }

            const MaterialX::ShaderStage& pixelStage = shader->getStage(MaterialX::Stage::PIXEL);
            result.mdl_source  = pixelStage.getSourceCode();
            result.module_name = "::materialx::" + name;
            extract_textures(shader, result);
            result.success = true;

        } catch (const std::exception& e) {
            result.error_message = std::string("Code generation failed: ") + e.what();
        }

        return result;
    }

    GeneratedMDL generate_from_node(MaterialX::NodePtr node, const std::string& name) {
        GeneratedMDL result;
        result.success = false;

        try {
            // Debug: check nodedef resolution before generating
            MaterialX::NodeDefPtr nodedef = node->getNodeDef();
            if (!nodedef && VerboseMdl()) {
                std::cout << "[DEBUG] getNodeDef() returned null for '"
                        << node->getName() << "' (category: '"
                        << node->getCategory() << "')\n";
                std::cout << "[DEBUG] Document has "
                        << node->getDocument()->getNodeDefs().size()
                        << " nodedefs total\n";
                // List first few nodedefs to see what's actually loaded
                int count = 0;
                for (auto& nd : node->getDocument()->getNodeDefs()) {
                    std::cout << "  nodedef: '" << nd->getName()
                            << "' node: '" << nd->getNodeString() << "'\n";
                    if (++count >= 5) { std::cout << "  ...\n"; break; }
                }
            } else if (VerboseMdl()) {
                std::cout << "[DEBUG] nodedef found: '" << nodedef->getName() << "'\n";
            }

            MaterialX::ShaderPtr shader = m_generator->generate(name, node, m_context);

            if (!shader) {
                result.error_message = "Failed to generate shader from node";
                return result;
            }

            const MaterialX::ShaderStage& pixelStage = shader->getStage(MaterialX::Stage::PIXEL);
            result.mdl_source  = pixelStage.getSourceCode();
            result.module_name = "::materialx::" + name;
            extract_textures(shader, result);
            result.success = true;

        } catch (const std::exception& e) {
            result.error_message = std::string("Code generation failed: ") + e.what();
        }

        return result;
    }

    void extract_textures(MaterialX::ShaderPtr shader, GeneratedMDL& result) {
        const MaterialX::ShaderStage& pixelStage = shader->getStage(MaterialX::Stage::PIXEL);

        for (const auto& [blockName, block] : pixelStage.getUniformBlocks()) {
            for (size_t i = 0; i < block->size(); ++i) {
                const MaterialX::ShaderPort* variable = (*block)[i];
                if (!variable) continue;

                if (variable->getType().getName() == "filename") {
                    auto value = variable->getValue();
                    if (value) {
                        result.texture_paths.push_back(value->getValueString());
                    }
                }
            }
        }
    }

    // The GenContext is shared state, so a shared generator must not run
    // two generations at once.
    std::mutex generate_mutex;

private:
    MaterialX::ShaderGeneratorPtr m_generator;
    MaterialX::GenContext         m_context;
    MaterialX::DocumentPtr        m_stdlib;
};

MDLGenerator::MDLGenerator(const std::string& mx_libraries_path)
    : m_impl(std::make_unique<Impl>(mx_libraries_path))
{}

MDLGenerator::~MDLGenerator() = default;

MaterialX::DocumentPtr MDLGenerator::get_stdlib() const {
    return m_impl->get_stdlib();
}

std::shared_ptr<MDLGenerator> MDLGenerator::shared(const std::string& mx_libraries_path) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<MDLGenerator>> generators;

    std::lock_guard<std::mutex> lock(mutex);
    auto& generator = generators[mx_libraries_path];
    if (!generator)
        generator = std::make_shared<MDLGenerator>(mx_libraries_path);
    return generator;
}

std::shared_ptr<MDLGenerator> MDLGenerator::workerLocal(
    const std::string& mx_libraries_path) {
    thread_local std::map<std::string, std::shared_ptr<MDLGenerator>> generators;
    auto& generator = generators[mx_libraries_path];
    if (!generator)
        generator = std::make_shared<MDLGenerator>(mx_libraries_path);
    return generator;
}

GeneratedMDL MDLGenerator::generate(MaterialX::DocumentPtr doc,
                                    const std::string& material_name) {
    std::lock_guard<std::mutex> lock(m_impl->generate_mutex);
    return m_impl->generate(doc, material_name);
}

GeneratedMDL MDLGenerator::generate(MaterialX::OutputPtr output) {
    std::lock_guard<std::mutex> lock(m_impl->generate_mutex);
    std::string name = output->getName();
    return m_impl->generate_from_output(output, name);
}
