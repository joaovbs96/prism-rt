#!/usr/bin/env python3
"""Builds the prismrt wheel from a configured and built PrismRT build tree.

    python tools/build_wheel.py --build-dir D:\\prism-builds\\sdk_wheel [--config RelWithDebInfo]
                                [--out <dir>] [--no-usd]

The build tree already holds the package (``<build>/python/prismrt``: the
nanobind extensions, the .py files, the stubs) and knows where every
dependency lives (``prism-runtime.json``, ``python_config/<config>/
_build_config.py``, ``CMakeCache.txt``). This script assembles the wheel
from them with the standard library only (docs/python.md, "Wheel"):

- ``prismrt/``: the package; ``_build_config.py`` is replaced by a packaged
  one (paths relative to the package, docs in prismrt/_dll.py);
- ``prismrt/bin/``: prism.dll, prism_usd.dll and every DLL they need
  (found by walking the PE import tables, delay-loaded imports included,
  from the extension modules and the dynamically loaded libraries: the MDL
  SDK and its nv_openimageio plugin, OIDN's device modules, DXC), the MSVC
  C++ runtime; system DLLs stay out; no PDBs;
- ``prismrt/usd/``: the USD build prism_usd links (its DLLs, lib/usd plug-in
  resources and plugin/usd), found only by ``prismrt.usd``;
- ``prismrt/materialx/libraries``: the MaterialX data libraries;
- ``prismrt/licenses/``: the licences of everything bundled.

The wheel is reproducible: entries sorted, timestamps from SOURCE_DATE_EPOCH
or the last commit. Windows only for now; Linux/macOS need the equivalent of
the import walk (auditwheel/delocate or ELF/Mach-O parsing) and rpaths
(docs/python.md).
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import sysconfig
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TAG_PLATFORM = "win_amd64"

# Loaded at run time by name (not imported), so the walk starts from them too.
MDL_LIBRARIES = ("libmdl_sdk.dll", "nv_openimageio.dll")
# The MSVC C++ runtime ships with the wheel (Visual Studio's redistributable
# files): msvcp140 is not part of Windows or Python.
MSVC_RUNTIME = re.compile(r"^(msvcp140(_\w+)?|vcruntime140(_\w+)?|concrt140|vcomp140)\.dll$", re.I)
# prism.dll imports the Vulkan loader, which machines without a GPU driver
# lack: it ships (PRISM_VULKAN_LOADER, else System32's copy), as in
# tools/package_windows.ps1.
VULKAN_LOADER = "vulkan-1.dll"
# USD plug-ins (plugin/usd) the wheel carries: the shader definitions.
USD_PLUGINS = ("usdShaders", "sdrGlslfx")
# Provided by the interpreter.
PYTHON_DLL = re.compile(r"^python3\d*\.dll$", re.I)


# --- PE import tables -------------------------------------------------------

def pe_imports(path: Path) -> list[str]:
    """The DLL names a PE file imports, delay-loaded ones included."""
    data = path.read_bytes()
    if data[:2] != b"MZ":
        raise ValueError(f"{path}: not a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE file")
    sections_count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    sections = []
    for i in range(sections_count):
        base = optional + optional_size + 40 * i
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, base + 8)
        sections.append((virtual_address, max(virtual_size, raw_size), raw_pointer))

    def offset(rva: int) -> int:
        for address, size, pointer in sections:
            if address <= rva < address + size:
                return rva - address + pointer
        raise ValueError(f"{path}: RVA {rva:#x} outside the sections")

    def name_at(rva: int) -> str:
        start = offset(rva)
        return data[start:data.index(b"\0", start)].decode("ascii")

    names = []
    import_rva = struct.unpack_from("<I", data, directories + 8 * 1)[0]
    if import_rva:
        entry = offset(import_rva)
        while True:
            fields = struct.unpack_from("<IIIII", data, entry)
            if not any(fields):
                break
            names.append(name_at(fields[3]))
            entry += 20
    delay_rva = struct.unpack_from("<I", data, directories + 8 * 13)[0]
    if delay_rva:
        entry = offset(delay_rva)
        while True:
            fields = struct.unpack_from("<IIIIIIII", data, entry)
            if not any(fields):
                break
            names.append(name_at(fields[1]))
            entry += 32
    return names


# --- the build tree ---------------------------------------------------------

def cmake_cache(build: Path) -> dict[str, str]:
    values = {}
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"^([A-Za-z0-9_.\-]+):[A-Z]+=(.*)$", line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def load_build_config(path: Path) -> dict:
    """The CMake-generated _build_config.py (our own file) as a dict."""
    namespace: dict = {}
    exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), namespace)
    return namespace


def system_directory() -> Path:
    return Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"


def msvc_redist_directory() -> Path | None:
    """Visual Studio's newest x64 CRT redistributable folder."""
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.exists():
        return None
    install = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                             capture_output=True, text=True).stdout.strip()
    if not install:
        return None
    candidates = sorted(Path(install, "VC/Redist/MSVC").glob("*/x64/Microsoft.VC14*.CRT"))
    return candidates[-1] if candidates else None


