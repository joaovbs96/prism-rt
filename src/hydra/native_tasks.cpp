#include "native_tasks.h"
#include "scene_bridge.h"
#include "render_buffer.h"
#include "selection_settings.h"
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/selection.h>
#include <pxr/imaging/hd/selectionsSchema.h>
#include <pxr/imaging/hd/selectionSchema.h>
#include <pxr/imaging/hd/instanceIndicesSchema.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/colorizeSelectionTask.h>
#include <pxr/imaging/hdx/pickFromRenderBufferTask.h>
#include <pxr/imaging/hdx/selectionTracker.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <map>
#include <mutex>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

class TaskTimer final {
public:
    TaskTimer(std::atomic<uint64_t> *nanoseconds,
              std::atomic<uint64_t> *calls)
        : nanoseconds(nanoseconds), calls(calls),
          start(std::chrono::steady_clock::now()) {}
    ~TaskTimer()
    {
        nanoseconds->fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count()),
            std::memory_order_relaxed);
        calls->fetch_add(1, std::memory_order_relaxed);
    }
private:
    std::atomic<uint64_t> *nanoseconds;
    std::atomic<uint64_t> *calls;
    std::chrono::steady_clock::time_point start;
};

void AppendInstanceSelection(
    std::vector<prism::hydra::PickingTable::InstanceLevel> const &chain,
    HdSelectionSchema const& selection, std::set<int>* selected)
{
    std::map<SdfPath,std::set<int>> constraints;
    const auto nested = selection.GetNestedInstanceIndices();
    for (size_t i=0;i<nested.GetNumElements();++i) {
        const auto level=nested.GetElement(i);
        if (!level.GetInstancer() || !level.GetInstanceIndices()) return;
        const auto values=level.GetInstanceIndices()->GetTypedValue(0);
        constraints[level.GetInstancer()->GetTypedValue(0)]={values.begin(),values.end()};
    }
    if (chain.empty()) return;
    auto translated = prism::hydra::TranslatePickingInstanceSelection(
        chain, constraints, false);
    selected->insert(translated.begin(), translated.end());
}

void AppendUsdviewSelection(
    std::vector<prism::hydra::PickingTable::InstanceLevel> const &chain,
    SdfPath const &path,
    std::map<SdfPath, std::set<int>> const& selections,
    std::set<int>* selected,
    bool* fullySelected,
    bool* constrainedByIndexedSelection,
    bool* constrainedByWholeSelection)
{
    if (selections.empty()) return;
    // A whole authored prim selection applies to renderable descendants.
    for (const auto& [selectionPath, indices] : selections) {
        if (indices.count(-1) && path.HasPrefix(selectionPath)) {
            *constrainedByWholeSelection = true;
            *fullySelected = true;
            return;
        }
    }
    if (chain.empty()) return;

    bool constrained = false;
    for (auto const &level : chain) {
        const auto constraint = selections.find(level.instancerPath);
        constrained = constrained || constraint != selections.end();
        if (constraint != selections.end()) {
            if (constraint->second.count(-1))
                *constrainedByWholeSelection = true;
            else
                *constrainedByIndexedSelection = true;
        }
    }
    if (constrained) {
        auto translated = prism::hydra::TranslatePickingInstanceSelection(
            chain, selections, true);
        selected->insert(translated.begin(), translated.end());
    }
}

