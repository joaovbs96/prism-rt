#include "plugin.h"

#include "native_tasks.h"
#include "task_delegate.h"
#include "transport_filter.h"

#include "pxr/imaging/hd/renderDelegateInfo.h"
#include "pxr/imaging/hd/renderer.h"
#include "pxr/imaging/hd/rendererCreateArgsSchema.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexCreateArgsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
#include "pxr/imaging/hdsi/lightLinkingSceneIndex.h"
#include "pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h"
#include "pxr/imaging/hdsi/renderSettingsFilteringSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdPrismRendererPlugin>();
}

HdRenderDelegate *HdPrismRendererPlugin::CreateRenderDelegate()
{ return new PrismTaskDelegate(); }
void HdPrismRendererPlugin::DeleteRenderDelegate(HdRenderDelegate *d) { delete d; }

bool HdPrismRendererPlugin::IsSupported(HdRendererCreateArgsSchema const &,
                                        std::string *) const
{ return true; }

HdContainerDataSourceHandle HdPrismRendererPlugin::GetSceneIndexCreateArgs() const
{
    HdRenderDelegateInfo info;
    info.materialBindingPurpose = HdTokens->full;
    info.materialRenderContexts = {TfToken("mtlx"), TfToken()};
    info.renderSettingsNamespaces = {TfToken("prism")};
    info.isPrimvarFilteringNeeded = false;
    info.isCoordSysSupported = false;
    return HdSceneIndexCreateArgsSchema::Builder()
        .SetMotionBlurSupport(HdRetainedTypedSampledDataSource<bool>::New(false))
        .SetCameraMotionBlurSupport(HdRetainedTypedSampledDataSource<bool>::New(false))
        .SetLegacyRenderDelegateInfo(
            HdRetainedTypedSampledDataSource<HdRenderDelegateInfo>::New(info))
        .Build();
}

std::unique_ptr<HdRenderer> HdPrismRendererPlugin::_CreateRenderer(
    HdSceneIndexBaseRefPtr const &sceneIndex,
    HdRendererCreateArgsSchema const &createArgs)
{
    const auto toMesh = HdRetainedTypedSampledDataSource<TfToken>::New(
        HdsiImplicitSurfaceSceneIndexTokens->toMesh);
    const auto conversion = HdRetainedContainerDataSource::New(
        HdPrimTypeTokens->cube, toMesh,
        HdPrimTypeTokens->cone, toMesh,
        HdPrimTypeTokens->cylinder, toMesh,
        HdPrimTypeTokens->capsule, toMesh,
        HdPrimTypeTokens->plane, toMesh);
    auto context = std::make_shared<prism::hydra::TaskContext>();
    auto converted = HdsiImplicitSurfaceSceneIndex::New(sceneIndex, conversion);
    // Pull CPU-computed primvars as ordinary values before owner-thread
    // snapshot capture severs their live data-source handles.
    auto computed = HdSiExtComputationPrimvarPruningSceneIndex::New(converted);
    // Light/shadow-link collections become categories on lights and geometry.
    auto linked = HdsiLightLinkingSceneIndex::New(computed, nullptr);
    // Render settings prims get their `active` flag (from the scene globals'
    // active render settings path) and only prism:* namespaced settings
    // (docs/hydra.md, "Render settings and products").
    VtArray<TfToken> prefixes{TfToken("prism:")};
    auto settings = HdsiRenderSettingsFilteringSceneIndex::New(linked,
        HdRetainedContainerDataSource::New(
            HdsiRenderSettingsFilteringSceneIndexTokens->namespacePrefixes,
            HdRetainedTypedSampledDataSource<VtArray<TfToken>>::New(prefixes)));
    auto transport = PrismTransportFilter::New(settings, context);
    // Let OpenUSD allocate the stock adapter.  This keeps HdRenderer and the
    // terminal HdRenderIndex constructor entirely on the OpenUSD side of the
    // DLL boundary while SetTerminalSceneIndex gives our delegate native scene
    // access.
    return _CreateRendererFromRenderDelegate(transport, createArgs);
}

PXR_NAMESPACE_CLOSE_SCOPE
