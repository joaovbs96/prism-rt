// OpaqueOccluders: which primitives always block shadow rays on the CPU,
// per placement and per primitive of mixed placements, with shadow links and
// culled back faces, and the Embree any-hit query that tests them.

#include "../src/device/opaque_occluders.h"
#include "../src/hydra/fallback_material.h"
#include "../src/scene.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

using prism::device::OpaqueOccluders;
using Kind = OpaqueOccluders::Kind;

namespace {

int failures = 0;

void require(bool condition, std::string const& what)
{
    if (!condition) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}

// Quads facing +z at height z, as two triangles each.
void AddQuad(Mesh& mesh, float x0, float x1, float z, int slot)
{
    const unsigned base = unsigned(mesh.vertices.size());
    for (Float3 p : {Float3(x0, -1, z), Float3(x1, -1, z), Float3(x1, 1, z), Float3(x0, 1, z)}) {
        mesh.vertices.push_back(p);
        mesh.normals.push_back(Float3(0, 0, 1));
        mesh.texcoords.push_back(Float2(0, 0));
    }
    for (unsigned i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    const int triangles = int(mesh.indices.size() / 3);
    mesh.material_mapping[triangles - 2] = slot;
    mesh.material_mapping[triangles - 1] = slot;
}

// A shadow ray straight down (or up) at (x, 0) over [tnear, tfar].
RTCRay Ray(float x, float fromZ, float toZ)
{
    RTCRay ray{};
    ray.org_x = x;
    ray.org_y = 0.0f;
    ray.org_z = fromZ;
    ray.dir_z = toZ < fromZ ? -1.0f : 1.0f;
    ray.tnear = 1.0e-4f;
    ray.tfar = std::abs(toZ - fromZ);
    ray.mask = ~0u;
    ray.flags = 0;
    return ray;
}

bool Occluded(OpaqueOccluders const& occluders, SceneManager& scene, float x, float fromZ, float toZ)
{
    RTCRay ray = Ray(x, fromZ, toZ);
    return occluders.occluded(scene, ray);
}

} // namespace

int main()
{
    RTCDevice device = rtcNewDevice(nullptr);
    if (!device) {
        std::cerr << "cannot create Embree device\n";
        return 1;
    }
    std::cout << "ray masks supported: " << rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_RAY_MASK_SUPPORTED)
              << ", filter functions: "
              << rtcGetDeviceProperty(device, RTC_DEVICE_PROPERTY_FILTER_FUNCTION_SUPPORTED) << '\n';
    {
        Diffuse opaque(nullptr);
        prism::hydra::FallbackMaterial cutout(nullptr, 0.5f);
        prism::hydra::FallbackMaterial solid(nullptr, 1.0f);

        SceneManager scene(device, prism::color::ColorManagement::Create({}));
        // 0: an opaque wall at z = 0.
        Mesh wall;
        AddQuad(wall, -1, 1, 0, 0);
        wall.materials[0] = &opaque;
        scene.attachMesh(&wall);
        // 1: a cutout card above it.
        Mesh card;
        AddQuad(card, -1, 1, 1, 0);
        card.materials[0] = &cutout;
        scene.attachMesh(&card);
        // 2: a merged mesh (glTF, OBJ): an opaque quad and a cutout quad.
        Mesh merged;
        AddQuad(merged, 2, 3, 0, 0);
        AddQuad(merged, 4, 5, 0, 1);
        merged.materials[0] = &solid;
        merged.materials[1] = &cutout;
        scene.attachMesh(&merged);
        // 3: an Embree instance of an opaque quad, moved by 10 along x.
        Mesh prototype;
        AddQuad(prototype, -1, 1, 0, 0);
        prototype.material_override = &opaque;
        RTCScene prototypeScene = rtcNewScene(device);
        prototype.setupEmbree(device, prototypeScene);
        rtcCommitScene(prototypeScene);
        RTCGeometry instance = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_INSTANCE);
        rtcSetGeometryInstancedScene(instance, prototypeScene);
        const float moved[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 0};
        rtcSetGeometryTransform(instance, 0, RTC_FORMAT_FLOAT3X4_COLUMN_MAJOR, moved);
        rtcCommitGeometry(instance);
        const unsigned instanceId = rtcAttachGeometry(scene.scene, instance);
        rtcReleaseGeometry(instance);
        scene.geometry[int(instanceId)] = &prototype;
        scene.intersection_geometry[int(instanceId)] = prototype.geom;
        scene.placement_transforms[int(instanceId)] = SceneManager::PlacementTransform{
            {1, 0, 0, 0, 1, 0, 0, 0, 1}, {1, 0, 0, 0, 1, 0, 0, 0, 1}, false, false};
        scene.commitScene();

