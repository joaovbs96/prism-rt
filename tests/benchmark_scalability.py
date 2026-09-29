#!/usr/bin/env python3
"""Run fresh-process texture-cache pressure measurements and save JSON."""

import argparse
import concurrent.futures
import json
import statistics
import subprocess
import time
from pathlib import Path


def run_one(executable, fixtures, cache, samples, cache_mb, log_path):
    command = [str(executable), "--fixtures", str(fixtures), "--cache", str(cache),
               "--samples", str(samples), "--cache-mb", str(cache_mb)]
    started = time.perf_counter()
    process = subprocess.run(command, capture_output=True, text=True)
    elapsed = time.perf_counter() - started
    log_path.write_text(process.stdout + process.stderr, encoding="utf-8")
    result_lines = [line.removeprefix("RESULT_JSON: ")
                    for line in process.stdout.splitlines()
                    if line.startswith("RESULT_JSON: ")]
    if process.returncode or len(result_lines) != 1:
        raise RuntimeError(f"benchmark failed ({process.returncode}); see {log_path}")
    result = json.loads(result_lines[0])
    result["processWallSeconds"] = elapsed
    result["exitCode"] = process.returncode
    result["command"] = command
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True,
                        help="path to the prism_texture_cache_benchmark executable")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--samples", type=int, default=30000)
    parser.add_argument("--cache-mb", type=int, default=32)
    parser.add_argument("--parity-only", action="store_true",
                        help="run two matched fresh-process filtered/bilinear checksum checks")
    args = parser.parse_args()
    executable = args.executable.resolve()
    if not executable.is_file():
        parser.error(f"benchmark executable does not exist: {executable}")
    if args.samples <= 0 or args.cache_mb <= 0:
        parser.error("samples and cache-mb must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    fixtures = (args.output / "fixtures").resolve()
    warm_cache = (args.output / "prepared-cache-warm").resolve()
    report = {
        "settings": {
            "textureCount": 20,
            "textureSize": 512,
            "channels": 4,
            "sourceFloatMiB": 80,
            "textureCacheBudgetMiB": args.cache_mb,
            "samplesPerProcess": args.samples,
            "repeats": 3,
            "processModel": "each recorded repeat uses a fresh process",
            "coldDefinition": "empty prepared .tx cache directory; OS file cache is not cleared",
            "warmDefinition": "prepared .tx files prepopulated by an unrecorded process",
        },
        "runs": {"coldPrepared": [], "warmPrepared": []},
        "summary": {},
    }
    report_path = args.output / "results.json"

    prewarm = run_one(executable, fixtures, warm_cache, args.samples,
                      args.cache_mb, args.output / "warm-preparation.log")
    report["warmCachePreparation"] = prewarm
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    if args.parity_only:
        parity_cache = (args.output / "prepared-cache-parity").resolve()
        parity_cache.mkdir(parents=True, exist_ok=True)
        parity_runs = [
            run_one(executable, fixtures, parity_cache, args.samples, args.cache_mb,
                    args.output / f"parity-{index}.log")
            for index in range(1, 3)
        ]
        checksum_fields = ("filteredChecksum", "bilinearChecksum")
        if (not all(run["finite"] and run["dedupeIdentity"] for run in parity_runs) or
                any(parity_runs[0][field] != parity_runs[1][field]
                    for field in checksum_fields)):
            raise RuntimeError("matched filtered/bilinear parity check failed")
        report["matchedFilteredBilinearParity"] = {
            "sampleCount": args.samples,
            "processes": parity_runs,
            "matchedSettings": True,
            "checksumsStable": True,
            "result": "passed",
        }
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"Results: {report_path}", flush=True)
        return

    # Both processes prepare identical sources into one new cache directory.
    # The helper validates each published .tx by opening it through OIIO.
    publication_cache = (args.output / "prepared-cache-concurrent").resolve()
    publication_cache.mkdir(parents=True, exist_ok=True)
    publication_jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for process_index in range(1, 3):
            publication_jobs.append(pool.submit(
                run_one, executable, fixtures, publication_cache, 100,
                args.cache_mb, args.output / f"publication-{process_index}.log"))
        publication_results = [job.result() for job in publication_jobs]
    tx_files = sorted(publication_cache.glob("*.tx"))
    temporary_files = [path for path in publication_cache.iterdir()
                       if path.name.endswith(".tmp.tx")]
    if (len(tx_files) != 20 or temporary_files or
            any(path.stat().st_size == 0 for path in tx_files) or
            any(result["filteredChecksum"] != publication_results[0]["filteredChecksum"]
                for result in publication_results)):
        raise RuntimeError("concurrent cache publication produced incomplete or inconsistent output")
    report["multiProcessPublication"] = {
        "processes": publication_results,
        "preparedTextureFiles": len(tx_files),
        "temporaryFilesRemaining": len(temporary_files),
        "sameFilteredChecksum": True,
        "result": "passed",
    }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    for group in ("coldPrepared", "warmPrepared"):
        for repeat in range(1, 4):
            cache = (args.output / f"prepared-cache-cold-{repeat}").resolve() \
                if group == "coldPrepared" else warm_cache
            log = args.output / f"{group}-{repeat}.log"
            print(f"Running {group} repeat {repeat}", flush=True)
            result = run_one(executable, fixtures, cache, args.samples,
                             args.cache_mb, log)
            result["repeat"] = repeat
            report["runs"][group].append(result)
            report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")

    for field in ("filteredChecksum", "bilinearChecksum"):
        values = [run[field] for group in report["runs"].values() for run in group]
        if len(set(values)) != 1:
            raise RuntimeError(f"nondeterministic {field}; see {report_path}")
    for group, runs in report["runs"].items():
        report["summary"][group] = {
            "preparationSecondsMedian": statistics.median(
                run["preparationSeconds"] for run in runs),
            "lookupSecondsMedian": statistics.median(
                run["lookupSeconds"] for run in runs),
            "processWallSecondsMedian": statistics.median(
                run["processWallSeconds"] for run in runs),
            "diskCacheHits": [run["serviceStats"]["diskCacheHits"] for run in runs],
            "memoryCacheHits": [run["serviceStats"]["memoryCacheHits"] for run in runs],
            "filteredChecksum": runs[0]["filteredChecksum"],
            "bilinearChecksum": runs[0]["bilinearChecksum"],
        }
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"Results: {report_path}", flush=True)


if __name__ == "__main__":
    main()
