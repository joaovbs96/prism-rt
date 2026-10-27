#pragma once

#include <MaterialXCore/Document.h>
#include <MaterialXCore/Node.h>
#include "../color/color_management.h"
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
    // `color`, when given, prepares the generator for that colour management
    // (else the first generation does).
    explicit MDLGenerator(const std::string& mx_libraries_path,
                          prism::color::ColorManagement const* color = nullptr);
    ~MDLGenerator();

    // Process-wide generator for a libraries path, created on first use.
    // Loading the MaterialX standard library is by far the most expensive part
    // of construction, so materials should share one instance instead of
    // building their own. generate() is serialized internally.
    static std::shared_ptr<MDLGenerator> shared(const std::string& mx_libraries_path);
    // One generator per worker avoids serializing GenContext while retaining
    // standard-library reuse for all jobs executed by that worker. It is taken
    // from a process-wide idle pool and returned when the thread exits, so
    // short-lived batch threads do not each build one. A new one is prepared
    // for `color`.
    static std::shared_ptr<MDLGenerator> workerLocal(const std::string& mx_libraries_path,
                                                     prism::color::ColorManagement const* color = nullptr);
    // Fills the idle pool up to count generators (bounded), loading the
    // standard library first if needed; new ones are prepared for `color`.
    static void reserveWorkers(const std::string& mx_libraries_path, size_t count,
                               prism::color::ColorManagement const* color = nullptr);

    // Generates in `color`'s working space: tagged colours and textures
    // decode into it (the generator's colour transforms).
    GeneratedMDL generate(MaterialX::DocumentPtr doc,
                          const std::string& material_name,
                          prism::color::ColorManagement const& color);
    GeneratedMDL generate(MaterialX::OutputPtr output,
                          prism::color::ColorManagement const& color);

    // Returns the stdlib document loaded by this generator.
    // Pass this to DocumentLoader so both share the same stdlib instance.
    MaterialX::DocumentPtr get_stdlib() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
