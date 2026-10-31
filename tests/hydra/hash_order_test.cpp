// Edits must not depend on the hash order of paths.
//
// SdfPath hashes come from the path pool's handles, so an unordered set of
// paths lists them in an order that changes with the order in which the paths
// were created (from process to process: stage loading is multithreaded).
// Placement edits and structural additions attach new Embree placements, and
// the light table lists emissive triangles by placement id: placements
// attached in hash order changed which emitter each light sample picks. The
// capture adds new prims to the retained scene in the order it is given, and
// a bank rebuilt from the retained scene walks them in that order.
//
// Each check builds the same scene and applies the same edit under several
// sets of prim names. The names differ, so their hash orders differ, but
// their sorted order is the same; the renders must be bit-identical.
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/materialBindingSchema.h>
#include <pxr/imaging/hd/materialBindingsSchema.h>
#include <pxr/imaging/hd/materialSchema.h>
#include <pxr/imaging/hd/utils.h>
#include "integration_fixture.h"
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
#include <pxr/imaging/hd/overlayContainerDataSource.h>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace pxr;
using namespace prism::hydra;
using namespace prism::hydra::test;

constexpr int kEmitters = 8;

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
    auto color = HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New({GfVec3f(.7f)}))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->constant)).Build();
    auto topology = HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New({4}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New(indices)).Build();
    auto mesh = HdRetainedContainerDataSource::New(
        HdPrimvarsSchemaTokens->primvars, HdRetainedContainerDataSource::New(
            TfToken("points"), points, TfToken("displayColor"), color),
        HdMeshSchemaTokens->mesh, HdMeshSchema::Builder().SetTopology(topology).Build());
    if (material.IsEmpty()) return mesh;
    HdDataSourceBaseHandle binding = HdMaterialBindingSchema::Builder()
        .SetPath(HdRetainedTypedSampledDataSource<SdfPath>::New(material)).Build();
    TfToken purpose = HdMaterialBindingsSchemaTokens->allPurpose;
    return HdOverlayContainerDataSource::New(HdRetainedContainerDataSource::New(
        HdMaterialBindingsSchemaTokens->materialBindings,
        HdMaterialBindingsSchema::BuildRetained(1, &purpose, &binding)), mesh);
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

// Emitter k's position: spread over the floor, in the order of k.
GfVec2f Spot(int k) { return GfVec2f(-.9f + .6f * float(k % 4), -.5f + .9f * float(k / 4)); }

void UseCpu(Fixture &fixture)
{
    auto settings = fixture.bridge->GetSettings();
    settings.device = prism::device::DevicePreference::Cpu;
    settings.samples = 16;
    fixture.bridge->SetSettings(settings);
}

// The paths of one name set. Created in reverse order, so that the pool hands
// out their handles in another order than the sorted one.
std::vector<SdfPath> Names(std::string const &prefix)
{
    std::vector<SdfPath> paths(kEmitters);
    for (int k = kEmitters - 1; k >= 0; --k)
        paths[k] = SdfPath("/" + prefix + "_" + std::to_string(k));
    return paths;
}

