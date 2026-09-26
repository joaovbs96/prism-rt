"""Run from an environment initialized by prism_env.ps1; no Python USD package needed."""
import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--spp", type=int, default=256)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    out = build / "hydra-validation"
    out.mkdir(parents=True, exist_ok=True)
    fixture = root / "tests/hydra/fixtures/parity.usda"
    preview = fixture.with_name("preview_parity.usda")
    preview_mtlx = fixture.with_name("preview_mtlx_parity.usda")
    standalone = build / args.config / "PrismRT.exe"
    hydra = build / "hydra/plugin/prism_hydra_render.exe"
    compare = build / "hydra/plugin/prism_compare_images.exe"
    results = []

    def run(name, command):
        start = time.monotonic()
        with (out / (name + ".log")).open("w", encoding="utf-8") as log:
            proc = subprocess.run([str(x) for x in command], cwd=root, stdout=log,
                                  stderr=subprocess.STDOUT, timeout=600)
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
            "--max-sample-radiance", "10", "--output", out / f"standalone-{rerun}.exr"])
        run(f"hydra-{rerun}", [hydra, fixture, "/World/Camera", out / f"hydra-{rerun}.exr",
            "128", "128", args.spp] + (["--exercise"] if rerun == 1 else []))
    for renderer in ("standalone", "hydra"):
        run(renderer + "-determinism", [compare, out / f"{renderer}-1.exr",
            out / f"{renderer}-2.exr", "--exact"])
    run("standalone-hydra-parity", [compare, out / "standalone-1.exr", out / "hydra-1.exr"])
    run("preview-surface", [hydra, preview, "/World/Camera", out / "preview.exr",
        "128", "128", args.spp])
    run("preview-materialx", [hydra, preview_mtlx, "/World/Camera",
        out / "preview-mtlx.exr", "128", "128", args.spp])
    run("preview-materialx-parity", [compare, out / "preview-mtlx.exr",
        out / "preview.exr"])
    print("Validation artifacts:", out)


if __name__ == "__main__":
    main()
