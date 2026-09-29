"""Run in the configured Prism runtime; benchmark the installed stock usdview."""
import argparse
import hashlib
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
    parser.add_argument("--prepared-cache", choices=["empty", "reuse"], default="empty",
                        help="Empty per-run renderer caches, or a persistent cache shared between runs; OS cache is never cleared")
    parser.add_argument("--prepared-cache-dir", type=Path,
                        help="Override the persistent prepared-cache directory (requires --prepared-cache reuse)")
    parser.add_argument("--navigate", action="store_true", help="Measure 60 camera moves after loading/recreation")
    parser.add_argument("--focus-prim", default="",
                        help="World-bound prim to frame for the separate close-view navigation pass")
    parser.add_argument("--focus-elevation", type=float, default=25.0,
                        help="Close-view camera elevation in degrees; positive is above the target")
    parser.add_argument("--plugin-binary", type=Path,
                        help="Plugin DLL selected by the runtime, for benchmark provenance")
    args = parser.parse_args()
    if args.prepared_cache_dir is not None and args.prepared_cache != "reuse":
        parser.error("--prepared-cache-dir requires --prepared-cache reuse")
    args.output.mkdir(parents=True, exist_ok=True)
    script = Path(__file__).with_name("usdview_loading.py").resolve()
    plugin_binary = (args.plugin_binary.resolve() if args.plugin_binary else
                     Path(__file__).resolve().parents[2] / "build" / "hydra" / "plugin" / "hdPrism.dll")
    if plugin_binary.is_file():
        plugin_stat = plugin_binary.stat()
        digest = hashlib.sha256()
        with plugin_binary.open("rb") as binary_stream:
            for block in iter(lambda: binary_stream.read(1024 * 1024), b""):
                digest.update(block)
        plugin_metadata = dict(path=str(plugin_binary), sha256=digest.hexdigest(),
                               lastWriteUtc=time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                   time.gmtime(plugin_stat.st_mtime)))
    else:
        plugin_metadata = dict(path=str(plugin_binary), sha256=None, lastWriteUtc=None)
    results = []
    for scene in args.scene:
        for workers in args.workers:
            for repeat in range(args.repeats):
                out = (args.output/f"{scene.stem}-workers{workers}-run{repeat+1}").resolve()
                out.mkdir(parents=True, exist_ok=True)
                cache = (out / "renderer-cache" if args.prepared_cache == "empty"
                         else args.prepared_cache_dir or args.output / "shared-renderer-cache").resolve()
                existing = [p for p in cache.rglob("*") if p.is_file()] if cache.exists() else []
                initial_bytes = sum(p.stat().st_size for p in existing)
                if args.prepared_cache == "empty" and existing:
                    raise RuntimeError(f"Cold cache is not empty: {cache}. Choose a new output directory.")
                cache.mkdir(parents=True, exist_ok=True)
                env = os.environ.copy()
                env.update(PRISM_MATERIAL_THREADS=str(workers), PRISM_VALIDATION_DIR=str(out),
                           PRISM_PROFILE_BUFFERS="1", PRISM_CACHE_DIR=str(cache),
                           PRISM_BENCH_NAVIGATION="1" if args.navigate else "0",
                           PRISM_BENCH_FOCUS_PRIM=args.focus_prim,
                           PRISM_BENCH_FOCUS_ELEVATION=str(args.focus_elevation),
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
                              navigationEnabled=args.navigate,
                              focusPrim=args.focus_prim,
                              focusElevation=args.focus_elevation,
                              timingScope="Prism renderer creation after Storm host startup",
                              cachePolicy="fresh process; OS cache uncontrolled",
                              preparedCachePolicy=args.prepared_cache,
                              preparedCacheInitialFiles=len(existing),
                              preparedCacheInitialBytes=initial_bytes,
                              pluginBinary=plugin_metadata,
                              rendererCache=str(cache), output=str(out))
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
