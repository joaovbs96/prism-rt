"""Run in the configured Prism runtime; benchmark the installed stock usdview."""
import argparse
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--testusdview", required=True, type=Path)
    parser.add_argument("--scene", action="append", type=Path, required=True)
    parser.add_argument("--workers", nargs="+", type=int, default=[1,2,4])
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("build/loading-validation"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    script = Path(__file__).with_name("usdview_loading.py").resolve()
    results = []
    for scene in args.scene:
        for workers in args.workers:
            for repeat in range(args.repeats):
                out = (args.output/f"{scene.stem}-workers{workers}-run{repeat+1}").resolve()
                out.mkdir(parents=True, exist_ok=True)
                env = os.environ.copy()
                env.update(PRISM_MATERIAL_THREADS=str(workers), PRISM_VALIDATION_DIR=str(out),
                           PRISM_PROFILE_BUFFERS="1",
                           PRISM_BENCH_START=str(time.perf_counter()))
                start = time.perf_counter()
                with (out/"usdview.log").open("w", encoding="utf-8") as log:
                    process = subprocess.run([sys.executable, str(args.testusdview.resolve()),
                        str(scene.resolve()), "--renderer", "HdStormRendererPlugin",
                        "--timing", "--testScript", str(script)], env=env,
                        stdout=log, stderr=subprocess.STDOUT, timeout=900)
                result = dict(scene=str(scene.resolve()), workers=workers, run=repeat+1,
                              processSeconds=time.perf_counter()-start, exitCode=process.returncode,
                              initialRenderer="Storm",
                              bufferWriteProfiling=True,
                              timingScope="Prism renderer creation after Storm host startup",
                              cachePolicy="fresh process; OS cache uncontrolled", output=str(out))
                log_text = (out/"usdview.log").read_text(encoding="utf-8", errors="replace")
                result["hostTimings"] = [dict(phase=phase, seconds=float(seconds))
                    for phase, seconds in re.findall(r"Time to (.*): ([0-9.]+)s", log_text)]
                results.append(result)
                (args.output/"runs.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
                print(json.dumps(result), flush=True)
                if process.returncode:
                    raise RuntimeError(f"usdview failed: {out / 'usdview.log'}")


if __name__ == "__main__":
    main()
