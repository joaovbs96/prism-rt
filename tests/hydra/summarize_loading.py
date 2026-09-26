"""Summarize usdview loading benchmark runs without conflating cache states."""
import argparse
import json
from pathlib import Path
import statistics
from collections import defaultdict


METRICS = {
    "snapshotBuildSeconds": "Snapshot build (s)",
    "sceneTraversalSeconds": "Scene traversal (s)",
    "meshConversionSeconds": "Mesh conversion (s)",
    "embreeGeometryCommitSeconds": "Embree geometry commit (s)",
    "embreeSceneCommitSeconds": "Embree scene commit (s)",
    "firstFrameSeconds": "First frame (s)",
    "convergedSeconds": "Converged (s)",
    "processStartToConvergedSeconds": "Process start to converged (s)",
    "materialConversionSeconds": "Material conversion (s)",
    "materialXGenerationSeconds": "MaterialX generation (s)",
    "materialClassCompilationSeconds": "Material class compile (s)",
    "materialNativeJitSeconds": "Native JIT (s)",
    "materialCompilationSeconds": "Material compilation total (s)",
    "materialBatchWallSeconds": "Material batch wall (s)",
    "materialWorkerCpuSeconds": "Material worker CPU (s)",
    "materialResourceReadSeconds": "Material resource reads (s)",
    "materialResourceBytes": "Material resource bytes",
    "bufferWriteMilliseconds": "Render-buffer write (ms)",
    "bufferPublicationMilliseconds": "Render-buffer publication (ms)",
    "bufferMapWaitMilliseconds": "Render-buffer map wait (ms)",
    "materialCpuToWallRatio": "Worker CPU / batch wall",
    "materialNativeJits": "Native JIT count",
    "prototypeCount": "Geometry prototypes",
    "prototypePathCount": "Prototype paths",
    "prototypeBuildCount": "Prototype builds",
    "instanceCount": "Instances",
    "meshCount": "Meshes",
    "triangleCount": "Triangles",
    "primCount": "Prims",
    "estimatedGeometryBytes": "Estimated geometry (bytes)",
    "workingSetBytes": "Working set (bytes)",
    "peakWorkingSetBytes": "Peak working set (bytes)",
}


def _number(value):
    if isinstance(value, bool) or value is None:
        return None
    if isinstance(value, (int, float)):
        return value
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _metric_values(rows, name):
    values = [_number(row.get(name)) for row in rows]
    return [value for value in values if value is not None]


def _range_summary(values):
    if not values:
        return {"count": 0, "median": None, "min": None, "max": None}
    return {"count": len(values), "median": statistics.median(values),
            "min": min(values), "max": max(values)}


def _read_json(path):
    with Path(path).open("r", encoding="utf-8") as source:
        return json.load(source)


def _load_rows(runs_path):
    runs = _read_json(runs_path)
    if not isinstance(runs, list):
        raise ValueError(f"Expected a list in runs file: {runs_path}")
    rows, warnings = [], []
    for process_index, process in enumerate(runs, 1):
        output_dir = Path(process.get("output", ""))
        if not output_dir.is_absolute():
            output_dir = Path(runs_path).resolve().parent / output_dir
        loading_path = output_dir / "loading.json"
        if process.get("exitCode", 0) != 0:
            warnings.append(f"Process run {process.get('run', process_index)} exited with {process['exitCode']}.")
        try:
            measurements = _read_json(loading_path)
            if not isinstance(measurements, list):
                raise ValueError("expected a list")
        except (OSError, ValueError, json.JSONDecodeError) as error:
            warnings.append(f"Could not read {loading_path}: {error}")
            continue

        for measurement in measurements:
            stats = measurement.get("stats") or {}
            row = {
                "scene": process.get("scene", "unknown scene"),
                "workers": process.get("workers"),
                "cache": measurement.get("cache", "unknown"),
                "processRun": process.get("run", process_index),
                "processSeconds": _number(process.get("processSeconds")),
                "exitCode": process.get("exitCode", 0),
                "repetition": measurement.get("repetition"),
                "cachePolicy": process.get("cachePolicy", "unspecified"),
                "hostTimings": process.get("hostTimings") or [],
            }
            for name in METRICS:
                source = name
                if name == "materialCpuToWallRatio":
                    cpu = _number(stats.get("materialWorkerCpuSeconds"))
                    wall = _number(stats.get("materialBatchWallSeconds"))
                    row[name] = cpu / wall if cpu is not None and wall else None
                elif name in ("firstFrameSeconds", "convergedSeconds",
                              "processStartToConvergedSeconds"):
                    row[name] = _number(measurement.get(name))
                elif name in ("workingSetBytes", "peakWorkingSetBytes"):
                    row[name] = _number(measurement.get(name))
                else:
                    row[name] = _number(stats.get(source))
            # Accept either spelling emitted by older or newer render-stat builds.
            if row["meshCount"] is None:
                row["meshCount"] = _number(stats.get("geometryCount"))
            rows.append(row)
    return rows, warnings, len(runs)


