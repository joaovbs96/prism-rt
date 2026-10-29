# prismrt examples

Recipes for the Python API (`docs/python.md`). Each script is standalone,
takes `--help`, and renders on the CPU by default (`--device` where offered).
Paths in the commands are relative to the repository root.

| Script | What it shows |
|---|---|
| `01_quickstart_usd.py` | Load a USD file, render, write EXR (every AOV) and PNG, read pixels as NumPy |
| `02_scene_from_numpy.py` | A procedural scene from arrays: height-field mesh, instanced spheres, lights |
| `03_turntable.py` | Orbit the camera and write frames; camera edits need no commit; find the orbit centre from the depth AOV |
| `04_parameter_sweep.py` | `scene.params` + incremental commits, a contact sheet of a light/material sweep |
| `05_progressive_preview.py` | Progressive render with progress, ETA, periodic previews, a time budget, clean Ctrl-C |
| `06_aovs_masks_picking.py` | Object masks from `primId`, normals and camera depth as images, pixel picking |
| `07_scene_from_dict.py` | A scene from a JSON-like dictionary, glTF mixed in, key-path errors, `to_dict` round trip |
| `08_synthetic_dataset.py` | Randomised scenes for training data: image, instance mask, depth and a JSON record per sample |
| `09_usd_unsaved_edits.py` | Edit a pxr stage in memory and render it without saving; the StageCache path |
| `10_convergence_study.py` | Noise against samples per pixel, RMSE slope against a reference |
| `11_gltf_material_swap.py` | Swap a glTF model's material for MaterialX looks; only the material recompiles |
| `12_material_preview.py` | MaterialX looks on the built-in shaderball: `preview_material`, then one preview scene with the material swapped |

```
. .\prism_env.ps1
$env:PYTHONPATH = "$PWD\build\python;$env:PYTHONPATH"     # or pip install the prismrt wheel
python examples\python\01_quickstart_usd.py assets\scenes\CornellBox.usda
python examples\python\03_turntable.py assets\scenes\InOneWeekend.usda --frames 24
python examples\python\09_usd_unsaved_edits.py assets\scenes\CornellBox.usda --camera /World/Camera
python examples\python\11_gltf_material_swap.py assets\models\Duck\Duck.glb
python examples\python\12_material_preview.py assets\materials\standard_surface_marble_solid.mtlx assets\materials\Car_Paint_1k_8b\Car_Paint.mtlx
```

Notes:
- `import pxr` before `prismrt` and PySide6 when a script uses both (DLL load order).
- AOVs are Hydra tokens (`"color"`, `"normal"`, `"primId"`, ...) or `prismrt.Aov` members;
  `"depth"` is normalised device depth, `prismrt.Aov.CameraDepth` the distance along the view axis.
- Images are `(height, width, channels)` read-only NumPy views without a copy; `.copy()` for a
  writable array.
