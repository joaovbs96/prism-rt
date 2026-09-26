"""Focused tests for the stock-usdview Prism instance-selection shim."""

import unittest
from pathlib import Path
from unittest import mock

from pxr import Plug, Sdf, Tf, Vt
from pxr.Usdviewq.qt import QtWidgets

import prismUsdview


class _Selection:
    def __init__(self, paths):
        self.paths = paths
        self.signalPrimSelectionChanged = _Signal()

    def getPrimPathInstances(self):
        return self.paths


class _Signal:
    def connect(self, callback):
        self.callback = callback


class _Api:
    def __init__(self, parent, paths):
        self.qMainWindow = parent
        self.dataModel = type("DataModel", (), {})()
        self.dataModel.selection = _Selection(paths)


class _FakeStageView(QtWidgets.QWidget):
    def __init__(self, parent, renderer_id):
        super().__init__(parent)
        self.renderer_id = renderer_id
        # Native GetRendererSetting returns None until the first setting write.
        self.settings = {}
        self.writes = []

    def GetCurrentRendererId(self):
        return self.renderer_id

    def GetRendererSetting(self, key):
        return self.settings.get(key)

    def SetRendererSetting(self, key, value):
        self.writes.append((key, value))
        self.settings[key] = value


class UsdviewSelectionPluginTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    def test_selection_wire_skips_pseudoroot_and_preserves_source_indices(self):
        records = prismUsdview.selection_records({
            Sdf.Path.absoluteRootPath: -1,
            Sdf.Path("/World/Instancer"): {7, 2},
            Sdf.Path("/World/AllInstances"): -1,
        })
        self.assertEqual(records, Vt.StringArray([
            "/World/AllInstances\t-1",
            "/World/Instancer\t2,7",
        ]))

    def test_empty_selection_becomes_empty_array(self):
        self.assertEqual(
            prismUsdview.selection_records({Sdf.Path.absoluteRootPath: -1}),
            Vt.StringArray(),
        )

    def test_container_supports_usdview_ui_configuration(self):
        # PluginContainer's default implementation raises NotImplementedError.
        prismUsdview.PrismContainer().configureView(None, None)

    def test_plug_registry_discovers_and_loads_container(self):
        plugin_dir = Path(__file__).resolve().parents[2] / "tools" / "usdview_plugins" / "prismUsdview"
        registry = Plug.Registry()
        registry.RegisterPlugins(str(plugin_dir))

        container_type = Tf.Type.FindByName("prismUsdview.PrismContainer")
        self.assertTrue(container_type)
        self.assertEqual(
            registry.GetPluginForType(container_type).name,
            "prismUsdview",
        )
        registry.GetPluginForType(container_type).Load()
        derived = Tf.Type.FindByName("pxr.Usdviewq.plugin.PluginContainer")
        self.assertIn(container_type, registry.GetAllDerivedTypes(derived))

    def test_sync_only_writes_prism_and_replays_after_renderer_recreation(self):
        window = QtWidgets.QWidget()
        instancer_path = Sdf.Path("/World/Instancer")
        api = _Api(window, {instancer_path: {2, 0}})
        prism_view = _FakeStageView(window, prismUsdview.RENDERER)
        other_view = _FakeStageView(window, "HdStormRendererPlugin")

        with mock.patch.object(prismUsdview, "StageView", _FakeStageView):
            bridge = prismUsdview._SelectionBridge(api)
            bridge._timer.stop()
            bridge.sync()
            expected = Vt.StringArray(["/World/Instancer\t0,2"])
            self.assertEqual(prism_view.GetRendererSetting(prismUsdview.SETTING), expected)
            self.assertEqual(len(prism_view.writes), 1)
            self.assertEqual(other_view.writes, [])

            # Simulate replacing the backend while keeping the StageView and
            # renderer plugin ID. The newly created renderer has default state.
            prism_view.settings[prismUsdview.SETTING] = Vt.StringArray()
            bridge.sync()
            self.assertEqual(prism_view.GetRendererSetting(prismUsdview.SETTING), expected)
            self.assertEqual(len(prism_view.writes), 2)
            self.assertEqual(other_view.writes, [])

            bridge.deleteLater()
            window.close()

    def test_first_empty_sync_writes_authoritative_empty_array(self):
        window = QtWidgets.QWidget()
        api = _Api(window, {Sdf.Path.absoluteRootPath: -1})
        prism_view = _FakeStageView(window, prismUsdview.RENDERER)

        with mock.patch.object(prismUsdview, "StageView", _FakeStageView):
            bridge = prismUsdview._SelectionBridge(api)
            bridge._timer.stop()
            bridge.sync()
            self.assertEqual(prism_view.GetRendererSetting(prismUsdview.SETTING), Vt.StringArray())
            self.assertEqual(prism_view.writes, [(prismUsdview.SETTING, Vt.StringArray())])
            bridge.sync()
            self.assertEqual(len(prism_view.writes), 1)
            bridge.deleteLater()
            window.close()


if __name__ == "__main__":
    unittest.main()
