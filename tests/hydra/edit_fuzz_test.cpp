// A seeded random sequence of scene edits (transforms, visibility, material
// rebinds, light changes, a prim that turns from a mesh into a sphere and
// back; sometimes two in one update) applied through the
// Hydra delegate (a USD stage rendered by UsdImagingGLEngine with hdPrism)
// and through the public Scene (its edit calls and commit), with a CPU image
// after every update:
// - each front end's incrementally updated image equals, bit for bit, a fresh
//   build of the same state (the delegate: a new engine over the edited
//   stage; the scene: a new Scene from the same description);
// - the two front ends agree within the noise floor: their difference is at
//   most that of two seeds of the scene, and their means agree within 1%.
//   Not bit for bit, for a structural reason: the delegate places every mesh
//   through an Embree instance while the scene bakes a mesh with one instance
//   into world space, so hits differ in their last bits and the paths
//   decorrelate after a bounce or two (the CLI-vs-Hydra parity fixtures
//   compare within a tolerance for the same reason).
//   prism_hydra_edit_fuzz [seed] [updates]
#include <prism/renderer.h>
#include <prism/scene.h>
#include <prism/session.h>
#include <prism/usd.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/types.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

constexpr int kWidth = 64, kHeight = 48, kSamples = 16, kMaxDepth = 6, kRouletteDepth = 3, kSeed = 23;
constexpr int kBoxes = 4, kMaterials = 3, kLights = 2;
// The noise floor: the scene's image against one with another seed (two
// independent estimates). The front ends' paths decorrelate after a few
// bounces (their floats differ), so their difference may reach that floor
// but not exceed it, and their means agree.
constexpr double kNoiseFactor = 1.25, kMeanTolerance = 0.01;

int failures = 0;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

