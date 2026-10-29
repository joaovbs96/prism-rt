"""Measure noise against samples per pixel: render a high-spp reference, then
increasing sample counts with different seeds, and fit the error slope
(Monte Carlo: RMSE ~ spp^-0.5). Handy to compare settings or devices.

    python 10_convergence_study.py assets/scenes/CornellBox.usda --camera /World/Camera
"""

import argparse

import numpy as np
import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("scene")
parser.add_argument("--camera", required=True)
parser.add_argument("--size", type=int, default=128)
parser.add_argument("--reference-spp", type=int, default=2048)
parser.add_argument("--device", default="cpu")
args = parser.parse_args()

renderer = prism.Renderer()
scene = renderer.create_scene()
prism.usd.load(scene, args.scene)
scene.commit()
# No adaptive sampling: every pixel gets exactly the requested samples.
session = renderer.create_session(scene, device=args.device, width=args.size, height=args.size,
                                  camera=args.camera, adaptive_threshold=0.0)

reference = np.asarray(session.render(samples=args.reference_spp, seed=12345).aovs["color"]).copy()
print(f"reference: {args.reference_spp} spp")

spps, errors = [], []
for spp in (1, 2, 4, 8, 16, 32, 64, 128):
    rmse = []
    for seed in range(3):                                         # a few independent runs per count
        image = np.asarray(session.render(samples=spp, seed=seed).aovs["color"])
        rmse.append(float(np.sqrt(np.mean((image - reference) ** 2))))
    spps.append(spp)
    errors.append(np.mean(rmse))
    print(f"{spp:4} spp: RMSE {errors[-1]:.5f}")

slope = np.polyfit(np.log(spps), np.log(errors), 1)[0]
print(f"error slope {slope:.2f} (ideal Monte Carlo: -0.50; flatter means bias or fireflies dominate)")
