# Python: the `prismrt` package

`prismrt` is the Python API of PrismRT: nanobind bindings of the public C++
API (`include/prism/*.h`) plus a thin Pythonic layer (keyword arguments,
names, `scene.params`, `Scene.from_dict`). It links the shared `prism.dll`
(and `prism_usd.dll` for `prismrt.usd`) of the build, so a process that also
loads hdPrism holds one copy of the core. Design: `docs/sdk_design.md`
section 6; decisions: `docs/DECISIONS.md` (2026-10-29).

This page covers the build tree. Wheels and a packaged layout are the next
migration step (8).

## Build

Requirements: Python 3.11 with its development files (the interpreter
`tools/deps.json` names), NumPy, and nanobind 3.1.0. nanobind comes from
PyPI into a versioned prefix (vcpkg's port of the same version builds its own
CPython, so it is not used):

```
py -3.11 -m pip install --no-deps --target D:\prism-deps\nanobind-3.1.0 nanobind==3.1.0
```

and `nanobind_DIR` points at it (`cmake/PrismLocalDependencies.cmake`, or
`-Dnanobind_DIR=D:/prism-deps/nanobind-3.1.0/nanobind/cmake`; the template
reads `PRISM_NANOBIND_ROOT`). Without `nanobind_DIR` the configure also tries
`python -m nanobind --cmake_dir`.

`PRISM_BUILD_PYTHON` defaults to ON when nanobind is found (OFF otherwise,
with a status line). The build produces, in `<build>/python/prismrt/`:

- `_prism.cp311-win_amd64.pyd`: the core with the glTF and OBJ loaders and
  image writing (`prism_gltf`, `prism_obj`, `prism_io` are static);
