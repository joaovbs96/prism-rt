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
#include <pxr/imaging/hdx/aovInputTask.h>
#include <pxr/imaging/hdx/pickFromRenderBufferTask.h>
#include <pxr/imaging/hdx/selectionTracker.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hdx/effectsShader.h>
#include <pxr/imaging/hdx/hgiConversions.h>
#include <pxr/imaging/cameraUtil/conformWindow.h>
#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/blitCmds.h>
#include <pxr/imaging/hgi/blitCmdsOps.h>
#include <pxr/imaging/hgi/shaderFunction.h>
#include <pxr/imaging/hgi/shaderProgram.h>
#include <pxr/imaging/hgi/sampler.h>
#include <cstring>
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
                // in OpenUSD 26.05 and 26.08 (the GPU shader's newer enum uses 4).
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
        const auto& rayScene=frame->pickScene;
        const auto ids=rayScene?std::vector<int>{}:ReadBuffer<int>(primView,HdFormatInt32),
                   instances=rayScene?std::vector<int>{}:ReadBuffer<int>(instanceView,HdFormatInt32),
                   elements=rayScene?std::vector<int>{}:ReadBuffer<int>(elementView,HdFormatInt32);
        const auto depths=rayScene?std::vector<float>{}:ReadBuffer<float>(depthView,HdFormatFloat32),
                   normals=rayScene?std::vector<float>{}:ReadBuffer<float>(normalView,HdFormatFloat32Vec3);
        if (!rayScene && (ids.empty() || depths.size()!=ids.size())) return;
        const int w=int(rayScene?frame->width:primView.width),
                  h=int(rayScene?frame->height:primView.height);
        if(w<=0 || h<=0) return;
        const GfMatrix4d projection=frame->camera.projection;
        const GfMatrix4d view=frame->camera.worldToView;
        const GfMatrix4d toWorld=(view*projection).GetInverse();
        const GfMatrix4d toPick=pick.viewMatrix*pick.projectionMatrix;
        const auto bounds=rayScene
            ? prism::hydra::GetPickRasterBounds(view*projection,toPick.GetInverse(),w,h)
            : prism::hydra::PickRasterBounds{0,w,0,h};
        std::set<std::tuple<int,int,int>> seen;
        double best=std::numeric_limits<double>::infinity();
        HdxPickHit nearest{};
        for(int y=bounds.yBegin;y<bounds.yEnd;++y) for(int x=bounds.xBegin;x<bounds.xEnd;++x) {
            const size_t i=size_t(y)*w+x;
            prism::hydra::PublishedPickHit rayHit;
            if(rayScene && !rayScene->PickPixel(x,h-1-y,&rayHit)) continue;
            const int primId=rayScene?rayHit.primId:ids[i];
            const float hitDepth=rayScene?rayHit.normalizedDepth:depths[i];
            if(primId<0 || !std::isfinite(hitDepth) || hitDepth<0 || hitDepth>=1) continue;
            const GfVec3d world=rayScene?rayHit.worldPoint:
                toWorld.Transform(GfVec3d(2*(x+.5)/w-1,2*(y+.5)/h-1,2*hitDepth-1));
            const GfVec3d ndc=toPick.Transform(world);
            if(std::abs(ndc[0])>1 || std::abs(ndc[1])>1 || std::abs(ndc[2])>1) continue;
            HdxPickHit hit{};
            if(size_t(primId)>=frame->picking->primPaths.size()) continue;
            hit.objectId=frame->picking->primPaths[primId];
            if(hit.objectId.IsEmpty()) continue;
            if(size_t(primId)<frame->picking->instancerPaths.size())
                hit.instancerId=frame->picking->instancerPaths[primId];
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
            hit.instanceIndex=rayScene?rayHit.instanceId:(instances.size()==ids.size()?instances[i]:-1);
            hit.elementIndex=rayScene?rayHit.elementId:(elements.size()==ids.size()?elements[i]:-1);
            hit.edgeIndex=hit.pointIndex=-1;
            hit.worldSpaceHitPoint=world;
            hit.normalizedDepth=hitDepth;
            if(rayScene) hit.worldSpaceHitNormal=rayHit.worldNormal;
            else if(normals.size()==ids.size()*3)
                hit.worldSpaceHitNormal=GfVec3f(view.GetInverse().TransformDir(GfVec3d(normals[i*3],normals[i*3+1],normals[i*3+2])));
            const double metric=pick.resolveMode==HdxPickTokens->resolveNearestToCenter
                ? ndc[0]*ndc[0]+ndc[1]*ndc[1] : double(hitDepth);
            if(metric<best) {best=metric;nearest=hit;}
            if(pick.resolveMode==HdxPickTokens->resolveAll) pick.outHits->push_back(hit);
            else if(pick.resolveMode==HdxPickTokens->resolveUnique &&
                seen.emplace(primId,hit.instanceIndex,pick.pickTarget==HdxPickTokens->pickFaces?hit.elementIndex:-1).second)
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

// One draw copies all updated atlas quads. Attachment Load is essential:
// pixels outside these quads belong to earlier immutable tile publications.
class PresentationCompositor final : public HdxEffectsShader {
public:
    struct Vertex { float x,y,z,w,u,v; };
    explicit PresentationCompositor(Hgi *hgi)
        : HdxEffectsShader(hgi,"Prism presentation tiles") {
        HgiShaderFunctionDesc vertex, fragment;
        vertex.debugName="Prism presentation vertex";
        vertex.shaderStage=HgiShaderStageVertex;
        HgiShaderFunctionAddStageInput(&vertex,"position","vec4","position");
        HgiShaderFunctionAddStageInput(&vertex,"uvIn","vec2");
        HgiShaderFunctionAddStageOutput(&vertex,"gl_Position","vec4","position");
        HgiShaderFunctionAddStageOutput(&vertex,"uvOut","vec2");
        vertex.shaderCode="void main() { gl_Position=position; uvOut=uvIn; }";
        fragment.debugName="Prism presentation fragment";
        fragment.shaderStage=HgiShaderStageFragment;
        HgiShaderFunctionAddStageInput(&fragment,"uvOut","vec2");
        HgiShaderFunctionAddStageOutput(&fragment,"hd_FragColor","vec4","color");
        HgiShaderFunctionAddTexture(&fragment,"source",0);
        HgiShaderFunctionAddConstantParam(&fragment,"background","vec4");
        fragment.shaderCode=R"(
            void main() {
                vec4 c=vec4(HgiGet_source(uvOut));
                if (background.a != 0.0) {
                    float a=clamp(background.a,0.0,1.0)*(1.0-clamp(c.a,0.0,1.0));
                    c.rgb += background.rgb*a;
                    c.a += a;
                }
                hd_FragColor=c;
            })";
        HgiShaderProgramDesc program;
        program.debugName="Prism presentation";
        program.shaderFunctions={hgi->CreateShaderFunction(vertex),hgi->CreateShaderFunction(fragment)};
        shader=hgi->CreateShaderProgram(program);
        if (!shader || !shader->IsValid() ||
            !program.shaderFunctions[0]->IsValid() || !program.shaderFunctions[1]->IsValid()) {
            if (shader) TF_WARN("Prism presentation program: %s",shader->GetCompileErrors().c_str());
            for (auto const &function : program.shaderFunctions)
                if (function && !function->IsValid())
                    TF_WARN("Prism presentation shader: %s",function->GetCompileErrors().c_str());
            _DestroyShaderProgram(&shader);
            return;
        }
        _SetShaderProgram(shader);
        HgiVertexAttributeDesc position,uv;
        position.format=HgiFormatFloat32Vec4; position.offset=0; position.shaderBindLocation=0;
        uv.format=HgiFormatFloat32Vec2; uv.offset=4*sizeof(float); uv.shaderBindLocation=1;
        HgiVertexBufferDesc vertices;
        vertices.bindingIndex=0; vertices.vertexStride=sizeof(Vertex);
        vertices.vertexAttributes={position,uv};
        _SetVertexBufferDescs({vertices});
        _SetPrimitiveType(HgiPrimitiveTypeTriangleList);
        HgiDepthStencilState depth;
        depth.depthTestEnabled=false; depth.depthWriteEnabled=false;
        depth.stencilTestEnabled=false;
        _SetDepthStencilState(depth);
        HgiMultiSampleState multisample;
        multisample.alphaToCoverageEnable=false;
        _SetMultiSampleState(multisample);
        HgiRasterizationState raster;
        raster.cullMode=HgiCullModeNone;
        _SetRasterizationState(raster);
        HgiSamplerDesc samplerDesc;
        samplerDesc.maxAnisotropy=1;
        sampler=hgi->CreateSampler(samplerDesc);
    }
    ~PresentationCompositor() override {
        _DestroyShaderProgram(&shader);
        if (sampler) _GetHgi()->DestroySampler(&sampler);
    }
    bool IsValid() const { return bool(shader) && bool(sampler); }
    static void Quad(std::vector<Vertex> *vertices,float x0,float y0,float x1,float y1,
                     float u0=0,float v0=0,float u1=1,float v1=1) {
        vertices->insert(vertices->end(),{
            {x0,y0,0,1,u0,v0},{x1,y0,0,1,u1,v0},{x1,y1,0,1,u1,v1},
            {x0,y0,0,1,u0,v0},{x1,y1,0,1,u1,v1},{x0,y1,0,1,u0,v1}});
    }
    void Draw(HgiTextureHandle source,HgiTextureHandle destination,
              std::vector<Vertex> const &vertices,GfVec4f background,bool preserve) {
        HgiBufferDesc desc;
        desc.debugName="Prism presentation quads"; desc.usage=HgiBufferUsageVertex;
        desc.byteSize=vertices.size()*sizeof(Vertex); desc.vertexStride=sizeof(Vertex);
        desc.initialData=vertices.data();
        vertexBuffer=_GetHgi()->CreateBuffer(desc);
        vertexCount=static_cast<uint32_t>(vertices.size());
        HgiTextureBindDesc binding;
        binding.bindingIndex=0; binding.stageUsage=HgiShaderStageFragment;
        binding.resourceType=HgiBindResourceTypeCombinedSamplerImage;
        binding.textures={source}; binding.samplers={sampler};
        _SetTextureBindings({binding});
        HgiAttachmentDesc attachment;
        attachment.loadOp=preserve ? HgiAttachmentLoadOpLoad : HgiAttachmentLoadOpDontCare;
        attachment.storeOp=HgiAttachmentStoreOpStore;
        attachment.blendEnabled=false;
        _SetColorAttachments({attachment});
        _SetShaderConstants(sizeof(background),background.data(),HgiShaderStageFragment);
        auto dim=destination->GetDescriptor().dimensions;
        _CreateAndSubmitGraphicsCmds({destination},{},{},{},GfVec4i(0,0,dim[0],dim[1]));
        // Every atlas and vertex buffer is immutable until Hgi's deferred
        // destruction releases GPU commands that reference it.
        _GetHgi()->DestroyBuffer(&vertexBuffer);
    }
private:
    void _RecordDrawCmds() override {
        _DrawNonIndexed(vertexBuffer,vertexCount,0,1,0);
    }
    HgiShaderProgramHandle shader;
    HgiSamplerHandle sampler;
    HgiBufferHandle vertexBuffer;
    uint32_t vertexCount=0;
};

