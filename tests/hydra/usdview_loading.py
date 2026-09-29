"""Cold/warm renderer loading instrumentation for stock testusdview.

Run each cold measurement in a fresh process. This script does not clear the
OS filesystem cache: cold means no process-local compiled-material cache.
"""
import ctypes
import json
import os
import time
from pathlib import Path
from pxr import Sdf, Usd, UsdGeom
from pxr.Usdviewq.qt import QtWidgets


def memory():
    if os.name != "nt":
        return {}
    class Counters(ctypes.Structure):
        _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
            (name, ctypes.c_size_t) for name in
            ("peakWorkingSet", "workingSet", "peakPagedPool", "pagedPool",
             "peakNonpagedPool", "nonpagedPool", "pagefile", "peakPagefile")]
    counters = Counters()
    counters.cb = ctypes.sizeof(counters)
    process = ctypes.windll.kernel32.GetCurrentProcess
    process.restype = ctypes.c_void_p
    get_info = ctypes.windll.psapi.GetProcessMemoryInfo
    get_info.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong]
    if not get_info(process(), ctypes.byref(counters), counters.cb):
        raise ctypes.WinError()
    return {"workingSetBytes": counters.workingSet,
            "peakWorkingSetBytes": counters.peakWorkingSet}


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-loading"))
    output.mkdir(parents=True, exist_ok=True)
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    if model.stage.GetPrimAtPath("/World/Camera"):
        model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    else:
        model.viewSettings.domeLightEnabled = True
        view.updateView(resetCam=True, forceComputeBBox=True)
    view.SetPhysicalWindowSize(512, 512)
    view.makeCurrent()
    # If the host already selected Prism, its first synchronization must be
    # reported as warm/unknown rather than claimed as a cold compilation.
    initially_prism = str(view.GetCurrentRendererId()) == "HdPrismRendererPlugin"
    results = []
    # One cold creation and one warm recreation per fresh process. The driver
    # repeats each worker configuration three times, giving three of each.
    for repetition in range(2):
        if repetition:
            assert view.SetRendererPlugin("HdStormRendererPlugin")
        start = time.perf_counter()
        assert view.SetRendererPlugin("HdPrismRendererPlugin")
        view.SetRendererSetting("prism:samplesPerPixel", 1)
        view.SetRendererSetting("prism:previewMaxDimension", 96)
        view.SetRendererSetting("prism:refinementDelayMs", 0)
        view.updateView()
        view.updateGL()
        # Qt6 updateGL schedules work. Force a presented frame before measuring
        # first-image latency, rather than timing only the repaint request.
        first_image = view.grabFrameBuffer()
        assert not first_image.isNull(), "Renderer did not present a first image"
        # A valid framebuffer can still contain the previous renderer's image.
        # Wait for Prism to publish a shaded preview or final accumulation pass.
        while True:
            stats = view._renderer.GetRenderStats()
            if stats.get("completedSamples", 0) > 0 or stats.get("previewPublishes", 0) > 0:
                first_image = view.grabFrameBuffer()
                break
            QtWidgets.QApplication.processEvents()
            view.updateGL()
            if time.perf_counter() - start > 600:
                raise RuntimeError("Loading benchmark timed out before first shaded image")
            time.sleep(.002)
        first_frame = time.perf_counter() - start
        while not view.IsRendererConverged():
            QtWidgets.QApplication.processEvents()
            view.updateGL()
            if time.perf_counter() - start > 600:
                raise RuntimeError("Loading benchmark timed out")
            time.sleep(.002)
        result = dict(repetition=repetition,
                      cache="fresh-renderer" if repetition == 0 and not initially_prism else "process-warm",
                      firstFrameSeconds=first_frame,
                      convergedSeconds=time.perf_counter()-start,
                      stats={str(k): v for k,v in view._renderer.GetRenderStats().items()},
                      **memory())
        if repetition == 0 and "PRISM_BENCH_START" in os.environ:
            result["processStartToConvergedSeconds"] = time.perf_counter()-float(os.environ["PRISM_BENCH_START"])
        assert result["stats"].get("materialFailures", 0) == 0, \
            "Loading benchmark encountered failed material compilation"
        results.append(result)
        view.grabFrameBuffer().save(str(output/f"load-{repetition}.png"))
        (output/"loading.json").write_text(json.dumps(results, indent=2, default=str), encoding="utf-8")
        print("Prism loading:", json.dumps(result, default=str), flush=True)

    if os.environ.get("PRISM_BENCH_NAVIGATION") == "1":
        original_view = view.copyViewState()
        view.switchToFreeCamera(computeAndSetClosestDistance=False)
        view.SetRendererSetting("prism:samplesPerPixel", 1000000)
        view.SetRendererSetting("prism:refinementDelayMs", 100)
        latencies = []
        navigation_stats = {key: [] for key in (
            "previewRayWorkMilliseconds", "previewPublicationMilliseconds",
            "stopRenderMilliseconds", "previewBoundAovCount",
            "previewPublishedBytes", "previewTracedPixels",
            "selectionOffsetMilliseconds", "selectionOffsetCalls",
            "selectionPathsVisited", "pickMilliseconds", "pickCalls")}
        host_phases = []
        for event in range(60):
            before_stats = view._renderer.GetRenderStats()
            previous = before_stats["requestedCameraRevision"]
            start = time.perf_counter()
            model.viewSettings.freeCamera.Tumble(0.25, 0.1 if event % 2 else -0.1)
            view.updateView()
            camera_updated = time.perf_counter()
            pumps = 0
            while view._renderer.GetRenderStats()["displayedCameraRevision"] <= previous:
                view.updateGL()
                QtWidgets.QApplication.processEvents()
                pumps += 1
                if time.perf_counter() - start > 10:
                    raise RuntimeError("Navigation did not present the requested camera")
                time.sleep(.001)
            preview_ready = time.perf_counter()
            view.updateGL()
            view.grabFrameBuffer()  # Include upload/presentation in latency.
            presented = time.perf_counter()
            latencies.append(presented - start)
            event_stats = view._renderer.GetRenderStats()
            host_phases.append(dict(
                cameraUpdateMs=1000 * (camera_updated-start),
                previewWaitMs=1000 * (preview_ready-camera_updated),
                finalFramebufferMs=1000 * (presented-preview_ready),
                eventPumps=pumps,
                selectionOffsetMs=event_stats.get("selectionOffsetMilliseconds", 0)
                    - before_stats.get("selectionOffsetMilliseconds", 0),
                pickMs=event_stats.get("pickMilliseconds", 0)
                    - before_stats.get("pickMilliseconds", 0)))
            for key in navigation_stats:
                navigation_stats[key].append(event_stats.get(key, 0))
        ordered = sorted(latencies)
        navigation = dict(samples=latencies,
            p95Seconds=ordered[int(.95*(len(ordered)-1))],
            p99Seconds=ordered[int(.99*(len(ordered)-1))],
            updatesPerSecond=len(latencies)/sum(latencies),
            targetP95Seconds=.05, metP95Target=ordered[int(.95*(len(ordered)-1))] < .05,
            width=512, height=512,
            previewWidth=view._renderer.GetRenderStats().get("previewWidth", 0),
            previewHeight=view._renderer.GetRenderStats().get("previewHeight", 0),
            previewMilliseconds=view._renderer.GetRenderStats().get("previewMilliseconds", 0),
            stopRenderMilliseconds=view._renderer.GetRenderStats().get("stopRenderMilliseconds", 0),
            instrumentation=navigation_stats, hostPhases=host_phases, **memory())

        # The auto-framed OpenChessSet occupies only a small part of the
        # viewport. Record a second pass closer to the subject so object-heavy
        # navigation is measured separately from mostly-environment rays.
        close_camera = model.viewSettings.freeCamera
        initial_close_distance = close_camera.dist
        focus_prim = os.environ.get("PRISM_BENCH_FOCUS_PRIM", "")
        if focus_prim:
            prim = model.stage.GetPrimAtPath(focus_prim)
            assert prim, "Close-view focus prim does not exist: " + focus_prim
            bounds = UsdGeom.BBoxCache(Usd.TimeCode.Default(),
                [UsdGeom.Tokens.default_, UsdGeom.Tokens.render,
                 UsdGeom.Tokens.proxy]).ComputeWorldBound(prim).ComputeAlignedRange()
            assert not bounds.IsEmpty(), "Close-view focus prim has empty bounds"
            extent = max(bounds.GetSize())
            assert extent > 0, "Close-view focus prim has degenerate bounds"
            close_camera.center = bounds.GetMidpoint()
            close_camera.dist = extent * 2.5
            # Stock usdview's free camera defaults to a one-world-unit near
            # plane when auto clipping is off. The chess pieces are ~0.1 m;
            # retaining that default clips the entire targeted close view.
            close_camera.overrideNear = max(extent * 0.001, 1e-7)
            close_camera.fov = 45
            close_camera.rotTheta = 45
            close_camera.rotPhi = float(os.environ.get("PRISM_BENCH_FOCUS_ELEVATION", "25"))
        else:
            close_camera.AdjustDistance(0.25)
            close_camera.Tumble(1.5, 2.5)
        previous = view._renderer.GetRenderStats()["requestedCameraRevision"]
        view.updateView()
        deadline = time.perf_counter() + 10
        while view._renderer.GetRenderStats()["displayedCameraRevision"] <= previous:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if time.perf_counter() > deadline:
                raise RuntimeError("Close-view camera did not publish a preview")
            time.sleep(.001)
        view.updateGL()
        view.grabFrameBuffer().save(str(output/"navigation-close-start.png"))
        close_stats = view._renderer.GetRenderStats()
        initial_preview_width = close_stats.get("previewWidth", 0)
        initial_preview_height = close_stats.get("previewHeight", 0)
        close_latencies = []
        close_navigation_stats = {key: [] for key in navigation_stats}
        for event in range(60):
            previous = view._renderer.GetRenderStats()["requestedCameraRevision"]
            start = time.perf_counter()
            close_camera.Tumble(0.2, 0.08 if event % 2 else -0.08)
            view.updateView()
            deadline = start + 10
            while view._renderer.GetRenderStats()["displayedCameraRevision"] <= previous:
                view.updateGL()
                QtWidgets.QApplication.processEvents()
                if time.perf_counter() > deadline:
                    raise RuntimeError("Close-view navigation did not present the camera")
                time.sleep(.001)
            view.updateGL()
            view.grabFrameBuffer()
            close_latencies.append(time.perf_counter() - start)
            event_stats = view._renderer.GetRenderStats()
            for key in close_navigation_stats:
                close_navigation_stats[key].append(event_stats.get(key, 0))
        close_ordered = sorted(close_latencies)
        close_stats = view._renderer.GetRenderStats()
        navigation["closeView"] = dict(
            samples=close_latencies,
            p95Seconds=close_ordered[int(.95*(len(close_ordered)-1))],
            p99Seconds=close_ordered[int(.99*(len(close_ordered)-1))],
            updatesPerSecond=len(close_latencies)/sum(close_latencies),
            targetP95Seconds=.05,
            metP95Target=close_ordered[int(.95*(len(close_ordered)-1))] < .05,
            previewWidth=close_stats.get("previewWidth", 0),
            previewHeight=close_stats.get("previewHeight", 0),
            initialPreviewWidth=initial_preview_width,
            initialPreviewHeight=initial_preview_height,
            initialDistance=initial_close_distance,
            focusPrim=focus_prim,
            focusElevation=close_camera.rotPhi,
            nearClip=close_camera.near,
            farClip=close_camera.far,
            closeDistance=close_camera.dist,
            viewportWidth=512, viewportHeight=512,
            previewMilliseconds=close_stats.get("previewMilliseconds", 0),
            stopRenderMilliseconds=close_stats.get("stopRenderMilliseconds", 0),
            instrumentation=close_navigation_stats, **memory())
        view.grabFrameBuffer().save(str(output/"navigation-close.png"))
        (output/"navigation.json").write_text(json.dumps(navigation, indent=2), encoding="utf-8")
        view.grabFrameBuffer().save(str(output/"navigation.png"))
        view.SetRendererSetting("prism:samplesPerPixel", 1)
        view.restoreViewState(original_view)
        print("Prism navigation:", json.dumps(navigation), flush=True)
