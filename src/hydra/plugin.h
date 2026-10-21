#pragma once

#include "pxr/imaging/hd/rendererPlugin.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdPrismRendererPlugin final : public HdRendererPlugin
{
public:
    HdRenderDelegate *CreateRenderDelegate() override;
    void DeleteRenderDelegate(HdRenderDelegate *delegate) override;
    bool IsSupported(HdRendererCreateArgsSchema const &,
                     std::string *reasonWhyNot = nullptr) const override;
    HdContainerDataSourceHandle GetSceneIndexCreateArgs() const override;

protected:
    std::unique_ptr<HdRenderer> _CreateRenderer(
        HdSceneIndexBaseRefPtr const &sceneIndex,
        HdRendererCreateArgsSchema const &createArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
