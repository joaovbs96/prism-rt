// One copy of the core per process (docs/sdk_design.md, section 4.1): the
// core `prism` and the USD module `prism_usd` are shared libraries that the
// CLI, hdPrism and this process all load, instead of each embedding its own
// copy of their state (above all the MDL SDK runtime, SharedMdlContext).
//   prism_single_runtime_test <prism.dll> <prism_usd.dll> <PrismRT.exe> <hdPrism.dll>
// Checks, on the files: every consumer imports the shared libraries, and the
// MDL runtime's code (a string of src/mdl/context.cpp) and the USD module's
// service (a warning of src/materialx/material_service.cpp) are in
// prism.dll / prism_usd.dll only, not linked statically into a consumer. In
// this process, after loading hdPrism.dll: one prism.dll and one
// prism_usd.dll are loaded, and exactly one module exports the MDL runtime
// accessor and the material service's registry, the same one this test calls.
// Windows only (exit code 77 elsewhere).

#include "../src/materialx/captured_material.h"
#include "../src/materialx/shared_mdl_context.h"
#include "../src/progress.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

int failures = 0;
void check(bool condition, std::string const& message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    } else {
        std::cout << "ok: " << message << '\n';
    }
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

std::vector<char> ReadFile(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string Reversed(std::string text)
{
    std::reverse(text.begin(), text.end());
    return text;
}

bool Contains(std::vector<char> const& bytes, std::string const& text)
{
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
}

#ifdef _WIN32
// The DLL names a PE file imports (its import directory, read from the file).
std::vector<std::string> Imports(std::vector<char> const& file)
{
    std::vector<std::string> names;
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return names;
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || size_t(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
        return names;
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS64 const*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return names;
    auto const* sections = IMAGE_FIRST_SECTION(nt);
    auto offset = [&](uint32_t rva) -> size_t {
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            auto const& s = sections[i];
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData))
                return size_t(rva - s.VirtualAddress) + s.PointerToRawData;
        }
        return SIZE_MAX;
    };
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    size_t at = offset(directory.VirtualAddress);
    while (at != SIZE_MAX && at + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= file.size()) {
        auto const* entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(file.data() + at);
        if (entry->Name == 0) break;
        size_t name = offset(entry->Name);
        if (name == SIZE_MAX || name >= file.size()) break;
        names.push_back(Lower(std::string(file.data() + name, strnlen(file.data() + name, file.size() - name))));
        at += sizeof(IMAGE_IMPORT_DESCRIPTOR);
    }
    return names;
}

bool Imports(std::vector<std::string> const& imports, std::string const& dll)
{
    return std::find(imports.begin(), imports.end(), Lower(dll)) != imports.end();
}

// Export names of a loaded module that contain `part`.
std::vector<std::string> ExportsContaining(HMODULE module, std::string const& part)
{
    std::vector<std::string> found;
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(base + dos->e_lfanew);
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return found;
    auto const* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY const*>(base + directory.VirtualAddress);
    auto const* names = reinterpret_cast<DWORD const*>(base + exports->AddressOfNames);
    for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
        char const* name = reinterpret_cast<char const*>(base + names[i]);
        if (std::strstr(name, part.c_str())) found.emplace_back(name);
    }
    return found;
}

std::filesystem::path ModulePath(HMODULE module)
{
    wchar_t buffer[MAX_PATH * 4];
    DWORD length = GetModuleFileNameW(module, buffer, DWORD(std::size(buffer)));
    return std::filesystem::path(std::wstring(buffer, length));
}

HMODULE ModuleOf(void const* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &module);
    return module;
}
#endif

} // namespace

