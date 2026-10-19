"""PrismRT host extension for unmodified OpenUSD's usdview.

Two roles, both built on usdview's plugin API and public StageView methods; the
extension neither patches USD nor replaces any usdview implementation.

Selection: OpenUSD 26.05's scene-index selection path loses AddSelected's
instance index. This extension passes the selection model's original source
indices to PrismRT.

Sample HUD: while a viewport uses PrismRT, a transparent, mouse-transparent
overlay in the HUD's top-right group (under usdview's Hydra/Hgi/AOV lines)
shows the completed and target sample counts and whether the image is still
refining or has converged (including adaptive early convergence). usdview stops
repainting once the renderer converges, so the overlay runs on its own timer.
It follows the HUD toggle and hides while the renderer is paused or stopped.
"""

import time

from pxr import Gf, Tf, Vt
from pxr.Usdviewq.plugin import PluginContainer
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
from pxr.Usdviewq.stageView import StageView


SETTING = "prism:usdviewSelection"
RENDERER = "HdPrismRendererPlugin"


def selection_records(selection):
    """Wire format: authored prim path, tab, sorted source instance indices."""
    records = []
    for path, indices in sorted(selection.items(), key=lambda item: str(item[0])):
        if str(path) == "/":
            continue  # usdview uses the pseudo-root for an empty selection.
        values = [indices] if isinstance(indices, int) else sorted(indices)
        records.append(str(path) + "\t" + ",".join(str(i) for i in values))
    return Vt.StringArray(records)


class _SelectionBridge(QtCore.QObject):
    def __init__(self, api):
        super().__init__(api.qMainWindow)
        self._api = api
        self._views = set()
        api.dataModel.selection.signalPrimSelectionChanged.connect(self.sync)
        # Plugins load before the viewport is constructed. This also discovers
        # a replacement viewport without retaining a renderer or patching it.
        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self.sync)
        self._timer.start(100)
        self.sync()

    def sync(self, *unused):
        views = set(self._api.qMainWindow.findChildren(StageView))
        for view in views - self._views:
            view.installEventFilter(self)
        self._views = views
        for view in views:
            self._sync_view(view)

    def _sync_view(self, view):
        if view.GetCurrentRendererId() != RENDERER:
            return
        value = selection_records(
            self._api.dataModel.selection.getPrimPathInstances())
        # Read back from the current renderer, so switching away and back (or
        # recreating it with the same ID) cannot leave stale selection state.
        if view.GetRendererSetting(SETTING) != value:
            view.SetRendererSetting(SETTING, value)

    def eventFilter(self, watched, event):
        if event.type() == QtCore.QEvent.Paint:
            self._sync_view(watched)
        return False


def hud_text(completed, target, converged):
    """HUD content for the sample overlay: (samples text or None, state).

    completed/target are None when the renderer statistics are unavailable, in
    which case only the convergence state is reported. state is "converged" or
    "refining"; a converged image below the target is adaptive early-out.
    """
    state = "converged" if converged else "refining"
    if completed is None:
        return None, state
    completed = max(0, int(completed))
    if target is not None and int(target) > 0:
        return "%d / %d" % (completed, int(target)), state
    return str(completed), state


# usdview's TopRight HUD group: x = width - 160, y = 14, line spacing 15, and
# keys right-justified to int(1.4 * len(longest key)); its keys are always five
# characters wide ("Hydra", "  Hgi", "  AOV"), so the colon sits after 7 columns.
_HUD_RIGHT_INSET = 160
_HUD_TOP = 14
_HUD_LINE = 15
_HUD_COLOR = (.733, .604, .333)
_CONVERGED_COLOR = (.45, .78, .42)
_STATS_INTERVAL = 0.1  # seconds; GetRenderStats is called at most 10x/s


def _enum(owner, group, name):
    """Qt enum member under both scoped (Qt6) and legacy (Qt5) spellings."""
    scope = getattr(owner, group, None)
    return getattr(scope, name, None) or getattr(owner, name)


