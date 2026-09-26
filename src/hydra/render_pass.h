#pragma once

#include "pxr/imaging/hd/renderPass.h"

namespace prism::hydra { class SceneBridge; }
PXR_NAMESPACE_OPEN_SCOPE
class HdRenderThread;

class PrismRenderPass final : public HdRenderPass
{
public:
    PrismRenderPass(HdRenderIndex *index, HdRprimCollection const &collection,
                    prism::hydra::SceneBridge *bridge,
                    HdRenderThread *renderThread);
    ~PrismRenderPass() override;
    bool IsConverged() const override;

protected:
    void _Execute(HdRenderPassStateSharedPtr const &state,
                  TfTokenVector const &renderTags) override;

private:
    prism::hydra::SceneBridge *_bridge;
    HdRenderThread *_renderThread;
};

PXR_NAMESPACE_CLOSE_SCOPE