int main(int argc, char** argv)
{
#ifndef _WIN32
    (void)argc;
    (void)argv;
    std::cout << "skipped: the PE checks are Windows-only\n";
    return 77;
#else
    if (argc != 5) {
        std::cerr << "usage: prism_single_runtime_test <prism.dll> <prism_usd.dll> <PrismRT.exe> <hdPrism.dll>\n";
        return 2;
    }
    const std::filesystem::path core = argv[1], usd = argv[2], cli = argv[3], plugin = argv[4];
    const std::string coreName = core.filename().string(), usdName = usd.filename().string();

    // 1. On the files: the consumers import the shared libraries.
    const auto cliBytes = ReadFile(cli), pluginBytes = ReadFile(plugin);
    const auto coreBytes = ReadFile(core), usdBytes = ReadFile(usd);
    const auto self = ReadFile(ModulePath(nullptr));
    check(!cliBytes.empty() && !pluginBytes.empty() && !coreBytes.empty() && !usdBytes.empty(), "binaries read");
    const auto cliImports = Imports(cliBytes), pluginImports = Imports(pluginBytes), usdImports = Imports(usdBytes);
    check(Imports(cliImports, coreName), cli.filename().string() + " imports " + coreName);
    check(Imports(cliImports, usdName), cli.filename().string() + " imports " + usdName);
    check(Imports(pluginImports, coreName), plugin.filename().string() + " imports " + coreName);
    check(Imports(pluginImports, usdName), plugin.filename().string() + " imports " + usdName);
    check(Imports(usdImports, coreName), usdName + " imports " + coreName);

    // 2. The MDL runtime's start-up code (src/mdl/context.cpp) is only in the
    // core; a static copy of the core in a consumer would carry it too. The
    // same for the material service of the USD module.
    // Spelled backwards here, so this test does not contain them itself.
    const std::string mdlMarker = Reversed(" :edoc rorre( KDS LDM trats ot deliaF");
    check(Contains(coreBytes, mdlMarker), "the MDL runtime is in " + coreName);
    check(!Contains(cliBytes, mdlMarker), "no MDL runtime code in " + cli.filename().string());
    check(!Contains(pluginBytes, mdlMarker), "no MDL runtime code in " + plugin.filename().string());
    check(!Contains(usdBytes, mdlMarker), "no MDL runtime code in " + usdName);
    check(!Contains(self, mdlMarker), "no MDL runtime code in this test");
    const std::string serviceMarker = Reversed("deppiks si erutxet eht ;'s%' rof dnuof selit MIDU oN ]lairetaM ardyH[");
    check(Contains(usdBytes, serviceMarker), "the material service is in " + usdName);
    check(!Contains(cliBytes, serviceMarker), "no material service code in " + cli.filename().string());
    check(!Contains(pluginBytes, serviceMarker), "no material service code in " + plugin.filename().string());
    check(!Contains(coreBytes, serviceMarker), "no material service code in " + coreName);

    // 3. In this process: load hdPrism.dll beside the core this test links.
    HMODULE coreModule = ModuleOf(reinterpret_cast<void const*>(&prism::materials::SharedMdlContext));
    HMODULE usdModule = ModuleOf(reinterpret_cast<void const*>(&prism::materialx::CapturedMaterialRegistry::Shared));
    check(coreModule && Lower(ModulePath(coreModule).filename().string()) == Lower(coreName),
          "SharedMdlContext is called in " + coreName);
    check(usdModule && Lower(ModulePath(usdModule).filename().string()) == Lower(usdName),
          "the captured-material registry is called in " + usdName);
    HMODULE pluginModule = LoadLibraryExW(std::filesystem::absolute(plugin).wstring().c_str(), nullptr,
                                          LOAD_WITH_ALTERED_SEARCH_PATH);
    check(pluginModule != nullptr, "hdPrism loads (error " + std::to_string(GetLastError()) + ")");

    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;
    EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed);
    modules.resize(std::min<size_t>(needed / sizeof(HMODULE), modules.size()));
    int coreCopies = 0, usdCopies = 0, mdlExporters = 0, registryExporters = 0;
    for (HMODULE module : modules) {
        const std::string name = Lower(ModulePath(module).filename().string());
        if (name == Lower(coreName)) ++coreCopies;
        if (name == Lower(usdName)) ++usdCopies;
        if (!ExportsContaining(module, "SharedMdlContext@materials@prism").empty()) {
            ++mdlExporters;
            check(module == coreModule, "SharedMdlContext exported by " + ModulePath(module).string());
        }
        if (!ExportsContaining(module, "Shared@CapturedMaterialRegistry@materialx@prism").empty()) {
            ++registryExporters;
            check(module == usdModule, "the registry exported by " + ModulePath(module).string());
        }
    }
    check(coreCopies == 1, "one " + coreName + " loaded (" + std::to_string(coreCopies) + ")");
    check(usdCopies == 1, "one " + usdName + " loaded (" + std::to_string(usdCopies) + ")");
    check(mdlExporters == 1, "one module exports the MDL runtime (" + std::to_string(mdlExporters) + ")");
    check(registryExporters == 1, "one module exports the registry (" + std::to_string(registryExporters) + ")");

    // The thread's progress binding is one variable of the core: a binding
    // made here is what the core's code sees.
    prism::Progress model;
    {
        prism::progress::Bind bind(&model);
        check(prism::Progress::Current() == &model, "the progress binding is shared with the core");
    }
    check(prism::Progress::Current() == nullptr, "the progress binding is restored");

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "one copy of " << coreName << " and " << usdName << " per process\n";
    return 0;
#endif
}