// Hdx's compositor is reusable; only its legacy Rprim-ID encoding is replaced.
class NativeSelectionTracker final : public HdxSelectionTracker {
public:
    std::shared_ptr<const prism::hydra::PublishedFrame> frame;
    NativeSelectionTracker(HdSceneIndexBaseRefPtr scene,
                           std::shared_ptr<prism::hydra::TaskContext> context)
        : scene(std::move(scene)), context(std::move(context)) {}
    void SetInputs(
        std::shared_ptr<const prism::hydra::PublishedFrame> nextFrame,
        HdxSelectionTrackerSharedPtr const &source,
        bool selectEnabled, bool locateEnabled)
    {
        std::lock_guard lock(cacheMutex);
        const uint64_t sourceVersion = source ? source->GetVersion() : 0;
        const uint64_t sceneRevision =
            context->selectionSceneRevision.load(std::memory_order_relaxed);
        const uint64_t settingsRevision =
            context->selectionSettings->revision.load(std::memory_order_relaxed);
        const uint64_t topologyRevision =
            context->instanceTopologyRevision.load(std::memory_order_relaxed);
        auto const nextPicking = nextFrame ? nextFrame->picking : nullptr;
        const bool pickingChanged = nextPicking != picking;
        const bool changed = pickingChanged || source != sourceTracker ||
            sourceVersion != sourceTrackerVersion ||
            sceneRevision != cachedSceneRevision ||
            settingsRevision != cachedSettingsRevision ||
            selectEnabled != cachedSelectEnabled ||
            locateEnabled != cachedLocateEnabled;
        frame = std::move(nextFrame);
        if (pickingChanged) pickingTopologyRevision = topologyRevision;
        topologyStale = topologyRevision != pickingTopologyRevision;
        if (!changed) return;
        picking = std::move(nextPicking);
        sourceTracker = source;
        sourceTrackerVersion = sourceVersion;
        cachedSceneRevision = sceneRevision;
        cachedSettingsRevision = settingsRevision;
        cachedSelectEnabled = selectEnabled;
        cachedLocateEnabled = locateEnabled;
        SetSelection(source ? source->GetSelectionMap() : HdSelectionSharedPtr());
        for (CacheEntry &entry : cache) entry.valid = false;
        _IncrementVersion();
    }
    void UpdateSelection(HdRenderIndex*) override {}
    bool GetSelectionOffsetBuffer(HdRenderIndex const*, bool select, bool locate,
                                  VtIntArray* output) const override {
        TaskTimer timer(&context->selectionOffsetNanoseconds,
                        &context->selectionOffsetCalls);
        std::lock_guard lock(cacheMutex);
        const size_t cacheIndex = (select ? 1u : 0u) | (locate ? 2u : 0u);
        CacheEntry &cached = cache[cacheIndex];
        if (cached.valid) {
            context->selectionOffsetCacheHits.fetch_add(
                1, std::memory_order_relaxed);
            *output = cached.data;
            return cached.any;
        }
        context->selectionOffsetCacheMisses.fetch_add(
            1, std::memory_order_relaxed);
        // Hdx invokes this from the render-index task thread. Cache misses may
        // traverse live selection and instancer data here; no background
        // snapshot worker calls this tracker.
        prism::hydra::SceneBridge * const bridge = context->bridge.load();
        if (!bridge) {
            output->clear();
            return false;
        }
        const std::vector<SdfPath> paths = frame && frame->picking
            ? frame->picking->primPaths : std::vector<SdfPath>{};
        context->selectionPathsVisited.fetch_add(
            static_cast<uint64_t>(paths.size()) *
                static_cast<uint64_t>(select) +
            static_cast<uint64_t>(paths.size()) *
                static_cast<uint64_t>(locate),
            std::memory_order_relaxed);
        std::vector<int> data{2,0,0};
        bool any = false;
        const auto legacy = GetSelectionMap();
        bool usdviewActive = false;
        std::map<SdfPath, std::set<int>> usdviewSelection;
        {
            const auto& settings = context->selectionSettings;
            std::lock_guard<std::mutex> lock(settings->usdviewMutex);
            usdviewActive = settings->usdviewSelectionActive;
            usdviewSelection = settings->usdviewSelection;
        }
        const bool usdviewHasWholeSelection = std::any_of(
            usdviewSelection.begin(), usdviewSelection.end(),
            [](const auto& entry) { return entry.second.count(-1) != 0; });
        for (int mode = 0; mode < 2; ++mode) {
            if (!(mode == 0 ? select : locate) || paths.empty()) continue;
            const size_t table = data.size();
            data[mode+1] = int(table);
            data.push_back(0); data.push_back(int(paths.size()));
            data.resize(data.size()+paths.size(),0);
            for (size_t id = 0; id < paths.size(); ++id) {
                static const std::vector<
                    prism::hydra::PickingTable::InstanceLevel> emptyTopology;
                auto const &instanceTopology = frame && frame->picking &&
                    id < frame->picking->instanceTopologies.size()
                    ? frame->picking->instanceTopologies[id] : emptyTopology;
                bool full = false;
                bool constrainedByIndexedSelection = false;
                bool constrainedByWholeSelection = false;
                std::set<int> instances, elements;
                if (legacy && !(mode == 0 && usdviewActive)) {
                    if (auto state = legacy->GetPrimSelectionState(HdSelection::HighlightMode(mode), paths[id])) {
                        full = state->fullySelected;
                        for (auto const& a : state->instanceIndices) instances.insert(a.begin(),a.end());
                        for (auto const& a : state->elementIndices) elements.insert(a.begin(),a.end());
                    }
                }
                if (mode == 0) {
                    if (usdviewActive) {
                        AppendUsdviewSelection(
                            instanceTopology, paths[id], usdviewSelection,
                            &instances, &full,
                            &constrainedByIndexedSelection,
                            &constrainedByWholeSelection);
                    }
                    if (!usdviewActive || constrainedByWholeSelection ||
                        (!constrainedByIndexedSelection &&
                         usdviewHasWholeSelection)) {
                    // Mesh-conversion filters create renderable descendants
                    // below the authored selected prim.  Use the nearest
                    // ancestor carrying selection data, then stop: continuing
                    // to a fully selected point-instancer ancestor would mark
                    // every prototype and instance selected.
                    for (auto selectionPath = paths[id];
                         !selectionPath.IsEmpty();
                         selectionPath = selectionPath.GetParentPath()) {
                        auto selections = HdSelectionsSchema::GetFromParent(
                            scene->GetPrim(selectionPath).dataSource);
                        if (selections.GetNumElements() == 0) continue;
                        for (size_t j = 0; j < selections.GetNumElements(); ++j) {
                            auto selection = selections.GetElement(j);
                            auto nested = selection.GetNestedInstanceIndices();
                            if (nested.GetNumElements()) {
                                if (!topologyStale)
                                    AppendInstanceSelection(
                                        instanceTopology, selection, &instances);
                            } else if (auto selected = selection.GetFullySelected()) {
                                full = full || selected->GetTypedValue(0);
                            }
                        }
                        break;
                    }
                    }
                }
                const size_t slot = table+2+id;
                if (full) { data[slot] = 1; any = true; continue; }
                // Hdx selection encoding: low bit = selected, high bits = next table.
                auto append = [&](const std::set<int>& indices, int type) -> int {
                    if (indices.empty() || *indices.begin() < 0) return 0;
                    const int first = *indices.begin(), last = *indices.rbegin()+1;
                    if (last-first > 10000000) return 0;
                    const int offset = int(data.size());
                    data.insert(data.end(),{type,first,last});
                    data.resize(data.size()+size_t(last-first),0);
                    for (int i : indices) data[offset+3+i-first] = 1;
                    return offset;
                };
                const int elementTable = append(elements,0);
                // HdxColorizeSelectionTask's CPU decoder uses 3 for INSTANCE
                // in OpenUSD 26.05 (the GPU shader's newer enum uses 4).
                const int instanceTable = append(instances,3);
                if (instanceTable) {
                    if (elementTable) for (int i : instances)
                        data[instanceTable+3+i-*instances.begin()] = elementTable << 1;
                    data[slot] = instanceTable << 1;
                } else if (elementTable) data[slot] = elementTable << 1;
                any = any || data[slot] != 0;
            }
        }
        data.resize(std::max(size_t(8),data.size()),0);
        cached.data = VtIntArray(data.begin(),data.end());
        cached.any = any;
        cached.valid = true;
        *output = cached.data;
        return cached.any;
    }
private:
    struct CacheEntry {
        bool valid = false;
        bool any = false;
        VtIntArray data;
    };
    HdSceneIndexBaseRefPtr scene;
    std::shared_ptr<prism::hydra::TaskContext> context;
    mutable std::mutex cacheMutex;
    mutable std::array<CacheEntry, 4> cache;
    std::shared_ptr<const prism::hydra::PickingTable> picking;
    HdxSelectionTrackerSharedPtr sourceTracker;
    uint64_t sourceTrackerVersion = 0;
    uint64_t cachedSceneRevision = 0;
    uint64_t cachedSettingsRevision = 0;
    bool cachedSelectEnabled = false;
    bool cachedLocateEnabled = false;
    uint64_t pickingTopologyRevision = 0;
    bool topologyStale = false;
};

