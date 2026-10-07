"""Exercise cached/tiled GPU presentation with stock testusdview and parity.usda."""
import json
import os
import time
from pathlib import Path

from pxr import Gf, Sdf
from pxr.Usdviewq.qt import QtWidgets
from pxr.Usdviewq.common import ClearColors, SelectionHighlightModes


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    settings = model.viewSettings
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-presentation"))
    output.mkdir(parents=True, exist_ok=True)
    settings.showBBoxes = False
    settings.showHUD = False
    settings.selHighlightMode = SelectionHighlightModes.ALWAYS
    settings.cameraPath = Sdf.Path("/World/Camera")
    settings.domeLightTexturesVisible = False
    settings.clearColorText = ClearColors.BLACK
    model.selection.clearPrims()
    view.DrawAxis = lambda *_: None
    view.SetPhysicalWindowSize(256, 256)
    view.makeCurrent()
    view.grabFrameBuffer()
    assert view._getRenderer() is not None, "Initial framebuffer did not create a renderer"
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    view.SetRendererSetting("prism:samplesPerPixel", 8)
    view.SetRendererSetting("prism:refinementDelayMs", 0)

    def stats():
        return dict(view._renderer.GetRenderStats())

    def paint():
        view.updateGL()
        QtWidgets.QApplication.processEvents()

    def settle():
        view.updateView()
        deadline = time.monotonic() + 180
        while True:
            paint()
            if view.IsRendererConverged():
                paint()  # Upload the final immutable publication.
                return stats()
            assert time.monotonic() < deadline, "Presentation render did not converge"
            time.sleep(.005)

    def capture(name):
        image = view.grabFrameBuffer()
        assert not image.isNull()
        assert image.save(str(output / name))
        return image

    settle()
    black = capture("01-black.png")
    baseline = stats()
    assert baseline.get("presentationUploadCalls", 0) > 0, "GPU cache path was not used"
    for _ in range(12):
        paint()
    assert capture("02-unchanged.png") == black, "Unchanged redraw changed color"
    unchanged = stats()
    assert unchanged["presentationUploadBytes"] == baseline["presentationUploadBytes"], \
        "Unchanged redraw uploaded CPU pixels"
    assert unchanged["presentationSkippedUploads"] > baseline["presentationSkippedUploads"]

    model.selection.setPrimPath(Sdf.Path("/World/Panel"))
    settle()
    selected = capture("03-selected.png")
    assert selected != black, "Selection overlay was not drawn"
    for _ in range(5):
        paint()
    assert capture("04-selection-redraw.png") == selected, "Selection accumulated on cached input"
    model.selection.clearPrims()
    settle()
    assert capture("05-deselected.png") == black, "Deselection retained pixels in cached input"
    assert stats()["presentationUploadBytes"] == baseline["presentationUploadBytes"], \
        "Selection changes uploaded unchanged CPU color/depth"

    before_background = stats()
    settings.clearColorText = ClearColors.WHITE
    settle()
    white = capture("06-white.png")
    assert all(min(white.pixelColor(x, y).getRgb()[:3]) >= 250
               for x, y in [(4, 4), (251, 4), (4, 251), (251, 251)]), \
        "GPU background composite left non-white clear pixels"
    assert white != black
    assert stats()["completedSamples"] == before_background["completedSamples"]
    assert stats()["presentationUploadBytes"] == before_background["presentationUploadBytes"], \
        "Background-only change uploaded unchanged raw color"
    settings.clearColorText = ClearColors.BLACK
    settle()
    assert capture("07-black-restored.png") == black

    # Exercise stock conversion fallback, then return to cached Float32 RGBA.
    assert view.SetRendererAov("normal")
    settle()
    capture("08-normal.png")
    assert view.SetRendererAov("color")
    settle()
    assert capture("09-color-restored.png") == black, "AOV switching reused a stale GPU source"

    # Odd dimensions exercise clipped tile payloads and atlas padding. Stop
    # after observing a real GPU atlas draw, then replace the camera and size.
    before_tiles = stats()
    view.SetPhysicalWindowSize(2049, 2049)
    view.SetRendererSetting("prism:samplesPerPixel", 128)
    view.updateView()
    deadline = time.monotonic() + 60
    while stats().get("presentationAtlasDraws", 0) <= before_tiles.get("presentationAtlasDraws", 0):
        paint()
        assert time.monotonic() < deadline, "No immutable tile atlas reached GPU presentation"
        time.sleep(.005)
    partial = capture("10-partial-tiles.png")
    tile_stats = stats()
    assert tile_stats["presentationAtlasBytes"] > before_tiles["presentationAtlasBytes"]
    assert not partial.isNull()
    view.SetPhysicalWindowSize(257, 193)
    camera_translation = model.stage.GetPrimAtPath("/World/Camera").GetAttribute("xformOp:translate")
    camera_translation.Set(Gf.Vec3d(.35, 0, 4))
    view.SetRendererSetting("prism:samplesPerPixel", 8)
    resized = settle()
    capture("11-new-camera-resized.png")
    assert resized["presentationUploadBytes"] > tile_stats["presentationUploadBytes"]
    assert resized["displayedCameraRevision"] == resized["requestedCameraRevision"]
    before_redraw = stats()
    for _ in range(5):
        paint()
    assert stats()["presentationUploadBytes"] == before_redraw["presentationUploadBytes"]
    report = dict(baseline=baseline, unchanged=unchanged, partial=tile_stats, final=stats())
    (output / "presentation.json").write_text(json.dumps(report, indent=2, default=str))
    print("Cached GPU presentation, background, selection, AOV switch and tile cancellation passed")
