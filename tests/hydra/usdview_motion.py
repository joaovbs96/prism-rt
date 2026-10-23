"""Sustained mouse-drag regression; run using OpenUSD's testusdview.

Accepts parity.usda or a larger asset such as OpenChessSet. No stage is saved.
PRISM_VALIDATION_DIR selects the screenshot and timing output directory.
"""
import json
import os
import statistics
import time
from pathlib import Path

import numpy as np
from pxr import Sdf
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-motion"))
    output.mkdir(parents=True, exist_ok=True)
    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.selection.clearPrims()
    # Moving GL overlays must not make a stale renderer image pass this test.
    view.DrawAxis = lambda *_: None
    fixture = bool(model.stage.GetPrimAtPath("/World/Panel"))
    if fixture:
        model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    else:
        model.viewSettings.domeLightEnabled = True
        view.updateView(resetCam=True, forceComputeBBox=True)
    view.SetPhysicalWindowSize(768, 768)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    view.SetRendererSetting("prism:samplesPerPixel", 1)

    def settle():
        start = time.monotonic()
        view.updateView()
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if view.IsRendererConverged():
                return
            assert time.monotonic() - start < 240, "Render failed to converge"
            time.sleep(0.005)

    def capture():
        image = view.grabFrameBuffer().convertToFormat(QtGui.QImage.Format_RGBA8888)
        pixels = np.frombuffer(image.constBits(), dtype=np.uint8).reshape(
            image.height(), image.bytesPerLine())[:, :image.width() * 4]
        return image, pixels.reshape(image.height(), image.width(), 4).copy()

    def capture_current_camera(previous_revision, label):
        """Pump the async renderer through the exact latest camera revision."""
        deadline = time.monotonic() + 10.0
        target_revision = None
        while True:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            stats = view._renderer.GetRenderStats()
            requested = stats.get("requestedCameraRevision", 0)
            displayed = stats.get("displayedCameraRevision", 0)
            if requested > previous_revision:
                # One input can cause usdview to refine its free-camera state
                # more than once. Require the framebuffer for the final
                # revision, rather than accepting an older preview publish.
                target_revision = requested
            if (target_revision is not None and
                    requested == target_revision and
                    displayed == target_revision):
                # The worker may have published after the preceding GL paint.
                # Force one presentation pass, capture it, and verify that no
                # newer camera revision arrived across that framebuffer read.
                view.updateGL()
                QtWidgets.QApplication.processEvents()
                image, pixels = capture()
                presented_stats = view._renderer.GetRenderStats()
                if (presented_stats.get("requestedCameraRevision", 0) ==
                        target_revision and
                        presented_stats.get("displayedCameraRevision", 0) ==
                        target_revision):
                    return image, pixels, presented_stats, target_revision
            assert time.monotonic() < deadline, \
                (f"{label} did not publish its requested camera revision "
                 f"(before={previous_revision}, requested={requested}, "
                 f"displayed={displayed}, target={target_revision})")
            time.sleep(0.001)

    def mouse(kind, x, y, button, buttons):
        point = QtCore.QPointF(x, y)
        return QtGui.QMouseEvent(kind, point, point, button, buttons,
                                QtCore.Qt.KeyboardModifier.AltModifier)

    settle()
    view.switchToFreeCamera()
    if not fixture:
        model.viewSettings.freeCamera.Tumble(0, 25)
        model.viewSettings.freeCamera.AdjustDistance(0.8)
    settle()
    baseline_camera = model.viewSettings.freeCamera.clone()
    baseline_builds = view._renderer.GetRenderStats()["snapshotBuilds"]
    view.SetRendererSetting("prism:samplesPerPixel", 1000000)
    view.SetRendererSetting("prism:adaptiveThreshold", 0.0)  # keep refining
    buttons = QtCore.Qt.MouseButton
    events = QtCore.QEvent.Type
    rows = []
    # Both trucking and orbiting exercise usdview's actual mouseMoveEvent path.
    for mode, button in [("truck", buttons.MiddleButton), ("orbit", buttons.LeftButton)]:
        model.viewSettings.freeCamera = baseline_camera.clone()
        view.updateView()
        x, y = view.width() // 2, view.height() // 2
        view.mousePressEvent(mouse(events.MouseButtonPress, x, y, button, button))
        previous_image, previous = capture()
        centers = []
        for step in range(24):
            # Exceed one coarse preview cell so silhouette motion is visible
            # even for the fixture's constant-color Lambertian surface.
            x += (12 if step < 12 else -12) if mode == "truck" else 4
            if mode == "orbit":
                y += 2
            before_stats = view._renderer.GetRenderStats()
            before = before_stats.get("previewPublishes", 0)
            before_revision = before_stats.get("requestedCameraRevision", 0)
            start = time.perf_counter()
            view.mouseMoveEvent(mouse(events.MouseMove, x, y, buttons.NoButton, button))
            # Preview generation is asynchronous. Pump Qt and capture until
            # the framebuffer carries this event's final camera revision.
            # The latency includes all waiting, repainting, and framebuffer
            # reads, rather than timing only event enqueue.
            image, current, stats, camera_revision = capture_current_camera(
                before_revision, f"{mode} step {step}")
            milliseconds = (time.perf_counter() - start) * 1000
            changed = int(np.count_nonzero(np.any(current != previous, axis=2)))
            assert changed > 100, f"{mode} step {step} retained a frozen image"
            assert stats.get("previewPublishes", 0) > before, \
                f"{mode} step {step} did not publish the current camera"
            assert stats["snapshotBuilds"] == baseline_builds, "Drag rebuilt geometry"
            assert not view.IsRendererConverged(), "Preview reported final convergence"
            # In the controlled fixture track the brown panel, not random noise.
            if fixture and mode == "truck":
                rgb = current[..., :3].astype(np.float32)
                mask = (rgb[..., 0] > 1.3 * rgb[..., 1]) & (rgb[..., 0] > 20)
                assert np.count_nonzero(mask) > 100, "Panel disappeared during drag"
                centers.append(float(np.nonzero(mask)[1].mean()))
            rows.append({"mode": mode, "step": step, "frame_ms": milliseconds,
                         "camera_revision": camera_revision,
                         "changed_pixels": changed,
                         "preview_ms": stats.get("previewMilliseconds"),
                         "prepare_ms": stats.get("prepareMilliseconds"),
                         "stop_ms": stats.get("stopRenderMilliseconds"),
                         "completed_samples": stats["completedSamples"]})
            if step in (0, 11, 23):
                assert image.save(str(output / f"{mode}-{step:02}.png"))
            previous = current
        # All assertions above occur with the button still held.
        if centers:
            assert centers[11] - centers[0] > 15, "Panel did not follow camera trucking"
            assert centers[-1] < centers[11] - 15, "Panel did not follow the reversed drag"
        view.mouseReleaseEvent(mouse(events.MouseButtonRelease, x, y, button, buttons.NoButton))
    view.SetRendererSetting("prism:samplesPerPixel", 16)
    settle()
    assert view._renderer.GetRenderStats()["completedSamples"] == 16
    image, _ = capture()
    assert image.save(str(output / "settled.png"))
    timings = [row["frame_ms"] for row in rows]
    result = {"median_frame_ms": statistics.median(timings),
              "p95_frame_ms": sorted(timings)[int(len(timings) * 0.95)],
              "frames": rows}
    (output / "motion.json").write_text(json.dumps(result, indent=2))
    # Broad scheduling tolerance, but catch viewport-sized tracing on the UI thread.
    assert result["median_frame_ms"] < float(os.environ.get("PRISM_MOTION_MAX_MS", 150)), result
    print("PrismRT sustained drag passed:", result["median_frame_ms"], "ms median;",
          result["p95_frame_ms"], "ms p95; screenshots:", output)
