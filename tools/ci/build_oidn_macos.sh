#!/usr/bin/env bash
# Builds Intel Open Image Denoise (OIDN) on macOS (Apple silicon) with its
# CPU and Metal devices, from Intel's source release, against PrismRT's
# oneTBB. The macOS twin of tools/ci/build_oidn.ps1 (docs/denoising.md).
#
# Versions and checksums come from tools/deps.json (oidn). The script
# downloads, from Intel's GitHub releases only,
#   https://github.com/RenderKit/oidn/releases/download/v<version>/oidn-<version>.src.tar.gz
#   https://github.com/ispc/ispc/releases/download/v<ispc>/ispc-v<ispc>-macOS.universal.tar.gz
# verifies their SHA-256 and builds Release into PREFIX
# (lib/libOpenImageDenoise*.dylib, lib/cmake/OpenImageDenoise-<version>).
#
# The Metal device (OIDN_DEVICE_METAL) is what the Metal render device
# denoises with; it needs Xcode 15 or newer with the Metal toolchain
# (`xcrun metal`; Command Line Tools alone do not have it, and Xcode 26+
# installs it with `xcodebuild -downloadComponent MetalToolchain`) and the
# macOS 14 SDK or newer. No RTLightmap weights and no apps, as on Windows.
#
# Usage:
#   tools/ci/build_oidn_macos.sh --work-dir ~/prism-deps/src --prefix ~/prism-deps/oidn-2.5.1 \
#       [--tbb-root <oneTBB prefix PrismRT uses>] [--jobs 8]
# --tbb-root: the oneTBB PrismRT links (PRISM_TBB_ROOT, or e.g. `brew --prefix tbb`);
# OIDN's CPU device must share the process's oneTBB. Default: CMake's search.
set -euo pipefail

work_dir="" prefix="" tbb_root="" jobs=8
while [ $# -gt 0 ]; do
    case "$1" in
        --work-dir) work_dir="$2"; shift 2 ;;
        --prefix) prefix="$2"; shift 2 ;;
        --tbb-root) tbb_root="$2"; shift 2 ;;
        --jobs) jobs="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [ -z "$work_dir" ] || [ -z "$prefix" ]; then
    echo "usage: $0 --work-dir DIR --prefix DIR [--tbb-root DIR] [--jobs N]" >&2
    exit 2
fi
if [ "$(uname)" != "Darwin" ]; then
    echo "build_oidn_macos.sh runs on macOS; on Windows use tools/ci/build_oidn.ps1" >&2
    exit 1
fi

root="$(cd "$(dirname "$0")/../.." && pwd)"
dep() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['oidn'][sys.argv[2]])" "$root/tools/deps.json" "$1"; }
version="$(dep version)"
tag="$(dep tag)"
source_sha256="$(dep source_sha256)"
ispc_version="$(dep ispc)"
ispc_sha256="$(dep ispc_macos_sha256)"

# OIDN's Metal kernels are compiled with xcrun metal at build time.
if ! xcrun --sdk macosx --find metal >/dev/null 2>&1; then
    echo "xcrun cannot find the Metal compiler: install Xcode (15 or newer; on Xcode 26+ also" >&2
    echo "'xcodebuild -downloadComponent MetalToolchain') and run with" >&2
    echo "DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer (prism_env.sh points DEVELOPER_DIR" >&2
    echo "at the Command Line Tools, which have no Metal compiler)." >&2
    exit 1
fi

mkdir -p "$work_dir" "$prefix"
work_dir="$(cd "$work_dir" && pwd)"
prefix="$(cd "$prefix" && pwd)"

fetch() {  # fetch URL FILE SHA256
    if [ ! -f "$2" ]; then
        echo "Downloading $1"
        curl -fL --retry 3 -o "$2.part" "$1"
        mv "$2.part" "$2"
    fi
    local hash
    hash="$(shasum -a 256 "$2" | cut -d' ' -f1 | tr '[:lower:]' '[:upper:]')"
    if [ "$hash" != "$(echo "$3" | tr '[:lower:]' '[:upper:]')" ]; then
        echo "$2 has SHA-256 $hash, tools/deps.json says $3" >&2
        exit 1
    fi
}

ispc_archive="$work_dir/ispc-v$ispc_version-macOS.universal.tar.gz"
fetch "https://github.com/ispc/ispc/releases/download/v$ispc_version/ispc-v$ispc_version-macOS.universal.tar.gz" \
    "$ispc_archive" "$ispc_sha256"
ispc_root="$work_dir/ispc-v$ispc_version-macOS.universal"
[ -x "$ispc_root/bin/ispc" ] || tar -xzf "$ispc_archive" -C "$work_dir"
# Downloaded with curl, so not quarantined; clear the attribute anyway in
# case the archive came through a browser.
xattr -dr com.apple.quarantine "$ispc_root" 2>/dev/null || true

archive="$work_dir/oidn-$version.src.tar.gz"
fetch "https://github.com/RenderKit/oidn/releases/download/$tag/oidn-$version.src.tar.gz" "$archive" "$source_sha256"
source="$work_dir/oidn-$version"
[ -f "$source/CMakeLists.txt" ] || tar -xzf "$archive" -C "$work_dir"

build="$work_dir/oidn-$version-build-macos"
tbb_args=()
[ -n "$tbb_root" ] && tbb_args=("-DTBB_ROOT=$tbb_root")
cmake -S "$source" -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    "-DCMAKE_INSTALL_PREFIX=$prefix" \
    "-DISPC_EXECUTABLE=$ispc_root/bin/ispc" \
    ${tbb_args[@]+"${tbb_args[@]}"} \
    -DOIDN_DEVICE_CPU=ON -DOIDN_DEVICE_METAL=ON \
    -DOIDN_FILTER_RT=ON -DOIDN_FILTER_RTLIGHTMAP=OFF \
    -DOIDN_APPS=OFF -DOIDN_INSTALL_DEPENDENCIES=OFF
cmake --build "$build" --parallel "$jobs"
cmake --install "$build"

# PrismRT's process loads one oneTBB: never install a second one.
if ls "$prefix/lib" | grep -q '^libtbb'; then
    echo "$prefix/lib contains a oneTBB library; OIDN must use PrismRT's oneTBB" >&2
    exit 1
fi
if ! ls "$prefix/lib" | grep -q 'OpenImageDenoise_device_metal'; then
    echo "$prefix/lib has no OpenImageDenoise_device_metal library: the Metal device was not built" >&2
    exit 1
fi
echo "OIDN $version (CPU and Metal devices) installed to $prefix"
ls "$prefix/lib" | grep OpenImageDenoise | sed 's/^/  /'
