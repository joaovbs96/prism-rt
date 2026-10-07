"""Summarize alternating usdview_refinement runs without third-party packages.

Run directories are named <resolution>-<iteration>-<baseline|tiled|candidate>.
Pass the preserved binaries actually used by the runs, not a later rebuild.
Paint and update-gap percentiles include both partial and completed frames.
"""
import argparse
import hashlib
import json
import statistics
from pathlib import Path


def percentile(values, fraction):
    values = sorted(values)
    if not values:
        return None
    position = (len(values) - 1) * fraction
    lower = int(position)
    return values[lower] + (values[min(lower + 1, len(values) - 1)] - values[lower]) * (position - lower)


def summarize(path):
    data = json.loads(path.read_text())
    resolution, iteration, variant = path.parent.name.rsplit("-", 2)
    completed = [row for row in data["sampleChanges"] if row["stats"]["completedSamples"] > 0]
    if not completed or not data["paints"]:
        raise ValueError(f"Missing completed sample or paint measurements: {path}")
    updates = []
    previous = None
    for paint in data["paints"]:
        key = (paint["samples"], paint["refinementPublishes"])
        if key != previous:
            updates.append(paint["time"])
            previous = key
    first, last = completed[0], completed[-1]
    stats = last["stats"]
    duration = data["paints"][-1]["time"]
    return {
        "resolution": resolution, "iteration": int(iteration), "variant": variant,
        "firstSampleSeconds": first["time"],
        "completedSamples": stats["completedSamples"],
        "observationSeconds": duration,
        "samplesPerSecond": stats["completedSamples"] / duration,
        "paintP95Ms": percentile([p["paintMs"] for p in data["paints"]], .95),
        "updateGapP95Seconds": percentile([b - a for a, b in zip(updates, updates[1:])], .95),
        "cancellationMs": data.get("settingCancellationMs"),
        "firstPassTimings": {k: v for k, v in first["stats"].items() if k.startswith("lastPass")},
        "counters": {k: v for k, v in stats.items() if
                     k.endswith("Bytes") or k.startswith("presentation") or
                     k in ("refinementPublishes", "bufferMapWaitMilliseconds", "bufferPublicationMilliseconds")},
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    for variant in ("baseline", "tiled", "candidate"):
        parser.add_argument("--" + variant, required=True, type=Path)
    parser.add_argument("--required-runs", type=int, default=5)
    args = parser.parse_args()
    runs = [summarize(path) for path in sorted(args.root.glob("*/cadence.json"))]
    metrics = ("firstSampleSeconds", "samplesPerSecond", "paintP95Ms", "updateGapP95Seconds", "cancellationMs")
    medians, gates = {}, {}
    for resolution in sorted({run["resolution"] for run in runs}):
        groups = {}
        for variant in ("baseline", "tiled", "candidate"):
            group = [run for run in runs if run["resolution"] == resolution and run["variant"] == variant]
            values = {metric: statistics.median(run[metric] for run in group if run[metric] is not None)
                      for metric in metrics if any(run[metric] is not None for run in group)}
            values["runs"] = len(group)
            groups[variant] = values
        medians[resolution] = groups
        if any(group["runs"] < args.required_runs for group in groups.values()):
            gates[resolution] = {"status": "incomplete"}
            continue
        baseline, tiled, candidate = (groups[v] for v in ("baseline", "tiled", "candidate"))
        checks = {
            "firstSample": candidate["firstSampleSeconds"] <= baseline["firstSampleSeconds"] * 1.05,
            "throughput": candidate["samplesPerSecond"] >= baseline["samplesPerSecond"] * .95,
            "paint": candidate["paintP95Ms"] <= tiled["paintP95Ms"] * 1.10,
            "updateGaps": candidate["updateGapP95Seconds"] <= tiled["updateGapP95Seconds"] * 1.10,
        }
        gates[resolution] = {"status": "pass" if all(checks.values()) else "not met", "checks": checks}
    report = {
        "binaryHashes": {v: hashlib.sha256(getattr(args, v).read_bytes()).hexdigest()
                         for v in ("baseline", "tiled", "candidate")},
        "runs": runs, "medians": medians, "gates": gates,
        "measurementNote": "Observed host paint completion times; interactive-machine background load is uncontrolled."
    }
    (args.root / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"medians": medians, "gates": gates}, indent=2))


if __name__ == "__main__":
    main()
