#pragma once

#include <memory>
#include <string>

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
// service owns the resulting materials and is safe to call from scene-sync
// code; MDL compilation and cache access are serialized internally.
class MaterialService {
public:
    MaterialService(mdl::Context* context,
                    std::string materialXLibrariesPath = {},
                    std::string materialXMdlPath = {});
    ~MaterialService();

    MaterialService(const MaterialService&) = delete;
    MaterialService& operator=(const MaterialService&) = delete;

    // materialPrimDataSource is the data source of an HdMaterial prim, as
    // returned by HdSceneIndexBase::GetPrim(materialPath).dataSource.
    // Returns nullptr when no supported network or MDL context is available.
    std::shared_ptr<Material> Resolve(
        const pxr::HdContainerDataSourceHandle& materialPrimDataSource,
        const pxr::SdfPath& materialPath);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace prism::hydra
