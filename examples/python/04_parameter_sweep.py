"""Sweep a parameter and lay the renders out as a contact sheet.
Uses ``scene.params`` (like Mitsuba's ``traverse``): set a key, ``update()``
commits only what changed (here: a light and a material, no mesh work).

    python 04_parameter_sweep.py
"""

import argparse

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("--size", type=int, default=200)
parser.add_argument("--spp", type=int, default=32)
parser.add_argument("--output", default="sweep.png")
args = parser.parse_args()

renderer = prism.Renderer()
scene = prism.Scene.from_dict(renderer, {
    "materials": {"floor_mat": {"diffuse": (0.7, 0.7, 0.7)}, "ball_mat": {"diffuse": (0.8, 0.2, 0.2)}},
    "meshes": {"plane": {"points": np.array([[-4, 0, -4], [4, 0, -4], [4, 0, 4], [-4, 0, 4]], np.float32),
                         "indices": np.array([[0, 2, 1], [0, 3, 2]], np.int32)}},
    "spheres": {"sphere": {"centers": np.zeros((1, 3), np.float32), "radii": np.array([1.0], np.float32)}},
    "instances": {"floor": {"prototype": "plane", "materials": ["floor_mat"]},
                  "ball": {"prototype": "sphere", "transform": prism.translate(0, 1, 0), "materials": ["ball_mat"]}},
    "lights": {"key": {"shape": "sphere", "radius": 0.5, "intensity": 30, "transform": prism.translate(2, 4, 2)}},
    "environments": {"sky": {"color": (0.05, 0.05, 0.08)}},
    "cameras": {"main": {"look_at": {"eye": (0, 2.5, 6), "target": (0, 0.8, 0), "vertical_fov": 35}}},
})
scene.commit()
session = renderer.create_session(scene, device="cpu", width=args.size, height=args.size, camera="main",
                                  samples=args.spp, seed=1)

params = scene.params
intensities = [5, 15, 45]
colors = [(0.8, 0.2, 0.2), (0.2, 0.7, 0.3), (0.2, 0.3, 0.9), (0.9, 0.9, 0.9)]
rows = []
for intensity in intensities:
    row = []
    for color in colors:
        params["key.intensity"] = intensity
        params["ball_mat.color"] = color     # names are unique across the whole scene
        changed = params.update()                         # a CommitResult: what was rebuilt
        row.append(np.asarray(session.render().aovs["color"]).copy())
        print(f"intensity {intensity:>3}, colour {color}: {changed.materials_compiled} materials compiled, "
              f"{changed.meshes_converted} meshes converted")
    rows.append(np.concatenate(row, axis=1))
sheet = np.concatenate(rows, axis=0)                      # (3 * size, 4 * size, 3)

prism.io.write(args.output, prism.Image(sheet), renderer)  # display-encoded through the renderer's view
print(f"wrote {args.output} ({sheet.shape[1]}x{sheet.shape[0]})")
