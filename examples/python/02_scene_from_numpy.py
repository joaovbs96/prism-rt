"""Build a scene procedurally from NumPy arrays: a height-field terrain mesh,
a grid of instanced spheres with random colours, a rect light and a sky colour.

    python 02_scene_from_numpy.py
"""

import argparse

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("--width", type=int, default=800)
parser.add_argument("--height", type=int, default=450)
parser.add_argument("--spp", type=int, default=64)
parser.add_argument("--output", default="procedural.png")
args = parser.parse_args()


def heightfield(n=96, size=12.0):
    """A grid mesh (points, triangle indices) displaced by a few sine waves."""
    x, z = np.meshgrid(np.linspace(-size / 2, size / 2, n), np.linspace(-size / 2, size / 2, n))
    y = 0.15 * np.sin(1.3 * x) * np.cos(0.9 * z) + 0.05 * np.sin(4.0 * x + 2.0 * z)
    points = np.stack([x, y, z], axis=-1).reshape(-1, 3).astype(np.float32)
    i = np.arange(n - 1)
    a = (i[:, None] * n + i[None, :]).ravel()          # top-left corner of each quad
    quads = np.stack([a, a + n, a + n + 1, a + 1], axis=-1)
    triangles = np.concatenate([quads[:, [0, 1, 2]], quads[:, [0, 2, 3]]]).astype(np.int32)
    return points, triangles


renderer = prism.Renderer()
scene = renderer.create_scene()
rng = np.random.default_rng(7)

points, triangles = heightfield()
scene.add_material(diffuse=(0.55, 0.5, 0.45), name="ground")
scene.add_mesh(points, indices=triangles, name="terrain")      # arrays are read at commit, not copied here
scene.add_instance("terrain", materials=["ground"], name="terrain_instance")

# One sphere prototype, many instances: each instance has its own transform and material.
scene.add_spheres(np.zeros((1, 3), np.float32), np.array([1.0], np.float32), name="ball")
for row in range(4):
    for col in range(6):
        color = tuple(rng.uniform(0.1, 0.9, 3))
        material = scene.add_material(diffuse=color, name=f"ball_{row}_{col}")
        radius = rng.uniform(0.25, 0.45)
        position = (-3.75 + 1.5 * col, radius + 0.1, -2.25 + 1.5 * row)
        scene.add_instance("ball", transform=prism.scale(radius) @ prism.translate(*position),
                           materials=[material], name=f"ball_{row}_{col}_instance")

scene.add_light("rect", width=4, height=4, intensity=6,
                transform=prism.look_at((3, 6, 4), (0, 0, 0)), name="key")
scene.add_environment(color=(0.25, 0.3, 0.4), name="sky")
scene.add_camera(prism.Camera.look_at((0, 4, 9), (0, 0, 0), vertical_fov=35), name="main")

print(scene)
result = prism.render(scene, "main", width=args.width, height=args.height, device="cpu", samples=args.spp, seed=1)
prism.io.write(args.output, result.aovs["color"])
print(f"wrote {args.output} in {result.render_seconds:.2f} s")