// SplitMix64: the same sequence on every platform and standard library.
struct Random {
    uint64_t state;
    uint64_t next()
    {
        uint64_t z = (state += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    double uniform() { return double(next() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
    double between(double a, double b) { return a + (b - a) * uniform(); }
    int below(int n) { return int(next() % uint64_t(n)); }
};

// ── The scene's state, which both front ends build and edit.

struct Box {
    double x = 0, z = 0, yaw = 0, scale = 1;
    int material = 0;
    bool visible = true;
};
struct Light {
    double x = 0, y = 3, z = 0;
    float intensity = 40.0f;
    GfVec3f color{1.0f};
};
struct State {
    std::array<Box, kBoxes> boxes;
    std::array<Light, kLights> lights;
    // The shape: a box mesh or a sphere of the box's size, at a fixed place
    // with material 1. Its type changes (a USD resync).
    bool shapeIsSphere = false;
};
constexpr double kShapeX = 0.2, kShapeZ = 2.0;
constexpr float kShapeRadius = 0.5f;

const std::array<GfVec3f, kMaterials> kPalette{GfVec3f(0.8f, 0.2f, 0.15f), GfVec3f(0.2f, 0.7f, 0.3f),
                                               GfVec3f(0.25f, 0.35f, 0.85f)};

GfMatrix4d BoxWorld(Box const& box)
{
    GfMatrix4d scale, rotate, translate;
    scale.SetScale(box.scale);
    rotate.SetRotate(GfRotation(GfVec3d(0, 1, 0), box.yaw));
    translate.SetTranslate(GfVec3d(box.x, 0, box.z));
    return scale * rotate * translate;
}

GfMatrix4d LightWorld(Light const& light)
{
    GfMatrix4d translate;
    translate.SetTranslate(GfVec3d(light.x, light.y, light.z));
    return translate;
}

prism::Matrix4d ToPrism(GfMatrix4d const& gf)
{
    prism::Matrix4d m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) m[size_t(r)][c] = gf[r][c];
    return m;
}

State RandomState(Random& random)
{
    State state;
    for (int i = 0; i < kBoxes; ++i) {
        Box& box = state.boxes[size_t(i)];
        box.x = -2.4 + 1.6 * i + random.between(-0.3, 0.3);
        box.z = random.between(-1.5, 1.0);
        box.yaw = random.between(0, 90);
        box.scale = random.between(0.6, 1.2);
        box.material = random.below(kMaterials);
    }
    for (int i = 0; i < kLights; ++i) {
        Light& light = state.lights[size_t(i)];
        light.x = i == 0 ? -1.5 : 2.0;
        light.y = random.between(2.5, 3.5);
        light.z = random.between(-1.0, 1.5);
        light.intensity = float(random.between(30, 60));
    }
    return state;
}

// One update: one edit, sometimes two. Returns what it did.
std::string Edit(State& state, Random& random)
{
    std::string done;
    const int edits = random.below(3) == 0 ? 2 : 1;
    for (int e = 0; e < edits; ++e) {
        if (!done.empty()) done += " + ";
        const int kind = random.below(5);
        const int i = random.below(kind == 3 ? kLights : kBoxes);
        if (kind == 4) {
            state.shapeIsSphere = !state.shapeIsSphere;
            done += state.shapeIsSphere ? "turn the shape into a sphere" : "turn the shape into a mesh";
        } else if (kind == 0) {
            Box& box = state.boxes[size_t(i)];
            box.x = std::clamp(box.x + random.between(-0.5, 0.5), -3.2, 3.2);
            box.z = std::clamp(box.z + random.between(-0.5, 0.5), -2.0, 1.5);
            box.yaw += random.between(-30, 30);
            done += "move box " + std::to_string(i);
        } else if (kind == 1) {
            Box& box = state.boxes[size_t(i)];
            box.visible = !box.visible;
            done += (box.visible ? "show box " : "hide box ") + std::to_string(i);
        } else if (kind == 2) {
            Box& box = state.boxes[size_t(i)];
            box.material = (box.material + 1 + random.below(kMaterials - 1)) % kMaterials;
            done += "bind material " + std::to_string(box.material) + " to box " + std::to_string(i);
        } else {
            Light& light = state.lights[size_t(i)];
            if (random.below(2) == 0) {
                light.intensity = float(std::clamp(double(light.intensity) * random.between(0.6, 1.5), 10.0, 120.0));
                light.color = GfVec3f(float(random.between(0.6, 1.0)), float(random.between(0.6, 1.0)),
                                      float(random.between(0.6, 1.0)));
                done += "change light " + std::to_string(i);
            } else {
                light.x = std::clamp(light.x + random.between(-0.8, 0.8), -3.0, 3.0);
                light.z = std::clamp(light.z + random.between(-0.8, 0.8), -1.5, 2.0);
                done += "move light " + std::to_string(i);
            }
        }
    }
    return done;
}

// A unit box standing on y = 0 (6 quads, outward winding) and the ground.
const std::vector<GfVec3f> kBoxPoints{{-0.5f, 0, -0.5f}, {0.5f, 0, -0.5f}, {0.5f, 0, 0.5f}, {-0.5f, 0, 0.5f},
                                      {-0.5f, 1, -0.5f}, {0.5f, 1, -0.5f}, {0.5f, 1, 0.5f}, {-0.5f, 1, 0.5f}};
const std::vector<int> kBoxIndices{0, 1, 2, 3, 4, 7, 6, 5, 3, 2, 6, 7, 0, 4, 5, 1, 1, 5, 6, 2, 0, 3, 7, 4};
const std::vector<GfVec3f> kGroundPoints{{-5, 0, -4}, {5, 0, -4}, {5, 0, 4}, {-5, 0, 4}};
const std::vector<int> kGroundIndices{0, 3, 2, 1};

std::string MaterialDocument(int index)
{
    GfVec3f const& c = kPalette[size_t(index)];
    std::ostringstream doc;
    doc << "<?xml version=\"1.0\"?>\n<materialx version=\"1.39\">\n"
        << "  <UsdPreviewSurface name=\"Surface\" type=\"surfaceshader\">\n"
        << "    <input name=\"diffuseColor\" type=\"color3\" value=\"" << c[0] << ", " << c[1] << ", " << c[2]
        << "\" />\n    <input name=\"roughness\" type=\"float\" value=\"0.6\" />\n  </UsdPreviewSurface>\n"
        << "  <surfacematerial name=\"M" << index << "\" type=\"material\">\n"
        << "    <input name=\"surfaceshader\" type=\"surfaceshader\" nodename=\"Surface\" />\n"
        << "  </surfacematerial>\n</materialx>\n";
    return doc.str();
}

// ── The delegate's side: a USD stage.

SdfPath BoxPath(int i) { return SdfPath("/World/Box" + std::to_string(i)); }
SdfPath LightPath(int i) { return SdfPath("/World/Light" + std::to_string(i)); }
SdfPath MaterialPath(int i) { return SdfPath("/World/Looks/M" + std::to_string(i)); }
const SdfPath kShapePath("/World/Shape");

GfMatrix4d ShapeWorld(bool sphere)
{
    GfMatrix4d translate;
    translate.SetTranslate(GfVec3d(kShapeX, sphere ? kShapeRadius : 0.0, kShapeZ));
    return translate;
}

void SetTransform(UsdPrim const& prim, GfMatrix4d const& world)
{
    UsdGeomXformable xformable(prim);
    xformable.ClearXformOpOrder();
    xformable.AddTransformOp().Set(world);
}

UsdGeomMesh DefineMesh(UsdStageRefPtr const& stage, SdfPath const& path, std::vector<GfVec3f> const& points,
                       std::vector<int> const& indices)
{
    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, path);
    mesh.CreatePointsAttr(VtValue(VtVec3fArray(points.begin(), points.end())));
    mesh.CreateFaceVertexCountsAttr(VtValue(VtIntArray(indices.size() / 4, 4)));
    mesh.CreateFaceVertexIndicesAttr(VtValue(VtIntArray(indices.begin(), indices.end())));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->none));
    UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim());
    return mesh;
}