// Structural additions: the emitters arrive in one structural batch.
std::vector<float> RenderAdded(std::string const &prefix, std::vector<float> *before,
                               std::vector<float> *rebuilt)
{
    const auto names = Names(prefix);
    const SdfPath material("/" + prefix + "_Emit");
    Fixture fixture;
    UseCpu(fixture);
    fixture.scene->AddPrims({{SdfPath("/" + prefix + "_Floor"), HdPrimTypeTokens->mesh, Quad(0, 0, 0, 2, false)},
        {material, HdPrimTypeTokens->material, EmissiveMaterial(material)},
        {SdfPath("/" + prefix + "_Seed"), HdPrimTypeTokens->mesh, Quad(.9f, .9f, .2f, .05f, true, material)}});
    fixture.Render();
    *before = Read<float>(fixture.color);
    std::vector<HdRetainedSceneIndex::AddedPrimEntry> added;
    std::vector<StructuralChange> changes;
    for (int k = 0; k < kEmitters; ++k) {
        added.push_back({names[k], HdPrimTypeTokens->mesh,
                         Quad(Spot(k)[0], Spot(k)[1], .2f, .05f, true, material)});
        changes.push_back({StructuralChange::Operation::Add, names[k], {}, HdPrimTypeTokens->mesh});
    }
    fixture.scene->AddPrims(added);
    fixture.bridge->MarkSceneDirtyBatch({{SceneDirtyKind::Structure,
        SdfPathVector(names.begin(), names.end()), {}, changes}});
    fixture.Render();
    Require(Stat(fixture.bridge->GetStats(), "lastStructuralPlacementsAttached") == kEmitters,
            "The emitters were not added as a structural leaf update");
    const auto result = Read<float>(fixture.color);
    // A mesh that loses its faces cannot be refreshed in place: the bank is
    // built again from the retained scene, which lists the added emitters in
    // the order the capture added them.
    const SdfPath seed("/" + prefix + "_Seed");
    const auto fallbacks = Stat(fixture.bridge->GetStats(), "prototypeUpdateFallbacks");
    auto empty = HdMeshTopologySchema::Builder()
        .SetFaceVertexCounts(HdRetainedTypedSampledDataSource<VtIntArray>::New({}))
        .SetFaceVertexIndices(HdRetainedTypedSampledDataSource<VtIntArray>::New({})).Build();
    fixture.scene->AddPrims({{seed, HdPrimTypeTokens->mesh, HdRetainedContainerDataSource::New(
        HdMeshSchemaTokens->mesh, HdMeshSchema::Builder().SetTopology(empty).Build())}});
    fixture.bridge->MarkSceneDirty(SceneDirtyKind::Prototype, seed);
    fixture.Render();
    Require(Stat(fixture.bridge->GetStats(), "prototypeUpdateFallbacks") > fallbacks,
            "The prototype change did not rebuild the bank");
    *rebuilt = Read<float>(fixture.color);
    return result;
}

// One instancer, one emissive prototype per name; the edit adds a second
// instance of every prototype.
HdContainerDataSourceHandle Instances(std::vector<SdfPath> const &prototypes, int perPrototype)
{
    // Instance i of prototype k is row i * count + k, so the edit keeps the
    // existing rows and appends the new ones.
    const size_t count = prototypes.size();
    std::vector<HdDataSourceBaseHandle> indices;
    VtVec3fArray positions(count * size_t(perPrototype));
    for (size_t k = 0; k < count; ++k) {
        VtIntArray rows;
        for (int i = 0; i < perPrototype; ++i) {
            const size_t row = size_t(i) * count + k;
            rows.push_back(int(row));
            positions[row] = GfVec3f(Spot(int(k))[0], Spot(int(k))[1] + .2f * float(i), 0.f);
        }
        indices.push_back(HdRetainedTypedSampledDataSource<VtIntArray>::New(rows));
    }
    auto topology = HdInstancerTopologySchema::Builder()
        .SetPrototypes(HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New(
            VtArray<SdfPath>(prototypes.begin(), prototypes.end())))
        .SetInstanceIndices(HdRetainedSmallVectorDataSource::New(indices.size(), indices.data())).Build();
    auto translations = HdPrimvarSchema::Builder().SetPrimvarValue(
        HdRetainedTypedSampledDataSource<VtVec3fArray>::New(positions))
        .SetInterpolation(HdPrimvarSchema::BuildInterpolationDataSource(HdPrimvarSchemaTokens->instance)).Build();
    return HdRetainedContainerDataSource::New(HdInstancerTopologySchemaTokens->instancerTopology, topology,
        HdPrimvarsSchemaTokens->primvars, HdRetainedContainerDataSource::New(
            HdInstancerTokens->instanceTranslations, translations));
}

