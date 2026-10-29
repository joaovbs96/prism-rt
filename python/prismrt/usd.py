"""USD stages into a prismrt scene (prism_usd; docs/python.md, docs/sdk_design.md 6.5).

``load(scene, source)`` takes a file path, or a ``pxr.Usd.Stage`` of the
same USD build that prismrt.usd links (in a build tree: the build's USD,
whose Python bindings ``prism_env.ps1`` puts on PYTHONPATH). Unsaved edits
of a stage render:

- a stage in ``UsdUtils.StageCache`` is looked up there by its id (the
  StageCache path of docs/sdk_design.md 6.5; the caller owns the entry);
- any other stage is read through a second stage over its open layers (root
  and session layer, found with their edits in the layer registry). prismrt
  does not insert it into the cache: USD's Python bindings hand a stage's
  ownership to the cache on insert, so erasing it again would close it.

The USD libraries are shared with ``pxr`` in one process, so a ``pxr`` of
another USD build would crash rather than fail: importing this module checks
that an already-imported ``pxr`` reports the version prismrt.usd was built
with and loaded the same ``usd_usd`` library, and raises ImportError
otherwise. Import ``pxr`` before PySide6 (PySide6 ships an old msvcp140.dll).
"""

from __future__ import annotations

import os
import sys

from . import _dll  # noqa: F401 - DLL directories first
from ._api import Scene

__all__ = ["load", "load_cached", "load_camera", "check_pxr", "LoadResult"]


def _loaded_module(name: str) -> str | None:
    """The file of a loaded module (Windows), or None."""
    if sys.platform != "win32":
        return None
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    kernel32.GetModuleFileNameW.restype = wintypes.DWORD
    kernel32.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    handle = kernel32.GetModuleHandleW(name)
    if not handle:
        return None
    buffer = ctypes.create_unicode_buffer(32768)
    length = kernel32.GetModuleFileNameW(handle, buffer, len(buffer))
    return buffer.value[:length] if length else None


def _same_file(a: str, b: str) -> bool:
    try:
        return os.path.samefile(a, b)
    except OSError:
        return os.path.normcase(os.path.abspath(a)) == os.path.normcase(os.path.abspath(b))


def check_pxr() -> None:
    """Raises ImportError when an imported ``pxr`` is not the USD build
    prismrt.usd links (version, and on Windows the loaded usd_usd library)."""
    if "pxr" not in sys.modules:
        return
    try:
        from . import _build_config
    except ImportError:
        _build_config = None
    from pxr import Usd
    found = tuple(Usd.GetVersion())
    expected = tuple(getattr(_build_config, "USD_VERSION", ())) or (
        tuple(_native.pxr_version()) if "_native" in globals() else ())
    if expected and found != expected:
        raise ImportError(
            f"prismrt.usd was built with USD {'.'.join(map(str, expected))} but the imported pxr is USD "
            f"{'.'.join(map(str, found))} ({os.path.dirname(sys.modules['pxr'].__file__)}): both must be the "
            "same USD build in one process (docs/python.md)")
    library = getattr(_build_config, "USD_LIBRARY", "")
    loaded = _loaded_module(os.path.basename(library)) if library else None
    if library and loaded and not _same_file(loaded, library):
        raise ImportError(
            f"the imported pxr loaded {loaded}, but prismrt.usd links {library}: both must be the same USD "
            "build in one process (docs/python.md)")


check_pxr()  # before prism_usd.dll loads (it would reuse an already-loaded usd_usd.dll)
from . import _prism_usd as _native  # noqa: E402

LoadResult = _native.LoadResult


def _time(time):
    """None (USD's default time), a number, or a pxr Usd.TimeCode."""
    if time is None:
        return None
    if hasattr(time, "IsDefault"):
        return None if time.IsDefault() else float(time.GetValue())
    return float(time)


def _purposes(values) -> list[str]:
    return [str(v) for v in (values or [])]


def load(scene: Scene, source, *, time=None, binding_purposes=(), included_purposes=(), domes: bool = True,
         cameras: bool = True) -> "LoadResult":
    """Adds a USD stage to ``scene``: its gprims (meshes, implicit surfaces,
    spheres; instancing, skinning, GeomSubsets, primvars), UsdShade
    materials (compiled here), UsdLux lights with linking, domes (``domes``)
    and cameras (``cameras``; named by prim path, see ``result.cameras``).
    ``source``: a file path or a pxr ``Usd.Stage`` (through
    UsdUtils.StageCache). ``time``: a time code (default: USD's default)."""
    if isinstance(source, (str, os.PathLike)):
        def call(arguments):
            return _native.load(scene._native, os.fspath(source), *arguments)
    else:
        def call(arguments):
            return _with_stage(source, lambda stage_id: _native.load_cached(scene._native, stage_id, *arguments),
                               lambda root, session: _native.load_layers(scene._native, root, session, *arguments))
    return _load(scene, call, time=time, binding_purposes=binding_purposes, included_purposes=included_purposes,
                 domes=domes, cameras=cameras)


def _load(scene, call, *, time=None, binding_purposes=(), included_purposes=(), domes=True, cameras=True):
    result = call((_time(time), _purposes(binding_purposes), _purposes(included_purposes), domes, cameras))
    for path, camera in result.cameras.items():
        if path not in scene:
            scene._register("camera", camera, scene._native.camera(camera), path)
    return result


def load_camera(source, camera: str, *, time=None):
    """A USD camera (prim path) of a file or stage as a CameraDesc, with its exposure."""
    if isinstance(source, (str, os.PathLike)):
        return _native.load_camera(os.fspath(source), str(camera), _time(time))
    return _with_stage(source, lambda stage_id: _native.load_camera_cached(stage_id, str(camera), _time(time)),
                       lambda root, session: _native.load_camera_layers(root, session, str(camera), _time(time)))


def _with_stage(stage, cached, layers):
    """``cached(stage_id)`` for a stage in UsdUtils.StageCache, else
    ``layers(root_identifier, session_identifier)``."""
    check_pxr()
    from pxr import Usd, UsdUtils
    if not isinstance(stage, Usd.Stage):
        raise TypeError(f"expected a file path or a pxr Usd.Stage, got {type(stage).__name__}")
    stage_id = UsdUtils.StageCache.Get().GetId(stage)
    if stage_id.IsValid():
        return cached(stage_id.ToLongInt())
    session = stage.GetSessionLayer()
    return layers(stage.GetRootLayer().identifier, session.identifier if session else "")


def load_cached(scene: Scene, stage_id, **kwargs) -> "LoadResult":
    """As load(), for the stage of UsdUtils.StageCache with this id
    (``UsdStageCache.Id`` or its ``ToLongInt()``)."""
    stage_id = stage_id.ToLongInt() if hasattr(stage_id, "ToLongInt") else int(stage_id)
    return _load(scene, lambda arguments: _native.load_cached(scene._native, stage_id, *arguments), **kwargs)