// The shape as the state has it: a type change replaces the prim.
void DefineShape(UsdStageRefPtr const& stage, bool sphere)
{
    UsdPrim existing = stage->GetPrimAtPath(kShapePath);
    if (existing && existing.IsA<UsdGeomSphere>() == sphere) return;
    if (existing) stage->RemovePrim(kShapePath);
    UsdPrim prim;
    if (sphere) {
        UsdGeomSphere shape = UsdGeomSphere::Define(stage, kShapePath);
        shape.CreateRadiusAttr(VtValue(double(kShapeRadius)));
        UsdShadeMaterialBindingAPI::Apply(shape.GetPrim());
        prim = shape.GetPrim();
    } else {
        prim = DefineMesh(stage, kShapePath, kBoxPoints, kBoxIndices).GetPrim();
    }
    UsdGeomXformable(prim).AddTransformOp().Set(ShapeWorld(sphere));
    UsdShadeMaterialBindingAPI(prim).Bind(UsdShadeMaterial(stage->GetPrimAtPath(MaterialPath(1))));
}

void Apply(UsdStageRefPtr const& stage, State const& state)
{
    for (int i = 0; i < kBoxes; ++i) {
        Box const& box = state.boxes[size_t(i)];
        UsdPrim prim = stage->GetPrimAtPath(BoxPath(i));
        GfMatrix4d current;
        bool resets = false;
        UsdGeomXformable(prim).GetLocalTransformation(&current, &resets);
        if (current != BoxWorld(box)) SetTransform(prim, BoxWorld(box));
        UsdGeomImageable imageable(prim);
        const bool visible = imageable.ComputeVisibility() != UsdGeomTokens->invisible;
        if (visible != box.visible) {
            if (box.visible) imageable.MakeVisible();
            else imageable.MakeInvisible();
        }
        UsdShadeMaterialBindingAPI binding(prim);
        UsdShadeMaterial material(stage->GetPrimAtPath(MaterialPath(box.material)));
        if (binding.ComputeBoundMaterial().GetPath() != material.GetPath()) binding.Bind(material);
    }
    DefineShape(stage, state.shapeIsSphere);
    for (int i = 0; i < kLights; ++i) {
        Light const& light = state.lights[size_t(i)];
        UsdPrim prim = stage->GetPrimAtPath(LightPath(i));
        UsdLuxLightAPI lux(prim);
        GfMatrix4d current;
        bool resets = false;
        UsdGeomXformable(prim).GetLocalTransformation(&current, &resets);
        if (current != LightWorld(light)) SetTransform(prim, LightWorld(light));
        float intensity = 0;
        GfVec3f color;
        lux.GetIntensityAttr().Get(&intensity);
        lux.GetColorAttr().Get(&color);
        if (intensity != light.intensity) lux.GetIntensityAttr().Set(light.intensity);
        if (color != light.color) lux.GetColorAttr().Set(light.color);
    }
}

