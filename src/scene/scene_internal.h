#pragma once

// Internal access to a public prism::Scene for the code in this repository
// (the loader modules and the CLI), not part of the public API: materials the
// modules compile themselves (USD's material service, docs/sdk_design.md
// 3.6), the committed SceneManager the CLI renders until the session API
// exists (migration step 4), and the render settings that still live on
// SceneManager (docs/DECISIONS.md, 2026-10-28: deferred from step 2).

#include "../internal_export.h"
#include <prism/scene.h>

#include "../mesh/mesh_conversion.h"
#include "../texture.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class Material;
class SceneManager;

namespace prism {
class RendererContext;
}

namespace prism::detail {

// What the id AOVs report for an Embree placement of the committed scene.
struct PlacementInfo {
    InstanceId instance;
    int primId = -1;
    int instanceId = -1;
    uint64_t userData = 0;
    // The mesh's coarse face per triangle (elementId); empty: the triangle
    // for triangle lists, none (-1) otherwise.
    prism::mesh::SharedFaceTable coarseFaces;
    bool triangleList = false;  // a mesh taken as given (glTF, OBJ)
};

// SceneManager's render settings (section 2.7 of the design), applied to
// each committed SceneManager; sessions set them from their RenderSettings
// before they render.
struct SceneRenderSettings {
    int maxDepth = 32;
    int rouletteDepth = 4;
    bool environmentSampling = true;
    prism::TextureFiltering textureFiltering = prism::TextureFiltering::Filtered;
    bool honorDoubleSided = false;
    bool textureStreaming = true;
    int gpuTextureBudgetMiB = 0;
    bool operator==(SceneRenderSettings const&) const = default;
};

// A committed revision kept alive by its holder: a progressive session
// renders it while the scene commits the next one.
struct CommittedRevision {
    std::shared_ptr<SceneManager> manager;
    std::shared_ptr<const std::unordered_map<unsigned, PlacementInfo>> placements;
    uint64_t revision = 0;
    // The placement of an Embree geometry id, or null.
    PlacementInfo const* placement(unsigned geometryId) const;
};

// The render lock of a scene (its sessions render one at a time, and a
// commit replaces the revision under it). It counts who waits for it, so a
// progressive session that holds it across passes yields to them at its pass
// boundaries (yield()).
class PRISM_INTERNAL_API RenderLock {
public:
    explicit RenderLock(Scene const& scene);
    RenderLock(std::mutex& mutex, std::atomic<int>& waiters);
    ~RenderLock();
    RenderLock(RenderLock const&) = delete;
    RenderLock& operator=(RenderLock const&) = delete;
    void lock();
    void unlock();
    // Lets a waiting render or commit go first, then locks again; true when
    // it did.
    bool yield();

private:
    std::mutex& m_mutex;
    std::atomic<int>& m_waiters;
    bool m_locked = false;
};

struct PRISM_INTERNAL_API SceneAccess {
    static RendererContext& context(Scene const& scene);
    // A Scene handle without a scene (a hosted session's, which renders its
    // front end's revisions); only copied and destroyed.
    static Scene detached();
    // A material the caller compiled (or a built-in one): the scene holds
    // it while any revision uses it. A null material is rejected.
    static MaterialId addCompiledMaterial(Scene& scene, std::shared_ptr<Material> material, std::string name);
    static void setRenderSettings(Scene& scene, SceneRenderSettings const& settings);
    // The settings last set and a counter of their changes (each
    // setRenderSettings), so sessions see another session's change.
    static SceneRenderSettings const& renderSettings(Scene const& scene);
    static uint64_t renderSettingsGeneration(Scene const& scene);
    // Held by the scene's sessions while they render: one at a time per
    // scene (the render settings above live on the committed revision).
    static std::mutex& renderMutex(Scene const& scene);
    static std::atomic<int>& renderWaiters(Scene const& scene);
    // The last committed revision, kept alive by the result; call with the
    // render lock held. Throws when nothing is committed.
    static CommittedRevision committedRevision(Scene const& scene);
    // The last committed revision's scene; throws when nothing is committed.
    static SceneManager& committed(Scene& scene);
    // The placement of an Embree geometry id of the committed scene, or null.
    static PlacementInfo const* placement(Scene const& scene, unsigned geometryId);
};

} // namespace prism::detail
