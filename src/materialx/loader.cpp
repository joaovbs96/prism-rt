#include "loader.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <algorithm>
#include <format>
#include <iostream>

DocumentLoader::DocumentLoader(MaterialX::DocumentPtr stdlib, bool clamp)
    : m_stdlib(stdlib), m_clamp(clamp)
{}

MaterialX::DocumentPtr DocumentLoader::load(const std::string& filepath)
{
    MaterialX::DocumentPtr doc = MaterialX::createDocument();
    doc->importLibrary(m_stdlib);

    try {
        MaterialX::readFromXmlFile(doc, filepath);

        std::string validation_errors;
        if (!doc->validate(&validation_errors)) {
            std::cerr << "[WARN] Document validation issues: " << validation_errors << "\n";
        }

        // Clamp color3 inputs to [0, 0.9999]. F0 values > 1.0 (e.g. gold base_color.R = 1.059)
        // cause a singularity in the conductor IOR formula (1+sqrt(F0))/(1-sqrt(F0)) used by
        // MaterialX's standard_surface MDL translation.
        if (m_clamp)
        {
            for (auto& node : doc->getNodes()) {
                for (auto& input : node->getInputs()) {
                    if (input->getType() == "color3" && input->hasValue()) {
                        MaterialX::Color3 c = input->getValue()->asA<MaterialX::Color3>();
                        MaterialX::Color3 orig = c;
                        c[0] = std::min(c[0], 0.9999f);
                        c[1] = std::min(c[1], 0.9999f);
                        c[2] = std::min(c[2], 0.9999f);
                        if (c != orig) {
                            input->setValue(c);
                            std::cout << std::format(
                                "[WARN] Clamped OOB color3 '{}.{}': ({:.4f},{:.4f},{:.4f}) -> ({:.4f},{:.4f},{:.4f})\n",
                                node->getName(), input->getName(),
                                orig[0], orig[1], orig[2], c[0], c[1], c[2]);
                        }
                    }
                }
            }
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
    doc->importLibrary(m_stdlib);
    return doc;
}