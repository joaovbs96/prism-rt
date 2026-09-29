"""Independent frontend parity for exported procedural USD scenes.

Use the configured Prism runtime. Image comparisons use the same thresholds as
validate.py; no sample clamp hides energy discrepancies. Loading is timed but
this script does not claim a cold filesystem-cache benchmark.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--scene", action="append", choices=["InOneWeekend", "CornellBox", "QuadTest"])
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--texture-filtering", choices=["filtered", "bilinear"], default="filtered")
    parser.add_argument("--inoneweekend-spp", type=int, default=1024,
                        help="higher sample count for comparing independently ingested spheres")
    parser.add_argument("--standalone", type=Path,
                        help="override the standalone executable (for installed-app validation)")
    parser.add_argument("--hydra", type=Path,
                        help="override the Hydra render helper (for installed-plugin validation)")
    parser.add_argument("--compare", type=Path,
                        help="override the image comparison helper")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    output = build / ("export-validation-" + args.texture_filtering)
    output.mkdir(parents=True, exist_ok=True)
    standalone = args.standalone.resolve() if args.standalone else build / args.config / "PrismRT.exe"
    hydra = args.hydra.resolve() if args.hydra else build / "hydra/plugin/prism_hydra_render.exe"
    compare = args.compare.resolve() if args.compare else build / "hydra/plugin/prism_compare_images.exe"
    results = []

    def run(name, command):
        if command[0] in (standalone, hydra) and "--export-scene" not in command:
            command = [*command, "--texture-filtering", args.texture_filtering]
        start = time.perf_counter()
        with (output / f"{name}.log").open("w", encoding="utf-8") as log:
            result = subprocess.run([str(x) for x in command], cwd=root,
                                    stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        results.append(dict(name=name, seconds=time.perf_counter()-start, exitCode=result.returncode))
        (output / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"{name} failed: {output / (name + '.log')}")
        print(f"{name}: passed ({results[-1]['seconds']:.2f}s)", flush=True)

    for name in args.scene or ["InOneWeekend", "CornellBox", "QuadTest"]:
        samples = args.inoneweekend_spp if name == "InOneWeekend" else args.spp
        scene = output / f"{name}.usda"
        run(f"{name}-export", [standalone, "--export-scene", name, "--seed", 0, "--output", scene])
        for repetition in (1, 2):
            run(f"{name}-standalone-{repetition}", [standalone, "--scene", scene,
                "--camera", "/World/Camera", "--width", 128, "--height", 128,
                "--spp", samples, "--max-sample-radiance", 0, "--seed", 0,
                "--output", output / f"{name}-standalone-{repetition}.exr"])
            run(f"{name}-hydra-{repetition}", [hydra, scene, "/World/Camera",
                output / f"{name}-hydra-{repetition}.exr", 128, 128, samples,
                "--sample-clamp", 0])
        for renderer in ("standalone", "hydra"):
            run(f"{name}-{renderer}-determinism", [compare,
                output / f"{name}-{renderer}-1.exr", output / f"{name}-{renderer}-2.exr", "--exact"])
        run(f"{name}-parity", [compare, output / f"{name}-standalone-1.exr",
                              output / f"{name}-hydra-1.exr"])


if __name__ == "__main__":
    main()