def _group_rows(rows):
    groups = defaultdict(list)
    for row in rows:
        # Cache is deliberately part of the key: process-warm samples never
        # contribute to a fresh-renderer median (or the reverse).
        key = (row["scene"], row["workers"], row["cache"])
        groups[key].append(row)
    result = []
    for (scene, workers, cache), samples in sorted(
            groups.items(), key=lambda item: (item[0][0], str(item[0][1]), item[0][2])):
        metric_summary = {name: _range_summary(_metric_values(samples, name))
                          for name in METRICS}
        # Host timings are process-level, so count a process once per phase even
        # when it contributes multiple renderer repetitions to this cache group.
        stages = defaultdict(dict)
        for sample in samples:
            process = str(sample["processRun"])
            for timing in sample["hostTimings"]:
                phase = str(timing.get("phase", "unknown phase"))
                seconds = _number(timing.get("seconds"))
                if seconds is not None:
                    stages[phase][process] = seconds
        host_summary = {phase: _range_summary(list(per_process.values()))
                        for phase, per_process in sorted(stages.items())}
        result.append({"scene": scene, "workers": workers, "cache": cache,
                       "sampleCount": len(samples), "metrics": metric_summary,
                       "hostTimings": host_summary, "samples": samples})
    return result


def _baseline_comparison(groups, baseline_path, current_config_path, baseline_config_path):
    if not baseline_path:
        return {"performed": False, "reason": "No baseline supplied."}
    if not current_config_path or not baseline_config_path:
        return {"performed": False,
                "reason": "Baseline skipped: both current and baseline configuration JSON are required."}
    current_config, baseline_config = (_read_json(current_config_path),
                                       _read_json(baseline_config_path))
    if current_config != baseline_config:
        return {"performed": False,
                "reason": "Baseline skipped: configuration JSON does not match exactly."}
    baseline_rows, warnings, _ = _load_rows(baseline_path)
    baseline_groups = {(g["scene"], g["workers"], g["cache"]): g
                       for g in _group_rows(baseline_rows)}
    comparisons = []
    compared_metrics = ("snapshotBuildSeconds", "firstFrameSeconds", "convergedSeconds",
                        "processStartToConvergedSeconds", "materialBatchWallSeconds",
                        "materialWorkerCpuSeconds", "peakWorkingSetBytes",
                        "estimatedGeometryBytes")
    for group in groups:
        key = (group["scene"], group["workers"], group["cache"])
        baseline_group = baseline_groups.get(key)
        if not baseline_group:
            continue
        for metric in compared_metrics:
            current = group["metrics"][metric]["median"]
            baseline = baseline_group["metrics"][metric]["median"]
            if current is None or baseline is None:
                continue
            comparisons.append({"scene": group["scene"], "workers": group["workers"],
                                "cache": group["cache"], "metric": metric,
                                "currentMedian": current, "baselineMedian": baseline,
                                "percentChange": ((current - baseline) / baseline * 100.0)
                                if baseline else None})
    return {"performed": True, "configuration": current_config,
            "baselineRuns": str(Path(baseline_path)), "comparisons": comparisons,
            "warnings": warnings}


