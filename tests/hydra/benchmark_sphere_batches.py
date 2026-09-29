#!/usr/bin/env python3
"""Compare ordinary and opt-in batched analytic spheres in fresh processes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def cache_state(path):
    files = [item for item in path.rglob("*") if item.is_file()] if path.exists() else []
    return {"files": len(files), "bytes": sum(item.stat().st_size for item in files)}


def run_process(executable, scene, output, cache, mode, repeat, filtering):
    image = output / f"iwo-{mode}-{repeat}.exr"
    log = output / f"iwo-{mode}-{repeat}.log"
    command = [str(executable), str(scene), "/World/Camera", str(image),
               "256", "144", "32", "--seed", "0", "--sample-clamp", "0",
               "--texture-filtering", filtering]
    if mode == "batched":
        command.append("--sphere-batching")
    before = cache_state(cache)
    env = os.environ.copy()
    env["PRISM_CACHE_DIR"] = str(cache)
    started = time.perf_counter()
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.run(command, cwd=scene.parents[2], env=env,
                                 stdout=stream, stderr=subprocess.STDOUT,
                                 timeout=900)
    elapsed = time.perf_counter() - started
    after = cache_state(cache)
    log_text = log.read_text(encoding="utf-8", errors="replace")
    render_stats = {name: value.strip() for name, value in re.findall(
        r"^Render stat ([^:]+): (.*)$", log_text, re.MULTILINE)}
    result = {"mode": mode, "repeat": repeat, "command": command,
              "wallSeconds": elapsed, "exitCode": process.returncode,
              "image": str(image), "log": str(log), "cache": str(cache),
              "cacheBefore": before, "cacheAfter": after,
              "renderStats": render_stats}
    if image.is_file():
        result.update(imageBytes=image.stat().st_size, imageSha256=file_hash(image))
    return result


def compare(compare_executable, reference, actual, exact, log):
    command = [str(compare_executable), str(reference), str(actual)]
    if exact:
        command.append("--exact")
    process = subprocess.run(command, capture_output=True, text=True, timeout=120)
    text = process.stdout + process.stderr
    log.write_text(text, encoding="utf-8")
    match = re.search(
        r"Normalized RMSE: ([0-9.eE+-]+); relative mean luminance error: ([0-9.eE+-]+)",
        text)
    return {"command": command, "exitCode": process.returncode,
            "exactRequested": exact, "normalizedRmse": float(match.group(1)) if match else None,
            "relativeMeanLuminanceError": float(match.group(2)) if match else None,
            "log": str(log)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hydra", type=Path,
                        default=Path("build/hydra/plugin/prism_hydra_render.exe"))
    parser.add_argument("--compare", type=Path,
                        default=Path("build/hydra/plugin/prism_compare_images.exe"))
    parser.add_argument("--scene", type=Path,
                        default=Path("assets/scenes/InOneWeekend.usda"))
    parser.add_argument("--output", type=Path,
                        default=Path("build/sphere-batch-comparison"))
    parser.add_argument("--texture-filtering", choices=("filtered", "bilinear"),
                        default="filtered")
    parser.add_argument("--prepared-cache", choices=("empty", "reuse"),
                        default="empty",
                        help="empty per-process renderer caches or one shared persistent cache")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    hydra = (root / args.hydra).resolve() if not args.hydra.is_absolute() else args.hydra
    comparator = ((root / args.compare).resolve() if not args.compare.is_absolute()
                  else args.compare)
    scene = (root / args.scene).resolve() if not args.scene.is_absolute() else args.scene
    output = (root / args.output).resolve() if not args.output.is_absolute() else args.output
    for path, label in ((hydra, "Hydra helper"), (comparator, "image comparator"),
                        (scene, "scene")):
        if not path.is_file():
            parser.error(f"{label} does not exist: {path}")
    plugin = hydra.with_name("hdPrism.dll")
    if not plugin.is_file():
        parser.error(f"Hydra plugin does not exist beside helper: {plugin}")
    output.mkdir(parents=True, exist_ok=True)
    report_path = output / "results.json"
    report = {"settings": {"scene": str(scene), "width": 256, "height": 144,
        "samplesPerPixel": 32, "seed": 0, "sampleClamp": 0,
        "textureFiltering": args.texture_filtering, "repeats": 3,
        "processModel": "each mode/repeat is a fresh process; modes alternate first",
        "preparedCachePolicy": args.prepared_cache,
        "coldDefinition": "a distinct initially empty PRISM_CACHE_DIR per process; OS file cache is not cleared",
        "reuseDefinition": "one persistent PRISM_CACHE_DIR shared by all processes; OS file cache is not cleared",
        "hydraExecutable": str(hydra), "hydraSha256": file_hash(hydra),
        "pluginBinary": str(plugin), "pluginSha256": file_hash(plugin)},
        "runs": [], "comparisons": [], "summary": {}}
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    shared_cache = output / "shared-renderer-cache"
    for repeat in range(1, 4):
        order = ("ordinary", "batched") if repeat % 2 else ("batched", "ordinary")
        for mode in order:
            cache = (output / f"cache-{mode}-{repeat}" if args.prepared_cache == "empty"
                     else shared_cache)
            if args.prepared_cache == "empty" and cache_state(cache)["files"]:
                raise RuntimeError(f"cold cache is not empty: {cache}; choose a new output")
            cache.mkdir(parents=True, exist_ok=True)
            run = run_process(
                hydra, scene, output, cache.resolve(), mode, repeat,
                args.texture_filtering)
            report["runs"].append(run)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
            if run["exitCode"]:
                raise RuntimeError(f"{mode} repeat {repeat} failed; see {run['log']}")

    images = {(run["mode"], run["repeat"]): Path(run["image"])
              for run in report["runs"]}
    for mode in ("ordinary", "batched"):
        for repeat in (2, 3):
            result = compare(comparator, images[(mode, 1)], images[(mode, repeat)],
                True, output / f"compare-{mode}-1-{repeat}-exact.log")
            result.update(kind="withinModeDeterminism", mode=mode, repeat=repeat)
            report["comparisons"].append(result)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
            if result["exitCode"]:
                raise RuntimeError(f"{mode} output is not deterministic at repeat {repeat}")
    for repeat in (1, 2, 3):
        exact = compare(comparator, images[("ordinary", repeat)],
            images[("batched", repeat)], True,
            output / f"compare-cross-{repeat}-exact.log")
        exact.update(kind="crossModeExactProbe", repeat=repeat)
        report["comparisons"].append(exact)
        tolerance = compare(comparator, images[("ordinary", repeat)],
            images[("batched", repeat)], False,
            output / f"compare-cross-{repeat}-one-percent.log")
        tolerance.update(kind="crossModeOnePercent", repeat=repeat)
        report["comparisons"].append(tolerance)
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        if tolerance["exitCode"]:
            raise RuntimeError(f"cross-mode 1% comparison failed at repeat {repeat}")
    timings = {mode: [run["wallSeconds"] for run in report["runs"]
                      if run["mode"] == mode] for mode in ("ordinary", "batched")}
    report["summary"] = {"runCount": len(report["runs"]),
        "successfulRunCount": sum(run["exitCode"] == 0 for run in report["runs"]),
        "ordinaryMedianWallSeconds": statistics.median(timings["ordinary"]),
        "batchedMedianWallSeconds": statistics.median(timings["batched"]),
        "crossModeExactPasses": sum(item["exitCode"] == 0 for item in report["comparisons"]
                                    if item["kind"] == "crossModeExactProbe"),
        "crossModeOnePercentPasses": sum(item["exitCode"] == 0 for item in report["comparisons"]
                                         if item["kind"] == "crossModeOnePercent")}
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Results: {report_path}")


if __name__ == "__main__":
    main()
