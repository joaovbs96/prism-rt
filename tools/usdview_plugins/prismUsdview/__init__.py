"""PrismRT host extension for unmodified OpenUSD's usdview.

Two roles, both built on usdview's plugin API and public StageView methods; the
extension neither patches USD nor replaces any usdview implementation.

Selection: OpenUSD's scene-index selection path loses AddSelected's instance
index (UsdImagingGLEngine::AddSelected ignores it; 26.05 and 26.08). This
extension passes the selection model's original source indices to PrismRT.

Sample HUD: while a viewport uses PrismRT, a transparent, mouse-transparent
overlay in the HUD's top-right group (under usdview's Hydra/Hgi/AOV lines)
shows the active render device (a GPU fallback in a warning colour), what the
renderer is doing while it cannot render yet (loading, compiling materials or
the GPU kernel, preparing textures, uploading; from the `prism*` render
statistics), and then the completed and target sample counts and whether the
image is still refining or has converged (including adaptive early
convergence). usdview stops
repainting once the renderer converges, so the overlay runs on its own timer.
It follows the HUD toggle and hides while the renderer is paused or stopped.

Colour correction: PrismRT's colour AOV is linear in its working space (ACEScg
by default), so usdview starts in its openColorIO mode when $OCIO is set (the
launchers set ocio://studio-config-latest), from the config's scene_linear
role (ACEScg). usdview saves the mode in its state file and starts in sRGB
otherwise. Set PRISM_USDVIEW_COLOR_CORRECTION to sRGB, disabled or
openColorIO to choose another start-up mode, or to "saved" to keep usdview's
saved one. The view is the config's default (for the studio config ACES 2.0
SDR on sRGB, like the CLI's 8-bit images) unless PRISM_USDVIEW_OCIO_DISPLAY
or PRISM_USDVIEW_OCIO_VIEW choose another (see startup_ocio_view).

Default dome light: usdview's Lights > "Enable Default Dome Light" starts
enabled, because a stage without lights is otherwise black. usdview saves the
toggle in its state file, so this sets it at each start-up only (the menu entry
still turns it off for the session). PrismRT uses the default dome light only
while the stage has no lights of its own. Set PRISM_USDVIEW_DEFAULT_DOME_LIGHT
to off to start with it disabled, or to "saved" to keep usdview's saved state.
"""

import os
import time

from pxr import Gf, Tf, Vt
from pxr.Usdviewq.plugin import PluginContainer
from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets
from pxr.Usdviewq.stageView import StageView


SETTING = "prism:usdviewSelection"
RENDERER = "HdPrismRendererPlugin"

# usdview's ColorCorrectionModes (HdxColorCorrectionTokens).
_COLOR_MODES = ("disabled", "sRGB", "openColorIO")
COLOR_CORRECTION_ENV = "PRISM_USDVIEW_COLOR_CORRECTION"


def startup_color_correction(saved, environ=os.environ):
    """usdview's colour correction mode at start-up, given its saved mode.

    openColorIO when $OCIO is set, unless PRISM_USDVIEW_COLOR_CORRECTION names
    another mode or "saved". Without $OCIO usdview cannot use openColorIO (it
    disables the menu entry but would keep a saved openColorIO mode, which
    OpenColorIO then runs without a config), so that becomes sRGB.
    """
    choice = environ.get(COLOR_CORRECTION_ENV, "").strip()
    has_ocio = bool(environ.get("OCIO"))
    if choice.lower() == "saved":
        mode = saved
    else:
        mode = next((m for m in _COLOR_MODES if m.lower() == choice.lower()), None)
        if mode is None:
            if choice:
                Tf.Warn("prismUsdview: %s=%s is not one of %s or saved" %
                        (COLOR_CORRECTION_ENV, choice, ", ".join(_COLOR_MODES)))
            mode = "openColorIO" if has_ocio else saved
    if mode == "openColorIO" and not has_ocio:
        mode = "sRGB"
    return mode


DEFAULT_DOME_ENV = "PRISM_USDVIEW_DEFAULT_DOME_LIGHT"


def startup_default_dome_light(saved, environ=os.environ):
    """Whether usdview's default dome light starts enabled, given its saved state.

    On, unless PRISM_USDVIEW_DEFAULT_DOME_LIGHT is off (0, false, no) or
    "saved" (keep usdview's saved state); on (1, true, yes) is the default.
    """
    choice = environ.get(DEFAULT_DOME_ENV, "").strip().lower()
    if choice == "saved":
        return saved
    if choice in ("off", "0", "false", "no"):
        return False
    if choice and choice not in ("on", "1", "true", "yes"):
        Tf.Warn("prismUsdview: %s=%s is not on, off or saved" %
                (DEFAULT_DOME_ENV, choice))
    return True