class _SampleLabel(QtWidgets.QWidget):
    """HUD-styled lines: coloured text over a 1-px dark shadow."""

    def __init__(self, view):
        super().__init__(view)
        for attribute in ("WA_TransparentForMouseEvents",
                          "WA_TranslucentBackground"):
            self.setAttribute(
                _enum(QtCore.Qt, "WidgetAttribute", attribute), True)
        self.setAutoFillBackground(False)
        self._lines = []  # [(text, (r, g, b))]
        self.hide()

    def lines(self):
        return list(self._lines)

    def set_lines(self, lines):
        self._lines = list(lines)
        self.update()

    @staticmethod
    def _font(view):
        # usdview keeps its HUD font on the private HUD helper (older versions
        # on the view); there is no public accessor, so fall back to monospace.
        hud = getattr(view, "_hud", None)
        base = getattr(view, "_HUDFont", None) or getattr(hud, "_HUDFont", None)
        if base is not None:
            font = QtGui.QFont(base)
            # usdview paints at device pixels with a point size scaled by the
            # pixel ratio; this widget paints in logical pixels.
            ratio = QtWidgets.QApplication.instance().devicePixelRatio() or 1.0
            font.setPointSizeF(max(1.0, base.pointSizeF() / ratio))
            return font
        font = QtGui.QFontDatabase.systemFont(
            _enum(QtGui.QFontDatabase, "SystemFont", "FixedFont"))
        font.setPointSizeF(9.0)
        return font

    def place(self, view, row):
        """Size and position under usdview's `row` top-right HUD lines."""
        font = self._font(view)
        self.setFont(font)
        metrics = QtGui.QFontMetrics(font)
        width = max([metrics.horizontalAdvance(text) for text, _ in self._lines]
                    + [0]) + 2
        height = _HUD_LINE * len(self._lines) + 2
        x = min(view.width() - _HUD_RIGHT_INSET, view.width() - width - 2)
        y = _HUD_TOP + _HUD_LINE * row + 1
        self.setGeometry(max(0, x), y, width, height)

    def paintEvent(self, event):
        painter = QtGui.QPainter(self)
        painter.setFont(self.font())
        # Same shadow tone as usdview: display 0.2 converted to linear.
        shadow = Gf.ConvertDisplayToLinear(Gf.Vec3f(.2, .2, .2))
        for i, (text, rgb) in enumerate(self._lines):
            baseline = 10 + _HUD_LINE * i
            painter.setPen(QtGui.QColor.fromRgbF(*shadow))
            painter.drawText(1, baseline + 1, text)
            painter.setPen(QtGui.QColor.fromRgbF(*rgb))
            painter.drawText(0, baseline, text)
        painter.end()


class _SampleHud(QtCore.QObject):
    """Sample-count and convergence overlay for PrismRT viewports."""

    def __init__(self, api):
        super().__init__(api.qMainWindow)
        self._api = api
        self._labels = {}  # StageView -> _SampleLabel
        self._stats_cache = {}  # StageView -> (monotonic time, stats dict)
        self._timer = QtCore.QTimer(self)
        self._timer.timeout.connect(self.refresh)
        self._timer.start(100)

    def refresh(self, *unused):
        try:
            self._refresh()
        except Exception as error:  # Never raise out of a Qt timer.
            Tf.Warn("prismUsdview sample HUD: %s" % error)

    def _refresh(self):
        views = set(self._api.qMainWindow.findChildren(StageView))
        for view in views - set(self._labels):
            self._labels[view] = _SampleLabel(view)
            view.installEventFilter(self)
        for view in list(self._labels):
            if view not in views:
                self._labels.pop(view).deleteLater()
                self._stats_cache.pop(view, None)
        for view, label in self._labels.items():
            self._refresh_view(view, label)

    def _refresh_view(self, view, label):
        if not self._wanted(view):
            label.hide()
            return
        completed, target, converged = self._read(view)
        if converged is None:
            label.hide()
            return
        samples, state = hud_text(completed, target, converged)
        lines = []
        if samples is not None:
            lines.append(("Samples".rjust(7) + ": " + samples, _HUD_COLOR))
        lines.append(("State".rjust(7) + ": " + state,
                      _CONVERGED_COLOR if converged else _HUD_COLOR))
        if lines != label.lines():
            label.set_lines(lines)
        self._place(view, label)
        label.show()
        label.raise_()

    def _wanted(self, view):
        if view.GetCurrentRendererId() != RENDERER or not view.isVisible():
            return False
        if not self._api.dataModel.viewSettings.showHUD:
            return False
        # usdview exposes pause/stop only through its private flags (they also
        # select "(paused)" for the Hydra line); no public getter exists.
        return not (getattr(view, "_renderPauseState", False) or
                    getattr(view, "_renderStopState", False))

    @staticmethod
    def _place(view, label):
        # Rows used by usdview's own Hydra / Hgi / AOV lines (public properties).
        rows = 1
        if getattr(view, "rendererHgiDisplayName", ""):
            rows += 1
        if getattr(view, "rendererAovName", "color") != "color":
            rows += 1
        label.place(view, rows)

    def _read(self, view):
        """(completed, target, converged); converged is None if unknown."""
        converged = view.IsRendererConverged()
        now = time.monotonic()
        cached = self._stats_cache.get(view)
        if cached and now - cached[0] < _STATS_INTERVAL * 0.9:
            stats = cached[1]
        else:
            stats = self._stats(view)
            self._stats_cache[view] = (now, stats)
        completed = stats.get("numCompletedSamples",
                              stats.get("completedSamples"))
        target = stats.get("targetSamples")
        if converged is None:
            converged = stats.get("converged")
        return completed, target, None if converged is None else bool(converged)

    @staticmethod
    def _stats(view):
        # StageView has no public render-stats accessor; its engine is the
        # private `_renderer`. Read it defensively and degrade to the
        # convergence marker alone if anything is missing.
        try:
            return dict(view._renderer.GetRenderStats())
        except Exception:
            return {}

    def eventFilter(self, watched, event):
        label = self._labels.get(watched)
        if label is not None and event.type() == QtCore.QEvent.Resize \
                and label.isVisible():
            self._place(watched, label)
        return False


class PrismContainer(PluginContainer):
    def registerPlugins(self, plugRegistry, usdviewApi):
        self._bridge = _SelectionBridge(usdviewApi)
        self._hud = _SampleHud(usdviewApi)

    def configureView(self, plugRegistry, plugUIBuilder):
        pass  # This extension supplies selection data and needs no menu entry.


Tf.Type.Define(PrismContainer)
