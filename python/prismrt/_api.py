"""The Pythonic layer over prismrt._prism: Renderer, Scene, Session.

Keyword arguments for the C++ ``*Desc`` structs, names for scene objects
(``name=``), a traverse-like parameter mapping (``scene.params``), progress
callbacks, the Python ``logging`` bridge and the ``atexit`` hook that stops
running sessions. No rendering logic of its own: every call maps onto the
C++ API (include/prism/*.h).
"""

from __future__ import annotations

import atexit
import logging
import os
import threading
import weakref
from typing import Any, Callable, Iterable, Mapping

import numpy as np

from . import _dll, _prism
from .errors import InvalidArgumentError, NotFoundError

__all__ = ["Renderer", "Scene", "Session", "render", "devices", "buffer"]

logger = logging.getLogger("prismrt")

_LOG_LEVELS = {
    _prism.LogLevel.Debug: logging.DEBUG,
    _prism.LogLevel.Info: logging.INFO,
    _prism.LogLevel.Warning: logging.WARNING,
    _prism.LogLevel.Error: logging.ERROR,
}


def _log_to_logging(level, category: str, message: str) -> None:
    name = f"prismrt.{category}" if category else "prismrt"
    logging.getLogger(name).log(_LOG_LEVELS.get(level, logging.WARNING), "%s", message)


# ── Arrays ──────────────────────────────────────────────────────────────

_FLOATS = (np.float32, np.float64)
_INDICES = (np.int32, np.uint32)


def buffer(value, dtype=np.float32, components: int = 0, name: str = "array",
           accept: Iterable = ()) -> _prism.BufferView:
    """A BufferView over ``value``. A C-contiguous CPU array of an accepted
    dtype is viewed without a copy (the view keeps it alive); other dtypes
    and layouts are converted once (to ``dtype``). ``components``: scalars per
    element (0: the array's last dimension, or 1)."""
    if isinstance(value, _prism.BufferView):
        return value
    accepted = tuple(np.dtype(d) for d in (tuple(accept) or (dtype,)))
    array = np.asarray(value)
    if array.dtype not in accepted:
        try:
            array = array.astype(dtype)
        except (TypeError, ValueError) as error:
            raise InvalidArgumentError(f"{name}: cannot convert {array.dtype} to {np.dtype(dtype)}") from error
    if not array.flags.c_contiguous:
        array = np.ascontiguousarray(array)
    if components:
        if components > 1 and array.ndim > 1 and array.shape[-1] != components:
            raise InvalidArgumentError(
                f"{name}: expected (N, {components}) {np.dtype(dtype)}, got {array.shape} {array.dtype}")
        if array.size % components:
            raise InvalidArgumentError(
                f"{name}: {array.size} values do not make elements of {components} components")
    return _prism.BufferView(array, components, name)


def _buffer_or_empty(value, *args, **kwargs):
    return _prism.BufferView() if value is None else buffer(value, *args, **kwargs)


def _is_samples(value) -> bool:
    """A list of (time, value) pairs (time samples, docs/sdk_design.md section 5)."""
    return isinstance(value, list) and bool(value) and all(
        isinstance(v, tuple) and len(v) == 2 and np.ndim(v[0]) == 0 and
        (isinstance(v[1], _prism.BufferView) or np.ndim(v[1]) > 0) for v in value)


def _samples(value, convert):
    """A value, or (time, value) pairs, converted per value."""
    if _is_samples(value):
        return [(float(t), convert(v)) for t, v in value]
    return convert(value)


def _matrix(value):
    """A 4x4 matrix (np.eye(4) when None), or (time, matrix) pairs."""
    if value is None:
        return np.eye(4)
    if _is_samples(value):
        return [(float(t), np.asarray(m, dtype=np.float64)) for t, m in value]
    m = np.asarray(value, dtype=np.float64)
    if m.shape != (4, 4):
        raise InvalidArgumentError(f"transform: expected a 4x4 matrix, got shape {m.shape}")
    return m


def _set_fields(desc, fields: Mapping[str, Any], what: str):
    for key, value in fields.items():
        if key not in type(desc)._fields:
            raise TypeError(f"{what}: unexpected keyword argument '{key}'")
        setattr(desc, key, value)
    return desc


# ── Scene objects by name ───────────────────────────────────────────────

_KINDS = {
    _prism.MeshId: "mesh", _prism.SpheresId: "spheres", _prism.InstanceId: "instance",
    _prism.MaterialId: "material", _prism.LightId: "light", _prism.EnvironmentId: "environment",
    _prism.CameraId: "camera",
}


