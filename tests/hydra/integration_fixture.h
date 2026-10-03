#pragma once
#include "../../src/hydra/scene_bridge.h"
#include "../../src/hydra/render_buffer.h"
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/hd/light.h>
#include <pxr/imaging/hd/lightSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/renderThread.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/sdf/assetPath.h>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

namespace prism::hydra::test {
inline void Require(bool condition,const char* reason) {
    if(!condition) throw std::runtime_error(reason);
}
template<class Predicate> void Wait(Predicate ready,const char* reason) {
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(20);
    while(!ready()) {
        if(std::chrono::steady_clock::now()>deadline) throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
inline int64_t Stat(pxr::VtDictionary const& stats,const char* name) {
    auto it=stats.find(name);
    return it!=stats.end() && it->second.IsHolding<int64_t>()
        ? it->second.UncheckedGet<int64_t>() : -1;
}
template<class T> std::vector<T> Read(pxr::PrismRenderBuffer const& buffer) {
    auto view=buffer.Capture();
    std::vector<T> values(view.pixels?view.pixels->size()/sizeof(T):0);
    if(!values.empty()) std::memcpy(values.data(),view.pixels->data(),view.pixels->size());
    return values;
}
inline pxr::HdContainerDataSourceHandle Plane(float offset=0,float opacity=1) {
    using namespace pxr;
    auto points=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(
            {{-.6f,-.6f,0},{.6f,-.6f,0},{.6f,.6f,0},{-.6f,.6f,0}}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex)).Build();
    auto color=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New({GfVec3f(.7f)}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant)).Build();
    auto alpha=HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtFloatArray>::New({opacity}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant)).Build();
    auto primvars=HdRetainedContainerDataSource::New(TfToken("points"),points,
        TfToken("displayColor"),color,TfToken("displayOpacity"),alpha);
    auto topology=HdMeshTopologySchema::Builder().SetFaceVertexCounts(
        HdRetainedTypedSampledDataSource<VtIntArray>::New({4}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New({0,1,2,3})).Build();
    GfMatrix4d matrix(1); matrix.SetTranslate(GfVec3d(offset,0,0));
    auto xform=HdXformSchema::Builder().SetMatrix(
        HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix)).Build();
    return HdRetainedContainerDataSource::New(HdPrimvarsSchemaTokens->primvars,primvars,
        HdMeshSchemaTokens->mesh,HdMeshSchema::Builder().SetTopology(topology).Build(),
        HdXformSchemaTokens->xform,xform);
}
inline pxr::HdContainerDataSourceHandle Dome(std::string path={},float intensity=1,
                                           float rotation=0) {
    using namespace pxr;
    auto light=HdRetainedContainerDataSource::New(HdLightTokens->color,
        HdRetainedTypedSampledDataSource<GfVec3f>::New(GfVec3f(1)),HdLightTokens->intensity,
        HdRetainedTypedSampledDataSource<float>::New(intensity),HdLightTokens->textureFile,
        HdRetainedTypedSampledDataSource<SdfAssetPath>::New(SdfAssetPath(path,path)));
    GfMatrix4d matrix(1); matrix.SetRotate(GfRotation(GfVec3d(0,1,0),rotation));
    auto xform=HdXformSchema::Builder().SetMatrix(
        HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix)).Build();
    return HdRetainedContainerDataSource::New(HdLightSchemaTokens->light,light,
        HdXformSchemaTokens->xform,xform);
}
struct Fixture {
    static constexpr unsigned Size=32;
    pxr::HdRetainedSceneIndexRefPtr scene=pxr::HdRetainedSceneIndex::New();
    pxr::HdRenderThread thread;
    std::unique_ptr<SceneBridge> bridge;
    pxr::PrismRenderBuffer color{pxr::SdfPath("/color"),&thread};
    pxr::PrismRenderBuffer prim{pxr::SdfPath("/prim"),&thread};
    CameraState camera;
    pxr::HdRenderPassAovBindingVector bindings;
    Fixture() {
        using namespace pxr;
        bridge=std::make_unique<SceneBridge>(scene,&thread);
        thread.SetRenderCallback([this]{if(bridge)bridge->Render();}); thread.StartThread();
        color.Allocate(GfVec3i(Size,Size,1),HdFormatFloat32Vec4,false);
        prim.Allocate(GfVec3i(Size,Size,1),HdFormatInt32,false);
        HdRenderPassAovBinding colorBinding,primBinding;
        colorBinding.aovName=HdAovTokens->color; colorBinding.renderBuffer=&color;
        colorBinding.clearValue=VtValue(GfVec4f(0));
        primBinding.aovName=HdAovTokens->primId; primBinding.renderBuffer=&prim;
        primBinding.clearValue=VtValue(-1); bindings={colorBinding,primBinding};
        camera.worldToView.SetTranslate(GfVec3d(0,0,-3));
        GfFrustum frustum; frustum.SetPerspective(40,1,.001,100);
        camera.projection=frustum.ComputeProjectionMatrix();
        auto settings=bridge->GetSettings(); settings.samples=64; settings.maxDepth=4;
        settings.rouletteDepth=32; settings.sampleClamp=0; settings.adaptivePreview=false;
        settings.previewMaxDimension=16; settings.refinementDelayMs=0;
        bridge->SetSettings(settings);
    }
    ~Fixture() { thread.StopRender(); bridge.reset(); thread.StopThread(); }
    void Prepare() {
        const pxr::TfTokenVector tags{pxr::HdRenderTagTokens->geometry};
        Wait([&]{
            bridge->Prepare(camera,bindings,tags,Size,Size);
            auto stats=bridge->GetStats(); auto pending=stats.find("preparationPending");
            return pending!=stats.end() && pending->second.IsHolding<bool>() &&
                !pending->second.UncheckedGet<bool>() &&
                Stat(stats,"displayedGeneration")>=Stat(stats,"preparationGeneration");
        },"Integration fixture preparation timed out");
    }
    void Render() {
        Prepare(); thread.StartRender();
        Wait([&]{return color.IsConverged() && bridge->IsConverged();},
             "Integration fixture render timed out");
        thread.StopRender();
    }
    void Preview() {
        bridge->RestartAccumulation();
        auto settings=bridge->GetSettings(); settings.refinementDelayMs=2000;
        bridge->SetSettings(settings); Prepare();
        const auto before=Stat(bridge->GetStats(),"previewPublishes");
        thread.StartRender();
        Wait([&]{return Stat(bridge->GetStats(),"previewPublishes")>before;},
             "Integration fixture preview timed out");
        thread.StopRender();
        settings.refinementDelayMs=0; bridge->SetSettings(settings);
    }
};
} // namespace prism::hydra::test
