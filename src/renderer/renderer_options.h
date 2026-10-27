#pragma once

// The renderer's options (docs/sdk_design.md, sections 2.2 and 3.4): what was
// read from process-wide environment variables at each use is one struct that
// the front ends fill, by default from the environment (FromEnvironment), and
// hand to the renderer context (renderer_context.h). Nothing below the front
// ends reads these environment variables.

#include "../color/color_management.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace prism {

struct MaterialOptions {
    std::string materialxLibraries;   // PRISM_MATERIALX_LIBRARY_PATH
    std::string materialxMdlPath;     // PRISM_MATERIALX_MDL (empty: <libraries>/mdl)
    // Prefetch compile policy (PRISM_MDL_COMPILE_MODE): Auto folds every
    // class parameter that no material of a batch needs to rebind; Class
    // keeps all but strings runtime arguments.
    enum class CompileMode { Auto, Class };
    CompileMode compileMode = CompileMode::Auto;
    // Scene-independent material initialization started ahead of the first
    // material (PRISM_MATERIAL_PREWARM=0 disables it).
    bool prewarm = true;
    // A batch's textures are prepared ahead of its compiles
    // (PRISM_TEXTURE_PREPARE=serial leaves them to the compiles).
    bool parallelTexturePreparation = true;
};

// Resource limits; 0 is the built-in default.
struct Limits {
    unsigned materialThreads = 0;     // PRISM_MATERIAL_THREADS (1-64)
    unsigned textureThreads = 0;      // PRISM_TEXTURE_THREADS (1-64)

    // Material compile workers: materialThreads, else half the hardware
    // threads, at most 8.
    unsigned MaterialWorkers() const;
    // Textures a material batch prepares concurrently: textureThreads, else
    // as MaterialWorkers.
    unsigned TexturePreparationThreads() const;
};

struct DeviceOptions {
    bool disableVulkan = false;       // PRISM_DISABLE_VULKAN (tests)
    bool disableMetal = false;        // PRISM_DISABLE_METAL
    // The GPU texture budget a device assumes before a scene sets one (its
    // early texture uploads): PRISM_GPU_TEXTURE_BUDGET_MIB; 0 is automatic.
    int initialTextureBudgetMiB = 0;
};

struct RendererOptions {
    // Colour management. An empty config is $OCIO in FromEnvironment and the
    // built-in studio config otherwise.
    color::Options color;
    MaterialOptions materials;
    Limits limits;
    DeviceOptions devices;
    // The root of the persistent caches (PRISM_CACHE_DIR); empty: the
    // platform's cache directory (DefaultCacheDirectory).
    std::filesystem::path cacheDirectory;

    // cacheDirectory, or the platform's cache directory when it is empty.
    std::filesystem::path CacheRoot() const;

    // The front ends' defaults: every field from its environment variable
    // (OCIO, PRISM_*), the built-in default where it is unset. Invalid values
    // are reported on std::cerr and take the default.
    static RendererOptions FromEnvironment();
};

// The platform's cache directory for PrismRT: %LOCALAPPDATA%/PrismRT/cache,
// else the temporary directory's PrismRT/cache.
std::filesystem::path DefaultCacheDirectory();

} // namespace prism
