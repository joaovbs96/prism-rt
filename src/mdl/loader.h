#pragma once

#include <mi/mdl_sdk.h>
#include <string>
#include <memory>
#include <iostream>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #define DLL_HANDLE HMODULE
    #define LOAD_LIBRARY(path) LoadLibraryA(path)
    #define GET_FUNCTION(handle, name) GetProcAddress(handle, name)
    #define FREE_LIBRARY(handle) FreeLibrary(handle)
    #define GET_ERROR() GetLastError()
    #define DEFAULT_LIB_NAME "libmdl_sdk.dll"
#else
    #include <dlfcn.h>
    #define DLL_HANDLE void*
    #define LOAD_LIBRARY(path) dlopen(path, RTLD_LAZY)
    #define GET_FUNCTION(handle, name) dlsym(handle, name)
    #define FREE_LIBRARY(handle) dlclose(handle)
    #define GET_ERROR() dlerror()
    #define DEFAULT_LIB_NAME "libmdl_sdk.so"
#endif

namespace mdl {

// RAII wrapper for MDL SDK dynamic library loading
class Loader {
public:
    explicit Loader(bool auto_load = false);
    ~Loader();
    
    // Disable copying
    Loader(const Loader&) = delete;
    Loader& operator=(const Loader&) = delete;
    
    // Enable moving
    Loader(Loader&& other) noexcept;
    Loader& operator=(Loader&& other) noexcept;
    
    // Load the MDL SDK library and create INeuray interface
    mi::neuraylib::INeuray* load(const char* library_path = nullptr);
    mi::neuraylib::INeuray* load(const std::string& library_path);
    
    // Get the loaded INeuray interface
    mi::neuraylib::INeuray* get_neuray() const { return m_neuray; }
    
    // Check if the library is loaded
    bool is_loaded() const { return m_library_handle != nullptr && m_neuray != nullptr; }
    
    // Unload the library and release resources
    void unload();
    
    // Load an MDL SDK plugin
    mi::Sint32 load_plugin(const char* plugin_name);
    mi::Sint32 load_plugin(const std::string& plugin_name);

private:
    bool try_load_library(const std::string& path);
    void* get_factory_function();
    mi::neuraylib::INeuray* create_neuray(void* factory_symbol);
    void print_error(const char* operation);

    DLL_HANDLE m_library_handle = nullptr;
    mi::neuraylib::INeuray* m_neuray = nullptr;
};

// Helper function to load MDL SDK with default settings
// Note: Caller is responsible for calling neuray->shutdown() and neuray->release()
mi::neuraylib::INeuray* load_mdl_sdk(const char* library_path = nullptr);

} // namespace mdl