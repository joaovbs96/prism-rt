"""Run with testusdview --testScript.

Records what the PrismRT HUD shows while a scene loads: starts the PrismRT
renderer, polls the render statistics and the overlay, and saves a screenshot
of the viewport the first time each phase shows (loading, materials, textures,
GPU kernel, upload, rendering) and again once the image converges. A timeline
of the phases and the overlay text goes to <tag>-timeline.json.

Environment:
  PRISM_VALIDATION_DIR  output directory (default build/hydra-hud-phases)
  PRISM_HUD_TAG         file name prefix (default "hud")
  PRISM_HUD_CAMERA      camera prim path (default /World/Camera)
  PRISM_HUD_SAMPLES     samples to add before the converged shot (default 32)
  PRISM_HUD_SIZE        viewport "WxH" (default 640x400)
  PRISM_HUD_TIMEOUT     seconds to wait for convergence (default 900)
"""
import json
import os
import time
from pathlib import Path

from pxr import Sdf
from pxr.Usdviewq.qt import QtWidgets

import prismUsdview


_SHOT_TEXT = {
    "loading": "Loading scene", "materials": "Compiling materials",
    "textures": "Preparing textures", "kernel": "Compiling GPU kernel",
    "upload": "Uploading to GPU", "rendering": "Samples:",
}


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-hud-phases"))
    output.mkdir(parents=True, exist_ok=True)
    tag = os.environ.get("PRISM_HUD_TAG", "hud")
    width, height = (int(v) for v in os.environ.get("PRISM_HUD_SIZE", "640x400").split("x"))
    timeout = float(os.environ.get("PRISM_HUD_TIMEOUT", "900"))
    model.viewSettings.showHUD = True
    model.viewSettings.cameraPath = Sdf.Path(os.environ.get("PRISM_HUD_CAMERA", "/World/Camera"))
    view.SetPhysicalWindowSize(width, height)
    view.makeCurrent()

    timeline = []
    shots = set()
    start = time.monotonic()

    def overlay():
        return [text for label in view.findChildren(prismUsdview._SampleLabel)
                if label.isVisible() for text, _ in label.lines()]

    def shot(name):
        image = view.grab()
        assert image.save(str(output / ("%s-%s.png" % (tag, name)))), "Cannot save " + name

    def stats():
        return dict(view._renderer.GetRenderStats())

    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"
    # An unreachable target keeps the first image refining for a "rendering"
    # shot; it is lowered afterwards so the image converges for the last one.
    final_samples = int(os.environ.get("PRISM_HUD_SAMPLES", "32"))
    view.SetRendererSetting("prism:samplesPerPixel", 1000000)
    view.updateView()
    lowered_at = None

    last = None
    converged_since = None
    deadline = start + timeout
    while time.monotonic() < deadline:
        view.updateGL()
        QtWidgets.QApplication.processEvents()
        try:
            current = stats()
        except Exception:
            current = {}
        phase = current.get("prismPhase")
        text = overlay()
        key = (phase, current.get("prismDevice"), tuple(text))
        if key != last and phase is not None:
            timeline.append({
                "t": round(time.monotonic() - start, 2), "phase": phase,
                "device": current.get("prismDevice"),
                "detail": current.get("prismPhaseDetail"),
                "active": current.get("prismActivePhases"),
                "samples": current.get("numCompletedSamples"),
                "overlay": text})
            last = key
        # First time a phase is on screen: screenshot of that frame. The overlay
        # refreshes every 100 ms, so wait for its text to name the phase.
        for name, words in _SHOT_TEXT.items():
            if name not in shots and any(words in t for t in text):
                shots.add(name)
                shot(name)
        # Long phases: a second look a few seconds in, with the elapsed counter.
        if phase == "kernel" and "kernel-later" not in shots and any(
                "GPU kernel" in t and t.rstrip().endswith(" s") for t in text) and any(
                "DXC" in t for t in text) and current.get("prismPhaseSeconds", 0) >= 6:
            shots.add("kernel-later")
            shot("kernel-later")
        if "rendering" in shots and lowered_at is None:
            lowered_at = time.monotonic()
        if lowered_at and lowered_at > 0 and time.monotonic() - lowered_at > 1.5:
            lowered_at = -1
            view.SetRendererSetting("prism:samplesPerPixel", int(current.get("numCompletedSamples", 0)) + final_samples)
            view.updateView()
        if lowered_at == -1 and view.IsRendererConverged() and current.get("numCompletedSamples", 0) > 0 \
                and phase == "rendering":
            converged_since = converged_since or time.monotonic()
            if time.monotonic() - converged_since > 0.6 and any(
                    t.endswith("converged") for t in overlay()):
                shot("converged")
                break
        time.sleep(0.02)
    else:
        raise AssertionError("Never converged; last overlay %r" % overlay())

    (output / ("%s-timeline.json" % tag)).write_text(
        json.dumps(timeline, indent=1), encoding="utf-8")
    phases = [entry["phase"] for entry in timeline]
    print("PrismRT HUD phases seen:", " -> ".join(dict.fromkeys(phases)))
    print("Final overlay:", overlay())
    print("Screenshots:", sorted(p.name for p in output.glob(tag + "-*.png")))
