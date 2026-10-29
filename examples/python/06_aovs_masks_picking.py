"""Use AOVs as data: per-object masks from the instance ids, depth and normals
as arrays, pixel picking, and one multi-layer EXR with everything.

    python 06_aovs_masks_picking.py assets/scenes/CornellBox.usda
"""

import argparse

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("scene")
parser.add_argument("--camera", default=None)
parser.add_argument("--size", type=int, default=400)
parser.add_argument("--spp", type=int, default=32)
args = parser.parse_args()

renderer = prism.Renderer()
scene = renderer.create_scene()
loaded = prism.usd.load(scene, args.scene)
scene.commit()

session = renderer.create_session(scene, device="cpu", width=args.size, height=args.size,
                                  camera=args.camera or next(iter(loaded.cameras)), samples=args.spp, seed=1,
                                  aovs=["color", "albedo", "normal", "cameraDepth", "primId", "instanceId"])
result = session.render()

color = np.asarray(result.aovs["color"])
normal = np.asarray(result.aovs["normal"])
depth = np.asarray(result.aov("cameraDepth"))[..., 0]   # distance along the view axis
prim_id = np.asarray(result.aovs["primId"])[..., 0]           # int32, -1 where nothing was hit

# One mask per object: pixel coverage and average colour of each prim.
ids, counts = np.unique(prim_id[prim_id >= 0], return_counts=True)
print(f"{len(ids)} objects visible")
for i, count in sorted(zip(ids, counts), key=lambda x: -x[1])[:8]:
    mask = prim_id == i
    print(f"  prim {i:4}: {count / prim_id.size:6.1%} of the image, mean colour {color[mask].mean(0).round(3)}")

# Masks as an image: each object in a random flat colour.
rng = np.random.default_rng(0)
palette = rng.uniform(0.1, 1.0, (int(prim_id.max()) + 2, 3)).astype(np.float32)
palette[-1] = 0                                                  # index -1: background
prism.io.write("masks.png", prism.Image(palette[prim_id]), renderer)

# Normals and depth as viewable images.
prism.io.write("normals.png", prism.Image((normal * 0.5 + 0.5).astype(np.float32)), renderer)
finite = prim_id >= 0
near, far = depth[finite].min(), depth[finite].max()
depth_view = np.where(finite, 1 - (depth - near) / max(far - near, 1e-6), 0).astype(np.float32)
prism.io.write("depth.png", prism.Image(np.repeat(depth_view[..., None], 3, axis=-1)), renderer)

# Picking: what is under the centre pixel? (Progressive sessions answer from the last pass.)
session.start()
session.wait(2.0)
hit = session.pick(args.size // 2, args.size // 2)
session.cancel()
session.wait()
if hit.hit:
    print(f"centre pixel: prim {hit.prim_id}, instance {hit.instance_id}, at {np.round(hit.position, 3)}, "
          f"normal {np.round(hit.normal, 3)}")

prism.io.write("aovs.exr", result)                              # every AOV as a layer
print("wrote masks.png, normals.png, depth.png, aovs.exr")
