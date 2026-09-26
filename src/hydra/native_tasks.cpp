#include "native_tasks.h"
#include "scene_bridge.h"
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
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <map>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

void AppendInstanceSelection(HdSceneIndexBaseRefPtr const& scene,
    SdfPath path, HdSelectionSchema const& selection, std::set<int>* selected)
{
    std::map<SdfPath,std::set<int>> constraints;
    const auto nested = selection.GetNestedInstanceIndices();
    for (size_t i=0;i<nested.GetNumElements();++i) {
        const auto level=nested.GetElement(i);
        if (!level.GetInstancer() || !level.GetInstanceIndices()) return;
        const auto values=level.GetInstanceIndices()->GetTypedValue(0);
        constraints[level.GetInstancer()->GetTypedValue(0)]={values.begin(),values.end()};
    }
    struct Level { SdfPath path; VtIntArray indices; };
    std::vector<Level> chain;
    std::set<SdfPath> visited;
    while (!path.IsEmpty() && visited.insert(path).second) {
        const auto instanced=HdInstancedBySchema::GetFromParent(scene->GetPrim(path).dataSource);
        if (!instanced.GetPaths()) break;
        const auto paths=instanced.GetPaths()->GetTypedValue(0);
        if (paths.empty()) break;
        auto topology=HdInstancerTopologySchema::GetFromParent(scene->GetPrim(paths[0]).dataSource);
        if (!topology) return;
        auto indices=topology.ComputeInstanceIndicesForProto(path);
        if (indices.empty()) return;
        chain.push_back({paths[0],std::move(indices)});
        path=paths[0];
    }
    if (chain.empty()) return;
    for (const auto& [instancer,indices] : constraints)
        if (std::none_of(chain.begin(),chain.end(),[&](auto const& l){return l.path==instancer;})) return;
    std::vector<int> flattened{0};
    for (auto it=chain.rbegin();it!=chain.rend();++it) {
        std::vector<int> next;
        const auto constraint=constraints.find(it->path);
        for (int parent : flattened) for (size_t ordinal=0;ordinal<it->indices.size();++ordinal) {
            // HdInstanceIndicesSchema stores compact per-prototype ordinals.
            // The instancer topology array contains authored source indices;
            // only its size/radix is relevant when translating the selection
            // to the flattened instance IDs written by SceneBridge.
            if (constraint!=constraints.end() &&
                !constraint->second.count(static_cast<int>(ordinal))) continue;
            const uint64_t id=uint64_t(parent)*it->indices.size()+ordinal;
            if (id>uint64_t(std::numeric_limits<int>::max())) return;
            next.push_back(int(id));
        }
        flattened=std::move(next);
    }
    selected->insert(flattened.begin(),flattened.end());
}

void AppendUsdviewSelection(
    HdSceneIndexBaseRefPtr const& scene,
    SdfPath path,
    std::map<SdfPath, std::set<int>> const& selections,
    std::set<int>* selected,
    bool* fullySelected,
    bool* constrainedByIndexedSelection,
    bool* constrainedByWholeSelection)
{
    struct Level { SdfPath path; VtIntArray sourceIndices; };
    std::vector<Level> chain;
    std::set<SdfPath> visited;
    SdfPath current = path;
    while (!current.IsEmpty() && visited.insert(current).second) {
        const auto instanced = HdInstancedBySchema::GetFromParent(
            scene->GetPrim(current).dataSource);
        if (!instanced.GetPaths()) break;
        const auto paths = instanced.GetPaths()->GetTypedValue(0);
        if (paths.empty()) break;
        auto topology = HdInstancerTopologySchema::GetFromParent(
            scene->GetPrim(paths[0]).dataSource);
        if (!topology) break;
        auto indices = topology.ComputeInstanceIndicesForProto(current);
        if (indices.empty()) break;
        chain.push_back({paths[0], std::move(indices)});
        current = paths[0];
    }

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
    std::vector<int> flattened{0};
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        std::vector<int> next;
        const auto constraint = selections.find(it->path);
        constrained = constrained || constraint != selections.end();
        if (constraint != selections.end()) {
            if (constraint->second.count(-1))
                *constrainedByWholeSelection = true;
            else
                *constrainedByIndexedSelection = true;
        }
        for (int parent : flattened) {
            for (size_t ordinal = 0; ordinal < it->sourceIndices.size(); ++ordinal) {
                if (constraint != selections.end() &&
                    !constraint->second.count(-1) &&
                    !constraint->second.count(it->sourceIndices[ordinal]))
                    continue;
                const uint64_t id = uint64_t(parent) * it->sourceIndices.size() + ordinal;
                if (id > uint64_t(std::numeric_limits<int>::max())) return;
                next.push_back(int(id));
            }
        }
        flattened = std::move(next);
    }
    if (constrained)
        selected->insert(flattened.begin(), flattened.end());
}