// CPU exports retain the raw AOV. The optimized Float32 RGBA path has a
// pristine GPU source; Hdx color correction/selection only touch working
// textures. Other formats retain the stock conversion behavior.
class PresentationInputTask final : public HdxAovInputTask {
public:
    PresentationInputTask(HdSceneDelegate *delegate, SdfPath const &id,
                          std::shared_ptr<prism::hydra::TaskContext> context)
        : HdxAovInputTask(delegate, id),context(std::move(context)) {}
    ~PresentationInputTask() override {
        for (auto *texture : {&sourceTexture,&workingTexture,&intermediateTexture,
                              &depthTexture,&depthIntermediate})
            if (*texture) _GetHgi()->DestroyTexture(texture);
    }
    // The stock override is not exported from OpenUSD's Windows DLL. Cache
    // readiness before upload just as it does, so an exporter cannot accept
    // an older texture when the render worker finishes during Execute.
    bool IsConverged() const override { return uploadedConverged; }
    void Prepare(HdTaskContext *ctx, HdRenderIndex *index) override {
        aovBuffer = aovPath.IsEmpty() ? nullptr : dynamic_cast<HdRenderBuffer *>(index->GetBprim(
            HdPrimTypeTokens->renderBuffer, aovPath));
        buffer = dynamic_cast<PrismRenderBuffer *>(aovBuffer);
        depthBuffer = depthPath.IsEmpty() ? nullptr : dynamic_cast<HdRenderBuffer *>(index->GetBprim(
            HdPrimTypeTokens->renderBuffer, depthPath));
        optimized=buffer && buffer->GetFormat()==HdFormatFloat32Vec4 &&
            (!depthBuffer || (dynamic_cast<PrismRenderBuffer *>(depthBuffer) &&
                              depthBuffer->GetFormat()==HdFormatFloat32));
        if (optimized && !compositor)
            compositor=std::make_unique<PresentationCompositor>(_GetHgi());
        optimized=optimized && compositor->IsValid();
        if (optimized) _GetHgi()->StartFrame();
        else HdxAovInputTask::Prepare(ctx,index);
    }
    void Execute(HdTaskContext *ctx) override {
        const bool readyBeforeUpload = !aovBuffer || (aovBuffer->IsConverged() &&
            (!depthBuffer || depthBuffer->IsConverged()));
        uploadedConverged = false;
        // Pin the same immutable publication for the stock upload and the
        // presentation copy even if a sample pass publishes during Execute.
        struct Mapping {
            PrismRenderBuffer *buffer;
            explicit Mapping(PrismRenderBuffer *buffer) : buffer(buffer) {
                if (buffer) buffer->Map();
            }
            ~Mapping() { if (buffer) buffer->Unmap(); }
        } mapping(buffer), depthMapping(dynamic_cast<PrismRenderBuffer *>(depthBuffer));
        if (optimized) ExecuteCached(ctx);
        else {
            HdxAovInputTask::Execute(ctx);
            UploadPresentation(ctx);
        }
        auto latest = [](HdRenderBuffer *candidate) {
            auto *prism = dynamic_cast<PrismRenderBuffer *>(candidate);
            return !prism || prism->Capture().pixels == prism->CapturePublished().pixels;
        };
        // Native selection may pin an earlier frame before this task uploads.
        // Require another Execute after that mapping is released.
        uploadedConverged = readyBeforeUpload && latest(aovBuffer) && latest(depthBuffer);
    }
private:
    HgiTextureHandle CreateTexture(unsigned width,unsigned height,HgiFormat format,
                                   HgiTextureUsageBits usage,void const *pixels=nullptr,
                                   size_t bytes=0) {
        HgiTextureDesc desc;
        desc.debugName="Prism presentation";
        desc.dimensions=GfVec3i(width,height,1); desc.format=format;
        desc.usage=usage|HgiTextureUsageBitsShaderRead;
        desc.layerCount=1; desc.mipLevels=1; desc.sampleCount=HgiSampleCount1;
        desc.initialData=pixels; desc.pixelsByteSize=bytes;
        return _GetHgi()->CreateTexture(desc);
    }
    void EnsureTarget(HgiTextureHandle &texture,unsigned width,unsigned height,
                      HgiFormat format,HgiTextureUsageBits usage) {
        if (texture && (texture->GetDescriptor().dimensions!=GfVec3i(width,height,1) ||
                        texture->GetDescriptor().format!=format)) _GetHgi()->DestroyTexture(&texture);
        if (!texture) texture=CreateTexture(width,height,format,usage);
    }
    void RecordUpload(size_t bytes) {
        context->presentationUploadBytes.fetch_add(bytes,std::memory_order_relaxed);
        context->presentationUploadCalls.fetch_add(1,std::memory_order_relaxed);
    }
    void ExecuteCached(HdTaskContext *ctx) {
        buffer->Resolve();
        auto state=buffer->CapturePresentationState();
        auto const &raw=state.raw;
        ctx->erase(HdAovTokens->color); ctx->erase(HdAovTokens->depth);
        ctx->erase(HdxAovTokens->colorIntermediate); ctx->erase(HdxAovTokens->depthIntermediate);
        if (!raw.pixels || !raw.width || !raw.height) return;
        auto pixels=state.legacyPixels ? state.legacyPixels : raw.pixels;
        bool restore = !sourceTexture || cachedPixels.lock()!=pixels || cachedFrame.lock()!=raw.frame ||
            sourceTexture->GetDescriptor().dimensions!=GfVec3i(raw.width,raw.height,1);
        if (sourceHasTiles && (!state.snapshot || state.snapshot->pass!=cachedPass ||
            state.snapshot->base.pixels!=cachedPixels.lock() ||
            state.snapshot->tiles.size()!=cachedTiles.size())) restore=true;
        if (!restore && sourceHasTiles && state.snapshot) {
            // A canceled/replaced packet can remove completed tiles. Rebuild
            // from raw instead of leaving pixels from that canceled work.
            for (size_t i=0;i<state.snapshot->tiles.size();++i)
                if (cachedTiles[i].present && !state.snapshot->tiles[i]) { restore=true; break; }
        }
        bool transferred=false;
        if (restore) {
            if (!sourceTexture || sourceTexture->GetDescriptor().dimensions !=
                    GfVec3i(raw.width,raw.height,1)) {
                if (sourceTexture) _GetHgi()->DestroyTexture(&sourceTexture);
                sourceTexture=CreateTexture(raw.width,raw.height,HgiFormatFloat32Vec4,
                    HgiTextureUsageBitsColorTarget,pixels->data(),pixels->size());
            } else {
                // Keep the GPU allocation and bindings across completed passes.
                // Hgi orders this upload after prior draws using the texture.
                HgiTextureCpuToGpuOp upload;
                upload.bufferByteSize=pixels->size();
                upload.cpuSourceBuffer=pixels->data();
                upload.gpuDestinationTexture=sourceTexture;
                auto commands=_GetHgi()->CreateBlitCmds();
                commands->CopyTextureCpuToGpu(upload);
                _GetHgi()->SubmitCmds(commands.get());
            }
            RecordUpload(pixels->size()); transferred=true;
            cachedPixels=pixels; cachedFrame=raw.frame; cachedSnapshot.reset();
            cachedTiles.clear(); sourceHasTiles=false;
        }
        if (state.snapshot && state.snapshot!=cachedSnapshot.lock()) {
            std::vector<std::shared_ptr<const PrismRenderBuffer::PresentationTile>> changed;
            for (size_t i=0;i<state.snapshot->tiles.size();++i) {
                auto const &tile=state.snapshot->tiles[i];
                if (tile && (i>=cachedTiles.size() ||
                             tile!=cachedTiles[i].tile.lock())) changed.push_back(tile);
            }
            // A faster pass can complete hundreds of tiles per publication.
            // Batch their quads into a bounded 1024x1024 atlas rather than
            // paying graphics submission overhead for every 64 tiles.
            constexpr size_t atlasTileCapacity=1024;
            for (size_t begin=0;begin<changed.size();begin+=atlasTileCapacity) {
                const size_t count=std::min(atlasTileCapacity,changed.size()-begin);
                const unsigned columns=static_cast<unsigned>(std::min(size_t(32),count));
                const unsigned atlasWidth=columns*32;
                const unsigned atlasHeight=static_cast<unsigned>((count+columns-1)/columns)*32;
                std::vector<float> atlas(size_t(atlasWidth)*atlasHeight*4,0);
                std::vector<PresentationCompositor::Vertex> vertices;
                vertices.reserve(count*6);
                for (size_t i=0;i<count;++i) {
                    auto const &tile=*changed[begin+i];
                    const unsigned ax=static_cast<unsigned>(i%columns)*32;
                    const unsigned ay=static_cast<unsigned>(i/columns)*32;
                    for (unsigned y=0;y<tile.height;++y)
                        std::memcpy(atlas.data()+(size_t(ay+y)*atlasWidth+ax)*4,
                            tile.rgba.data()+size_t(y)*tile.width*4,size_t(tile.width)*4*sizeof(float));
                    PresentationCompositor::Quad(&vertices,
                        2.0f*tile.x/raw.width-1,2.0f*tile.y/raw.height-1,
                        2.0f*(tile.x+tile.width)/raw.width-1,2.0f*(tile.y+tile.height)/raw.height-1,
                        float(ax)/atlasWidth,float(ay)/atlasHeight,
                        float(ax+tile.width)/atlasWidth,float(ay+tile.height)/atlasHeight);
                }
                const size_t bytes=atlas.size()*sizeof(float);
                auto texture=CreateTexture(atlasWidth,atlasHeight,HgiFormatFloat32Vec4,
                    HgiTextureUsageBitsShaderRead,atlas.data(),bytes);
                RecordUpload(bytes); transferred=true;
                context->presentationAtlasBytes.fetch_add(bytes,std::memory_order_relaxed);
                compositor->Draw(texture,sourceTexture,vertices,GfVec4f(0),true);
                context->presentationAtlasDraws.fetch_add(1,std::memory_order_relaxed);
                _GetHgi()->DestroyTexture(&texture);
            }
        }
        cachedSnapshot=state.snapshot;
        sourceHasTiles=bool(state.snapshot);
        if (state.snapshot) {
            cachedPass=state.snapshot->pass;
            cachedTiles.resize(state.snapshot->tiles.size());
            for (size_t i=0;i<cachedTiles.size();++i)
                cachedTiles[i]={state.snapshot->tiles[i],bool(state.snapshot->tiles[i])};
        } else cachedTiles.clear();
        if (!transferred) context->presentationSkippedUploads.fetch_add(1,std::memory_order_relaxed);
        EnsureTarget(workingTexture,raw.width,raw.height,HgiFormatFloat32Vec4,HgiTextureUsageBitsColorTarget);
        EnsureTarget(intermediateTexture,raw.width,raw.height,HgiFormatFloat32Vec4,HgiTextureUsageBitsColorTarget);
        std::vector<PresentationCompositor::Vertex> fullQuad;
        PresentationCompositor::Quad(&fullQuad,-1,-1,1,1);
        compositor->Draw(sourceTexture,workingTexture,fullQuad,
            state.hasBackground ? state.background : GfVec4f(0),false);
        (*ctx)[HdAovTokens->color]=VtValue(workingTexture);
        (*ctx)[HdxAovTokens->colorIntermediate]=VtValue(intermediateTexture);
        if (auto *depth=dynamic_cast<PrismRenderBuffer *>(depthBuffer)) {
            depth->Resolve();
            auto view=depth->Capture();
            if (view.pixels && view.width && view.height) {
                if (!depthTexture || depthPixels.lock()!=view.pixels ||
                    depthTexture->GetDescriptor().dimensions!=GfVec3i(view.width,view.height,1)) {
                    if (depthTexture) _GetHgi()->DestroyTexture(&depthTexture);
                    depthTexture=CreateTexture(view.width,view.height,HgiFormatFloat32,
                        HgiTextureUsageBitsDepthTarget,view.pixels->data(),view.pixels->size());
                    RecordUpload(view.pixels->size()); depthPixels=view.pixels;
                } else context->presentationSkippedUploads.fetch_add(1,std::memory_order_relaxed);
                EnsureTarget(depthIntermediate,view.width,view.height,HgiFormatFloat32,HgiTextureUsageBitsDepthTarget);
                (*ctx)[HdAovTokens->depth]=VtValue(depthTexture);
                (*ctx)[HdxAovTokens->depthIntermediate]=VtValue(depthIntermediate);
            }
        }
    }
    void UploadPresentation(HdTaskContext *ctx) {
        if (!buffer) return;
        auto const raw = buffer->Capture();
        auto const presentation = buffer->CapturePresentation();
        if (presentation.pixels == raw.pixels || !presentation.pixels) return;
        auto const color = ctx->find(HdAovTokens->color);
        if (color == ctx->end() || !color->second.IsHolding<HgiTextureHandle>()) return;
        auto texture = color->second.UncheckedGet<HgiTextureHandle>();
        if (!texture || texture->GetDescriptor().format != HgiFormatFloat32Vec4 ||
            texture->GetDescriptor().dimensions != GfVec3i(presentation.width, presentation.height, 1)) return;
        HgiTextureCpuToGpuOp upload;
        upload.bufferByteSize = presentation.pixels->size();
        upload.cpuSourceBuffer = presentation.pixels->data();
        upload.gpuDestinationTexture = texture;
        auto commands = _GetHgi()->CreateBlitCmds();
        commands->PushDebugGroup("Prism viewport background");
        commands->CopyTextureCpuToGpu(upload);
        commands->PopDebugGroup();
        _GetHgi()->SubmitCmds(commands.get());
    }
protected:
    void _Sync(HdSceneDelegate *delegate, HdTaskContext *ctx, HdDirtyBits *dirty) override {
        if (*dirty & HdChangeTracker::DirtyParams) {
            HdxAovInputTaskParams params;
            if (_GetTaskParams(delegate, &params)) {
                aovPath = params.aovBufferPath;
                depthPath = params.depthBufferPath;
            }
        }
        HdxAovInputTask::_Sync(delegate, ctx, dirty);
    }
private:
    SdfPath aovPath;
    SdfPath depthPath;
    HdRenderBuffer *aovBuffer = nullptr;
    HdRenderBuffer *depthBuffer = nullptr;
    PrismRenderBuffer *buffer = nullptr;
    bool uploadedConverged = false;
    bool optimized=false;
    std::shared_ptr<prism::hydra::TaskContext> context;
    std::unique_ptr<PresentationCompositor> compositor;
    HgiTextureHandle sourceTexture,workingTexture,intermediateTexture,depthTexture,depthIntermediate;
    // Identity tracking must not pin the CPU publication after upload; doing
    // so would defeat the worker's raw-buffer storage recycling.
    std::weak_ptr<const std::vector<uint8_t>> cachedPixels,depthPixels;
    std::weak_ptr<const prism::hydra::PublishedFrame> cachedFrame;
    std::weak_ptr<const PrismRenderBuffer::PresentationSnapshot> cachedSnapshot;
    struct CachedTile {
        std::weak_ptr<const PrismRenderBuffer::PresentationTile> tile;
        bool present=false;
    };
    std::vector<CachedTile> cachedTiles;
    uint64_t cachedPass=0;
    bool sourceHasTiles=false;
};

class Factory final : public HdLegacyTaskFactory {
public:
    Factory(HdLegacyTaskFactorySharedPtr original,HdSceneIndexBaseRefPtr scene,
            std::shared_ptr<prism::hydra::TaskContext> context)
        : original(std::move(original)),scene(std::move(scene)),context(std::move(context)) {}
    HdTaskSharedPtr Create(HdSceneDelegate* delegate,SdfPath const& id) override {
        auto task=original->Create(delegate,id);
        if(dynamic_cast<HdxAovInputTask*>(task.get())) return std::make_shared<PresentationInputTask>(delegate,id,context);
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