class NativeSelectionTask final : public HdxColorizeSelectionTask {
public:
    NativeSelectionTask(HdSceneDelegate* delegate, SdfPath const& id,
                        HdSceneIndexBaseRefPtr const& scene,
                        std::shared_ptr<prism::hydra::TaskContext> const& context)
        : HdxColorizeSelectionTask(delegate,id),
          tracker(std::make_shared<NativeSelectionTracker>(scene,context)),
          taskContext(context), sceneDelegate(delegate) {}
    ~NativeSelectionTask() override { Unpin(); }
    void Execute(HdTaskContext* context) override {
        HdxColorizeSelectionTask::Execute(context);
        Unpin();
    }
    bool IsConverged() const override {
        return (!primId || primId->IsConverged()) &&
               (!instanceId || instanceId->IsConverged()) &&
               (!elementId || elementId->IsConverged());
    }
    void Prepare(HdTaskContext* context, HdRenderIndex* index) override {
        Unpin();
        std::shared_ptr<const prism::hydra::PublishedFrame> selectedFrame;
        // Pin one coherent published frame through all Hdx Execute calls.
        // Nested Map calls made by Hdx upload/selection see these same pixels,
        // while render workers can continue publishing later generations.
        if(auto* bridge=taskContext->bridge.load()) {
            for(int attempt=0;attempt<2;++attempt) {
                pinned=bridge->GetRenderBuffers();
                bool consistent=!pinned.empty();
                std::shared_ptr<const prism::hydra::PublishedFrame> frame;
                for(auto* buffer:pinned) buffer->Map();
                for(auto* buffer:pinned) {
                    const auto captured=buffer->Capture();
                    if(!frame) frame=captured.frame;
                    if(!captured.frame || captured.frame!=frame) consistent=false;
                }
                if(consistent) { selectedFrame=std::move(frame); break; }
                Unpin();
            }
        }
        // HdRenderDelegate::Update runs from inside SyncAll, where emitting a
        // scene-index dirty notice is forbidden.  Refresh these lightweight
        // presentation parameters directly before Prepare instead.  The
        // filter's sampled task data source reads the same shared settings.
        const uint64_t revision = taskContext->selectionSettings->revision.load();
        if (revision != selectionRevision) {
            HdDirtyBits dirty = HdChangeTracker::DirtyParams;
            _GetTaskParams(sceneDelegate, &params);
            HdxColorizeSelectionTask::_Sync(sceneDelegate, context, &dirty);
            selectionRevision = revision;
        }
        auto it = context->find(HdxTokens->selectionState);
        const bool hadOriginal = it != context->end();
        VtValue original = hadOriginal ? it->second : VtValue();
        HdxSelectionTrackerSharedPtr sourceTracker;
        if (original.IsHolding<HdxSelectionTrackerSharedPtr>())
            sourceTracker = original.UncheckedGet<HdxSelectionTrackerSharedPtr>();
        tracker->SetInputs(std::move(selectedFrame), sourceTracker,
                           params.enableSelectionHighlight,
                           params.enableLocateHighlight);
        (*context)[HdxTokens->selectionState] = VtValue(HdxSelectionTrackerSharedPtr(tracker));
        HdxColorizeSelectionTask::Prepare(context,index);
        primId = dynamic_cast<HdRenderBuffer*>(index->GetBprim(
            HdPrimTypeTokens->renderBuffer, params.primIdBufferPath));
        instanceId = dynamic_cast<HdRenderBuffer*>(index->GetBprim(
            HdPrimTypeTokens->renderBuffer, params.instanceIdBufferPath));
        elementId = dynamic_cast<HdRenderBuffer*>(index->GetBprim(
            HdPrimTypeTokens->renderBuffer, params.elementIdBufferPath));
        if (hadOriginal) (*context)[HdxTokens->selectionState] = original;
        else context->erase(HdxTokens->selectionState);
    }
protected:
    void _Sync(HdSceneDelegate* delegate, HdTaskContext* context,
               HdDirtyBits* dirty) override {
        const bool paramsDirty = (*dirty & HdChangeTracker::DirtyParams) != 0;
        if (paramsDirty)
            _GetTaskParams(delegate, &params);
        HdxColorizeSelectionTask::_Sync(delegate, context, dirty);
        if (paramsDirty)
            selectionRevision = taskContext->selectionSettings->revision.load();
    }
private:
    void Unpin() {
        for(auto* buffer:pinned) buffer->Unmap();
        pinned.clear();
    }
    std::vector<PrismRenderBuffer*> pinned;
    std::shared_ptr<NativeSelectionTracker> tracker;
    std::shared_ptr<prism::hydra::TaskContext> taskContext;
    HdSceneDelegate* sceneDelegate;
    uint64_t selectionRevision = std::numeric_limits<uint64_t>::max();
    HdxColorizeSelectionTaskParams params;
    HdRenderBuffer *primId = nullptr;
    HdRenderBuffer *instanceId = nullptr;
    HdRenderBuffer *elementId = nullptr;
};

