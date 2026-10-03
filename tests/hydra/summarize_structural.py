#!/usr/bin/env python3
"""Summarize completed structural-scaling usdview probes (standard library only)."""

import argparse
import json
import math
import statistics
from collections import defaultdict
from pathlib import Path


def finite_number(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def median(values):
    values = list(values)
    return statistics.median(values) if values else None


def numeric_field_medians(samples):
    fields = defaultdict(list)
    for sample in samples:
        if not isinstance(sample, dict):
            continue
        for name, value in sample.items():
            if finite_number(value):
                fields[name].append(value)
    return {name: statistics.median(values)
            for name, values in sorted(fields.items())}


def summarize_operation(samples):
    elapsed = [row["elapsedSeconds"] for row in samples
               if finite_number(row.get("elapsedSeconds"))]
    cpu = [row["processCpuSeconds"] for row in samples
           if finite_number(row.get("processCpuSeconds"))]
    stats_time = [row["statsQuerySeconds"] for row in samples
                  if finite_number(row.get("statsQuerySeconds"))]
    after = [row.get("after", {}) for row in samples]
    differences = [row.get("numericDifference", {}) for row in samples]
    return {
        "count": len(samples),
        "elapsedSeconds": {
            "median": median(elapsed),
            "min": min(elapsed) if elapsed else None,
            "max": max(elapsed) if elapsed else None,
        },
        "processCpuSecondsMedian": median(cpu),
        "statsQuerySecondsMedian": median(stats_time),
        "hostTreeResetSecondsMedian": median(
            row["hostTreeResetSeconds"] for row in samples
            if finite_number(row.get("hostTreeResetSeconds"))),
        "workingSetBytesMedian": median(
            row["workingSetBytes"] for row in samples
            if finite_number(row.get("workingSetBytes"))),
        "peakWorkingSetBytesMedian": median(
            row["peakWorkingSetBytes"] for row in samples
            if finite_number(row.get("peakWorkingSetBytes"))),
        "afterNumericMedians": numeric_field_medians(after),
        "gaugeDifferenceMedians": numeric_field_medians(differences),
        "gaugeDifferenceInterpretation": (
            "Medians of callback numericDifference values (after minus before); "
            "these are gauge differences, not complete work counters."),
    }


def group_sort_key(item):
    axis, value = item
    try:
        numeric_value = float(value)
    except (TypeError, ValueError):
        numeric_value = math.inf
    return str(axis), numeric_value, str(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, help="structural_scaling.py results.json")
    parser.add_argument("--output", type=Path,
                        help="summary JSON path (default: <input-stem>_summary.json)")
    args = parser.parse_args()

    input_path = args.results.resolve()
    try:
        source = json.loads(input_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        parser.error(f"cannot read {input_path}: {error}")

    operation_groups = defaultdict(lambda: defaultdict(list))
    group_success_counts = defaultdict(int)
    failed_runs = []
    malformed_runs = []

    for run in source.get("runs", []):
        case = run.get("case") or {}
        key = (case.get("axis", run.get("axis", "unknown")),
               case.get("value", run.get("value", "unknown")))
        if run.get("exitCode") != 0:
            failed_runs.append({
                "axis": key[0], "value": key[1],
                "repeat": run.get("repeat"), "exitCode": run.get("exitCode"),
                "processElapsedSeconds": run.get("processElapsedSeconds"),
                "log": run.get("log"), "validation": run.get("validation"),
                "scene": run.get("scene"),
            })
            continue

        probe = run.get("probe")
        operations = probe.get("operations") if isinstance(probe, dict) else None
        if not isinstance(operations, list) or not operations:
            malformed_runs.append({
                "axis": key[0], "value": key[1],
                "repeat": run.get("repeat"), "reason": "successful process has no probe operations",
                "log": run.get("log"), "validation": run.get("validation"),
            })
            continue

        group_success_counts[key] += 1
        for operation in operations:
            name = operation.get("operation")
            if not isinstance(name, str) or not name:
                malformed_runs.append({
                    "axis": key[0], "value": key[1],
                    "repeat": run.get("repeat"), "reason": "probe operation has no name",
                    "log": run.get("log"),
                })
                continue
            operation_groups[key][name].append(operation)

    groups = []
    for key in sorted(set(operation_groups) | set(group_success_counts), key=group_sort_key):
        operations = operation_groups.get(key, {})
        groups.append({
            "axis": key[0], "value": key[1],
            "successfulRunCount": group_success_counts[key],
            "operations": {name: summarize_operation(samples)
                           for name, samples in sorted(operations.items())},
        })

    summary = {
        "source": str(input_path),
        "settings": source.get("settings", {}),
        "caseCount": len(source.get("cases", [])),
        "successfulRunCount": sum(group_success_counts.values()),
        "failedRuns": failed_runs,
        "malformedRuns": malformed_runs,
        "groups": groups,
        "notes": [
            "Operation afterNumericMedians are the callback's post-operation gauges.",
            "gaugeDifferenceMedians are after-minus-before gauge differences, not work totals.",
        ],
    }
    output_path = (args.output.resolve() if args.output else
                   input_path.with_name(input_path.stem + "_summary.json"))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n",
                           encoding="utf-8")

    operations_to_show = (
        "cold", "recreate", "transform-1", "material-parameter",
        "structural-rename-control", "structural-restore-control-name",
        "one-prototype-points", "one-point-instance-position",
        "instancer-count-add", "instancer-count-remove",
    )
    columns = ("axis", "value", "n") + tuple(f"{name}_s_med" for name in operations_to_show)
    print("\t".join(columns))
    for group in groups:
        row = [str(group["axis"]), str(group["value"]),
               str(group["successfulRunCount"])]
        for name in operations_to_show:
            result = group["operations"].get(name)
            value = (result or {}).get("elapsedSeconds", {}).get("median")
            row.append(f"{value:.3f}" if value is not None else "-")
        print("\t".join(row))
    work_fields = (
        "structuralLeafUpdates", "structuralFallbacks",
        "lastStructuralPathsUpdated", "lastStructuralPlacementsAttached",
        "lastStructuralPlacementsDetached", "capturedPrims",
        "lastPlacementRecordsVisited", "lastPlacementTransformsUpdated",
        "lastPrototypePathsUpdated", "lastPrototypePlacementsRepointed",
        "prototypeBuildCount", "snapshotBuilds", "topLevelSceneBuilds",
        "topLevelSceneCommits", "placementTopologyFallbacks",
        "prototypeUpdateFallbacks",
    )
    for group in groups:
        for name in ("structural-control", "structural-after-material",
                     "structural-remove-control", "structural-remove-after-material"):
            operation = group["operations"].get(name)
            if not operation:
                continue
            medians = operation["afterNumericMedians"]
            values = {field: medians[field] for field in work_fields if field in medians}
            print("work\t{}\t{}\t{}\t{}".format(
                group["axis"], group["value"], name,
                json.dumps(values, sort_keys=True, separators=(",", ":"))))
    for group in groups:
        if group["axis"] != "placements":
            continue
        for name in ("one-prototype-points", "one-point-instance-position",
                     "instancer-count-add", "instancer-count-remove"):
            operation = group["operations"].get(name)
            if not operation:
                continue
            medians = operation["afterNumericMedians"]
            values = {field: medians[field] for field in work_fields if field in medians}
            print("work\t{}\t{}\t{}\t{}".format(
                group["axis"], group["value"], name,
                json.dumps(values, sort_keys=True, separators=(",", ":"))))
    print(f"summary_json\t{output_path}")
    print(f"failed_runs\t{len(failed_runs)}\tmalformed_runs\t{len(malformed_runs)}")


if __name__ == "__main__":
    main()
