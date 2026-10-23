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
        {
            // usdview's default dome light is an engine prim; it lights a scene
            // without lights and is ignored once the scene has its own.
            const SdfPath engineDome("/_UsdImaging_Test_0/lights/light_1");
            const SdfPath cameraLight("/_UsdImaging_Test_0/lights/light_0");
            const size_t center=(Fixture::Size/2*Fixture::Size+Fixture::Size/2)*4;
            enum Lights { EngineDome=1, StageDome=2, CameraLight=4, StageDistant=8 };
            auto litCenter=[&](int lights) {
                Fixture fixture;
                HdRetainedSceneIndex::AddedPrimEntries prims{{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}};
                if(lights&EngineDome) prims.push_back({engineDome,HdPrimTypeTokens->domeLight,Dome({},1)});
                if(lights&StageDome) prims.push_back({SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome({},3)});
                if(lights&CameraLight) prims.push_back({cameraLight,HdPrimTypeTokens->distantLight,Dome({},3)});
                if(lights&StageDistant) prims.push_back({SdfPath("/Sun"),HdPrimTypeTokens->distantLight,Dome({},3)});
                fixture.scene->AddPrims(prims); fixture.Render();
                auto rgba=Read<float>(fixture.color);
                return std::vector<float>(rgba.begin()+center,rgba.begin()+center+4);
            };
            const auto none=litCenter(0), engineOnly=litCenter(EngineDome);
            Require(none[0]==0,"A scene without lights is lit");
            Require(engineOnly[0]>0,"The default dome does not light a scene without lights");
            const auto stageDome=litCenter(StageDome), stageDomeAndEngine=litCenter(StageDome|EngineDome);
            Require(stageDome[0]>engineOnly[0]*1.5f,"Test stage dome is not brighter than the default dome");
            const auto sun=litCenter(StageDistant), sunAndEngine=litCenter(StageDistant|EngineDome);
            Require(sun[0]>0,"The test scene's own light does not light it");
            for(int channel=0;channel<3;++channel) {
                Require(stageDomeAndEngine[channel]==stageDome[channel],
                        "The default dome adds to the scene's own dome");
                Require(sunAndEngine[channel]==sun[channel],
                        "The default dome adds to the scene's own light");
            }
            // usdview's camera light is not the scene's light: the dome still applies.
            const auto camera=litCenter(CameraLight), cameraAndEngine=litCenter(CameraLight|EngineDome);
            Require(cameraAndEngine[0]>camera[0],"The camera light stops the default dome");
        }
        std::cout << "Primary coverage integration tests passed\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
