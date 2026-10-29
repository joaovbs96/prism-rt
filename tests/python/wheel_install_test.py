"""prism_python_wheel_install: the prismrt wheel works in a clean venv.

1. Builds the wheel from the build tree (tools/build_wheel.py).
2. Creates a fresh venv in the build directory with this Python (the build's
   3.11) and installs the wheel and NumPy (tools/deps.json's version) only.
3. Runs tests/python/wheel_render.py in the venv with a sanitized
   environment: PATH is System32 and the venv's Scripts; no PYTHONPATH,
   PXR_*, PRISM_*, OCIO, MDL_SDK_ROOT; ``python -I``.
4. Runs the same script with the build tree's package (reference).
5. Checks: the images are bit-identical (the in-memory pixels byte for byte,
   the EXRs with prism_compare_images --exact); every module the wheel's
   process loaded is in the venv, the base Python or Windows; `import
   prismrt` loaded no USD; the default cache directory is per user, not in
   site-packages.

Environment (CTest): PRISMRT_TEST_BUILD, PRISMRT_TEST_CONFIG,
PRISMRT_TEST_SOURCE, PRISMRT_TEST_COMPARE, PRISMRT_TEST_USD (1 when the
wheel carries prismrt.usd). Renders on the CPU only.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

SOURCE = Path(os.environ.get("PRISMRT_TEST_SOURCE", Path(__file__).resolve().parents[2]))
BUILD = Path(os.environ["PRISMRT_TEST_BUILD"])
CONFIG = os.environ.get("PRISMRT_TEST_CONFIG", "RelWithDebInfo")
COMPARE = os.environ.get("PRISMRT_TEST_COMPARE")
WITH_USD = os.environ.get("PRISMRT_TEST_USD", "1") == "1"
WORK = BUILD / "python_wheel"
SCRIPT = SOURCE / "tests/python/wheel_render.py"
# Caches on C: (the per-user cache root), never on the build drive.
CACHE_ROOT = Path(os.environ.get("LOCALAPPDATA", os.environ.get("TEMP", str(WORK)))) / "PrismRT" / "tests" / "wheel"
KEEP = ("SystemRoot", "SystemDrive", "windir", "TEMP", "TMP", "LOCALAPPDATA", "APPDATA", "USERPROFILE",
        "HOMEDRIVE", "HOMEPATH", "USERNAME", "COMPUTERNAME", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE",
        "PROCESSOR_IDENTIFIER", "OS", "PATHEXT", "ComSpec", "ProgramData")


def run(command, log: Path, **kwargs) -> subprocess.CompletedProcess:
    print("$", " ".join(str(c) for c in command), flush=True)
    completed = subprocess.run([str(c) for c in command], capture_output=True, text=True, **kwargs)
    log.write_text(completed.stdout + completed.stderr, encoding="utf-8")
    if completed.returncode != 0:
        print(completed.stdout + completed.stderr)
        raise SystemExit(f"failed ({completed.returncode}): see {log}")
    return completed


def summary_of(completed) -> dict:
    for line in completed.stdout.splitlines():
        if line.startswith("WHEEL_RENDER "):
            return json.loads(line[len("WHEEL_RENDER "):])
    raise SystemExit("no WHEEL_RENDER line:\n" + completed.stdout + completed.stderr)


def under(path: str, roots) -> bool:
    path = os.path.normcase(os.path.abspath(path))
    return any(path == r or path.startswith(r + os.sep) for r in roots)


def main() -> int:
    WORK.mkdir(parents=True, exist_ok=True)
    logs = WORK / "logs"
    logs.mkdir(exist_ok=True)

    # 1. The wheel.
    dist = WORK / "dist"
    if dist.exists():
        shutil.rmtree(dist)
    command = [sys.executable, SOURCE / "tools/build_wheel.py", "--build-dir", BUILD, "--config", CONFIG,
               "--out", dist]
    if not WITH_USD:
        command.append("--no-usd")
    built = run(command, logs / "build_wheel.log")
    print(built.stdout.strip().splitlines()[1] if built.stdout.strip() else "")
    wheels = sorted(dist.glob("prismrt-*.whl"))
    assert len(wheels) == 1, wheels

    # 2. A fresh venv with the wheel and NumPy only.
    venv = WORK / "venv"
    if venv.exists():
        shutil.rmtree(venv)
    run([sys.executable, "-m", "venv", venv], logs / "venv.log")
    python = venv / "Scripts/python.exe"
    deps = json.loads((SOURCE / "tools/deps.json").read_text(encoding="utf-8"))
    numpy = deps["python"]["packages"]["numpy"]
    run([python, "-m", "pip", "install", "--disable-pip-version-check", "--no-input", "--only-binary=:all:",
         wheels[0], f"numpy=={numpy}"], logs / "pip_install.log")
    installed = run([python, "-m", "pip", "list", "--format=freeze", "--disable-pip-version-check"],
                    logs / "pip_list.log").stdout.split()
    names = sorted(line.split("==")[0].lower() for line in installed)
    assert names == ["numpy", "pip", "prismrt"] or names == ["numpy", "pip", "prismrt", "setuptools"], names

    # 3. The wheel, sanitized.
    for run_name in ("wheel", "reference"):
        cache = CACHE_ROOT / run_name
        if cache.exists():
            shutil.rmtree(cache)
        out = WORK / f"out_{run_name}"
        if out.exists():
            shutil.rmtree(out)
    system = Path(os.environ.get("SystemRoot", r"C:\Windows"))
    clean = {k: os.environ[k] for k in KEEP if k in os.environ}
    clean["PATH"] = os.pathsep.join([str(system / "System32"), str(system / "System32/Wbem"),
                                     str(venv / "Scripts")])
    usd = ["--usd"] if WITH_USD else []
    wheel_run = run([python, "-I", SCRIPT, "--source", SOURCE, "--out", WORK / "out_wheel",
                     "--cache", CACHE_ROOT / "wheel"] + usd, logs / "render_wheel.log", env=clean, cwd=WORK)
    wheel = summary_of(wheel_run)

    # 4. The build tree (its DLL directories come from _build_config.py).
    reference_env = {k: v for k, v in os.environ.items()
                     if not k.upper().startswith(("PRISM_", "PXR_", "PYTHON")) and k.upper() not in ("OCIO", "MDL_SDK_ROOT")}
    reference = summary_of(run([sys.executable, "-I", SCRIPT, "--source", SOURCE, "--out", WORK / "out_reference",
                                "--cache", CACHE_ROOT / "reference", "--package", BUILD / "python"] + usd,
                               logs / "render_reference.log", env=reference_env, cwd=WORK))

    # 5. Checks.
    site = os.path.normcase(str(venv))
    assert os.path.normcase(wheel["prismrt"]).startswith(site), wheel["prismrt"]
    assert not os.path.normcase(reference["prismrt"]).startswith(site), reference["prismrt"]
    roots = [os.path.normcase(os.path.abspath(p)) for p in (venv, sys.base_prefix, system)]
    outside = [m for m in wheel["modules_after_render"] if not under(m, roots)]
    assert not outside, "the wheel's process loaded modules from outside the venv:\n" + "\n".join(outside)
    usd_at_import = [m for m in wheel["modules_after_import"] if os.path.basename(m).lower().startswith("usd_")]
    assert not usd_at_import, f"import prismrt loaded USD: {usd_at_import}"
    if WITH_USD:
        assert any(os.path.basename(m).lower() == "usd_usd.dll" and under(m, [site])
                   for m in wheel["modules_after_render"]), "prismrt.usd did not load the wheel's USD"
    cache_default = os.path.normcase(wheel["default_cache_directory"])
    assert not cache_default.startswith(site), cache_default
    if os.environ.get("LOCALAPPDATA"):
        assert cache_default.startswith(os.path.normcase(os.environ["LOCALAPPDATA"])), cache_default

    assert wheel["files"] == reference["files"], (wheel["files"], reference["files"])
    import numpy as np
    for name in wheel["files"]:
        a = np.load(WORK / "out_wheel" / f"{name}.npy")
        b = np.load(WORK / "out_reference" / f"{name}.npy")
        assert a.shape == b.shape and a.dtype == b.dtype and a.tobytes() == b.tobytes(), \
            f"{name}: the wheel's pixels differ from the build tree's"
        if COMPARE:
            run([COMPARE, WORK / "out_reference" / f"{name}.exr", WORK / "out_wheel" / f"{name}.exr", "--exact"],
                logs / f"compare_{name}.log")
    size = wheels[0].stat().st_size
    print(f"OK: {wheels[0].name} ({size / 2**20:.1f} MiB), renders {wheel['files']} bit-identical, "
          f"{len(wheel['modules_after_render'])} modules, none outside the venv/Python/Windows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
