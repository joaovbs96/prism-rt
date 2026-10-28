#include "renderer_options.h"

#include "../render_defaults.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <iostream>
#include <string_view>
#include <thread>

namespace prism {

namespace {

const char* Text(const char* name)
{
    const char* value = std::getenv(name);
    return value && *value ? value : nullptr;
}

// A positive integer of at most `maximum`; 0 (the built-in default) when the
// variable is unset or invalid (reported, as ResourceLimit does).
uint64_t PositiveLimit(const char* name, uint64_t maximum)
{
    const char* text = Text(name);
    if (!text) return 0;
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (*text == '-' || end == text || *end || value == 0 || value > maximum) {
        std::cerr << "PrismRT: ignoring invalid " << name << '\n';
        return 0;
    }
    return value;
}

unsigned DefaultWorkers()
{
    return std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
}

} // namespace

unsigned Limits::MaterialWorkers() const
{
    return materialThreads ? materialThreads : DefaultWorkers();
}

unsigned Limits::TexturePreparationThreads() const
{
    return textureThreads ? textureThreads : DefaultWorkers();
}

std::filesystem::path DefaultCacheDirectory()
{
    if (const char* value = Text("LOCALAPPDATA")) return std::filesystem::path(value) / "PrismRT" / "cache";
    return std::filesystem::temp_directory_path() / "PrismRT" / "cache";
}

std::filesystem::path RendererOptions::CacheRoot() const
{
    return cacheDirectory.empty() ? DefaultCacheDirectory() : cacheDirectory;
}

RendererOptions RendererOptions::FromEnvironment()
{
    RendererOptions options;
    if (const char* value = Text("OCIO")) options.color.config = value;

    auto& materials = options.materials;
    if (const char* value = std::getenv("PRISM_MATERIALX_LIBRARY_PATH")) materials.materialxLibraries = value;
    if (const char* value = std::getenv("PRISM_MATERIALX_MDL")) materials.materialxMdlPath = value;
    materials.sssFallback = defaults::FromEnvironment().sssFallback;
    if (const char* value = std::getenv("PRISM_MDL_COMPILE_MODE")) {
        const std::string_view mode(value);
        if (mode == "class") materials.compileMode = MaterialOptions::CompileMode::Class;
        else if (!mode.empty() && mode != "auto")
            std::cerr << "PrismRT: PRISM_MDL_COMPILE_MODE must be auto or class; using auto\n";
    }
    if (const char* value = std::getenv("PRISM_MATERIAL_PREWARM"))
        materials.prewarm = std::string_view(value) != "0";
    if (const char* value = std::getenv("PRISM_TEXTURE_PREPARE")) {
        const std::string_view mode(value);
        if (mode == "serial") materials.parallelTexturePreparation = false;
        else if (!mode.empty() && mode != "parallel")
            std::cerr << "PrismRT: PRISM_TEXTURE_PREPARE must be parallel or serial; using parallel\n";
    }

    // Only its presence counts (as before: a measurement switch).
    if (std::getenv("PRISM_DISABLE_GENERATION_CACHE")) materials.generationCache = false;
    if (const char* value = std::getenv("PRISM_MDL_RESOURCE_MODE")) materials.mdlResourceMode = value;

    auto& limits = options.limits;
    limits.materialThreads = unsigned(PositiveLimit("PRISM_MATERIAL_THREADS", 64));
    limits.textureThreads = unsigned(PositiveLimit("PRISM_TEXTURE_THREADS", 64));
    limits.materialCacheMiB = PositiveLimit("PRISM_MATERIAL_CACHE_MB", 1048576);
    limits.preparationMiB = PositiveLimit("PRISM_PREPARATION_MB", 1048576);
    limits.texturePrepareMiB = PositiveLimit("PRISM_TEXTURE_PREPARE_MB", 1u << 20);
    limits.textureCacheMiB = PositiveLimit("PRISM_TEXTURE_CACHE_MB", 65536);
    limits.textureOpenFiles = unsigned(PositiveLimit("PRISM_TEXTURE_OPEN_FILES", 8192));
    limits.textureWriterThreads = unsigned(PositiveLimit("PRISM_TEXTURE_WRITER_THREADS", 256));
    if (const char* value = Text("PRISM_DISK_CACHE_GB")) {
        // Invalid values take the default (silently, as before).
        try {
            size_t consumed = 0;
            const double parsed = std::stod(value, &consumed);
            if (consumed == std::string_view(value).size() && parsed > 0 && std::isfinite(parsed))
                limits.diskCacheGiB = std::min(parsed, 1024.0);
        } catch (...) {}
    }

    if (const char* value = Text("PRISM_TEXTURE_COMPRESSION")) options.textures.compression = value;
    if (const char* value = std::getenv("PRISM_TEXTURE_BACKGROUND_WRITES"))
        options.textures.backgroundWrites = std::string_view(value) != "0";

    auto disabled = [](const char* name) {
        const char* value = Text(name);
        return value && *value != '0';
    };
    options.devices.disableVulkan = disabled("PRISM_DISABLE_VULKAN");
    options.devices.disableMetal = disabled("PRISM_DISABLE_METAL");
    options.devices.initialTextureBudgetMiB = defaults::FromEnvironment().gpuTextureBudgetMiB;

    auto on = [](const char* name) {
        const char* value = Text(name);
        return value && std::string_view(value) != "0";
    };
    if (on("PRISM_MDL_VERBOSE")) options.debugCategories.push_back("mdl");
    if (on("PRISM_GPU_VERBOSE")) options.debugCategories.push_back("gpu");
    if (on("PRISM_VERBOSE_LIGHTS")) options.debugCategories.push_back("lights");

    if (const char* value = Text("PRISM_CACHE_DIR")) options.cacheDirectory = value;
    return options;
}

} // namespace prism
