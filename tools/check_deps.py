#!/usr/bin/env python3
"""Checks that the repository agrees with tools/deps.json, the version list.

- vcpkg.json's builtin-baseline is deps.json's vcpkg.baseline, and
  tools/ci/mdl_vcpkg/vcpkg.json's is mdl_sdk.vcpkg_baseline (vcpkg manifests
  cannot import JSON, so they repeat the commit). --write updates them.
- The current docs mention no other version of a dependency than deps.json's.
  A mention is "<Name> <version>", also across a table cell
  ("| OpenUSD | 26.08 |"). Versions match when one is a dot-wise prefix of
  the other (oneTBB "2022.2" matches "2022.2.0"). A line ending in
  "<!-- deps: historical -->" is skipped (measurements made with older builds).

Exit code 1 on any mismatch. Run by CTest (prism_deps_consistency_test) and CI.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ["docs/windows_vulkan.md", "docs/deployment.md", "docs/hydra.md", "docs/python.md",
        "README.md", "tools/package/README.txt"]
HISTORICAL = "<!-- deps: historical -->"


def expected_versions(deps):
    packages = deps["python"]["packages"]
    return {
        "OpenUSD": deps["openusd"]["version"],
        "MaterialX": deps["materialx"]["version"],
        "OpenSubdiv": deps["opensubdiv"]["version"],
        "oneTBB": deps["onetbb"]["version"],
        "Embree": deps["embree"]["version"],
        "Open Image Denoise": deps["oidn"]["version"],
        "OIDN": deps["oidn"]["version"],
        "ISPC": deps["oidn"]["ispc"],
        "vk_oidn": deps["vk_oidn"]["version"],
        "MDL SDK": deps["mdl_sdk"]["version"],
        "Python": deps["python"]["version"],
        "PySide6": packages["PySide6"],
        "PyOpenGL": packages["PyOpenGL"],
        "numpy": packages["numpy"],
        "PyOpenColorIO": packages["opencolorio"],
        "pytest": packages["pytest"],
        "nanobind": deps["nanobind"]["version"],
        "Vulkan SDK": deps["vulkan_sdk"]["version"],
        "OpenImageIO": deps["vcpkg"]["openimageio"],
        "OpenColorIO": deps["vcpkg"]["opencolorio"],
    }


def compatible(found, expected):
    a, b = found.split("."), expected.split(".")
    n = min(len(a), len(b))
    return n > 0 and all(int(x) == int(y) for x, y in zip(a[:n], b[:n]))


def check_docs(versions, errors):
    for rel in DOCS:
        path = ROOT / rel
        if not path.exists():
            continue
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if line.rstrip().endswith(HISTORICAL):
                continue
            for name, expected in versions.items():
                pattern = r"(?<![\w-])" + re.escape(name) + r"(?:\s*\|)?\s+v?(\d+(?:\.\d+)+)\b"
                for match in re.finditer(pattern, line):
                    found = match.group(1)
                    if not compatible(found, expected):
                        errors.append(f"{rel}:{number}: {name} {found}, tools/deps.json says {expected}")


def check_manifest(rel, baseline, write, errors):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    current = json.loads(text).get("builtin-baseline")
    if current == baseline:
        return
    if write:
        path.write_text(text.replace(current, baseline), encoding="utf-8")
        print(f"{rel}: builtin-baseline {current} -> {baseline}")
    else:
        errors.append(f"{rel}: builtin-baseline {current}, tools/deps.json says {baseline} "
                      f"(run python tools/check_deps.py --write)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true", help="update the vcpkg manifests")
    args = parser.parse_args()
    deps = json.loads((ROOT / "tools/deps.json").read_text(encoding="utf-8"))
    errors = []
    check_manifest("vcpkg.json", deps["vcpkg"]["baseline"], args.write, errors)
    check_manifest("tools/ci/mdl_vcpkg/vcpkg.json", deps["mdl_sdk"]["vcpkg_baseline"], args.write, errors)
    check_docs(expected_versions(deps), errors)
    # The wheel's PyOpenColorIO and Hdx's vcpkg OpenColorIO share one process.
    if deps["python"]["packages"]["opencolorio"] != deps["vcpkg"]["opencolorio"]:
        errors.append("tools/deps.json: python.packages.opencolorio "
                      f"{deps['python']['packages']['opencolorio']} differs from vcpkg.opencolorio "
                      f"{deps['vcpkg']['opencolorio']}")
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        print(f"{len(errors)} difference(s) from tools/deps.json", file=sys.stderr)
        return 1
    print("Repository matches tools/deps.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
