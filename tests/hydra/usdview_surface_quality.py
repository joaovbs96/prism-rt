"""Capture analytic spheres and smooth mesh normals in stock testusdview.

Use an authored camera via PRISM_VALIDATION_CAMERA (default /World/Camera).
This script changes only the in-memory session; it never saves the stage.
"""
import json
import os
import time
from pathlib import Path
from pxr import Sdf
from pxr.UsdAppUtils.complexityArgs import RefinementComplexities
from pxr.Usdviewq.qt import QtWidgets


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/surface-quality"))
    output.mkdir(parents=True, exist_ok=True)
    camera = os.environ.get("PRISM_VALIDATION_CAMERA", "/World/Camera")
    samples = int(os.environ.get("PRISM_VALIDATION_SAMPLES", "32"))
    assert model.stage.GetPrimAtPath(camera), camera
    model.viewSettings.cameraPath = Sdf.Path(camera)
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.selection.clearPrims()
    view.DrawAxis = lambda *_: None
    view.SetPhysicalWindowSize(768, 512)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    view.SetRendererSetting("prism:samplesPerPixel", samples)
    view.SetRendererSetting("prism:refinementDelayMs", 0)

    def settle():
        start = time.monotonic()
        view.updateView()
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if view.IsRendererConverged():
                return
            assert time.monotonic() - start < 600, "Viewport did not converge"
            time.sleep(0.002)

    model.viewSettings.complexity = RefinementComplexities.LOW
    settle()
    low = view.grabFrameBuffer()
    assert low.save(str(output / "beauty-low.png"))
    model.viewSettings.complexity = RefinementComplexities.VERY_HIGH
    settle()
    high = view.grabFrameBuffer()
    assert high.save(str(output / "beauty-high.png"))
    stats = view._renderer.GetRenderStats()
    ground = model.stage.GetPrimAtPath("/World/Geometry/Object_0")
    if ground and ground.GetTypeName() == "Sphere":
        assert stats["triangleCount"] == 0, "USD spheres were tessellated"
        assert low == high, "Complexity changed the analytic sphere image"
    # Geometric AOVs are deterministic primary intersections; one pass suffices.
    view.SetRendererSetting("prism:samplesPerPixel", 1)
    assert view.SetRendererAov("normal")
    settle()
    assert view.grabFrameBuffer().save(str(output / "normals.png"))
    assert view.SetRendererAov("color")
    # Renderer recreation must retain the scene's surface representation.
    assert view.SetRendererPlugin("HdStormRendererPlugin")
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    view.SetRendererSetting("prism:samplesPerPixel", samples)
    view.SetRendererSetting("prism:refinementDelayMs", 0)
    settle()
    assert view.grabFrameBuffer() == high, "Renderer recreation changed the image"
    (output / "stats.json").write_text(json.dumps({
        "initial": dict(stats), "recreated": dict(view._renderer.GetRenderStats())
    }, indent=2, default=str))
    print("Surface quality viewport checks passed:", output)
