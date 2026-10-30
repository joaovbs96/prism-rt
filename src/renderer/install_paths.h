#pragma once

// Windows' 260-character path limit (docs/deployment.md, "Path length").
//
// A relocatable install (the Windows package, the Python wheel) carries files
// whose paths grow with the folder it is extracted to: the MaterialX
// libraries, the MDL modules, USD's plug-in resources, the shaderball. Past
// MAX_PATH, a process that is not long-path aware cannot open them (MaterialX
// reads its libraries with std::ifstream: "Failed to open file for reading").
// Rather than failing deep inside material loading, the front ends call
// CheckInstallPathLength() once at startup.
//
// The packaging scripts write a small text file, prism_paths.txt, beside
// prism.dll (tools/package_windows.ps1, tools/build_wheel.py):
//
//     root=..          the install root, relative to the file's directory
//     required=<N>     the longest path of the files PrismRT needs, relative
//                      to the root, with its leading separator, in characters
//     all=<N>          the same for every file of the install
//
// A build tree or a plain `cmake --install` has no such file and is not
// checked: the check costs one small file read, never a directory walk.
//
// PRISM_SKIP_PATH_CHECK=1 turns the check off.

#include "../internal_export.h"
#include <prism/options.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace prism {

// The longest path (in characters, without the terminating NUL) the Win32
// ANSI/wide APIs accept without long-path support: MAX_PATH - 1.
inline constexpr std::size_t kWindowsPathLimit = 259;

struct InstallPaths {
    std::filesystem::path root;
    std::size_t required = 0;   // longest relative path PrismRT needs
    std::size_t all = 0;        // longest relative path of the whole install
};

// Parses the text of prism_paths.txt; root is made absolute against
// markerDirectory. False when the text has no usable required length.
PRISM_INTERNAL_API bool ParseInstallPaths(std::string const& text,
                                          std::filesystem::path const& markerDirectory,
                                          InstallPaths& out);

// The pure decision: an empty string when root plus `relativeLength` fits in
// the Windows limit (or long paths work), otherwise one message that names the
// longest path, the limit and the two remedies. `relativeLength` includes the
// leading separator.
PRISM_INTERNAL_API std::string InstallPathProblem(std::filesystem::path const& root,
                                                  std::size_t relativeLength,
                                                  bool longPathsWork);

// True when this process opens paths past MAX_PATH (Windows: the system's
// LongPathsEnabled setting and the executable's longPathAware manifest, as
// RtlAreLongPathsEnabled reports; always true elsewhere).
PRISM_INTERNAL_API bool LongPathsWork();

// CheckInstallPathLength (include/prism/options.h) is the startup check: it
// finds prism_paths.txt beside the module that holds this code (prism.dll)
// and returns InstallPathProblem for it.

} // namespace prism
