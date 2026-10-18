#include "hlsl_compiler.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <thread>

namespace prism::device::gpu {

namespace {

int PCloseProcess(FILE* pipe)
{
#ifdef _WIN32
    return _pclose(pipe);
#else
    return pclose(pipe);
#endif
}

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

bool CompileHlsl(std::string const& source, HlslTarget target, std::filesystem::path const& workDirectory,
                 std::vector<char>* binary, std::string* error)
{
    const std::string stem = SourceDigest(source) +
        std::format(".{}", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const auto input = workDirectory / (stem + ".hlsl");
    const auto output = workDirectory / (stem + (target == HlslTarget::Dxil ? ".dxil" : ".spv"));
    std::ofstream(input, std::ios::binary).write(source.data(), std::streamsize(source.size()));
    // No DXC optimization: Metal's and the Vulkan drivers' compilers optimize,
    // and DXC's optimizer on these very large kernels is slow (its DXIL output
    // can also exceed what Metal compiles).
    const std::string targetOptions = target == HlslTarget::Dxil
        ? "-Od -Qstrip_debug -Qstrip_reflect"
        : "-spirv -fspv-target-env=vulkan1.3 -O0";
    const std::string command = std::format("\"{}\" -T cs_6_6 -E main -HV 2021 {} -Fo \"{}\" \"{}\" 2>&1",
                                            DxcExecutable(), targetOptions, output.string(), input.string());
#ifdef _WIN32
    // cmd.exe drops the outer quotes of a command that starts with a quote.
    FILE* pipe = _popen(("\"" + command + "\"").c_str(), "r");
#else
    FILE* pipe = popen(command.c_str(), "r");
#endif
    std::string log;
    char buffer[4096];
    while (pipe && fgets(buffer, sizeof(buffer), pipe)) log += buffer;
    const int status = pipe ? PCloseProcess(pipe) : -1;
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
