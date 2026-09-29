"""Run three environment access strategies in separate, sequential processes.

Invoke from an environment initialized by prism_env.ps1. This measures the
existing environment integrator; it does not replace MDL material textures.
"""
import argparse
import ctypes
import json
from pathlib import Path
import re
import subprocess
import sys
import time


class ProcessMemoryCounters(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + [
        (name, ctypes.c_size_t) for name in (
            "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
            "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
            "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]


def measure(command, log):
    start = time.perf_counter()
    memory = {"peakWorkingSetBytes": 0, "lastWorkingSetBytes": 0}
    get_memory = None
    if sys.platform == "win32":
        get_memory = ctypes.WinDLL("psapi").GetProcessMemoryInfo
        get_memory.argtypes = [ctypes.c_void_p,
                              ctypes.POINTER(ProcessMemoryCounters),
                              ctypes.c_ulong]
        get_memory.restype = ctypes.c_int
    with log.open("w", encoding="utf-8") as stream:
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        while process.poll() is None:
            if get_memory:
                counters = ProcessMemoryCounters()
                counters.cb = ctypes.sizeof(counters)
                if get_memory(int(process._handle), ctypes.byref(counters), counters.cb):
                    memory["peakWorkingSetBytes"] = max(
                        memory["peakWorkingSetBytes"], counters.PeakWorkingSetSize)
                    memory["lastWorkingSetBytes"] = counters.WorkingSetSize
            time.sleep(0.1)
    return dict(exitCode=process.returncode, processSeconds=time.perf_counter()-start,
                memory=memory, command=command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--env-file", type=Path)
    parser.add_argument("--include-usd", action="store_true")
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=144)
    parser.add_argument("--spp", type=int, default=32)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    executable = (args.build / "RelWithDebInfo/prism_render_cost_benchmark.exe").resolve()
    comparator = (args.build / "hydra/plugin/prism_compare_images.exe").resolve()
    report = {"settings": dict(width=args.width, height=args.height, spp=args.spp,
                               includeUsd=args.include_usd,
                               environment=str(args.env_file) if args.env_file else "exported sky"),
              "runs": {}, "comparisons": {}}
    report_path = args.output / "results.json"
    strategies = (("imagebuf", [], "four ImageBuf getpixel reads per radiance lookup"),
                  ("resident", ["--resident-hdr"], "resident RGB float array"),
                  ("texturesystem", ["--texture-system"], "private benchmark TextureSystem"))
    report["strategies"] = {name: description for name, _, description in strategies}
    for name, flags, description in strategies:
        out = (args.output / name).resolve()
        command = [str(executable), str(out), str(args.width), str(args.height), str(args.spp)] + flags
        if not args.include_usd:
            command.append("--skip-usd")
        if args.env_file:
            command += ["--env-file", str(args.env_file.resolve())]
        log = args.output / (name + ".log")
        print("Running", name, flush=True)
        result = measure(command, log)
        result["strategy"] = description
        result["renders"] = []
        for match in re.finditer(r"^result variant=(\S+) mode=(\S+) medianSeconds=(\S+) repeats=(\S+) checksum=(\S+)",
                                 log.read_text(encoding="utf-8", errors="replace"), re.MULTILINE):
            result["renders"].append(dict(variant=match[1], mode=match[2],
                medianSeconds=float(match[3]), repeats=[float(v) for v in match[4].split(",")],
                checksum=match[5]))
        report["runs"][name] = result
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        if result["exitCode"]:
            raise RuntimeError(f"{name} failed; see {log}")
    for name in ("resident", "texturesystem"):
        for variant in (["procedural-direct", "exported-usd"] if args.include_usd else ["procedural-direct"]):
            for mode in ("mis", "bsdf"):
                label = f"{name}-{variant}-{mode}"
                command = [str(comparator), str(args.output / "imagebuf" / f"{variant}-{mode}.exr"),
                           str(args.output / name / f"{variant}-{mode}.exr")]
                if name == "resident":
                    command.append("--exact")
                comparison = subprocess.run(command, capture_output=True, text=True)
                report["comparisons"][label] = dict(exitCode=comparison.returncode,
                    output=comparison.stdout + comparison.stderr)
    report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    if any(value["exitCode"] for value in report["comparisons"].values()):
        raise RuntimeError(f"Image comparison failed; see {report_path}")
    print("Results:", report_path, flush=True)


if __name__ == "__main__":
    main()