class _Entry:
    __slots__ = ("name", "kind", "handle", "desc", "arrays")

    def __init__(self, name, kind, handle, desc, arrays=None):
        self.name, self.kind, self.handle, self.desc = name, kind, handle, desc
        self.arrays = arrays or {}


class SceneParameters:
    """A traverse-like view of a scene's named objects: ``params["key.intensity"]``
    reads a copy of a description field; setting it applies the matching
    update (update_light, set_transform, ...) as a scene edit. ``update()``
    commits them (``scene.commit()``)."""

    def __init__(self, scene: "Scene"):
        self._scene = scene

    def _split(self, key: str):
        name, _, field = key.rpartition(".")
        entry = self._scene._entries.get(name)
        if entry is None or entry.desc is None or field not in type(entry.desc)._fields:
            raise KeyError(key)
        return entry, field

    def keys(self) -> list[str]:
        return [f"{entry.name}.{field}" for entry in self._scene._entries.values() if entry.desc is not None
                for field in type(entry.desc)._fields]

    def __iter__(self):
        return iter(self.keys())

    def __len__(self) -> int:
        return len(self.keys())

    def __contains__(self, key) -> bool:
        try:
            self._split(key)
            return True
        except KeyError:
            return False

    def items(self):
        return [(key, self[key]) for key in self.keys()]

    def __getitem__(self, key: str):
        entry, field = self._split(key)
        if field in entry.arrays:
            return np.array(entry.arrays[field], copy=True)
        value = getattr(entry.desc, field)
        return value.copy() if isinstance(value, np.ndarray) else value

    def __setitem__(self, key: str, value) -> None:
        entry, field = self._split(key)
        self._scene._update(entry, {field: value})

    def update(self) -> _prism.CommitResult:
        """Commits the edits (``scene.commit()``)."""
        return self._scene.commit()

    def __repr__(self) -> str:
        return f"SceneParameters({len(self)} keys)"


# ── Renderer ────────────────────────────────────────────────────────────

class Renderer:
    """A renderer: colour management, the material system, textures, logging.
    Several renderers per process are allowed; there is no global state.

    ``env``: start from the environment's options (OCIO, PRISM_*) or the
    built-in defaults. ``log``: True sends diagnostics to the Python logger
    ``prismrt`` (default), a callable ``log(level, category, message)``
    receives them, False leaves them on standard error. Other keywords set
    RendererOptions fields: ``working_space``, ``color_config``, ``display``,
    ``view``, ``cache_directory``, ``disable_vulkan``, ``log_level``,
    ``materialx_libraries``, ``sss_fallback``."""

    def __init__(self, options: _prism.RendererOptions | None = None, *, env: bool = True,
                 log: bool | Callable = True, working_space: str | None = None, color_config: str | None = None,
                 display: str | None = None, view: str | None = None, cache_directory=None,
                 disable_vulkan: bool | None = None, log_level=None, materialx_libraries: str | None = None,
                 sss_fallback: bool | None = None):
        if options is None:
            options = _prism.RendererOptions.from_environment() if env else _prism.RendererOptions()
            if _dll.PACKAGED and not options.materials.materialx_libraries:
                # A wheel's own MaterialX libraries (env=False, or the variable set empty).
                options.materials.materialx_libraries = _dll.package_path("materialx/libraries")
                options.materials.materialx_mdl_path = _dll.package_path("materialx/libraries/mdl")
        if working_space is not None:
            options.color.working_space = working_space
        if color_config is not None:
            options.color.config = color_config
        if display is not None:
            options.color.display = display
        if view is not None:
            options.color.view = view
        if cache_directory is not None:
            options.cache_directory = os.fspath(cache_directory)
        if disable_vulkan is not None:
            options.devices.disable_vulkan = disable_vulkan
        if log_level is not None:
            options.log_level = log_level
        if materialx_libraries is not None:
            options.materials.materialx_libraries = materialx_libraries
        if sss_fallback is not None:
            options.materials.sss_fallback = sss_fallback
        sink = _log_to_logging if log is True else (log if callable(log) else None)
        self._native = _prism.Renderer(options, sink)

    @property
    def options(self) -> _prism.RendererOptions:
        return self._native.options

    def color(self) -> _prism.ColorInfo:
        return self._native.color()

    def devices(self) -> list[_prism.DeviceInfo]:
        """Devices this build and machine can create, the CPU last."""
        return self._native.devices()

    def capabilities(self) -> _prism.Capabilities:
        return self._native.capabilities()

    def stats(self) -> _prism.RendererStats:
        return self._native.stats()

    def create_scene(self, options: _prism.SceneOptions | None = None, **fields) -> "Scene":
        """A scene of this renderer; keywords set SceneOptions fields."""
        options = _set_fields(options if options is not None else _prism.SceneOptions(), fields, "create_scene")
        return Scene(self, self._native.create_scene(options))

    def create_session(self, scene: "Scene", options: _prism.SessionOptions | None = None, *,
                       device="auto", allow_fallback: bool = True, width: int | None = None,
                       height: int | None = None, camera=None, aovs=None, settings=None, denoise=None,
                       denoised_color: bool | None = None, data_window=None, pixel_aspect_ratio: float | None = None,
                       conform_policy=None, progressive=None, **render_settings) -> "Session":
        """A session rendering ``scene`` on ``device`` ("auto", "cpu", "vulkan",
        "metal"). ``camera``: a CameraId, a camera's name (USD cameras: their
        prim path) or a CameraDesc. ``settings``: a RenderSettings, and/or its
        fields as keywords (``samples=64, seed=1``). ``denoise``: True or
        DenoiseSettings. Without ``allow_fallback`` a missing device raises
        UnsupportedError."""
        o = options if options is not None else _prism.SessionOptions()
        if options is None or device != "auto":
            o.device = device
        if options is None or not allow_fallback:
            o.allow_fallback = allow_fallback
        if width is not None:
            o.width = width
        if height is not None:
            o.height = height
        if camera is not None:
            if isinstance(camera, _prism.CameraDesc):
                o.camera_desc = camera
            else:
                o.camera = scene._handle(camera, _prism.CameraId)
        if aovs is not None:
            o.aovs = aovs
        if settings is not None or render_settings:
            base = settings if settings is not None else o.settings
            o.settings = _set_fields(_copy(base), render_settings, "create_session")
        if denoise is not None:
            o.denoise = _prism.DenoiseSettings() if denoise is True else (None if denoise is False else denoise)
        if denoised_color is not None:
            o.denoised_color = denoised_color
        if data_window is not None:
            o.data_window = data_window if isinstance(data_window, _prism.Window) else _prism.Window(
                **dict(zip(("x", "y", "width", "height"), data_window)))
        if pixel_aspect_ratio is not None:
            o.pixel_aspect_ratio = pixel_aspect_ratio
        if conform_policy is not None:
            o.conform_policy = conform_policy
        if progressive is not None:
            o.progressive = progressive
        return Session(self, scene, self._native.create_session(scene._native, o))

    def __repr__(self) -> str:
        color = self.color()
        return f"Renderer(working_space={color.working_space!r}, view={color.view!r})"