PrismRenderBuffer::ReadView CaptureBuffer(HdRenderBuffer* buffer) {
    auto* native=dynamic_cast<PrismRenderBuffer*>(buffer);
    return native ? native->Capture() : PrismRenderBuffer::ReadView{};
}
template<class T> std::vector<T> ReadBuffer(PrismRenderBuffer::ReadView const& view, HdFormat format) {
    if(view.format!=format || !view.pixels) return {};
    const size_t count=size_t(view.width)*view.height*HdGetComponentCount(format);
    if(view.pixels->size()<count*sizeof(T)) return {};
    const T* data=reinterpret_cast<const T*>(view.pixels->data());
    return std::vector<T>(data,data+count);
}

class NativePickTask final : public HdxTask {
public:
    NativePickTask(HdSceneDelegate* delegate,SdfPath const& id,
                   std::shared_ptr<prism::hydra::TaskContext> context)
        : HdxTask(id), context(std::move(context)) {}
    bool IsConverged() const override { return true; }
    void Prepare(HdTaskContext*,HdRenderIndex* index) override {
        auto get = [&](SdfPath const& p) { return dynamic_cast<HdRenderBuffer*>(index->GetBprim(HdPrimTypeTokens->renderBuffer,p)); };
        prim = get(params.primIdBufferPath); instance = get(params.instanceIdBufferPath);
        element = get(params.elementIdBufferPath); normal = get(params.normalBufferPath); depth = get(params.depthBufferPath);
        camera = dynamic_cast<HdCamera*>(index->GetSprim(HdPrimTypeTokens->camera,params.cameraId));
    }
    void Execute(HdTaskContext*) override {
        TaskTimer timer(&context->pickNanoseconds, &context->pickCalls);
        prism::hydra::SceneBridge * const bridge = context->bridge.load();
        if (!bridge || !pick.outHits || !prim || !depth || !camera) return;
        if ((pick.pickTarget != HdxPickTokens->pickPrimsAndInstances && pick.pickTarget != HdxPickTokens->pickFaces) ||
            pick.resolveMode == HdxPickTokens->resolveDeep) {
            TF_WARN("PrismRT supports visible prim, instance and face picking only"); return;
        }
        const auto primView=CaptureBuffer(prim), instanceView=CaptureBuffer(instance),
            elementView=CaptureBuffer(element), depthView=CaptureBuffer(depth), normalView=CaptureBuffer(normal);
        const auto frame=primView.frame;
        if(!frame || !frame->picking || depthView.frame!=frame ||
           (instance && instanceView.frame!=frame) || (element && elementView.frame!=frame) ||
           (normal && normalView.frame!=frame)) return;
        const auto ids=ReadBuffer<int>(primView,HdFormatInt32), instances=ReadBuffer<int>(instanceView,HdFormatInt32),
                   elements=ReadBuffer<int>(elementView,HdFormatInt32);
        const auto depths=ReadBuffer<float>(depthView,HdFormatFloat32), normals=ReadBuffer<float>(normalView,HdFormatFloat32Vec3);
        if (ids.empty() || depths.size()!=ids.size()) return;
        const int w=int(primView.width),h=int(primView.height);
        const GfMatrix4d projection=frame->camera.projection;
        const GfMatrix4d view=frame->camera.worldToView;
        const GfMatrix4d toWorld=(view*projection).GetInverse();
        const GfMatrix4d toPick=pick.viewMatrix*pick.projectionMatrix;
        std::set<std::tuple<int,int,int>> seen;
        double best=std::numeric_limits<double>::infinity();
        HdxPickHit nearest{};
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
            const size_t i=size_t(y)*w+x;
            if(ids[i]<0 || !std::isfinite(depths[i]) || depths[i]<0 || depths[i]>=1) continue;
            const GfVec3d world=toWorld.Transform(GfVec3d(2*(x+.5)/w-1,2*(y+.5)/h-1,2*depths[i]-1));
            const GfVec3d ndc=toPick.Transform(world);
            if(std::abs(ndc[0])>1 || std::abs(ndc[1])>1 || std::abs(ndc[2])>1) continue;
            HdxPickHit hit{};
            if(size_t(ids[i])>=frame->picking->primPaths.size()) continue;
            hit.objectId=frame->picking->primPaths[ids[i]];
            if(hit.objectId.IsEmpty()) continue;
            if(size_t(ids[i])<frame->picking->instancerPaths.size())
                hit.instancerId=frame->picking->instancerPaths[ids[i]];
            const auto matches = [&](SdfPath const& root) {
                return hit.objectId.HasPrefix(root) ||
                    (!hit.instancerId.IsEmpty() && hit.instancerId.HasPrefix(root));
            };
            const auto& roots = pick.collection.GetRootPaths();
            const auto& excluded = pick.collection.GetExcludePaths();
            if ((!roots.empty() && !std::any_of(roots.begin(),roots.end(),matches)) ||
                std::any_of(excluded.begin(),excluded.end(),matches)) continue;
            bool clipped = false;
            for (const auto& plane : pick.clipPlanes) {
                if (plane[0]*world[0]+plane[1]*world[1]+plane[2]*world[2]+plane[3] < 0) {
                    clipped = true; break;
                }
            }
            if (clipped) continue;
            hit.instanceIndex=instances.size()==ids.size()?instances[i]:-1;
            hit.elementIndex=elements.size()==ids.size()?elements[i]:-1;
            hit.edgeIndex=hit.pointIndex=-1;
            hit.worldSpaceHitPoint=world;
            hit.normalizedDepth=depths[i];
            if(normals.size()==ids.size()*3)
                hit.worldSpaceHitNormal=GfVec3f(view.GetInverse().TransformDir(GfVec3d(normals[i*3],normals[i*3+1],normals[i*3+2])));
            const double metric=pick.resolveMode==HdxPickTokens->resolveNearestToCenter
                ? ndc[0]*ndc[0]+ndc[1]*ndc[1] : double(depths[i]);
            if(metric<best) {best=metric;nearest=hit;}
            if(pick.resolveMode==HdxPickTokens->resolveAll) pick.outHits->push_back(hit);
            else if(pick.resolveMode==HdxPickTokens->resolveUnique &&
                seen.emplace(ids[i],hit.instanceIndex,pick.pickTarget==HdxPickTokens->pickFaces?hit.elementIndex:-1).second)
                pick.outHits->push_back(hit);
        }
        if((pick.resolveMode==HdxPickTokens->resolveNearestToCenter || pick.resolveMode==HdxPickTokens->resolveNearestToCamera) && nearest.IsValid())
            pick.outHits->push_back(nearest);
    }