class Resolver:
    """Finds each imported DLL in the build's DLL directories (in their
    order, as prismrt/_dll.py preloads the first copy) and walks the imports."""

    def __init__(self, directories: list[Path], crt: Path | None):
        self.directories = [d for d in directories if d.is_dir()]
        self.crt = crt
        self.system = system_directory()
        self.found: dict[str, Path] = {}       # lower-case name -> source file
        self.parents: dict[str, str] = {}      # lower-case name -> first importer
        self.missing: dict[str, str] = {}

    def locate(self, name: str) -> Path | None:
        key = name.lower()
        if MSVC_RUNTIME.match(name):
            for directory in ([self.crt] if self.crt else []) + [self.system]:
                if (directory / name).exists():
                    return directory / name
            return None
        if key == VULKAN_LOADER:
            for candidate in (os.environ.get("PRISM_VULKAN_LOADER"), self.system / name):
                if candidate and Path(candidate).is_file():
                    return Path(candidate)
            return None
        for directory in self.directories:
            candidate = directory / name
            if candidate.exists():
                return candidate
        return None

    def is_system(self, name: str) -> bool:
        lower = name.lower()
        if lower.startswith(("api-ms-win-", "ext-ms-")) or PYTHON_DLL.match(name):
            return True
        if MSVC_RUNTIME.match(name) or lower == VULKAN_LOADER:
            return False
        return (self.system / name).exists()

    def walk(self, root: Path) -> None:
        pending = [root]
        while pending:
            current = pending.pop()
            for name in pe_imports(current):
                key = name.lower()
                if key in self.found or key in self.missing or self.is_system(name):
                    continue
                source = self.locate(name)
                if source is None:
                    self.missing[key] = current.name
                    continue
                self.found[key] = source
                self.parents[key] = current.name
                pending.append(source)


# --- assembling -------------------------------------------------------------

def copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)


def copy_tree(source: Path, destination: Path, skip=(".pdb", ".lib", ".exp", ".ilk")) -> None:
    for path in sorted(source.rglob("*")):
        if path.is_file() and path.suffix.lower() not in skip and "__pycache__" not in path.parts:
            copy(path, destination / path.relative_to(source))


def read_version() -> str:
    text = (ROOT / "python/prismrt/_version.py").read_text(encoding="utf-8")
    return re.search(r'^__version__ = "([^"]+)"', text, re.M).group(1)


def source_date_epoch() -> int:
    if os.environ.get("SOURCE_DATE_EPOCH"):
        return int(os.environ["SOURCE_DATE_EPOCH"])
    completed = subprocess.run(["git", "-C", str(ROOT), "log", "-1", "--format=%ct"], capture_output=True, text=True)
    return int(completed.stdout.strip()) if completed.returncode == 0 and completed.stdout.strip() else 315532800


