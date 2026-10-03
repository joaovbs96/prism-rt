"""Diagnostic scaling probe executed by stock testusdview; no stage is saved."""
import json
import faulthandler
import os
import time
from pathlib import Path

from pxr import Gf, Sdf, UsdGeom, UsdShade
from pxr.Usdviewq.qt import QtWidgets

from usdview_loading import memory


def testUsdviewInputFunction(appController):
    faulthandler.dump_traceback_later(45, repeat=True)
    view = appController._stageView
    model = appController._dataModel
    case = json.loads(os.environ["PRISM_STRUCTURAL_CASE"])
    output = Path(os.environ["PRISM_VALIDATION_DIR"])
    output.mkdir(parents=True, exist_ok=True)
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.selection.clearPrims()
    # Keep the browser state controlled. Restoring thousands of expanded rows
    # on a USD resync can dominate host latency independently of Hydra work.
    collapsed_browser = os.environ.get("PRISM_STRUCTURAL_COLLAPSE_BROWSER", "1") == "1"
    if collapsed_browser:
        tree = appController._ui.primView
        tree.collapseAll()
        root_item = tree.topLevelItem(0)
        if root_item is not None:
            root_item.setExpanded(True)
    model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    view.SetPhysicalWindowSize(64, 64)
    view.makeCurrent()
    initial_host_memory = memory()
    initial_renderer = str(view.GetCurrentRendererId())
    rows = []
    stats_seconds = 0.0
    stats_calls = 0
    host_tree_seconds = 0.0
    original_reset = appController._resetPrimView

    def timed_reset(*args, **kwargs):
        nonlocal host_tree_seconds
        begin = time.perf_counter()
        try:
            return original_reset(*args, **kwargs)
        finally:
            host_tree_seconds += time.perf_counter() - begin

    appController._resetPrimView = timed_reset

    def stats():
        nonlocal stats_seconds, stats_calls
        begin = time.perf_counter()
        result = dict(view._renderer.GetRenderStats())
        stats_seconds += time.perf_counter() - begin
        stats_calls += 1
        return result

    def settle(previous_generation=None):
        deadline = time.perf_counter() + 180
        next_report = time.perf_counter() + 10
        view.updateView()
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            current = stats()
            if time.perf_counter() >= next_report:
                print("STRUCTURAL pending", previous_generation, json.dumps(current, default=str), flush=True)
                next_report = time.perf_counter() + 10
            requested = current.get("preparationGeneration", 0)
            displayed = current.get("displayedGeneration", -1)
            changed = previous_generation is None or requested > previous_generation
            if (changed and not current.get("preparationPending", True)
                    and displayed >= requested and current.get("completedSamples", 0) >= 1
                    and view.IsRendererConverged()):
                frame = view.grabFrameBuffer()
                assert not frame.isNull(), "No presented image"
                return current
            if time.perf_counter() > deadline:
                raise RuntimeError(f"Probe did not settle: old={previous_generation}, stats={current}")
            time.sleep(.01)

    def save(label, start, before, after, stats_start, calls_start, cpu_start):
        delta = {key: value - before.get(key, 0) for key, value in after.items()
                 if isinstance(value, (int, float)) and not isinstance(value, bool)
                 and isinstance(before.get(key, 0), (int, float))}
        row = dict(operation=label, elapsedSeconds=time.perf_counter()-start,
                   processCpuSeconds=time.process_time()-cpu_start,
                   statsQuerySeconds=stats_seconds-stats_start,
                   statsQueryCalls=stats_calls-calls_start,
                   before=before, after=after, numericDifference=delta, **memory())
        previous_host_time = rows[-1]["hostTreeResetCumulativeSeconds"] if rows else 0
        row["hostTreeResetSeconds"] = host_tree_seconds - previous_host_time
        row["hostTreeResetCumulativeSeconds"] = host_tree_seconds
        rows.append(row)
        (output / "probe.json").write_text(json.dumps(
            dict(case=case, operations=rows, initialHostMemory=initial_host_memory,
                 initialRenderer=initial_renderer,
                 primBrowser="root-expanded" if collapsed_browser else "host-default",
                 note="numericDifference includes last-operation gauges; consult before/after. "
                      "Elapsed time includes host pumps, statistics queries and final framebuffer read."),
            indent=2, default=str), encoding="utf-8")
        print("STRUCTURAL", label, row["elapsedSeconds"], flush=True)
        assert after.get("materialFailures", 0) == 0, "Material compilation failed"

    for repeat in range(2):
        if repeat:
            assert view.SetRendererPlugin("HdStormRendererPlugin")
        begin, cpu = time.perf_counter(), time.process_time()
        s0, c0 = stats_seconds, stats_calls
        assert view.SetRendererPlugin("HdPrismRendererPlugin")
        view.SetRendererSetting("prism:samplesPerPixel", 1)
        view.SetRendererSetting("prism:maxDepth", 2)
        view.SetRendererSetting("prism:refinementDelayMs", 0)
        after = settle()
        save("cold" if repeat == 0 else "recreate", begin, {}, after, s0, c0, cpu)
    view.grabFrameBuffer().save(str(output / "scene.png"))

    def edit(label, change):
        print("STRUCTURAL begin", label, flush=True)
        before = stats()
        s0, c0 = stats_seconds, stats_calls
        begin, cpu = time.perf_counter(), time.process_time()
        change()
        print("STRUCTURAL authored", label, flush=True)
        after = settle(before["preparationGeneration"])
        save(label, begin, before, after, s0, c0, cpu)

    prim = model.stage.GetPrimAtPath(case["editPrim"])
    translate = prim.GetAttribute("xformOp:translate")
    if translate:
        origin = translate.Get() or Gf.Vec3d(0)
        for iteration in range(3):
            value = origin + Gf.Vec3d(.001 * (iteration+1), 0, 0)
            edit(f"transform-{iteration}", lambda value=value: translate.Set(value))
    material = model.stage.GetPrimAtPath(case["editMaterial"])
    attribute = material.GetAttribute("inputs:diffuse_roughness")
    if attribute:
        # A supported mesh reliably generates a structural notice. An empty
        # Xform can be filtered out by the stock adapter. These changes must
        # retain the preparation of all unchanged material bindings.
        def add_control_mesh(path):
            mesh = UsdGeom.Mesh.Define(model.stage, path)
            mesh.CreatePointsAttr([(-10, 0, 0), (-10, .01, 0), (-10, 0, .01)])
            mesh.CreateFaceVertexCountsAttr([3])
            mesh.CreateFaceVertexIndicesAttr([0, 1, 2])
            mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.none)
            binding = UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim())
            binding.Bind(UsdShade.Material(material.GetParent()))
        edit("structural-control", lambda: add_control_mesh("/World/ProbeBaseline"))
        edit("material-parameter", lambda: attribute.Set(.125))
        edit("structural-after-material", lambda: add_control_mesh("/World/ProbeAfterMaterial"))
        edit("structural-remove-control", lambda: model.stage.RemovePrim("/World/ProbeBaseline"))
        edit("structural-remove-after-material", lambda: model.stage.RemovePrim("/World/ProbeAfterMaterial"))
    visibility = UsdGeom.Imageable(prim).CreateVisibilityAttr()
    edit("visibility-hide", lambda: visibility.Set(UsdGeom.Tokens.invisible))
    edit("visibility-show", lambda: visibility.Set(UsdGeom.Tokens.inherited))
    mesh = model.stage.GetPrimAtPath(case["editMesh"])
    points = mesh.GetAttribute("points")
    if points:
        value = points.Get()
        value[0] += Gf.Vec3f(.0001, 0, 0)
        edit("one-prototype-points", lambda: points.Set(value))
        row = rows[-1]
        before, after = row["before"], row["after"]
        assert after.get("lastPrototypePathsUpdated", 0) > 0, (
            "Prototype point edit did not report an updated prototype path")
        assert after.get("lastPrototypePlacementsRepointed", 0) > 0, (
            "Prototype point edit did not report repointed placements")
        assert after.get("topLevelSceneBuilds") == before.get("topLevelSceneBuilds"), (
            "Prototype point edit allocated a new top-level scene")
        assert after.get("snapshotBuilds") == before.get("snapshotBuilds"), (
            "Prototype point edit rebuilt a complete snapshot")
        row["prototypeUpdateEvidence"] = {
            "prototypePathsUpdated": after.get("lastPrototypePathsUpdated"),
            "placementsRepointed": after.get("lastPrototypePlacementsRepointed"),
            "topLevelSceneBuildsBefore": before.get("topLevelSceneBuilds"),
            "topLevelSceneBuildsAfter": after.get("topLevelSceneBuilds"),
            "snapshotBuildsBefore": before.get("snapshotBuilds"),
            "snapshotBuildsAfter": after.get("snapshotBuilds"),
            "topLevelSceneCommitsBefore": before.get("topLevelSceneCommits"),
            "topLevelSceneCommitsAfter": after.get("topLevelSceneCommits"),
        }
        write_probe()
    if case.get("editInstancePositions"):
        instancer = model.stage.GetPrimAtPath(case["editInstancePositions"])
        attribute = instancer.GetAttribute("positions")
        prototype_indices = instancer.GetAttribute("protoIndices")
        original_positions = [Gf.Vec3f(position) for position in attribute.Get()]
        original_indices = list(prototype_indices.Get())
        value = [Gf.Vec3f(position) for position in original_positions]
        value[0] += Gf.Vec3f(.001, 0, 0)
        assert value != original_positions, "Point-instancer edit did not change its position array"
        edit("one-point-instance-position", lambda: attribute.Set(value))
        row = rows[-1]
        before, after = row["before"], row["after"]
        transforms_updated = after.get("lastPlacementTransformsUpdated", 0)
        records_visited = after.get("lastPlacementRecordsVisited", 0)
        assert transforms_updated > 0, "Point edit did not update a placement transform"
        assert transforms_updated <= records_visited, (
            "Updated placement transforms exceed visited placement records")
        assert after.get("prototypeBuildCount") == before.get("prototypeBuildCount"), (
            "Point-instancer position edit rebuilt prototype geometry")
        assert after.get("topLevelSceneBuilds") == before.get("topLevelSceneBuilds"), (
            "Point-instancer position edit allocated a new top-level scene")
        assert after.get("topLevelSceneCommits", 0) > before.get("topLevelSceneCommits", 0), (
            "Point-instancer position edit did not commit the changed top-level scene")
        row["stablePlacementEvidence"] = {
            "placementTransformsUpdated": transforms_updated,
            "placementRecordsVisited": records_visited,
            "prototypeBuildsBefore": before.get("prototypeBuildCount"),
            "prototypeBuildsAfter": after.get("prototypeBuildCount"),
            "topLevelSceneBuildsBefore": before.get("topLevelSceneBuilds"),
            "topLevelSceneBuildsAfter": after.get("topLevelSceneBuilds"),
            "topLevelSceneCommitsBefore": before.get("topLevelSceneCommits"),
            "topLevelSceneCommitsAfter": after.get("topLevelSceneCommits"),
        }
        write_probe()

        # Exercise the documented count/topology fallback separately from
        # stable-count instance movement. Batch both required arrays so each
        # probe represents one logical edit.
        count_base_positions = list(attribute.Get())
        count_base_indices = list(prototype_indices.Get())
        baseline_stats = stats()
        baseline_meshes = baseline_stats.get("meshCount")
        baseline_triangles = baseline_stats.get("triangleCount")

        def set_instancer_arrays(label, positions, indices, expected_count):
            def change_arrays():
                with Sdf.ChangeBlock():
                    attribute.Set(positions)
                    prototype_indices.Set(indices)
            edit(label, change_arrays)
            after = rows[-1]["after"]
            assert after.get("instanceCount") == expected_count, (
                f"{label}: expected {expected_count} instances, "
                f"got {after.get('instanceCount')}")
            assert after.get("meshCount") == baseline_meshes, (
                f"{label}: mesh count changed: {after.get('meshCount')}")
            assert after.get("triangleCount") == baseline_triangles, (
                f"{label}: triangle count changed: {after.get('triangleCount')}")
            topology_fallbacks_before = rows[-1]["before"].get(
                "placementTopologyFallbacks", 0)
            topology_fallbacks_after = after.get("placementTopologyFallbacks", 0)
            prototype_fallbacks_before = rows[-1]["before"].get(
                "prototypeUpdateFallbacks", 0)
            prototype_fallbacks_after = after.get("prototypeUpdateFallbacks", 0)
            if (topology_fallbacks_after > topology_fallbacks_before or
                    prototype_fallbacks_after > prototype_fallbacks_before):
                assert after.get("lastRebuildReason"), (
                    f"{label}: fallback counter advanced without a named rebuild reason")
            rows[-1]["topologyFallbackEvidence"] = {
                "lastRebuildReason": after.get("lastRebuildReason"),
                "expectedInstanceCount": expected_count,
                "instanceCount": after.get("instanceCount"),
                "meshCount": after.get("meshCount"),
                "triangleCount": after.get("triangleCount"),
                "placementTopologyFallbacksBefore": rows[-1]["before"].get(
                    "placementTopologyFallbacks"),
                "placementTopologyFallbacksAfter": after.get(
                    "placementTopologyFallbacks"),
                "prototypeUpdateFallbacksBefore": rows[-1]["before"].get(
                    "prototypeUpdateFallbacks"),
                "prototypeUpdateFallbacksAfter": after.get(
                    "prototypeUpdateFallbacks"),
            }
            write_probe()

        added_positions = count_base_positions + [
            count_base_positions[-1] + Gf.Vec3f(.002, 0, 0)]
        added_indices = count_base_indices + [count_base_indices[-1]]
        set_instancer_arrays("instancer-count-add", added_positions,
                             added_indices, len(count_base_positions) + 1)
        set_instancer_arrays("instancer-count-remove", count_base_positions,
                             count_base_indices, len(count_base_positions))
        assert list(attribute.Get()) != original_positions, (
            "Position restore would be a no-op; the authored point edit was not retained")
        edit("restore-one-point-instance-position",
             lambda: attribute.Set(original_positions))
        restored = stats()
        assert restored.get("instanceCount") == len(original_positions), (
            "Restoring instancer positions changed the instance count")
        assert list(attribute.Get()) == original_positions
        assert list(prototype_indices.Get()) == original_indices
    faulthandler.cancel_dump_traceback_later()
    appController._resetPrimView = original_reset
