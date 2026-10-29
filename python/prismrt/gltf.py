"""glTF 2.0 files into a prismrt scene (prism_gltf; docs/gltf.md)."""

from __future__ import annotations

import os

from . import _prism

__all__ = ["load"]


def load(scene, path, *, name: str | None = None) -> _prism.GltfLoadResult:
    """Adds the default scene of a .gltf or .glb file: one mesh of all its
    triangles (``name``, default ``gltf<N>``), an instance of it
    (``<name>.instance``) and its materials (compiled at commit). Raises
    NotFoundError or InvalidArgumentError."""
    result = _prism.load_gltf(scene._native, os.fspath(path))
    _register(scene, "gltf", result, name)
    return result


def _register(scene, prefix: str, result, name) -> None:
    if name is None:
        n = 0
        while f"{prefix}{n}" in scene:
            n += 1
        name = f"{prefix}{n}"
    scene._register("mesh", result.mesh, None, name)
    scene._register("instance", result.instance, None, f"{name}.instance")
