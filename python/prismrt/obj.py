"""Wavefront OBJ files into a prismrt scene (prism_obj; docs/obj.md)."""

from __future__ import annotations

import os

from . import _prism
from .gltf import _register

__all__ = ["load"]


def load(scene, path, *, name: str | None = None) -> _prism.ObjLoadResult:
    """Adds an OBJ file: one mesh of its triangles (``name``, default
    ``obj<N>``), an instance of it (``<name>.instance``) and its MTL
    materials (compiled at commit; faces their MTL disp displaces are
    displaced at commit). Raises NotFoundError or InvalidArgumentError."""
    result = _prism.load_obj(scene._native, os.fspath(path))
    _register(scene, "obj", result, name)
    return result
