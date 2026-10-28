#pragma once

// Material resources read through USD's Ar (asset resolver): the USD
// loaders' side of material_resource.h.

#include "material_resource.h"

#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>

namespace prism::materialx {

// A reader of an Ar-resolved asset in its resolver context.
inline MaterialResourceReader ArResourceReader(const PXR_NS::ArResolvedPath& resolved,
                                               const PXR_NS::ArResolverContext& resolverContext)
{
    MaterialResourceReader reader;
    reader.path = resolved.GetPathString();
    reader.resolverIdentity = resolverContext.GetDebugString();
    reader.context = MaterialResourceContext(resolverContext);
    reader.open = [resolved, resolverContext]() -> std::optional<MaterialResourceReader::Bytes> {
        PXR_NS::ArResolverContextBinder binder(resolverContext);
        auto asset = PXR_NS::ArGetResolver().OpenAsset(resolved);
        if (!asset) return std::nullopt;
        return MaterialResourceReader::Bytes{asset->GetBuffer(), asset->GetSize()};
    };
    return reader;
}

inline MaterialResourceResult PrepareMaterialResource(
    const PXR_NS::ArResolvedPath& resolved,
    const PXR_NS::ArResolverContext& resolverContext,
    const std::string& sourceVersion,
    const std::filesystem::path& cacheDir,
    const std::string& extension)
{
    return PrepareMaterialResource(ArResourceReader(resolved, resolverContext), sourceVersion, cacheDir, extension);
}

} // namespace prism::materialx
