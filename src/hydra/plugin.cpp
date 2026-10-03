#include "plugin.h"

#include "native_tasks.h"
#include "task_delegate.h"
#include "transport_filter.h"

#include "pxr/imaging/hd/renderDelegateInfo.h"
#include "pxr/imaging/hd/renderer.h"
#include "pxr/imaging/hd/rendererCreateArgs.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexInputArgsSchema.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdsi/implicitSurfaceSceneIndex.h"
#include "pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdPrismRendererPlugin>();
}

HdRenderDelegate *HdPrismRendererPlugin::CreateRenderDelegate()
{ return new PrismTaskDelegate(); }
void HdPrismRendererPlugin::DeleteRenderDelegate(HdRenderDelegate *d) { delete d; }

bool HdPrismRendererPlugin::IsSupported(HdContainerDataSourceHandle const &,
                                        std::string *) const
{ return true; }

bool HdPrismRendererPlugin::IsSupported(HdRendererCreateArgs const &,
                                        std::string *) const
{ return true; }

HdContainerDataSourceHandle HdPrismRendererPlugin::GetSceneIndexInputArgs() const
{
    HdRenderDelegateInfo info;
    info.materialBindingPurpose = HdTokens->full;
    info.materialRenderContexts = {TfToken("mtlx"), TfToken()};
    info.renderSettingsNamespaces = {TfToken("prism")};
    info.isPrimvarFilteringNeeded = false;
    info.isCoordSysSupported = false;
    return HdSceneIndexInputArgsSchema::Builder()
        .SetMotionBlurSupport(HdRetainedTypedSampledDataSource<bool>::New(false))
        .SetCameraMotionBlurSupport(HdRetainedTypedSampledDataSource<bool>::New(false))
        .SetLegacyRenderDelegateInfo(
            HdRetainedTypedSampledDataSource<HdRenderDelegateInfo>::New(info))
        .Build();
}

std::unique_ptr<HdRenderer> HdPrismRendererPlugin::_CreateRenderer(
    HdSceneIndexBaseRefPtr const &sceneIndex,
    HdContainerDataSourceHandle const &createArgs)
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
    auto transport = PrismTransportFilter::New(computed, context);
    // Let OpenUSD allocate the stock adapter.  This keeps HdRenderer and the
    // terminal HdRenderIndex constructor entirely on the OpenUSD side of the
    // DLL boundary while SetTerminalSceneIndex gives our delegate native scene
    // access.
    return _CreateRendererFromRenderDelegate(transport, createArgs);
}

PXR_NAMESPACE_CLOSE_SCOPE