UsdStageRefPtr BuildStage(State const& state)
{
    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    stage->DefinePrim(SdfPath("/World"), TfToken("Xform"));
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/World/Camera"));
    GfMatrix4d view;
    view.SetLookAt(GfVec3d(0, 3.5, 7.5), GfVec3d(0, 0.4, 0), GfVec3d(0, 1, 0));
    camera.AddTransformOp().Set(view.GetInverse());
    camera.GetFocalLengthAttr().Set(18.0f);
    camera.GetClippingRangeAttr().Set(GfVec2f(0.1f, 100.0f));
    for (int i = 0; i < kMaterials; ++i) {
        UsdShadeMaterial material = UsdShadeMaterial::Define(stage, MaterialPath(i));
        UsdShadeShader shader = UsdShadeShader::Define(stage, MaterialPath(i).AppendChild(TfToken("Surface")));
        shader.CreateIdAttr(VtValue(TfToken("UsdPreviewSurface")));
        shader.CreateInput(TfToken("diffuseColor"), SdfValueTypeNames->Color3f).Set(kPalette[size_t(i)]);
        shader.CreateInput(TfToken("roughness"), SdfValueTypeNames->Float).Set(0.6f);
        material.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput(TfToken("surface"), SdfValueTypeNames->Token));
    }
    auto mesh = [&](SdfPath const& path, std::vector<GfVec3f> const& points, std::vector<int> const& indices) {
        return DefineMesh(stage, path, points, indices);
    };
    UsdGeomMesh ground = mesh(SdfPath("/World/Ground"), kGroundPoints, kGroundIndices);
    UsdShadeMaterialBindingAPI(ground.GetPrim()).Bind(UsdShadeMaterial(stage->GetPrimAtPath(MaterialPath(0))));
    for (int i = 0; i < kBoxes; ++i) {
        UsdGeomMesh box = mesh(BoxPath(i), kBoxPoints, kBoxIndices);
        box.AddTransformOp().Set(BoxWorld(state.boxes[size_t(i)]));
    }
    for (int i = 0; i < kLights; ++i) {
        UsdLuxSphereLight light = UsdLuxSphereLight::Define(stage, LightPath(i));
        light.CreateRadiusAttr(VtValue(0.3f));
        UsdLuxLightAPI lux(light.GetPrim());
        lux.CreateIntensityAttr(VtValue(state.lights[size_t(i)].intensity));
        lux.CreateColorAttr(VtValue(state.lights[size_t(i)].color));
        UsdGeomXformable(light.GetPrim()).AddTransformOp().Set(LightWorld(state.lights[size_t(i)]));
    }
    Apply(stage, state);
    return stage;
}

std::unique_ptr<UsdImagingGLEngine> MakeEngine()
{
    UsdImagingGLEngine::Parameters parameters;
    parameters.gpuEnabled = false;
    parameters.rendererPluginId = TfToken("HdPrismRendererPlugin");
    auto engine = std::make_unique<UsdImagingGLEngine>(parameters);
    if (engine->GetCurrentRendererId() != parameters.rendererPluginId)
        throw std::runtime_error("PrismRT plugin not loaded; check PXR_PLUGINPATH_NAME and DLL paths");
    engine->SetCameraPath(SdfPath("/World/Camera"));
    engine->SetRenderViewport(GfVec4d(0, 0, kWidth, kHeight));
    auto set = [&](const char* name, VtValue value) { engine->SetRendererSetting(TfToken(name), value); };
    set("prism:device", VtValue(TfToken("cpu")));
    set("prism:samplesPerPixel", VtValue(kSamples));
    set("prism:maxDepth", VtValue(kMaxDepth));
    set("prism:rrMinDepth", VtValue(kRouletteDepth));
    set("prism:seed", VtValue(kSeed));
    set("prism:adaptiveThreshold", VtValue(0.0f));
    set("prism:maxSampleRadiance", VtValue(0.0f));
    set("prism:lightSelection", VtValue(TfToken("spatial")));
    engine->SetEnablePresentation(false);
    engine->SetRendererAovs({HdAovTokens->color});
    return engine;
}

