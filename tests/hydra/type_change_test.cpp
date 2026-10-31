// Prims that change type (mesh to sphere, sphere to mesh, to an unsupported
// type and back), plain and instanced, and full rebuilds over pooled spheres.
//
// Pooled prototype storage (PrototypePool::buckets) outlives the bank that
// built it: a later bank reuses it. A pooled sphere used to keep the material
// of the bank that built it, and the opaque-occluder classification read that
// material after the bank had died (a dynamic_cast on a freed object; seen
// after a mesh became a sphere: the failed in-place refresh built the sphere
// in the old bank, the rebuilt bank reused it). The crash depended on the
// freed memory, so each check runs several rounds and edits the scene again
// after the type change, so that more banks are prepared over the reused
// storage.
//   prism_hydra_scene_bridge_test type-change [rounds]
#include "integration_fixture.h"
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <pxr/imaging/hd/sphereSchema.h>
#include <pxr/imaging/hd/utils.h>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;

constexpr int kAdded = 8;

HdContainerDataSourceHandle Bound(HdContainerDataSourceHandle source, SdfPath const &material)
{
    if (material.IsEmpty()) return source;
    HdDataSourceBaseHandle binding = HdMaterialBindingSchema::Builder()
        .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(material)).Build();
    TfToken purpose = HdMaterialBindingsSchemaTokens->allPurpose;
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdMaterialBindingsSchemaTokens->materialBindings,
        HdMaterialBindingsSchema::BuildRetained(1, &purpose, &binding)), source);
}

HdDataSourceBaseHandle Constant(VtValue const &value)
{
    return HdPrimvarSchema::Builder().SetPrimvarValue(HdRetainedSampledDataSource::New(value))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant)).Build();
}

// A quad of half-size `half` centred at (x, y, z); `down` winds it to face -z.
HdContainerDataSourceHandle Quad(float x, float y, float z, float half, bool down,
                                 SdfPath const &material = {})
{
    VtVec3fArray corners{{x - half, y - half, z}, {x + half, y - half, z},
                         {x + half, y + half, z}, {x - half, y + half, z}};
    VtIntArray indices = down ? VtIntArray{0, 3, 2, 1} : VtIntArray{0, 1, 2, 3};
    auto points = HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(corners))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->vertex)).Build();
    auto topology = HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New({4}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)).Build();
    return Bound(HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->primvars, HdRetainedContainerDataSource::New(
            TfToken("points"), points, TfToken("displayColor"), Constant(VtValue(VtVec3fArray{GfVec3f(.7f)}))),
        HdMeshSchemaTokens->mesh, HdMeshSchema::Builder().SetTopology(topology).Build()), material);
}

// A sphere at (x, y, z). A display colour and opacity give it a material of
// the bank's own (FallbackMaterial); without them it takes the bank's
// fallback material.
HdContainerDataSourceHandle Ball(float x, float y, float z, double radius, bool displayColor,
                                 SdfPath const &material = {})
{
    GfMatrix4d matrix(1);
    matrix.SetTranslate(GfVec3d(x, y, z));
    auto xform = HdXformSchema::Builder().SetMatrix(
        HdRetainedTypedSampledDataSource<GfMatrix4d>::New(matrix)).Build();
    auto sphere = HdSphereSchema::Builder().SetRadius(
        HdRetainedTypedSampledDataSource<double>::New(radius)).Build();
    if (!displayColor)
        return Bound(HdRetainedContainerDataSource::New(HdSphereSchemaTokens->sphere, sphere,
                                                        HdXformSchemaTokens->xform, xform), material);
    auto primvars = HdRetainedContainerDataSource::New(
        TfToken("displayColor"), Constant(VtValue(VtVec3fArray{GfVec3f(.2f, .5f, .8f)})),
        TfToken("displayOpacity"), Constant(VtValue(VtFloatArray{.5f})));
    return Bound(HdRetainedContainerDataSource::New(HdSphereSchemaTokens->sphere, sphere,
        HdXformSchemaTokens->xform, xform, HdPrimvarsSchemaTokens->primvars, primvars), material);
}

HdContainerDataSourceHandle EmissiveMaterial(SdfPath const &path)
{
    HdMaterialNode preview;
    preview.path = path.AppendChild(TfToken("Preview"));
    preview.identifier = TfToken("UsdPreviewSurface");
    preview.parameters[TfToken("diffuseColor")] = VtValue(GfVec3f(0.f));
    preview.parameters[TfToken("emissiveColor")] = VtValue(GfVec3f(4.f));
    HdMaterialNetworkMap networkMap;
    networkMap.map[TfToken("surface")].nodes = {preview};
    HdDataSourceBaseHandle network = HdUtils::ConvertHdMaterialNetworkToHdMaterialNetworkSchema(networkMap);
    TfToken context = HdMaterialSchemaTokens->universalRenderContext;
    return HdRetainedContainerDataSource::New(HdMaterialSchemaTokens->material,
        HdMaterialSchema::BuildRetained(1, &context, &network));
}

