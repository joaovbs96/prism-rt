#include "integration_fixture.h"
#include "../../src/hydra/task_delegate.h"
#include <iostream>

int RunCoverageIntegrationTests() {
    using namespace prism::hydra;
    using namespace prism::hydra::test;
    using namespace pxr;
    try {
        PrismTaskDelegate delegate;
        delegate.SetTerminalSceneIndex(HdRetainedSceneIndex::New());
        const TfToken visibility("domeLightCameraVisibility");
        Require(delegate.GetRenderSetting(visibility)==VtValue(true),"Dome visibility default is not true");
        delegate.SetRenderSetting(visibility,VtValue(false));
        Require(delegate.GetRenderSetting(visibility)==VtValue(false),"Standard dome visibility setting ignored");
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render();
            const auto visible=Read<float>(fixture.color);
            for(float component:visible) Require(component==1,"Default visible dome changed RGBA");
            auto settings=fixture.bridge->GetSettings(); settings.domeLightCameraVisibility=false;
            fixture.bridge->SetSettings(settings); fixture.Preview();
            for(float component:Read<float>(fixture.color))
                Require(component==0,"Hidden dome primary miss is not black transparent in preview");
            fixture.Render();
            for(float component:Read<float>(fixture.color))
                Require(component==0,"Hidden dome primary miss is not black transparent at full resolution");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}});
            fixture.Render(); const auto rgba=Read<float>(fixture.color);
            bool fractional=false,opaque=false,empty=false;
            for(size_t pixel=0;pixel<rgba.size()/4;++pixel) {
                const float alpha=rgba[pixel*4+3];
                fractional|=alpha>0 && alpha<1; opaque|=alpha==1; empty|=alpha==0;
                if(alpha==0) Require(rgba[pixel*4]==0 && rgba[pixel*4+1]==0 && rgba[pixel*4+2]==0,
                                     "No-dome escaped RGB is nonzero");
            }
            Require(fractional && opaque && empty,"Silhouette does not accumulate fractional coverage");
        }
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()},
                                     {SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome()}});
            fixture.Render(); const auto visible=Read<float>(fixture.color);
            auto settings=fixture.bridge->GetSettings(); settings.domeLightCameraVisibility=false;
            fixture.bridge->SetSettings(settings); fixture.Render();
            const auto hidden=Read<float>(fixture.color);
            const size_t center=(Fixture::Size/2*Fixture::Size+Fixture::Size/2)*4;
            Require(hidden[center+3]==1 && hidden[center]>0,"Hidden dome no longer lights accepted surfaces");
            for(int channel=0;channel<3;++channel)
                Require(hidden[center+channel]==visible[center+channel],"Dome camera toggle changed secondary lighting");
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane(0,.5f)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype,SdfPath("/Mesh")); fixture.Render();
            const auto cutout=Read<float>(fixture.color);
            Require(cutout[center+3]>.2f && cutout[center+3]<.8f,
                    "Stochastic display-opacity cutout has opaque primary coverage");
        }
        std::cout << "Primary coverage integration tests passed\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
