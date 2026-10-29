"""Preview MaterialX documents on the built-in shaderball (the USD-WG
StandardShaderBall in its own lit box): one call for one look, then one
preview scene whose material is swapped between renders, so each commit
compiles one material and converts no mesh.

    python 12_material_preview.py assets/materials/standard_surface_marble_solid.mtlx assets/materials/Car_Paint_1k_8b/Car_Paint.mtlx
"""

import argparse

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("looks", nargs="+", help=".mtlx documents (each previews its first material)")
parser.add_argument("--size", type=int, default=320)
parser.add_argument("--spp", type=int, default=64)
parser.add_argument("--device", default="cpu")
args = parser.parse_args()

renderer = prism.Renderer()

# One look: PrismRT --material in Python.
result = prism.preview_material(args.looks[0], width=args.size, height=args.size, samples=args.spp, seed=1,
                                device=args.device, renderer=renderer)
prism.io.write("preview.png", result.aovs["color"], renderer)
print("wrote preview.png")

# Several looks: the preview stage once, the material swapped.
scene = renderer.create_scene()
preview = prism.usd.load_material_preview(scene, args.looks[0])
print(preview)
scene.commit()
session = renderer.create_session(scene, device=args.device, width=args.size, height=args.size,
                                  camera=preview.camera, samples=args.spp, seed=1, sample_clamp=10.0)
panels = []
for index, path in enumerate(args.looks):
    if index:
        scene.update_material("preview_material", kind="materialx_file", path=path, material_name="")
        changed = scene.commit()
        print(f"{path}: {changed.materials_compiled} material compiled, {changed.meshes_converted} meshes converted")
    panels.append(np.asarray(session.render().aovs["color"]).copy())

prism.io.write("previews.png", prism.Image(np.concatenate(panels, axis=1)), renderer)
print("wrote previews.png")
