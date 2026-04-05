import sys
import os
import subprocess
import shutil

# Visual Studio paths
VS_SHELL = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"

# Environment variables
VCPKG_ROOT = os.environ.get("VCPKG_ROOT", "")

# Base directories
PROGRAMMING_PROJECTS = "C:/Users/joaov/Desktop/Programming_Projects"
EXTERNAL_DEPS = f"{PROGRAMMING_PROJECTS}/external_deps"

# Embree
EMBREE_DIR = f"{PROGRAMMING_PROJECTS}/Embree/build/Release"

# TBB (oneTBB)
# NOTE: TBB_ROOT points to the root of the oneTBB installation.
# TBB_INCLUDE_DIRS should be the directory that CONTAINS the "tbb" folder,
# i.e. the parent of "tbb/tbb.h"
TBB_ROOT = f"{PROGRAMMING_PROJECTS}/include/tbb"
TBB_DIR = f"{TBB_ROOT}/lib/cmake/tbb"
TBB_INCLUDE_DIRS = f"{TBB_ROOT}/include"
# The actual .lib file lives here - needed so USD's FindTBB doesn't pick up
# the wrong one from a system path or the USD-bundled legacy TBB.
TBB_LIB_DIR = f"{TBB_ROOT}/lib"

# OpenSubdiv
OSD_ROOT = f"{EXTERNAL_DEPS}/OpenSubdiv"
OSD_CMAKE_PATH = f"{OSD_ROOT}/install/lib/cmake/OpenSubdiv"
OSD_INCLUDE_PATH = f"{OSD_ROOT}/install/include/opensubdiv"

# MaterialX
MTLX_ROOT = f"{EXTERNAL_DEPS}/MaterialX"
MTLX_CMAKE_PATH = f"{MTLX_ROOT}/install/lib/cmake/MaterialX"

# OpenUSD
USD_ROOT = f"{EXTERNAL_DEPS}/OpenUSD"
USD_CMAKE_PATH = f"{USD_ROOT}/install"
USD_INCLUDE_PATH = f"{USD_ROOT}/install/include/pxr"

# ── STALE USD INSTALL CHECK ──────────────────────────────────────────────────
# Your build errors reference headers from a DIFFERENT path:
#   C:\Users\joaov\Desktop\Programming_Projects\OpenUSD\install\include\tbb\
# This is NOT the same as EXTERNAL_DEPS/OpenUSD. If this old install exists,
# CMake's find_package(pxr) may find it first and pull in its bundled legacy
# TBB headers, causing the 'split' ambiguity and missing tbb::task errors.
STALE_USD_PATH = f"{PROGRAMMING_PROJECTS}/OpenUSD"

# OSL
OSL_DIR = "C:/osl_install/lib/cmake/OSL"

# MDL SDK
MDL_SDK_ROOT = f"{PROGRAMMING_PROJECTS}/mdl-sdk"
MDL_SDK_DIR = MDL_SDK_ROOT

# Common TBB options passed to every build that needs TBB.
COMMON_TBB_OPTIONS = (
    f'-DTBB_ROOT="{TBB_ROOT}" '
    f'-DTBB_DIR="{TBB_DIR}" '
    f'-DTBB_INCLUDE_DIRS="{TBB_INCLUDE_DIRS}" '
    f'-DCMAKE_PREFIX_PATH="{TBB_ROOT}" '
)