HdContainerDataSourceHandle Instanced(HdContainerDataSourceHandle source, SdfPath const &instancer)
{
    auto by = HdInstancedBySchema::Builder().SetPaths(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({instancer})).Build();
    return HdOverlayContainerDataSource::New(
        HdRetainedContainerDataSource::New(HdInstancedBySchemaTokens->instancedBy, by), source);
}

HdContainerDataSourceHandle Instancer(SdfPath const &prototype, int count)
{
    VtIntArray rows;
    VtVec3fArray positions;
    for (int i = 0; i < count; ++i) {
        rows.push_back(i);
        positions.push_back(GfVec3f(-.6f + .4f * float(i), .3f, 0.f));
    }
    HdDataSourceBaseHandle indices = HdRetainedTypedSampledDataSource<VtIntArray>::New(rows);
    auto topology = HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({prototype}))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(1, &indices)).Build();
    auto translations = HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(positions))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->instance)).Build();
    return HdRetainedContainerDataSource::New(HdInstancerTopologySchemaTokens->instancerTopology, topology,
        HdPrimvarsSchemaTokens->primvars, HdRetainedContainerDataSource::New(
            HdInstancerTokens->instanceTranslations, translations));
}

void UseCpu(Fixture &fixture)
{
    auto settings = fixture.bridge->GetSettings();
    settings.device = prism::device::DevicePreference::Cpu;
    settings.samples = 4;
    fixture.bridge->SetSettings(settings);
}

// Renders and checks that the image is finite and not black.
void RenderChecked(Fixture &fixture, std::string const &what)
{
    fixture.Render();
    const auto rgba = Read<float>(fixture.color);
    double sum = 0;
    for (float v : rgba) {
        if (!std::isfinite(v)) throw std::runtime_error(what + ": non-finite pixel");
        sum += v;
    }
    if (rgba.empty() || !(sum > 0)) throw std::runtime_error(what + ": empty or black image");
}

// More banks over the scene: the floor's points change (a prototype refresh)
// and everything is prepared again.
void EditFloor(Fixture &fixture, SdfPath const &floor, std::string const &what)
{
    for (int i = 1; i <= 3; ++i) {
        fixture.scene->AddPrims({{floor, HdPrimTypeTokens->mesh, Quad(0, 0, -.01f * float(i), 2, false)}});
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype, floor);
        RenderChecked(fixture, what + ", floor edit " + std::to_string(i));
    }
}

void Retype(Fixture &fixture, SdfPath const &path, TfToken const &type,
            HdContainerDataSourceHandle const &source)
{
    fixture.scene->AddPrims({{path, type, source}});
    fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype, path);
}

// A structural addition of meshes, then the seed changes type: the in-place
// prototype refresh fails on the type change and the bank is rebuilt.
void ChangeAfterAdditions(bool seedIsMesh)
{
    const std::string what = seedIsMesh ? "mesh to sphere after additions" : "sphere to mesh after additions";
    const SdfPath floor("/Floor"), material("/Emit"), seed("/Seed");
    Fixture fixture;
    UseCpu(fixture);
    auto mesh = Quad(.9f, .9f, .2f, .05f, true, material);
    auto ball = Ball(.9f, .9f, .2f, .05, false);
    fixture.scene->AddPrims({{floor, HdPrimTypeTokens->mesh, Quad(0, 0, 0, 2, false)},
        {material, HdPrimTypeTokens->material, EmissiveMaterial(material)},
        {seed, seedIsMesh ? HdPrimTypeTokens->mesh : HdPrimTypeTokens->sphere, seedIsMesh ? mesh : ball}});
    RenderChecked(fixture, what + ", initial");
    std::vector<HdRetainedSceneIndex::AddedPrimEntry> added;
    std::vector<StructuralChange> changes;
    SdfPathVector names;
    for (int k = 0; k < kAdded; ++k) {
        const SdfPath name("/Added_" + std::to_string(k));
        names.push_back(name);
        added.push_back({name, HdPrimTypeTokens->mesh,
                         Quad(-.9f + .6f * float(k % 4), -.5f + .9f * float(k / 4), .2f, .05f, true, material)});
        changes.push_back({StructuralChange::Operation::Add, name, {}, HdPrimTypeTokens->mesh});
    }
    fixture.scene->AddPrims(added);
    fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure, names, {}, changes}});
    RenderChecked(fixture, what + ", additions");
    const auto fallbacks = Stat(fixture.bridge->GetStats(), "prototypeUpdateFallbacks");
    Retype(fixture, seed, seedIsMesh ? HdPrimTypeTokens->sphere : HdPrimTypeTokens->mesh,
           seedIsMesh ? ball : mesh);
    RenderChecked(fixture, what + ", type change");
    Require(Stat(fixture.bridge->GetStats(), "prototypeUpdateFallbacks") > fallbacks,
            "The type change did not rebuild the bank");
    EditFloor(fixture, floor, what);
    // And back.
    Retype(fixture, seed, seedIsMesh ? HdPrimTypeTokens->mesh : HdPrimTypeTokens->sphere,
           seedIsMesh ? mesh : ball);
    RenderChecked(fixture, what + ", changed back");
    EditFloor(fixture, floor, what + " and back");
}

