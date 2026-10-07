#include "../../src/hydra/task_delegate.h"

#include <pxr/imaging/hd/renderSettingsSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/sceneIndex.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/base/tf/refPtr.h>

#include <atomic>
#include <iostream>
#include <map>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

void Require(bool condition, char const *message)
{
    if (!condition) throw std::runtime_error(message);
}

HdContainerDataSourceHandle SettingsData(int samples)
{
    HdContainerDataSourceHandle const namespaced =
        HdRetainedContainerDataSource::New(
            TfToken("prism:samplesPerPixel"),
            HdRetainedTypedSampledDataSource<int>::New(samples));
    HdContainerDataSourceHandle const settings =
        HdRenderSettingsSchema::Builder()
            .SetNamespacedSettings(namespaced)
            .SetActive(HdRetainedTypedSampledDataSource<bool>::New(true))
            .Build();
    return HdRetainedContainerDataSource::New(
        HdRenderSettingsSchema::GetSchemaToken(), settings);
}

class CountingSceneIndex final : public HdSceneIndexBase
{
public:
    struct PrimRecord {
        TfToken type;
        HdContainerDataSourceHandle data;
    };

    void Add(SdfPath const &path, TfToken const &type,
             HdContainerDataSourceHandle const &data)
    {
        prims[path] = {type, data};
        if (path != SdfPath::AbsoluteRootPath())
            children[path.GetParentPath()].push_back(path);
        children.try_emplace(path);
    }

    HdSceneIndexPrim GetPrim(SdfPath const &path) const override
    {
        getPrimCalls.fetch_add(1, std::memory_order_relaxed);
        auto const found = prims.find(path);
        if (found == prims.end()) return {};
        return {found->second.type, found->second.data};
    }

    SdfPathVector GetChildPrimPaths(SdfPath const &path) const override
    {
        getChildCalls.fetch_add(1, std::memory_order_relaxed);
        auto const found = children.find(path);
        return found == children.end() ? SdfPathVector{} : found->second;
    }

    void ReplaceSettings(SdfPath const &path, int samples)
    {
        prims[path].data = SettingsData(samples);
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        entries.emplace_back(path, HdDataSourceLocatorSet(
            HdRenderSettingsSchema::GetNamespacedSettingsLocator()));
        _SendPrimsDirtied(entries);
    }

    void Dirty(SdfPath const &path, HdDataSourceLocator const &locator)
    {
        HdSceneIndexObserver::DirtiedPrimEntries entries;
        entries.emplace_back(path, HdDataSourceLocatorSet(locator));
        _SendPrimsDirtied(entries);
    }

    mutable std::atomic<size_t> getPrimCalls{0};
    mutable std::atomic<size_t> getChildCalls{0};
    std::map<SdfPath, PrimRecord> prims;
    std::map<SdfPath, SdfPathVector> children;
};

} // namespace

