"""Renders the Typhoon test suite with PrismRT and compares it with Typhoon's references.

The suite (https://github.com/anderslanglands/typhoon-test-suite) is a set of
USD scenes with reference EXRs rendered by Typhoon (hdEmbree) through
Goldeneye. This runner renders every scene with prism_hydra_render on the CPU
device, matched to the scene's render settings, then compares each image with
its reference (per-channel NRMSE and mean ratio, and HDR-FLIP as Goldeneye
computes it when flip_evaluator is importable) and writes a Markdown and an
HTML summary with side-by-side images.

Settings taken from each scene's active RenderSettings prim:
  camera, resolution                     the settings (or first product) values
  ty:convergedSamplesPerPixel (256)      --spp, capped at --spp-cap
  ty:maxBounces (16)                     --max-depth maxBounces + 2 (camera hit,
                                         maxBounces indirect bounces, light)
  renderingColorSpace                    honoured by PrismRT when authored; when
                                         not, --working-space lin_rec709
                                         (Typhoon's default)
  start time code / usdlux frames        --time
Fixed: --adaptive-threshold 0, --seed 0, --sample-clamp 0 (PrismRT's clamp
limits whole samples, which would cut visible lights; Typhoon clamps
contributions), and a per-scene --time-limit.

Run from PowerShell after dot-sourcing prism_env.ps1 (import order: pxr is
imported before anything else that loads DLLs):

  . .\\prism_env.ps1
  python tools\\conformance\\run_typhoon_suite.py --suite <typhoon-test-suite>
      --references <dir with test-suite/, materials/, usdlux/ reference trees>
      --renderer build\\RelWithDebInfo\\prism_hydra_render.exe
      --output local\\reports\\typhoon [--capped local\\alab\\run_capped.ps1]

Renders run one at a time. With --capped each runs inside run_capped.ps1's
job-object memory cap (--limit-gb). --compare-only reuses existing renders.
"""

from pxr import Sdf, Usd, UsdRender  # noqa: F401  (pxr first: DLL load order)

import argparse
import fnmatch
import html
import json
import math
import os
import struct
import subprocess
import sys
import time
import tomllib
import zlib
from pathlib import Path

import numpy as np

SUITES = ("test-suite", "materials", "usdlux")
TYPHOON_DEFAULT_SPP = 256
TYPHOON_DEFAULT_MAX_BOUNCES = 16
GOLDENEYE_FLIP_THRESHOLD = 0.04


# --- discovery ---------------------------------------------------------------

def suite_config(root: Path, suite: str) -> dict:
    path = root / suite / "goldeneye-suite.toml"
    return tomllib.loads(path.read_text()) if path.exists() else {}


def case_config(scene: Path) -> dict:
    path = scene.with_suffix(".goldeneye.toml")
    return tomllib.loads(path.read_text()) if path.exists() else {}


def discover(root: Path, references: Path, only: list[str]) -> list[dict]:
    cases = []
    for suite in SUITES:
        config = suite_config(root, suite)
        frames = config.get("frames", {})
        base = root / suite
        for scene in sorted(base.rglob("*.usda")):
            rel = scene.relative_to(base)
            if rel.parts[0].startswith("_") or rel.parts[0] == "reference":
                continue
            stem = rel.with_suffix("").as_posix()
            extra = case_config(scene)
            threshold = extra.get("comparison", {}).get("flip_threshold",
                config.get("comparison", {}).get("flip_threshold", GOLDENEYE_FLIP_THRESHOLD))
            suspect = bool(extra.get("test", {}).get("suspect", False))
            if suite == "usdlux":
                if stem not in frames:
                    continue
                first, last = (int(v) for v in str(frames[stem]).split(":"))
                frame_list = list(range(first, last + 1))
            else:
                frame_list = [None]
            for frame in frame_list:
                if frame is None:
                    key = f"{suite}/{stem}"
                    reference = references / suite / "reference" / f"{stem}.exr"
                else:
                    key = f"{suite}/{stem}.{frame:04d}"
                    reference = references / suite / "reference" / f"{stem}-embree.{frame:04d}.exr"
                if only and not any(fnmatch.fnmatch(key, pattern) or pattern in key for pattern in only):
                    continue
                cases.append({"key": key, "suite": suite, "scene": str(scene), "frame": frame,
                              "reference": str(reference), "flip_threshold": threshold,
                              "suspect": suspect})
    return cases


