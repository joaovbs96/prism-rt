#pragma once

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/imaging/hd/aov.h"
#include "pxr/imaging/hd/sceneIndex.h"
#include "pxr/base/vt/dictionary.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE
class HdRenderThread;
class PrismRenderBuffer;
PXR_NAMESPACE_CLOSE_SCOPE

namespace prism::hydra {

struct RenderSettings {
    int samples = 256;
    int maxDepth = 32;
    int rouletteDepth = 4;
    int seed = 0;
    float sampleClamp = 10.0f;
    int previewMaxDimension = 96;
    int refinementDelayMs = 100;
    int resourceRevision = 0;
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

struct CameraState {
    pxr::GfMatrix4d worldToView{1.0};
    pxr::GfMatrix4d projection{1.0};
    float nearClip = 0.001f;
    float farClip = 1.0e30f;
    float fStop = 0.0f;
    float focalLength = 0.0f;
    float focusDistance = 1.0f;
    bool orthographic = false;
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
    pxr::VtDictionary GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
    std::atomic<bool> _converged{true};
};

} // namespace prism::hydra
