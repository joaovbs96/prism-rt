"""Renders the wheel test's scenes with whatever prismrt is importable
(prism_python_wheel_install, tests/python/wheel_install_test.py).

    python -I wheel_render.py --source <repo> --out <dir> --cache <dir> [--package <dir>] [--usd]

On the CPU with a fixed seed, sample count and no adaptive sampling: a scene
from NumPy arrays plus a textured glTF file (MaterialX through MDL, image
textures), and with --usd assets/scenes/CornellBox.usda through prismrt.usd.
Writes <name>.exr and <name>.npy per render and prints one JSON line: the
DLLs the process loaded (after `import prismrt`, after the renders), the
default cache directory and the files written.
"""

from __future__ import annotations

import argparse
import json
import os
import sys


def loaded_modules() -> list[str]:
    """Full paths of every module (DLL, pyd, exe) in this process (psapi)."""
    import ctypes
    from ctypes import wintypes
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    psapi.EnumProcessModules.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.HMODULE), wintypes.DWORD,
                                         ctypes.POINTER(wintypes.DWORD)]
    psapi.GetModuleFileNameExW.argtypes = [wintypes.HANDLE, wintypes.HMODULE, wintypes.LPWSTR, wintypes.DWORD]
    process = kernel32.GetCurrentProcess()
    count = 1024
    while True:
        modules = (wintypes.HMODULE * count)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcessModules(process, modules, ctypes.sizeof(modules), ctypes.byref(needed)):
            raise OSError(ctypes.get_last_error(), "EnumProcessModules")
        if needed.value <= ctypes.sizeof(modules):
            break
        count = needed.value // ctypes.sizeof(wintypes.HMODULE) + 16
    paths = []
    buffer = ctypes.create_unicode_buffer(32768)
    for module in modules[:needed.value // ctypes.sizeof(wintypes.HMODULE)]:
        length = psapi.GetModuleFileNameExW(process, module, buffer, len(buffer))
        if length:
            paths.append(buffer.value[:length])
    return sorted(set(paths))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--cache", required=True)
    parser.add_argument("--package", help="directory to put first on sys.path (a build tree's python/)")
    parser.add_argument("--usd", action="store_true")
    args = parser.parse_args()
    if args.package:
        sys.path.insert(0, args.package)
    os.makedirs(args.out, exist_ok=True)

    import numpy as np
    import prismrt as prism
    summary = {"prismrt": os.path.dirname(prism.__file__), "version": prism.__version__,
               "default_cache_directory": str(prism.default_cache_directory()),
               "modules_after_import": loaded_modules(), "files": []}

    def write(name, renderer, result):
        exr = os.path.join(args.out, f"{name}.exr")
        problems = result.write(exr, renderer._native)
        assert problems == [], problems
        pixels = np.asarray(result.aovs["color"])
        assert np.isfinite(pixels).all() and pixels.max() > 0, f"{name}: empty image"
        np.save(os.path.join(args.out, f"{name}.npy"), pixels)
        summary["files"].append(name)

    renderer = prism.Renderer(disable_vulkan=True, cache_directory=args.cache)

    # A scene from arrays (a polygon quad, a triangle list, spheres) and a
    # textured glTF file (its MaterialX material compiles through MDL).
    scene = renderer.create_scene()
    points = np.array([[-1, -1, 0], [1, -1, 0], [1, 1, 0], [-1, 1, 0]], np.float32)
    scene.add_material(diffuse=(0.8, 0.1, 0.1), name="red")
    scene.add_material(diffuse=(0.2, 0.6, 0.3), name="green")
    scene.add_mesh(points, indices=np.array([0, 1, 2, 3], np.int32), counts=np.array([4], np.int32), name="quad")
    scene.add_instance("quad", transform=prism.translate(-2.1, 0, 0), materials=["red"], name="left")
    triangles = np.array([[0, -1, 0.5], [1, 1, 0.5], [-1, 1, 0.5]], np.float32)
    scene.add_mesh(triangles, indices=np.array([[0, 1, 2]], np.int32), name="triangle")
    scene.add_instance("triangle", transform=prism.translate(2.1, 0, 0), materials=["green"], name="right")
    scene.add_spheres(np.array([[0, 0, 0]], np.float32), np.array([0.4], np.float32), name="ball")
    scene.add_instance("ball", transform=prism.translate(0, -1.4, 0.6), materials=["red"], name="ball_instance")
    prism.gltf.load(scene, os.path.join(args.source, "tests/gltf/textured.gltf"), name="textured")
    scene.add_environment(color=(0.25, 0.5, 0.75), name="sky")
    scene.add_light("sphere", transform=prism.translate(0, 0, 3), intensity=10, name="key")
    scene.add_camera(camera_to_world=prism.translate(0, 0, 6), focal_length=20, near_clip=0.1, far_clip=100,
                     name="camera")
    commit = scene.commit()
    errors = [d.message for d in scene.diagnostics() if str(d.level).endswith("Error")]
    assert not errors, errors   # a material that fails to compile would render magenta
    session = renderer.create_session(scene, device="cpu", allow_fallback=False, width=96, height=64,
                                      camera="camera", samples=16, adaptive_threshold=0.0, seed=7)
    write("arrays_gltf", renderer, session.render())
    summary["materials_compiled"] = commit.materials_compiled

    if args.usd:
        import prismrt.usd as usd
        summary["modules_before_usd"] = summary["modules_after_import"]
        scene = renderer.create_scene()
        loaded = usd.load(scene, os.path.join(args.source, "assets/scenes/CornellBox.usda"),
                          included_purposes=["default"])
        assert loaded.renderable_placements > 0 and "/World/Camera" in loaded.cameras
        scene.commit()
        session = renderer.create_session(scene, device="cpu", allow_fallback=False, width=48, height=48,
                                          camera="/World/Camera", samples=8, adaptive_threshold=0.0, seed=3,
                                          sample_clamp=10.0)
        write("cornell_usd", renderer, session.render())

    summary["modules_after_render"] = loaded_modules()
    print("WHEEL_RENDER " + json.dumps(summary))
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
