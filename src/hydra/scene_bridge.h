#pragma once

#include "../texture_service.h"
#include "published_pick_scene.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/base/vt/dictionary.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderThread;
class PrismRenderBuffer;
class HdRenderBuffer;
PXR_NAMESPACE_CLOSE_SCOPE

namespace prism::hydra {

struct RenderSettings {
    int samples = 256;
    int maxDepth = 32;
    int rouletteDepth = 4;
    int seed = 0;
    float sampleClamp = 10.0f;
    bool adaptivePreview = true;
    bool domeLightCameraVisibility = true;
    int previewTargetMs = 33;
    int previewMaxDimension = 96;
    int refinementDelayMs = 100;
    int resourceRevision = 0;
    bool sphereBatching = false;
    // Soft target for converted geometry retained only by hidden prototypes.
    // Zero preserves unlimited retention; source data and Embree are excluded.
    int hiddenGeometryBudgetMiB = 0;
    prism::TextureFiltering textureFiltering =
        prism::TextureFiltering::Filtered;
    enum class EnvironmentSampling { Mis, Bsdf } environmentSampling =
        EnvironmentSampling::Mis;
};

enum class SceneDirtyKind : unsigned int {
    None = 0,
    Placement = 1u << 0,
    Prototype = 1u << 1,
    Material = 1u << 2,
    Environment = 1u << 3,
    Structure = 1u << 4,
    Unknown = 1u << 5
};

struct StructuralChange {
    enum class Operation { Add, Remove, Replace, Rename, Unknown };
    Operation operation = Operation::Unknown;
    pxr::SdfPath path;
    pxr::TfToken previousType;
    pxr::TfToken newType;
    bool previousHadChildren = false;
    pxr::SdfPath newPath; // Explicit destination for Rename; path is the source.
};

struct SceneDirtyNotice {
    SceneDirtyKind kind = SceneDirtyKind::Unknown;
    pxr::SdfPathVector paths;
    std::string reason;
    std::vector<StructuralChange> structuralChanges;
};

struct CameraState {
    pxr::GfMatrix4d worldToView{1.0};
    pxr::GfMatrix4d projection{1.0};
    float nearClip = 0.001f;
    float farClip = 1.0e30f;
    float fStop = 0.0f;
    float focalLength = 0.0f;
    float focusDistance = 1.0f;
    bool orthographic = false;
    float linearExposureScale = 1.0f;
};

struct PickingTable {
    struct InstanceLevel {
        pxr::SdfPath instancerPath;
        std::vector<int> sourceIndicesByOrdinal;
        bool operator==(InstanceLevel const &) const = default;
    };
    std::vector<pxr::SdfPath> primPaths;
    std::vector<pxr::SdfPath> instancerPaths;
    std::vector<std::vector<InstanceLevel>> instanceTopologies;
    std::vector<std::size_t> logicalPlacementCountsByPrim;
    bool operator==(PickingTable const &) const = default;
};

struct PublishedFrame {
    uint64_t generation = 0;
    uint64_t cameraRevision = 0;
    std::shared_ptr<const PickingTable> picking;
    std::shared_ptr<const PublishedPickScene> pickScene;
    CameraState camera;
    unsigned int width = 0;
    unsigned int height = 0;
};

class SceneBridge final
{
public:
    SceneBridge(pxr::HdSceneIndexBaseRefPtr sceneIndex,
                pxr::HdRenderThread *renderThread);
    ~SceneBridge();

    SceneBridge(const SceneBridge &) = delete;
    SceneBridge &operator=(const SceneBridge &) = delete;

    void MarkSceneDirty();
    void MarkSceneDirty(SceneDirtyKind kind, pxr::SdfPath const &path = {});
    void MarkScenePathsDirty(SceneDirtyKind kind, pxr::SdfPathVector const &paths);
    void MarkSceneDirtyBatch(std::vector<SceneDirtyNotice> const &notices);
    void RestartAccumulation();
    void SetSettings(RenderSettings const &settings);
    RenderSettings GetSettings() const;
    bool Prepare(CameraState const &camera,
                 pxr::HdRenderPassAovBindingVector const &bindings,
                 pxr::TfTokenVector const &renderTags,
                 unsigned int width, unsigned int height);
    void Render();
    bool IsConverged() const { return _converged.load(); }
    pxr::SdfPath GetPrimPath(int primId) const;
    pxr::SdfPath GetInstancerPath(int primId) const;
    std::vector<pxr::PrismRenderBuffer *> GetRenderBuffers() const;
    void RemoveRenderBuffer(pxr::HdRenderBuffer *buffer);
    pxr::VtDictionary GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
    std::atomic<bool> _converged{true};
};

} // namespace prism::hydra
