#include "loader.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <MaterialXCore/Traversal.h>
#include <algorithm>
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