# Extended TBB options specifically for OpenUSD.
#
# KEY FIX: The old flag -DPXR_USE_INTERNAL_TBB=OFF only controls whether USD
# builds its own bundled TBB from source — it does NOT switch USD to the
# oneTBB API. The correct flag from Pixar's own build_usd.py is:
#
#   -DPXR_FIND_TBB_IN_CONFIG=ON
#
# This tells USD to find TBB via CMake config mode (TBBConfig.cmake from
# oneTBB) instead of its legacy FindTBB.cmake module. This is what makes
# USD's "work" library use the oneTBB task API (tbb::task_group) instead of
# the removed legacy API (tbb::task, tbb::empty_task, spawn, etc.).
USD_TBB_OPTIONS = (
    # All the standard hints
    f'-DTBB_ROOT="{TBB_ROOT}" '
    f'-DTBB_DIR="{TBB_DIR}" '
    f'-DTBB_INCLUDE_DIRS="{TBB_INCLUDE_DIRS}" '
    # Explicitly set include/lib so USD's FindTBB.cmake doesn't go hunting
    f'-DTBB_INCLUDE_DIR="{TBB_INCLUDE_DIRS}" '
    f'-DTBB_LIBRARY="{TBB_LIB_DIR}/tbb12.lib" '
    f'-DTBB_LIBRARY_DEBUG="{TBB_LIB_DIR}/tbb12_debug.lib" '
    # Tell CMake where to find the TBBConfig.cmake / tbb-config.cmake
    f'-DCMAKE_PREFIX_PATH="{TBB_ROOT}" '
    # *** THE KEY FLAG: use oneTBB config mode instead of legacy FindTBB ***
    '-DPXR_FIND_TBB_IN_CONFIG=ON '
    # Don't use debug TBB for RelWithDebInfo builds
    '-DTBB_USE_DEBUG_BUILD=OFF '
    # oneTBB ships TBBConfig.cmake; point find_package() at it directly
    f'-DCMAKE_MODULE_PATH="{TBB_DIR}" '
)

# Config mapping for multi-config generators (Visual Studio).
# Tells CMake: "If you can't find a specific configuration for an imported
# dependency, fall back to these alternatives."
CONFIG_MAP_OPTIONS = (
    "-DCMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO=RelWithDebInfo;Release "
    "-DCMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL=RelWithDebInfo;Release "
    "-DCMAKE_MAP_IMPORTED_CONFIG_RELEASE=Release;RelWithDebInfo "
    "-DCMAKE_MAP_IMPORTED_CONFIG_DEBUG=Debug;RelWithDebInfo;Release "
)

def clean_build_dirs(*roots: str):
    """Delete the 'build' and 'install' subdirectories under each given root."""
    for root in roots:
        for folder in ("build", "install"):
            path = os.path.join(root.replace("/", os.sep), folder)
            if os.path.exists(path):
                print(f"  Removing {path} ...")
                shutil.rmtree(path)
            else:
                print(f"  Skipping {path} (does not exist)")


def run_vs_shell(cmd: str):
    """Run command in Visual Studio developer command prompt environment"""
    temp_bat = os.path.join(os.path.dirname(__file__), "build_script_temp.bat")

    # Write batch file
    with open(temp_bat, "w") as f:
        f.write('@echo off\n')
        f.write(f'call "{VS_SHELL}"\n')
        f.write('if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%\n')
        f.write(f'{cmd}\n')
        f.write('if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%\n')

    print(f"Running: {cmd}")

    try:
        result = subprocess.run(
            temp_bat,
            shell=True,
            check=True
        )
        return result.returncode
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] Command failed with exit code {e.returncode}")
        raise
    finally:
        if os.path.exists(temp_bat):
            try:
                os.remove(temp_bat)
            except:
                pass


# # ── Warn about stale USD install ──────────────────────────────────────────────
# stale_install = os.path.join(STALE_USD_PATH.replace("/", os.sep), "install")
# if os.path.exists(stale_install):
#     print("\n" + "!" * 60)
#     print("WARNING: Found old OpenUSD install at:")
#     print(f"  {stale_install}")
#     print("")
#     print("This is NOT the same as your external_deps/OpenUSD path.")
#     print("CMake may find this first and pull in its bundled legacy TBB,")
#     print("causing 'split' ambiguity and tbb::task errors.")
#     print("")
#     print("Options:")
#     print("  1. Delete or rename it (recommended)")
#     print("  2. Continue anyway (build may fail)")
#     print("!" * 60)
#     response = input("Delete it now? [y/N]: ").strip().lower()
#     if response == 'y':
#         print(f"  Removing {stale_install} ...")
#         shutil.rmtree(stale_install)
#         print("  Done.")
#     else:
#         print("  Continuing without deleting (build may fail)...")


# # ── Clean all build/install directories upfront ───────────────────────────────
# print("\n" + "="*60)
# print("Cleaning build and install directories")
# print("="*60)
# clean_build_dirs(OSD_ROOT, MTLX_ROOT, USD_ROOT)


