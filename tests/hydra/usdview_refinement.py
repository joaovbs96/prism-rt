"""Record settled-camera screen refinement cadence in stock testusdview."""
import json
import os
import time
import zlib
from pathlib import Path

import numpy as np
from pxr import Sdf
from pxr.Usdviewq.qt import QtGui, QtWidgets

# Windows monotonic() can be quantized at the system tick; use the high
# resolution performance counter for paint and first-sample comparisons.
clock = time.perf_counter


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-refinement"))
    output.mkdir(parents=True, exist_ok=True)
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.selection.clearPrims()
    view.DrawAxis = lambda *_: None
    fixture = bool(model.stage.GetPrimAtPath("/World/Panel"))
    if fixture:
        model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    else:
        model.viewSettings.domeLightEnabled = True
        view.updateView(resetCam=True, forceComputeBBox=True)
    width = int(os.environ.get("PRISM_REFINEMENT_WIDTH", "1280"))
    height = int(os.environ.get("PRISM_REFINEMENT_HEIGHT", "720"))
    view.SetPhysicalWindowSize(width, height)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    view.SetRendererSetting("prism:samplesPerPixel", 1)
    start = clock()
    while not view.IsRendererConverged():
        view.updateGL()
        QtWidgets.QApplication.processEvents()
        assert clock() - start < 180, "Initial render did not converge"
        time.sleep(.005)
    initial_refinement = int(dict(view._renderer.GetRenderStats()).get("refinementPublishes", 0))
    view.SetRendererSetting("prism:samplesPerPixel", 10000)
    view.switchToFreeCamera()
    model.viewSettings.freeCamera.Tumble(3, 0)
    view.updateView()
    rows = []
    frames = []
    screenshots = []
    start = clock()
    next_capture = 0
    paint_rows = []
    original_paint = view.paintGL
    def observe_paint(*args, **kwargs):
        paint_start = clock()
        result = original_paint(*args, **kwargs)
        paint_end = clock()
        stats = dict(view._renderer.GetRenderStats())
        paint_rows.append({"time": paint_end - start,
                           "paintMs": (paint_end-paint_start)*1000,
                           "samples": stats.get("completedSamples", 0),
                           "refinementPublishes": stats.get("refinementPublishes", 0)})
        return result
    view.paintGL = observe_paint
    previous_samples = -1
    previous_refinement = -1
    display_rows = []
    while clock() - start < 12:
        draw_start = clock()
        view.updateGL()
        QtWidgets.QApplication.processEvents()
        draw_ms = (clock() - draw_start) * 1000
        stats = dict(view._renderer.GetRenderStats())
        now = clock() - start
        samples = stats.get("completedSamples", 0)
        refinement = stats.get("refinementPublishes", 0)
        if refinement != previous_refinement or samples != previous_samples:
            display_rows.append({"time": now, "samples": samples, "refinementPublishes": refinement})
            previous_refinement = refinement
        if samples != previous_samples:
            rows.append({"time": now, "drawMs": draw_ms, "stats": stats})
            previous_samples = samples
        if now >= next_capture:
            image = view.grabFrameBuffer().convertToFormat(QtGui.QImage.Format_RGBA8888)
            raw = np.frombuffer(image.constBits(), dtype=np.uint8).reshape(image.height(), image.bytesPerLine())
            pixels = raw[:, :image.width()*4].copy()
            frames.append({"time": clock()-start, "samples": samples,
                           "crc": zlib.crc32(pixels), "width": image.width(), "height": image.height()})
            if len(frames) in (1, 8, 24):
                screenshots.append((output / f"screen-{len(frames)}.png", image.copy()))
            next_capture = now + .15
        time.sleep(.005)
    view.paintGL = original_paint
    # PNG compression is deliberately outside the measured event loop.
    for path, image in screenshots:
        image.save(str(path))
    cancel_start = clock()
    view.SetRendererSetting("prism:seed", 1)
    cancellation_ms = (clock() - cancel_start) * 1000
    view.SetRendererSetting("prism:samplesPerPixel", max(1, int(previous_samples)))
    result = {"stage": model.stage.GetRootLayer().identifier, "sampleChanges": rows, "screenFrames": frames, "displayChanges": display_rows, "initialRefinementPublishes": initial_refinement, "paints": paint_rows, "settingCancellationMs": cancellation_ms}
    (output / "cadence.json").write_text(json.dumps(result, indent=2, default=str))
    print("Refinement cadence:", [(round(row["time"],3), row["stats"]["completedSamples"]) for row in rows])
