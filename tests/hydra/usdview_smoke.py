"""Run with the rebuilt USD's testusdview --testScript option and parity.usda."""
import os
import json
import time
from pathlib import Path

from pxr import Gf, Sdf, Usd, UsdGeom
from pxr.Usdviewq.qt import QtWidgets
from pxr.Usdviewq.common import SelectionHighlightModes


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-validation"))
    output.mkdir(parents=True, exist_ok=True)
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.viewSettings.selHighlightMode = SelectionHighlightModes.ALWAYS
    model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    view.SetPhysicalWindowSize(256, 256)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"
    for name, default, low, high in [
        ("prism:previewMaxDimension", 96, 16, 1024),
        ("prism:refinementDelayMs", 100, 0, 2000),
    ]:
        assert view.GetRendererSetting(name) == default
        view.SetRendererSetting(name, low - 1)
        assert view.GetRendererSetting(name) == low
        view.SetRendererSetting(name, high + 1)
        assert view.GetRendererSetting(name) == high
        view.SetRendererSetting(name, default)
    assert view.GetRendererSetting("prism:environmentSampling") == "mis"
    view.SetRendererSetting("prism:environmentSampling", "bsdf")
    assert view.GetRendererSetting("prism:environmentSampling") == "bsdf"
    view.SetRendererSetting("prism:environmentSampling", "mis")
    view.SetRendererSetting("prism:samplesPerPixel", 16)
    timings = {}

    def settle():
        start = time.monotonic()
        view.updateView()
        # Process at least one frame even if the previous render converged.
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if view.IsRendererConverged():
                return time.monotonic() - start
            if time.monotonic() - start > 180:
                raise AssertionError("usdview renderer failed to converge")
            time.sleep(0.01)

    def shot(name):
        image = view.grabFrameBuffer()
        assert not image.isNull(), "usdview produced no framebuffer"
        assert image.save(str(output / name)), "Cannot save usdview screenshot"
        return image

    settle()
    original = shot("01-color.png")
    initial_builds = view._renderer.GetRenderStats().get("snapshotBuilds")
    inside, frustum = view.computePickFrustum(view.width() / 2, view.height() / 2)
    assert inside
    hit = view.pick(frustum)
    assert str(hit[2]) == "/World/Panel", f"Unexpected center pick: {hit}"

    model.selection.setPrimPath(Sdf.Path("/World/Panel"))
    settle()
    selected = shot("02-selected.png")
    assert selected != original, "Selection did not change the viewport"
    center = (selected.width() // 2, selected.height() // 2)
    assert selected.pixelColor(*center) == original.pixelColor(*center), \
        "Outline mode tinted the selected object's interior"
    view.SetRendererSetting("prism:selectionOutlineRadius", 6)
    settle()
    wide = shot("02-selected-wide.png")
    assert wide != selected, "Outline radius did not update the viewport"
    view.SetRendererSetting("prism:selectionOutline", False)
    settle()
    filled = shot("02-selected-filled.png")
    assert filled.pixelColor(*center) != original.pixelColor(*center), \
        "Filled selection mode did not tint the object's interior"
    view.SetRendererSetting("prism:selectionOutline", True)
    view.SetRendererSetting("prism:selectionOutlineRadius", 2)
    model.selection.clearPrims()
    settle()
    assert shot("02-deselected.png") == original, \
        "Deselecting did not restore the original viewport"
    assert view._renderer.GetRenderStats().get("snapshotBuilds") == initial_builds, \
        "Selection presentation rebuilt scene geometry"

    camera_translate = model.stage.GetPrimAtPath("/World/Camera").GetAttribute("xformOp:translate")
    # A moved camera must immediately publish a current-camera preview, even
    # when no full-resolution sample can finish before the next input event.
    view.SetRendererSetting("prism:samplesPerPixel", 1)
    view.SetPhysicalWindowSize(768, 768)
    settle()
    camera_baseline = shot("02-camera-baseline.png")
    view.SetRendererSetting("prism:samplesPerPixel", 1000000)
    previews_before = view._renderer.GetRenderStats().get("previewPublishes", 0)
    camera_translate.Set(Gf.Vec3d(0.5, 0, 4))
    view.updateView()
    view.updateGL()
    QtWidgets.QApplication.processEvents()
    in_flight = shot("02-camera-in-flight.png")
    assert view._renderer.GetRenderStats().get("previewPublishes", 0) > previews_before, \
        "Camera update did not publish an interactive preview"
    assert in_flight != camera_baseline, "Camera update kept displaying the old camera"
    sampled_colors = {
        in_flight.pixelColor(x, y).rgba()
        for y in range(0, in_flight.height(), 16)
        for x in range(0, in_flight.width(), 16)
    }
    assert len(sampled_colors) > 1, "Camera reset presented a uniform blank frame"
    view.SetRendererSetting("prism:samplesPerPixel", 16)
    settle()
    assert shot("02-camera-moved.png") != original, "Camera edit did not update the viewport"
    camera_translate.Set(Gf.Vec3d(0, 0, 4))
    view.SetPhysicalWindowSize(256, 256)
    settle()
    assert view._renderer.GetRenderStats().get("snapshotBuilds") == initial_builds, \
        "Camera-only navigation rebuilt scene geometry"

    points = UsdGeom.Mesh(model.stage.GetPrimAtPath("/World/Panel")).GetPointsAttr()
    original_points = points.Get()
    old_frame = model.currentFrame
    points.Set(original_points, Usd.TimeCode(1))
    points.Set([p * 0.6 for p in original_points], Usd.TimeCode(2))
    model.currentFrame = Usd.TimeCode(1)
    settle()
    frame_one = shot("02-time-1.png")
    model.currentFrame = Usd.TimeCode(2)
    timings["geometryEditToConvergedSeconds"] = settle()
    assert shot("02-time-2.png") != frame_one, "Time change did not update geometry"
    points.ClearAtTime(Usd.TimeCode(1))
    points.ClearAtTime(Usd.TimeCode(2))
    model.currentFrame = old_frame
    settle()

    base_color = model.stage.GetPrimAtPath("/World/Mat/Surface").GetAttribute("inputs:base_color")
    base_color.Set(Gf.Vec3f(0.1, 0.6, 0.2))
    timings["materialEditToConvergedSeconds"] = settle()
    assert shot("02-material-edited.png") != original, "Material edit reused stale compiled code"
    base_color.Set(Gf.Vec3f(0.5, 0.2, 0.1))
    settle()

    panel = UsdGeom.Imageable(model.stage.GetPrimAtPath("/World/Panel"))
    panel.MakeInvisible()
    timings["visibilityEditToConvergedSeconds"] = settle()
    hidden = shot("03-hidden.png")
    assert hidden != original, "Visibility edit did not update the viewport"
    panel.MakeVisible()
    settle()

    assert view.SetRendererAov("depth")
    settle()
    shot("04-depth.png")
    assert view.SetRendererAov("color")
    view.SetPhysicalWindowSize(320, 240)
    timings["resizeToConvergedSeconds"] = settle()
    shot("05-resized.png")

    # Destroy and recreate the Prism renderer while the USD stage stays alive.
    # Storm's dome-light GPU path does not accept this deliberately tiny PPM
    # parity fixture. Temporarily clear only the dome texture while exercising
    # the renderer switch so its unrelated Hio error does not dirty Tf's test
    # error mark.
    dome_texture = model.stage.GetPrimAtPath("/World/Environment").GetAttribute(
        "inputs:texture:file")
    saved_dome_texture = dome_texture.Get()
    dome_texture.Set(Sdf.AssetPath())
    assert view.SetRendererPlugin("HdStormRendererPlugin")
    QtWidgets.QApplication.processEvents()
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    dome_texture.Set(saved_dome_texture)
    view.SetRendererSetting("prism:samplesPerPixel", 8)
    settle()
    shot("06-recreated.png")
    (output / "interaction-timings.json").write_text(json.dumps(timings, indent=2))
    print("PrismRT usdview smoke checks passed; screenshots:", output.resolve())
