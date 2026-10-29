"""Finds the native libraries before the extension modules load.

Python 3.8+ on Windows ignores PATH for the dependencies of extension
modules, so the directories holding prism.dll, prism_usd.dll and their
dependencies (Embree, oneTBB, MaterialX, OpenImageIO, OpenColorIO, OIDN, the
MDL SDK; USD for prismrt.usd) are added with os.add_dll_directory:

- PRISMRT_DLL_PATH (os.pathsep-separated), if set;
- in a build tree, the directories CMake wrote into _build_config.py;
- the package directory itself (a packaged layout, migration step 8).

_build_config.py also names defaults for environment variables the renderer
reads (the MaterialX libraries, the MDL SDK root); variables already set
win.
"""

from __future__ import annotations

import os
import sys

_handles = []
DLL_DIRECTORIES: list[str] = []


def _configure() -> None:
    package = os.path.dirname(os.path.abspath(__file__))
    directories = []
    extra = os.environ.get("PRISMRT_DLL_PATH", "")
    directories += [d for d in extra.split(os.pathsep) if d]
    try:
        from . import _build_config  # generated in the build tree
    except ImportError:
        _build_config = None
    if _build_config is not None:
        directories += list(_build_config.DLL_DIRECTORIES)
        for name, value in _build_config.ENVIRONMENT.items():
            if value and not os.environ.get(name):
                os.environ[name] = value
    directories.append(package)
    seen = set()
    for directory in directories:
        key = os.path.normcase(os.path.abspath(directory))
        if key in seen or not os.path.isdir(directory):
            continue
        seen.add(key)
        DLL_DIRECTORIES.append(directory)
        if sys.platform == "win32":
            _handles.append(os.add_dll_directory(directory))
    if sys.platform == "win32":
        _preload_first_copies()


def _preload_first_copies() -> None:
    """Windows searches os.add_dll_directory directories in an unspecified
    order, so a DLL found in several of them (Embree ships an older tbb12.dll
    than oneTBB's) could come from any. Load the first directory's copy of
    each such DLL by its full path, as PATH order would."""
    import ctypes
    first: dict[str, str] = {}
    duplicated = []
    for directory in DLL_DIRECTORIES:
        try:
            names = os.listdir(directory)
        except OSError:
            continue
        for name in names:
            if not name.lower().endswith(".dll"):
                continue
            key = name.lower()
            if key not in first:
                first[key] = os.path.join(directory, name)
            elif key not in duplicated:
                duplicated.append(key)
    for key in duplicated:
        try:
            _preloaded.append(ctypes.WinDLL(first[key]))
        except OSError:
            pass  # loaded later by name, as before


_preloaded = []
_configure()
