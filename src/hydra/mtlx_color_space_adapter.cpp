// Publishes the document colour space of a .mtlx file referenced into a stage
// on its Material prims (src/usd/mtlx_color_space.h). usdMtlx applies
// MaterialXConfigAPI to every Material it translates, so this API schema
// adapter sees each one with its UsdPrim, which the delegate's scene index
// does not otherwise expose.

#include "../usd/mtlx_color_space.h"

#include <pxr/usdImaging/usdImaging/apiSchemaAdapter.h>

PXR_NAMESPACE_OPEN_SCOPE

class PrismMtlxColorSpaceAdapter : public UsdImagingAPISchemaAdapter
{
public:
    HdContainerDataSourceHandle GetImagingSubprimData(
        UsdPrim const& prim, TfToken const& subprim, TfToken const&,
        UsdImagingDataSourceStageGlobals const&) override
    {
        if (!subprim.IsEmpty()) return nullptr;
        const TfToken space = prism::usd::MtlxDocumentColorSpace(prim);
        if (space.IsEmpty()) return nullptr;
        return HdRetainedContainerDataSource::New(prism::usd::MtlxDocumentToken(),
            prism::usd::MtlxDocumentDataSource(space));
    }

    // The colour space is layer metadata of the .mtlx file: changing it
    // reloads the layer, which resyncs the prims.
    HdDataSourceLocatorSet InvalidateImagingSubprim(
        UsdPrim const&, TfToken const&, TfToken const&, TfTokenVector const&,
        UsdImagingPropertyInvalidationType) override
    {
        return {};
    }
};

TF_REGISTRY_FUNCTION(TfType)
{
    using Adapter = PrismMtlxColorSpaceAdapter;
    TfType type = TfType::Define<Adapter, TfType::Bases<UsdImagingAPISchemaAdapter>>();
    type.SetFactory<UsdImagingAPISchemaAdapterFactory<Adapter>>();
}

PXR_NAMESPACE_CLOSE_SCOPE