// The colour (RGBA, top row first) once converged.
std::vector<float> Render(UsdImagingGLEngine& engine, UsdStageRefPtr const& stage)
{
    UsdImagingGLRenderParams params;
    params.enableSceneMaterials = true;
    params.enableSceneLights = true;
    const auto start = std::chrono::steady_clock::now();
    do {
        engine.Render(stage->GetPseudoRoot(), params);
        if (std::chrono::steady_clock::now() - start > std::chrono::minutes(5))
            throw std::runtime_error("the delegate did not converge within 5 minutes");
        if (!engine.IsConverged()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (!engine.IsConverged());
    HdRenderBuffer* buffer = engine.GetAovRenderBuffer(HdAovTokens->color);
    if (!buffer || buffer->GetFormat() != HdFormatFloat32Vec4 || buffer->GetWidth() != unsigned(kWidth) ||
        buffer->GetHeight() != unsigned(kHeight))
        throw std::runtime_error("missing or invalid colour AOV");
    buffer->Resolve();
    auto const* data = static_cast<float const*>(buffer->Map());
    std::vector<float> rgba(size_t(kWidth) * kHeight * 4);
    // Hydra's rows are bottom-up.
    for (int y = 0; y < kHeight; ++y)
        std::copy_n(data + size_t(kHeight - 1 - y) * kWidth * 4, size_t(kWidth) * 4, rgba.data() + size_t(y) * kWidth * 4);
    buffer->Unmap();
    return rgba;
}

// ── The public scene's side.

struct Built {
    prism::Scene scene;
    std::array<prism::MaterialId, kMaterials> materials;
    std::array<prism::InstanceId, kBoxes> boxes;
    std::array<prism::LightId, kLights> lights;
    prism::MeshId shapeMesh;
    prism::SpheresId shapeSphere;
    prism::InstanceId shape;
    bool shapeIsSphere = false;
};

prism::MeshDesc MeshOf(std::vector<GfVec3f> const& points, std::vector<int> const& indices)
{
    auto p = std::make_shared<std::vector<float>>();
    for (auto const& point : points) p->insert(p->end(), {point[0], point[1], point[2]});
    auto counts = std::make_shared<std::vector<int32_t>>(indices.size() / 4, 4);
    auto faces = std::make_shared<std::vector<int32_t>>(indices.begin(), indices.end());
    prism::MeshDesc mesh;
    mesh.points = prism::Constant(prism::BufferView::Of(std::shared_ptr<const std::vector<float>>(p), 3,
                                                        prism::DataType::Float32));
    mesh.faceVertexCounts = prism::BufferView::Of(std::shared_ptr<const std::vector<int32_t>>(counts), 1,
                                                  prism::DataType::Int32);
    mesh.faceVertexIndices = prism::BufferView::Of(std::shared_ptr<const std::vector<int32_t>>(faces), 1,
                                                   prism::DataType::Int32);
    mesh.subdivision.scheme = prism::SubdivisionDesc::Scheme::None;
    return mesh;
}

prism::LightDesc LightOf(Light const& light)
{
    prism::LightDesc desc;
    desc.shape = prism::LightDesc::Shape::Sphere;
    desc.radius = 0.3f;
    desc.intensity = light.intensity;
    desc.color = Color3(light.color[0], light.color[1], light.color[2]);
    desc.transform = prism::Constant(ToPrism(LightWorld(light)));
    return desc;
}

prism::SpheresDesc SphereOf(float radius)
{
    auto centers = std::make_shared<std::vector<float>>(std::vector<float>{0, 0, 0});
    auto radii = std::make_shared<std::vector<float>>(std::vector<float>{radius});
    prism::SpheresDesc desc;
    desc.centers = prism::Constant(prism::BufferView::Of(std::shared_ptr<const std::vector<float>>(centers), 3,
                                                         prism::DataType::Float32));
    desc.radii = prism::BufferView::Of(std::shared_ptr<const std::vector<float>>(radii), 1, prism::DataType::Float32);
    return desc;
}

prism::InstanceDesc ShapeOf(Built const& built, bool sphere)
{
    prism::InstanceDesc desc;
    if (sphere) desc.prototype = built.shapeSphere;
    else desc.prototype = built.shapeMesh;
    desc.transform = prism::Constant(ToPrism(ShapeWorld(sphere)));
    desc.materials = {built.materials[1]};
    return desc;
}

prism::InstanceDesc BoxOf(Built const& built, prism::MeshId mesh, Box const& box)
{
    prism::InstanceDesc desc;
    desc.prototype = mesh;
    desc.transform = prism::Constant(ToPrism(BoxWorld(box)));
    desc.materials = {built.materials[size_t(box.material)]};
    desc.visible = box.visible;
    return desc;
}

Built BuildScene(prism::Renderer const& renderer, State const& state)
{
    Built built{renderer.createScene()};
    prism::Scene& scene = built.scene;
    for (int i = 0; i < kMaterials; ++i) {
        prism::MaterialDesc material;
        material.kind = prism::MaterialDesc::Kind::MaterialXString;
        material.document = MaterialDocument(i);
        material.name = "M" + std::to_string(i);
        built.materials[size_t(i)] = scene.addMaterial(material);
    }
    prism::InstanceDesc ground;
    ground.prototype = scene.addMesh(MeshOf(kGroundPoints, kGroundIndices));
    ground.materials = {built.materials[0]};
    scene.addInstance(ground);
    // A mesh per box, as USD has a prim per box.
    for (int i = 0; i < kBoxes; ++i)
        built.boxes[size_t(i)] =
            scene.addInstance(BoxOf(built, scene.addMesh(MeshOf(kBoxPoints, kBoxIndices)), state.boxes[size_t(i)]));
    built.shapeMesh = scene.addMesh(MeshOf(kBoxPoints, kBoxIndices));
    built.shapeSphere = scene.addSpheres(SphereOf(kShapeRadius));
    built.shapeIsSphere = state.shapeIsSphere;
    built.shape = scene.addInstance(ShapeOf(built, state.shapeIsSphere));
    for (int i = 0; i < kLights; ++i) built.lights[size_t(i)] = scene.addLight(LightOf(state.lights[size_t(i)]));
    scene.commit();
    return built;
}

void ApplyScene(Built& built, State const& state)
{
    for (int i = 0; i < kBoxes; ++i) {
        Box const& box = state.boxes[size_t(i)];
        prism::InstanceId id = built.boxes[size_t(i)];
        built.scene.setTransform(id, prism::Constant(ToPrism(BoxWorld(box))));
        built.scene.setVisible(id, box.visible);
        built.scene.setMaterials(id, {built.materials[size_t(box.material)]});
    }
    if (built.shapeIsSphere != state.shapeIsSphere) {
        built.shapeIsSphere = state.shapeIsSphere;
        built.scene.updateInstance(built.shape, ShapeOf(built, state.shapeIsSphere));
    }
    for (int i = 0; i < kLights; ++i) built.scene.updateLight(built.lights[size_t(i)], LightOf(state.lights[size_t(i)]));
    built.scene.commit();
}

// The colour (RGB, top row first).
std::vector<float> RenderScene(prism::Renderer const& renderer, Built const& built, prism::CameraDesc const& camera,
                               uint32_t seed = kSeed)
{
    prism::SessionOptions options;
    options.device = prism::DevicePreference::Cpu;
    options.width = kWidth;
    options.height = kHeight;
    options.cameraDesc = camera;
    options.settings.samples = kSamples;
    options.settings.adaptiveThreshold = 0.0f;
    options.settings.maxDepth = kMaxDepth;
    options.settings.rouletteDepth = kRouletteDepth;
    options.settings.seed = seed;
    options.settings.sampleClamp = 0.0f;
    prism::Session session = renderer.createSession(built.scene, options);
    prism::Image const image = session.render().aov(prism::Aov::Color);
    return std::vector<float>(image.floats(), image.floats() + image.valueCount());
}

// ── Comparisons.

std::vector<float> Rgb(std::vector<float> const& rgba)
{
    std::vector<float> rgb;
    rgb.reserve(rgba.size() / 4 * 3);
    for (size_t i = 0; i < rgba.size(); i += 4) rgb.insert(rgb.end(), {rgba[i], rgba[i + 1], rgba[i + 2]});
    return rgb;
}

size_t Differing(std::vector<float> const& a, std::vector<float> const& b)
{
    if (a.size() != b.size()) return std::max(a.size(), b.size());
    size_t count = 0;
    for (size_t i = 0; i < a.size(); ++i) count += std::memcmp(&a[i], &b[i], sizeof(float)) != 0;
    return count;
}

// prism_compare_images's normalized RMSE: sqrt(sum (a - b)^2 / sum b^2).
double Nrmse(std::vector<float> const& actual, std::vector<float> const& reference)
{
    double error = 0, energy = 0;
    for (size_t i = 0; i < actual.size() && i < reference.size(); ++i) {
        const double d = double(actual[i]) - reference[i];
        error += d * d;
        energy += double(reference[i]) * reference[i];
    }
    return std::sqrt(error / std::max(energy, 1e-20));
}

// PRISM_EDIT_FUZZ_IMAGES=<directory>: each image as a PPM (exposure 4,
// gamma 2.2) for a look at a failure.
void WriteImage(std::string const& name, std::vector<float> const& pixels, int channels)
{
    const char* directory = std::getenv("PRISM_EDIT_FUZZ_IMAGES");
    if (!directory || !*directory) return;
    std::ofstream out(std::string(directory) + "/" + name + ".ppm", std::ios::binary);
    out << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
    for (size_t p = 0; p < size_t(kWidth) * kHeight; ++p)
        for (int c = 0; c < 3; ++c) {
            const float v = std::pow(std::clamp(4.0f * pixels[p * size_t(channels) + size_t(c)], 0.0f, 1.0f), 1.0f / 2.2f);
            out.put(char(uint8_t(v * 255.0f + 0.5f)));
        }
}

double Mean(std::vector<float> const& values)
{
    double sum = 0;
    for (float v : values) sum += v;
    return values.empty() ? 0.0 : sum / double(values.size());
}

} // namespace

int main(int argc, char** argv)
{
    const uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
    const int updates = argc > 2 ? std::atoi(argv[2]) : 20;
    try {
        Random random{seed};
        State state = RandomState(random);
        prism::RendererOptions rendererOptions = prism::RendererOptions::FromEnvironment();
        rendererOptions.devices.disableVulkan = true;
        prism::Renderer renderer(rendererOptions);

        UsdStageRefPtr stage = BuildStage(state);
        const prism::CameraDesc camera = prism::usd::LoadCamera(stage, SdfPath("/World/Camera"));
        auto engine = MakeEngine();
        Built built = BuildScene(renderer, state);
        std::cout << "seed " << seed << ", " << updates << " updates\n";

        for (int step = 0; step <= updates; ++step) {
            std::string what = "initial state";
            if (step > 0) {
                what = Edit(state, random);
                Apply(stage, state);
                ApplyScene(built, state);
            }
            const auto start = std::chrono::steady_clock::now();
            const std::vector<float> hydra = Render(*engine, stage);
            std::vector<float> hydraFresh;
            {
                auto fresh = MakeEngine();
                hydraFresh = Render(*fresh, stage);
            }
            const std::vector<float> scene = RenderScene(renderer, built, camera);
            const std::vector<float> sceneFresh = RenderScene(renderer, BuildScene(renderer, state), camera);
            const std::vector<float> hydraRgb = Rgb(hydra);
            const double cross = Nrmse(hydraRgb, scene);
            const double noise = Nrmse(RenderScene(renderer, built, camera, kSeed + 1), scene);
            const double meanError = std::abs(Mean(hydraRgb) - Mean(scene)) / std::max(Mean(scene), 1e-6);
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << "update " << step << " (" << what << "): delegate vs fresh " << Differing(hydra, hydraFresh)
                      << " values differ, scene vs fresh " << Differing(scene, sceneFresh)
                      << ", delegate vs scene NRMSE " << cross << " (noise floor " << noise << "), means "
                      << Mean(hydraRgb) << " and " << Mean(scene) << ", " << seconds << " s" << std::endl;
            WriteImage("update" + std::to_string(step) + "_delegate", hydra, 4);
            WriteImage("update" + std::to_string(step) + "_scene", scene, 3);
            check(Mean(scene) > 1e-3, "update " + std::to_string(step) + ": the scene's image is black");
            check(Differing(hydra, hydraFresh) == 0,
                  "update " + std::to_string(step) + ": the delegate's updated image differs from a fresh build");
            check(Differing(scene, sceneFresh) == 0,
                  "update " + std::to_string(step) + ": the scene's updated image differs from a fresh build");
            check(cross <= kNoiseFactor * noise && meanError <= kMeanTolerance, "update " + std::to_string(step) +
                  ": the delegate and the scene differ beyond the noise floor");
        }
    } catch (std::exception const& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "Edit fuzz checks passed\n";
    return 0;
}
