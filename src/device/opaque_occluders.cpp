#include "opaque_occluders.h"

#include "../batched_spheres.h"
#include "../hydra/fallback_material.h"
#include "../mdl_material.h"
#include "../scene.h"

#include <algorithm>
#include <unordered_map>

namespace prism::device {

namespace {

// A material whose cutout opacity is 1 everywhere: shadow rays stop at it.
bool AlwaysBlocks(Material* material)
{
    if (!material) return true;
    if (auto* mdl = dynamic_cast<MDLMaterial*>(material)) {
        const auto cutout = mdl->constant_cutout();
        return !mdl->is_valid() || (cutout && *cutout >= 1.0f);
    }
    if (auto* fallback = dynamic_cast<prism::hydra::FallbackMaterial*>(material))
        return fallback->provenOpaque();
    return true; // Material::cutoutOpacity is 1
}

// The placement ids index a table; ids sparser than this keep the old path.
constexpr size_t kMaxPlacementTable = size_t(1) << 26;

struct OcclusionQuery {
    RTCRayQueryContext context; // first: Embree passes its address back
    OpaqueOccluders const* occluders;
    SceneManager* scene;
};

void OpaqueFilter(const RTCFilterFunctionNArguments* args)
{
    // rtcOccluded1: one ray, laid out as RTCRay / RTCHit.
    if (args->N != 1 || args->valid[0] == 0) return;
    auto const* query = reinterpret_cast<OcclusionQuery const*>(args->context);
    auto const* ray = reinterpret_cast<RTCRay const*>(args->ray);
    auto const* hit = reinterpret_cast<RTCHit const*>(args->hit);
    if (!query->occluders->blocks(*query->scene, *ray, *hit)) args->valid[0] = 0;
}

} // namespace

OpaqueOccluders OpaqueOccluders::Build(SceneManager& scene, bool culling)
{
    OpaqueOccluders result;
    int maxId = -1;
    for (auto const& [id, geometry] : scene.geometry) {
        if (id < 0) return result; // unsupported: per-hit path
        maxId = std::max(maxId, id);
    }
    if (size_t(maxId + 1) > kMaxPlacementTable) return result;
    result.m_supported = true;
    result.m_placements.resize(size_t(maxId + 1));

    // Shadow links: a placement outside some linked light's categories lets
    // that light through (path_tracer.h visible()).
    uint32_t shadowBits = 0u;
    if (scene.lightLinking)
        for (auto const& light : scene.lights) shadowBits |= light.shadowLinkBit;

    std::unordered_map<Material*, bool> known;
    auto blocks = [&](Material* material) {
        auto [found, inserted] = known.emplace(material, false);
        if (inserted) found->second = AlwaysBlocks(material);
        return found->second;
    };

    bool allOpaque = true, cutouts = false;
    for (auto const& [id, geometry] : scene.geometry) {
        Placement& placement = result.m_placements[size_t(id)];
        auto binding = scene.placement_materials.find(id);
        auto const* bound = binding != scene.placement_materials.end() ? &binding->second : nullptr;

        // The materials the placement may show, as SceneManager::getMaterial
        // resolves them.
        bool anyBlocks = false, anyPasses = false, recognised = true;
        auto see = [&](Material* material) { (blocks(material) ? anyBlocks : anyPasses) = true; };
        if (bound) {
            if (bound->fallback) see(bound->fallback); // null: the geometry's own
            bound->triangles.forEach(see);
        }
        size_t primitives = 0;
        if (auto* mesh = dynamic_cast<Mesh*>(geometry)) {
            if (mesh->material_override) see(mesh->material_override); // null: none
            for (auto const& [index, material] : mesh->materials) see(material);
            primitives = mesh->indices.size() / 3;
        } else if (auto* sphere = dynamic_cast<Sphere*>(geometry)) {
            // null: none (the delegate's pooled spheres; the binding has it)
            if (sphere->material) see(sphere->material);
            primitives = 1;
        } else if (auto* batch = dynamic_cast<BatchedSpheres*>(geometry)) {
            for (auto const& entry : batch->entries) see(entry.material);
            primitives = batch->entries.size();
        } else {
            recognised = false; // unknown geometry: keep the general shadow loop
        }

        if (!recognised || (anyPasses && !anyBlocks)) {
            placement.kind = Kind::NotOpaque;
        } else if (!anyPasses) {
            placement.kind = Kind::Opaque;
        } else {
            // Mixed materials (a merged glTF or OBJ mesh): a flag per primitive.
            placement.kind = Kind::Mixed;
            placement.primitives = uint32_t(result.m_primitives.size());
            std::vector<uint8_t>& flags = result.m_primitives.emplace_back(primitives, uint8_t(0));
            const unsigned instance = scene.intersection_geometry.contains(id) ? unsigned(id)
                                                                               : RTC_INVALID_GEOMETRY_ID;
            Material* last = nullptr;
            bool lastBlocks = false;
            for (size_t p = 0; p < primitives; ++p) {
                Material* material = nullptr;
                bool resolved = false;
                if (bound) {
                    if (Material* triangle = bound->triangles.at(size_t(p))) material = triangle, resolved = true;
                    else if (bound->fallback) material = bound->fallback, resolved = true;
                }
                if (!resolved) {
                    try {
                        material = geometry->getMaterial(unsigned(p), instance);
                    } catch (std::exception const&) {
                        continue; // not opaque
                    }
                }
                if (material != last || p == 0) {
                    last = material;
                    lastBlocks = blocks(material);
                }
                flags[p] = lastBlocks ? 1u : 0u;
            }
            // All alike after all (a material no primitive uses): one kind.
            const bool someOpaque = std::find(flags.begin(), flags.end(), uint8_t(1)) != flags.end();
            const bool someCutout = std::find(flags.begin(), flags.end(), uint8_t(0)) != flags.end();
            if (!someOpaque || !someCutout) {
                result.m_primitives.pop_back();
                placement.kind = someOpaque ? Kind::Opaque : Kind::NotOpaque;
                placement.primitives = 0;
            }
        }

        if (shadowBits != 0u && placement.kind != Kind::NotOpaque) {
            auto masks = scene.placement_link_masks.find(id);
            const uint32_t casts = masks != scene.placement_link_masks.end() ? masks->second.second : 0u;
            if ((casts & shadowBits) != shadowBits) placement.kind = Kind::NotOpaque;
        }

        // Single-sided triangles whose back faces the closest-hit path skips
        // (SceneManager::culledBackFace).
        if (culling && placement.kind != Kind::NotOpaque && geometry && geometry->type == RTC_GEOMETRY_TYPE_TRIANGLE) {
            auto const* mesh = dynamic_cast<Mesh const*>(geometry);
            const bool single = scene.single_sided_placements.contains(id) ||
                (mesh && (mesh->single_sided_all || !mesh->single_sided.empty()));
            if (single) {
                placement.cull = true;
                NormalTransform transform;
                if (auto found = scene.placement_transforms.find(id); found != scene.placement_transforms.end()) {
                    transform.normal = found->second.normal;
                    // SceneManager::prepareHit's orientation for triangles.
                    transform.flip = found->second.flipOrientation || found->second.mirrored;
                } else {
                    transform.normal = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                }
                placement.transform = uint32_t(result.m_transforms.size());
                result.m_transforms.push_back(transform);
            }
        }

        if (placement.kind != Kind::Opaque || placement.cull) allOpaque = false;
        if (placement.kind == Kind::NotOpaque) cutouts = true;
        if (placement.kind == Kind::Mixed) {
            auto const& flags = result.m_primitives[placement.primitives];
            if (std::find(flags.begin(), flags.end(), uint8_t(0)) != flags.end()) cutouts = true;
        }
    }
    result.m_allOpaque = allOpaque;
    result.m_mayHaveCutouts = cutouts;
    return result;
}

OpaqueOccluders::Kind OpaqueOccluders::kind(int placement) const
{
    if (placement < 0 || size_t(placement) >= m_placements.size()) return Kind::NotOpaque;
    return m_placements[size_t(placement)].kind;
}

bool OpaqueOccluders::opaquePrimitive(int placement, unsigned primitive) const
{
    if (placement < 0 || size_t(placement) >= m_placements.size()) return false;
    Placement const& p = m_placements[size_t(placement)];
    if (p.kind == Kind::Opaque) return true;
    if (p.kind == Kind::NotOpaque) return false;
    auto const& flags = m_primitives[p.primitives];
    return primitive < flags.size() && flags[primitive] != 0u;
}

bool OpaqueOccluders::culls(int placement) const
{
    return placement >= 0 && size_t(placement) < m_placements.size() && m_placements[size_t(placement)].cull;
}

bool OpaqueOccluders::blocks(SceneManager& scene, RTCRay const& ray, RTCHit const& hit) const
{
    const unsigned id = hit.instID[0] != RTC_INVALID_GEOMETRY_ID ? hit.instID[0] : hit.geomID;
    if (id >= m_placements.size()) return false;
    Placement const& placement = m_placements[id];
    if (placement.kind == Kind::NotOpaque) return false;
    if (placement.kind == Kind::Mixed) {
        auto const& flags = m_primitives[placement.primitives];
        if (hit.primID >= flags.size() || flags[hit.primID] == 0u) return false;
    }
    if (!placement.cull) return true;
    // A back face (RayHit::front_face is false) of a culled triangle lets
    // the ray through. Ng is in the placement's prototype space.
    NormalTransform const& t = m_transforms[placement.transform];
    const float x = hit.Ng_x, y = hit.Ng_y, z = hit.Ng_z;
    float nx = x * t.normal[0] + y * t.normal[3] + z * t.normal[6];
    float ny = x * t.normal[1] + y * t.normal[4] + z * t.normal[7];
    float nz = x * t.normal[2] + y * t.normal[5] + z * t.normal[8];
    if (t.flip) nx = -nx, ny = -ny, nz = -nz;
    const Float3 n = Float3(nx, ny, nz).normalize();
    const bool front = dot(Float3(ray.dir_x, ray.dir_y, ray.dir_z), n) < 0;
    return front || !scene.culledBackFace(int(id), int(hit.primID));
}

bool OpaqueOccluders::occluded(SceneManager& scene, RTCRay& ray) const
{
    if (m_allOpaque) {
        rtcOccluded1(scene.scene, &ray);
        return ray.tfar < 0.0f;
    }
    if (!m_supported) return false;
    OcclusionQuery query;
    rtcInitRayQueryContext(&query.context);
    query.occluders = this;
    query.scene = &scene;
    RTCOccludedArguments args;
    rtcInitOccludedArguments(&args);
    args.flags = RTCRayQueryFlags(RTC_RAY_QUERY_FLAG_INCOHERENT | RTC_RAY_QUERY_FLAG_INVOKE_ARGUMENT_FILTER);
    args.context = &query.context;
    args.filter = OpaqueFilter;
    rtcOccluded1(scene.scene, &ray, &args);
    return ray.tfar < 0.0f;
}

} // namespace prism::device
