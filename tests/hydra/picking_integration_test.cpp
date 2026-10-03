#include "integration_fixture.h"
#include <iostream>

int RunPickingIntegrationTests() {
    using namespace prism::hydra;
    using namespace prism::hydra::test;
    using namespace pxr;
    try {
        {
            Fixture normal;
            normal.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}});
            normal.Render();
            const auto before=Stat(normal.bridge->GetStats(),"reusedSnapshotBuilds");
            for(float offset:{.1f,.2f,.3f}) {
                normal.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane(offset)}});
                normal.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Mesh"));
                normal.Render();
            }
            Require(Stat(normal.bridge->GetStats(),"pinnedSnapshotFallbacks")==0 &&
                    Stat(normal.bridge->GetStats(),"reusedSnapshotBuilds")>=before+3,
                    "Released-frame edit cycles stopped reusing inactive scene banks");
        }
        std::shared_ptr<const PublishedFrame> retained;
        PublishedPickHit oldHit;
        {
            Fixture fixture;
            fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane()}});
            fixture.Preview(); retained=fixture.prim.Capture().frame;
            Require(retained && retained->pickScene,"Preview does not publish an immutable picking scene");
            Require(retained->pickScene->PickPixel(16,16,&oldHit),"Full-resolution preview ray misses visible mesh");
            fixture.Render(); const auto ids=Read<int>(fixture.prim);
            auto full=fixture.prim.Capture().frame;
            for(unsigned y=0;y<Fixture::Size;++y) for(unsigned x=0;x<Fixture::Size;++x) {
                PublishedPickHit hit;
                const bool picked=full->pickScene->PickPixel(x,Fixture::Size-1-y,&hit);
                const int id=ids[y*Fixture::Size+x];
                Require(picked==(id>=0) && (!picked || hit.primId==id),
                        "Published ray IDs differ from settled deterministic primary AOV");
            }
            auto mapped=static_cast<const int*>(fixture.prim.Map());
            const std::vector<int> mappedBefore(mapped,mapped+ids.size());
            const auto before=Stat(fixture.bridge->GetStats(),"pinnedSnapshotFallbacks");
            for(float position:{2.0f,-2.0f}) {
                fixture.scene->AddPrims({{SdfPath("/Mesh"),HdPrimTypeTokens->mesh,Plane(position)}});
                fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement,SdfPath("/Mesh")); fixture.Render();
                PublishedPickHit still;
                Require(retained->pickScene->PickPixel(16,16,&still) &&
                        still.worldPoint==oldHit.worldPoint && still.worldNormal==oldHit.worldNormal,
                        "Later bank edits mutated retained frame geometry");
                Require(std::equal(mappedBefore.begin(),mappedBefore.end(),mapped),
                        "Later publication changed mapped primary pixels");
            }
            Require(Stat(fixture.bridge->GetStats(),"pinnedSnapshotFallbacks")>before,
                    "Externally pinned bank reused mutable geometry");
            fixture.prim.Unmap();
            fixture.camera.worldToView.SetTranslate(GfVec3d(.2,0,-3)); fixture.Render();
            PublishedPickHit still;
            Require(retained->pickScene->PickPixel(16,16,&still) && still.worldPoint==oldHit.worldPoint,
                    "Retained picker uses current camera instead of published camera");
        }
        // The frame/device must remain usable after the bridge, render thread
        // and all buffers have been destroyed; releasing it must also be safe.
        PublishedPickHit afterDestruction;
        Require(retained->pickScene->PickPixel(16,16,&afterDestruction) &&
                afterDestruction.worldPoint==oldHit.worldPoint,
                "Retained picker depends on destroyed bridge/device");
        retained.reset();
        std::cout << "Immutable published picking integration tests passed\n";
        return 0;
    } catch(std::exception const& error) { std::cerr << error.what() << '\n'; return 1; }
}
