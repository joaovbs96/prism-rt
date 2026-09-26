#include "../../src/hydra/transport_filter.h"
#include "../../src/hydra/selection_settings.h"
#include "../../src/hydra/native_tasks.h"

#include <pxr/base/gf/vec4f.h>
#include <pxr/imaging/hd/legacyTaskFactory.h>
#include <pxr/imaging/hd/legacyTaskSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/colorizeSelectionTask.h>

#include <iostream>
#include <memory>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// transport_filter.cpp calls this factory wrapper when exposing tasks to the
// private control index. Keep this test independent of native_tasks.cpp and
// the renderer bridge: task wrapping itself is intentionally a no-op here.
PXR_NAMESPACE_OPEN_SCOPE
HdLegacyTaskFactorySharedPtr PrismWrapTaskFactory(
    HdLegacyTaskFactorySharedPtr const& original,
    HdSceneIndexBaseRefPtr const&,
    std::shared_ptr<prism::hydra::TaskContext> const&)
{
    return original;
}
PXR_NAMESPACE_CLOSE_SCOPE

namespace {

class NullTaskFactory final : public HdLegacyTaskFactory {
public:
    HdTaskSharedPtr Create(HdSceneDelegate*, const SdfPath&) override
    {
        return nullptr;
    }
};

class RecordingObserver final : public HdSceneIndexObserver {
public:
    void PrimsAdded(const HdSceneIndexBase&,
                    const AddedPrimEntries&) override {}
    void PrimsRemoved(const HdSceneIndexBase&,
                      const RemovedPrimEntries&) override {}
    void PrimsDirtied(const HdSceneIndexBase&,
                      const DirtiedPrimEntries& entries) override
    {
        dirtied.insert(dirtied.end(), entries.begin(), entries.end());
    }
    void PrimsRenamed(const HdSceneIndexBase&,
                      const RenamedPrimEntries&) override {}

    HdSceneIndexObserver::DirtiedPrimEntries dirtied;
};

bool Check(bool condition, const char* message)
{
    if (condition) return true;
    std::cerr << "selection filter test failed: " << message << '\n';
    return false;
}

HdContainerDataSourceHandle MakeTaskPrim(
    const HdLegacyTaskFactorySharedPtr& factory,
    const HdxColorizeSelectionTaskParams& params)
{
    auto task = HdLegacyTaskSchema::Builder()
        .SetFactory(HdRetainedTypedSampledDataSource<
            HdLegacyTaskFactorySharedPtr>::New(factory))
        .SetParameters(HdRetainedTypedSampledDataSource<
            HdxColorizeSelectionTaskParams>::New(params))
        .Build();
    return HdRetainedContainerDataSource::New(
        HdLegacyTaskSchemaTokens->task, task);
}

bool ReadParams(const HdSceneIndexPrim& prim,
                HdxColorizeSelectionTaskParams* output)
{
    auto task = HdLegacyTaskSchema::GetFromParent(prim.dataSource);
    if (!task || !task.GetParameters()) return false;
    const VtValue value = task.GetParameters()->GetValue(0);
    if (!value.IsHolding<HdxColorizeSelectionTaskParams>()) return false;
    *output = value.UncheckedGet<HdxColorizeSelectionTaskParams>();
    return true;
}

} // namespace