# --- scene settings ----------------------------------------------------------

def attribute_value(prim, name, default=None):
    attribute = prim.GetAttribute(name) if prim else None
    if attribute and attribute.HasAuthoredValue():
        return attribute.Get()
    return default


def scene_settings(scene: str) -> dict:
    stage = Usd.Stage.Open(scene)
    path = stage.GetMetadata("renderSettingsPrimPath") or ""
    settings = UsdRender.Settings(stage.GetPrimAtPath(path)) if path else UsdRender.Settings()
    if not settings:
        raise RuntimeError(f"no RenderSettings prim (renderSettingsPrimPath '{path}')")
    prim = settings.GetPrim()
    cameras = settings.GetCameraRel().GetForwardedTargets()
    resolution = attribute_value(prim, "resolution")
    products = settings.GetProductsRel().GetForwardedTargets()
    if products:
        product = UsdRender.Product(stage.GetPrimAtPath(products[0]))
        if not cameras:
            cameras = product.GetCameraRel().GetForwardedTargets()
        resolution = attribute_value(product.GetPrim(), "resolution", resolution)
    if resolution is None:
        resolution = settings.GetResolutionAttr().Get()
    rendering_space = attribute_value(prim, "renderingColorSpace")
    start = stage.GetStartTimeCode() if stage.HasAuthoredTimeCodeRange() else None
    return {
        "settings": str(prim.GetPath()),
        "camera": str(cameras[0]) if cameras else "",
        "width": int(resolution[0]), "height": int(resolution[1]),
        "spp": int(attribute_value(prim, "ty:convergedSamplesPerPixel", TYPHOON_DEFAULT_SPP)),
        "max_bounces": int(attribute_value(prim, "ty:maxBounces", TYPHOON_DEFAULT_MAX_BOUNCES)),
        "rendering_color_space": str(rendering_space) if rendering_space else "",
        "start_time": start,
    }


# --- rendering ---------------------------------------------------------------

def render(case: dict, args, output: Path, log: Path) -> dict:
    s = scene_settings(case["scene"])
    spp = min(s["spp"], args.spp_cap)
    command = [case["scene"], s["camera"], str(output), str(s["width"]), str(s["height"]), str(spp),
               "--render-settings", s["settings"], "--adaptive-threshold", "0", "--seed", "0",
               "--sample-clamp", "0", "--max-depth", str(s["max_bounces"] + 2),
               "--time-limit", str(args.time_limit)]
    if not s["rendering_color_space"]:
        command += ["--working-space", "lin_rec709"]
    when = case["frame"] if case["frame"] is not None else s["start_time"]
    if when is not None:
        command += ["--time", str(when)]
    info = dict(s, spp_rendered=spp, command=command)
    output.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    environment = dict(os.environ, PRISM_DEVICE="cpu")
    if args.cache_dir:
        environment["PRISM_CACHE_DIR"] = args.cache_dir
    if args.capped:
        quoted = " ".join(f'"{part}"' if (" " in part or not part) else part for part in command)
        launch = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", args.capped,
                  "-LimitGB", str(args.limit_gb), "-Exe", args.renderer, "-Arguments", quoted]
    else:
        launch = [args.renderer] + command
    start = time.perf_counter()
    with log.open("w", encoding="utf-8", errors="replace") as stream:
        stream.write(" ".join(launch) + "\n\n")
        stream.flush()
        try:
            result = subprocess.run(launch, stdout=stream, stderr=subprocess.STDOUT, env=environment,
                                    timeout=args.time_limit + args.load_timeout)
            info["exit_code"] = result.returncode
        except subprocess.TimeoutExpired:
            info["exit_code"] = "timeout"
    info["seconds"] = round(time.perf_counter() - start, 1)
    return info


