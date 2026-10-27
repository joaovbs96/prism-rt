#pragma once

#include "../device/device.h"
#include "../color/color_management.h"
#include "../progress.h"
#include "../render_defaults.h"
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
    // Defaults shared with the CLI and prism_hydra_render (render_defaults.h).
    int samples = prism::defaults::kSamplesPerPixel;
    int maxDepth = 32;
    int rouletteDepth = 4;
    int seed = 0;
    float sampleClamp = 10.0f;
    // Adaptive sampling error threshold (0 disables); `samples` is then the
    // per-pixel maximum.
    float adaptiveThreshold = prism::defaults::kAdaptiveThreshold;
    int adaptiveMinSamples = prism::defaults::kAdaptiveMinSamples;
    // Wall-clock limit of an accumulation in seconds (0: none): refinement
    // stops and reports convergence at the first pass boundary past it.
    double timeLimitSeconds = prism::defaults::kTimeLimitSeconds;
    // Mesh conversion (prism:subdivisionLevel and so on); their defaults
    // come from the PRISM_* environment variables. Process-wide, like the
    // colour management.
    int subdivisionLevel = prism::defaults::FromEnvironment().subdivisionLevel;
    int subdivisionMaxFaces = prism::defaults::FromEnvironment().subdivisionMaxFaces;
    int subdivisionBudgetFaces = prism::defaults::FromEnvironment().subdivisionBudgetFaces;
    int displacementLevel = prism::defaults::FromEnvironment().displacementLevel;
    int displacementMaxFaces = prism::defaults::FromEnvironment().displacementMaxFaces;
    // GPU texture budget in MiB, 0: 45% of device memory
    // (prism:gpuTextureBudgetMiB, default PRISM_GPU_TEXTURE_BUDGET_MIB).
    int gpuTextureBudgetMiB = prism::defaults::FromEnvironment().gpuTextureBudgetMiB;
    // Subsurface scattering renders its diffuse lobe (prism:sssFallback,
    // default PRISM_SSS_FALLBACK). Process-wide.
    bool sssFallback = prism::defaults::FromEnvironment().sssFallback;
    // Probabilistic NEE (see prism::device::NeeSettings; 0 always samples).
    float neeProbabilityMin = 0.0f;
    int neeFromDepth = 2;
    // Spatial light selection among analytic lights (prism:lightSelection).
    bool spatialLights = true;
    bool adaptivePreview = true;
    bool domeLightCameraVisibility = true;
    int previewTargetMs = 33;
    // 0: automatic (512 on GPU devices, 96 on the CPU).
    int previewMaxDimension = 0;
    int refinementDelayMs = 100;
    int resourceRevision = 0;
    bool sphereBatching = false;
    // USD doubleSided (prism:honorDoubleSided): back faces of single-sided,
    // non-transmissive meshes are not hit. Off by default.
    bool honorDoubleSided = false;
    // Soft target for converted geometry retained only by hidden prototypes.
    // Zero preserves unlimited retention; source data and Embree are excluded.
    int hiddenGeometryBudgetMiB = 0;
    prism::TextureFiltering textureFiltering =
        prism::TextureFiltering::Filtered;
    // GPU texture streaming (Vulkan): load the mip levels shading asks for
    // (prism:textureStreaming), and settle them before an accumulation
    // starts so all of its samples see the same levels
    // (prism:textureStreamingPrepass; offline renders).
    bool textureStreaming = true;
    bool textureStreamingPrepass = false;
    enum class EnvironmentSampling { Mis, Bsdf } environmentSampling =
        EnvironmentSampling::Mis;
    // Requested render device; unavailable GPU devices fall back to the CPU.
    prism::device::DevicePreference device = prism::device::DevicePreference::Auto;
    // Denoising (docs/denoising.md). prism:denoise shows the colour AOV
    // denoised every denoiseInterval passes and when the render converges or
    // pauses (its accumulation, which Map reads, stays raw); a bound
    // 'denoised' AOV is denoised whatever it says.
    bool denoise = false;
    int denoiseInterval = 16;
    prism::device::DenoiseSettings denoiseSettings;
    // While a GPU device prepares the scene (kernel compile, upload), render
    // progressively on the CPU device and hand over when the GPU is ready.
    bool gpuPreparationPreview = true;
    // The preview is skipped for scenes whose Embree memory exceeds this
    // many MiB (0 never skips).
    int gpuPreparationPreviewMaxMiB = 8192;
    // prism:colorManagement:{ocioConfig,workingSpace,display,view}; empty
    // fields take the defaults (docs/color_management.md). The colour AOV is
    // linear in the working space; display and view only record the request.
    prism::color::Options color;
    // The active render settings prim's materialBindingPurposes: a prim's
    // material is its binding for the first of these purposes that it has
    // (the empty token is allPurpose). Process-wide, like the colour
    // management.
    pxr::TfTokenVector materialBindingPurposes{pxr::TfToken("full"), pxr::TfToken()};
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
    // The prism* progress/status keys of GetStats, without taking the bridge
    // mutex or any other renderer lock.
    pxr::VtDictionary GetProgressStats() const;
    prism::Progress::Snapshot GetProgress() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
    std::atomic<bool> _converged{true};
};

} // namespace prism::hydra
