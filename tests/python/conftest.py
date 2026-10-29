"""pytest setup of the prismrt tests (CTest: prism_python_*; docs/python.md).

CTest passes the build tree's package (PRISMRT_TEST_PACKAGE), the USD
Python bindings (PRISMRT_TEST_PXR), the CLI and the image comparison tool,
the source tree and an output directory. Tests render on the CPU unless
their name says otherwise.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

_package = os.environ.get("PRISMRT_TEST_PACKAGE")
if _package and _package not in sys.path:
    sys.path.insert(0, _package)
_pxr = os.environ.get("PRISMRT_TEST_PXR")
if _pxr and os.path.isdir(_pxr) and _pxr not in sys.path:
    sys.path.append(_pxr)

import numpy as np  # noqa: E402

import prismrt as prism  # noqa: E402

SOURCE = Path(os.environ.get("PRISMRT_TEST_SOURCE", Path(__file__).resolve().parents[2]))


@pytest.fixture(scope="session")
def source_dir() -> Path:
    return SOURCE


@pytest.fixture(scope="session")
def output_dir() -> Path:
    path = Path(os.environ.get("PRISMRT_TEST_OUTPUT", Path.cwd() / "python_tests"))
    path.mkdir(parents=True, exist_ok=True)
    return path


@pytest.fixture(scope="session")
def renderer() -> prism.Renderer:
    # CPU tests never open a GPU device (devices() would create one).
    return prism.Renderer(working_space="lin_rec709", log=True, disable_vulkan=True)


def quad_scene(renderer: prism.Renderer, **scene_options) -> prism.Scene:
    """A red 2x2 quad at z = 0, a constant environment, a sphere light and a
    camera at z = 4 looking down -Z (tests/session_test.cpp's scene)."""
    scene = renderer.create_scene(**scene_options)
    points = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], np.float32)
    scene.add_material(diffuse=(0.8, 0.1, 0.1), name="red")
    scene.add_mesh(points, indices=np.array([0, 1, 2, 3], np.int32), counts=np.array([4], np.int32), name="quad")
    scene.add_instance("quad", materials=["red"], prim_id=7, name="quad_instance")
    scene.add_environment(color=(0.25, 0.5, 0.75), name="sky")
    scene.add_light("sphere", transform=prism.translate(0, 0, 3), intensity=10, name="key")
    scene.add_camera(camera_to_world=prism.translate(0, 0, 4), focal_length=20, near_clip=0.1, far_clip=100,
                     name="camera")
    return scene


def cpu_session(renderer, scene, **kwargs) -> prism.Session:
    options = dict(device="cpu", allow_fallback=False, width=32, height=24, camera="camera", samples=8,
                   adaptive_threshold=0.0)
    options.update(kwargs)
    return renderer.create_session(scene, **options)
