"""Scenes as dictionaries (after Mitsuba's ``load_dict``), in pure Python over
the ``Scene.add_*`` calls.

A description has sections, each a dict of name -> spec (the name is the
object's id) or a list of specs (``name`` optional inside)::

    {
        "options": {"default_dome": False},              # SceneOptions fields
        "materials": {"red": {"diffuse": (0.8, 0.1, 0.1)},
                      "gold": {"mtlx_file": "gold.mtlx"}},
        "meshes": {"floor": {"points": P, "indices": I, "uvs": UV}},
        "spheres": {"ball": {"centers": C, "radii": R}},
        "instances": {"floor": {"prototype": "floor", "materials": ["red"]},
                      "ball": {"prototype": "ball", "transform": translate(0, 0.5, 0),
                               "materials": [{"ref": "gold"}]}},
        "lights": {"key": {"shape": "rect", "width": 2, "intensity": 10}},
        "environments": {"sky": {"color": (0.05, 0.05, 0.05)}},
        "cameras": {"main": {"look_at": {"eye": (0, 2, 5), "target": (0, 0, 0),
                                         "vertical_fov": 40}}},
    }

References are names (or ``{"ref": name}``); sections are created in the
order above, so an instance may name a mesh or material written after it.
Errors carry the key path: ``scene.instances['ball'].materials[0]: no
material named 'gld' in this scene``.
"""

from __future__ import annotations

from typing import Any, Mapping

import numpy as np

from . import _prism
from ._api import Renderer, Scene
from .errors import InvalidArgumentError
from .transforms import Camera

__all__ = ["from_dict", "to_dict"]

SECTIONS = ("materials", "meshes", "spheres", "instances", "lights", "environments", "cameras")
_KIND_SECTIONS = {"material": "materials", "mesh": "meshes", "spheres": "spheres", "instance": "instances",
                  "light": "lights", "environment": "environments", "camera": "cameras"}


def _rethrow(path: str, error: Exception):
    """The same exception type with the key path in front of its message."""
    message = f"{path}: {error}"
    try:
        new = type(error)(message)
    except Exception:  # noqa: BLE001 - an exception type with another constructor
        new = InvalidArgumentError(message)
    raise new from error


def _ref(value, path: str):
    if isinstance(value, Mapping):
        if set(value) != {"ref"}:
            raise InvalidArgumentError(f"{path}: a reference is a name or {{'ref': name}}")
        return value["ref"]
    return value


def _items(description: Mapping, section: str):
    value = description.get(section)
    if value is None:
        return []
    if isinstance(value, Mapping):
        return [(f"scene.{section}[{name!r}]", name, spec) for name, spec in value.items()]
    if isinstance(value, (list, tuple)):
        out = []
        for index, spec in enumerate(value):
            spec = dict(spec)
            out.append((f"scene.{section}[{index}]", spec.pop("name", None), spec))
        return out
    raise InvalidArgumentError(f"scene.{section}: expected a dict of name -> spec or a list of specs")


def from_dict(renderer: Renderer, description: Mapping, **scene_options) -> Scene:
    """A new scene of ``renderer`` with the objects of ``description`` (not committed)."""
    if not isinstance(description, Mapping):
        raise InvalidArgumentError("scene: expected a dict")
    unknown = set(description) - set(SECTIONS) - {"options"}
    if unknown:
        raise InvalidArgumentError(f"scene: unknown section(s) {sorted(unknown)} (expected {', '.join(SECTIONS)}, "
                                   "options)")
    options = dict(description.get("options") or {})
    options.update(scene_options)
    try:
        scene = renderer.create_scene(**options)
    except Exception as error:  # noqa: BLE001
        _rethrow("scene.options", error)

    for path, name, spec in _items(description, "materials"):
        try:
            scene.add_material(name=name, **dict(spec))
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    for path, name, spec in _items(description, "meshes"):
        try:
            scene.add_mesh(name=name, **dict(spec))
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    for path, name, spec in _items(description, "spheres"):
        try:
            scene.add_spheres(name=name, **dict(spec))
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    for path, name, spec in _items(description, "instances"):
        spec = dict(spec)
        if "prototype" not in spec:
            raise InvalidArgumentError(f"{path}.prototype: missing")
        try:
            prototype = _ref(spec.pop("prototype"), f"{path}.prototype")
            scene._prototype(prototype)
        except Exception as error:  # noqa: BLE001
            _rethrow(f"{path}.prototype", error)
        materials = spec.pop("materials", None)
        if materials is not None:
            if isinstance(materials, (str, Mapping)):
                materials = [materials]
            resolved = []
            for index, material in enumerate(materials):
                key = f"{path}.materials[{index}]"
                try:
                    material = _ref(material, key)
                    resolved.append(None if material is None else scene._handle(material, _prism.MaterialId))
                except Exception as error:  # noqa: BLE001
                    _rethrow(key, error)
            materials = resolved
        try:
            scene.add_instance(prototype, materials=materials, name=name, **spec)
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    for path, name, spec in _items(description, "lights"):
        try:
            scene.add_light(name=name, **dict(spec))
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    for path, name, spec in _items(description, "environments"):
        try:
            scene.add_environment(name=name, **dict(spec))
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    for path, name, spec in _items(description, "cameras"):
        spec = dict(spec)
        try:
            if "look_at" in spec:
                look = dict(spec.pop("look_at"))
                desc = Camera.look_at(look.pop("eye"), look.pop("target"), look.pop("up", (0.0, 1.0, 0.0)), **look)
                scene.add_camera(desc, name=name, **spec)
            else:
                scene.add_camera(name=name, **spec)
        except KeyError as error:
            raise InvalidArgumentError(f"{path}.look_at: missing {error}") from error
        except Exception as error:  # noqa: BLE001
            _rethrow(path, error)
    return scene


