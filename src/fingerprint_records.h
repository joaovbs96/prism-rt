#pragma once

// Persistent fingerprint records: the content digest of a file, remembered
// across processes by the file's metadata, so a warm load does not read and
// hash every source again. A record is keyed by an identity string that the
// caller builds from the file's path, size and modification time plus
// whatever else selects its interpretation, and FileChangeStamp(): the
// file's change time and file ID, which every write updates even when the
// modification time is restored. A record names the artifact it vouches
// for and that artifact's own stamp, so a replaced or removed artifact is
// verified again. Records are small text files with a digest; a damaged or
// partial one is ignored.

#include <OpenImageIO/hash.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#ifdef _WIN32
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace prism::fingerprint_records {

// The file's change time and identity ("" if unavailable).
inline std::string FileChangeStamp(std::filesystem::path const &path)
{
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    FILE_BASIC_INFO basic{};
    BY_HANDLE_FILE_INFORMATION identity{};
    const bool ok = GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) &&
        GetFileInformationByHandle(file, &identity);
    CloseHandle(file);
    if (!ok) return {};
    std::ostringstream out;
    out << basic.ChangeTime.QuadPart << ':' << identity.dwVolumeSerialNumber << ':' << identity.nFileIndexHigh
        << ':' << identity.nFileIndexLow;
    return out.str();
#else
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return {};
    std::ostringstream out;
#ifdef __APPLE__
    out << info.st_ctimespec.tv_sec << '.' << info.st_ctimespec.tv_nsec;
#else
    out << info.st_ctim.tv_sec << '.' << info.st_ctim.tv_nsec;
#endif
    out << ':' << info.st_dev << ':' << info.st_ino;
    return out.str();
#endif
}

// Size, modification time and change stamp ("" if unavailable).
inline std::string FileStamp(std::filesystem::path const &path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    const std::string change = FileChangeStamp(path);
    if (change.empty()) return {};
    return std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count())) +
        ":" + change;
}

struct Record {
    std::string fingerprint;
    std::string artifact;      // file name, relative to the record's directory owner
    std::string artifactStamp; // FileStamp of the artifact when recorded
};

inline std::filesystem::path RecordPath(std::filesystem::path const &directory, std::string const &identity)
{
    return directory / "fingerprints" / (OIIO::SHA1::digest(identity.data(), identity.size()) + ".txt");
}

namespace detail {
inline std::string Body(std::string const &identity, Record const &record)
{
    return "prism-fingerprint-record-v1\n" + identity + "\n" + record.fingerprint + "\n" + record.artifact + "\n" +
        record.artifactStamp + "\n";
}
} // namespace detail

inline bool Read(std::filesystem::path const &directory, std::string const &identity, Record *record)
{
    std::ifstream in(RecordPath(directory, identity), std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string text = buffer.str();
    const size_t digestStart = text.rfind('\n', text.size() >= 2 ? text.size() - 2 : 0);
    if (digestStart == std::string::npos) return false;
    const std::string body = text.substr(0, digestStart + 1);
    std::string digest = text.substr(digestStart + 1);
    if (!digest.empty() && digest.back() == '\n') digest.pop_back();
    if (digest != OIIO::SHA1::digest(body.data(), body.size())) return false;
    std::istringstream lines(body);
    std::string magic, storedIdentity;
    Record result;
    if (!std::getline(lines, magic) || magic != "prism-fingerprint-record-v1") return false;
    // The identity may span lines; it is compared whole.
    const size_t identityLines = static_cast<size_t>(std::count(identity.begin(), identity.end(), '\n')) + 1;
    for (size_t i = 0; i < identityLines; ++i) {
        std::string line;
        if (!std::getline(lines, line)) return false;
        storedIdentity += (i ? "\n" : "") + line;
    }
    if (storedIdentity != identity) return false;
    if (!std::getline(lines, result.fingerprint) || !std::getline(lines, result.artifact) ||
        !std::getline(lines, result.artifactStamp) || result.fingerprint.empty())
        return false;
    *record = std::move(result);
    return true;
}

// Best effort: a record that cannot be written is simply not used.
inline void Write(std::filesystem::path const &directory, std::string const &identity, Record const &record)
{
    if (record.fingerprint.empty() || record.artifactStamp.empty()) return;
    const auto path = RecordPath(directory, identity);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    static std::atomic<uint64_t> serial{0};
#ifdef _WIN32
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    const auto temporary = std::filesystem::path(path.string() + "." + std::to_string(process) + "." +
                                                 std::to_string(++serial) + ".tmp");
    const std::string body = detail::Body(identity, record);
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << body << OIIO::SHA1::digest(body.data(), body.size()) << '\n';
        out.close();
        if (!out) {
            std::filesystem::remove(temporary, ec);
            return;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) std::filesystem::remove(temporary, ec);
}

} // namespace prism::fingerprint_records