OCIO_DISPLAY_ENV = "PRISM_USDVIEW_OCIO_DISPLAY"
OCIO_VIEW_ENV = "PRISM_USDVIEW_OCIO_VIEW"
_STUDIO_DISPLAY = "sRGB - Display"
def startup_ocio_view(environ=os.environ):
    """(display, view) for usdview's openColorIO mode, or None for the config's
    defaults.

    PRISM_USDVIEW_OCIO_VIEW chooses the view ("default" or unset: the
    config's defaults), on PRISM_USDVIEW_OCIO_DISPLAY (default: the studio
    config's "sRGB - Display"); usdview takes a display only with a view. The
    studio config's default ACES 2.0 views need PrismRT's OpenUSD patch
    (tools/ci/patches/openusd-*-hdx-ocio-lut-sampler.patch); with an
    unpatched OpenUSD set PRISM_USDVIEW_OCIO_VIEW=Un-tone-mapped.
    """
    display = environ.get(OCIO_DISPLAY_ENV, "").strip()
    view = environ.get(OCIO_VIEW_ENV, "").strip()
    if not view or view.lower() == "default":
        return None
    return (display or _STUDIO_DISPLAY, view)


def _apply_color_correction(api):
    # Plugins load before the stage and the viewport, so the first image already
    # uses this mode (usdview documents this order for global settings).
    settings = api.dataModel.viewSettings
    mode = startup_color_correction(settings.colorCorrectionMode)
    if mode != settings.colorCorrectionMode:
        settings.colorCorrectionMode = mode
    if mode == "openColorIO":
        view = startup_ocio_view()
        if view and not settings.ocioSettings.view:
            settings.setOcioSettings(display=view[0], view=view[1])


def _apply_default_dome_light(api):
    settings = api.dataModel.viewSettings
    enabled = startup_default_dome_light(settings.domeLightEnabled)
    if enabled != settings.domeLightEnabled:
        settings.domeLightEnabled = enabled


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
_BUSY_COLOR = (.45, .72, .95)     # work in progress before the first sample
_WARNING_COLOR = (1.0, .42, .2)    # GPU fallback, errors
_STATS_INTERVAL = 0.1  # seconds; GetRenderStats is called at most 10x/s

_KIND_COLORS = {"normal": _HUD_COLOR, "converged": _CONVERGED_COLOR,
                "busy": _BUSY_COLOR, "warning": _WARNING_COLOR}

# Phase names of the `prismPhase` statistic -> (HUD wording, counter key).
_PHASE_TEXT = {
    "device": ("Creating render device", None),
    "loading": ("Loading scene", "Meshes"),
    "materials": ("Compiling materials", "Materials"),
    "textures": ("Preparing textures", "Textures"),
    "kernel": ("Compiling GPU kernel", None),
    "upload": ("Uploading to GPU", "Upload"),
}
_PHASE_ORDER = ("upload", "kernel", "materials", "textures", "loading", "device")
_COUNTER_LABELS = {"loading": ("Meshes", "meshes"), "materials": ("Materials", "materials"),
                   "textures": ("Textures", "textures"), "upload": ("Upload", "upload")}
_VENDOR_PREFIXES = ("NVIDIA GeForce ", "NVIDIA ", "AMD Radeon ", "AMD ", "Intel(R) ")


def _count(stats, name):
    """(done, total) of a prism<Name>Done/Total statistic pair, or None."""
    done = stats.get("prism%sDone" % name)
    total = stats.get("prism%sTotal" % name)
    if done is None or total is None or int(total) <= 0:
        return None
    return min(int(done), int(total)), int(total)


def device_line(stats):
    """(text, kind) for the active render device, or None if not reported.

    "vulkan: NVIDIA GeForce RTX 4070 Ti" -> "vulkan (RTX 4070 Ti)". A CPU
    device that stands in for a GPU one ("cpu (Vulkan fallback: ...)") is a
    warning.
    """
    device = str(stats.get("prismDevice") or "").strip()
    if not device:
        return None
    kind = "normal"
    if " fallback" in device:
        kind = "warning"
    elif ": " in device:
        name, _, model = device.partition(": ")
        for prefix in _VENDOR_PREFIXES:
            if model.startswith(prefix):
                model = model[len(prefix):]
                break
        device = "%s (%s)" % (name, model)
    return "Device".rjust(7) + ": " + device, kind


