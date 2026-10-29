#pragma once

// The document-level colour space of a .mtlx file referenced into a USD stage
// (docs/materialx_compatibility.md, "Document colour space through USD").
//
// usdMtlx keeps <materialx colorspace="..."> only as the translated layer's
// customLayerData["colorSpace"], which composition does not carry to the
// referencing stage: UsdShade inputs without their own colorspace arrive
// untagged and would be read as working-space values. Both loaders publish
// the colour space of the .mtlx layer that defines a Material as the prim
// data source `prismMtlxDocument.colorSpace` (the CLI in UsdMaterialDataSource,
// Hydra through an API schema adapter for MaterialXConfigAPI, which usdMtlx
// applies to every Material), and the material service tags the network's
// untagged colour and file parameters with it before the network is captured
// (so every cache key includes it) and converted.

#include <pxr/base/tf/token.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/hd/dataSource.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/materialNetworkInterface.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/usd/prim.h>
#include <string>

namespace prism::usd {

inline PXR_NS::TfToken const& MtlxDocumentToken()
{
    static const PXR_NS::TfToken token("prismMtlxDocument");
    return token;
}

inline PXR_NS::TfToken const& MtlxDocumentColorSpaceToken()
{
    static const PXR_NS::TfToken token("colorSpace");
    return token;
}

// The colorspace of the strongest .mtlx layer (usdMtlx's file format) with an
// opinion on `prim`; empty when no such layer or no document colour space.
inline PXR_NS::TfToken MtlxDocumentColorSpace(PXR_NS::UsdPrim const& prim)
{
    using namespace PXR_NS;
    if (!prim) return {};
    static const TfToken mtlxFormat("mtlx");
    for (SdfPrimSpecHandle const& spec : prim.GetPrimStack()) {
        if (!spec) continue;
        SdfLayerHandle const layer = spec->GetLayer();
        if (!layer || !layer->GetFileFormat() ||
            layer->GetFileFormat()->GetFormatId() != mtlxFormat) continue;
        VtDictionary const data = layer->GetCustomLayerData();
        auto const found = data.find("colorSpace");
        if (found == data.end() || !found->second.IsHolding<std::string>()) return {};
        return TfToken(found->second.UncheckedGet<std::string>());
    }
    return {};
}

// The prim-level data source both loaders publish; null for no colour space.
inline PXR_NS::HdContainerDataSourceHandle MtlxDocumentDataSource(PXR_NS::TfToken const& colorSpace)
{
    using namespace PXR_NS;
    if (colorSpace.IsEmpty()) return nullptr;
    HdDataSourceBaseHandle const value = HdRetainedTypedSampledDataSource<TfToken>::New(colorSpace);
    return HdRetainedContainerDataSource::New(MtlxDocumentColorSpaceToken(), value);
}

// The colour space a material prim's data source carries, if any.
inline PXR_NS::TfToken MtlxDocumentColorSpace(PXR_NS::HdContainerDataSourceHandle const& prim)
{
    using namespace PXR_NS;
    if (!prim) return {};
    auto const document = HdContainerDataSource::Cast(prim->Get(MtlxDocumentToken()));
    if (!document) return {};
    auto const value = HdTypedSampledDataSource<TfToken>::Cast(
        document->Get(MtlxDocumentColorSpaceToken()));
    return value ? value->GetTypedValue(0.0f) : TfToken();
}

// Tags the untagged colour (color3/color4) and file parameters of every node
// with the document's colour space, as MaterialX inherits it: an input's own
// colorspace (USD colorSpace metadata, which usdMtlx authors from it) wins.
// vector3/vector4 values hold the same Gf types; MaterialX applies colour
// spaces to colour-typed inputs only, so their tag has no effect, as the
// inherited document space has none on them. Returns the parameters tagged.
inline size_t ApplyMtlxDocumentColorSpace(PXR_NS::HdMaterialNetworkInterface& net,
                                          PXR_NS::TfToken const& colorSpace)
{
    using namespace PXR_NS;
    if (colorSpace.IsEmpty()) return 0;
    size_t tagged = 0;
    for (TfToken const& node : net.GetNodeNames()) {
        for (TfToken const& name : net.GetAuthoredNodeParameterNames(node)) {
            std::string const& text = name.GetString();
            // Legacy interfaces list metadata as synthetic parameters.
            if (text.starts_with("typeName:") || text.starts_with("colorSpace:")) continue;
            auto data = net.GetNodeParameterData(node, name);
            if (!data.colorSpace.IsEmpty()) continue;
            VtValue const& value = data.value;
            if (!value.IsHolding<GfVec3f>() && !value.IsHolding<GfVec4f>() &&
                !value.IsHolding<SdfAssetPath>()) continue;
            data.colorSpace = colorSpace;
            net.SetNodeParameterData(node, name, data);
            ++tagged;
        }
    }
    return tagged;
}

} // namespace prism::usd