def _fmt(value, suffix=""):
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.3f}{suffix}"
    return f"{value}{suffix}"


def _fmt_range(summary, suffix=""):
    if not summary["count"]:
        return "—"
    return (f"{_fmt(summary['median'], suffix)} "
            f"[{_fmt(summary['min'], suffix)}–{_fmt(summary['max'], suffix)}]")


def render_markdown(summary):
    lines = ["# Loading benchmark summary", "",
             f"Process runs: {summary['processRunCount']}; renderer samples: {summary['sampleCount']}.",
             "OS filesystem cache state was uncontrolled; fresh process does not mean cold disk cache.",
             "Fresh-renderer and process-warm measurements are grouped separately.", ""]
    baseline = summary["baselineComparison"]
    if baseline["performed"]:
        lines.append(f"Baseline comparison used matching configuration JSON: `{baseline['baselineRuns']}`.")
    else:
        lines.append(f"No prior baseline comparison: {baseline['reason']}")
    if summary["warnings"]:
        lines.extend(["", "Input warnings:"])
        lines.extend(f"- {warning}" for warning in summary["warnings"])

    lines.extend(["", "## Group medians and ranges", "",
                  "| Scene | Workers | Cache class | Samples | Snapshot (s) | First frame (s) | Converged (s) | Process start→converged (s) | Native JITs | Prototypes / instances | Estimated / peak memory (MiB) |",
                  "|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|"])
    for group in summary["groups"]:
        m = group["metrics"]
        scene = Path(group["scene"]).name if group["scene"] != "unknown scene" else group["scene"]
        proto = _fmt_range(m["prototypeCount"])
        inst = _fmt_range(m["instanceCount"])
        estimated = _range_summary([v / (1024 * 1024) for v in _metric_values(group["samples"], "estimatedGeometryBytes")])
        peak = _range_summary([v / (1024 * 1024) for v in _metric_values(group["samples"], "peakWorkingSetBytes")])
        lines.append("| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} / {} | {} / {} |".format(
            scene, group["workers"], group["cache"], group["sampleCount"],
            _fmt_range(m["snapshotBuildSeconds"]), _fmt_range(m["firstFrameSeconds"]),
            _fmt_range(m["convergedSeconds"]), _fmt_range(m["processStartToConvergedSeconds"]),
            _fmt_range(m["materialNativeJits"]), proto, inst,
            _fmt_range(estimated, " MiB"), _fmt_range(peak, " MiB")))

    for group_index, group in enumerate(summary["groups"], 1):
        lines.extend(["", f"## {group_index}. {Path(group['scene']).name} — {group['workers']} workers — {group['cache']}", "",
                      "| Process run | Renderer repetition | Process (s) | Snapshot (s) | First frame (s) | Converged (s) | Start→converged (s) | Material CPU / wall (s) | CPU / wall | JITs | Meshes / proto paths / instances | Estimated / peak bytes |",
                      "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|"])
        for sample in group["samples"]:
            lines.append("| {} | {} | {} | {} | {} | {} | {} | {} / {} | {} | {} | {} / {} / {} | {} / {} |".format(
                sample["processRun"], sample["repetition"], _fmt(sample["processSeconds"]),
                _fmt(sample["snapshotBuildSeconds"]), _fmt(sample["firstFrameSeconds"]),
                _fmt(sample["convergedSeconds"]), _fmt(sample["processStartToConvergedSeconds"]),
                _fmt(sample["materialWorkerCpuSeconds"]), _fmt(sample["materialBatchWallSeconds"]),
                _fmt(sample["materialCpuToWallRatio"]), _fmt(sample["materialNativeJits"]),
                _fmt(sample["meshCount"]), _fmt(sample["prototypePathCount"]),
                _fmt(sample["instanceCount"]), _fmt(sample["estimatedGeometryBytes"]),
                _fmt(sample["peakWorkingSetBytes"])))
        lines.extend(["", "Measured phase median [min–max] (seconds unless noted):"])
        phase_names = ("materialConversionSeconds", "materialXGenerationSeconds",
                       "materialClassCompilationSeconds", "materialNativeJitSeconds",
                       "materialCompilationSeconds", "materialBatchWallSeconds",
                       "materialWorkerCpuSeconds", "materialResourceReadSeconds",
                       "sceneTraversalSeconds", "meshConversionSeconds",
                       "embreeGeometryCommitSeconds", "embreeSceneCommitSeconds",
                       "bufferWriteMilliseconds", "bufferPublicationMilliseconds",
                       "bufferMapWaitMilliseconds")
        lines.extend(f"- {METRICS[name]}: {_fmt_range(group['metrics'][name])}"
                     for name in phase_names)
        lines.append(f"- {METRICS['materialResourceBytes']}: "
                     f"{_fmt_range(group['metrics']['materialResourceBytes'])}")
        if group["hostTimings"]:
            lines.extend(["", "Host stage timings (one value per process run), median [min–max] seconds:"])
            lines.extend(f"- {phase}: {_fmt_range(values)}"
                         for phase, values in group["hostTimings"].items())
    if baseline["performed"] and baseline["comparisons"]:
        lines.extend(["", "## Matching-configuration baseline deltas", "",
                      "Positive percentages mean the current value is larger (usually slower for timings).",
                      "", "| Scene | Workers | Cache | Metric | Current median | Baseline median | Change |",
                      "|---|---:|---|---|---:|---:|---:|"])
        for item in baseline["comparisons"]:
            change = "—" if item["percentChange"] is None else f"{item['percentChange']:+.1f}%"
            lines.append(f"| {Path(item['scene']).name} | {item['workers']} | {item['cache']} | {METRICS[item['metric']]} | {_fmt(item['currentMedian'])} | {_fmt(item['baselineMedian'])} | {change} |")
    return "\n".join(lines) + "\n"