std::vector<float> RenderInstanced(std::string const &prefix, std::vector<float> *before)
{
    const auto names = Names(prefix);
    const SdfPath material("/" + prefix + "_Emit"), instancer("/" + prefix + "_Instancer");
    Fixture fixture;
    UseCpu(fixture);
    std::vector<HdRetainedSceneIndex::AddedPrimEntry> prims{
        {SdfPath("/" + prefix + "_Floor"), HdPrimTypeTokens->mesh, Quad(0, 0, 0, 2, false)},
        {material, HdPrimTypeTokens->material, EmissiveMaterial(material)},
        {instancer, HdPrimTypeTokens->instancer, Instances(names, 1)}};
    auto by = HdInstancedBySchema::Builder().SetPaths(
        HdRetainedTypedSampledDataSource<VtArray<SdfPath>>::New({instancer})).Build();
    for (auto const &name : names)
        prims.push_back({name, HdPrimTypeTokens->mesh, HdOverlayContainerDataSource::New(
            HdRetainedContainerDataSource::New(HdInstancedBySchemaTokens->instancedBy, by),
            Quad(0, 0, .2f, .05f, true, material))});
    fixture.scene->AddPrims(prims);
    fixture.Render();
    *before = Read<float>(fixture.color);
    fixture.scene->AddPrims({{instancer, HdPrimTypeTokens->instancer, Instances(names, 2)}});
    fixture.bridge->MarkSceneDirty(SceneDirtyKind::Placement, instancer);
    fixture.Render();
    const auto stats = fixture.bridge->GetStats();
    if (Stat(stats, "lastPlacementTopologyAttached") != kEmitters) {
        for (auto const &[name, value] : stats)
            if (name.find("allback") != std::string::npos || name.find("Placement") != std::string::npos ||
                name.find("Builds") != std::string::npos)
                std::cerr << "  " << name << " = " << TfStringify(value) << "\n";
        Require(false, "The instances were not added as a placement update");
    }
    return Read<float>(fixture.color);
}

bool Same(char const *what, std::vector<std::vector<float>> const &images)
{
    bool ok = !images.empty() && !images.front().empty();
    for (size_t i = 1; i < images.size(); ++i) {
        size_t differing = 0;
        for (size_t p = 0; p < images[i].size() && p < images[0].size(); ++p)
            differing += images[i][p] != images[0][p];
        if (images[i].size() != images[0].size() || differing) {
            std::cerr << "FAIL: " << what << ": name set " << i << " renders differently ("
                      << differing << " of " << images[0].size() << " values)\n";
            ok = false;
        }
    }
    return ok;
}
} // namespace

int RunHashOrderTests()
{
    try {
        // A few name sets: their hash orders differ from each other, while
        // the sorted order of every set is the same.
        const std::vector<std::string> prefixes{"Alpha", "Beta", "Gamma", "Delta"};
        std::vector<std::vector<float>> added, instanced, initial(2 * prefixes.size()),
            rebuilt(prefixes.size());
        for (size_t i = 0; i < prefixes.size(); ++i)
            added.push_back(RenderAdded(prefixes[i], &initial[i], &rebuilt[i]));
        for (size_t i = 0; i < prefixes.size(); ++i)
            instanced.push_back(RenderInstanced(prefixes[i], &initial[prefixes.size() + i]));
        // The scenes before the edits (built in traversal order) agree.
        bool ok = Same("initial build (structural)",
                       {initial.begin(), initial.begin() + prefixes.size()});
        ok &= Same("initial build (instanced)", {initial.begin() + prefixes.size(), initial.end()});
        ok &= Same("structural additions", added);
        ok &= Same("placement update", instanced);
        ok &= Same("bank rebuilt after structural additions", rebuilt);
        if (!ok) return 1;
        std::cout << "Hash order tests passed\n";
        return 0;
    } catch (std::exception const &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
