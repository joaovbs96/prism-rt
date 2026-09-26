"""Cold/warm renderer loading instrumentation for stock testusdview.

Run each cold measurement in a fresh process. This script does not clear the
OS filesystem cache: cold means no process-local compiled-material cache.
"""
import ctypes
import json
import os
import time
from pathlib import Path
from pxr import Sdf
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
