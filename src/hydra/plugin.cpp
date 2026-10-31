#include "plugin.h"

#include "native_tasks.h"
#include "task_delegate.h"
#include "transport_filter.h"
#include "../renderer/install_paths.h"

#include "pxr/imaging/hd/renderDelegateInfo.h"
#include "pxr/imaging/hd/renderer.h"
#include "pxr/imaging/hd/rendererCreateArgsSchema.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/imaging/hd/retainedDataSource.h"
#include "pxr/imaging/hd/sceneIndexCreateArgsSchema.h"
#include "pxr/imaging/hd/tokens.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdPrismRendererPlugin>();
}

HdRenderDelegate *HdPrismRendererPlugin::CreateRenderDelegate()
{
    // An install path past Windows' 260-character limit breaks material loading
    // (docs/deployment.md, "Path length"): say so once, early and clearly.
    static const bool checked = [] {
        const std::string problem = prism::CheckInstallPathLength(/*includeAll=*/true);
        if (!problem.empty()) TF_RUNTIME_ERROR("%s", problem.c_str());
        return true;
    }();
    (void)checked;
    return new PrismTaskDelegate();
}
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
    auto context = std::make_shared<prism::hydra::TaskContext>();
    // The scene filters (PrismAppendSceneFilters), then the transport filter
    // that admits only tasks, cameras and render buffers to the render index.
    auto transport = PrismTransportFilter::New(PrismAppendSceneFilters(sceneIndex), context);
    // Let OpenUSD allocate the stock adapter.  This keeps HdRenderer and the
    // terminal HdRenderIndex constructor entirely on the OpenUSD side of the
    // DLL boundary while SetTerminalSceneIndex gives our delegate native scene
    // access.
    return _CreateRendererFromRenderDelegate(transport, createArgs);
}

PXR_NAMESPACE_CLOSE_SCOPE