# ── OpenSubdiv ────────────────────────────────────────────────────────────────
print("\n" + "="*60)
print("Building OpenSubdiv")
print("="*60)

osd_options = COMMON_TBB_OPTIONS
osd_options += "-DNO_TUTORIALS=1 -DNO_EXAMPLES=1 -DNO_REGRESSION=1 "

run_vs_shell(f'cd /d "{OSD_ROOT}" && cmake -B build {osd_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(f'cd /d "{OSD_ROOT}" && cmake --build build --target install --config RelWithDebInfo')


# ── MaterialX ─────────────────────────────────────────────────────────────────
print("\n" + "="*60)
print("Building MaterialX")
print("="*60)

mtlx_options = COMMON_TBB_OPTIONS
mtlx_options += "-DMATERIALX_BUILD_GEN_MDL=ON "
mtlx_options += "-DMATERIALX_BUILD_GEN_GLSL=ON -DMATERIALX_BUILD_GEN_OSL=ON "
mtlx_options += "-DMATERIALX_BUILD_GEN_ESSL=ON -DMATERIALX_BUILD_GEN_MSL=ON "
mtlx_options += "-DMATERIALX_BUILD_PYTHON=OFF -DMATERIALX_BUILD_VIEWER=OFF "
mtlx_options += "-DMATERIALX_BUILD_GRAPH_EDITOR=OFF -DMATERIALX_BUILD_RENDER=ON "
mtlx_options += "-DMATERIALX_BUILD_TESTS=OFF -DMATERIALX_INSTALL_PYTHON=OFF "

run_vs_shell(f'cd /d "{MTLX_ROOT}" && cmake -B build {mtlx_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(f'cd /d "{MTLX_ROOT}" && cmake --build build --target install --config RelWithDebInfo')


# ── OpenUSD ───────────────────────────────────────────────────────────────────
print("\n" + "="*60)
print("Building OpenUSD")
print("="*60)

usd_options = USD_TBB_OPTIONS  # Uses PXR_FIND_TBB_IN_CONFIG=ON for oneTBB
usd_options += CONFIG_MAP_OPTIONS
usd_options += f'-DOpenSubdiv_DIR="{OSD_CMAKE_PATH}" -DOPENSUBDIV_INCLUDE_DIR="{OSD_INCLUDE_PATH}" '
usd_options += f'-DMaterialX_DIR="{MTLX_CMAKE_PATH}" '
usd_options += "-DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF "
usd_options += "-DPXR_BUILD_GPU_SUPPORT=OFF "
usd_options += "-DPXR_ENABLE_GL_SUPPORT=OFF "
usd_options += "-DPXR_BUILD_USD_TOOLS=OFF -DPXR_ENABLE_PYTHON_SUPPORT=OFF "
usd_options += "-DPXR_ENABLE_MATERIALX_SUPPORT=ON "
usd_options += "-DPXR_ENABLE_OPENVDB_SUPPORT=OFF "

run_vs_shell(f'cd /d "{USD_ROOT}" && cmake -B build {usd_options} -DCMAKE_INSTALL_PREFIX=install -G "Visual Studio 17 2022" -A x64 -S .')
run_vs_shell(f'cd /d "{USD_ROOT}" && cmake --build build --target install --config RelWithDebInfo')


# ── Post-build: remove legacy TBB headers from USD install if present ─────────
# Some USD versions install a copy of old TBB headers under <install>/include/tbb/.
# These conflict with oneTBB at compile time for downstream consumers.
# If USD installed them, remove them so PrismRT only sees the oneTBB headers.
usd_tbb_headers = os.path.join(USD_CMAKE_PATH.replace("/", os.sep), "include", "tbb")
if os.path.exists(usd_tbb_headers):
    print(f"\n  Removing legacy TBB headers from USD install: {usd_tbb_headers}")
    shutil.rmtree(usd_tbb_headers)
    print("  Done — downstream builds will use only oneTBB headers.")


print("\n" + "="*60)
print("All dependencies built successfully!")
print("="*60)