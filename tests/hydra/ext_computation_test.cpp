#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/imaging/hd/extComputationContext.h>
#include <pxr/imaging/hd/extComputationCpuCallback.h>
#include <pxr/imaging/hd/extComputationOutputSchema.h>
#include <pxr/imaging/hd/extComputationPrimvarSchema.h>
#include <pxr/imaging/hd/extComputationPrimvarsSchema.h>
#include <pxr/imaging/hd/extComputationSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdsi/extComputationPrimvarPruningSceneIndex.h>
#include <pxr/imaging/hdsi/implicitSurfaceSceneIndex.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>
#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
const TfToken inputName("inputPoints"), outputName("deformedPoints");
class TranslatePoints final : public HdExtComputationCpuCallback {
public:
    int calls = 0;
    void Compute(HdExtComputationContext *context) override {
        ++calls;
        auto points = context->GetTypedInputValue<VtVec3fArray>(inputName);
        for (auto &point : points) point += GfVec3f(2, 0, 0);
        context->SetOutputValue(outputName, VtValue(points));
    }
};
}

int main(int argc, char **argv) {
    auto source = HdRetainedSceneIndex::New();
    auto callback = std::make_shared<TranslatePoints>();
    const SdfPath computationPath("/Compute"), meshPath("/Mesh");
    auto tupleType = HdRetainedTypedSampledDataSource<HdTupleType>::New(
        HdTupleType{HdTypeFloatVec3, 1});
    auto computation = [&](VtVec3fArray const &points) {
        return HdRetainedContainerDataSource::New(
            HdExtComputationSchemaTokens->extComputation,
            HdExtComputationSchema::Builder()
                .SetInputValues(HdRetainedContainerDataSource::New(inputName,
                    HdRetainedTypedSampledDataSource<VtVec3fArray>::New(points)))
                .SetOutputs(HdRetainedContainerDataSource::New(outputName,
                    HdExtComputationOutputSchema::Builder()
                        .SetValueType(tupleType).Build()))
                .SetCpuCallback(HdRetainedTypedSampledDataSource<
                    HdExtComputationCpuCallbackSharedPtr>::New(callback))
                .Build());
    };
    auto computedPoints = HdExtComputationPrimvarSchema::Builder()
        .SetInterpolation(HdRetainedTypedSampledDataSource<TfToken>::New(
            HdPrimvarSchemaTokens->vertex))
        .SetRole(HdRetainedTypedSampledDataSource<TfToken>::New(
            HdPrimvarSchemaTokens->point))
        .SetSourceComputation(HdRetainedTypedSampledDataSource<SdfPath>::New(
            computationPath))
        .SetSourceComputationOutputName(
            HdRetainedTypedSampledDataSource<TfToken>::New(outputName))
        .SetValueType(tupleType).Build();
    source->AddPrims({
        {computationPath, HdPrimTypeTokens->extComputation,
            computation(VtVec3fArray{GfVec3f(0,0,0), GfVec3f(1,0,0), GfVec3f(0,1,0)})},
        {meshPath, HdPrimTypeTokens->mesh,
            HdRetainedContainerDataSource::New(
                HdExtComputationPrimvarsSchemaTokens->extComputationPrimvars,
                HdRetainedContainerDataSource::New(HdTokens->points, computedPoints))}});
    auto filter = HdSiExtComputationPrimvarPruningSceneIndex::New(source);
    auto readPoints = [&]() {
        auto prim = filter->GetPrim(meshPath);
        return HdPrimvarsSchema::GetFromParent(prim.dataSource)
            .GetPrimvar(HdTokens->points).GetPrimvarValue()->GetValue(0.0f);
    };
    if (callback->calls != 0 ||
        HdPrimvarsSchema::GetFromParent(source->GetPrim(meshPath).dataSource)) {
        std::cerr << "Computed points unexpectedly existed as authored values\n";
        return 1;
    }
    const VtValue first = readPoints();
    if (!first.IsHolding<VtVec3fArray>() || callback->calls == 0 ||
        first.UncheckedGet<VtVec3fArray>()[0] != GfVec3f(2,0,0)) {
        std::cerr << "CPU computation did not produce ordinary point values\n";
        return 1;
    }
    source->AddPrims({{computationPath, HdPrimTypeTokens->extComputation,
        computation(VtVec3fArray{GfVec3f(1,2,3)})}});
    const VtValue second = readPoints();
    if (!second.IsHolding<VtVec3fArray>() ||
        second.UncheckedGet<VtVec3fArray>()[0] != GfVec3f(3,2,3) ||
        first.UncheckedGet<VtVec3fArray>()[0] != GfVec3f(2,0,0)) {
        std::cerr << "Changed computation reused stale values or mutated capture\n";
        return 1;
    }
    if (argc != 2) {
        std::cerr << "Expected skinned USD fixture path\n";
        return 1;
    }
    auto stage = UsdStage::Open(argv[1]);
    if (!stage) return 1;
    UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    auto indices = UsdImagingCreateSceneIndices(info);
    auto converted = HdsiImplicitSurfaceSceneIndex::New(
        indices.finalSceneIndex, HdRetainedContainerDataSource::New());
    auto resolved = HdSiExtComputationPrimvarPruningSceneIndex::New(converted);
    for (int frame : {1, 2}) {
        indices.stageSceneIndex->SetTime(UsdTimeCode(frame));
        indices.stageSceneIndex->ApplyPendingUpdates();
        indices.postInstancingNoticeBatchingSceneIndex->Flush();
        auto prim = resolved->GetPrim(SdfPath("/World/Panel"));
        auto value = HdPrimvarsSchema::GetFromParent(prim.dataSource)
            .GetPrimvar(HdTokens->points).GetPrimvarValue();
        if (!value) {
            std::cerr << "Skinned mesh did not expose point values\n";
            return 1;
        }
        auto points = value->GetValue(0.0f);
        const GfVec3f expected(frame == 1 ? 0.0f : -1.0f, -0.5f, 0.0f);
        if (!points.IsHolding<VtVec3fArray>() ||
            points.UncheckedGet<VtVec3fArray>().size() != 3 ||
            (points.UncheckedGet<VtVec3fArray>()[0] - expected).GetLength() > 1e-6f) {
            std::cerr << "Skinned points mismatch at frame " << frame << '\n';
            return 1;
        }
    }
    return 0;
}
