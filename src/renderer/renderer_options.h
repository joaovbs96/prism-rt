#pragma once

// The renderer's options (docs/sdk_design.md, sections 2.2 and 3.4): what was
// read from process-wide environment variables at each use is one struct that
// the front ends fill, by default from the environment (FromEnvironment), and
// hand to the renderer context (renderer_context.h). Nothing below the front
// ends reads these environment variables.

#include "../color/color_management.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace prism {

class TextureService;

struct MaterialOptions {
    std::string materialxLibraries;   // PRISM_MATERIALX_LIBRARY_PATH
    std::string materialxMdlPath;     // PRISM_MATERIALX_MDL (empty: <libraries>/mdl)
    // Subsurface scattering renders its diffuse lobe (PRISM_SSS_FALLBACK;
    // the front ends' prism:sssFallback overrides it).
    bool sssFallback = false;
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
    // Generated MDL templates are reused from memory and disk
    // (PRISM_DISABLE_GENERATION_CACHE disables it; for measurement).
    bool generationCache = true;
    // How compiled materials read their textures (PRISM_MDL_RESOURCE_MODE):
    // auto, custom (PrismRT's texture runtime) or builtin (the MDL SDK's).
    std::string mdlResourceMode = "auto";
};

// Resource limits; 0 is the built-in default.
struct Limits {
    unsigned materialThreads = 0;     // PRISM_MATERIAL_THREADS (1-64)
    unsigned textureThreads = 0;      // PRISM_TEXTURE_THREADS (1-64)
    // Unused compiled materials kept in memory: PRISM_MATERIAL_CACHE_MB
    // (default 512). Process-wide: the largest any renderer asks for.
    uint64_t materialCacheMiB = 0;
    // Material compiles and template binds admitted at once:
    // PRISM_PREPARATION_MB (default 2048). Process-wide: the largest.
    uint64_t preparationMiB = 0;
    // Texture preparations admitted at once: PRISM_TEXTURE_PREPARE_MB
    // (default an eighth of memory in [2, 16] GiB). Process-wide: the first
    // renderer's.
    uint64_t texturePrepareMiB = 0;
    // The persistent caches' disk budget in GiB: PRISM_DISK_CACHE_GB
    // (default 20). Process-wide: the first renderer's.
    double diskCacheGiB = 0.0;
    // OIIO's texture tile cache: PRISM_TEXTURE_CACHE_MB (default a
    // sixteenth of memory in [512, 4096] MiB).
    uint64_t textureCacheMiB = 0;
    // Files OIIO keeps open: PRISM_TEXTURE_OPEN_FILES (default the stdio
    // limit less 512, in [100, 7680]).
    unsigned textureOpenFiles = 0;
    // Threads writing prepared textures in the background:
    // PRISM_TEXTURE_WRITER_THREADS (default the hardware threads).
    unsigned textureWriterThreads = 0;

    // Material compile workers: materialThreads, else half the hardware
    // threads, at most 8.
    unsigned MaterialWorkers() const;
    // Textures a material batch prepares concurrently: textureThreads, else
    // as MaterialWorkers.
    unsigned TexturePreparationThreads() const;
};

struct TextureOptions {
    // Compression of prepared textures: PRISM_TEXTURE_COMPRESSION
    // (empty: zip:1).
    std::string compression;
    // Prepared .tx.exr files are written after their GPU-ready copies are
    // published (PRISM_TEXTURE_BACKGROUND_WRITES=0 writes them first).
    bool backgroundWrites = true;
};

struct DeviceOptions {
    bool disableVulkan = false;       // PRISM_DISABLE_VULKAN (tests)
    bool disableMetal = false;        // PRISM_DISABLE_METAL
    // The GPU texture budget a device assumes before a scene sets one (its
    // early texture uploads): PRISM_GPU_TEXTURE_BUDGET_MIB; 0 is automatic.
    int initialTextureBudgetMiB = 0;
    // The renderer's texture service (RendererContext::deviceOptions); null:
    // the runtime's (TextureService::Acquire()).
    std::shared_ptr<TextureService> textures;
};

struct RendererOptions {
    // Colour management. An empty config is $OCIO in FromEnvironment and the
    // built-in studio config otherwise.
    color::Options color;
    MaterialOptions materials;
    Limits limits;
    TextureOptions textures;
    DeviceOptions devices;
    // The root of the persistent caches (PRISM_CACHE_DIR); empty: the
    // platform's cache directory (DefaultCacheDirectory). Process-wide: the
    // first renderer's (renderer/runtime.h).
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
