"""Validate material work counts and summarize structural benchmark phases.

Timing targets are informational gates separate from correctness/work counts.
Run reports from structural_scaling.py; this checker never clears caches.
"""
import argparse
import json
import statistics
from pathlib import Path


COUNTERS = (
    "materialConversionCalls", "materialGenerationCalls",
    "materialClassCompilationSeconds", "materialNativeJits",
    "materialNativeJitSeconds", "materialProgramWaitSeconds",
    "materialWorkerCpuSeconds", "materialResourceReadBytesCumulative",
    "processMaterialResourceReadCalls", "processMaterialResourceHashCalls",
    "processTextureHashCalls", "processTextureSourceBytesHashed",
    "processMaterialResourceBytesRead",
    "processTextureSourceWaitSeconds", "processTextureContentWaitSeconds",
)
PROCESS_MONOTONIC = (
    "processTextureHashCalls", "processMaterialResourceReadCalls",
    "processMaterialResourceHashCalls", "processMaterialResourceBytesRead",
    "processTextureSourceBytesHashed",
)
RENDERER_MONOTONIC = (
    "materialConversionCalls", "materialGenerationCalls",
    "materialSdkTranslationCalls", "materialResourceReadBytesCumulative",
)
WORK_REQUIRED = (
    "materialSdkTranslationCalls", "materialConversionCalls",
    "materialGenerationCalls", "materialFallbackTranslations",
)
STRUCTURAL_STABLE = (
    "materialConversionCalls", "materialGenerationCalls",
    "materialSdkTranslationCalls", "materialResourceReadBytesCumulative",
    "processMaterialResourceReadCalls", "processMaterialResourceHashCalls",
    "processTextureHashCalls",
)
TARGETS = {("identical_materials", 64): 4.0, ("textures_unique", 32): 10.0}
SUMMARY_COUNTERS = (
    "processMaterialResourceReadCalls", "processMaterialResourceHashCalls",
    "processMaterialResourceBytesRead", "materialResourceReadBytesCumulative",
    "processTextureHashCalls",
)


def _phase_row(run, row):
    after = row.get("after", {})
    delta = row.get("numericDifference", {})
    tracked = (
        "materialConversionCalls", "materialGenerationCalls",
        "materialClassCompilationSeconds", "materialNativeJits",
        "materialNativeJitSeconds", "materialProgramWaitSeconds",
        "materialWorkerCpuSeconds", "materialResourceReadBytesCumulative",
        "processMaterialResourceReadCalls", "processMaterialResourceHashCalls",
        "processTextureHashCalls", "processTextureSourceBytesHashed",
        "processTextureSourceWaitSeconds", "processTextureContentWaitSeconds",
    )
    return {
        "axis": run.get("axis"), "value": run.get("value"),
        "repeat": run.get("repeat"), "phase": row.get("operation", "unknown"),
        "elapsedSeconds": row.get("elapsedSeconds"),
        "processCpuSeconds": row.get("processCpuSeconds"),
        "workerCpuSeconds": after.get("materialWorkerCpuSeconds"),
        "classCompilationSeconds": after.get("materialClassCompilationSeconds"),
        "nativeJits": after.get("materialNativeJits"),
        "nativeJitSeconds": after.get("materialNativeJitSeconds"),
        "programWaitSeconds": after.get("materialProgramWaitSeconds"),
        "resourceReadBytesCumulative": after.get("materialResourceReadBytesCumulative"),
        "materialResourceReadCalls": after.get("processMaterialResourceReadCalls"),
        "materialResourceHashCalls": after.get("processMaterialResourceHashCalls"),
        "textureHashCalls": after.get("processTextureHashCalls"),
        "textureSourceBytesHashed": after.get("processTextureSourceBytesHashed"),
        "textureSourceWaitSeconds": after.get("processTextureSourceWaitSeconds"),
        "textureContentWaitSeconds": after.get("processTextureContentWaitSeconds"),
        "counterDeltas": {key: delta.get(key) for key in tracked if key in delta},
    }


