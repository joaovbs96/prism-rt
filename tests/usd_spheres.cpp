#include "../src/scene.h"
#include "../src/mesh/usd_mesh.h"
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <fstream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
Float3 v(const GfVec3d& p) { return Float3(float(p[0]), float(p[1]), float(p[2])); }
void run(RTCDevice device) {
    const auto path = std::filesystem::current_path() / "analytic-spheres.usda";
    auto stage = UsdStage::CreateNew(path.string());
    auto ground = UsdGeomSphere::Define(stage, SdfPath("/Ground"));
    ground.CreateRadiusAttr(VtValue(1000.0));
    ground.AddTranslateOp().Set(GfVec3d(0, -1000, 0));
    auto ellipse = UsdGeomSphere::Define(stage, SdfPath("/Ellipsoid"));
    GfMatrix4d matrix(1); matrix.SetScale(GfVec3d(-2, 3, 1));
    matrix.SetTranslateOnly(GfVec3d(5, 4, 0));
    ellipse.AddTransformOp().Set(matrix);
    auto instancer = UsdGeomPointInstancer::Define(stage, SdfPath("/Points"));
    auto prototype = UsdGeomSphere::Define(stage, SdfPath("/Points/Prototype"));
    prototype.CreateRadiusAttr(VtValue(0.5));
    instancer.CreatePrototypesRel().SetTargets({prototype.GetPath()});
    instancer.CreateProtoIndicesAttr().Set(VtIntArray{0,0});
    instancer.CreatePositionsAttr().Set(VtVec3fArray{{-5,1,0},{-7,1,0}});
    stage->GetRootLayer()->Save();
    stage.Reset();

    USDMesh imported(path);
    require(imported.vertices.empty() && imported.sphereCount() == 4,
            "USD spheres must remain analytic, including point instances");
    SceneManager scene(device, prism::color::ColorManagement::Create({}));
    prism::usd::AttachImportedMesh(scene, imported);
    scene.commitScene();
    struct Release { RTCScene scene; ~Release() { rtcReleaseScene(scene); } } release{scene.scene};
    const auto hit = scene.cast_ray(initialize_ray(Float3(0,2,0), Float3(0,-1,0)));
    require(hit.geomid != RTC_INVALID_GEOMETRY_ID && std::abs(hit.p.y()) < 0.001f,
            "Large analytic ground must reach y=0");
    require(hit.front_face && hit.n.y() > 0.999f, "Ground outward normal is incorrect");

    const GfVec3d local(0.6,0.8,0);
    const auto point = matrix.Transform(local);
    const auto normal = matrix.GetInverse().GetTranspose().TransformDir(local).GetNormalized();
    const auto transformed = scene.cast_ray(initialize_ray(v(point + normal*2), v(-normal)));
    require(transformed.geomid != RTC_INVALID_GEOMETRY_ID &&
            (transformed.p-v(point)).length() < 0.001f,
            "Nonuniform mirrored sphere must intersect as an ellipsoid");
    require(transformed.front_face && (transformed.n-v(normal)).length() < 0.001f,
            "Reflection must not invert analytic outward normals");
    require(std::abs(transformed.u - 0.5f) < 0.001f &&
            std::abs(transformed.v - std::acos(0.8f)/PI) < 0.001f,
            "Sphere UVs must remain in object space under an affine placement");
    const auto inside = scene.cast_ray(initialize_ray(v(matrix.Transform(GfVec3d(0))), v(normal)));
    require(!inside.front_face, "Inside ellipsoid ray must classify as a back-face hit");
    const auto a = scene.cast_ray(initialize_ray(Float3(-5,1,3), Float3(0,0,-1)));
    const auto b = scene.cast_ray(initialize_ray(Float3(-7,1,3), Float3(0,0,-1)));
    require(a.geomid != b.geomid && scene.geometry.at(a.geomid) == scene.geometry.at(b.geomid),
            "Sphere placements must have distinct IDs and share prototype storage");
}
}
int main() {
    RTCDevice device = rtcNewDevice(nullptr);
    try { run(device); rtcReleaseDevice(device); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; rtcReleaseDevice(device); return 1; }
}