// Spheres with their own display materials, then banks built from scratch
// over the same prototype pool.
void FullRebuilds()
{
    const SdfPath floor("/Floor"), light("/Light");
    Fixture fixture;
    UseCpu(fixture);
    std::vector<HdRetainedSceneIndex::AddedPrimEntry> prims{
        {floor, HdPrimTypeTokens->mesh, Quad(0, 0, 0, 2, false)},
        {light, HdPrimTypeTokens->domeLight, Dome()}};
    for (int k = 0; k < 4; ++k)
        prims.push_back({SdfPath("/Ball_" + std::to_string(k)), HdPrimTypeTokens->sphere,
                         Ball(-.6f + .4f * float(k), 0, .2f, .15, k % 2 == 0)});
    fixture.scene->AddPrims(prims);
    RenderChecked(fixture, "full rebuilds, initial");
    for (int i = 0; i < 4; ++i) {
        fixture.bridge->MarkSceneDirty(SceneDirtyKind::Unknown, SdfPath("/Ball_" + std::to_string(i)));
        RenderChecked(fixture, "full rebuild " + std::to_string(i));
    }
    EditFloor(fixture, floor, "full rebuilds");
}

// An instanced prototype that changes type, and back; then to a type the
// delegate does not render (points), and back.
void InstancedChanges()
{
    const SdfPath floor("/Floor"), light("/Light"), instancer("/Instancer"), proto("/Instancer/Proto");
    Fixture fixture;
    UseCpu(fixture);
    auto ball = Instanced(Ball(0, 0, .2f, .12, true), instancer);
    auto mesh = Instanced(Quad(0, 0, .2f, .1f, true), instancer);
    fixture.scene->AddPrims({{floor, HdPrimTypeTokens->mesh, Quad(0, 0, 0, 2, false)},
        {light, HdPrimTypeTokens->domeLight, Dome()},
        {instancer, HdPrimTypeTokens->instancer, Instancer(proto, 4)},
        {proto, HdPrimTypeTokens->sphere, ball}});
    RenderChecked(fixture, "instanced sphere, initial");
    Retype(fixture, proto, HdPrimTypeTokens->mesh, mesh);
    RenderChecked(fixture, "instanced sphere to mesh");
    EditFloor(fixture, floor, "instanced sphere to mesh");
    Retype(fixture, proto, HdPrimTypeTokens->sphere, ball);
    RenderChecked(fixture, "instanced mesh to sphere");
    EditFloor(fixture, floor, "instanced mesh to sphere");
    Retype(fixture, proto, HdPrimTypeTokens->points, Instanced(HdRetainedContainerDataSource::New(), instancer));
    RenderChecked(fixture, "instanced sphere to points");
    Retype(fixture, proto, HdPrimTypeTokens->sphere, ball);
    RenderChecked(fixture, "instanced points to sphere");
    EditFloor(fixture, floor, "instanced points to sphere");
}

// A plain sphere that becomes an unsupported type and comes back.
void UnsupportedAndBack()
{
    const SdfPath floor("/Floor"), light("/Light"), ball("/Ball");
    Fixture fixture;
    UseCpu(fixture);
    auto source = Ball(0, 0, .2f, .2, true);
    fixture.scene->AddPrims({{floor, HdPrimTypeTokens->mesh, Quad(0, 0, 0, 2, false)},
        {light, HdPrimTypeTokens->domeLight, Dome()}, {ball, HdPrimTypeTokens->sphere, source}});
    RenderChecked(fixture, "sphere, initial");
    Retype(fixture, ball, HdPrimTypeTokens->points, HdRetainedContainerDataSource::New());
    RenderChecked(fixture, "sphere to points");
    Retype(fixture, ball, HdPrimTypeTokens->sphere, source);
    RenderChecked(fixture, "points to sphere");
    EditFloor(fixture, floor, "points to sphere");
}
} // namespace

int RunTypeChangeTests(int rounds)
{
    try {
        for (int round = 0; round < rounds; ++round) {
            ChangeAfterAdditions(true);
            ChangeAfterAdditions(false);
            FullRebuilds();
            InstancedChanges();
            UnsupportedAndBack();
        }
        std::cout << "Type change tests passed (" << rounds << " rounds)\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