def _default(desc_type) -> Any:
    return desc_type()


def _value(value):
    if isinstance(value, _prism.BufferView):
        return value if value.count else None
    return value


def _equal(a, b) -> bool:
    try:
        if isinstance(a, np.ndarray) or isinstance(b, np.ndarray):
            return np.array_equal(np.asarray(a, dtype=object), np.asarray(b, dtype=object))
        if isinstance(a, _prism.BufferView) and isinstance(b, _prism.BufferView):
            return a.count == 0 and b.count == 0
        if isinstance(a, list) and isinstance(b, list):
            return len(a) == len(b) and all(_equal(x, y) for x, y in zip(a, b))
        if isinstance(a, tuple) and isinstance(b, tuple):
            return len(a) == len(b) and all(_equal(x, y) for x, y in zip(a, b))
        return bool(a == b)
    except Exception:  # noqa: BLE001
        return False


def to_dict(scene: Scene) -> dict:
    """The scene's objects that were added through the Python API, as a
    description ``from_dict`` reads back (arrays as given, references as
    names, fields at their defaults left out)."""
    out: dict[str, dict] = {}
    options = scene.options
    defaults = _prism.SceneOptions()
    changed = {f: getattr(options, f) for f in type(options)._fields if not _equal(getattr(options, f),
                                                                                   getattr(defaults, f))}
    if changed:
        out["options"] = changed
    for entry in scene._entries.values():
        if entry.desc is None:
            continue
        section = out.setdefault(_KIND_SECTIONS[entry.kind], {})
        default = _default(type(entry.desc))
        spec: dict[str, Any] = {}
        for field in type(entry.desc)._fields:
            if field in entry.arrays:
                spec[field] = entry.arrays[field]
                continue
            value = getattr(entry.desc, field)
            if _equal(value, getattr(default, field)):
                continue
            if field in ("transform", "camera_to_world", "points", "centers"):
                value = value[0][1] if len(value) == 1 and value[0][0] == 0.0 else value
            if entry.kind == "instance" and field == "prototype":
                value = scene.name_of(value) or value
            elif entry.kind == "instance" and field == "materials":
                value = [scene.name_of(m) or m for m in value]
            value = _value(value)
            if value is not None:
                spec[field] = value
        if entry.kind == "instance" and "prototype" not in spec:
            spec["prototype"] = scene.name_of(entry.desc.prototype)
        if entry.kind == "material":
            spec.pop("name", None) if spec.get("name") == entry.name else None
            spec = _material_spec(spec)
        section[entry.name] = spec
    return out


def _material_spec(spec: dict) -> dict:
    """MaterialDesc fields as add_material keywords."""
    kind = spec.pop("kind", _prism.MaterialKind.Diffuse)
    color = spec.pop("color", None)
    if kind == _prism.MaterialKind.MaterialXFile:
        spec["mtlx_file"] = spec.pop("path")
    elif kind == _prism.MaterialKind.MaterialXString:
        spec["mtlx_string"] = spec.pop("document")
    elif kind == _prism.MaterialKind.Emissive:
        spec["emissive"] = color if color is not None else _prism.MaterialDesc().color
    else:
        spec["diffuse"] = color if color is not None else _prism.MaterialDesc().color
    return spec
