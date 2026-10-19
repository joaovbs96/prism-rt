#include "hlsl_compiler.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace prism::device::gpu {

namespace {

#ifdef _WIN32
// SPIR-V legalization of a kernel with very many materials can take tens of
// GB; DXC is stopped at PRISM_DXC_MEMORY_MIB, by default a quarter of the
// physical memory and at most 8 GiB.
uint64_t DxcMemoryLimit()
{
    if (const char* value = std::getenv("PRISM_DXC_MEMORY_MIB"); value && *value)
        return uint64_t(std::max(256ll, std::atoll(value))) << 20;
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    const uint64_t quarter = GlobalMemoryStatusEx(&status) ? status.ullTotalPhys / 4 : uint64_t(8) << 30;
    return std::min<uint64_t>(uint64_t(8) << 30, quarter);
}

// Runs a command line (no shell) in a job limited to `memoryLimit` bytes,
// capturing its output. Returns the exit code, or -1 if it did not start.
int RunLimited(std::string command, uint64_t memoryLimit, std::string* log, bool* overMemory)
{
    *overMemory = false;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return -1;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    // Only the pipe is inherited, so concurrent compiles keep no other's pipe open.
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
    HANDLE inherited[] = {writeEnd};
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                              nullptr, nullptr);
    STARTUPINFOEXA startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = writeEnd;
    startup.StartupInfo.hStdError = writeEnd;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup.StartupInfo, &process);
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        *log = std::format("cannot run {} (error {})", command, GetLastError());
        return -1;
    }
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.ProcessMemoryLimit = SIZE_T(memoryLimit);
    const bool limited = job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                                        sizeof(limits)) &&
                         AssignProcessToJobObject(job, process.hProcess);
    ResumeThread(process.hThread);
    char buffer[4096];
    DWORD bytes = 0;
    while (ReadFile(readEnd, buffer, sizeof(buffer), &bytes, nullptr) && bytes > 0) log->append(buffer, bytes);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = DWORD(-1);
    GetExitCodeProcess(process.hProcess, &code);
    if (limited && code != 0) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION used{};
        if (QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &used, sizeof(used), nullptr))
            *overMemory = uint64_t(used.PeakProcessMemoryUsed) + (uint64_t(64) << 20) >= memoryLimit;
    }
    if (job) CloseHandle(job);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readEnd);
    return int(code);
}
#else
int PCloseProcess(FILE* pipe) { return pclose(pipe); }
#endif

std::string DxcExecutable()
{
    if (const char* path = std::getenv("PRISM_DXC"); path && *path) return path;
#ifdef PRISM_DXC_EXECUTABLE
    return PRISM_DXC_EXECUTABLE;
#else
    return "dxc";
#endif
}

} // namespace

std::filesystem::path ShaderCacheDirectory(std::string const& device)
{
    std::filesystem::path root;
    if (const char* dir = std::getenv("PRISM_CACHE_DIR"); dir && *dir) root = dir;
#ifdef __APPLE__
    else if (const char* home = std::getenv("HOME")) root = std::filesystem::path(home) / "Library/Caches/PrismRT";
#else
    else if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) root = std::filesystem::path(local) / "PrismRT";
    else if (const char* home = std::getenv("HOME")) root = std::filesystem::path(home) / ".cache/PrismRT";
#endif
    else root = std::filesystem::temp_directory_path() / "PrismRT";
    root /= device;
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    return root;
}

std::string SourceDigest(std::string const& text)
{
    uint64_t a = 1469598103934665603ull, b = 0x9e3779b97f4a7c15ull;
    for (unsigned char c : text) {
        a = (a ^ c) * 1099511628211ull;
        b = (b + c) * 0x100000001b3ull ^ (b >> 29);
    }
    return std::format("{:016x}{:016x}-{}", a, b, text.size());
}

namespace {

constexpr const char* kTooLarge = "exceeded its memory limit";

// No DXC optimization: Metal's and the Vulkan drivers' compilers optimize,
// and DXC's optimizer on these very large kernels is slow (its DXIL output
// can also exceed what Metal compiles).
// SPIR-V legalization inlines every function and runs out of the default
// id bound (0x3FFFFF) on kernels with many materials; DXC then crashes
// ("Internal compiler error: stack overflow"). Compacting the ids at the
// end brings the final module's bound back down to a fraction of that.
const char* TargetOptions(HlslTarget target)
{
    return target == HlslTarget::Dxil ? "-Od -Qstrip_debug -Qstrip_reflect"
                                      : "-spirv -fspv-target-env=vulkan1.3 -O0 -fspv-max-id 268435455";
}

} // namespace

std::string HlslCompilerSignature(HlslTarget target)
{
    std::string signature = DxcExecutable() + " -T cs_6_6 -E main -HV 2021 " + TargetOptions(target);
#ifdef _WIN32
    signature += std::format(" memory {}", DxcMemoryLimit());
#endif
    return signature;
}

bool HlslTooLarge(std::string const& error) { return error.find(kTooLarge) != std::string::npos; }

bool CompileHlsl(std::string const& source, HlslTarget target, std::filesystem::path const& workDirectory,
                 std::vector<char>* binary, std::string* error)
{
    const std::string stem = SourceDigest(source) +
        std::format(".{}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto input = workDirectory / (stem + ".hlsl");
    const auto output = workDirectory / (stem + (target == HlslTarget::Dxil ? ".dxil" : ".spv"));
    std::ofstream(input, std::ios::binary).write(source.data(), std::streamsize(source.size()));
    const std::string targetOptions = TargetOptions(target);
    const std::string command = std::format("\"{}\" -T cs_6_6 -E main -HV 2021 {} -Fo \"{}\" \"{}\"",
                                            DxcExecutable(), targetOptions, output.string(), input.string());
    std::string log;
#ifdef _WIN32
    const uint64_t memoryLimit = DxcMemoryLimit();
    bool overMemory = false;
    const int status = RunLimited(command, memoryLimit, &log, &overMemory);
    if (overMemory)
        log = std::format("DXC {} of {} MiB (PRISM_DXC_MEMORY_MIB): the kernel of this scene's materials is "
                          "too large to compile", kTooLarge, memoryLimit >> 20);
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    char buffer[4096];
    while (pipe && fgets(buffer, sizeof(buffer), pipe)) log += buffer;
    const int status = pipe ? PCloseProcess(pipe) : -1;
#endif
    std::ifstream file(output, std::ios::binary);
    binary->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    file.close();
    std::error_code ignored;
    std::filesystem::remove(output, ignored);
    if (status != 0 || binary->empty()) {
        // Keep the source for inspection.
        std::filesystem::rename(input, workDirectory / "failed_kernel.hlsl", ignored);
        if (error) *error = "DXC failed: " + log.substr(0, 4000);
        return false;
    }
    std::filesystem::remove(input, ignored);
    return true;
}

} // namespace prism::device::gpu