// Hdx's compositor is reusable; only its legacy Rprim-ID encoding is replaced.
class NativeSelectionTracker final : public HdxSelectionTracker {
public:
    NativeSelectionTracker(HdSceneIndexBaseRefPtr scene,
                           std::shared_ptr<prism::hydra::TaskContext> context)
        : scene(std::move(scene)), context(std::move(context)) {}
    void UpdateSelection(HdRenderIndex*) override { _IncrementVersion(); }
    bool GetSelectionOffsetBuffer(HdRenderIndex const*, bool select, bool locate,
                                  VtIntArray* output) const override {
        prism::hydra::SceneBridge * const bridge = context->bridge.load();
        if (!bridge) {
            output->clear();
            return false;
        }
        std::vector<SdfPath> paths;
        for (int id = 0; ; ++id) {
            auto path = bridge->GetPrimPath(id);
            if (path.IsEmpty()) break;
            paths.push_back(path);
        }
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
                            scene, paths[id], usdviewSelection,
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
                                AppendInstanceSelection(
                                    scene,paths[id],selection,&instances);
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
        *output = VtIntArray(data.begin(),data.end());
        return any;
    }
private:
    HdSceneIndexBaseRefPtr scene;
    std::shared_ptr<prism::hydra::TaskContext> context;
};

class NativeSelectionTask final : public HdxColorizeSelectionTask {
public:
    NativeSelectionTask(HdSceneDelegate* delegate, SdfPath const& id,
                        HdSceneIndexBaseRefPtr const& scene,
                        std::shared_ptr<prism::hydra::TaskContext> const& context)
        : HdxColorizeSelectionTask(delegate,id),
          tracker(std::make_shared<NativeSelectionTracker>(scene,context)),
          taskContext(context), sceneDelegate(delegate) {}
    bool IsConverged() const override {
        return (!primId || primId->IsConverged()) &&
               (!instanceId || instanceId->IsConverged()) &&
               (!elementId || elementId->IsConverged());
    }
    void Prepare(HdTaskContext* context, HdRenderIndex* index) override {
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
        if (original.IsHolding<HdxSelectionTrackerSharedPtr>())
            tracker->SetSelection(original.UncheckedGet<HdxSelectionTrackerSharedPtr>()->GetSelectionMap());
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
    std::shared_ptr<NativeSelectionTracker> tracker;
    std::shared_ptr<prism::hydra::TaskContext> taskContext;
    HdSceneDelegate* sceneDelegate;
    uint64_t selectionRevision = std::numeric_limits<uint64_t>::max();
    HdxColorizeSelectionTaskParams params;
    HdRenderBuffer *primId = nullptr;
    HdRenderBuffer *instanceId = nullptr;
    HdRenderBuffer *elementId = nullptr;
};

template<class T> std::vector<T> ReadBuffer(HdRenderBuffer* buffer, HdFormat format) {
    if (!buffer || buffer->GetFormat() != format) return {};
    buffer->Resolve();
    const T* data = static_cast<const T*>(buffer->Map());
    std::vector<T> result;
    if (data) result.assign(data,data+size_t(buffer->GetWidth())*buffer->GetHeight()*HdGetComponentCount(format));
    buffer->Unmap();
    return result;
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
        prism::hydra::SceneBridge * const bridge = context->bridge.load();
        if (!bridge || !pick.outHits || !prim || !depth || !camera) return;
        if ((pick.pickTarget != HdxPickTokens->pickPrimsAndInstances && pick.pickTarget != HdxPickTokens->pickFaces) ||
            pick.resolveMode == HdxPickTokens->resolveDeep) {
            TF_WARN("PrismRT supports visible prim, instance and face picking only"); return;
        }
        const auto ids=ReadBuffer<int>(prim,HdFormatInt32), instances=ReadBuffer<int>(instance,HdFormatInt32),
                   elements=ReadBuffer<int>(element,HdFormatInt32);
        const auto depths=ReadBuffer<float>(depth,HdFormatFloat32), normals=ReadBuffer<float>(normal,HdFormatFloat32Vec3);
        if (ids.empty() || depths.size()!=ids.size()) return;
        const int w=int(prim->GetWidth()),h=int(prim->GetHeight());
        const auto policy=params.overrideWindowPolicy.value_or(camera->GetWindowPolicy());
        const GfMatrix4d projection=params.framing.IsValid()
            ? params.framing.ApplyToProjectionMatrix(camera->ComputeProjectionMatrix(),policy)
            : CameraUtilConformedWindow(camera->ComputeProjectionMatrix(),policy, double(w)/std::max(h,1));
        const GfMatrix4d view=camera->GetTransform().GetInverse();
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
            hit.objectId=bridge->GetPrimPath(ids[i]);
            if(hit.objectId.IsEmpty()) continue;
            hit.instancerId=bridge->GetInstancerPath(ids[i]);
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
                hit.worldSpaceHitNormal=GfVec3f(camera->GetTransform().TransformDir(GfVec3d(normals[i*3],normals[i*3+1],normals[i*3+2])));
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
