"""usdview integration check for point-instance picking and selection.

Run via the configured OpenUSD `testusdview --testScript` harness with
`tests/hydra/fixtures/instances.usda` and the PrismRT renderer enabled.
"""

import os
import time
from pathlib import Path

from pxr import Sdf
from pxr.Usdviewq.common import SelectionHighlightModes
from pxr.Usdviewq.qt import QtWidgets


def _settle(view):
    view.updateView()
    started = time.monotonic()
    while True:
        view.updateGL()
        QtWidgets.QApplication.processEvents()
        if view.IsRendererConverged():
            return
        if time.monotonic() - started > 180:
            raise AssertionError("usdview PrismRT render did not converge")
        time.sleep(0.01)


def _count_differences(a, b, center_x, center_y, radius):
    count = 0
    for y in range(max(0, center_y - radius), min(a.height(), center_y + radius + 1)):
        for x in range(max(0, center_x - radius), min(a.width(), center_x + radius + 1)):
            if a.pixelColor(x, y) != b.pixelColor(x, y):
                count += 1
    return count


def testUsdviewInputFunction(appController):
    view = appController._stageView
    model = appController._dataModel
    output = Path(os.environ.get("PRISM_VALIDATION_DIR", "build/hydra-validation"))
    output.mkdir(parents=True, exist_ok=True)

    model.viewSettings.showBBoxes = False
    model.viewSettings.showHUD = False
    model.viewSettings.selHighlightMode = SelectionHighlightModes.ALWAYS
    model.viewSettings.cameraPath = Sdf.Path("/World/Camera")
    view.SetPhysicalWindowSize(512, 288)
    view.makeCurrent()
    assert view.SetRendererPlugin("HdPrismRendererPlugin"), "PrismRT plugin unavailable"
    view.SetRendererSetting("prism:samplesPerPixel", 16)

    _settle(view)
    assert view.GetRendererSetting("prism:usdviewSelection") is not None, \
        "PrismRT usdview selection extension was not loaded by the launcher"
    before = view.grabFrameBuffer()
    assert not before.isNull(), "usdview produced no framebuffer"
    before.save(str(output / "instances-before.png"))

    # Camera is centered on the middle instance. Its authored source index is
    # 1 while its prototype index is 1 too; the first and last both use proto 0.
    inside, frustum = view.computePickFrustum(view.width() / 2, view.height() / 2)
    assert inside, "center pick fell outside the camera image"
    hit = view.pick(frustum)
    # usdview returns (worldPoint, worldNormal, prototypePrimPath,
    # elementIndex, instancerPath, authoredInstanceIndex).
    assert hit and str(hit[4]) == "/World/MainInstancer", \
        f"Expected authored point-instancer path, got pick {hit}"
    assert hit[5] == 1, f"Expected authored instance index 1, got pick {hit}"

    selection = model.selection
    selection.setPrimPath(Sdf.Path("/World/MainInstancer"), instance=hit[5])
    _settle(view)
    after = view.grabFrameBuffer()
    assert not after.isNull() and after != before, "instance selection did not alter the viewport"
    after.save(str(output / "instances-selected-1.png"))

    selected_instances = selection.getPrimPathInstances()
    expected_path = Sdf.Path("/World/MainInstancer")
    assert selected_instances == {expected_path: {1}}, \
        f"Selection model did not retain the picked source instance: {selected_instances}"

    # With the 512x288 framing, the left/center/right source instances project
    # near x=156/256/356. Selection should outline only the middle instance.
    # Use the blue cube's bottom edge: its side borders overlap the red cube
    # borders in screen space, which would make a central box count outlines
    # belonging to the neighboring instances.
    changed_selected = _count_differences(before, after, 256, 210, 25)
    changed_left = _count_differences(before, after, 156, 144, 42)
    changed_right = _count_differences(before, after, 356, 144, 42)
    assert changed_selected > 0, "selected instance has no outline/highlight pixels"
    assert changed_left == 0 and changed_right == 0, \
        f"unselected instances changed under instance selection ({changed_left}, {changed_right} pixels)"

    selection.clearPrims()
    _settle(view)
    assert view.grabFrameBuffer() == before, "clearing the instance selection did not restore the render"

    # A whole-prim selection elsewhere must not restore stock usdview's
    # over-broad selection for the individually selected main instance.
    selection.setPrimPath(expected_path, instance=1)
    selection.addPrimPath(Sdf.Path("/World/NestedOuter"))
    _settle(view)
    mixed = view.grabFrameBuffer()
    assert _count_differences(before, mixed, 256, 210, 25) > 0
    for x in (156, 356):
        assert _count_differences(before, mixed, x, 144, 42) == 0, \
            "mixed whole-prim/instance selection highlighted an unselected instance"

    # Source indices 0 and 2 share a prototype. Test programmatic multi-selection
    # too: forwarding only the last pick cannot satisfy this case.
    selection.setPrimPath(expected_path, instance=0)
    selection.addPrimPath(expected_path, instance=2)
    _settle(view)
    multiple = view.grabFrameBuffer()
    assert _count_differences(before, multiple, 156, 144, 42) > 0
    assert _count_differences(before, multiple, 356, 144, 42) > 0
    assert _count_differences(before, multiple, 256, 210, 25) == 0, \
        "multi-selection highlighted an unselected source instance"
    multiple.save(str(output / "instances-selected-0-2.png"))

    # Preserve an existing selection through renderer destruction/recreation,
    # without generating a new selection-model signal afterward.
    assert view.SetRendererPlugin("HdStormRendererPlugin")
    assert view.SetRendererPlugin("HdPrismRendererPlugin")
    view.SetRendererSetting("prism:samplesPerPixel", 16)
    _settle(view)
    assert view.grabFrameBuffer() == multiple, \
        "renderer recreation lost the existing source-instance selection"

    selection.setPrimPath(expected_path)  # ALL_INSTANCES sentinel
    _settle(view)
    all_instances = view.grabFrameBuffer()
    for x, y, radius in ((156, 144, 42), (256, 210, 25), (356, 144, 42)):
        assert _count_differences(before, all_instances, x, y, radius) > 0, \
            "whole-prim selection did not highlight every instance"
    selection.clearPrims()
    _settle(view)
    assert view.grabFrameBuffer() == before
    print("PrismRT usdview instance picking/selection passed; screenshots:", output.resolve())
