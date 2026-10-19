"""Run with testusdview --testScript and parity.usda.

Checks the PrismRT sample HUD overlay installed by the prismUsdview extension:
it appears under usdview's top-right Hydra line while refining, flips to a
converged marker, follows the HUD toggle, and hides for other renderers.
Screenshots of the viewport widget go to PRISM_VALIDATION_DIR.
"""
import os
import time
from pathlib import Path

from pxr import Sdf
from pxr.Usdviewq.qt import QtWidgets

import prismUsdview


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-validation"))
    output.mkdir(parents=True, exist_ok=True)
    model.viewSettings.showHUD = True
    model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    view.SetPhysicalWindowSize(512, 384)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"

    def labels():
        return view.findChildren(prismUsdview._SampleLabel)

    def pump(seconds, until=None):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if until and until():
                return True
            time.sleep(0.02)
        return until is None

    def texts():
        return [text for label in labels() for text, _ in label.lines()
                if label.isVisible()]

    def shot(name):
        image = view.grab()
        assert image.save(str(output / name)), "Cannot save HUD screenshot"

    # Refining: an unreachable target keeps the renderer busy.
    view.SetRendererSetting("prism:samplesPerPixel", 1000000)
    view.updateView()
    assert pump(60, lambda: any("/ 1000000" in t for t in texts())), \
        "Refining overlay never appeared: %r" % texts()
    assert any(t.endswith("refining") for t in texts()), texts()
    label = labels()[0]
    assert label.geometry().x() == view.width() - 160, label.geometry()
    assert label.geometry().y() > 14 + 15, label.geometry()
    shot("hud-refining.png")

    # Converged: the target is reached and the marker changes.
    view.SetRendererSetting("prism:samplesPerPixel", 16)
    view.updateView()
    assert pump(120, lambda: view.IsRendererConverged() and
                any(t.endswith("converged") for t in texts())), \
        "Converged overlay never appeared: %r" % texts()
    assert "Samples: 16 / 16" in texts(), texts()
    assert label.lines()[-1][1] == prismUsdview._CONVERGED_COLOR
    shot("hud-converged.png")

    # usdview's own HUD toggle hides the overlay.
    model.viewSettings.showHUD = False
    assert pump(5, lambda: not label.isVisible()), "Overlay ignored the HUD toggle"
    model.viewSettings.showHUD = True
    assert pump(5, label.isVisible), "Overlay did not return with the HUD"

    # Resizing keeps the overlay on the right edge.
    view.SetPhysicalWindowSize(640, 384)
    assert pump(5, lambda: label.geometry().x() == view.width() - 160), \
        "Overlay did not follow the resize: %r" % label.geometry()

    # A different renderer owns the viewport: no PrismRT overlay.
    dome = model.stage.GetPrimAtPath("/World/Environment").GetAttribute(
        "inputs:texture:file")
    saved = dome.Get()
    dome.Set(Sdf.AssetPath())  # Storm cannot load the tiny parity dome.
    assert view.SetRendererPlugin("HdStormRendererPlugin")
    assert pump(5, lambda: not label.isVisible()), "Overlay shown for Storm"
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    dome.Set(saved)
    assert pump(60, label.isVisible), "Overlay missing after switching back"
    print("PrismRT sample HUD checks passed; screenshots:", output.resolve())
