#pragma once

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include <memory>
#include <string>
#include <vector>

struct GeneratedUniform {
    std::string name;
    std::string path;
    std::string type;
    MaterialX::ValuePtr value;
};

struct GeneratedMDL {
    std::string module_name;
    std::string mdl_source;
    std::vector<std::string> texture_paths;
    // Generator-published mapping used only after the backend validates the
    // corresponding compiled parameter name and exact MDL type.
    std::vector<GeneratedUniform> uniforms;
    bool success;
    bool generationCacheHit = false;
    bool generationPersistentCacheHit = false;
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
