"""prismrt.usd: files, pxr stages through UsdUtils.StageCache (unsaved edits
render), cameras, and the check that refuses a mismatched pxr. Skipped when
the build's USD Python bindings (pxr) are not importable."""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap

import numpy as np
import pytest

# pxr first: the DLL load order of docs/python.md.
pxr = pytest.importorskip("pxr.Usd", reason="the build's USD Python bindings (pxr) are not importable")
from pxr import Gf, Sdf, Usd, UsdGeom, UsdUtils  # noqa: E402

import prismrt as prism  # noqa: E402

usd = pytest.importorskip("prismrt.usd")


def render(renderer, scene, camera, **kwargs):
    scene.commit()
    session = renderer.create_session(scene, device="cpu", allow_fallback=False, width=48, height=48,
                                      camera=camera, samples=4, adaptive_threshold=0.0, **kwargs)
    return np.asarray(session.render().aovs["color"]).copy()


def test_same_usd_build_in_process():
    assert tuple(Usd.GetVersion()) == tuple(usd._native.pxr_version())
    if sys.platform == "win32":
        loaded = usd._native.loaded_modules("usd_usd.dll")
        assert len(loaded) == 1, loaded
    usd.check_pxr()


def test_load_file_and_cameras(renderer, source_dir):
    scene = renderer.create_scene()
    result = usd.load(scene, source_dir / "assets/scenes/CornellBox.usda")
    assert result.renderable_placements == 8 and result.materials >= 4
    assert list(result.cameras) == ["/World/Camera"] and "/World/Camera" in scene
    desc = usd.load_camera(source_dir / "assets/scenes/CornellBox.usda", "/World/Camera")
    assert desc.focal_length == pytest.approx(50)
    assert np.allclose(scene.camera("/World/Camera").camera_to_world[0][1], desc.camera_to_world[0][1])
    with pytest.raises(prism.NotFoundError):
        usd.load(scene, source_dir / "assets/scenes/missing.usda")
    with pytest.raises(prism.NotFoundError):
        usd.load_camera(source_dir / "assets/scenes/CornellBox.usda", "/World/Nothing")


MOVED_CAMERA = Gf.Matrix4d((-1, 0, 0, 0), (0, 1, 0, 0), (0, 0, -1, 0), (278, 400, -800, 1))


def _move_camera(stage):
    """An unsaved edit: the camera moves up; the file is unchanged."""
    camera = UsdGeom.Xformable(stage.GetPrimAtPath(Sdf.Path("/World/Camera")))
    camera.GetOrderedXformOps()[0].Set(MOVED_CAMERA)


def test_stage_cache_path(renderer, source_dir, monkeypatch):
    """A stage in UsdUtils.StageCache is loaded by its id (docs/sdk_design.md 6.5)."""
    path = str(source_dir / "assets/scenes/CornellBox.usda")
    from_file = renderer.create_scene()
    usd.load(from_file, path)
    reference = render(renderer, from_file, "/World/Camera")

    stage = Usd.Stage.Open(path)
    _move_camera(stage)
    cache = UsdUtils.StageCache.Get()
    stage_id = cache.Insert(stage)  # the cache owns the stage now (USD's Python bindings)
    try:
        def no_layers(*args, **kwargs):
            raise AssertionError("a cached stage must load through the StageCache")
        monkeypatch.setattr(usd._native, "load_layers", no_layers)
        cached = renderer.create_scene()
        assert "/World/Camera" in usd.load(cached, stage).cameras
        image = render(renderer, cached, "/World/Camera")
        assert not np.array_equal(image, reference)
        by_id = renderer.create_scene()
        usd.load_cached(by_id, stage_id)
        assert np.array_equal(render(renderer, by_id, "/World/Camera"), image)
        assert usd.load_camera(stage, "/World/Camera").camera_to_world[0][1][3][1] == pytest.approx(400)
        assert cache.Contains(stage_id) and stage  # prismrt leaves the caller's entry alone
    finally:
        cache.Erase(stage_id)
    with pytest.raises(prism.NotFoundError):
        usd.load_cached(renderer.create_scene(), stage_id)


def test_uncached_stage_renders_unsaved_edits(renderer, source_dir):
    """Any other stage: a second stage over its open layers; nothing is cached."""
    path = str(source_dir / "assets/scenes/CornellBox.usda")
    stage = Usd.Stage.Open(path)
    cache = UsdUtils.StageCache.Get()
    same = renderer.create_scene()
    usd.load(same, stage)
    unchanged = render(renderer, same, "/World/Camera")
    _move_camera(stage)
    edited = renderer.create_scene()
    usd.load(edited, stage)
    image = render(renderer, edited, "/World/Camera")
    assert not np.array_equal(image, unchanged)
    assert stage and not cache.Contains(stage)  # the caller's stage stays open and uncached
    assert stage.GetPrimAtPath(Sdf.Path("/World/Camera"))
    with pytest.raises(TypeError):
        usd.load(renderer.create_scene(), object())


def test_mismatched_pxr_is_refused(monkeypatch):
    from prismrt import _build_config
    monkeypatch.setattr(_build_config, "USD_VERSION", (0, 0, 1))
    with pytest.raises(ImportError, match="same USD build"):
        usd.check_pxr()
    monkeypatch.undo()
    monkeypatch.setattr(_build_config, "USD_LIBRARY", os.path.join(os.path.dirname(__file__), "usd_usd.dll"))
    if sys.platform == "win32":
        with pytest.raises(ImportError, match="same USD build"):
            usd.check_pxr()


def test_import_order_check_in_fresh_process():
    """pxr then prismrt.usd in a fresh process works; the module refuses a
    pxr whose version differs before prism_usd.dll loads."""
    env = dict(os.environ)
    env["PRISMRT_TEST_PACKAGE"] = os.path.dirname(os.path.dirname(prism.__file__))
    script = textwrap.dedent("""
        import os, sys
        sys.path.insert(0, os.environ["PRISMRT_TEST_PACKAGE"])
        if os.environ.get("PRISMRT_TEST_PXR"):
            sys.path.append(os.environ["PRISMRT_TEST_PXR"])
        from pxr import Usd
        import prismrt
        from prismrt import _build_config
        _build_config.USD_VERSION = (0, 0, 1)
        try:
            import prismrt.usd
        except ImportError as error:
            print("refused:", error)
        else:
            print("loaded")
        print("prism_usd loaded:", "prismrt._prism_usd" in sys.modules)
    """)
    completed = subprocess.run([sys.executable, "-c", script], capture_output=True, text=True, timeout=120,
                               env=env)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert "refused:" in completed.stdout and "prism_usd loaded: False" in completed.stdout
