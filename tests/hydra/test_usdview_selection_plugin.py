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


class _Engine:
    def __init__(self, stats):
        self.stats = stats
        self.calls = 0

    def GetRenderStats(self):
        self.calls += 1
        if isinstance(self.stats, Exception):
            raise self.stats
        return self.stats


class _HudView(_FakeStageView):
    def __init__(self, parent, renderer_id, stats, converged):
        super().__init__(parent, renderer_id)
        self._renderer = _Engine(stats)
        self.converged = converged
        self.resize(400, 300)

    def IsRendererConverged(self):
        return self.converged


class UsdviewSampleHudTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    def test_hud_text_refining_converged_and_adaptive(self):
        self.assertEqual(prismUsdview.hud_text(512, 2048, False),
                         ("512 / 2048", "refining"))
        self.assertEqual(prismUsdview.hud_text(2048, 2048, True),
                         ("2048 / 2048", "converged"))
        # Adaptive early-out: converged below the target.
        self.assertEqual(prismUsdview.hud_text(300, 2048, True),
                         ("300 / 2048", "converged"))

    def test_hud_text_degrades_without_stats(self):
        self.assertEqual(prismUsdview.hud_text(None, None, True),
                         (None, "converged"))
        self.assertEqual(prismUsdview.hud_text(None, 64, False),
                         (None, "refining"))
        self.assertEqual(prismUsdview.hud_text(7, None, False),
                         ("7", "refining"))
        self.assertEqual(prismUsdview.hud_text(7, 0, False), ("7", "refining"))

    def test_device_line_names_gpu_and_flags_fallback(self):
        self.assertEqual(prismUsdview.device_line({}), None)
        self.assertEqual(
            prismUsdview.device_line({"prismDevice": "vulkan: NVIDIA GeForce RTX 4070 Ti"}),
            (" Device: vulkan (RTX 4070 Ti)", "normal"))
        self.assertEqual(prismUsdview.device_line({"prismDevice": "cpu"}),
                         (" Device: cpu", "normal"))
        self.assertEqual(
            prismUsdview.device_line({
                "prismDevice": "cpu (Vulkan fallback: kernel too large)"}),
            (" Device: cpu (Vulkan fallback: kernel too large)", "warning"))

    def test_cpu_preview_keeps_gpu_phase_and_counts_samples(self):
        preview = {"prismPhase": "kernel", "prismPhaseSeconds": 12.4,
                   "prismDevice": "cpu (preview while the GPU prepares)",
                   "prismGpuPreview": True}
        self.assertEqual(
            prismUsdview.device_line(preview),
            (" Device: cpu (preview while the GPU prepares)", "normal"))
        self.assertEqual(
            prismUsdview.hud_lines(preview, 17, 256, False),
            [(" Device: cpu (preview while the GPU prepares)", "normal"),
             (" Status: Compiling GPU kernel... 12 s", "busy"),
             ("Samples: 17 / 256", "normal")])
        # No samples yet, or an ordinary busy renderer: the phase lines alone.
        self.assertEqual(
            [t for t, _ in prismUsdview.hud_lines(preview, 0, 256, False)],
            [" Device: cpu (preview while the GPU prepares)",
             " Status: Compiling GPU kernel... 12 s"])
        plain = dict(preview, prismGpuPreview=False)
        self.assertEqual(len(prismUsdview.hud_lines(plain, 17, 256, False)), 2)
        # After the handover the GPU is named and the status line is gone.
        done = {"prismPhase": "rendering", "prismGpuPreview": False,
                "prismDevice": "vulkan: NVIDIA GeForce RTX 4070 Ti"}
        self.assertEqual(
            prismUsdview.hud_lines(done, 40, 256, False),
            [(" Device: vulkan (RTX 4070 Ti)", "normal"),
             ("Samples: 40 / 256", "normal"), ("  State: refining", "normal")])

    def test_phase_lines_for_each_phase(self):
        def lines(**stats):
            return prismUsdview.phase_lines(stats)

        self.assertEqual(lines(), [])  # older renderer: no phase statistics
        self.assertEqual(lines(prismPhase="rendering"), [])
        self.assertEqual(lines(prismPhase="idle"),
                         [(" Status: Preparing...", "busy")])
        self.assertEqual(
            lines(prismPhase="loading", prismMeshesDone=3, prismMeshesTotal=10),
            [(" Status: Loading scene (meshes 3 / 10)", "busy")])
        self.assertEqual(lines(prismPhase="loading"),
                         [(" Status: Loading scene...", "busy")])
        self.assertEqual(
            lines(prismPhase="materials", prismMaterialsDone=12,
                  prismMaterialsTotal=35),
            [(" Status: Compiling materials 12 / 35", "busy")])
        self.assertEqual(
            lines(prismPhase="materials", prismMaterialsDone=12,
                  prismMaterialsTotal=35, prismPhaseSeconds=7.9),
            [(" Status: Compiling materials 12 / 35 7 s", "busy")])
        self.assertEqual(
            lines(prismPhase="textures", prismTexturesDone=1, prismTexturesTotal=4),
            [(" Status: Preparing textures 1 / 4", "busy")])
        # Cold kernel compiles are long: elapsed time from the first second.
        self.assertEqual(
            lines(prismPhase="kernel", prismPhaseSeconds=42.2,
                  prismPhaseDetail="DXC to SPIR-V, 9 MDL programs, 310 KB HLSL"),
            [(" Status: Compiling GPU kernel... 42 s", "busy"),
             (" " * 9 + "DXC to SPIR-V, 9 MDL programs, 310 KB HLSL", "busy")])
        self.assertEqual(
            lines(prismPhase="upload", prismPhaseDetail="images",
                  prismUploadDone=5, prismUploadTotal=20),
            [(" Status: Uploading to GPU (images) 5 / 20", "busy")])
        self.assertEqual(
            lines(prismPhase="upload", prismPhaseDetail="acceleration structures",
                  prismUploadDone=0, prismUploadTotal=0),
            [(" Status: Uploading to GPU (acceleration structures)", "busy")])
        self.assertEqual(
            lines(prismPhase="error", prismError="scene conversion failed: x"),
            [(" Status: Error: scene conversion failed: x", "warning")])

    def test_overlapping_phases_show_secondary_counters(self):
        stats = {"prismPhase": "materials", "prismMaterialsDone": 2,
                 "prismMaterialsTotal": 35,
                 "prismActivePhases": "loading,materials,textures",
                 "prismTexturesDone": 4, "prismTexturesTotal": 20,
                 "prismMeshesDone": 5, "prismMeshesTotal": 5}
        self.assertEqual(prismUsdview.phase_lines(stats), [
            (" Status: Compiling materials 2 / 35", "busy"),
            (" " * 9 + "textures 4 / 20", "busy")])  # finished meshes omitted

    def test_hud_lines_switch_from_phase_to_samples(self):
        busy = {"prismPhase": "kernel", "prismDevice": "vulkan: AMD Radeon RX 7900",
                "numCompletedSamples": 0, "targetSamples": 256}
        self.assertEqual(
            [t for t, _ in prismUsdview.hud_lines(busy, 0, 256, False)],
            [" Device: vulkan (RX 7900)", " Status: Compiling GPU kernel..."])
        done = dict(busy, prismPhase="rendering",
                    prismDevice="cpu (Vulkan fallback: kernel too large)")
        lines = prismUsdview.hud_lines(done, 300, 2048, True)
        self.assertEqual(
            lines,
            [(" Device: cpu (Vulkan fallback: kernel too large)", "warning"),
             ("Samples: 300 / 2048", "normal"), ("  State: converged", "converged")])
        # Without phase statistics the original two lines remain.
        self.assertEqual(prismUsdview.hud_lines({}, 7, 64, False),
                         [("Samples: 7 / 64", "normal"), ("  State: refining", "normal")])

    def test_texture_streaming_line(self):
        self.assertIsNone(prismUsdview.texture_line({}))
        self.assertIsNone(prismUsdview.texture_line({"prismTextureStreaming": False}))
        stats = {"prismPhase": "rendering", "prismTextureStreaming": True,
                 "prismTextureResidentMiB": 411.6, "prismTextureBudgetMiB": 5355.2,
                 "prismTexturesPending": 12, "prismTexturesBelowRequested": 0}
        self.assertEqual(prismUsdview.texture_line(stats),
                         ("Texture: 412 / 5355 MiB, 12 pending", "busy"))
        settled = dict(stats, prismTexturesPending=0)
        self.assertEqual(prismUsdview.texture_line(settled), ("Texture: 412 / 5355 MiB", "normal"))
        limited = dict(settled, prismTexturesBelowRequested=7)
        self.assertEqual(prismUsdview.texture_line(limited),
                         ("Texture: 412 / 5355 MiB, 7 below requested", "warning"))
        self.assertEqual(prismUsdview.hud_lines(settled, 9, 64, False),
                         [("Texture: 412 / 5355 MiB", "normal"), ("Samples: 9 / 64", "normal"),
                          ("  State: refining", "normal")])

    def test_overlay_shows_phase_and_device_colors(self):
        window, hud, view = self._hud({
            "prismPhase": "materials", "prismMaterialsDone": 12,
            "prismMaterialsTotal": 35,
            "prismDevice": "cpu (Vulkan fallback: kernel too large)",
            "numCompletedSamples": 0, "targetSamples": 256})
        label = hud._labels[view]
        self.assertEqual(label.lines(), [
            (" Device: cpu (Vulkan fallback: kernel too large)",
             prismUsdview._WARNING_COLOR),
            (" Status: Compiling materials 12 / 35", prismUsdview._BUSY_COLOR)])
        view._renderer.stats = {
            "prismPhase": "rendering", "prismDevice": "vulkan: NVIDIA GeForce RTX 4070 Ti",
            "numCompletedSamples": 8, "targetSamples": 256}
        hud._stats_cache.clear()
        hud.refresh()
        self.assertEqual([text for text, _ in label.lines()], [
            " Device: vulkan (RTX 4070 Ti)", "Samples: 8 / 256", "  State: refining"])
        window.close()

    def _hud(self, stats, converged=False, renderer=None, show=True):
        window = QtWidgets.QWidget()
        window.resize(400, 300)
        api = _Api(window, {})
        api.dataModel.viewSettings = type("ViewSettings", (), {"showHUD": show})()
        view = _HudView(window, renderer or prismUsdview.RENDERER, stats, converged)
        window.show()
        patcher = mock.patch.object(prismUsdview, "StageView", _HudView)
        patcher.start()
        self.addCleanup(patcher.stop)
        hud = prismUsdview._SampleHud(api)
        hud._timer.stop()
        hud.refresh()
        return window, hud, view

    def test_overlay_shows_progress_and_converged_state(self):
        window, hud, view = self._hud({
            "numCompletedSamples": 512, "targetSamples": 2048})
        label = hud._labels[view]
        self.assertTrue(label.isVisible())
        self.assertEqual([text for text, _ in label.lines()],
                         ["Samples: 512 / 2048", "  State: refining"])
        # Below usdview's Hydra line, aligned with its top-right group.
        self.assertEqual(label.geometry().x(), view.width() - 160)
        self.assertGreater(label.geometry().y(), 14 + 15)
        self.assertTrue(label.testAttribute(
            prismUsdview._enum(prismUsdview.QtCore.Qt, "WidgetAttribute",
                               "WA_TransparentForMouseEvents")))

        view.converged = True
        view._renderer.stats = {"numCompletedSamples": 300, "targetSamples": 2048}
        hud._stats_cache.clear()
        hud.refresh()
        self.assertEqual(label.lines()[1][0], "  State: converged")
        self.assertEqual(label.lines()[0][0], "Samples: 300 / 2048")
        self.assertEqual(label.lines()[1][1], prismUsdview._CONVERGED_COLOR)
        window.close()

    def test_overlay_hides_and_survives_missing_stats(self):
        window, hud, view = self._hud({})
        self.assertEqual([t for t, _ in hud._labels[view].lines()],
                         ["  State: refining"])
        window.close()
        window, hud, view = self._hud(RuntimeError("no stats"), converged=True)
        self.assertEqual([t for t, _ in hud._labels[view].lines()],
                         ["  State: converged"])
        window.close()

        for kwargs in ({"show": False}, {"renderer": "HdStormRendererPlugin"}):
            window, hud, view = self._hud({"completedSamples": 1}, **kwargs)
            self.assertFalse(hud._labels[view].isVisible(), kwargs)
            window.close()

        window, hud, view = self._hud({"completedSamples": 1})
        view._renderPauseState = True
        hud.refresh()
        self.assertFalse(hud._labels[view].isVisible())
        window.close()

    def test_stats_are_read_at_most_ten_times_per_second(self):
        window, hud, view = self._hud({"numCompletedSamples": 1})
        before = view._renderer.calls
        for _ in range(5):
            hud.refresh()
        self.assertEqual(view._renderer.calls, before)
        window.close()

    def test_resize_repositions_overlay(self):
        window, hud, view = self._hud({"numCompletedSamples": 1})
        label = hud._labels[view]
        view.resize(640, 300)
        hud.eventFilter(view, QtCore_event("Resize"))
        self.assertEqual(label.geometry().x(), 640 - 160)
        window.close()


def QtCore_event(name):
    return prismUsdview.QtCore.QEvent(
        getattr(prismUsdview.QtCore.QEvent.Type, name))


if __name__ == "__main__":
    unittest.main()