def licenses(stage: Path, runtime: dict, cache: dict, vcpkg_bin: Path, with_usd: bool) -> list[str]:
    """Copies the licence files of what the wheel bundles; returns the
    components without one."""
    out = stage / "prismrt/licenses"
    absent = []

    def take(component: str, *candidates) -> None:
        for candidate in candidates:
            if candidate and Path(candidate).is_file():
                copy(Path(candidate), out / f"{component}-{Path(candidate).name}")
                return
        absent.append(component)

    take("PrismRT", ROOT / "LICENSE")
    if (ROOT / "NOTICE").is_file():
        take("PrismRT-NOTICE", ROOT / "NOTICE")
    tbb_root = Path(runtime["tbbRuntime"]).parents[2] if runtime.get("tbbRuntime") else None
    take("oneTBB", tbb_root and tbb_root / "LICENSE.txt")
    if tbb_root and (tbb_root / "third-party-programs.txt").is_file():
        copy(tbb_root / "third-party-programs.txt", out / "oneTBB-third-party-programs.txt")
    embree_root = Path(runtime.get("embreeRuntime") or runtime.get("embreeBuildRuntime")).parent
    take("Embree", embree_root / "doc/LICENSE.txt", embree_root / "LICENSE.txt")
    if (embree_root / "doc/third-party-programs.txt").is_file():
        copy(embree_root / "doc/third-party-programs.txt", out / "Embree-third-party-programs.txt")
    if runtime.get("oidnRuntime"):
        doc = Path(runtime["oidnRuntime"]).parent / "share/doc/OpenImageDenoise"
        take("OpenImageDenoise", doc / "LICENSE.txt")
        for name in ("third-party-programs.txt", "third-party-programs-oneTBB.txt"):
            if (doc / name).is_file():
                copy(doc / name, out / f"OpenImageDenoise-{name}")
    if runtime.get("vkOidnRoot"):
        vk = Path(runtime["vkOidnRoot"])
        take("vk_oidn", vk / "LICENSE")
        take("vk_oidn-NOTICE", vk / "NOTICE")
        take("oidn-weights", vk / "prefilter/LICENSE-oidn-weights.txt")
    mdl = Path(runtime["mdlSdkRoot"])
    for name in ("LICENSE.md", "LICENSE_THIRDPARTY.md", "LICENSE_IMAGES.md"):
        take(f"MDL-SDK-{Path(name).stem}", mdl / name)
    materialx = Path(runtime["materialXRoot"])
    take("MaterialX", materialx / "LICENSE")
    if (materialx / "THIRD-PARTY.md").is_file():
        copy(materialx / "THIRD-PARTY.md", out / "MaterialX-THIRD-PARTY.md")
    subdiv = Path(runtime.get("openSubdivRoot") or "")
    take("OpenSubdiv", subdiv / "LICENSE.txt", ROOT / "tools/licenses/OpenSubdiv-LICENSE.txt")
    if with_usd:
        usd = Path(runtime["usdRoot"])
        take("OpenUSD", usd / "LICENSE.txt")
        take("OpenUSD-NOTICE", usd / "NOTICE.txt")
        for patch in sorted((usd / "patches").glob("*.patch")):
            copy(patch, out / "OpenUSD-patches" / patch.name)
    # DXC (NCSA) from the Vulkan SDK, whose licence file covers its components.
    dxc = Path(cache.get("PRISM_DXC_EXECUTABLE", ""))
    if dxc.is_file():
        take("DXC", ROOT / "tools/licenses/DXC-LICENSE.txt")
        sdk_license = dxc.parent.parent / "Licenses/LICENSE.txt"
        if sdk_license.is_file():
            copy(sdk_license, out / "VulkanSDK-LICENSE.txt")
    if (stage / "prismrt/bin" / VULKAN_LOADER).is_file():
        take("Vulkan-Loader", ROOT / "tools/licenses/Vulkan-Loader-LICENSE.txt")
    nanobind = Path(cache.get("nanobind_DIR", "")).parent.parent
    take("nanobind", *sorted(nanobind.glob("nanobind-*.dist-info/licenses/LICENSE")),
         nanobind / "LICENSE")
    take("tinygltf", ROOT / "external/tinygltf/LICENSE")
    take("tinyobjloader", ROOT / "external/tinyobj/LICENSE")
    # OpenImageIO, OpenColorIO and their dependencies (vcpkg's copyright files).
    share = vcpkg_bin.parent / "share"
    for port in sorted(share.iterdir()) if share.is_dir() else []:
        if (port / "copyright").is_file():
            copy(port / "copyright", out / "vcpkg" / f"{port.name}-copyright.txt")
    return absent


