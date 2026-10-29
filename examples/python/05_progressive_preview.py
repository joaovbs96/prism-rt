"""Progressive rendering with live progress, an ETA and periodic previews,
stopping at a time budget or at convergence (adaptive sampling).
Ctrl-C stops the render cleanly.

    python 05_progressive_preview.py assets/scenes/InOneWeekend.usda --budget 30
"""

import argparse
import time

import prismrt as prism

parser = argparse.ArgumentParser()
parser.add_argument("scene")
parser.add_argument("--camera", default=None)
parser.add_argument("--width", type=int, default=960)
parser.add_argument("--height", type=int, default=540)
parser.add_argument("--spp", type=int, default=1024, help="maximum samples per pixel")
parser.add_argument("--threshold", type=float, default=0.01, help="adaptive sampling noise threshold")
parser.add_argument("--budget", type=float, default=60.0, help="seconds before stopping")
parser.add_argument("--preview-every", type=float, default=5.0, help="seconds between preview images")
args = parser.parse_args()

renderer = prism.Renderer()
scene = renderer.create_scene()
loaded = prism.usd.load(scene, args.scene)


def on_commit(p: prism.ProgressInfo) -> bool:
    print(f"\rloading: {p.phase} {p.fraction:5.1%} {p.detail[:50]:50}", end="", flush=True)
    return True


scene.commit(on_commit)
print()

session = renderer.create_session(scene, device="cpu", width=args.width, height=args.height,
                                  camera=args.camera or next(iter(loaded.cameras)), samples=args.spp,
                                  adaptive_threshold=args.threshold, seed=1)


def on_progress(p: prism.ProgressInfo) -> bool:
    eta = "?" if p.estimated_seconds_remaining < 0 else f"{p.estimated_seconds_remaining:5.1f} s"
    print(f"\r{p.samples:7.1f}/{p.target_samples} spp  {p.fraction:6.1%}  "
          f"{p.samples_per_second / 1e6:6.2f} M samples/s  active {p.active_fraction:5.1%}  ETA {eta}   ", end="", flush=True)
    return True                                            # False would cancel


with session:                                              # cancels and joins on exit, also on Ctrl-C
    session.start(on_progress)
    started = last_preview = time.monotonic()
    while True:
        status = session.wait(0.25)
        if status.state != prism.SessionState.Running:
            break
        now = time.monotonic()
        if now - last_preview >= args.preview_every:
            prism.io.write("preview.png", session.aov("color"))   # snapshot of the last finished pass
            last_preview = now
        if now - started >= args.budget:
            session.cancel()
            status = session.wait()
            break
print()
print(f"{status.state.name} after {status.elapsed_seconds:.1f} s with {status.samples} samples per pixel")
prism.io.write("final.exr", session.aov("color"))
prism.io.write("final.png", session.aov("color"))
