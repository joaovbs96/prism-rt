"""Preserve usdview's instance selection when using unmodified OpenUSD.

OpenUSD 26.05's scene-index selection path loses AddSelected's instance index.
This host extension passes the selection model's original source indices to
PrismRT. It uses usdview's plugin API and StageView renderer-setting methods;
it neither patches USD nor replaces any usdview implementation.
"""

from pxr import Tf, Vt
from pxr.Usdviewq.plugin import PluginContainer
from pxr.Usdviewq.qt import QtCore
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


class PrismContainer(PluginContainer):
    def registerPlugins(self, plugRegistry, usdviewApi):
        self._bridge = _SelectionBridge(usdviewApi)

    def configureView(self, plugRegistry, plugUIBuilder):
        pass  # This extension supplies selection data and needs no menu entry.


Tf.Type.Define(PrismContainer)
