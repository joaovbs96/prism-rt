#pragma once

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <string>
#include <vector>

struct GeneratedMDL {
    std::string module_name;
    std::string mdl_source;
    std::vector<std::string> texture_paths;
    bool success;
    std::string error_message;
};

class MDLGenerator {
public:
    explicit MDLGenerator(const std::string& mx_libraries_path);
    ~MDLGenerator();

    GeneratedMDL generate(MaterialX::DocumentPtr doc,
                          const std::string& material_name);
    GeneratedMDL generate(MaterialX::OutputPtr output);

    // Returns the stdlib document loaded by this generator.
    // Pass this to DocumentLoader so both share the same stdlib instance.
    MaterialX::DocumentPtr get_stdlib() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};