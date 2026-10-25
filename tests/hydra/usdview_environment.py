"""Verify the Prism > Environment... menu in a real usdview session with PrismRT.

Run with stock testusdview's --testScript option, a stage such as
fixtures/unlit.usda (no lights of its own, camera /World/Camera) and
PRISM_DEVICE=cpu. Saves default.png (usdview's default dome), then screenshots
with a chosen HDR (as authored by the dialog), a rotated and a darker one, and
off.png, which must match default.png again. Output: PRISM_VALIDATION_DIR
(default local/reports/usdview_env).
"""
import os
import time
from pathlib import Path

from pxr import Sdf
from pxr.Usdviewq.qt import QtWidgets


def _mean(image):
    total = [0, 0, 0]
    count = 0
    for y in range(0, image.height(), 3):
        for x in range(0, image.width(), 3):
            r, g, b = image.pixelColor(x, y).getRgb()[:3]
            total[0] += r
            total[1] += g
            total[2] += b
            count += 1
    return [t / count for t in total]


def _diff(a, b):
    total = 0.0
    count = 0
    for y in range(0, a.height(), 3):
        for x in range(0, a.width(), 3):
            total += sum(abs(p - q) for p, q in zip(a.pixelColor(x, y).getRgb()[:3],
                                                    b.pixelColor(x, y).getRgb()[:3]))
            count += 3
    return total / count


def testUsdviewInputFunction(appController):
    import prismUsdview.environment as environment

    view = appController._stageView
    model = appController._dataModel
    settings = model.viewSettings
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "local/reports/usdview_env"))
    output.mkdir(parents=True, exist_ok=True)
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    settings.showHUD = False
    settings.showBBoxes = False
    settings.domeLightTexturesVisible = True  # the HDR shows as the backdrop
    settings.cameraPath = Sdf.Path("/World/Camera")
    view.SetPhysicalWindowSize(480, 320)
    view.makeCurrent()
    view.grabFrameBuffer()
    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"

    def settle():
        end = time.monotonic() + 1.0  # let the edit reach the renderer
        while time.monotonic() < end:
            QtWidgets.QApplication.processEvents()
            time.sleep(0.01)
        deadline = time.monotonic() + 240
        view.updateView()
        while time.monotonic() < deadline:
            view.updateGL()
            QtWidgets.QApplication.processEvents()
            if view.IsRendererConverged():
                return
            time.sleep(0.01)
        raise AssertionError("PrismRT did not converge")

    images = {}

    def capture(name):
        settle()
        image = view.grabFrameBuffer()
        assert image.save(str(output / name)), name
        images[name] = image
        return _mean(image)

    stage = model.stage
    default = capture("default.png")
    check(not stage.GetPrimAtPath(environment.ENVIRONMENT_PATH).IsValid(),
          "environment exists before use")

    # The menu entry exists and opens the dialog.
    bar = appController._mainWindow.menuBar()
    titles = [a.text().replace("&", "") for a in bar.actions()]
    check("Prism" in titles, "no Prism menu: %s" % titles)
    entry = []
    for top in bar.actions():
        if top.text().replace("&", "") == "Prism" and top.menu():
            entry += [a for a in top.menu().actions() if a.text() == "Environment..."]
    check(len(entry) == 1, "no Prism > Environment... entry")
    entry[0].trigger()
    dialog = environment._dialogs.get("dialog")
    assert dialog is not None, "the menu entry did not open the dialog"
    labels = [dialog.source.itemText(i) for i in range(dialog.source.count())]
    print("dialog sources:", labels)
    check(labels[0].startswith("Off"), "first dialog entry is not Off")

    # Pick the first real environment (USD's Stinson Beach), then a repo EXR.
    choices = environment.environment_choices()
    check(bool(choices), "no environment choices")
    picks = [choices[0]] + ([c for c in choices if c[1].lower().endswith(".exr")][:1])
    results = {}
    for label, path in picks:
        index = [i for i in range(dialog.source.count())
                 if dialog.source.itemData(i) == path][0]
        dialog.source.setCurrentIndex(index)
        name = "env_" + Path(path).stem + ".png"
        results[label] = capture(name)
        prim = stage.GetPrimAtPath(environment.ENVIRONMENT_PATH)
        check(prim.IsValid(), "no environment prim for " + label)
        check(not stage.GetRootLayer().dirty, "root layer modified")
        check(stage.GetSessionLayer().GetPrimAtPath(environment.ENVIRONMENT_PATH)
              is not None, "environment not in the session layer")
        check(stage.GetRootLayer().GetPrimAtPath(environment.ENVIRONMENT_PATH) is None,
              "environment leaked into the root layer")
        print(label, "mean rgb", results[label], "default", default)
    check(any(abs(a - b) > 3 for a, b in zip(results[picks[-1][0]], default)),
          "the chosen environment looks like the default dome")

    # Rotation and exposure change the image.
    before = results[picks[0][0]] if len(picks) == 1 else results[picks[-1][0]]
    dialog.rotation.setValue(180.0)
    rotated = capture("rotated.png")
    turn = _diff(images["rotated.png"], images["env_" + Path(picks[-1][1]).stem + ".png"])
    print("rotation 180 mean abs pixel difference", turn)
    check(turn > 1, "rotating the environment did not change the image: %s" % turn)
    dialog.exposure.setValue(-2.0)
    dark = capture("exposure_minus2.png")
    check(sum(dark) < sum(rotated) * 0.85, "exposure -2 did not darken: %s %s" % (dark, rotated))
    print("before", before, "rotated", rotated, "dark", dark)

    # Off brings usdview's default dome back.
    dialog.source.setCurrentIndex(0)
    off = capture("off.png")
    check(not stage.GetPrimAtPath(environment.ENVIRONMENT_PATH).IsValid(),
          "environment prim survived Off")
    check(all(abs(a - b) < 2.5 for a, b in zip(off, default)),
          "Off did not restore the default dome: %s vs %s" % (off, default))
    dialog.close()
    if failures:
        raise AssertionError("Environment checks failed:\n" + "\n".join(failures))
    print("PrismRT usdview environment menu passed:", output.resolve())
