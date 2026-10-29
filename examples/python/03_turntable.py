"""A turntable: orbit the camera around a USD asset and write numbered frames.
Camera edits need no commit, so the scene is converted and compiled once.

    python 03_turntable.py assets/scenes/InOneWeekend.usda --frames 24
    ffmpeg -framerate 24 -i turntable_%03d.png turntable.mp4     # optional
"""

import argparse
import math

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("scene")
parser.add_argument("--frames", type=int, default=36)
parser.add_argument("--width", type=int, default=480)
parser.add_argument("--height", type=int, default=480)
parser.add_argument("--spp", type=int, default=32)
parser.add_argument("--radius", type=float, default=None,
                    help="distance from the camera to the orbit centre (default: what the centre pixel hits)")
args = parser.parse_args()

renderer = prism.Renderer()
scene = renderer.create_scene()
loaded = prism.usd.load(scene, args.scene)
scene.commit()

# Orbit around the point the file's camera looks at: one tiny render of the
# camera-depth AOV gives the distance to whatever is under the centre pixel.
start_id = next(iter(loaded.cameras))
start = scene.camera(start_id)
camera_to_world = prism.Camera.transform(start)                          # a 4x4 NumPy array
eye = camera_to_world[3, :3]
forward = -camera_to_world[2, :3] / np.linalg.norm(camera_to_world[2, :3])    # cameras look down -Z
probe = renderer.create_session(scene, device="cpu", width=33, height=33, camera=start_id, samples=1,
                                aovs=["color", "cameraDepth", "primId"]).render()
hit = np.asarray(probe.aovs["primId"])[16, 16, 0] >= 0
distance = float(np.asarray(probe.aov("cameraDepth"))[16, 16, 0]) if hit else 10.0
target = eye + forward * (args.radius or distance)
height = eye[1]

camera = scene.add_camera(prism.Camera.from_matrix(camera_to_world, focal_length=start.focal_length,
                                                   vertical_aperture=start.vertical_aperture,
                                                   horizontal_aperture=start.horizontal_aperture),
                          name="orbit")
session = renderer.create_session(scene, device="cpu", width=args.width, height=args.height, camera=camera,
                                  samples=args.spp, seed=1)

for frame in range(args.frames):
    angle = 2 * math.pi * frame / args.frames
    offset = (eye - target)
    rotated = (offset[0] * math.cos(angle) - offset[2] * math.sin(angle), 0.0,
               offset[0] * math.sin(angle) + offset[2] * math.cos(angle))
    position = (target[0] + rotated[0], height, target[2] + rotated[2])
    scene.update_camera("orbit", camera_to_world=prism.look_at(position, target))
    session.set_camera("orbit")
    result = session.render()
    prism.io.write(f"turntable_{frame:03d}.png", result.aovs["color"])
    print(f"frame {frame + 1}/{args.frames}: {result.render_seconds:.2f} s")
