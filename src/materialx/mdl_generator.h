#pragma once

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <memory>
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

    // Process-wide generator for a libraries path, created on first use.
    // Loading the MaterialX standard library is by far the most expensive part
    // of construction, so materials should share one instance instead of
    // building their own. generate() is serialized internally.
    static std::shared_ptr<MDLGenerator> shared(const std::string& mx_libraries_path);
    // One generator per worker avoids serializing GenContext while retaining
    // standard-library reuse for all jobs executed by that worker.
    static std::shared_ptr<MDLGenerator> workerLocal(const std::string& mx_libraries_path);

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