def packaged_config(usd: bool, msvc_minimum, usd_version) -> str:
    return f'''"""Written by tools/build_wheel.py for the wheel: paths relative to the
package (prismrt/_dll.py)."""
PACKAGED = True
DLL_DIRECTORIES = ["bin"]
ENVIRONMENT = {{
    "PRISM_MATERIALX_LIBRARY_PATH": "materialx/libraries",
    "PRISM_MATERIALX_MDL": "materialx/libraries/mdl",
    "PRISM_DXC": "bin/dxc.exe",
}}
PRELOAD = [{", ".join(repr("bin/" + name) for name in MDL_LIBRARIES)}]
USD_DLL_DIRECTORIES = {["usd/lib"] if usd else []!r}
USD_VERSION = {tuple(usd_version) if usd else ()!r}
USD_LIBRARY = {"usd/lib/usd_usd.dll" if usd else ""!r}
MSVC_RUNTIME_MINIMUM = {msvc_minimum!r}
'''


def write_path_marker(stage: Path, marker: Path) -> None:
    """prism_paths.txt beside prism.dll: the longest path of the wheel's files
    relative to site-packages (the stage), with its leading separator. The
    install root is two levels above bin (site-packages)."""
    longest_required = longest_all = 0
    for path in stage.rglob("*"):
        if not path.is_file():
            continue
        relative = "\\" + path.relative_to(stage).as_posix().replace("/", "\\")
        longest_all = max(longest_all, len(relative))
        if not relative.startswith("\\prismrt\\licenses\\"):
            longest_required = max(longest_required, len(relative))
    marker.write_text(f"root=..\\..\nrequired={longest_required}\nall={longest_all}\n", encoding="ascii")


def metadata(version: str) -> str:
    readme = (
        "PrismRT is a physically based path tracer (CPU, Vulkan, Metal). This package is its Python API:\n"
        "scenes from NumPy arrays, glTF, OBJ and USD files, MaterialX materials, CPU and GPU rendering.\n"
        "See https://github.com/joaovbs96/prism-rt and docs/python.md.\n")
    return (
        "Metadata-Version: 2.1\n"
        "Name: prismrt\n"
        f"Version: {version}\n"
        "Summary: PrismRT path tracer: Python API (CPU, Vulkan)\n"
        "Home-page: https://github.com/joaovbs96/prism-rt\n"
        "License: Apache-2.0\n"
        "Classifier: License :: OSI Approved :: Apache Software License\n"
        "Classifier: Operating System :: Microsoft :: Windows\n"
        "Classifier: Programming Language :: Python :: 3.11\n"
        "Classifier: Topic :: Multimedia :: Graphics :: 3D Rendering\n"
        "Requires-Python: >=3.11,<3.12\n"
        "Requires-Dist: numpy>=1.24\n"
        "Description-Content-Type: text/plain\n"
        "\n" + readme)


