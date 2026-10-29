// prismrt._prism_usd: USD stages into a prismrt scene (prism_usd.dll;
// docs/python.md, docs/sdk_design.md section 6.5). Imported by prismrt.usd
// on first use. Stages come as files, or as stages of the same USD build in
// this process through UsdUtilsStageCache (load_cached).

#include "common.h"

#include <nanobind/stl/map.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <prism/usd.h>

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdUtils/stageCache.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include <map>

namespace nb = nanobind;
using namespace nb::literals;

namespace prism::python {
namespace {

struct UsdLoadResult {
    usd::LoadResult counts;
    // The stage's cameras, added to the scene, by prim path.
    std::map<std::string, CameraId> cameras;
};

pxr::UsdTimeCode TimeOf(std::optional<double> time)
{
    return time ? pxr::UsdTimeCode(*time) : pxr::UsdTimeCode::Default();
}

usd::LoadOptions Options(std::optional<double> time, std::vector<std::string> const& bindingPurposes,
                         std::vector<std::string> const& includedPurposes, bool domes)
{
    usd::LoadOptions options;
    for (auto const& purpose : bindingPurposes) options.bindingPurposes.emplace_back(purpose);
    for (auto const& purpose : includedPurposes) options.includedPurposes.emplace_back(purpose);
    options.time = TimeOf(time);
    options.domes = domes;
    return options;
}

UsdLoadResult LoadStage(PyScene& scene, pxr::UsdStageRefPtr const& stage, usd::LoadOptions const& options,
                        bool cameras)
{
    UsdLoadResult result;
    result.counts = usd::Load(scene.get(), stage, options);
    if (cameras)
        for (pxr::UsdPrim const& prim : stage->Traverse())
            if (prim.IsA<pxr::UsdGeomCamera>())
                result.cameras[prim.GetPath().GetString()] =
                    scene.get().addCamera(usd::LoadCamera(stage, prim.GetPath(), options.time));
    return result;
}

pxr::UsdStageRefPtr OpenStage(std::string const& path)
{
    pxr::UsdStageRefPtr stage = pxr::UsdStage::Open(path);
    if (!stage) throw prism::Error(ErrorCode::NotFound, "cannot open USD stage: " + path);
    return stage;
}

pxr::UsdStageRefPtr CachedStage(long id)
{
    pxr::UsdStageRefPtr stage = pxr::UsdUtilsStageCache::Get().Find(pxr::UsdStageCache::Id::FromLongInt(id));
    if (!stage)
        throw prism::Error(ErrorCode::NotFound, "no stage with id " + std::to_string(id) + " in UsdUtils.StageCache");
    return stage;
}

// A second stage over the layers of a stage the caller holds open (found in
// the layer registry with their unsaved edits): a pxr stage that is not in
// UsdUtilsStageCache. Inserting it there instead would hand its ownership to
// the cache (USD's Python bindings), and erasing it again would close it.
pxr::UsdStageRefPtr StageOverLayers(std::string const& root, std::string const& session)
{
    pxr::SdfLayerRefPtr rootLayer = pxr::SdfLayer::Find(root);
    if (!rootLayer) throw prism::Error(ErrorCode::NotFound, "no open layer " + root);
    pxr::SdfLayerRefPtr sessionLayer;
    if (!session.empty()) sessionLayer = pxr::SdfLayer::Find(session);
    pxr::UsdStageRefPtr stage = sessionLayer ? pxr::UsdStage::Open(rootLayer, sessionLayer)
                                             : pxr::UsdStage::Open(rootLayer);
    if (!stage) throw prism::Error(ErrorCode::NotFound, "cannot open a stage over layer " + root);
    return stage;
}

// The files of the loaded modules named `name` (Windows; elsewhere empty).
std::vector<std::string> LoadedModules(std::string const& name)
{
    std::vector<std::string> paths;
#if defined(_WIN32)
    HMODULE modules[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return paths;
    const size_t count = std::min<size_t>(needed / sizeof(HMODULE), 2048);
    for (size_t i = 0; i < count; ++i) {
        char file[MAX_PATH * 2];
        const DWORD length = GetModuleFileNameA(modules[i], file, DWORD(sizeof(file)));
        if (!length) continue;
        std::string path(file, length);
        const size_t slash = path.find_last_of("\\/");
        std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        for (char& c : base) c = char(std::tolower((unsigned char)c));
        if (base == name) paths.push_back(path);
    }
#else
    (void)name;
#endif
    return paths;
}

} // namespace
} // namespace prism::python

NB_MODULE(_prism_usd, m)
{
    using namespace prism::python;
    nb::set_leak_warnings(false);
    // The scene and camera types are prismrt._prism's.
    nb::module_::import_("prismrt._prism");
    m.doc() = "USD stages into a prismrt scene (prism_usd).";

    nb::class_<UsdLoadResult>(m, "LoadResult")
        .def_prop_ro("mesh_placements", [](UsdLoadResult const& r) { return r.counts.meshPlacements; })
        .def_prop_ro("spheres", [](UsdLoadResult const& r) { return r.counts.spheres; })
        .def_prop_ro("lights", [](UsdLoadResult const& r) { return r.counts.lights; })
        .def_prop_ro("domes", [](UsdLoadResult const& r) { return r.counts.domes; })
        .def_prop_ro("materials", [](UsdLoadResult const& r) { return r.counts.materials; })
        .def_prop_ro("renderable_placements", [](UsdLoadResult const& r) { return r.counts.renderablePlacements(); })
        .def_ro("cameras", &UsdLoadResult::cameras, "The stage's cameras (added to the scene) by prim path.")
        .def("__repr__", [](UsdLoadResult const& r) {
            return "LoadResult(placements=" + std::to_string(r.counts.renderablePlacements()) +
                   ", lights=" + std::to_string(r.counts.lights) + ", domes=" + std::to_string(r.counts.domes) +
                   ", materials=" + std::to_string(r.counts.materials) +
                   ", cameras=" + std::to_string(r.cameras.size()) + ")";
        });

    m.def(
        "load",
        [](PyScene& scene, std::string const& path, std::optional<double> time,
           std::vector<std::string> const& bindingPurposes, std::vector<std::string> const& includedPurposes,
           bool domes, bool cameras) {
            const prism::usd::LoadOptions options = Options(time, bindingPurposes, includedPurposes, domes);
            nb::gil_scoped_release release;
            return LoadStage(scene, OpenStage(path), options, cameras);
        },
        "scene"_a, "path"_a, "time"_a = nb::none(), "binding_purposes"_a = std::vector<std::string>(),
        "included_purposes"_a = std::vector<std::string>(), "domes"_a = true, "cameras"_a = true,
        "Opens a USD file and adds its gprims, materials, lights, domes and cameras to the scene.");
    m.def(
        "load_cached",
        [](PyScene& scene, long stageId, std::optional<double> time, std::vector<std::string> const& bindingPurposes,
           std::vector<std::string> const& includedPurposes, bool domes, bool cameras) {
            const prism::usd::LoadOptions options = Options(time, bindingPurposes, includedPurposes, domes);
            nb::gil_scoped_release release;
            return LoadStage(scene, CachedStage(stageId), options, cameras);
        },
        "scene"_a, "stage_id"_a, "time"_a = nb::none(), "binding_purposes"_a = std::vector<std::string>(),
        "included_purposes"_a = std::vector<std::string>(), "domes"_a = true, "cameras"_a = true,
        "As load(), for a stage of UsdUtils.StageCache (UsdStageCache.Id.ToLongInt()).");
    m.def(
        "load_layers",
        [](PyScene& scene, std::string const& root, std::string const& session, std::optional<double> time,
           std::vector<std::string> const& bindingPurposes, std::vector<std::string> const& includedPurposes,
           bool domes, bool cameras) {
            const prism::usd::LoadOptions options = Options(time, bindingPurposes, includedPurposes, domes);
            nb::gil_scoped_release release;
            return LoadStage(scene, StageOverLayers(root, session), options, cameras);
        },
        "scene"_a, "root_layer"_a, "session_layer"_a, "time"_a = nb::none(),
        "binding_purposes"_a = std::vector<std::string>(), "included_purposes"_a = std::vector<std::string>(),
        "domes"_a = true, "cameras"_a = true,
        "As load(), for a stage over open layers (identifiers; their unsaved edits included).");
    m.def(
        "load_camera_layers",
        [](std::string const& root, std::string const& session, std::string const& camera, std::optional<double> time) {
            nb::gil_scoped_release release;
            return prism::usd::LoadCamera(StageOverLayers(root, session), pxr::SdfPath(camera), TimeOf(time));
        },
        "root_layer"_a, "session_layer"_a, "camera"_a, "time"_a = nb::none());
    m.def(
        "load_camera",
        [](std::string const& path, std::string const& camera, std::optional<double> time) {
            nb::gil_scoped_release release;
            return prism::usd::LoadCamera(OpenStage(path), pxr::SdfPath(camera), TimeOf(time));
        },
        "path"_a, "camera"_a, "time"_a = nb::none(), "A USD file's camera, with its exposure.");
    m.def(
        "load_camera_cached",
        [](long stageId, std::string const& camera, std::optional<double> time) {
            nb::gil_scoped_release release;
            return prism::usd::LoadCamera(CachedStage(stageId), pxr::SdfPath(camera), TimeOf(time));
        },
        "stage_id"_a, "camera"_a, "time"_a = nb::none());
    m.def(
        "default_dome_texture",
        [] {
            const pxr::PlugPluginPtr hdx = pxr::PlugRegistry::GetInstance().GetPluginWithName("hdx");
            return hdx ? hdx->FindPluginResource("textures/StinsonBeach.hdr", true) : std::string();
        },
        "usdview's default dome texture (hdx's StinsonBeach.hdr), as the CLI uses it for "
        "SceneOptions.default_dome_texture; empty when hdx is not found.");
    m.def("pxr_version", [] { return nb::make_tuple(PXR_MAJOR_VERSION, PXR_MINOR_VERSION, PXR_PATCH_VERSION); },
          "The USD version prism_usd was built with, as pxr.Usd.GetVersion() reports it.");
    m.def("loaded_modules", &LoadedModules, "name"_a,
          "The files of the loaded modules with this (lower-case) file name (Windows).");
}
