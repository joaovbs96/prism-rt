#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include <pxr/pxr.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/imaging/hd/dataSourceTypeDefs.h>

class Material;
namespace mdl { class Context; }

namespace prism::hydra {

// Process-lifetime MDL runtime shared by renderer instances. Hydra renderer
// objects may be created/destroyed repeatedly while compiled materials still
// reference SDK state, so the runtime intentionally outlives plugin instances.
mdl::Context& SharedMdlContext();

// Converts scene-index material data sources to PrismRT MDL materials. The
// service owns the resulting materials. Prefetch captures Hydra data on the
// calling sync thread, then compiles immutable MaterialX inputs in parallel.
class MaterialService {
public:
    struct Request {
        pxr::HdContainerDataSourceHandle dataSource;
        pxr::SdfPath path;
    };
    struct MaterialStats {
        uint64_t cacheAccountedBytes = 0; // Known code, argument and XML bytes; SDK overhead excluded.
        uint64_t unusedCacheBytes = 0;
        uint64_t cacheEvictions = 0;
        uint64_t preparationPeakReservedBytes = 0;
        uint64_t requests = 0;
        uint64_t pathCacheHits = 0;
        uint64_t contentCacheHits = 0;
        uint64_t earlyCacheHits = 0;
        uint64_t conversionCalls = 0;
        uint64_t generationCalls = 0;
        uint64_t templateCacheHits = 0;
        uint64_t compilations = 0;
        // Compilations whose program has class parameters folded to constants.
        uint64_t foldedCompilations = 0;
        uint64_t nativeJits = 0;
        uint64_t sdkTranslationCalls = 0;
        uint64_t programCacheHits = 0;
        uint64_t bindingCreations = 0;
        uint64_t fallbackTranslations = 0;
        uint64_t persistentGenerationCacheHits = 0;
        uint64_t sdkTextureFallbacks = 0;
        uint64_t customTextureTargets = 0;
        uint64_t jobs = 0;
        uint64_t failures = 0;
        uint64_t textureBytes = 0;
        double conversionSeconds = 0.0;
        double compilationSeconds = 0.0;
        double generationSeconds = 0.0;
        double classCompilationSeconds = 0.0;
        double nativeJitSeconds = 0.0;
        double programWaitSeconds = 0.0;
        double resourceReadSeconds = 0.0;
        double batchWallSeconds = 0.0;
        double workerCpuSeconds = 0.0;
        // Duration of the finished pre-warm of this service's key, and time
        // Prefetch/Resolve of this service spent waiting for a running one.
        double prewarmSeconds = 0.0;
        double prewarmWaitSeconds = 0.0;
        // Pre-warm jobs started in this process, for every key.
        uint64_t prewarmJobs = 0;
    };
    MaterialService(mdl::Context* context,
                    std::string materialXLibrariesPath = {},
                    std::string materialXMdlPath = {},
                    uint64_t associationScope = 0);
    ~MaterialService();

    MaterialService(const MaterialService&) = delete;
    MaterialService& operator=(const MaterialService&) = delete;

    // materialPrimDataSource is the data source of an HdMaterial prim, as
    // returned by HdSceneIndexBase::GetPrim(materialPath).dataSource.
    // Returns nullptr when no supported network or MDL context is available.
    // Records are cached by path until Invalidate/InvalidateAll. Callers must
    // forward material dirty notices. InvalidateAll is the explicit reload
    // boundary for resolver-backed resources; ordinary filesystem textures are
    // guarded by size and modification time across service recreation.
    // Data-source pointer identity is not a reliable change notification.
    std::shared_ptr<Material> Resolve(
        const pxr::HdContainerDataSourceHandle& materialPrimDataSource,
        const pxr::SdfPath& materialPath);

    // Starts, on a std::async thread, the scene-independent initialization the
    // first material would otherwise pay: the MaterialX libraries of USD's
    // converter and of the generator, idle worker generators, MDL search
    // paths and the MaterialX MDL support modules. Runs at most once per
    // process for this context and MaterialX library/MDL paths; thread-safe.
    // Prefetch and Resolve wait for a running pre-warm of the same key, and
    // the destructor waits for one this service started. A failure is logged
    // and materials then initialize on first use. PRISM_MATERIAL_PREWARM=0
    // disables it.
    void Prewarm();
    // Blocks until the pre-warm of this service's key, if any, has finished.
    void WaitForPrewarm();

    // Captures a batch on the calling Hydra sync thread, then resolves it with
    // a bounded worker pool. Do not call this from multiple sync threads.
    void Prefetch(const std::vector<Request>& requests,
                  const std::function<bool()>& cancelled = {});
    // Partitions process-wide records by the caller's resource generation.
    void SetResourceRevision(uint64_t revision);
    // These also invalidate matching process-wide conversion records.
    void Invalidate(const pxr::SdfPath& path, bool subtree = false);
    // Drops only path-to-result shortcuts. Immutable exact graph/content
    // records remain eligible when a recapture proves that they still match.
    // An empty path drops every association owned by this service.
    void InvalidateAssociations(const pxr::SdfPath& path,
                                bool subtree = false);
    // Marks matching material paths dirty and removes their process-wide
    // early associations. Canonical content/program caches remain immutable.
    void InvalidateResources(const pxr::SdfPath& path,
                             bool subtree = false);
    // Explicitly reload a resolved filesystem resource, including edits that
    // preserve size/mtime. Call on the synchronization thread. The returned
    // dependent paths must be scheduled for snapshot replacement by the caller.
    // Existing bindings remain valid; compatible programs are retained.
    std::vector<pxr::SdfPath> ReloadResource(const std::string& resolvedPath);
    void InvalidateAll();
    MaterialStats GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::hydra