def write_wheel(stage: Path, out: Path, version: str, tag: str, epoch: int) -> Path:
    name = f"prismrt-{version}-{tag}.whl"
    dist_info = f"prismrt-{version}.dist-info"
    files: list[tuple[str, bytes | Path]] = []
    for path in sorted(stage.rglob("*")):
        if path.is_file():
            files.append((path.relative_to(stage).as_posix(), path))
    files.append((f"{dist_info}/METADATA", metadata(version).encode()))
    files.append((f"{dist_info}/WHEEL", (
        "Wheel-Version: 1.0\nGenerator: prismrt tools/build_wheel.py\nRoot-Is-Purelib: false\n"
        f"Tag: {tag}\n").encode()))
    files.append((f"{dist_info}/top_level.txt", b"prismrt\n"))
    timestamp = time.gmtime(max(epoch, 315532800))[:6]
    out.mkdir(parents=True, exist_ok=True)
    target = out / name
    record = io.StringIO()
    writer = csv.writer(record, lineterminator="\n")
    temporary = target.with_suffix(".tmp")
    with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for arcname, content in files:
            data = content.read_bytes() if isinstance(content, Path) else content
            info = zipfile.ZipInfo(arcname, date_time=timestamp)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, data, compresslevel=6)
            digest = base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b"=").decode()
            writer.writerow([arcname, f"sha256={digest}", len(data)])
        writer.writerow([f"{dist_info}/RECORD", "", ""])
        info = zipfile.ZipInfo(f"{dist_info}/RECORD", date_time=timestamp)
        info.external_attr = 0o644 << 16
        archive.writestr(info, record.getvalue().encode(), compress_type=zipfile.ZIP_DEFLATED)
    os.replace(temporary, target)
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--out", type=Path, help="output directory (default <build>/python_wheel/dist)")
    parser.add_argument("--no-usd", action="store_true", help="leave prismrt.usd and USD out")
    args = parser.parse_args()
    if sys.platform != "win32":
        print("build_wheel.py: Windows only so far (docs/python.md lists the Linux/macOS work)", file=sys.stderr)
        return 2

    build = args.build_dir.resolve()
    out = (args.out or build / "python_wheel/dist").resolve()
    stage = build / "python_wheel/stage"
    package_build = build / "python/prismrt"
    runtime = json.loads((build / "prism-runtime.json").read_text(encoding="utf-8"))
    cache = cmake_cache(build)
    config = load_build_config(build / "python_config" / args.config / "_build_config.py")
    version = read_version()
    with_usd = not args.no_usd
    extensions = sorted(package_build.glob("_prism*.pyd"))
    python_tag = f"cp{sys.version_info.major}{sys.version_info.minor}"
    if not extensions:
        raise SystemExit(f"no extension modules in {package_build}: build the prismrt_python target")
    for extension in extensions:
        if f".{python_tag}-" not in extension.name:
            raise SystemExit(f"{extension.name} is not a {python_tag} module: run this script with the build's Python")
    if not with_usd:
        extensions = [e for e in extensions if not e.name.startswith("_prism_usd")]

    if stage.exists():
        shutil.rmtree(stage)
    package = stage / "prismrt"

    # 1. The package.
    for path in sorted(package_build.iterdir()):
        # DLLs there are vcpkg's app-local copies; the import walk places them in bin.
        if path.name in ("_build_config.py", "__pycache__") or \
                path.suffix.lower() in (".pdb", ".lib", ".exp", ".dll"):
            continue
        if not with_usd and path.name.startswith(("_prism_usd", "usd.py")):
            continue
        if path.is_file():
            copy(path, package / path.name)
    usd_version = config.get("USD_VERSION", ())

    # 2. Native libraries: walk the imports from the extensions and the
    #    libraries loaded by name at run time.
    usd_root = Path(runtime["usdRoot"])
    usd_lib = usd_root / "lib"
    directories = [Path(d) for d in config["DLL_DIRECTORIES"]]
    mdl_bin = next(d for d in (Path(runtime["mdlSdkRoot"]) / "bin", Path(runtime["mdlSdkRoot"]) / "nt-x86-64/lib")
                   if (d / "libmdl_sdk.dll").exists())
    oidn = Path(runtime["oidnRuntime"]) if runtime.get("oidnRuntime") else None
    dxc = Path(cache.get("PRISM_DXC_EXECUTABLE", ""))
    directories += [mdl_bin] + ([oidn] if oidn else []) + ([dxc.parent] if dxc.is_file() else [])
    resolver = Resolver(directories, msvc_redist_directory())
    # The core first: everything it reaches goes to bin, and none of it may
    # come from the USD build (bin is all `import prismrt` adds).
    roots = [e for e in extensions if not e.name.startswith("_prism_usd")] + [mdl_bin / n for n in MDL_LIBRARIES]
    if oidn:
        if list(oidn.glob("tbb*.dll")):
            raise SystemExit(f"{oidn} holds a tbb DLL: use OIDN built against PrismRT's oneTBB (tools/ci/build_oidn.ps1)")
        roots += sorted(oidn.glob("OpenImageDenoise*.dll"))
    if dxc.is_file():
        roots += [dxc, dxc.parent / "dxcompiler.dll"]
    usd_roots = [e for e in extensions if e.name.startswith("_prism_usd")]
    plugins = usd_root / "plugin/usd"
    if with_usd:
        # Every library of the USD build: USD loads plug-in libraries (file
        # formats such as usdMtlx, schemas) by name on demand, beyond what
        # prism_usd imports. Of plugin/usd only the shader definitions
        # (Storm and its Hio image readers are usdview's, not ours).
        usd_roots += sorted(usd_lib.glob("*.dll"))
        usd_roots += [plugins / f"{name}.dll" for name in USD_PLUGINS if (plugins / f"{name}.dll").is_file()]

    placed: dict[str, Path] = {}

    def place(core: bool) -> None:
        for key, source in sorted(resolver.found.items()):
            if key in placed:
                continue
            from_usd = source.parent.resolve() == usd_lib.resolve()
            if core and from_usd:
                raise SystemExit(f"{source.name} (imported by {resolver.parents.get(key)}) comes from the USD build: "
                                 "the core must not need USD")
            destination = package / ("usd/lib" if from_usd else "bin") / source.name
            copy(source, destination)
            placed[key] = destination

    for core, files in ((True, roots), (False, usd_roots)):
        for root_file in files:
            if not root_file.is_file():
                raise SystemExit(f"missing {root_file}")
            resolver.walk(root_file)
            if root_file.suffix.lower() != ".pyd" and root_file.parent.resolve() != plugins.resolve():
                resolver.found.setdefault(root_file.name.lower(), root_file)
        if resolver.missing:
            details = ", ".join(f"{name} (imported by {parent})" for name, parent in sorted(resolver.missing.items()))
            raise SystemExit(f"unresolved imports: {details}")
        place(core)
    tbb = [k for k in placed if k.startswith("tbb12")]
    if len(tbb) != 1:
        raise SystemExit(f"expected one tbb12.dll, found {tbb}")

    # 3. USD's plug-in resources and plug-ins, beside its DLLs (USD finds
    #    lib/usd and plugin/usd relative to usd_plug.dll).
    if with_usd:
        copy_tree(usd_lib / "usd", package / "usd/lib/usd")
        if plugins.is_dir():
            if (plugins / "plugInfo.json").is_file():
                copy(plugins / "plugInfo.json", package / "usd/plugin/usd/plugInfo.json")
            for name in USD_PLUGINS:
                if (plugins / name).is_dir():
                    copy_tree(plugins / name, package / "usd/plugin/usd" / name)
                if (plugins / f"{name}.dll").is_file():
                    copy(plugins / f"{name}.dll", package / "usd/plugin/usd" / f"{name}.dll")

    # 4. Data.
    copy_tree(Path(runtime["materialXLibraries"]), package / "materialx/libraries")
    if with_usd:
        # The material preview's stage, found beside prism_usd's directory (bin/..).
        copy_tree(ROOT / "resources/shaderball", package / "resources/shaderball", skip=(".gitattributes",))
        copy(ROOT / "resources/material_preview.usda", package / "resources/material_preview.usda")

    # 5. The packaged configuration.
    (package / "_build_config.py").write_text(
        packaged_config(with_usd, config.get("MSVC_RUNTIME_MINIMUM"), usd_version), encoding="utf-8")

    # 6. Licences.
    vcpkg_bin = Path(cache.get("PRISM_VCPKG_BIN") or runtime["vcpkgRuntime"])
    absent = licenses(stage, runtime, cache, vcpkg_bin, with_usd)

    # 7. The longest installed paths, read by prismrt at import against Windows'
    #    260-character limit (src/renderer/install_paths.h, docs/python.md).
    write_path_marker(stage, package / "bin" / "prism_paths.txt")

    tag = f"{python_tag}-{python_tag}-{TAG_PLATFORM}"
    wheel = write_wheel(stage, out, version, tag, source_date_epoch())
    size = wheel.stat().st_size
    unpacked = sum(p.stat().st_size for p in stage.rglob("*") if p.is_file())
    print(f"wheel: {wheel}")
    print(f"size: {size / 2**20:.1f} MiB (unpacked {unpacked / 2**20:.1f} MiB)")
    print("DLLs: " + ", ".join(sorted(p.relative_to(package).as_posix() for p in placed.values())))
    if absent:
        print("licences not found: " + ", ".join(absent), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
