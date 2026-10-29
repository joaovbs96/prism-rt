"""Load a glTF model and swap its material for MaterialX looks: render the
original and each look side by side. Only the material changes between
renders, so each commit compiles one material and converts no mesh.

    python 11_gltf_material_swap.py assets/models/Duck/Duck.glb
"""

import argparse
import pathlib

import numpy as np
import prismrt as prism

ASSETS = pathlib.Path(__file__).resolve().parents[2] / "assets"

parser = argparse.ArgumentParser()
parser.add_argument("model")
parser.add_argument("--looks", nargs="*", default=[str(ASSETS / "materials/standard_surface_marble_solid.mtlx"),
                                                     str(ASSETS / "materials/Car_Paint_1k_8b/Car_Paint.mtlx")])
parser.add_argument("--size", type=int, default=320)
parser.add_argument("--spp", type=int, default=64)
args = parser.parse_args()

renderer = prism.Renderer()
scene = renderer.create_scene()
model = prism.gltf.load(scene, args.model, name="model")
print(model)

scene.add_environment(color=(0.6, 0.6, 0.65), name="sky")
scene.add_light("distant", angle=3.0, intensity=2.0, transform=prism.look_at((1, 2, 1), (0, 0, 0)), name="sun")
scene.add_camera(prism.Camera.look_at((2.2, 1.8, 2.8), (0.1, 0.7, 0), vertical_fov=35), name="cam")  # frames the Duck (~1.6 units)
scene.commit()
session = renderer.create_session(scene, device="cpu", width=args.size, height=args.size, camera="cam",
                                  samples=args.spp, seed=1)

panels = [np.asarray(session.render().aovs["color"]).copy()]
for path in args.looks:
    look = scene.add_material(mtlx_file=path)
    scene.set_materials(model.instance, [look])
    changed = scene.commit()
    print(f"{path}: {changed.materials_compiled} material compiled, {changed.meshes_converted} meshes converted")
    panels.append(np.asarray(session.render().aovs["color"]).copy())

prism.io.write("looks.png", prism.Image(np.concatenate(panels, axis=1)), renderer)
print("wrote looks.png")