        {
            const OpaqueOccluders occluders = OpaqueOccluders::Build(scene, scene.cullsBackFaces());
            require(occluders.kind(0) == Kind::Opaque, "the wall is opaque");
            require(occluders.kind(1) == Kind::NotOpaque, "the card is a cutout");
            require(occluders.kind(2) == Kind::Mixed, "the merged mesh is mixed");
            require(occluders.kind(int(instanceId)) == Kind::Opaque, "the instance is opaque");
            require(occluders.opaquePrimitive(2, 0) && occluders.opaquePrimitive(2, 1) &&
                        !occluders.opaquePrimitive(2, 2) && !occluders.opaquePrimitive(2, 3),
                    "the merged mesh's primitives follow their materials");
            require(!occluders.allOpaque() && occluders.mayHaveCutouts(), "the scene has cutouts");
            require(Occluded(occluders, scene, 0.0f, 5.0f, -5.0f), "the wall blocks behind the card");
            require(Occluded(occluders, scene, 0.0f, -5.0f, 0.5f), "the wall blocks from below");
            require(!Occluded(occluders, scene, 0.0f, 5.0f, 0.5f), "the card alone does not block");
            require(Occluded(occluders, scene, 2.5f, 5.0f, -5.0f), "the merged mesh's opaque quad blocks");
            require(!Occluded(occluders, scene, 4.5f, 5.0f, -5.0f), "the merged mesh's cutout quad does not");
            require(Occluded(occluders, scene, 10.0f, 5.0f, -5.0f), "the opaque instance blocks");
            require(!Occluded(occluders, scene, 20.0f, 5.0f, -5.0f), "nothing blocks a miss");
        }

        // Shadow links: placements outside a linked light's categories let
        // its light through, so only the wall (in every category) is opaque.
        {
            prism::SceneLight light;
            light.shadowLinkBit = 1u;
            scene.lights.push_back(light);
            scene.lightLinking = true;
            scene.placement_link_masks[0] = {~0u, 1u};
            scene.placement_link_masks[2] = {~0u, 0u};
            const OpaqueOccluders occluders = OpaqueOccluders::Build(scene, scene.cullsBackFaces());
            require(occluders.kind(0) == Kind::Opaque, "the linked wall stays opaque");
            require(occluders.kind(2) == Kind::NotOpaque, "the unlinked merged mesh is not opaque");
            require(occluders.kind(int(instanceId)) == Kind::NotOpaque, "placements in no category are not opaque");
            require(!Occluded(occluders, scene, 2.5f, 5.0f, -5.0f), "the unlinked opaque quad does not block");
            scene.lights.clear();
            scene.lightLinking = false;
            scene.placement_link_masks.clear();
        }

        // Culling (prism:honorDoubleSided): the wall's back face lets rays
        // through, its front face blocks.
        {
            scene.honorDoubleSided = true;
            wall.single_sided_all = true;
            require(scene.cullsBackFaces(), "the wall is single-sided");
            const OpaqueOccluders occluders = OpaqueOccluders::Build(scene, scene.cullsBackFaces());
            require(occluders.kind(0) == Kind::Opaque && occluders.culls(0), "the wall culls back faces");
            require(!occluders.allOpaque(), "culling is not all opaque");
            // One side of the wall is its front face (Embree's winding).
            const bool down = Occluded(occluders, scene, 0.0f, 5.0f, -5.0f);
            const bool up = Occluded(occluders, scene, 0.0f, -5.0f, 0.5f);
            require(down != up, "the wall's front face blocks, its back face is culled");
            scene.honorDoubleSided = false;
            wall.single_sided_all = false;
        }
        rtcReleaseScene(prototypeScene);
    }

    // A scene of opaque surfaces only takes the plain any-hit query.
    {
        Diffuse opaque(nullptr);
        SceneManager scene(device, prism::color::ColorManagement::Create({}));
        Mesh wall;
        AddQuad(wall, -1, 1, 0, 0);
        wall.materials[0] = &opaque;
        scene.attachMesh(&wall);
        scene.commitScene();
        const OpaqueOccluders occluders = OpaqueOccluders::Build(scene, false);
        require(occluders.allOpaque() && !occluders.mayHaveCutouts(), "an opaque scene is all opaque");
        require(Occluded(occluders, scene, 0.0f, 5.0f, -5.0f), "the opaque scene's wall blocks");
        require(!Occluded(occluders, scene, 0.0f, 5.0f, 0.5f), "nothing blocks before the wall");
    }
    rtcReleaseDevice(device);
    if (failures) return 1;
    std::cout << "opaque occluders: ok\n";
    return 0;
}
