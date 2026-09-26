#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <vector>

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
        uint64_t requests = 0;
        uint64_t pathCacheHits = 0;
        uint64_t contentCacheHits = 0;
        uint64_t compilations = 0;
        uint64_t nativeJits = 0;
        uint64_t jobs = 0;
        uint64_t failures = 0;
        uint64_t textureBytes = 0;
        double conversionSeconds = 0.0;
        double compilationSeconds = 0.0;
        double generationSeconds = 0.0;
        double classCompilationSeconds = 0.0;
        double nativeJitSeconds = 0.0;
        double resourceReadSeconds = 0.0;
        double batchWallSeconds = 0.0;
        double workerCpuSeconds = 0.0;
    };
    MaterialService(mdl::Context* context,
                    std::string materialXLibrariesPath = {},
                    std::string materialXMdlPath = {});
    ~MaterialService();

    MaterialService(const MaterialService&) = delete;
    MaterialService& operator=(const MaterialService&) = delete;

    // materialPrimDataSource is the data source of an HdMaterial prim, as
    // returned by HdSceneIndexBase::GetPrim(materialPath).dataSource.
    // Returns nullptr when no supported network or MDL context is available.
    // Records are cached by path until Invalidate/InvalidateAll. Callers must
    // forward material dirty notices and explicitly invalidate texture reloads;
    // data-source pointer identity is not a reliable change notification.
    std::shared_ptr<Material> Resolve(
        const pxr::HdContainerDataSourceHandle& materialPrimDataSource,
        const pxr::SdfPath& materialPath);

    // Captures a batch on the calling Hydra sync thread, then resolves it with
    // a bounded worker pool. Do not call this from multiple sync threads.
    void Prefetch(const std::vector<Request>& requests);
    void Invalidate(const pxr::SdfPath& path, bool subtree = false);
    void InvalidateAll();
    MaterialStats GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::hydra
