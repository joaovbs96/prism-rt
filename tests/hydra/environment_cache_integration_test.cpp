#include "integration_fixture.h"
#include <OpenImageIO/imagebuf.h>
#include <filesystem>
#include <iostream>

int RunEnvironmentCacheIntegrationTests() {
    using namespace prism::hydra;
    using namespace prism::hydra::test;
    using namespace pxr;
    try {
        const auto path=std::filesystem::absolute("hydra-environment-cache-integration.exr");
        OIIO::ImageBuf image(OIIO::ImageSpec(16,8,3,OIIO::TypeDesc::FLOAT));
        for(int y=0;y<8;++y) for(int x=0;x<16;++x) {
            const float color[3]={.1f+x,.2f+y,.3f+x+y}; image.setpixel(x,y,color);
        }
        Require(image.write(path.string()),"Cannot write environment cache fixture");
        Fixture fixture;
        auto settings=fixture.bridge->GetSettings(); settings.samples=1; fixture.bridge->SetSettings(settings);
        fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome(path.string())}});
        fixture.Render(); const auto initial=fixture.bridge->GetStats();
        Require(Stat(initial,"environmentDistributionBuilds")==1 &&
                Stat(initial,"environmentDistributionCacheHits")>=1,
                "Initial and spare banks rebuilt identical local environment distribution");
        Require(Stat(initial,"sharedEnvironmentImportanceBytes")>0,
                "Cached wrapper omitted shared distribution memory");
        for(float rotation:{90.0f,180.0f,270.0f}) {
            fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,
                                     Dome(path.string(),1,rotation)}});
            fixture.bridge->MarkSceneDirty(SceneDirtyKind::Environment,SdfPath("/Dome")); fixture.Render();
            Require(Stat(fixture.bridge->GetStats(),"environmentDistributionBuilds")==1,
                    "Dome rotation rebuilt local-space distribution");
        }
        fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome(path.string(),2,270)}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Environment,SdfPath("/Dome")); fixture.Render();
        Require(Stat(fixture.bridge->GetStats(),"environmentDistributionBuilds")==2,
                "Exact multiplier change did not rebuild distribution");
        const auto replacement=std::filesystem::absolute("hydra-environment-cache-replacement.exr");
        const float edit[3]={100,200,300}; image.setpixel(0,0,edit);
        Require(image.write(replacement.string()),"Cannot write replacement environment fixture");
        fixture.scene->AddPrims({{SdfPath("/Dome"),HdPrimTypeTokens->domeLight,Dome(replacement.string(),2,270)}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Environment,SdfPath("/Dome")); fixture.Render();
        Require(Stat(fixture.bridge->GetStats(),"environmentDistributionBuilds")==3,
                "Prepared resource change reused old distribution");
        std::cout << "Environment bank/rotation cache integration tests passed\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
