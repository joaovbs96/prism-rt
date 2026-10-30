#include "install_paths.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace prism {

namespace {

// "key=value" lines; unknown keys and blank lines are ignored.
bool parseSize(std::string const& value, std::size_t& out)
{
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos || value.size() > 9)
        return false;
    out = static_cast<std::size_t>(std::stoul(value));
    return true;
}

#ifdef _WIN32
// LongPathsEnabled in the registry, regardless of this process's manifest.
bool systemLongPathsEnabled()
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                        L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
}
#endif

} // namespace

bool ParseInstallPaths(std::string const& text, std::filesystem::path const& markerDirectory,
                       InstallPaths& out)
{
    InstallPaths paths;
    std::string relativeRoot = "..";
    bool haveRequired = false;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        auto const equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string const key = line.substr(0, equals);
        std::string const value = line.substr(equals + 1);
        if (key == "root") relativeRoot = value;
        else if (key == "required") haveRequired = parseSize(value, paths.required);
        else if (key == "all") parseSize(value, paths.all);
    }
    if (!haveRequired) return false;
    if (paths.all < paths.required) paths.all = paths.required;
    // Lexical, so the result is the path the user sees (no symlink or 8.3 rewriting).
    paths.root = (markerDirectory / std::filesystem::path(relativeRoot)).lexically_normal();
    while (!paths.root.empty() && !paths.root.has_filename() && paths.root != paths.root.root_path())
        paths.root = paths.root.parent_path();
    out = paths;
    return true;
}

std::string InstallPathProblem(std::filesystem::path const& root, std::size_t relativeLength,
                               bool longPathsWork)
{
    if (longPathsWork) return {};
    std::size_t const rootLength = root.native().size();
    std::size_t const total = rootLength + relativeLength;
    if (total <= kWindowsPathLimit) return {};

    std::ostringstream message;
    message << "PrismRT is installed in a folder whose path is too long for Windows: "
            << root.string() << "\n"
            << "The longest file of the installation would have " << total
            << " characters (the limit is " << kWindowsPathLimit + 1
            << "), and PrismRT would fail to open its MaterialX libraries and other resources.\n"
            << "Move the installation to a shorter folder (for example C:\\PrismRT; at most "
            << (kWindowsPathLimit > relativeLength ? kWindowsPathLimit - relativeLength : 0)
            << " characters for its path), or enable Windows long paths: set the DWORD "
               "HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\\LongPathsEnabled to 1 "
               "(administrator rights), then restart. See docs/deployment.md, \"Path length\".";
    return message.str();
}

bool LongPathsWork()
{
#ifdef _WIN32
    // ntdll's RtlAreLongPathsEnabled: true when the registry setting is on and
    // this executable's manifest declares longPathAware.
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using Fn = BOOLEAN(NTAPI*)();
        auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlAreLongPathsEnabled")));
        if (fn) return fn() != FALSE;
    }
    return false;
#else
    return true;
#endif
}

std::string CheckInstallPathLength(bool includeAll)
{
#ifdef _WIN32
    char const* skip = std::getenv("PRISM_SKIP_PATH_CHECK");
    if (skip && skip[0] && std::string(skip) != "0") return {};

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&CheckInstallPathLength), &module))
        return {};
    std::wstring buffer(1024, L'\0');
    for (;;) {
        DWORD const n = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) { buffer.resize(n); break; }
        buffer.resize(buffer.size() * 2);
    }
    // Strip the \\?\ prefix a long module path may carry.
    if (buffer.rfind(L"\\\\?\\", 0) == 0) buffer.erase(0, 4);
    std::filesystem::path const directory = std::filesystem::path(buffer).parent_path();

    std::ifstream file(std::filesystem::path(L"\\\\?\\" + (directory / "prism_paths.txt").wstring()),
                       std::ios::binary);
    if (!file) return {};
    std::ostringstream text;
    text << file.rdbuf();
    InstallPaths paths;
    if (!ParseInstallPaths(text.str(), directory, paths)) return {};
    if (LongPathsWork()) return {};

    std::string message = InstallPathProblem(paths.root, includeAll ? paths.all : paths.required, false);
    if (!message.empty() && systemLongPathsEnabled())
        message += "\nWindows long paths are enabled, but this program is not long-path aware "
                   "(its manifest lacks longPathAware): move the installation to a shorter folder.";
    return message;
#else
    (void)includeAll;
    return {};
#endif
}

} // namespace prism