# --- images ------------------------------------------------------------------

def load_flip():
    try:
        import flip_evaluator
        return flip_evaluator
    except ImportError:
        return None


def read_rgb(flip, path: str) -> np.ndarray:
    if flip is not None:
        image = np.asarray(flip.load(path), dtype=np.float32)
    else:
        import OpenImageIO as oiio  # fallback; may clash with pxr's DLLs
        buffer = oiio.ImageBuf(path)
        image = buffer.get_pixels(oiio.FLOAT)
    if image.ndim == 2:
        image = np.stack([image] * 3, axis=-1)
    if image.shape[-1] == 1:
        image = np.repeat(image, 3, axis=-1)
    return np.nan_to_num(image[..., :3].astype(np.float32), nan=0.0, posinf=0.0, neginf=0.0)


def flip_mean(flip, reference: np.ndarray, render: np.ndarray):
    if flip is None:
        return None
    kwargs = {"inputsRGB": True, "applyMagma": False, "computeMeanError": True}
    combined = np.concatenate([reference.ravel(), render.ravel()])
    if combined.size and np.all(combined >= 0) and float(combined.max()) <= 1e-6:
        kwargs["parameters"] = {"startExposure": -10.0, "stopExposure": 10.0, "numExposures": 1}
    _, mean, _ = flip.evaluate(np.ascontiguousarray(reference), np.ascontiguousarray(render), "HDR", **kwargs)
    return float(mean)


