"""Render a USD file to EXR and PNG, and look at the pixels with NumPy.

    python 01_quickstart_usd.py assets/scenes/CornellBox.usda --camera /World/Camera
"""

import argparse

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("scene")
parser.add_argument("--camera", default=None, help="camera prim path (default: the first camera in the file)")
parser.add_argument("--width", type=int, default=640)
parser.add_argument("--height", type=int, default=480)
parser.add_argument("--spp", type=int, default=64)
parser.add_argument("--device", default="cpu", help="auto, cpu, vulkan or metal")
parser.add_argument("--output", default="quickstart")
args = parser.parse_args()

renderer = prism.Renderer()                      # colour management etc. from the environment
scene = renderer.create_scene()
loaded = prism.usd.load(scene, args.scene)       # meshes, materials, lights, domes, cameras
print(loaded)

commit = scene.commit()                          # converts meshes, compiles materials
print(f"commit: {commit.meshes} meshes, {commit.materials} materials in {commit.seconds:.1f} s")

camera = args.camera or next(iter(loaded.cameras))
session = renderer.create_session(scene, device=args.device, width=args.width, height=args.height,
                                  camera=camera, aovs=["color", "albedo", "normal", prism.Aov.CameraDepth, "primId"],
                                  samples=args.spp, seed=1)
print(f"rendering on {session.device.name} {session.device_note}")
result = session.render()

# Every AOV as an EXR layer (linear, working space); the colour as a display-ready PNG.
prism.io.write(f"{args.output}.exr", result)
prism.io.write(f"{args.output}.png", result.aovs["color"])

# The pixels are NumPy views (no copy): (height, width, channels), top row first.
color = np.asarray(result.aovs["color"])
distance = np.asarray(result.aov(prism.Aov.CameraDepth))[..., 0]   # along the view axis, scene units
hit = np.asarray(result.aovs["primId"])[..., 0] >= 0             # -1 where no geometry was hit
print(f"{result.render_seconds:.2f} s, mean colour {color.reshape(-1, 3).mean(0)}, "
      f"{hit.mean():.0%} of pixels hit geometry, distance {distance[hit].min():.2f}..{distance[hit].max():.2f}")
