#include "render_pass.h"

#include "render_buffer.h"
#include "scene_bridge.h"

#include "pxr/base/gf/range1f.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/camera.h"
#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/renderThread.h"

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

PrismRenderPass::PrismRenderPass(HdRenderIndex *index,
                                 HdRprimCollection const &collection,
                                 prism::hydra::SceneBridge *bridge,
                                 HdRenderThread *renderThread)
    : HdRenderPass(index, collection), _bridge(bridge), _renderThread(renderThread) {}
PrismRenderPass::~PrismRenderPass() = default;

bool PrismRenderPass::IsConverged() const { return _bridge->IsConverged(); }

void PrismRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                               TfTokenVector const &renderTags)
{
    prism::hydra::CameraState camera;
    camera.worldToView = state->GetWorldToViewMatrix();
    camera.projection = state->GetProjectionMatrix();
    if (HdCamera const *hdCamera = state->GetCamera()) {
        camera.orthographic = hdCamera->GetProjection() == HdCamera::Orthographic;
        GfRange1f const &clip = hdCamera->GetClippingRange();
        camera.nearClip = std::max(0.0f, clip.GetMin());
        camera.farClip = clip.GetMax();
        // UsdGeom cameras author fStop; the legacy focusOn field is not
        // populated by UsdImaging's camera data source and defaults false.
        camera.fStop = hdCamera->GetFStop();
        camera.focalLength = hdCamera->GetFocalLength();
        camera.focusDistance = hdCamera->GetFocusDistance();
    }

    HdRenderPassAovBindingVector bindings = state->GetAovBindings();
    unsigned int width = 0, height = 0;
    for (HdRenderPassAovBinding const &binding : bindings) {
        if (binding.renderBuffer) {
            width = binding.renderBuffer->GetWidth();
            height = binding.renderBuffer->GetHeight();
            if (width && height) break;
        }
    }
    if (!width || !height) {
        GfVec4f const viewport = state->GetViewport();
        width = static_cast<unsigned int>(std::max(0.0f, viewport[2]));
        height = static_cast<unsigned int>(std::max(0.0f, viewport[3]));
    }
    if (_bridge->Prepare(camera, bindings, renderTags, width, height))
        _renderThread->StartRender();
}

PXR_NAMESPACE_CLOSE_SCOPE