def encode_srgb(linear: np.ndarray) -> np.ndarray:
    x = np.clip(linear, 0.0, 1.0)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def write_png(path: Path, rgb8: np.ndarray) -> None:
    height, width, _ = rgb8.shape
    raw = b"".join(b"\0" + rgb8[y].tobytes() for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def side_by_side(path: Path, reference: np.ndarray, render: np.ndarray) -> None:
    """Reference | PrismRT | |difference| x4, sRGB-encoded, 4 px gaps."""
    difference = np.abs(render - reference) * 4.0
    height, width, _ = reference.shape
    gap = np.full((height, 4, 3), 0.5, dtype=np.float32)
    row = np.concatenate([encode_srgb(reference), gap, encode_srgb(render), gap, encode_srgb(difference)], axis=1)
    write_png(path, (row * 255.0 + 0.5).astype(np.uint8))


def compare(case: dict, render_path: Path, flip, image_path: Path) -> dict:
    reference = read_rgb(flip, case["reference"])
    render = read_rgb(flip, str(render_path))
    if render.shape != reference.shape:
        return {"error": f"size {render.shape[1]}x{render.shape[0]} vs reference "
                         f"{reference.shape[1]}x{reference.shape[0]}"}
    ref_mean = reference.reshape(-1, 3).mean(axis=0)
    ren_mean = render.reshape(-1, 3).mean(axis=0)
    rms = np.sqrt(((render - reference) ** 2).reshape(-1, 3).mean(axis=0))
    scale = np.maximum(ref_mean, 1e-6)
    # The same on 8x8 block means: mostly bias, where the per-pixel NRMSE of
    # two noisy images is mostly noise.
    h, w = (reference.shape[0] // 8) * 8, (reference.shape[1] // 8) * 8
    block = lambda image: image[:h, :w].reshape(h // 8, 8, w // 8, 8, 3).mean(axis=(1, 3))
    block_rms = np.sqrt(((block(render) - block(reference)) ** 2).reshape(-1, 3).mean(axis=0))
    side_by_side(image_path, reference, render)
    luminance = np.array([0.2126, 0.7152, 0.0722])
    return {
        "nrmse": [float(v) for v in rms / scale],
        "nrmse_block8": [float(v) for v in block_rms / scale],
        "mean_ratio": [float(r / m) if m > 1e-6 else (1.0 if r <= 1e-6 else math.inf)
                       for r, m in zip(ren_mean, ref_mean)],
        "mean_reference": [float(v) for v in ref_mean],
        "mean_render": [float(v) for v in ren_mean],
        "luminance_ratio": float((ren_mean @ luminance) / max(ref_mean @ luminance, 1e-6)),
        "flip": flip_mean(flip, reference, render),
    }


# --- summary -----------------------------------------------------------------

def status_of(row: dict) -> str:
    if "error" in row:
        return "error"
    if row.get("flip") is not None:
        return "pass" if row["flip"] <= row["flip_threshold"] else "fail"
    return "pass" if max(row["nrmse"]) <= 0.1 and abs(row["luminance_ratio"] - 1) <= 0.05 else "fail"


def classify(row: dict, rules: list[dict]) -> tuple[str, str]:
    for rule in rules:
        if fnmatch.fnmatch(row["key"], rule["match"]):
            return rule.get("bucket", ""), rule.get("note", "")
    return "", ""


def triple(values, digits=3) -> str:
    return " / ".join("inf" if math.isinf(v) else f"{v:.{digits}f}" for v in values)


def write_summary(rows: list[dict], output: Path, rules: list[dict]) -> None:
    for row in rows:
        row["status"] = status_of(row)
        row["bucket"], row["note"] = classify(row, rules) if row["status"] != "pass" else ("", "")
    counts = {}
    for row in rows:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    buckets = {}
    for row in rows:
        if row["status"] != "pass":
            buckets[row["bucket"] or "unclassified"] = buckets.get(row["bucket"] or "unclassified", 0) + 1
    (output / "results.json").write_text(json.dumps(rows, indent=1))

    lines = ["# Typhoon test suite: PrismRT (CPU)", "",
             "Status: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) +
             f" of {len(rows)}. Pass: HDR-FLIP mean within the suite threshold (Goldeneye's test).", "",
             "Deviations by bucket: " + (", ".join(f"{k} {v}" for k, v in sorted(buckets.items())) or "none"), "",
             "| case | status | FLIP (thr) | NRMSE R/G/B | NRMSE 8x8 R/G/B | mean ratio R/G/B | spp | s | bucket | note |",
             "|---|---|---|---|---|---|---|---|---|---|"]
    for row in rows:
        if "error" in row:
            lines.append(f"| {row['key']} | error | | | | | | {row.get('seconds', '')} | {row['bucket']} | "
                         f"{row['error']} |")
            continue
        flip = "" if row["flip"] is None else f"{row['flip']:.4f} ({row['flip_threshold']})"
        lines.append(f"| [{row['key']}](images/{row['image']}) | {row['status']} | {flip} | "
                     f"{triple(row['nrmse'])} | {triple(row['nrmse_block8'])} | {triple(row['mean_ratio'])} | "
                     f"{row.get('spp_rendered', '')} | "
                     f"{row.get('seconds', '')} | {row['bucket']} | {row['note']} |")
    (output / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    cells = []
    for row in rows:
        e = html.escape
        if "error" in row:
            cells.append(f"<tr class='error'><td>{e(row['key'])}</td><td>error</td><td colspan=5>{e(row['error'])}"
                         f"</td><td>{e(row['bucket'])}</td><td>{e(row['note'])}</td><td></td></tr>")
            continue
        flip = "" if row["flip"] is None else f"{row['flip']:.4f} ({row['flip_threshold']})"
        cells.append(f"<tr class='{row['status']}'><td>{e(row['key'])}</td><td>{row['status']}</td><td>{flip}</td>"
                     f"<td>{triple(row['nrmse'])}</td><td>{triple(row['nrmse_block8'])}</td><td>{triple(row['mean_ratio'])}</td>"
                     f"<td>{row.get('spp_rendered', '')}</td><td>{e(row['bucket'])}</td><td>{e(row['note'])}</td>"
                     f"<td><img loading='lazy' src='images/{e(row['image'])}'></td></tr>")
    page = f"""<!doctype html><html><head><meta charset="utf-8"><title>Typhoon suite: PrismRT</title>
<style>body{{font:13px sans-serif;background:#fff;color:#111}} table{{border-collapse:collapse}}
td,th{{border:1px solid #ccc;padding:3px 6px;vertical-align:top}} tr.fail td:nth-child(2){{color:#b00}}
tr.error td:nth-child(2){{color:#b0b}} tr.pass td:nth-child(2){{color:#070}} img{{max-height:140px}}</style></head>
<body><h1>Typhoon test suite: PrismRT (CPU)</h1>
<p>{html.escape(lines[2])}<br>{html.escape(lines[4])}<br>Images: reference | PrismRT | |difference| x4 (sRGB, clamped).</p>
<table><tr><th>case</th><th>status</th><th>FLIP (thr)</th><th>NRMSE R/G/B</th><th>NRMSE 8x8</th><th>mean ratio R/G/B</th>
<th>spp</th><th>bucket</th><th>note</th><th>image</th></tr>{''.join(cells)}</table></body></html>"""
    (output / "summary.html").write_text(page, encoding="utf-8")


# --- main --------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--suite", required=True, type=Path, help="typhoon-test-suite checkout")
    parser.add_argument("--references", type=Path, help="reference root (default: the suite)")
    parser.add_argument("--renderer", required=True, help="prism_hydra_render executable")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--capped", help="run_capped.ps1: run each render in a job-object memory cap")
    parser.add_argument("--limit-gb", type=float, default=24.0)
    parser.add_argument("--spp-cap", type=int, default=128)
    parser.add_argument("--time-limit", type=float, default=120.0, help="render time limit per scene (s)")
    parser.add_argument("--load-timeout", type=float, default=600.0, help="extra wall time for loading (s)")
    parser.add_argument("--cache-dir", help="PRISM_CACHE_DIR for the renders")
    parser.add_argument("--only", nargs="*", default=[], help="case key globs or substrings")
    parser.add_argument("--compare-only", action="store_true", help="compare existing renders")
    parser.add_argument("--classification", type=Path, help="JSON list of {match, bucket, note}")
    args = parser.parse_args()
    # Absolute paths: run_capped.ps1 starts the renderer from its own directory.
    args.renderer = str(Path(args.renderer).resolve())
    if not args.compare_only and not Path(args.renderer).is_file():
        parser.error(f"no renderer at {args.renderer} (build/hydra/plugin/prism_hydra_render.exe)")
    args.output = args.output.resolve()
    args.suite = args.suite.resolve()
    references = (args.references or args.suite).resolve()
    cases = discover(args.suite, references, args.only)
    flip = load_flip()
    rules = json.loads(args.classification.read_text()) if args.classification else []
    args.output.mkdir(parents=True, exist_ok=True)
    previous = {}
    if (args.output / "results.json").exists():
        previous = {row["key"]: row for row in json.loads((args.output / "results.json").read_text())}
    rows = []
    for index, case in enumerate(cases):
        name = case["key"].replace("/", "__")
        render_path = args.output / "renders" / f"{name}.exr"
        row = dict(case)
        if args.compare_only:
            old = previous.get(case["key"], {})
            row.update({k: old[k] for k in ("spp_rendered", "seconds", "exit_code", "command", "camera",
                                             "width", "height", "spp", "max_bounces",
                                             "rendering_color_space") if k in old})
        else:
            try:
                row.update(render(case, args, render_path, args.output / "logs" / f"{name}.log"))
            except Exception as error:  # a scene the runner cannot read
                row["error"] = f"settings: {error}"
        if "error" not in row:
            if not Path(case["reference"]).exists():
                row["error"] = "no reference image"
            elif not render_path.exists():
                row["error"] = f"no render (exit {row.get('exit_code')}, see logs/{name}.log)"
            else:
                row["image"] = f"{name}.png"
                row.update(compare(case, render_path, flip, args.output / "images" / row["image"]))
        print(f"[{index + 1}/{len(cases)}] {case['key']}: " +
              (row["error"] if "error" in row else
               f"flip {row['flip']} nrmse {triple(row['nrmse'])} ratio {triple(row['mean_ratio'])} "
               f"({row.get('seconds', '?')} s)"), flush=True)
        rows.append(row)
        if (index + 1) % 10 == 0:
            write_summary(rows, args.output, rules)
    write_summary(rows, args.output, rules)
    return 0


if __name__ == "__main__":
    sys.exit(main())
