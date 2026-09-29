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
    if case.get("editInstancePositions"):
        attribute = model.stage.GetPrimAtPath(case["editInstancePositions"]).GetAttribute("positions")
        value = attribute.Get()
        value[0] += Gf.Vec3f(.001, 0, 0)
        edit("one-point-instance-position", lambda: attribute.Set(value))
    faulthandler.cancel_dump_traceback_later()
    appController._resetPrimView = original_reset
