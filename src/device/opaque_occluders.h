#pragma once

// Which surfaces always block shadow rays on the CPU (the integrator's
// occludedByOpaque, shaders/integrator/path_tracer.h visible()).
//
// Each placement is classified when the CPU device prepares a scene: opaque
// (every material blocks: no cutout), not opaque (a cutout, or outside some
// light's shadow-link categories), or mixed, with one flag per primitive (the
// merged meshes of glTF and OBJ files carry all their materials in one
// placement). The opaque test is one Embree any-hit query whose argument
// filter accepts only hits on opaque primitives, and skips culled back faces
// (prism:honorDoubleSided) as the closest-hit path does. The GPU devices do
// the same with instance masks (kMaskOpaque); Embree geometry masks would be
// state shared by the SDK's scene revisions, so the CPU keeps its
// classification with the render state instead (docs/DECISIONS.md,
// 2026-10-30).

#include "../internal_export.h"

#include <embree4/rtcore.h>

#include <array>
#include <cstdint>
#include <vector>

class SceneManager;

namespace prism::device {

class PRISM_INTERNAL_API OpaqueOccluders {
public:
    enum class Kind : uint8_t { NotOpaque = 0, Opaque = 1, Mixed = 2 };

    // `culling`: SceneManager::cullsBackFaces.
    static OpaqueOccluders Build(SceneManager& scene, bool culling);

    // Every primitive blocks every shadow ray (no cutouts, culling or shadow
    // links): any hit at all is a blocker.
    bool allOpaque() const { return m_allOpaque; }
    // Some primitive may let a shadow ray through: visible() must look for
    // cutouts once no opaque primitive blocks.
    bool mayHaveCutouts() const { return m_mayHaveCutouts; }

    // Whether an opaque primitive lies on `ray` (between tnear and tfar).
    // Leaves `ray` as rtcOccluded1 does.
    bool occluded(SceneManager& scene, RTCRay& ray) const;

    // The classification of a placement (NotOpaque for unknown ones), and of
    // one of its primitives.
    Kind kind(int placement) const;
    bool opaquePrimitive(int placement, unsigned primitive) const;
    bool culls(int placement) const;

    // The filter's decision for a candidate hit (tests).
    bool blocks(SceneManager& scene, RTCRay const& ray, RTCHit const& hit) const;

private:
    struct Placement {
        Kind kind = Kind::NotOpaque;
        bool cull = false;
        // Mixed: index of its primitive flags; culling: of its normal transform.
        uint32_t primitives = 0;
        uint32_t transform = ~0u;
    };
    struct NormalTransform {
        std::array<float, 9> normal{};
        bool flip = false;
    };

    std::vector<Placement> m_placements;            // by placement id
    std::vector<std::vector<uint8_t>> m_primitives; // mixed placements: 1 opaque
    std::vector<NormalTransform> m_transforms;      // culling placements
    bool m_allOpaque = false;
    bool m_mayHaveCutouts = true;
    // False when the placement ids are too sparse for a table: visible()
    // then takes its per-hit path for every shadow ray, as before.
    bool m_supported = false;
};

} // namespace prism::device