def phase_lines(stats):
    """[(text, kind)] describing work in progress, or [] once rendering.

    Reads the `prismPhase*` statistics: a primary line such as "Compiling
    materials 12 / 35" (with an elapsed-seconds counter for long phases) and,
    while several phases overlap, one indented counter line per other active
    phase that still has work left. Statistics without `prismPhase` (older
    renderers) yield no lines.
    """
    phase = stats.get("prismPhase")
    if phase is None or phase == "rendering":
        return []
    if phase == "error":
        message = str(stats.get("prismError") or "error")
        return [("Status".rjust(7) + ": Error: " + message, "warning")]
    detail = str(stats.get("prismPhaseDetail") or "")
    if phase == "idle":
        return [("Status".rjust(7) + ": Preparing...", "busy")]
    wording, counter = _PHASE_TEXT.get(phase, (str(phase), None))
    text = wording
    progress = _count(stats, counter) if counter else None
    if phase == "loading":
        text = wording + (" (meshes %d / %d)" % progress if progress else "...")
    elif phase == "upload":
        text = wording + (" (%s)" % detail if detail else "...")
        if progress:
            text += " %d / %d" % progress
    elif progress:
        text = "%s %d / %d" % (wording, progress[0], progress[1])
    else:
        text = wording + "..."
    seconds = stats.get("prismPhaseSeconds") or 0.0
    if seconds >= (1.0 if phase == "kernel" else 3.0):
        text += " %d s" % int(seconds)
    lines = [("Status".rjust(7) + ": " + text, "busy")]
    if phase == "kernel" and detail:
        lines.append((" " * 9 + detail, "busy"))
    active = [name for name in str(stats.get("prismActivePhases") or "").split(",")
              if name]
    for name in _PHASE_ORDER:
        if name == phase or name not in active or name not in _COUNTER_LABELS:
            continue
        label, key = _COUNTER_LABELS[name]
        other = _count(stats, label)
        if other and other[0] < other[1]:
            lines.append((" " * 9 + "%s %d / %d" % (key, other[0], other[1]), "busy"))
    return lines


def texture_line(stats):
    """(text, kind) for GPU texture streaming, or None when it is off.

    "Texture: 412 / 5355 MiB, 12 pending" while the levels shading asks for
    are being loaded (busy), then the resident memory alone; textures the
    budget keeps below the requested resolution are a warning.
    """
    if not stats.get("prismTextureStreaming"):
        return None
    resident = float(stats.get("prismTextureResidentMiB") or 0.0)
    budget = float(stats.get("prismTextureBudgetMiB") or 0.0)
    text = "Texture".rjust(7) + ": %d / %d MiB" % (round(resident), round(budget))
    kind = "normal"
    pending = int(stats.get("prismTexturesPending") or 0)
    below = int(stats.get("prismTexturesBelowRequested") or 0)
    if pending:
        text += ", %d pending" % pending
        kind = "busy"
    if below:
        text += ", %d below requested" % below
        kind = "warning"
    return text, kind


def hud_lines(stats, completed, target, converged):
    """Every HUD line for one refresh as [(text, kind)].

    The device line comes first; then either the phase lines (nothing is
    rendering yet, or a scene edit is being converted) or the texture
    streaming line, the sample count and state.
    """
    lines = []
    device = device_line(stats)
    if device:
        lines.append(device)
    busy = phase_lines(stats)
    if busy:
        # The CPU renders a preview while the GPU prepares ("prismGpuPreview"):
        # keep the GPU phase and count the preview's samples beside it.
        if stats.get("prismGpuPreview") and completed:
            samples, _ = hud_text(completed, target, converged)
            busy = busy + [("Samples".rjust(7) + ": " + samples, "normal")]
        return lines + busy
    texture = texture_line(stats)
    if texture:
        lines.append(texture)
    samples, state = hud_text(completed, target, converged)
    if samples is not None:
        lines.append(("Samples".rjust(7) + ": " + samples, "normal"))
    lines.append(("State".rjust(7) + ": " + state,
                  "converged" if converged else "normal"))
    return lines


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
        completed, target, converged, stats = self._read(view)
        if converged is None:
            label.hide()
            return
        lines = [(text, _KIND_COLORS[kind]) for text, kind in
                 hud_lines(stats, completed, target, converged)]
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
        """(completed, target, converged, stats); converged is None if unknown."""
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
        return (completed, target, None if converged is None else bool(converged),
                stats)

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
        try:
            _apply_color_correction(usdviewApi)
        except Exception as error:  # Never stop usdview from starting.
            Tf.Warn("prismUsdview colour correction: %s" % error)
        try:
            _apply_default_dome_light(usdviewApi)
        except Exception as error:
            Tf.Warn("prismUsdview default dome light: %s" % error)
        self._bridge = _SelectionBridge(usdviewApi)
        self._hud = _SampleHud(usdviewApi)

    def configureView(self, plugRegistry, plugUIBuilder):
        pass  # This extension needs no menu entry.


Tf.Type.Define(PrismContainer)
