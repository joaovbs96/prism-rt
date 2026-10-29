"""4x4 transforms and cameras as NumPy arrays.

PrismRT follows USD's GfMatrix4d layout: row vectors (p' = p @ M), the
translation in row 3, local-to-world. Compose with ``a @ b`` (a first, then
b). Every function accepts tuples, lists or arrays for vectors.
"""

from __future__ import annotations

import math

import numpy as np

from . import _prism

__all__ = ["identity", "translate", "scale", "rotate", "look_at", "matrix_at", "Camera"]


def _vec3(value, what: str) -> np.ndarray:
    v = np.asarray(value, dtype=np.float64).reshape(-1)
    if v.size != 3:
        raise ValueError(f"{what}: expected 3 numbers, got {v.size}")
    return v


def identity() -> np.ndarray:
    """The 4x4 identity."""
    return np.eye(4)


def translate(x, y=None, z=None) -> np.ndarray:
    """A translation: ``translate(x, y, z)`` or ``translate((x, y, z))``."""
    v = _vec3((x, y, z) if y is not None else x, "translate")
    m = np.eye(4)
    m[3, :3] = v
    return m


def scale(x, y=None, z=None) -> np.ndarray:
    """A scale: uniform ``scale(s)``, ``scale(x, y, z)`` or ``scale((x, y, z))``."""
    if y is None and np.ndim(x) == 0:
        v = np.full(3, float(x))
    else:
        v = _vec3((x, y, z) if y is not None else x, "scale")
    m = np.eye(4)
    m[0, 0], m[1, 1], m[2, 2] = v
    return m


def rotate(axis, degrees: float) -> np.ndarray:
    """A rotation by ``degrees`` (counter-clockwise looking down the axis) about ``axis``."""
    a = _vec3(axis, "rotate")
    length = np.linalg.norm(a)
    if length == 0:
        raise ValueError("rotate: the axis has zero length")
    x, y, z = a / length
    t = math.radians(degrees)
    c, s, k = math.cos(t), math.sin(t), 1 - math.cos(t)
    # Column-vector rotation, transposed into the row-vector layout.
    r = np.array([[c + x * x * k, x * y * k - z * s, x * z * k + y * s],
                  [y * x * k + z * s, c + y * y * k, y * z * k - x * s],
                  [z * x * k - y * s, z * y * k + x * s, c + z * z * k]])
    m = np.eye(4)
    m[:3, :3] = r.T
    return m


def look_at(eye, target, up=(0.0, 1.0, 0.0)) -> np.ndarray:
    """A camera-to-world (or light-to-world) transform at ``eye`` looking at
    ``target``: local -Z points at the target, +Y towards ``up``."""
    e, t, u = _vec3(eye, "eye"), _vec3(target, "target"), _vec3(up, "up")
    forward = t - e
    if np.linalg.norm(forward) == 0:
        raise ValueError("look_at: eye and target coincide")
    forward /= np.linalg.norm(forward)
    side = np.cross(forward, u)
    if np.linalg.norm(side) == 0:
        raise ValueError("look_at: up is parallel to the view direction")
    side /= np.linalg.norm(side)
    true_up = np.cross(side, forward)
    m = np.eye(4)
    m[0, :3], m[1, :3], m[2, :3], m[3, :3] = side, true_up, -forward, e
    return m


def matrix_at(value, time: float | None = None) -> np.ndarray:
    """A 4x4 NumPy matrix from what a description field reads back: a list of
    (time, matrix) samples (``desc.transform``, ``desc.camera_to_world``), or a
    matrix. ``time=None`` needs a single sample; with a time, the sample
    nearest to it is used."""
    if isinstance(value, list) and value and isinstance(value[0], tuple):
        if time is None:
            if len(value) != 1:
                raise ValueError(f"the transform has {len(value)} time samples: pass time=")
            sample = value[0]
        else:
            sample = min(value, key=lambda s: abs(s[0] - time))
        value = sample[1]
    m = np.array(value, dtype=np.float64)
    if m.shape != (4, 4):
        raise ValueError(f"expected a 4x4 matrix, got shape {m.shape}")
    return m


class Camera:
    """CameraDesc constructors (USD camera units: lens values in tenths of the scene unit)."""

    @staticmethod
    def look_at(eye, target, up=(0.0, 1.0, 0.0), *, vertical_fov: float | None = None,
                focal_length: float = 50.0, aspect: float = 1.0, **fields) -> _prism.CameraDesc:
        """A perspective camera at ``eye`` looking at ``target``. With
        ``vertical_fov`` (degrees) the apertures give that field of view over a
        ``aspect`` = width / height aperture; sessions fit it to the image
        (ConformPolicy, ExpandAperture by default keeps it for landscape images).
        Other CameraDesc fields go as keywords."""
        desc = _prism.CameraDesc(camera_to_world=look_at(eye, target, up), focal_length=focal_length, **fields)
        if vertical_fov is not None:
            vertical = 2.0 * focal_length * math.tan(math.radians(vertical_fov) / 2.0)
            desc.vertical_aperture = vertical
            desc.horizontal_aperture = vertical * aspect
        return desc

    @staticmethod
    def from_matrix(camera_to_world, **fields) -> _prism.CameraDesc:
        """A camera with this camera-to-world transform (or (time, matrix) samples)."""
        return _prism.CameraDesc(camera_to_world=camera_to_world, **fields)

    @staticmethod
    def transform(desc: _prism.CameraDesc, time: float | None = None) -> np.ndarray:
        """The camera-to-world matrix of a CameraDesc (``scene.camera(id)``) as
        a 4x4 NumPy array: its only sample, or the sample nearest to ``time``."""
        return matrix_at(desc.camera_to_world, time)
