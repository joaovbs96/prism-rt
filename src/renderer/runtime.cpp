#include "runtime.h"

#include "../resource_limits.h"

#include <algorithm>
#include <iostream>

namespace prism {

std::filesystem::path RuntimeSettings::CacheRoot() const
{
    return cacheDirectory.empty() ? DefaultCacheDirectory() : cacheDirectory;
}

Runtime& Runtime::Instance()
{
    // Leaked, as the caches whose settings it holds (they hold MDL SDK
    // handles and must not be torn down by static destruction).
    static Runtime* runtime = new Runtime;
    return *runtime;
}

void Runtime::configureLocked(RendererOptions const& options, bool fromRenderer)
{
    RuntimeSettings requested;
    requested.cacheDirectory = options.cacheDirectory;
    if (options.limits.diskCacheGiB > 0.0) requested.diskCacheGiB = options.limits.diskCacheGiB;
    if (options.limits.preparationMiB) requested.preparationMiB = options.limits.preparationMiB;
    if (options.limits.materialCacheMiB) requested.materialCacheMiB = options.limits.materialCacheMiB;
    requested.texturePrepareMiB = options.limits.texturePrepareMiB;
    requested.limits = options.limits;
    requested.textures = options.textures;
    if (!m_configured) {
        m_settings = requested;
        m_configured = true;
        return;
    }
    if (!fromRenderer) return;
    if (requested.cacheDirectory != m_settings.cacheDirectory || requested.diskCacheGiB != m_settings.diskCacheGiB)
        std::cerr << "PrismRT: a renderer asked for another cache directory or disk cache budget than the "
                     "first one of this process; the shared caches keep the first one's\n";
    m_settings.preparationMiB = std::max(m_settings.preparationMiB, requested.preparationMiB);
    m_settings.materialCacheMiB = std::max(m_settings.materialCacheMiB, requested.materialCacheMiB);
}

void Runtime::Configure(RendererOptions const& options)
{
    uint64_t preparationMiB = 0;
    {
        std::lock_guard lock(m_mutex);
        configureLocked(options, true);
        preparationMiB = m_settings.preparationMiB;
    }
    // Admission created before (by an earlier renderer) grows to the largest.
    PreparationAdmission::Shared().RaiseLimit(preparationMiB * 1024 * 1024);
}

RuntimeSettings Runtime::settings() const
{
    std::lock_guard lock(m_mutex);
    if (!m_configured) const_cast<Runtime*>(this)->configureLocked(RendererOptions::FromEnvironment(), false);
    return m_settings;
}

} // namespace prism