def summarize(runs_path, output_path, json_output=None, baseline_path=None,
              configuration_path=None, baseline_configuration_path=None):
    rows, warnings, run_count = _load_rows(runs_path)
    groups = _group_rows(rows)
    baseline = _baseline_comparison(groups, baseline_path, configuration_path,
                                    baseline_configuration_path)
    if baseline.get("warnings"):
        warnings.extend(baseline["warnings"])
    summary = {"runsFile": str(Path(runs_path).resolve()),
               "processRunCount": run_count, "sampleCount": len(rows),
               "osCacheState": "uncontrolled", "groups": groups,
               "warnings": warnings, "baselineComparison": baseline}
    output_path = Path(output_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(render_markdown(summary), encoding="utf-8")
    json_path = Path(json_output) if json_output else output_path.with_suffix(".json")
    json_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=Path, default=Path("build/loading-validation/runs.json"))
    parser.add_argument("--output", type=Path, default=Path("build/loading-validation/summary.md"))
    parser.add_argument("--json-output", type=Path)
    parser.add_argument("--baseline-runs", type=Path)
    parser.add_argument("--configuration", type=Path,
                        help="JSON identity for this run, required for baseline comparison")
    parser.add_argument("--baseline-configuration", type=Path,
                        help="JSON identity for baseline runs; must exactly match")
    args = parser.parse_args()
    summarize(args.runs, args.output, args.json_output, args.baseline_runs,
              args.configuration, args.baseline_configuration)
    print(f"Wrote {args.output} and {args.json_output or args.output.with_suffix('.json')}")


if __name__ == "__main__":
    main()
