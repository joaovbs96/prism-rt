#include "render_pass.h"

#include "render_buffer.h"
#include "scene_bridge.h"
#include "pass_camera.h"

#include "pxr/imaging/hd/renderPassState.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/renderThread.h"
#include "pxr/base/tf/diagnostic.h"

PXR_NAMESPACE_OPEN_SCOPE

PrismRenderPass::PrismRenderPass(HdRenderIndex *index,
                                 HdRprimCollection const &collection,
                                 prism::hydra::SceneBridge *bridge,
                                 HdRenderThread *renderThread)
    : HdRenderPass(index, collection), _bridge(bridge), _renderThread(renderThread) {}
PrismRenderPass::~PrismRenderPass() = default;

bool PrismRenderPass::IsConverged() const
{ return !_framingInvalid && _bridge->IsConverged(); }

void PrismRenderPass::_Execute(HdRenderPassStateSharedPtr const &state,
                               TfTokenVector const &renderTags)
{
    auto const camera = prism::hydra::CameraFromPass(*state);
    HdRenderPassAovBindingVector bindings = state->GetAovBindings();
    unsigned int width = 0, height = 0;
    std::vector<GfVec2i> dimensions;
    for (HdRenderPassAovBinding const &binding : bindings) {
        if (auto *buffer = dynamic_cast<PrismRenderBuffer *>(binding.renderBuffer))
            buffer->SetPresentationClearColor(binding.aovName == HdAovTokens->color
                ? binding.clearValue : VtValue());
        if (binding.renderBuffer)
            dimensions.emplace_back(binding.renderBuffer->GetWidth(),
                                    binding.renderBuffer->GetHeight());
    }
    _framingInvalid = !prism::hydra::PassDimensions(*state, dimensions, &width, &height);
    if (_framingInvalid) {
        _renderThread->StopRender();
        if (!_warnedFramingMismatch) {
            TF_WARN("PrismRT: AOV dimensions must match the framing data window; skipping render pass");
            _warnedFramingMismatch = true;
        }
        return;
    }
    if (_bridge->Prepare(camera, bindings, renderTags, width, height))
        _renderThread->StartRender();
}

PXR_NAMESPACE_CLOSE_SCOPE