int RunAuthoredSettingsTests()
{
    try {
        auto index = TfCreateRefPtr(new CountingSceneIndex());
        const SdfPath root = SdfPath::AbsoluteRootPath();
        const SdfPath world("/World");
        index->Add(root, TfToken(), HdRetainedContainerDataSource::New());
        index->Add(world, TfToken("scope"), HdRetainedContainerDataSource::New());

        // Deliberately not lexical child order. The old LIFO traversal visits
        // the last settings child first, so /World/M must remain authoritative.
        for (auto const &[name, samples] : {
                 std::pair{"A", 17}, std::pair{"Z", 19}, std::pair{"M", 23}}) {
            const SdfPath path = world.AppendChild(TfToken(name));
            index->Add(path, HdPrimTypeTokens->renderSettings,
                       SettingsData(samples));
        }
        for (int i = 0; i < 40; ++i) {
            const SdfPath parent = world.AppendChild(
                TfToken("Noise" + std::to_string(i)));
            index->Add(parent, TfToken("scope"), HdRetainedContainerDataSource::New());
            for (int child = 0; child < 3; ++child)
                index->Add(parent.AppendChild(TfToken("Child" + std::to_string(child))),
                           TfToken("scope"), HdRetainedContainerDataSource::New());
        }
        index->children[world] = {
            world.AppendChild(TfToken("A")),
            world.AppendChild(TfToken("Z")),
            world.AppendChild(TfToken("M"))};
        for (int i = 0; i < 40; ++i)
            index->children[world].push_back(world.AppendChild(
                TfToken("Noise" + std::to_string(i))));

        PrismTaskDelegate delegate;
        delegate.SetTerminalSceneIndex(index);
        const TfToken hiddenBudget("prism:hiddenGeometryBudgetMiB");
        Require(delegate.GetRenderSetting(hiddenBudget).UncheckedGet<int>() == 0,
                "Hidden geometry budget must default to unlimited");
        delegate.SetRenderSetting(hiddenBudget, VtValue(12));
        Require(delegate.GetRenderSetting(hiddenBudget).UncheckedGet<int>() == 12,
                "Hidden geometry budget was not applied");
        delegate.SetRenderSetting(hiddenBudget, VtValue(-1));
        delegate.SetRenderSetting(hiddenBudget, VtValue(2.0f));
        delegate.SetRenderSetting(hiddenBudget, VtValue(std::string("2")));
        Require(delegate.GetRenderSetting(hiddenBudget).UncheckedGet<int>() == 12,
                "Invalid hidden geometry budget replaced the previous value");
        delegate.SetRenderSetting(hiddenBudget, VtValue(0));
        delegate.Update();
        Require(delegate.GetRenderSetting(TfToken("prism:samplesPerPixel"))
                    .UncheckedGet<int>() == 23,
                "Cached settings order changed the prior reverse-child precedence");

        index->ReplaceSettings(world.AppendChild(TfToken("M")), 31);
        const size_t childrenBeforeSync =
            index->getChildCalls.load(std::memory_order_relaxed);
        const size_t primsBeforeSync =
            index->getPrimCalls.load(std::memory_order_relaxed);
        delegate.Update();
        const size_t primQueries = index->getPrimCalls.load(
            std::memory_order_relaxed) - primsBeforeSync;
        Require(index->getChildCalls.load(std::memory_order_relaxed) ==
                    childrenBeforeSync,
                "Settings sync traversed scene-index children");
        Require(primQueries == 3,
                "Settings sync queried more than the cached settings prims");
        Require(delegate.GetRenderSetting(TfToken("prism:samplesPerPixel"))
                    .UncheckedGet<int>() == 31,
                "Dirty active settings value was not synchronized");

        auto dirtyIndex = TfCreateRefPtr(new CountingSceneIndex());
        dirtyIndex->Add(root, TfToken(), HdRetainedContainerDataSource::New());
        dirtyIndex->Add(SdfPath("/Mesh"), HdPrimTypeTokens->mesh,
                        HdRetainedContainerDataSource::New());
        dirtyIndex->Add(SdfPath("/Mesh/Subset"), HdPrimTypeTokens->geomSubset,
                        HdRetainedContainerDataSource::New());
        PrismTaskDelegate dirtyDelegate;
        dirtyDelegate.SetTerminalSceneIndex(dirtyIndex);
        auto stat = [](VtDictionary const &stats, char const *name) {
            auto const found = stats.find(name);
            return found != stats.end() && found->second.IsHolding<int64_t>()
                ? found->second.UncheckedGet<int64_t>() : int64_t{0};
        };
        VtDictionary const beforeSubset = dirtyDelegate.GetRenderStats();
        dirtyIndex->Dirty(SdfPath("/Mesh/Subset"), HdDataSourceLocator(
            {TfToken("geomSubset"), TfToken("indices")}));
        VtDictionary const afterSubset = dirtyDelegate.GetRenderStats();
        Require(stat(afterSubset, "prototypeInvalidations") ==
                    stat(beforeSubset, "prototypeInvalidations") + 1,
                "geomSubset indices dirtiness was not mapped to prototype");
        dirtyIndex->Dirty(SdfPath("/Mesh/Subset"), HdDataSourceLocator(
            {TfToken("geomSubset"), TfToken("elementType")}));
        VtDictionary const afterElementType = dirtyDelegate.GetRenderStats();
        Require(stat(afterElementType, "prototypeInvalidations") ==
                    stat(afterSubset, "prototypeInvalidations") + 1,
                "geomSubset elementType dirtiness was not mapped to prototype");
        dirtyIndex->Dirty(SdfPath("/Mesh/Subset"),
            HdMaterialBindingsSchema::GetDefaultLocator());
        VtDictionary const afterBinding = dirtyDelegate.GetRenderStats();
        Require(stat(afterBinding, "materialInvalidations") ==
                    stat(afterSubset, "materialInvalidations") + 1,
                "root materialBindings dirtiness on geomSubset was not mapped to material");

        VtDictionary const beforeIgnored = dirtyDelegate.GetRenderStats();
        dirtyIndex->Dirty(SdfPath("/Mesh"), HdDataSourceLocator(
            {TfToken("extent")}));
        dirtyIndex->Dirty(SdfPath("/Mesh"), HdDataSourceLocator(
            {TfToken("displayStyle")}));
        VtDictionary const afterIgnored = dirtyDelegate.GetRenderStats();
        Require(stat(afterIgnored, "materialInvalidations") ==
                    stat(beforeIgnored, "materialInvalidations") &&
                stat(afterIgnored, "placementInvalidations") ==
                    stat(beforeIgnored, "placementInvalidations") &&
                stat(afterIgnored, "prototypeInvalidations") ==
                    stat(beforeIgnored, "prototypeInvalidations") &&
                stat(afterIgnored, "conservativeRebuilds") ==
                    stat(beforeIgnored, "conservativeRebuilds"),
                "extent/displayStyle dirtiness triggered snapshot invalidation");

        dirtyIndex->Dirty(SdfPath("/Mesh"), HdDataSourceLocator());
        VtDictionary const afterEmpty = dirtyDelegate.GetRenderStats();
        Require(stat(afterEmpty, "conservativeRebuilds") ==
                    stat(afterIgnored, "conservativeRebuilds") + 1,
                "empty dirty locator set did not retain conservative fallback");
    } catch (std::exception const &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