def _copy(value):
    import copy
    return copy.copy(value)


def devices() -> list[_prism.DeviceInfo]:
    """The devices of a renderer with the environment's options."""
    return Renderer(log=False).devices()


# ── Scene ───────────────────────────────────────────────────────────────

class Scene:
    """A scene: meshes, spheres, instances, materials, lights, environments
    and cameras as handles. Edits are recorded; ``commit()`` publishes them as
    a new revision (sessions render committed revisions). One writer at a
    time. Every ``add_*`` takes ``name=`` (else a name like ``light0``);
    handles and names are interchangeable in the other calls."""

    def __init__(self, renderer: Renderer, native: _prism.Scene):
        self._renderer = renderer
        self._native = native
        self._entries: dict[str, _Entry] = {}
        self._by_handle: dict[tuple, _Entry] = {}
        self._counters: dict[str, int] = {}

    # Names and handles.
    def _register(self, kind, handle, desc, name, arrays=None) -> None:
        if name is None:
            n = self._counters.get(kind, 0)
            while f"{kind}{n}" in self._entries:
                n += 1
            self._counters[kind] = n + 1
            name = f"{kind}{n}"
        elif name in self._entries:
            raise InvalidArgumentError(f"name '{name}' is already used in this scene")
        entry = _Entry(name, kind, handle, desc, arrays)
        self._entries[name] = entry
        self._by_handle[(kind, handle.value)] = entry

    def _check_name(self, name) -> None:
        if name is not None and name in self._entries:
            raise InvalidArgumentError(f"name '{name}' is already used in this scene")

    def _handle(self, value, cls):
        if isinstance(value, cls):
            return value
        if isinstance(value, str):
            entry = self._entries.get(value)
            if entry is None or not isinstance(entry.handle, cls):
                raise NotFoundError(f"no {_KINDS[cls]} named '{value}' in this scene")
            return entry.handle
        raise InvalidArgumentError(f"expected a {cls.__name__} or a name, got {type(value).__name__}")

    def _prototype(self, value):
        if isinstance(value, (_prism.MeshId, _prism.SpheresId)):
            return value
        if isinstance(value, str) and value in self._entries and \
                isinstance(self._entries[value].handle, (_prism.MeshId, _prism.SpheresId)):
            return self._entries[value].handle
        raise NotFoundError(f"no mesh or spheres named '{value}' in this scene")

    def _materials(self, values):
        if values is None:
            return []
        if isinstance(values, (str, _prism.MaterialId)):
            values = [values]
        return [_prism.MaterialId() if v is None else self._handle(v, _prism.MaterialId) for v in values]

    def __getitem__(self, name: str):
        """The handle of a named object."""
        return self._entries[name].handle

    def __contains__(self, name: str) -> bool:
        return name in self._entries

    @property
    def names(self) -> dict[str, Any]:
        """The named objects' handles."""
        return {name: entry.handle for name, entry in self._entries.items()}

    def name_of(self, handle) -> str | None:
        entry = self._by_handle.get((_KINDS.get(type(handle)), getattr(handle, "value", None)))
        return entry.name if entry else None

    @property
    def params(self) -> SceneParameters:
        return SceneParameters(self)

    @property
    def renderer(self) -> Renderer:
        return self._renderer

    # Meshes and spheres.
    @staticmethod
    def _mesh_desc(desc, fields, arrays):
        convert = {
            "points": lambda v: _samples(v, lambda a: buffer(a, np.float32, 3, "points", _FLOATS)),
            "face_vertex_counts": lambda v: _buffer_or_empty(v, np.int32, 1, "face_vertex_counts"),
            "face_vertex_indices": lambda v: _buffer_or_empty(v, np.int32, 1, "face_vertex_indices", _INDICES),
            "normals": lambda v: _buffer_or_empty(v, np.float32, 3, "normals", _FLOATS),
            "uvs": lambda v: _buffer_or_empty(v, np.float32, 2, "uvs", _FLOATS),
            "tangents": lambda v: _buffer_or_empty(v, np.float32, 4, "tangents"),
            "velocities": lambda v: _buffer_or_empty(v, np.float32, 3, "velocities", _FLOATS),
            "accelerations": lambda v: _buffer_or_empty(v, np.float32, 3, "accelerations", _FLOATS),
            "face_material_slots": lambda v: _buffer_or_empty(v, np.int32, 1, "face_material_slots"),
            "single_sided_faces": lambda v: _buffer_or_empty(v, np.uint8, 1, "single_sided_faces"),
        }
        for key, value in fields.items():
            if key not in type(desc)._fields:
                raise TypeError(f"add_mesh: unexpected keyword argument '{key}'")
            if key in convert:
                if value is not None and not isinstance(value, _prism.BufferView) and not _is_samples(value):
                    arrays[key] = value
                value = convert[key](value)
            elif key == "primvars":
                value = [p if isinstance(p, _prism.Primvar) else _primvar(*p) for p in (value or [])]
            setattr(desc, key, value)
        return desc

    def add_mesh(self, points=None, indices=None, counts=None, *, desc: _prism.MeshDesc | None = None,
                 name: str | None = None, uvs_interpolation=None, **fields) -> _prism.MeshId:
        """A mesh from arrays: ``points`` (N, 3) float32/float64 (or (time,
        points) pairs), ``indices`` (flat or (M, 3)) int32 face-vertex indices,
        ``counts`` per-face vertex counts (a polygon mesh; without them a
        triangle list). Other MeshDesc fields as keywords (``normals``,
        ``uvs``, ``uvs_interpolation``, ``subdivision``, ...). C-contiguous
        arrays of the right dtype are not copied here; the scene reads them
        at commit()."""
        if isinstance(points, _prism.MeshDesc):
            desc, points = points, None
        d = desc if desc is not None else _prism.MeshDesc()
        if points is not None:
            fields["points"] = points
        if indices is not None:
            fields["face_vertex_indices"] = indices
        if counts is not None:
            fields["face_vertex_counts"] = counts
        arrays = {}
        self._mesh_desc(d, fields, arrays)
        if uvs_interpolation is None and "uvs" in fields and fields["uvs"] is not None:
            # Per vertex when there is one per point, else per face corner.
            points0 = d.points[0][1] if d.points else None
            uvs_interpolation = "vertex" if points0 is not None and len(d.uvs) == len(points0) else "face_varying"
        if uvs_interpolation is not None:
            d.uvs_interpolation = uvs_interpolation
        self._check_name(name)
        handle = self._native.add_mesh(d)
        self._register("mesh", handle, d, name, arrays)
        return handle

    def add_spheres(self, centers=None, radii=None, *, desc: _prism.SpheresDesc | None = None,
                    name: str | None = None) -> _prism.SpheresId:
        """Analytic spheres: ``centers`` (N, 3) float32 (or (time, centers)
        pairs) and ``radii`` (N,) float32."""
        d = desc if desc is not None else _prism.SpheresDesc()
        arrays = {}
        if centers is not None:
            if not _is_samples(centers):
                arrays["centers"] = centers
            d.centers = _samples(centers, lambda a: buffer(a, np.float32, 3, "centers"))
        if radii is not None:
            arrays["radii"] = radii
            d.radii = buffer(radii, np.float32, 1, "radii")
        self._check_name(name)
        handle = self._native.add_spheres(d)
        self._register("spheres", handle, d, name, arrays)
        return handle

    def add_instance(self, prototype, transform=None, materials=None, *, desc: _prism.InstanceDesc | None = None,
                     name: str | None = None, **fields) -> _prism.InstanceId:
        """A placement of a mesh or spheres (handle or name): ``transform``
        (4x4 local-to-world, or (time, matrix) pairs), ``materials`` (handles
        or names; slot 0 is the default). Other InstanceDesc fields as
        keywords (``visible``, ``prim_id``, ``light_link_categories``, ...)."""
        d = desc if desc is not None else _prism.InstanceDesc()
        d.prototype = self._prototype(prototype)
        if transform is not None or desc is None:
            d.transform = _matrix(transform)
        if materials is not None:
            d.materials = self._materials(materials)
        if "primvars" in fields:
            fields["primvars"] = [p if isinstance(p, _prism.Primvar) else _primvar(*p)
                                  for p in (fields["primvars"] or [])]
        _set_fields(d, fields, "add_instance")
        self._check_name(name)
        handle = self._native.add_instance(d)
        self._register("instance", handle, d, name)
        return handle

    def add_material(self, desc: _prism.MaterialDesc | None = None, *, mtlx_file=None, mtlx_string: str | None = None,
                     mtlx_document=None, diffuse=None, emissive=None, name: str | None = None,
                     **fields) -> _prism.MaterialId:
        """A material: a MaterialX document (``mtlx_file``, ``mtlx_string`` or a
        ``MaterialX.Document`` as ``mtlx_document``; compiled at commit), a
        Lambertian ``diffuse`` colour or an ``emissive`` colour. Other
        MaterialDesc fields as keywords (``material_name``, ``intensity``,
        ``opacity``, ...)."""
        d = desc if desc is not None else _prism.MaterialDesc()
        if mtlx_file is not None:
            d.kind, d.path = "materialx_file", os.fspath(mtlx_file)
        elif mtlx_string is not None:
            d.kind, d.document = "materialx_string", mtlx_string
        elif mtlx_document is not None:
            import MaterialX  # the caller's MaterialX; serialised here
            d.kind, d.document = "materialx_string", MaterialX.writeToXmlString(mtlx_document)
        elif diffuse is not None:
            d.kind, d.color = "diffuse", diffuse
        elif emissive is not None:
            d.kind, d.color = "emissive", emissive
        _set_fields(d, fields, "add_material")
        if name is not None and not d.name:
            d.name = name
        self._check_name(name)
        handle = self._native.add_material(d)
        self._register("material", handle, d, name)
        return handle

    def add_light(self, shape="sphere", *, desc: _prism.LightDesc | None = None, transform=None,
                  name: str | None = None, **fields) -> _prism.LightId:
        """A UsdLux light: ``shape`` ("sphere", "rect", "disk", "cylinder",
        "distant", "point"), ``transform``, and LightDesc fields as keywords
        (``color`` linear Rec.709, ``intensity``, ``exposure``, ``radius``,
        ``width``, ``height``, ``normalize``, ...)."""
        d = desc if desc is not None else _prism.LightDesc()
        if desc is None or shape != "sphere":
            d.shape = shape
        if transform is not None:
            d.transform = _matrix(transform)
        _set_fields(d, fields, "add_light")
        self._check_name(name)
        handle = self._native.add_light(d)
        self._register("light", handle, d, name)
        return handle

    def add_environment(self, kind=None, *, desc: _prism.EnvironmentDesc | None = None, transform=None,
                        name: str | None = None, **fields) -> _prism.EnvironmentId:
        """An environment: ``kind`` "dome" (a lat-long ``texture`` placed by
        ``transform``; the default with a texture), "lat_long" or "constant"
        (the default without a texture: radiance ``color``). Other
        EnvironmentDesc fields as keywords."""
        d = desc if desc is not None else _prism.EnvironmentDesc()
        if kind is None and desc is None:
            kind = "dome" if fields.get("texture") else "constant"
        if kind is not None:
            d.kind = kind
        if transform is not None:
            d.transform = _matrix(transform)
        _set_fields(d, fields, "add_environment")
        self._check_name(name)
        handle = self._native.add_environment(d)
        self._register("environment", handle, d, name)
        return handle

    def add_camera(self, desc: _prism.CameraDesc | None = None, *, name: str | None = None,
                   **fields) -> _prism.CameraId:
        """A camera: a CameraDesc (``prismrt.Camera.look_at(...)``) or its
        fields as keywords. Camera edits take effect without a commit."""
        d = desc if desc is not None else _prism.CameraDesc()
        if "camera_to_world" in fields:
            fields["camera_to_world"] = _matrix(fields["camera_to_world"])
        _set_fields(d, fields, "add_camera")
        self._check_name(name)
        handle = self._native.add_camera(d)
        self._register("camera", handle, d, name)
        return handle

    # Updates.
    def _entry(self, target, kind) -> _Entry:
        if isinstance(target, str):
            entry = self._entries.get(target)
            if entry is None or entry.kind != kind:
                raise NotFoundError(f"no {kind} named '{target}' in this scene")
            return entry
        entry = self._by_handle.get((kind, getattr(target, "value", None)))
        if entry is None or entry.kind != kind:
            raise NotFoundError(f"no {kind} {target!r} added through this scene object")
        return entry

    def _update(self, entry: _Entry, fields: Mapping[str, Any]) -> None:
        native, handle = self._native, entry.handle
        if entry.kind == "instance" and set(fields) <= {"transform", "visible", "materials"}:
            for key, value in fields.items():
                if key == "transform":
                    value = _matrix(value)
                    native.set_transform(handle, value)
                elif key == "visible":
                    value = bool(value)
                    native.set_visible(handle, value)
                else:
                    value = self._materials(value)
                    native.set_materials(handle, value)
                if entry.desc is not None:
                    setattr(entry.desc, key, value)
            return
        if entry.desc is None:
            raise InvalidArgumentError(f"'{entry.name}' was added by a loader: only its transform, visibility "
                                       "and materials (instances) can change through prismrt")
        if entry.kind == "mesh" and set(fields) == {"points"}:
            arrays = {}
            self._mesh_desc(entry.desc, fields, arrays)
            entry.arrays.update(arrays)
            native.update_mesh_points(handle, entry.desc.points)
            return
        desc = _copy(entry.desc)
        if entry.kind == "mesh":
            arrays = {}
            self._mesh_desc(desc, fields, arrays)
            entry.arrays.update(arrays)
        elif entry.kind == "spheres":
            for key, value in fields.items():
                if key == "centers":
                    desc.centers = _samples(value, lambda a: buffer(a, np.float32, 3, "centers"))
                elif key == "radii":
                    desc.radii = buffer(value, np.float32, 1, "radii")
                else:
                    raise TypeError(f"update: unexpected field '{key}'")
                if not _is_samples(value):
                    entry.arrays[key] = value
        else:
            fields = dict(fields)
            for key in ("transform", "camera_to_world"):
                if key in fields:
                    fields[key] = _matrix(fields[key])
            if entry.kind == "instance":
                if "prototype" in fields:
                    fields["prototype"] = self._prototype(fields["prototype"])
                if "materials" in fields:
                    fields["materials"] = self._materials(fields["materials"])
            _set_fields(desc, fields, f"update {entry.kind}")
        getattr(native, f"update_{entry.kind}")(handle, desc)
        entry.desc = desc

    def update_mesh(self, mesh, **fields) -> None:
        """Changes fields of a mesh (``points=...`` alone updates its points)."""
        self._update(self._entry(mesh, "mesh"), fields)

    def update_mesh_points(self, mesh, points) -> None:
        self._update(self._entry(mesh, "mesh"), {"points": points})

    def update_spheres(self, spheres, **fields) -> None:
        self._update(self._entry(spheres, "spheres"), fields)

    def update_instance(self, instance, **fields) -> None:
        self._update(self._entry(instance, "instance"), fields)

    def set_transform(self, instance, transform) -> None:
        self._update(self._entry(instance, "instance"), {"transform": transform})

    def set_visible(self, instance, visible: bool) -> None:
        self._update(self._entry(instance, "instance"), {"visible": visible})

    def set_materials(self, instance, materials) -> None:
        self._update(self._entry(instance, "instance"), {"materials": materials})

    def update_material(self, material, **fields) -> None:
        self._update(self._entry(material, "material"), fields)

    def update_light(self, light, **fields) -> None:
        self._update(self._entry(light, "light"), fields)

    def update_environment(self, environment, **fields) -> None:
        self._update(self._entry(environment, "environment"), fields)

    def update_camera(self, camera, **fields) -> None:
        self._update(self._entry(camera, "camera"), fields)

    def camera(self, camera) -> _prism.CameraDesc:
        return self._native.camera(self._handle(camera, _prism.CameraId))

    def remove(self, target) -> None:
        """Removes an object (handle or name)."""
        if isinstance(target, str):
            entry = self._entries.get(target)
            if entry is None:
                raise NotFoundError(f"no object named '{target}' in this scene")
            handle = entry.handle
        else:
            handle = target
            entry = self._by_handle.get((_KINDS.get(type(handle)), getattr(handle, "value", None)))
        self._native.remove(handle)
        if entry is not None:
            del self._entries[entry.name]
            del self._by_handle[(entry.kind, entry.handle.value)]

    # Publishing.
    def commit(self, progress: Callable | None = None) -> _prism.CommitResult:
        """Publishes the edits as a new revision (blocking; the GIL is
        released). ``progress(info)`` is called with ProgressInfo on this
        thread's worker; False (or Ctrl-C) cancels: CancelledError, and the
        edits stay pending."""
        return self._native.commit(progress)

    @property
    def pending_edits(self) -> int:
        """The DirtyKind bits of the edits since the last commit (0: none)."""
        return self._native.pending_edits

    @property
    def revision(self) -> int:
        return self._native.revision

    def diagnostics(self) -> list[_prism.Diagnostic]:
        """Diagnostics of the last commit (failed materials, ...)."""
        return self._native.diagnostics()

    @property
    def options(self) -> _prism.SceneOptions:
        return self._native.options

    # Dictionaries.
    @classmethod
    def from_dict(cls, renderer: Renderer, description: Mapping, **scene_options) -> "Scene":
        """A scene from a dictionary (prismrt.scene_dict)."""
        from .scene_dict import from_dict
        return from_dict(renderer, description, **scene_options)

    def to_dict(self) -> dict:
        """The named objects as a dictionary from_dict reads back."""
        from .scene_dict import to_dict
        return to_dict(self)

    def __repr__(self) -> str:
        counts: dict[str, int] = {}
        for entry in self._entries.values():
            counts[entry.kind] = counts.get(entry.kind, 0) + 1
        text = ", ".join(f"{kind if kind.endswith('s') else kind + 's'}={n}" for kind, n in counts.items())
        return f"Scene(revision={self.revision}, {text or 'empty'}, pending_edits={self.pending_edits})"


