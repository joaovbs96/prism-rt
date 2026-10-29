"""PrismRT: a physically based path tracer (CPU, Vulkan, Metal).

Python bindings of the public C++ API (docs/python.md)::

    import prismrt as prism

    r = prism.Renderer()
    scene = r.create_scene()
    prism.usd.load(scene, "assets/scenes/CornellBox.usda")
    scene.commit()
    session = r.create_session(scene, device="cpu", width=512, height=512,
                               camera="/World/Camera", samples=64, adaptive_threshold=0)
    result = session.render()
    rgb = np.asarray(result.aovs["color"])   # (512, 512, 3) float32, no copy

Submodules: ``prismrt.usd`` (needs USD; imported on first use), ``prismrt.gltf``,
``prismrt.obj``, ``prismrt.io``, ``prismrt.transforms``, ``prismrt.scene_dict``.
"""

from __future__ import annotations

import os as _os
import sys as _sys

from . import _dll  # noqa: F401 - DLL directories before the extension loads
from ._version import __version__  # noqa: F401


def _check_runtime() -> None:
    """Raises ImportError when an older MSVC C++ runtime (msvcp140.dll) than
    the one prismrt was built for is already loaded: PySide6 ships one, and
    prism.dll would crash on it. Import prismrt (and pxr) before PySide6."""
    if _sys.platform != "win32" or _os.environ.get("PRISMRT_SKIP_RUNTIME_CHECK"):
        return
    try:
        from . import _build_config
    except ImportError:
        return
    minimum = getattr(_build_config, "MSVC_RUNTIME_MINIMUM", None)
    if not minimum:
        return
    import ctypes
    from ctypes import wintypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE
    kernel32.GetModuleHandleW.argtypes = [wintypes.LPCWSTR]
    handle = kernel32.GetModuleHandleW("msvcp140.dll")
    if not handle:
        return
    buffer = ctypes.create_unicode_buffer(32768)
    kernel32.GetModuleFileNameW.argtypes = [wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    kernel32.GetModuleFileNameW(handle, buffer, len(buffer))
    path = buffer.value
    version = ctypes.windll.version
    size = version.GetFileVersionInfoSizeW(path, None)
    if not size:
        return
    data = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(path, 0, size, data):
        return
    pointer, length = ctypes.c_void_p(), wintypes.UINT()
    if not version.VerQueryValueW(data, "\\", ctypes.byref(pointer), ctypes.byref(length)):
        return

    class _FixedFileInfo(ctypes.Structure):
        _fields_ = [("signature", wintypes.DWORD), ("struct_version", wintypes.DWORD),
                    ("file_version_ms", wintypes.DWORD), ("file_version_ls", wintypes.DWORD)]

    info = _FixedFileInfo.from_address(pointer.value)
    found = (info.file_version_ms >> 16, info.file_version_ms & 0xFFFF)
    if found < tuple(minimum):
        raise ImportError(
            f"an older MSVC C++ runtime is already loaded ({path}, {found[0]}.{found[1]}; prismrt needs "
            f"{minimum[0]}.{minimum[1]} or newer). Import prismrt (and pxr) before PySide6 or other packages "
            "that ship their own msvcp140.dll; PRISMRT_SKIP_RUNTIME_CHECK=1 skips this check.")


_check_runtime()

from . import _prism  # noqa: E402
from .errors import (ERRORS as _ERRORS, CancelledError, DeviceLostError, Error, InternalError,  # noqa: E402
                     InvalidArgumentError, InvalidHandleError, NotFoundError, OutOfMemoryError, UnsupportedError)

_prism._set_error_classes(dict(_ERRORS))

from ._prism import (  # noqa: E402
    Aov, BufferView, CameraDesc, CameraId, Capabilities, ColorInfo, ColorOptions, CommitResult, CompileMode,
    ConformPolicy, DataType, DenoiseQuality, DenoiseSettings, DeviceInfo, DeviceKind, DeviceOptions,
    DevicePreference, Diagnostic, DiagnosticLevel, DirtyKind, EnvironmentDesc, EnvironmentId, EnvironmentKind,
    Image, InstanceDesc, InstanceId, Interpolation, LightDesc, LightId, LightShape, LightShaping, Limits,
    LogLevel, MaterialDesc, MaterialId, MaterialKind, MaterialOptions, MeshDesc, MeshId, NeeSettings, Pick,
    Primvar, ProgressInfo, ProgressiveOptions, Projection, RendererOptions, RendererStats, RenderResult,
    RenderSettings, SceneOptions, SessionOptions, SessionState, SessionStatus, Shutter, SpheresDesc, SpheresId,
    SubdivisionDesc, SubdivisionScheme, TextureFiltering, TextureOptions, Window, aov_from_name, aov_name,
    default_cache_directory)
from ._api import Renderer, Scene, SceneParameters, Session, buffer, devices, render  # noqa: E402
from .transforms import Camera, identity, look_at, rotate, scale, translate  # noqa: E402
from . import gltf, io, obj, scene_dict, transforms  # noqa: E402,F401


def __getattr__(name: str):
    # prismrt.usd loads USD (prism_usd.dll) on first use.
    if name == "usd":
        import importlib
        return importlib.import_module(".usd", __name__)
    raise AttributeError(f"module 'prismrt' has no attribute {name!r}")
