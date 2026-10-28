// The public scene API (include/prism/scene.h): scenes built from arrays,
// commit, a small CPU render, handle lifetime and errors on misuse.
//   prism_scene_api_test [studio_env.hdr]
#include <prism/renderer.h>
#include <prism/scene.h>

#include "../src/batched_spheres.h"
#include "../src/camera_rays.h"
#include "../src/device/device.h"
#include "../src/environment.h"
#include "../src/renderer/renderer_context.h"
#include "../src/scene.h"
#include "../src/scene/scene_internal.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

namespace {

int failures = 0;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// Runs `f`; true when it throws prism::Error with `code`.
bool throws(prism::ErrorCode code, std::function<void()> const& f)
{
    try {
        f();
    } catch (prism::Error const& error) {
        return error.code() == code;
    } catch (...) {
        return false;
    }
    return false;
}

template <class T>
prism::BufferView view(std::vector<T> const& values, uint32_t components, prism::DataType type)
{
    return prism::BufferView::Of(values, components, type);  // copied by the scene (no owner)
}

prism::Matrix4d translate(double x, double y, double z)
{
    prism::Matrix4d m;
    m[3][0] = x; m[3][1] = y; m[3][2] = z;
    return m;
}

// A 2x2 quad at z = 0 facing +Z: as a polygon mesh, or a triangle list.
prism::MeshDesc quad(bool triangles)
{
    static const std::vector<float> points{-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    static const std::vector<int32_t> counts{4}, polygon{0, 1, 2, 3}, list{0, 1, 2, 0, 2, 3};
    prism::MeshDesc mesh;
    mesh.points = prism::Constant(view(points, 3, prism::DataType::Float32));
    if (triangles) {
        mesh.faceVertexIndices = view(list, 1, prism::DataType::Int32);
    } else {
        mesh.faceVertexCounts = view(counts, 1, prism::DataType::Int32);
        mesh.faceVertexIndices = view(polygon, 1, prism::DataType::Int32);
    }
    return mesh;
}

// A cube of 6 quads (a Catmull-Clark surface when `subdivide`).
prism::MeshDesc cube(bool subdivide)
{
    static const std::vector<float> points{-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
                                           -1, -1, 1,  1, -1, 1,  1, 1, 1,  -1, 1, 1};
    static const std::vector<int32_t> counts(6, 4);
    static const std::vector<int32_t> indices{0, 3, 2, 1, 4, 5, 6, 7, 0, 1, 5, 4, 2, 3, 7, 6, 1, 2, 6, 5, 0, 4, 7, 3};
    prism::MeshDesc mesh;
    mesh.points = prism::Constant(view(points, 3, prism::DataType::Float32));
    mesh.faceVertexCounts = view(counts, 1, prism::DataType::Int32);
    mesh.faceVertexIndices = view(indices, 1, prism::DataType::Int32);
    if (subdivide) mesh.subdivision.scheme = prism::SubdivisionDesc::Scheme::CatmullClark;
    return mesh;
}

prism::RendererOptions options()
{
    prism::RendererOptions result = prism::RendererOptions::FromEnvironment();
    result.color = {};
    result.color.workingSpace = "lin_rec709";
    return result;
}

RayHit cast(SceneManager& scene, Float3 origin, Float3 direction)
{
    return scene.cast_ray(initialize_ray(origin, direction));
}

void buildAndRender(prism::Renderer const& renderer)
{
    prism::Scene scene = renderer.createScene();
    prism::MaterialDesc red;
    red.color = Color3(0.8f, 0.1f, 0.1f);
    const prism::MaterialId material = scene.addMaterial(red);
    const prism::MeshId mesh = scene.addMesh(quad(false));
    prism::InstanceDesc instance;
    instance.prototype = mesh;
    instance.materials = {material};
    instance.primId = 7;
    const prism::InstanceId placed = scene.addInstance(instance);
    prism::EnvironmentDesc environment;
    environment.kind = prism::EnvironmentDesc::Kind::Constant;
    environment.color = Color3(0.25f, 0.5f, 0.75f);
    scene.addEnvironment(environment);
    prism::LightDesc light;
    light.shape = prism::LightDesc::Shape::Sphere;
    light.transform = prism::Constant(translate(0, 0, 3));
    light.intensity = 10;
    scene.addLight(light);

    const prism::CommitResult result = scene.commit();
    check(result.revision == 1 && scene.revision() == 1, "the first commit is revision 1");
    check(result.triangles == 2 && result.instances == 1 && result.meshes == 1 && result.lights == 1,
          "the quad commits as two triangles in one placement");
    SceneManager& manager = prism::detail::SceneAccess::committed(scene);
    const RayHit hit = cast(manager, Float3(0.5f, 0.25f, 2), Float3(0, 0, -1));
    check(hit.geomid != int(RTC_INVALID_GEOMETRY_ID) && std::abs(hit.p.z()) < 1e-5f, "a ray hits the quad");
    check(manager.getMaterial(hit) != nullptr && manager.lights.size() == 1, "the hit has the material; one light");
    auto const* info = prism::detail::SceneAccess::placement(scene, unsigned(hit.geomid));
    check(info && info->primId == 7 && info->instance == placed, "the placement reports the instance's prim id");

    // A 16x16 CPU render looking down -Z from z = 4.
    prism::device::ImageRequest request;
    request.width = request.height = 16;
    request.samples = 4;
    request.adaptive.threshold = 0.0f;
    request.camera.cameraToWorld = translate(0, 0, 4);
    const double n = 0.1, f = 100.0, t = std::tan(0.25);  // a narrow field of view
    prism::Matrix4d projection(1.0 / t, 0, 0, 0, 0, 1.0 / t, 0, 0, 0, 0, (f + n) / (n - f), -1, 0, 0,
                               2 * f * n / (n - f), 0);
    request.camera.inverseProjection = projection.inverse();
    prism::DeviceOptions deviceOptions = renderer.context()->deviceOptions();
    auto device = prism::device::CreateDevice(prism::device::DevicePreference::Cpu, deviceOptions);
    device->SceneChanged(manager);
    const std::vector<float> pixels = device->RenderImage(manager, request);
    check(pixels.size() == 16 * 16 * 3, "the render has 16x16 RGB pixels");
    const float* centre = pixels.data() + (8 * 16 + 8) * 3;
    check(centre[0] > centre[2] && centre[0] > 0.0f && std::isfinite(centre[0]), "the centre sees the red quad");
    bool finite = true;
    for (float value : pixels) finite = finite && std::isfinite(value) && value >= 0.0f;
    check(finite, "every pixel is finite and nonnegative");

    // Edits take effect at the next commit: moving the instance.
    scene.setTransform(placed, prism::Constant(translate(10, 0, 0)));
    check(cast(prism::detail::SceneAccess::committed(scene), Float3(0.5f, 0.25f, 2), Float3(0, 0, -1)).geomid !=
              int(RTC_INVALID_GEOMETRY_ID),
          "an edit does not change the committed revision");
    scene.commit();
    SceneManager& moved = prism::detail::SceneAccess::committed(scene);
    check(cast(moved, Float3(0.5f, 0.25f, 2), Float3(0, 0, -1)).geomid == int(RTC_INVALID_GEOMETRY_ID) &&
              cast(moved, Float3(10.5f, 0.25f, 2), Float3(0, 0, -1)).geomid != int(RTC_INVALID_GEOMETRY_ID),
          "the moved instance is hit at its new place after commit");
    check(scene.revision() == 2, "the second commit is revision 2");

    // Hidden instances are not placed.
    scene.setVisible(placed, false);
    check(scene.commit().instances == 0, "a hidden instance is not placed");
}

void geometry(prism::Renderer const& renderer)
{
    // A triangle list placed with a transform: an Embree instance.
    {
        prism::Scene scene = renderer.createScene();
        prism::InstanceDesc instance;
        instance.prototype = scene.addMesh(quad(true));
        instance.transform = prism::Constant(translate(0, 0, -2));
        std::vector<float> value{0.25f, 0.5f};
        instance.primvars.push_back({"tint", prism::Interpolation::Constant, view(value, 2, prism::DataType::Float32)});
        scene.addInstance(instance);
        scene.commit();
        SceneManager& manager = prism::detail::SceneAccess::committed(scene);
        const RayHit hit = cast(manager, Float3(0.25f, 0.5f, 2), Float3(0, 0, -1));
        check(hit.geomid != int(RTC_INVALID_GEOMETRY_ID) && std::abs(hit.p.z() + 2.0f) < 1e-5f,
              "a transformed triangle list is hit at its place");
        check(manager.intersection_geometry.size() == 1 && manager.placement_primvars.size() == 1,
              "it is an instance with its instance primvars");
        float tint[2] = {0, 0};
        check(hit.instancePrimvars &&
                  static_cast<InstancePrimvars const*>(hit.instancePrimvars)->lookup("tint", tint, 2) && tint[1] == 0.5f,
              "the instance primvar reaches the hit");
    }
    // Subdivision: a Catmull-Clark cube refines; level 0 renders its hull.
    for (int level : {0, 2}) {
        prism::SceneOptions options;
        options.subdivisionLevel = level;
        prism::Scene scene = renderer.createScene(options);
        prism::InstanceDesc instance;
        instance.prototype = scene.addMesh(cube(true));
        scene.addInstance(instance);
        const size_t triangles = scene.commit().triangles;
        check(level == 0 ? triangles == 12 : triangles == 6 * 16 * 2,
              "a Catmull-Clark cube at level " + std::to_string(level) + " has " + std::to_string(triangles) +
                  " triangles");
    }
    // A polygon mesh with two instances shares one prototype.
    {
        prism::Scene scene = renderer.createScene();
        prism::InstanceDesc instance;
        instance.prototype = scene.addMesh(cube(false));
        scene.addInstance(instance);
        instance.transform = prism::Constant(translate(5, 0, 0));
        scene.addInstance(instance);
        const auto result = scene.commit();
        SceneManager& manager = prism::detail::SceneAccess::committed(scene);
        const RayHit a = cast(manager, Float3(0, 0, 5), Float3(0, 0, -1));
        const RayHit b = cast(manager, Float3(5, 0, 5), Float3(0, 0, -1));
        check(result.meshes == 1 && result.instances == 2 && a.geomid != b.geomid &&
                  manager.geometry.at(a.geomid) == manager.geometry.at(b.geomid),
              "two instances of a mesh share its prototype");
    }
    // Per-face material slots.
    {
        prism::Scene scene = renderer.createScene();
        prism::MaterialDesc first, second;
        second.kind = prism::MaterialDesc::Kind::Emissive;
        prism::MeshDesc mesh = quad(true);
        std::vector<int32_t> slots{0, 1};
        mesh.faceMaterialSlots = view(slots, 1, prism::DataType::Int32);
        prism::InstanceDesc instance;
        instance.prototype = scene.addMesh(mesh);
        instance.materials = {scene.addMaterial(first), scene.addMaterial(second)};
        scene.addInstance(instance);
        scene.commit();
        SceneManager& manager = prism::detail::SceneAccess::committed(scene);
        Material* lower = manager.getMaterial(cast(manager, Float3(0.5f, -0.5f, 1), Float3(0, 0, -1)));
        Material* upper = manager.getMaterial(cast(manager, Float3(-0.5f, 0.5f, 1), Float3(0, 0, -1)));
        check(lower && upper && lower != upper && dynamic_cast<Emissive*>(upper), "faces take their slot's material");
    }
    // Analytic spheres: one and several per description.
    {
        prism::Scene scene = renderer.createScene();
        std::vector<float> centers{0, 0, 0, 3, 0, 0}, radii{1, 0.5f};
        prism::SpheresDesc spheres;
        spheres.centers = prism::Constant(view(centers, 3, prism::DataType::Float32));
        spheres.radii = view(radii, 1, prism::DataType::Float32);
        prism::InstanceDesc instance;
        instance.prototype = scene.addSpheres(spheres);
        scene.addInstance(instance);
        scene.commit();
        SceneManager& manager = prism::detail::SceneAccess::committed(scene);
        const RayHit a = cast(manager, Float3(0, 0, 5), Float3(0, 0, -1));
        const RayHit b = cast(manager, Float3(3, 0, 5), Float3(0, 0, -1));
        check(std::abs(a.p.z() - 1.0f) < 1e-4f && std::abs(b.p.z() - 0.5f) < 1e-4f &&
                  dynamic_cast<BatchedSpheres*>(manager.geometry.at(a.geomid)),
              "a spheres description renders its analytic spheres");
    }
    // Light linking: categories become bits, in light order.
    {
        prism::Scene scene = renderer.createScene();
        prism::LightDesc key, fill;
        key.lightLinkCategory = "key";
        fill.shadowLinkCategory = "fill";
        scene.addLight(key);
        scene.addLight(fill);
        prism::InstanceDesc instance;
        instance.prototype = scene.addMesh(quad(false));
        instance.lightLinkCategories = {"key"};
        instance.shadowLinkCategories = {"fill", "unknown"};
        scene.addInstance(instance);
        scene.commit();
        SceneManager& manager = prism::detail::SceneAccess::committed(scene);
        check(manager.lightLinking && manager.lights[0].lightLinkBit == 1u && manager.lights[1].shadowLinkBit == 1u &&
                  manager.placement_link_masks.size() == 1 &&
                  manager.placement_link_masks.begin()->second == std::pair<uint32_t, uint32_t>(1u, 1u),
              "link categories become bits of the lights and the placement");
    }
}

void environments(prism::Renderer const& renderer, std::string const& texture)
{
    // Nothing lights the scene: without the default dome it stays black.
    prism::SceneOptions options;
    options.defaultDome = true;
    options.defaultDomeTexture = texture;
    prism::Scene scene = renderer.createScene(options);
    prism::InstanceDesc instance;
    instance.prototype = scene.addMesh(quad(false));
    scene.addInstance(instance);
    scene.commit();
    SceneManager& manager = prism::detail::SceneAccess::committed(scene);
    if (!texture.empty())
        check(dynamic_cast<TransformedEnvironmentMap*>(manager.environment) != nullptr,
              "an unlit scene gets the default dome");
    // Two domes add up.
    prism::EnvironmentDesc dome;
    dome.kind = prism::EnvironmentDesc::Kind::Dome;
    scene.addEnvironment(dome);
    scene.addEnvironment(dome);
    scene.commit();
    check(dynamic_cast<CompositeEnvironment*>(prism::detail::SceneAccess::committed(scene).environment) != nullptr,
          "two domes make a composite environment");
}

void materials(prism::Renderer const& renderer)
{
    if (renderer.context()->options().materials.materialxLibraries.empty()) {
        std::cout << "MaterialX checks skipped (no PRISM_MATERIALX_LIBRARY_PATH)\n";
        return;
    }
    prism::Scene scene = renderer.createScene();
    prism::MaterialDesc good;
    good.kind = prism::MaterialDesc::Kind::MaterialXString;
    good.name = "red standard_surface";
    good.document = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="surface" type="surfaceshader">
    <input name="base_color" type="color3" value="0.8, 0.1, 0.1" />
  </standard_surface>
  <surfacematerial name="Red" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="surface" />
  </surfacematerial>
</materialx>)";
    prism::MaterialDesc broken = good;
    broken.name = "broken";
    broken.document = "<materialx version=\"1.39\"><surfacematerial name=";  // not XML
    prism::InstanceDesc instance;
    instance.prototype = scene.addMesh(quad(false));
    instance.materials = {scene.addMaterial(good)};
    scene.addInstance(instance);
    instance.materials = {scene.addMaterial(broken)};
    instance.transform = prism::Constant(translate(5, 0, 0));
    scene.addInstance(instance);
    scene.commit();
    SceneManager& manager = prism::detail::SceneAccess::committed(scene);
    Material* compiled = manager.getMaterial(cast(manager, Float3(0, 0, 2), Float3(0, 0, -1)));
    Material* failed = manager.getMaterial(cast(manager, Float3(5, 0, 2), Float3(0, 0, -1)));
    check(compiled && dynamic_cast<ErrorMaterial*>(compiled) == nullptr, "a MaterialX string compiles");
    check(failed == ErrorMaterial::Shared(), "a broken document renders with the error material");
    bool diagnosed = false;
    for (auto const& diagnostic : scene.diagnostics())
        diagnosed = diagnosed || (diagnostic.category == "material" && diagnostic.message.find("broken") != std::string::npos);
    check(diagnosed, "the failure is a diagnostic of the commit");
    // A missing file is an error of the commit.
    prism::MaterialDesc missing;
    missing.kind = prism::MaterialDesc::Kind::MaterialXFile;
    missing.path = "does-not-exist.mtlx";
    scene.addMaterial(missing);
    check(throws(prism::ErrorCode::NotFound, [&] { scene.commit(); }), "a missing MaterialX file throws NotFound");
}

void misuse(prism::Renderer const& renderer)
{
    prism::Scene scene = renderer.createScene();
    check(throws(prism::ErrorCode::InvalidArgument, [&] { scene.addMesh(prism::MeshDesc{}); }), "a mesh needs points");
    check(throws(prism::ErrorCode::InvalidArgument, [&] {
              prism::MeshDesc mesh = quad(true);
              std::vector<int32_t> indices{0, 1};
              mesh.faceVertexIndices = view(indices, 1, prism::DataType::Int32);
              scene.addMesh(mesh);
          }),
          "a triangle list needs three indices per triangle");
    check(throws(prism::ErrorCode::InvalidArgument, [&] {
              prism::MeshDesc mesh = quad(true);
              std::vector<float> normals{0, 0};
              mesh.normals = view(normals, 2, prism::DataType::Float32);
              scene.addMesh(mesh);
          }),
          "normals need three components");
    check(throws(prism::ErrorCode::InvalidArgument, [&] {
              prism::BufferView points;
              points.count = 4;
              points.components = 3;
              prism::MeshDesc mesh;
              mesh.points = prism::Constant(points);
              scene.addMesh(mesh);
          }),
          "a view without data is rejected");
    check(throws(prism::ErrorCode::InvalidHandle, [&] {
              prism::InstanceDesc instance;
              instance.prototype = prism::MeshId{12345};
              scene.addInstance(instance);
          }),
          "an instance of an unknown mesh");
    const prism::MeshId mesh = scene.addMesh(quad(false));
    prism::InstanceDesc instance;
    instance.prototype = mesh;
    const prism::InstanceId placed = scene.addInstance(instance);
    scene.remove(placed);
    check(throws(prism::ErrorCode::InvalidHandle, [&] { scene.setVisible(placed, true); }), "a removed instance");
    check(throws(prism::ErrorCode::InvalidHandle, [&] { scene.remove(placed); }), "removing twice");
    // The slot is reused under a new generation; the old handle stays invalid.
    const prism::InstanceId again = scene.addInstance(instance);
    check(again != placed && throws(prism::ErrorCode::InvalidHandle, [&] { scene.setVisible(placed, true); }),
          "a reused slot does not revive the removed handle");
    // A removed mesh still referenced by an instance fails the commit.
    scene.remove(mesh);
    check(throws(prism::ErrorCode::InvalidHandle, [&] { scene.commit(); }), "an instance of a removed mesh");
    // Out-of-range indices fail the commit, and the committed revision stays.
    prism::Scene bad = renderer.createScene();
    prism::MeshDesc broken = quad(true);
    std::vector<int32_t> indices{0, 1, 9};
    broken.faceVertexIndices = view(indices, 1, prism::DataType::Int32);
    instance.prototype = bad.addMesh(broken);
    bad.addInstance(instance);
    check(throws(prism::ErrorCode::InvalidArgument, [&] { bad.commit(); }) && bad.revision() == 0,
          "a triangle with an index out of range fails the commit");
    check(throws(prism::ErrorCode::InvalidArgument, [&] { prism::detail::SceneAccess::committed(bad); }),
          "an uncommitted scene has no committed revision");
    // Cameras are stored for the sessions.
    prism::CameraDesc camera;
    camera.focalLength = 35;
    const prism::CameraId id = scene.addCamera(camera);
    check(scene.camera(id).focalLength == 35.0f, "a camera reads back");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        // The scene keeps its renderer alive: the renderer handle may go first.
        prism::Scene survivor = [] {
            prism::Renderer temporary(options());
            return temporary.createScene();
        }();
        prism::InstanceDesc instance;
        instance.prototype = survivor.addMesh(quad(false));
        survivor.addInstance(instance);
        check(survivor.commit().triangles == 2, "a scene outlives the renderer handle it came from");

        const prism::Renderer renderer(options());
        buildAndRender(renderer);
        geometry(renderer);
        environments(renderer, argc > 1 ? argv[1] : "");
        materials(renderer);
        misuse(renderer);
    } catch (std::exception const& error) {
        std::cerr << "FAILED with an exception: " << error.what() << '\n';
        return 1;
    }
    if (failures) return 1;
    std::cout << "scene API: all checks passed\n";
    return 0;
}