def _primvar(name, data, interpolation="vertex", components=0):
    """A Primvar from (name, array[, interpolation[, components]])."""
    array = np.asarray(data)
    dtype = array.dtype if array.dtype in (np.float32, np.float64, np.int32) else np.float32
    return _prism.Primvar(name=name, interpolation=interpolation,
                          data=buffer(array, dtype, components, f"primvar {name}", (np.float32, np.float64, np.int32)))


# ── Session ─────────────────────────────────────────────────────────────

_sessions: "weakref.WeakSet[Session]" = weakref.WeakSet()
_sessions_lock = threading.Lock()


@atexit.register
def _stop_sessions() -> None:
    """Cancels and joins every running session before the interpreter
    finalizes (docs/sdk_design.md 3.8)."""
    with _sessions_lock:
        sessions = list(_sessions)
    for session in sessions:
        try:
            session._native.cancel()
            session._native.wait()
        except BaseException:  # noqa: BLE001 - shutting down
            pass


class Session:
    """A device, render settings and a camera that render a scene's committed
    revisions: ``render()`` (batch, blocking) or ``start()`` (progressive, on
    the session's thread) with ``wait``, ``status``, ``aov``, ``pause``,
    ``resume``, ``restart`` and ``cancel``."""

    def __init__(self, renderer: Renderer, scene: Scene, native: _prism.Session):
        self._renderer = renderer
        self._scene = scene
        self._native = native
        with _sessions_lock:
            _sessions.add(self)

    @property
    def device(self) -> _prism.DeviceInfo:
        return self._native.device

    @property
    def device_note(self) -> str:
        """Why the device is not the preferred one (empty when it is)."""
        return self._native.device_note

    @property
    def options(self) -> _prism.SessionOptions:
        return self._native.options

    @property
    def settings(self) -> _prism.RenderSettings:
        """A copy of the current render settings (change them with set_settings)."""
        return self._native.options.settings

    @property
    def scene(self) -> Scene:
        return self._scene

    def set_settings(self, settings: _prism.RenderSettings | None = None, **fields) -> None:
        """Replaces the render settings, or changes some fields (``samples=...``)."""
        base = settings if settings is not None else self.settings
        self._native.set_settings(_set_fields(_copy(base), fields, "set_settings"))

    def set_camera(self, camera) -> None:
        """A CameraId, a camera's name or a CameraDesc."""
        self._native.set_camera(camera if isinstance(camera, _prism.CameraDesc)
                                else self._scene._handle(camera, _prism.CameraId))

    def resize(self, width: int, height: int, data_window=None, pixel_aspect_ratio: float = 1.0,
               conform_policy="expand_aperture") -> None:
        if data_window is not None and not isinstance(data_window, _prism.Window):
            data_window = _prism.Window(**dict(zip(("x", "y", "width", "height"), data_window)))
        self._native.resize(width, height, data_window, pixel_aspect_ratio, conform_policy)

    def set_aovs(self, aovs) -> None:
        self._native.set_aovs(aovs)

    def set_denoise(self, denoise=True, denoised_color: bool = False) -> None:
        self._native.set_denoise(_prism.DenoiseSettings() if denoise is True else (denoise or None),
                                 denoised_color)

    def prepare(self) -> None:
        self._native.prepare()

    def _tag(self, value):
        value._renderer = self._renderer._native
        return value

    def render(self, progress: Callable | None = None, *, samples: int | None = None, seed: int | None = None,
               allow_uncommitted: bool = False) -> _prism.RenderResult:
        """Renders the scene's last committed revision to completion (blocking,
        the GIL released). ``progress(info)`` runs on a C++ thread about ten
        times a second; returning False cancels (CancelledError), as does
        Ctrl-C (KeyboardInterrupt). ``samples``/``seed`` override the settings
        for this call. A scene with edits since its last commit raises
        InvalidArgumentError unless ``allow_uncommitted``."""
        if not allow_uncommitted and self._scene.pending_edits:
            raise InvalidArgumentError(
                "the scene has edits since its last commit: call scene.commit() (or scene.params.update()) "
                "first, or pass allow_uncommitted=True to render the last committed revision")
        previous = None
        if samples is not None or seed is not None:
            previous = self.settings
            overridden = _copy(previous)
            if samples is not None:
                overridden.samples = samples
            if seed is not None:
                overridden.seed = seed
            self._native.set_settings(overridden)
        try:
            return self._tag(self._native.render(progress))
        finally:
            if previous is not None:
                self._native.set_settings(previous)

    def cancel(self) -> None:
        """Stops the running render (batch or progressive) at its next sample boundary."""
        self._native.cancel()

    def start(self, progress: Callable | None = None) -> None:
        """Starts progressive rendering on the session's thread and returns.
        ``progress(info)`` runs at pass boundaries; False cancels (an
        exception too, re-raised by wait())."""
        self._native.start(progress)

    def pause(self) -> None:
        self._native.pause()

    def resume(self) -> None:
        self._native.resume()

    def restart(self) -> None:
        self._native.restart()

    def wait(self, timeout: float | None = None) -> _prism.SessionStatus:
        """Waits until the progressive render is not running (or ``timeout``
        seconds passed) and returns its status. Re-raises a failed render's
        error and an exception its progress callback raised; Ctrl-C raises
        KeyboardInterrupt (the render keeps running: cancel() stops it)."""
        status = self._native.wait(-1.0 if timeout is None else float(timeout))
        if status.state == _prism.SessionState.Cancelled:
            self._native._take_callback_error()
        return status

    def status(self) -> _prism.SessionStatus:
        return self._native.status()

    def aov(self, aov="color") -> _prism.Image:
        """A snapshot of the progressive render's last published pass."""
        return self._tag(self._native.aov(aov))

    def pick(self, x: int, y: int) -> _prism.Pick:
        return self._native.pick(x, y)

    def __enter__(self) -> "Session":
        return self

    def __exit__(self, *exc) -> None:
        self._native.cancel()
        self._native.wait()

    def __repr__(self) -> str:
        device = self.device
        return f"Session(device={device.kind.name}:{device.name!r}, state={self.status().state.name})"


def render(scene: Scene, camera=None, *, width: int = 1280, height: int = 720, device="auto",
           aovs=("color",), progress: Callable | None = None, **kwargs) -> _prism.RenderResult:
    """One render of ``scene`` (committed first if it has pending edits) from
    ``camera``: creates a session, renders and returns the RenderResult.
    Keywords are create_session's (``samples=64, seed=1, adaptive_threshold=0``)."""
    if scene.pending_edits or scene.revision == 0:
        scene.commit()
    if camera is None:
        cameras = [entry.handle for entry in scene._entries.values() if entry.kind == "camera"]
        if not cameras:
            raise InvalidArgumentError("render: the scene has no camera (pass camera=...)")
        camera = cameras[0]
    session = scene.renderer.create_session(scene, device=device, width=width, height=height, camera=camera,
                                            aovs=list(aovs), **kwargs)
    return session.render(progress)
