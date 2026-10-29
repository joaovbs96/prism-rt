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

The wheel carries its own USD build (not namespaced yet; docs/python.md):
pass file paths, and do not import another ``pxr`` in the same process.
"""

from __future__ import annotations

import os
import sys

from . import _dll  # DLL directories first
from ._api import Scene

__all__ = ["load", "load_cached", "load_camera", "default_dome_texture", "check_pxr", "LoadResult",
           "load_material_preview", "material_preview_scene", "preview_material", "MaterialPreview"]


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
    library = _dll.package_path(library) if library else ""
    loaded = _loaded_module(os.path.basename(library)) if library else None
    if library and loaded and not _same_file(loaded, library):
        raise ImportError(
            f"the imported pxr loaded {loaded}, but prismrt.usd links {library}: both must be the same USD "
            "build in one process (docs/python.md)")


def _add_usd_directories() -> None:
    """A wheel's private USD (its DLLs and plug-in resources) is found only
    once prismrt.usd is imported, so ``import prismrt`` loads no USD."""
    try:
        from . import _build_config
    except ImportError:
        return
    _dll.add_directories(getattr(_build_config, "USD_DLL_DIRECTORIES", ()))


check_pxr()  # before prism_usd.dll loads (it would reuse an already-loaded usd_usd.dll)
_add_usd_directories()
from . import _prism_usd as _native  # noqa: E402

LoadResult = _native.LoadResult
MaterialPreview = _native.MaterialPreview
default_dome_texture = _native.default_dome_texture
material_preview_scene = _native.material_preview_scene


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


def _material_desc(material, material_name, search_path):
    """A MaterialDesc of a .mtlx path, MaterialX XML text or a ``MaterialX.Document``."""
    from . import _prism
    desc = _prism.MaterialDesc()
    if isinstance(material, _prism.MaterialDesc):
        desc = material
    elif hasattr(material, "getMaterialNodes"):
        import MaterialX  # the caller's MaterialX; serialised here (as Scene.add_material)
        desc.kind, desc.document = "materialx_string", MaterialX.writeToXmlString(material)
        # Textures resolve against the document's own file, when it has one.
        source = material.getSourceUri() if hasattr(material, "getSourceUri") else ""
        if search_path is None and source and os.path.isfile(source):
            search_path = os.path.dirname(os.path.abspath(source))
    elif isinstance(material, str) and material.lstrip().startswith("<"):
        desc.kind, desc.document = "materialx_string", material
    elif isinstance(material, (str, os.PathLike)):
        desc.kind, desc.path = "materialx_file", os.fspath(material)
    else:
        raise TypeError("expected a .mtlx path, MaterialX XML text, a MaterialX.Document or a MaterialDesc, got "
                        f"{type(material).__name__}")
    if material_name is not None:
        desc.material_name = str(material_name)
    if search_path is not None:
        desc.search_path = os.fspath(search_path)
    return desc


def load_material_preview(scene: Scene, material, *, material_name: str | None = None, search_path=None,
                          scene_file=None, time=None, name: str | None = None) -> "MaterialPreview":
    """Adds the built-in material preview to ``scene``: the USD-WG
    StandardShaderBall (subdivision surfaces) in the asset's lit box, with
    ``material`` bound to the ball's surface, and the asset's camera
    (``result.camera``; named ``preview_camera``). ``material``: a .mtlx
    path, MaterialX XML text or a ``MaterialX.Document``; ``material_name``
    picks one of its materials (default: the first; see
    ``result.document_materials``). The material is named ``name`` (default:
    ``preview_material``). ``scene_file``: another preview stage with the
    same prims (default: the shipped one). Raises NotFoundError for a
    missing file or an unknown ``material_name``, InvalidArgumentError for a
    document without materials. The caller commits and renders."""
    desc = _material_desc(material, material_name, search_path)
    material_label = name if name is not None else "preview_material"
    scene._check_name(material_label)
    scene._check_name("preview_camera")
    preview = _native.load_material_preview(scene._native, desc, os.fspath(scene_file) if scene_file else "",
                                            _time(time))
    scene._register("material", preview.material, desc, material_label)
    scene._register("camera", preview.camera, preview.camera_desc, "preview_camera")
    return preview


def preview_material(material, *, material_name: str | None = None, width: int = 512, height: int = 512,
                     samples: int | None = None, device="auto", renderer=None, search_path=None,
                     aovs=("color",), progress=None, **kwargs):
    """Renders ``material`` (a .mtlx path, MaterialX XML text or a
    ``MaterialX.Document``) on the built-in shaderball and returns the
    RenderResult: ``PrismRT --material`` in Python, with the CLI's defaults
    (sample clamp 10). ``renderer``: a Renderer to reuse (default: a new one
    from the environment). Other keywords are create_session's
    (``seed=1, adaptive_threshold=0``, ...)."""
    from ._api import Renderer
    renderer = renderer if renderer is not None else Renderer()
    scene = renderer.create_scene()
    preview = load_material_preview(scene, material, material_name=material_name, search_path=search_path)
    scene.commit()
    kwargs.setdefault("sample_clamp", 10.0)  # the CLI's --max-sample-radiance default
    if samples is not None:
        kwargs["samples"] = samples
    session = renderer.create_session(scene, device=device, width=width, height=height, camera=preview.camera,
                                      aovs=list(aovs), **kwargs)
    return session.render(progress)


def load_cached(scene: Scene, stage_id, **kwargs) -> "LoadResult":
    """As load(), for the stage of UsdUtils.StageCache with this id
    (``UsdStageCache.Id`` or its ``ToLongInt()``)."""
    stage_id = stage_id.ToLongInt() if hasattr(stage_id, "ToLongInt") else int(stage_id)
    return _load(scene, lambda arguments: _native.load_cached(scene._native, stage_id, *arguments), **kwargs)
