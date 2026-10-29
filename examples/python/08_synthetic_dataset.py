"""Synthetic training data: randomise object placement, colours, lighting and
camera; save the image, a per-pixel instance mask, depth and a JSON record
per sample. The scene is built once; every sample is a few cheap edits and
an incremental commit.

    python 08_synthetic_dataset.py --samples 20 --out dataset
"""

import argparse
import json
import math
import pathlib

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("--samples", type=int, default=10)
parser.add_argument("--objects", type=int, default=6)
parser.add_argument("--size", type=int, default=256)
parser.add_argument("--spp", type=int, default=32)
parser.add_argument("--out", default="dataset")
parser.add_argument("--seed", type=int, default=0)
args = parser.parse_args()

out = pathlib.Path(args.out)
out.mkdir(parents=True, exist_ok=True)
rng = np.random.default_rng(args.seed)

renderer = prism.Renderer()
scene = renderer.create_scene()
scene.add_mesh(np.array([[-10, 0, -10], [10, 0, -10], [10, 0, 10], [-10, 0, 10]], np.float32),
               indices=np.array([[0, 2, 1], [0, 3, 2]], np.int32), name="ground_mesh")
scene.add_material(diffuse=(0.5, 0.5, 0.5), name="ground_mat")
scene.add_instance("ground_mesh", materials=["ground_mat"], name="ground")
scene.add_spheres(np.zeros((1, 3), np.float32), np.array([1.0], np.float32), name="sphere")
# A cube mesh as a second shape class.
corners = np.array([[x, y, z] for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)], np.float32)
faces = np.array([[0, 1, 3, 2], [4, 6, 7, 5], [0, 4, 5, 1], [2, 3, 7, 6], [0, 2, 6, 4], [1, 5, 7, 3]], np.int32)
scene.add_mesh(corners, indices=faces.ravel(), counts=np.full(6, 4, np.int32), name="cube")

classes = ["sphere", "cube"]
objects = []
for i in range(args.objects):
    shape = classes[i % len(classes)]
    scene.add_material(diffuse=(0.5, 0.5, 0.5), name=f"mat{i}")
    # prim_id tags the instance in the primId AOV: the mask value of this object.
    scene.add_instance(shape, materials=[f"mat{i}"], prim_id=i + 1, name=f"obj{i}")
    objects.append((f"obj{i}", f"mat{i}", shape))
scene.add_light("distant", angle=2.0, intensity=3.0, name="sun")
scene.add_environment(color=(0.3, 0.35, 0.45), name="sky")
scene.add_camera(prism.Camera.look_at((0, 6, 8), (0, 0, 0), vertical_fov=40), name="cam")
scene.commit()

session = renderer.create_session(scene, device="cpu", width=args.size, height=args.size, camera="cam",
                                  samples=args.spp, aovs=["color", "primId", "depth"])
params = scene.params

for n in range(args.samples):
    record = {"index": n, "objects": []}
    for i, (instance, material, shape) in enumerate(objects):
        scale = rng.uniform(0.3, 0.8)
        position = (rng.uniform(-3, 3), scale, rng.uniform(-3, 3))
        yaw = rng.uniform(0, 360)
        color = rng.uniform(0.05, 0.95, 3)
        visible = bool(rng.random() > 0.15)
        params[f"{instance}.transform"] = prism.scale(scale) @ prism.rotate((0, 1, 0), yaw) @ prism.translate(*position)
        params[f"{instance}.visible"] = visible
        params[f"{material}.color"] = tuple(color)
        record["objects"].append({"id": i + 1, "class": shape, "visible": visible, "position": position,
                                  "scale": scale, "yaw": yaw, "color": color.tolist()})
    elevation, azimuth = math.radians(rng.uniform(20, 70)), math.radians(rng.uniform(0, 360))
    sun_direction = (math.cos(elevation) * math.cos(azimuth), math.sin(elevation), math.cos(elevation) * math.sin(azimuth))
    params["sun.transform"] = prism.look_at(sun_direction, (0, 0, 0))
    params["sun.intensity"] = float(rng.uniform(1.5, 4.0))
    distance = rng.uniform(7, 11)
    eye = (distance * math.sin(azimuth + 1), rng.uniform(3, 7), distance * math.cos(azimuth + 1))
    scene.update_camera("cam", camera_to_world=prism.look_at(eye, (0, 0, 0)))
    params.update()                                           # incremental: transforms, materials, light
    record["camera"] = {"eye": eye}

    result = session.render(seed=n)
    prism.io.write(out / f"{n:05d}.png", result.aovs["color"])
    mask = np.asarray(result.aovs["primId"])[..., 0]         # 0..N object ids, -1 for the background
    np.save(out / f"{n:05d}_mask.npy", np.where(mask > 0, mask, 0).astype(np.uint8))
    np.save(out / f"{n:05d}_depth.npy", np.asarray(result.aovs["depth"])[..., 0])
    record["visible_ids"] = sorted(int(v) for v in np.unique(mask) if v > 0)
    (out / f"{n:05d}.json").write_text(json.dumps(record, indent=1))
    print(f"sample {n + 1}/{args.samples}: {len(record['visible_ids'])} objects in view, "
          f"{result.render_seconds:.2f} s")

# With PyTorch installed, images go to tensors without a copy:
#   torch.from_dlpack(result.aovs["color"])  ->  (H, W, 3) float32 tensor on the CPU
