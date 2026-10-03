"""Probe preview-buffer picking against settled full-resolution IDs.

Run with the configured OpenUSD ``testusdview --testScript`` harness and
``tests/hydra/fixtures/parity.usda`` using PrismRT. The script authors a small
cube in memory; the stage file is not modified.
"""

import os
import time
from pathlib import Path

from pxr import Gf, Sdf, UsdGeom
from pxr.Usdviewq.common import SelectionHighlightModes
from pxr.Usdviewq.qt import QtWidgets


TARGET = Sdf.Path("/World/PickTarget")


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-preview-pick"))
    output.mkdir(parents=True, exist_ok=True)

    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.viewSettings.selHighlightMode = SelectionHighlightModes.NEVER
    model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    view.SetPhysicalWindowSize(256, 256)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"
    view.SetRendererSetting("prism:previewMaxDimension", 16)
    view.SetRendererSetting("prism:adaptivePreview", False)
    view.SetRendererSetting("prism:refinementDelayMs", 0)
    view.SetRendererSetting("prism:samplesPerPixel", 1)

    initialStats = view._renderer.GetRenderStats()
    initialGeneration = initialStats.get("preparationGeneration", 0)
    camera = UsdGeom.Camera(model.stage.GetPrimAtPath("/World/Camera"))
    cameraTranslation = camera.GetPrim().GetAttribute("xformOp:translate")

    # Project to about x=30px and y=128px for the parity camera. Its roughly
    # five-pixel silhouette falls between the preview's eight-pixel sample
    # centers at x=24 and x=40, while full-resolution rays hit its right edge.
    cube = UsdGeom.Cube.Define(model.stage, TARGET)
    cube.CreateSizeAttr(1.0)
    cubeXform = UsdGeom.Xformable(cube.GetPrim())
    cubeXform.AddTranslateOp().Set(Gf.Vec3d(-0.875, 0.0, 0.08))
    cubeXform.AddScaleOp().Set(Gf.Vec3f(0.022, 0.022, 0.018))

    def pump_until(predicate, label, timeout=90):
        deadline = time.monotonic() + timeout
        view.updateView()
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            stats = view._renderer.GetRenderStats()
            if predicate(stats):
                return stats
            if time.monotonic() >= deadline:
                raise AssertionError(f"Timed out waiting for {label}: {stats}")
            time.sleep(0.005)

    def settle():
        view.updateView()
        return pump_until(lambda _stats: view.IsRendererConverged(), "convergence")

    # First find exact pixels covered by the tiny target at the settled camera.
    targetStats = pump_until(
        lambda s: s.get("preparationGeneration", 0) > initialGeneration and
                  not s.get("preparationPending", True) and
                  s.get("displayedGeneration", -1) >=
                      s.get("preparationGeneration", 0) and
                  view.IsRendererConverged(),
        "settled target-scene generation")
    fullCoordinates = []
    for y in range(120, 137):
        for x in range(22, 39):
            inside, frustum = view.computePickFrustum(x + 0.5, y + 0.5)
            if not inside:
                continue
            hit = view.pick(frustum)
            if hit and Sdf.Path(hit[2]) == TARGET:
                fullCoordinates.append((x, y))
    assert fullCoordinates, "Full-resolution pick scan did not find the authored cube"

    # Hold refinement long enough to query the published low-resolution ID
    # buffer, then return to the original camera before any full pass replaces it.
    view.SetRendererSetting("prism:refinementDelayMs", 2000)
    stats = view._renderer.GetRenderStats()
    previousRevision = stats.get("requestedCameraRevision", 0)
    originalTranslation = cameraTranslation.Get()
    cameraTranslation.Set(Gf.Vec3d(originalTranslation[0] + 0.1,
                                   originalTranslation[1],
                                   originalTranslation[2]))
    view.updateView()
    pump_until(lambda s: s.get("displayedCameraRevision", 0) > previousRevision,
               "displaced-camera preview")
    displacedRevision = view._renderer.GetRenderStats().get("requestedCameraRevision", 0)
    cameraTranslation.Set(originalTranslation)
    view.updateView()
    previewStats = pump_until(
        lambda s: s.get("displayedCameraRevision", 0) > displacedRevision and
                  s.get("previewPublishes", 0) > stats.get("previewPublishes", 0),
        "original-camera preview")
    view.updateGL()
    view.grabFrameBuffer()
    assert previewStats.get("previewWidth", 256) < 256 and \
           previewStats.get("previewHeight", 256) < 256, \
        "Test did not publish a reduced-resolution preview"

    mismatches = []
    previewPickStates = []
    for x, y in fullCoordinates:
        beforePick = view._renderer.GetRenderStats()
        assert beforePick.get("displayedCameraRevision") == \
               previewStats.get("displayedCameraRevision"), \
            f"Displayed camera changed before pick {(x, y)}: {beforePick}"
        assert beforePick.get("displayedGeneration") == \
               previewStats.get("displayedGeneration"), \
            f"Displayed scene generation changed before pick {(x, y)}: {beforePick}"
        assert beforePick.get("previewPublishes") == \
               previewStats.get("previewPublishes"), \
            f"Preview was replaced before pick {(x, y)}: {beforePick}"
        inside, frustum = view.computePickFrustum(x + 0.5, y + 0.5)
        if not inside:
            continue
        hit = view.pick(frustum)
        picked = Sdf.Path(hit[2]) if hit else Sdf.Path.emptyPath
        afterPick = view._renderer.GetRenderStats()
        assert afterPick.get("displayedCameraRevision") == \
               beforePick.get("displayedCameraRevision") and \
               afterPick.get("displayedGeneration") == \
               beforePick.get("displayedGeneration") and \
               afterPick.get("previewPublishes") == \
               beforePick.get("previewPublishes"), \
            f"Render state changed during pick {(x, y)}: {beforePick} -> {afterPick}"
        previewPickStates.append({
            "pixel": (x, y),
            "requestedCameraRevision": beforePick.get("requestedCameraRevision"),
            "displayedCameraRevision": beforePick.get("displayedCameraRevision"),
            "displayedGeneration": beforePick.get("displayedGeneration"),
            "previewPublishes": beforePick.get("previewPublishes"),
        })
        if picked != TARGET:
            mismatches.append(((x, y), str(picked)))

    output.joinpath("preview-pick.txt").write_text(
        f"preview={previewStats.get('previewWidth')}x{previewStats.get('previewHeight')}\n"
        f"previewStats={previewStats}\n"
        f"pickStates={previewPickStates}\n"
        f"fullTargetCoordinates={fullCoordinates}\n"
        f"previewMismatches={mismatches}\n",
        encoding="utf-8")
    assert not mismatches, (
        "Tiny-object edge picks differ at preview and full resolution; "
        f"published ray picking failed (generation "
        f"{targetStats.get('displayedGeneration')})")

    # Let refinement finish so testusdview exits with clean renderer teardown.
    view.SetRendererSetting("prism:refinementDelayMs", 0)
    settle()