def check(report):
    work_failures, failed_runs, material_failures = [], [], []
    phase_measurements, timing_samples, operation_samples = [], {}, {}
    previous_process_counters = {}
    previous_renderer_counters = {}

    for run in report.get("runs", []):
        axis, value, repeat = run.get("axis", "?"), run.get("value", "?"), run.get("repeat", "?")
        label = f"{axis}/{value}/run{repeat}"
        if run.get("exitCode") != 0:
            failed_runs.append({"run": label, "exitCode": run.get("exitCode"),
                                "phase": "usdview-process"})
            continue
        operations = run.get("probe", {}).get("operations", [])
        if not operations:
            failed_runs.append({"run": label, "exitCode": 0,
                                "phase": "probe-report", "reason": "no operations recorded"})
            continue

        # Process registries survive renderer swaps. Renderer-local counters
        # may reset at the known recreation boundary; per-generation native JIT
        # counts are gauges and are intentionally excluded from monotonicity.
        previous_process_counters = {}
        previous_renderer_counters = {}
        for row in operations:
            phase = row.get("operation", "unknown")
            after = row.get("after", {})
            delta = row.get("numericDifference", {})
            phase_measurements.append(_phase_row(run, row))
            sample = operation_samples.setdefault((axis, value, phase), {
                "elapsed": [], "processCpu": [], "counters": {key: [] for key in SUMMARY_COUNTERS}})
            for field, target in (("elapsedSeconds", "elapsed"),
                                  ("processCpuSeconds", "processCpu")):
                value_sample = row.get(field)
                if isinstance(value_sample, (int, float)):
                    sample[target].append(value_sample)
            for key in SUMMARY_COUNTERS:
                value_sample = after.get(key)
                if isinstance(value_sample, (int, float)):
                    sample["counters"][key].append(value_sample)
            if after.get("materialFailures", 0):
                material_failures.append({"run": label, "phase": phase,
                                          "count": after["materialFailures"]})

            for key in PROCESS_MONOTONIC:
                current = after.get(key)
                if isinstance(current, (int, float)):
                    prior = previous_process_counters.get(key)
                    if prior is not None and current < prior:
                        work_failures.append({"run": label, "phase": phase,
                                              "metric": key, "actual": current,
                                              "expected": f">= {prior}",
                                              "reason": "process counter reset within process"})
                    previous_process_counters[key] = current

            if phase == "recreate":
                previous_renderer_counters = {
                    key: after[key] for key in RENDERER_MONOTONIC
                    if isinstance(after.get(key), (int, float))}
            else:
                for key in RENDERER_MONOTONIC:
                    current = after.get(key)
                    if isinstance(current, (int, float)):
                        prior = previous_renderer_counters.get(key)
                        if prior is not None and current < prior:
                            work_failures.append({"run": label, "phase": phase,
                                                  "metric": key, "actual": current,
                                                  "expected": f">= {prior}",
                                                  "reason": "renderer-lifetime counter reset outside recreation"})
                        previous_renderer_counters[key] = current

            if phase == "cold":
                timing_samples.setdefault((axis, value), []).append(row.get("elapsedSeconds"))
                for key in WORK_REQUIRED + ("materialProgramWaitSeconds",
                                            "processTextureHashCalls",
                                            "processMaterialResourceReadCalls",
                                            "processMaterialResourceHashCalls",
                                            "processMaterialResourceBytesRead",
                                            "processTextureSourceBytesHashed",
                                            "processTextureSourceWaitSeconds",
                                            "processTextureContentWaitSeconds"):
                    if key not in after:
                        work_failures.append({"run": label, "phase": phase,
                                              "metric": key, "reason": "missing counter"})
                if axis == "identical_materials":
                    for key in WORK_REQUIRED[:3]:
                        if after.get(key) != 1:
                            work_failures.append({"run": label, "phase": phase,
                                                  "metric": key, "actual": after.get(key),
                                                  "expected": 1})
                if axis == "textures_unique" and value == 32:
                    for key in WORK_REQUIRED[1:3]:
                        if after.get(key) != 1:
                            work_failures.append({"run": label, "phase": phase,
                                                  "metric": key, "actual": after.get(key),
                                                  "expected": 1})
                    for key, expected in (("materialSdkTranslationCalls", 1),
                                          ("materialFallbackTranslations", 0)):
                        if after.get(key) != expected:
                            work_failures.append({"run": label, "phase": phase,
                                                  "metric": key, "actual": after.get(key),
                                                  "expected": expected})
            if phase.startswith("structural-"):
                for key in STRUCTURAL_STABLE:
                    if key not in delta or delta[key] != 0:
                        work_failures.append({"run": label, "phase": phase,
                                              "metric": key, "actual": delta.get(key),
                                              "expected": 0})

    timings = []
    performance_misses = []
    for key, values in sorted(timing_samples.items()):
        samples = [value for value in values if isinstance(value, (int, float))]
        target = TARGETS.get(key)
        enough = len(samples) >= 3
        median = statistics.median(samples) if samples else None
        met = None if target is None or not enough else median <= target
        row = dict(axis=key[0], value=key[1], runs=len(samples),
                   medianSeconds=median, targetSeconds=target,
                   targetMet=met,
                   status="insufficient-repeats" if target is not None and not enough else
                          "measured" if target is not None else "no-target")
        timings.append(row)
        if met is False:
            performance_misses.append({"axis": key[0], "value": key[1],
                                       "phase": "cold", "medianSeconds": median,
                                       "targetSeconds": target})

    operation_summaries = []
    for (axis, value, phase), sample in sorted(operation_samples.items(),
                                               key=lambda item: (str(item[0][0]), str(item[0][1]), item[0][2])):
        elapsed, process_cpu = sample["elapsed"], sample["processCpu"]
        operation_summaries.append({
            "axis": axis, "value": value, "phase": phase,
            "runs": len(elapsed),
            "medianElapsedSeconds": statistics.median(elapsed) if elapsed else None,
            "medianProcessCpuSeconds": statistics.median(process_cpu) if process_cpu else None,
            "medianProcessCounters": {
                key: statistics.median(values) if values else None
                for key, values in sample["counters"].items()},
            "repeatRequirement": 3,
            "status": "measured" if len(elapsed) >= 3 else "insufficient-repeats",
        })

    return {
        "workCountFailures": work_failures,
        "failures": {"usdviewRuns": failed_runs, "materialFailures": material_failures},
        "performanceMisses": performance_misses,
        "timings": timings,
        "operationSummaries": operation_summaries,
        "phaseAttribution": phase_measurements,
        "conditions": report.get("settings", {}) | {
            "cacheLabels": sorted({run.get("cacheLabel") for run in report.get("runs", [])
                                    if run.get("cacheLabel") is not None})},
        "note": "Elapsed and process CPU are distinct from renderer worker CPU. "
                "Performance misses do not relax work-count gates; OS caches are not flushed.",
    }


