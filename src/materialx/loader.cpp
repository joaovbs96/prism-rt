#include "loader.h"
#include <MaterialXFormat/XmlIo.h>
#include <MaterialXFormat/Util.h>
#include <iostream>

DocumentLoader::DocumentLoader(MaterialX::DocumentPtr stdlib)
    : m_stdlib(stdlib)
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