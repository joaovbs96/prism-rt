"""Run with the rebuilt USD's testusdview --testScript option and parity.usda."""
import os
import json
import time
from pathlib import Path

from pxr import Gf, Sdf, Usd, UsdGeom, UsdShade
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
        ("prism:previewTargetMs", 33, 1, 1000),
    ]:
        assert view.GetRendererSetting(name) == default
        view.SetRendererSetting(name, low - 1)
        assert view.GetRendererSetting(name) == low
        view.SetRendererSetting(name, high + 1)
        assert view.GetRendererSetting(name) == high
        view.SetRendererSetting(name, default)
    assert view.GetRendererSetting("prism:environmentSampling") == "mis"
    assert view.GetRendererSetting("prism:adaptivePreview") is True
    assert view.GetRendererSetting("prism:textureFiltering") == "filtered"
    view.SetRendererSetting("prism:textureFiltering", "bilinear")
    assert view.GetRendererSetting("prism:textureFiltering") == "bilinear"
    view.SetRendererSetting("prism:textureFiltering", "filtered")
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

    def wait_latest_generation(previous_generation=None, timeout=180):
        deadline = time.monotonic() + timeout
        view.updateView()
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            stats = view._renderer.GetRenderStats()
            requested = stats.get("preparationGeneration", 0)
            displayed = stats.get("displayedGeneration", -1)
            changed = previous_generation is None or requested > previous_generation
            if (changed and not stats.get("preparationPending", True) and
                    displayed >= requested and stats.get("completedSamples", 0) >= 1 and
                    view.IsRendererConverged()):
                return stats
            if time.monotonic() > deadline:
                raise AssertionError("Latest texture generation did not settle: " + str(stats))
            time.sleep(.01)

    def wait_for_pending(previous_generation, timeout=10):
        deadline = time.monotonic() + timeout
        view.updateView()
        while time.monotonic() < deadline:
            stats = view._renderer.GetRenderStats()
            if (stats.get("preparationGeneration", 0) > previous_generation and
                    stats.get("preparationPending", False)):
                return stats
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            time.sleep(.001)
        raise AssertionError("Texture supersession probe never observed preparationPending")

    settle()
    original = shot("01-color.png")
    initial_builds = view._renderer.GetRenderStats().get("snapshotBuilds")
    inside, frustum = view.computePickFrustum(view.width() / 2, view.height() / 2)
    assert inside
    hit = view.pick(frustum)
    assert str(hit[2]) == "/World/Panel", f"Unexpected center pick: {hit}"

    selection_stats_before = view._renderer.GetRenderStats()
    model.selection.setPrimPath(Sdf.Path("/World/Panel"))
    settle()
    selection_stats_after = view._renderer.GetRenderStats()
    assert selection_stats_after.get("selectionOffsetCacheMisses", 0) > \
        selection_stats_before.get("selectionOffsetCacheMisses", 0), \
        "Live selection did not invalidate cached selection encoding"
    selected = shot("02-selected.png")
    assert selected != original, "Selection did not change the viewport"
    model.viewSettings.selHighlightMode = SelectionHighlightModes.NEVER
    settle()
    assert shot("02-highlight-disabled.png") == original, \
        "Disabling host selection highlight reused a stale selection table"
    model.viewSettings.selHighlightMode = SelectionHighlightModes.ALWAYS
    settle()
    assert shot("02-highlight-restored.png") == selected, \
        "Re-enabling host selection highlight did not restore selection"
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
    before_stats = view._renderer.GetRenderStats()
    previews_before = before_stats.get("previewPublishes", 0)
    camera_revision_before = before_stats["requestedCameraRevision"]
    preview_start = time.monotonic()
    camera_translate.Set(Gf.Vec3d(0.5, 0, 4))
    view.updateView()
    def wait_preview(previous):
        deadline = time.monotonic() + 5
        while view._renderer.GetRenderStats()["displayedCameraRevision"] <= previous:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            assert time.monotonic() < deadline, "Camera update did not publish a preview"
            time.sleep(0.001)
        # Upload the complete worker publication in the presentation task.
        view.updateGL()
        view.grabFrameBuffer()
    wait_preview(camera_revision_before)
    camera_stats = view._renderer.GetRenderStats()
    if camera_stats.get("selectionOffsetCalls", 0) > \
            before_stats.get("selectionOffsetCalls", 0):
        assert camera_stats.get("selectionOffsetCacheHits", 0) > \
            before_stats.get("selectionOffsetCacheHits", 0), \
            "Camera-only frame did not reuse selection encoding"
        assert camera_stats.get("selectionPathsVisited", 0) == \
            before_stats.get("selectionPathsVisited", 0), \
            "Camera-only frame traversed scene selection paths"
        assert camera_stats.get("selectionOffsetCacheMisses", 0) == \
            before_stats.get("selectionOffsetCacheMisses", 0), \
            "Camera-only frame missed the selection encoding cache"
    timings["cameraInputToImageSeconds"] = time.monotonic() - preview_start
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
    latencies = []
    for event in range(60):
        before = view._renderer.GetRenderStats()["requestedCameraRevision"]
        start = time.monotonic()
        camera_translate.Set(Gf.Vec3d(0.5 + (event % 20 - 10) * 0.02, 0, 4))
        view.updateView()
        wait_preview(before)
        latencies.append(time.monotonic() - start)
    ordered = sorted(latencies)
    timings["navigation"] = dict(
        samples=latencies, p95Seconds=ordered[int(.95*(len(ordered)-1))],
        p99Seconds=ordered[int(.99*(len(ordered)-1))],
        updatesPerSecond=len(latencies)/sum(latencies),
        targetP95Seconds=.05,
        metP95Target=ordered[int(.95*(len(ordered)-1))] < .05)
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

    # Exercise real MaterialX image-node resource identity and file-input edits.
    # The two uniform midtone fixtures are generated under this run's validation
    # directory so no external image asset or cache state is involved.
    def write_midtone_ppm(path, rgb):
        path.write_bytes(b"P6\n4 4\n255\n" + bytes(rgb) * 16)

    texture_a = output / "texture-a.ppm"
    texture_b = output / "texture-b.ppm"
    write_midtone_ppm(texture_a, (188, 76, 64))
    write_midtone_ppm(texture_b, (64, 112, 196))
    material_path = Sdf.Path("/World/Mat")
    surface = UsdShade.Shader(model.stage.GetPrimAtPath(material_path.AppendChild("Surface")))
    surface_base = surface.GetInput("base_color")
    original_base_color = surface_base.Get()
    image_shader = UsdShade.Shader.Define(model.stage, material_path.AppendChild("Image"))
    image_shader.CreateIdAttr("ND_image_color3")
    image_file = image_shader.CreateInput("file", Sdf.ValueTypeNames.Asset)
    image_uv = UsdShade.Shader.Define(model.stage, material_path.AppendChild("TextureUV"))
    image_uv.CreateIdAttr("ND_texcoord_vector2")
    image_output = image_shader.CreateOutput("out", Sdf.ValueTypeNames.Color3f)
    uv_output = image_uv.CreateOutput("out", Sdf.ValueTypeNames.Float2)
    image_shader.CreateInput("texcoord", Sdf.ValueTypeNames.Float2).ConnectToSource(uv_output)
    surface_base.ConnectToSource(image_output)
    image_file.Set(Sdf.AssetPath(str(texture_a.resolve())))
    texture_a_stats = wait_latest_generation()
    texture_a_image = shot("02-texture-a.png")

    image_file.Set(Sdf.AssetPath(str(texture_b.resolve())))
    texture_b_stats = wait_latest_generation(texture_a_stats.get("preparationGeneration", 0))
    texture_b_image = shot("02-texture-b.png")
    assert texture_b_image != texture_a_image, "USD texture file edit did not change the material image"
    assert texture_b_stats.get("materialFailures", 0) == 0, "Texture B material preparation failed"
    assert texture_b_stats.get("materialSdkTranslationCalls", 0) == \
        texture_a_stats.get("materialSdkTranslationCalls", 0), \
        "Changing only the texture resource translated a new native program"

    # Establish an independently settled B reference at the camera used for
    # the final post-supersession comparison.
    texture_camera = Gf.Vec3d(.12, 0, 4)
    camera_translate.Set(texture_camera)
    settle()
    texture_b_reference_stats = view._renderer.GetRenderStats()
    texture_b_reference = shot("02-texture-b-reference.png")
    camera_translate.Set(Gf.Vec3d(0, 0, 4))
    settle()

    variant_multiply = UsdShade.Shader.Define(
        model.stage, material_path.AppendChild("TextureVariant"))
    variant_multiply.CreateIdAttr("ND_multiply_color3")
    variant_multiply.CreateInput("in2", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(.83, .91, .77))
    variant_multiply.CreateInput("in1", Sdf.ValueTypeNames.Color3f).ConnectToSource(image_output)
    variant_output = variant_multiply.CreateOutput("out", Sdf.ValueTypeNames.Color3f)
    image_file.Set(Sdf.AssetPath(str(texture_a.resolve())))
    surface_base.ConnectToSource(variant_output)
    pending_before_stats = view._renderer.GetRenderStats()
    pending_before = pending_before_stats.get("preparationGeneration", 0)
    discarded_before = pending_before_stats.get("discardedSnapshotBuilds", 0)
    pending_stats = wait_for_pending(pending_before)

    # Supersede the uncompiled graph/resource generation with the already
    # prepared B graph while also changing camera state. The overlap is only
    # claimed when preparationPending was observed above.
    surface_base.DisconnectSource()
    surface_base.ConnectToSource(image_output)
    image_file.Set(Sdf.AssetPath(str(texture_b.resolve())))
    camera_translate.Set(texture_camera)
    final_texture_stats = wait_latest_generation(
        pending_stats.get("preparationGeneration", pending_before))
    final_texture_image = shot("02-texture-b-after-supersession.png")
    assert final_texture_stats.get("displayedGeneration", -1) == \
        final_texture_stats.get("preparationGeneration", -2), \
        "A stale texture generation remained displayed after supersession"
    assert final_texture_stats.get("materialFailures", 0) == 0, \
        "Texture supersession produced a material failure"
    assert final_texture_stats.get("discardedSnapshotBuilds", 0) > discarded_before, \
        "The obsolete texture graph generation was not discarded"
    assert final_texture_image == texture_b_reference, \
        "Latest texture generation did not reproduce independently settled B pixels"
    timings["textureEditSupersession"] = dict(
        pendingObserved=True,
        cancelledGeneration=pending_stats.get("preparationGeneration"),
        finalGeneration=final_texture_stats.get("preparationGeneration"),
        displayedGeneration=final_texture_stats.get("displayedGeneration"),
        discardedSnapshotBuildsBefore=discarded_before,
        discardedSnapshotBuildsAfter=final_texture_stats.get("discardedSnapshotBuilds", 0),
        textureAProgramTranslations=texture_a_stats.get("materialSdkTranslationCalls"),
        textureBProgramTranslations=texture_b_stats.get("materialSdkTranslationCalls"),
        finalMaterialFailures=final_texture_stats.get("materialFailures", 0))

    surface_base.DisconnectSource()
    surface_base.Set(original_base_color)
    model.stage.RemovePrim(variant_multiply.GetPath())
    model.stage.RemovePrim(image_shader.GetPath())
    model.stage.RemovePrim(image_uv.GetPath())
    camera_translate.Set(Gf.Vec3d(0, 0, 4))
    settle()
    assert shot("02-texture-restored.png") == original, \
        "Restoring the original material input did not restore the original image"

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
