#pragma once

#include "pxr/imaging/hd/rendererPlugin.h"

PXR_NAMESPACE_OPEN_SCOPE

class HdPrismRendererPlugin final : public HdRendererPlugin
{
public:
    HdRenderDelegate *CreateRenderDelegate() override;
    void DeleteRenderDelegate(HdRenderDelegate *delegate) override;
    bool IsSupported(HdContainerDataSourceHandle const &,
                     std::string *reasonWhyNot = nullptr) const override;
    bool IsSupported(HdRendererCreateArgs const &,
                     std::string *reasonWhyNot = nullptr) const override;
    HdContainerDataSourceHandle GetSceneIndexInputArgs() const override;

protected:
    std::unique_ptr<HdRenderer> _CreateRenderer(
        HdSceneIndexBaseRefPtr const &sceneIndex,
        HdContainerDataSourceHandle const &createArgs) override;
};

PXR_NAMESPACE_CLOSE_SCOPE
