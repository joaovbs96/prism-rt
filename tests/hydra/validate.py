"""Run from an environment initialized by prism_env.ps1 (or prism_env.sh); no Python USD
package needed."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

EXE = ".exe" if sys.platform == "win32" else ""


def standalone_path(build, config):
    """PrismRT in multi-config (Visual Studio) or single-config (Ninja) build layouts."""
    multi = build / config / ("PrismRT" + EXE)
    return multi if multi.exists() else build / ("PrismRT" + EXE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--spp", type=int, default=256)
    parser.add_argument("--texture-filtering", choices=["filtered", "bilinear"], default="filtered")
    parser.add_argument("--standalone", type=Path,
                        help="override the standalone executable (for installed-app validation)")
    parser.add_argument("--hydra", type=Path,
                        help="override the Hydra render helper (for installed-plugin validation)")
    parser.add_argument("--compare", type=Path,
                        help="override the image comparison helper")
    parser.add_argument("--device", choices=["auto", "cpu", "vulkan", "metal"],
                        help="render device of both frontends (default: their auto)")
    args = parser.parse_args()
    env = dict(os.environ)
    if args.device:
        env["PRISM_DEVICE"] = args.device
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    out = build / ("hydra-validation-" + args.texture_filtering)
    out.mkdir(parents=True, exist_ok=True)
    fixture = root / "tests/hydra/fixtures/parity.usda"
    preview = fixture.with_name("preview_parity.usda")
    preview_mtlx = fixture.with_name("preview_mtlx_parity.usda")
    standalone = args.standalone.resolve() if args.standalone else standalone_path(build, args.config)
    hydra = args.hydra.resolve() if args.hydra else build / ("hydra/plugin/prism_hydra_render" + EXE)
    compare = args.compare.resolve() if args.compare else build / ("hydra/plugin/prism_compare_images" + EXE)
    results = []

    def run(name, command):
        if command[0] in (standalone, hydra):
            command = [*command, "--texture-filtering", args.texture_filtering]
        if command[0] == standalone and args.device:
            command = [*command, "--device", args.device]
        start = time.monotonic()
        with (out / (name + ".log")).open("w", encoding="utf-8") as log:
            proc = subprocess.run([str(x) for x in command], cwd=root, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=600, env=env)
        result = dict(name=name, seconds=time.monotonic() - start, exit_code=proc.returncode)
        results.append(result)
        (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
        if proc.returncode:
            raise RuntimeError(f"{name} failed ({proc.returncode}); see {out / (name + '.log')}")
        print(f"{name}: passed in {result['seconds']:.2f}s", flush=True)

    for rerun in (1, 2):
        run(f"standalone-{rerun}", [standalone, "--scene", fixture,
            "--camera", "/World/Camera", "--env", fixture.with_name("environment.ppm"),
            "--width", "128", "--height", "128", "--spp", args.spp,
            "--seed", "0", "--max-depth", "32", "--rr-min-depth", "4",
            "--max-sample-radiance", "10", "--adaptive-threshold", "0",
            "--output", out / f"standalone-{rerun}.exr"])
        run(f"hydra-{rerun}", [hydra, fixture, "/World/Camera", out / f"hydra-{rerun}.exr",
            "128", "128", args.spp, "--adaptive-threshold", "0"] + (["--exercise"] if rerun == 1 else []))
    for renderer in ("standalone", "hydra"):
        run(renderer + "-determinism", [compare, out / f"{renderer}-1.exr",
            out / f"{renderer}-2.exr", "--exact"])
    run("standalone-hydra-parity", [compare, out / "standalone-1.exr", out / "hydra-1.exr"])
    run("preview-surface", [hydra, preview, "/World/Camera", out / "preview.exr",
        "128", "128", args.spp, "--adaptive-threshold", "0"])
    run("preview-materialx", [hydra, preview_mtlx, "/World/Camera",
        out / "preview-mtlx.exr", "128", "128", args.spp, "--adaptive-threshold", "0"])
    run("preview-materialx-parity", [compare, out / "preview-mtlx.exr",
        out / "preview.exr"])
    print("Validation artifacts:", out)


if __name__ == "__main__":
    main()