- `_prism_usd.cp311-win_amd64.pyd`: `prismrt.usd` (`prism_usd.dll`, USD);
- the package's `.py` files (copied from `python/prismrt/`), `py.typed`, and
  `.pyi` stubs of both extensions (nanobind's stubgen, at build time);
- `_build_config.py` (generated): the directories of `prism.dll` and its
  dependencies for this build, the MaterialX/MDL defaults, the USD version
  and library prismrt.usd links, the MSVC runtime it needs.

Not built with the stable ABI: nanobind's linked stable ABI needs CPython
3.12 or later (its split mode is new in nanobind 3); this build makes a `cp311`
module.

## Use from the build tree

```
. .\prism_env.ps1                                   # USD's pxr on PYTHONPATH, OCIO, PRISM_*
$env:PYTHONPATH = "D:\prism-builds\<build>\python;$env:PYTHONPATH"
python -c "import prismrt; print(prismrt.__version__)"
```

`prism_env.ps1` is not needed for `prismrt` itself: CPython (since 3.8) ignores
`PATH` for the dependencies of extension modules, so `prismrt/__init__.py`
adds the directories of `_build_config.py` with `os.add_dll_directory`
(`PRISMRT_DLL_PATH`, `os.pathsep`-separated, adds more). A DLL found in
several of them (Embree ships an older `tbb12.dll` than oneTBB) is loaded
from the first, as `PATH` order would. The environment script still provides
`OCIO`, the `PRISM_*` variables and the USD Python bindings (`pxr`).

Import order: `import pxr` (and `prismrt`) before PySide6. PySide6 ships an
old `msvcp140.dll`; `import prismrt` raises an `ImportError` that says so when
an older C++ runtime than the build's is already loaded
(`PRISMRT_SKIP_RUNTIME_CHECK=1` skips the check).

## A first render

```python
import numpy as np
import prismrt as prism

r = prism.Renderer()                       # options from the environment (OCIO, PRISM_*)
scene = r.create_scene()
loaded = prism.usd.load(scene, "assets/scenes/CornellBox.usda")
scene.commit()                             # converts meshes, compiles materials (GIL released)

session = r.create_session(scene, device="cpu", width=512, height=512,
                           camera="/World/Camera", aovs=["color", "albedo", "normal"],
                           samples=64, adaptive_threshold=0.0, seed=1)
result = session.render()                  # blocking, GIL released
rgb = np.asarray(result.aovs["color"])     # (512, 512, 3) float32, no copy, read-only
result.write("cornell.exr", r._native)     # every AOV as EXR layers (prism_io)
result.aovs["color"].write("cornell.png")  # the colour through the renderer's view
```

Images are `(height, width, channels)` arrays, top row first; ids
(`primId`, `elementId`, `instanceId`) are `int32`, everything else
`float32`. `np.asarray(image)` and `np.from_dlpack(image)` (or
`torch.from_dlpack`) view the pixels without a copy and keep them alive;
the view is read-only (`.copy()` for a writable one). `prism.Image(array)`
copies a float32 or int32 array.

`prism.render(scene, camera, width=..., samples=..., ...)` does commit,
session and render in one call.

## Scenes from arrays

```python
points = np.array([[-1, 0, -1], [1, 0, -1], [1, 0, 1], [-1, 0, 1]], np.float32)
indices = np.array([[0, 1, 2], [0, 2, 3]], np.int32)

scene = r.create_scene()
scene.add_material(diffuse=(0.8, 0.1, 0.1), name="red")
scene.add_material(mtlx_file="materials/gold.mtlx", name="gold")
scene.add_mesh(points, indices=indices, name="floor")       # a triangle list
scene.add_instance("floor", materials=["red"], name="floor_instance")
scene.add_spheres(np.zeros((1, 3), np.float32), np.array([0.5], np.float32), name="ball")
scene.add_instance("ball", transform=prism.translate(0, 0.5, 0), materials=["gold"])
scene.add_light("rect", width=2, height=2, intensity=10,
                transform=prism.look_at((0, 4, 0), (0, 0, 0), up=(0, 0, 1)), name="key")
scene.add_environment(color=(0.05, 0.05, 0.05))             # constant; texture= makes a dome
scene.add_camera(prism.Camera.look_at((0, 2, 5), (0, 0, 0), vertical_fov=40), name="main")
scene.commit()
```

- Every `add_*` returns a handle (`MeshId`, `InstanceId`, ...) and takes
  `name=` (else `mesh0`, `light1`, ...; names are unique in a scene).
  Handles and names are interchangeable in the other calls;
  `scene["key"]` is the handle.
- Keyword arguments are the C++ `*Desc` fields in snake_case
  (`prism.LightDesc`, `prism.MeshDesc`, ...); a description object can be
  passed as `desc=`. Enums take their members or names as strings
  (`"rect"`, `"lat_long"`, `"face_varying"`, Hydra AOV tokens such as
  `"primvars:st"`).
- Vectors and colours take tuples, lists or arrays; matrices are 4x4 in
  USD's layout (row vectors, translation in row 3, local to world).
  `prism.translate`, `scale`, `rotate`, `look_at` and `identity` return NumPy
  4x4 arrays; compose with `a @ b`. A list of `(time, value)` pairs is a
  time-sampled value (accepted; one instant renders until motion blur).
- Meshes: `counts` (per-face vertex counts) makes a polygon mesh (USD's
  conversion, subdivision, displacement); without it `indices` are
  triangles. `uvs` default to per-vertex when there is one per point.
- Arrays are not copied in Python: a C-contiguous CPU array of an accepted
  dtype (float32 or float64 points, int32 or uint32 indices, ...) is viewed
  as it is and kept alive by the scene, which reads it at `commit()` and
  converts it into its own structures. Other dtypes or layouts are
  converted once. In-place changes after `commit()` have no effect until
  `update_mesh_points` and another commit. `prism.buffer(array, ...)` makes
  such a view (`.address` is the array's memory).
- `prism.gltf.load(scene, path)` and `prism.obj.load(scene, path)` add a
  file's mesh, instance (`<name>.instance`) and materials.

### Edits, `params` and commits

Edits are recorded and published by `scene.commit()` (incremental: it
rebuilds what the edits reach). `scene.pending_edits` holds the
`DirtyKind` bits of the edits since the last commit.

```python
params = scene.params                     # like Mitsuba's traverse()
params["key.intensity"] = 40              # update_light
params["floor_instance.transform"] = prism.translate(0, -1, 0)   # set_transform
result = params.update()                  # scene.commit(): a CommitResult
```

Keys are `<name>.<field>` for objects added through the Python API; reads
return copies. `session.render()` raises `InvalidArgumentError` while the
scene has uncommitted edits (the C++ session would render the last committed
revision; `allow_uncommitted=True` does that); a progressive session takes a
new commit at its next pass. Camera edits take effect without a commit.

### Dictionaries

```python
scene = prism.Scene.from_dict(r, {
    "materials": {"red": {"diffuse": (0.8, 0.1, 0.1)}},
    "meshes": {"floor_mesh": {"points": points, "indices": indices}},
    "instances": {"floor": {"prototype": "floor_mesh", "materials": [{"ref": "red"}]}},
    "lights": [{"name": "key", "shape": "rect", "intensity": 10, "transform": prism.translate(0, 0, 3)}],
    "environments": {"sky": {"color": (0.05, 0.05, 0.05)}},
    "cameras": {"main": {"look_at": {"eye": (0, 0, 5), "target": (0, 0, 0), "vertical_fov": 40}}},
})
```

Sections are dicts of name -> spec (or lists of specs with `name`), the
specs are the `add_*` keywords, references are names or `{"ref": name}` and
resolve whatever the order. Errors carry the key path
(`scene.instances['floor'].materials[0]: no material named 'redd' in this
scene`). `scene.to_dict()` returns what `from_dict` reads back (objects added
through the Python API).

## Sessions, progress and cancellation

```python
def on_progress(p: prism.ProgressInfo) -> bool:
    print(f"\r{p.phase:10} {p.fraction:6.1%} {p.samples:7.1f} spp", end="")
    return not stop.is_set()               # False cancels

session = r.create_session(scene, camera="main", width=1280, height=720, samples=1024)
session.start(on_progress)                 # progressive; returns at once
while session.wait(0.5).state == prism.SessionState.Running:
    preview = np.asarray(session.aov("color"))   # snapshot of the last pass
session.cancel()

try:
    session.render(on_progress)            # blocking form
except prism.CancelledError:
    pass
```

- The GIL is released in `Renderer()`, `commit`, `render`, `prepare`,
  `wait`, `aov`, the loaders and every session call; callbacks run on C++
  threads (render: a monitor thread about ten times a second; progressive:
  the session's thread at pass boundaries; commit: the committing thread)
  and take the GIL.
- `commit` and `render` run on a C++ thread while the calling thread waits in
  100 ms slices: Ctrl-C cancels, joins and raises `KeyboardInterrupt`;
  `wait()` raises it without stopping the render. A callback that returns
  False cancels (`CancelledError`); one that raises cancels and its
  exception is raised from `render`, `commit` or the next `wait()`.
- `render(samples=..., seed=...)` overrides those settings for one call;
  `set_settings(**fields)` changes them.
- At exit an `atexit` hook cancels and joins every running session, so the
  interpreter exits cleanly (also with Python callbacks still registered).
  `with session:` cancels and waits at the end of the block.

### Devices

`create_session(device="auto" | "cpu" | "vulkan" | "metal",
allow_fallback=True)`: `auto` (the default) prefers the GPU device.
`session.device` and `session.device_note` say which device renders and why
it fell back; `allow_fallback=False` raises `UnsupportedError`.
`renderer.devices()` lists the devices this build and machine have (it
creates them: `Renderer(disable_vulkan=True)` or `PRISM_DISABLE_VULKAN=1`
keeps a process off the GPU).

## USD

```python
from pxr import Usd                        # pxr first (DLL load order)
import prismrt as prism

stage = Usd.Stage.Open("shot.usda")
...                                       # unsaved edits
loaded = prism.usd.load(scene, stage, time=0)   # or a file path
loaded.cameras                             # {"/World/Camera": CameraId}: cameras by prim path
```

- `load(scene, source, time=None, binding_purposes=(), included_purposes=(),
  domes=True, cameras=True)`: gprims, UsdShade materials (compiled during
  the load), lights with linking, domes and cameras (added to the scene,
  named by prim path, so `camera="/World/Camera"` works).
  `load_camera(source, path)` returns a `CameraDesc`.
- A stage in `UsdUtils.StageCache` loads through the cache by its id (the
  StageCache path; `load_cached(scene, id)` too). Any other stage loads
  through a second stage over its open root and session layers, so unsaved
  edits render and the caller's stage is untouched. prismrt does not insert
  stages into the cache: USD's Python bindings hand a stage's ownership to
  the cache on `Insert`, and erasing it again closes the caller's stage.
- `pxr` and `prismrt.usd` share the USD libraries in one process, so they
  must be the same build: importing `prismrt.usd` after `pxr` checks
  `pxr.Usd.GetVersion()` against the version prismrt.usd was built with and
  that the loaded `usd_usd.dll` is the one it links, and raises
  `ImportError` before `prism_usd.dll` loads. In the build tree that is the
  build's USD (`D:\prism-deps\openusd-...`, whose `lib/python` `prism_env.ps1`
  puts on `PYTHONPATH`). The packaged module will use a private USD and take
  stages as files or exported layers (`docs/DECISIONS.md`, 2026-10-27).

Matching the CLI: the CLI clamps samples at 10 by default
(`--max-sample-radiance`), `RenderSettings.sample_clamp` defaults to 0; the
CLI lights scenes without lights with usdview's default dome
(`create_scene(default_dome=True, default_dome_texture=prism.usd.default_dome_texture())`). With those
and the same settings, Python and CLI images are bit-identical
(`prism_python_render`).

## Errors and logging

| Exception | Also a | C++ `ErrorCode` |
|---|---|---|
| `prismrt.Error` | `RuntimeError` | (base) |
| `InvalidArgumentError` | `ValueError` | InvalidArgument |
| `InvalidHandleError` | `ValueError` | InvalidHandle |
| `NotFoundError` | `LookupError` | NotFound |
| `UnsupportedError` | `NotImplementedError` | Unsupported |
| `DeviceLostError` | | DeviceLost |
| `OutOfMemoryError` | `MemoryError` | OutOfMemory |
| `CancelledError` | | Cancelled |
| `InternalError` | | Internal |

Diagnostics of a renderer go to the Python logger `prismrt` (child loggers
per category: `prismrt.lights`, `prismrt.materials`, ...) by default;
`Renderer(log=callable)` receives `(level, category, message)` on any
thread, `log=False` leaves them on standard error. `scene.diagnostics()`
lists the last commit's (failed materials render magenta). Some loader
output still prints to standard output.

## Renderer options

`Renderer()` starts from `RendererOptions.from_environment()` (`OCIO`,
`PRISM_*`); `Renderer(env=False)` from the built-in defaults (then set
`materialx_libraries=` for MaterialX materials). Keywords: `working_space`,
`color_config`, `display`, `view`, `cache_directory`, `disable_vulkan`,
`log_level`, `materialx_libraries`, `sss_fallback`; or pass a
`prism.RendererOptions`. Several renderers per process are allowed (no
global state; the MDL runtime and the content-addressed caches are shared).

## Stubs, tests

`prismrt/_prism.pyi` and `_prism_usd.pyi` are generated at build time;
`py.typed` marks the package as typed.

Tests (`tests/python`, pytest; label `python`, the Vulkan one also `gpu` with
`RESOURCE_LOCK gpu`): `prism_python_api` (exceptions, zero-copy inputs and
outputs, params, from_dict/to_dict, vectors and enums, logging, two
renderers), `prism_python_render` (Python and CLI images bit-identical on
two USD scenes), `prism_python_progress` (callbacks, cancel, Ctrl-C,
progressive sessions, cancelled commits), `prism_python_exit` (interpreter
exit with running sessions), `prism_python_usd` (files, StageCache, unsaved
edits, the pxr check; skipped without `pxr`), `prism_python_render_vulkan`.
They need pytest 9.1.1 and NumPy in the build's Python; CPU tests run with
`PRISM_DISABLE_VULKAN=1`.

```
ctest --test-dir <build> -C RelWithDebInfo -L python -LE gpu --output-on-failure
```

Not yet: wheels and a relocatable layout (step 8), free-threaded Python,
`MaterialDesc` parameter overrides, GPU-resident DLPack.
