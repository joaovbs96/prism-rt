#pragma once

// The process runtime (docs/sdk_design.md, sections 2.3 and 3.1): what every
// renderer context of the process shares. The MDL SDK runs one INeuray per
// process (SharedMdlContext), and the caches of generated MDL, compiled
// materials, material templates, GPU programs and prepared textures are
// shared by content: their keys carry everything that changes their contents
// (MaterialSettings::Identity for the colour management and the SSS fallback,
// the MaterialX library digest, the texture's colour identity). Those caches
// stay in their modules; the runtime holds the settings they need: the cache
// root and the budgets.
//
// The first renderer context configures it. A later one with another cache
// directory or disk budget gets a warning and uses the first one's (the
// caches would otherwise be split between roots mid-process), as for the
// texture preparation budget and the default texture service's options; the
// admission and material cache budgets take the largest any renderer asked
// for. Code that runs before any
// renderer (tests, tools) gets the environment's settings
// (RendererOptions::FromEnvironment).

#include "../internal_export.h"
#include "renderer_options.h"

#include <cstdint>
#include <filesystem>
#include <mutex>

namespace prism {

struct PRISM_INTERNAL_API RuntimeSettings {
    // The explicit cache root; empty: each cache's platform default
    // (DefaultCacheDirectory; GPU shaders keep their own, hlsl_compiler.h).
    std::filesystem::path cacheDirectory;
    double diskCacheGiB = 20.0;
    uint64_t preparationMiB = 2048;    // PreparationAdmission
    uint64_t materialCacheMiB = 512;   // unused compiled materials kept
    uint64_t texturePrepareMiB = 0;    // 0: an eighth of memory in [2, 16] GiB
    // The texture options of the first renderer: those of the texture
    // service that code without a renderer at hand uses
    // (TextureService::Acquire()).
    Limits limits;
    TextureOptions textures;

    // cacheDirectory, else DefaultCacheDirectory().
    std::filesystem::path CacheRoot() const;
};

class PRISM_INTERNAL_API Runtime {
public:
    // The process runtime; configured from the environment on first use
    // unless a renderer context configured it before.
    static Runtime& Instance();

    // Called by every renderer context with its options.
    void Configure(RendererOptions const& options);
    RuntimeSettings settings() const;

private:
    Runtime() = default;
    void configureLocked(RendererOptions const& options, bool fromRenderer);
    mutable std::mutex m_mutex;
    bool m_configured = false;
    RuntimeSettings m_settings;
};

} // namespace prism