int main()
{
    auto input = HdRetainedSceneIndex::New();
    HdxColorizeSelectionTaskParams authored;
    authored.enableSelectionHighlight = true;
    authored.enableLocateHighlight = true;
    authored.selectionColor = GfVec4f(0.12f, 0.34f, 0.56f, 0.78f);
    authored.locateColor = GfVec4f(0.87f, 0.65f, 0.43f, 0.21f);
    authored.enableOutline = false;
    authored.outlineRadius = 9;

    const auto factory = std::make_shared<NullTaskFactory>();
    const auto empty = HdRetainedContainerDataSource::New();
    input->AddPrims({
        {SdfPath("/World"), TfToken("xform"), empty},
        {SdfPath("/World/Camera"), HdPrimTypeTokens->camera, empty},
        {SdfPath("/World/Buffer"), HdPrimTypeTokens->renderBuffer, empty},
        {SdfPath("/World/Task"), TfToken("task"), MakeTaskPrim(factory, authored)},
        {SdfPath("/World/Mesh"), HdPrimTypeTokens->mesh, empty},
        {SdfPath("/World/Material"), HdPrimTypeTokens->material, empty},
        {SdfPath("/World/Instancer"), HdPrimTypeTokens->instancer, empty},
        {SdfPath("/World/Instancer/Child"), HdPrimTypeTokens->mesh, empty},
    });

    auto context = std::make_shared<prism::hydra::TaskContext>();
    auto settings = context->selectionSettings;
    auto filter = PrismTransportFilter::New(input, context);

    if (!Check(filter->GetPrim(SdfPath("/World/Camera")).primType == HdPrimTypeTokens->camera,
               "camera should pass through") ||
        !Check(filter->GetPrim(SdfPath("/World/Buffer")).primType == HdPrimTypeTokens->renderBuffer,
               "render buffer should pass through") ||
        !Check(bool(HdLegacyTaskSchema::GetFromParent(
                   filter->GetPrim(SdfPath("/World/Task")).dataSource)),
               "legacy task should pass through"))
        return 1;

    for (const char* path : {"/World/Mesh", "/World/Material", "/World/Instancer"}) {
        if (!Check(!filter->GetPrim(SdfPath(path)), "geometry/material/instancer should be stripped"))
            return 2;
    }
    const SdfPathVector filteredChildren = filter->GetChildPrimPaths(SdfPath("/World/Instancer"));
    if (!Check(filteredChildren.size() == 1 &&
               filteredChildren.front() == SdfPath("/World/Instancer/Child"),
               "stripped instancer must retain structural child traversal"))
        return 3;

    HdxColorizeSelectionTaskParams visible;
    const auto cachedTask = HdLegacyTaskSchema::GetFromParent(
        filter->GetPrim(SdfPath("/World/Task")).dataSource);
    const auto cachedParameters = cachedTask.GetParameters();
    if (!Check(ReadParams(filter->GetPrim(SdfPath("/World/Task")), &visible),
               "task must expose typed colorize-selection parameters") ||
        !Check(visible.enableOutline && visible.outlineRadius == 2,
               "default Prism settings should override outline and radius") ||
        !Check(visible.enableSelectionHighlight == authored.enableSelectionHighlight &&
               visible.enableLocateHighlight == authored.enableLocateHighlight &&
               visible.selectionColor == authored.selectionColor &&
               visible.locateColor == authored.locateColor,
               "host selection/highlight colors must remain untouched"))
        return 4;

    settings->enableOutline.store(false);
    settings->outlineRadius.store(7);
    settings->revision.fetch_add(1);
    if (!Check(ReadParams(filter->GetPrim(SdfPath("/World/Task")), &visible) &&
               !visible.enableOutline && visible.outlineRadius == 7,
               "filled mode and changed outline radius should be honored") ||
        !Check(cachedParameters &&
               cachedParameters->GetValue(0).IsHolding<HdxColorizeSelectionTaskParams>() &&
               !cachedParameters->GetValue(0)
                    .UncheckedGet<HdxColorizeSelectionTaskParams>().enableOutline &&
               cachedParameters->GetValue(0)
                    .UncheckedGet<HdxColorizeSelectionTaskParams>().outlineRadius == 7,
               "retained parameter data source must reflect live settings"))
        return 5;

    settings->overrideTaskSettings.store(false); // UI's use-host-setting option.
    settings->revision.fetch_add(1);
    if (!Check(ReadParams(filter->GetPrim(SdfPath("/World/Task")), &visible) &&
               visible.enableOutline == authored.enableOutline &&
               visible.outlineRadius == authored.outlineRadius &&
               visible.selectionColor == authored.selectionColor &&
               visible.locateColor == authored.locateColor,
               "use-host-setting should preserve original task settings") ||
        !Check(cachedParameters->GetValue(0)
                    .UncheckedGet<HdxColorizeSelectionTaskParams>().outlineRadius ==
                    authored.outlineRadius,
               "retained parameter data source must restore host settings"))
        return 6;

    std::cout << "Hydra transport filter selection tests passed\n";
    return 0;
}
