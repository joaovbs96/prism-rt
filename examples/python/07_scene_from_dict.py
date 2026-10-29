"""Describe a scene as data: a dictionary (from code, JSON or YAML) with
named objects and references, glTF/OBJ models mixed in, and a round trip
through ``to_dict``.

    python 07_scene_from_dict.py
"""

import json
import pathlib

import numpy as np
import prismrt as prism

ASSETS = pathlib.Path(__file__).resolve().parents[2] / "assets"

# Plain data: what a JSON file or a config system would hold. References are
# names (or {"ref": name}) and resolve in any order.
description = json.loads("""
{
  "materials": {
    "white": {"diffuse": [0.8, 0.8, 0.8]},
    "marble": {"mtlx_file": "materials/standard_surface_marble_solid.mtlx"},
    "lamp": {"emissive": [8.0, 7.0, 6.0]}
  },
  "spheres": {
    "unit_sphere": {"centers": [[0, 0, 0]], "radii": [1.0]}
  },
  "instances": {
    "statue": {"prototype": "unit_sphere", "materials": [{"ref": "marble"}]},
    "bulb": {"prototype": "unit_sphere", "materials": ["lamp"]}
  },
  "lights": {"key": {"shape": "rect", "width": 3, "height": 3, "intensity": 8}},
  "environments": {"sky": {"color": [0.05, 0.05, 0.07]}},
  "cameras": {"main": {"look_at": {"eye": [0, 1.5, 5], "target": [0, 0.5, 0], "vertical_fov": 40}}}
}
""")

marble = description["materials"]["marble"]
marble["mtlx_file"] = str(ASSETS / marble["mtlx_file"])        # paths in the file are relative to assets/

# Arrays and transforms can be NumPy values in the same dictionary.
description["meshes"] = {"floor_mesh": {
    "points": np.array([[-5, 0, -5], [5, 0, -5], [5, 0, 5], [-5, 0, 5]], np.float32),
    "indices": np.array([[0, 2, 1], [0, 3, 2]], np.int32)}}
description["instances"]["floor"] = {"prototype": "floor_mesh", "materials": ["white"]}
description["instances"]["statue"]["transform"] = prism.translate(0, 1, 0)
description["lights"]["key"]["transform"] = prism.look_at((2, 4, 3), (0, 0, 0))
description["instances"]["bulb"]["transform"] = prism.scale(0.2) @ prism.translate(1.5, 2.5, 1)

renderer = prism.Renderer()
scene = prism.Scene.from_dict(renderer, description)

# Files load into the same scene next to the dictionary's objects.
duck = prism.gltf.load(scene, ASSETS / "models/Duck/Duck.glb", name="duck")
scene.set_transform(duck.instance, prism.rotate((0, 1, 0), 30) @ prism.translate(-1.6, 0, 0.8))

# Errors name the key path, e.g. "scene.instances['floor'].materials[0]: no material named 'whyte'".
try:
    broken = dict(description, instances={"floor": {"prototype": "floor_mesh", "materials": ["whyte"]}})
    prism.Scene.from_dict(renderer, broken)
except prism.NotFoundError as error:
    print(f"as expected: {error}")

result = prism.render(scene, "main", width=640, height=400, device="cpu", samples=64, seed=1)
prism.io.write("from_dict.png", result.aovs["color"])

# to_dict returns what from_dict reads back (the objects added through Python).
saved = scene.to_dict()
print("sections:", {k: len(v) for k, v in saved.items()})
clone = prism.Scene.from_dict(renderer, saved)
print("round trip:", clone)