protected:
    void _Sync(HdSceneDelegate* delegate,HdTaskContext* context,HdDirtyBits* dirty) override {
        _GetTaskContextData(context,HdxPickTokens->pickParams,&pick);
        if(*dirty & HdChangeTracker::DirtyParams) _GetTaskParams(delegate,&params);
        *dirty=HdChangeTracker::Clean;
    }
private:
    std::shared_ptr<prism::hydra::TaskContext> context;
    HdxPickFromRenderBufferTaskParams params;
    HdxPickTaskContextParams pick;
    HdRenderBuffer *prim=nullptr,*instance=nullptr,*element=nullptr,*normal=nullptr,*depth=nullptr;
    HdCamera* camera=nullptr;
};

class Factory final : public HdLegacyTaskFactory {
public:
    Factory(HdLegacyTaskFactorySharedPtr original,HdSceneIndexBaseRefPtr scene,
            std::shared_ptr<prism::hydra::TaskContext> context)
        : original(std::move(original)),scene(std::move(scene)),context(std::move(context)) {}
    HdTaskSharedPtr Create(HdSceneDelegate* delegate,SdfPath const& id) override {
        auto task=original->Create(delegate,id);
        if(dynamic_cast<HdxPickFromRenderBufferTask*>(task.get())) return std::make_shared<NativePickTask>(delegate,id,context);
        if(dynamic_cast<HdxColorizeSelectionTask*>(task.get())) return std::make_shared<NativeSelectionTask>(delegate,id,scene,context);
        return task;
    }
private:
    HdLegacyTaskFactorySharedPtr original;
    HdSceneIndexBaseRefPtr scene;
    std::shared_ptr<prism::hydra::TaskContext> context;
};
}
HdLegacyTaskFactorySharedPtr PrismWrapTaskFactory(HdLegacyTaskFactorySharedPtr const& original,
    HdSceneIndexBaseRefPtr const& scene,
    std::shared_ptr<prism::hydra::TaskContext> const& context) {
    return std::make_shared<Factory>(original,scene,context);
}
PXR_NAMESPACE_CLOSE_SCOPE
