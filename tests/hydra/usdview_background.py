"""Verify usdview's dome visibility and viewport background with PrismRT.

Run using stock testusdview's --testScript option and fixtures/parity.usda.
"""
import os
import subprocess
import time
from pathlib import Path

from pxr import Sdf, Usd, UsdGeom
from pxr.Usdviewq.qt import QtWidgets
from pxr.Usdviewq.common import ClearColors


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    settings = model.viewSettings
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-background"))
    output.mkdir(parents=True, exist_ok=True)
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)
    settings.showBBoxes = False
    settings.showHUD = False
    settings.cameraPath = Sdf.Path("/World/Camera")
    settings.clearColorText = ClearColors.WHITE
    view.SetPhysicalWindowSize(256, 256)
    view.makeCurrent()
    # A hidden Qt window may not receive its initial paint event. Force one
    # before selecting a renderer; SetRendererPlugin otherwise succeeds with
    # no engine, and IsRendererConverged can never become true.
    view.grabFrameBuffer()
    assert view._getRenderer() is not None, "Initial framebuffer did not create a renderer"
    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"

    def settle():
        deadline = time.monotonic() + 180
        view.updateView()
        while time.monotonic() < deadline:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if view.IsRendererConverged():
                return
            time.sleep(0.01)
        raise AssertionError("PrismRT failed to converge in the background probe")

    def capture(name):
        image = view.grabFrameBuffer()
        assert not image.isNull(), "usdview produced no framebuffer"
        assert image.save(str(output / name)), f"Could not save {name}"
        # Sample clear regions away from the camera-framed panel. Averaging
        # corners avoids relying on one edge pixel's rasterization.
        points = [(4, 4), (251, 4), (4, 251), (251, 251)]
        return [image.pixelColor(x, y).getRgb()[:3] for x, y in points]

    settings.domeLightTexturesVisible = True
    settle()
    environment = capture("dome-visible.png")
    check(any(min(pixel) > 5 for pixel in environment),
          f"Visible dome background rendered black: {environment}")
    check(any(max(pixel) < 245 for pixel in environment),
          f"Visible dome background was replaced by white host clear color: {environment}")

    settings.domeLightTexturesVisible = False
    settle()
    cleared = capture("dome-hidden-white-background.png")
    for pixel in cleared:
        check(all(channel >= 250 for channel in pixel),
              f"Hidden dome did not show usdview's white clear color: {cleared}")

    settings.clearColorText = ClearColors.BLACK
    settle()
    black = capture("dome-hidden-black-background.png")
    for pixel in black:
        check(all(channel <= 5 for channel in pixel),
              f"Changing usdview clear color did not update presentation: {black}")

    # FrameRecorder is the actual Hydra image-export path. Its transparent
    # clear must stay in the raw AOV even though usdview composites a viewport
    # background for display.
    view.makeCurrent()
    # testusdview loads this script before constructing QApplication. Load
    # export support after the viewport engine, avoiding Windows GL/plugin
    # initialization failures caused by importing it during script loading.
    from pxr import UsdAppUtils
    recorder = UsdAppUtils.FrameRecorder("HdPrismRendererPlugin", True, True)
    check(str(recorder.GetCurrentRendererId()) == "HdPrismRendererPlugin",
          "FrameRecorder did not select PrismRT")
    recorder.SetImageWidth(256)
    recorder.SetDomeLightVisibility(False)
    camera = UsdGeom.Camera(model.stage.GetPrimAtPath(Sdf.Path("/World/Camera")))
    for suffix in ("png", "exr"):
        image_path = (output / f"hidden-dome-export.{suffix}").resolve()
        recorded = recorder.Record(model.stage, camera, Usd.TimeCode.Default(), str(image_path))
        check(recorded, f"FrameRecorder failed to write {suffix}")
        if not recorded:
            continue
        checker = Path(os.environ.get(
            "PRISM_HYDRA_RENDER_EXE",
            str(Path.cwd() / "build/hydra/plugin/prism_hydra_render.exe")))
        result = subprocess.run(
            [str(checker), "--check-transparent-alpha", str(image_path)],
            check=False, capture_output=True, text=True)
        check(result.returncode == 0,
              f"{suffix} export lost transparent background alpha: {result.stdout}{result.stderr}")

    # Restore the host defaults for any later test hooks sharing this process.
    settings.domeLightTexturesVisible = True
    settings.clearColorText = ClearColors.DARK_GREY
    settle()
    if failures:
        raise AssertionError("Background checks failed:\n" + "\n".join(failures))
    print("PrismRT usdview dome/background presentation passed:", output.resolve())