def _conditions_label(conditions):
    if not conditions:
        return "Conditions not recorded; OS cache state uncontrolled."
    parts = []
    for key in ("processModel", "cacheLabels", "cachePolicy", "preparedCache",
                "osCache", "materialThreads", "initialRenderer", "pluginSha256"):
        if key in conditions:
            parts.append(f"{key}={conditions[key]}")
    return "; ".join(parts) + ("; OS cache state uncontrolled" if "osCache" not in conditions else "")


def markdown(results, baseline_results=None):
    lines = ["# Material initialization summary", "",
             "Elapsed and process CPU are separate measurements; renderer worker CPU is not added to either.",
             "Each phase requires at least 3 successful repeats for a measured status.", ""]
    baseline_by_key = {}
    if baseline_results:
        for result in baseline_results.values():
            for row in result["operationSummaries"]:
                baseline_by_key[(row["axis"], row["value"], row["phase"])] = row
    for report_name, result in results.items():
        lines.extend([f"## {report_name}", "", _conditions_label(result.get("conditions", {})), "",
                      "| Axis | Value | Phase | Repeats | Elapsed median (s) | Process CPU median (s) | Resource reads / hashes | Source bytes | Texture hashes | Status |",
                      "|---|---:|---|---:|---:|---:|---:|---:|---:|---|"])
        for row in result["operationSummaries"]:
            counters = row["medianProcessCounters"]
            reads = counters.get("processMaterialResourceReadCalls")
            hashes = counters.get("processMaterialResourceHashCalls")
            bytecount = counters.get("processMaterialResourceBytesRead")
            texhashes = counters.get("processTextureHashCalls")
            elapsed = row["medianElapsedSeconds"]
            baseline_text = ""
            baseline_row = baseline_by_key.get((row["axis"], row["value"], row["phase"]))
            if baseline_row and elapsed is not None and baseline_row.get("medianElapsedSeconds") is not None:
                baseline_text = f" (baseline {baseline_row['medianElapsedSeconds']:.3f}s)"
            lines.append("| {axis} | {value} | {phase} | {runs} | {elapsed} | {cpu} | {reads} / {hashes} | {bytes} | {textures} | {status}{baseline} |".format(
                axis=row["axis"], value=row["value"], phase=row["phase"], runs=row["runs"],
                elapsed="—" if elapsed is None else f"{elapsed:.3f}" + baseline_text,
                cpu="—" if row["medianProcessCpuSeconds"] is None else f"{row['medianProcessCpuSeconds']:.3f}",
                reads="—" if reads is None else f"{reads:g}",
                hashes="—" if hashes is None else f"{hashes:g}",
                bytes="—" if bytecount is None else f"{bytecount:g}",
                textures="—" if texhashes is None else f"{texhashes:g}", status=row["status"],
                baseline=""))
        lines.append("")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--markdown", type=Path,
                        help="also write a concise Markdown phase summary to this path")
    parser.add_argument("--baseline", type=Path,
                        help="optional baseline results.json for elapsed-median context in Markdown")
    args = parser.parse_args()
    reports = {str(path): json.loads(path.read_text(encoding="utf-8")) for path in args.reports}
    results = {path: check(report) for path, report in reports.items()}
    text = json.dumps(results, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    if args.markdown:
        baseline = None
        if args.baseline:
            baseline_raw = {str(args.baseline): json.loads(args.baseline.read_text(encoding="utf-8"))}
            baseline = {path: check(report) for path, report in baseline_raw.items()}
        args.markdown.write_text(markdown(results, baseline), encoding="utf-8")
    has_work_failure = any(result["workCountFailures"] for result in results.values())
    has_run_failure = any(result["failures"]["usdviewRuns"] for result in results.values())
    has_material_failure = any(result["failures"]["materialFailures"] for result in results.values())
    raise SystemExit(int(has_work_failure or has_run_failure or has_material_failure))


if __name__ == "__main__":
    main()
