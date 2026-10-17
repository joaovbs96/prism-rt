#include "loader.h"

#include <cstdlib>
#include <format>
#include <vector>

namespace mdl {

namespace {

// Directories below MDL_SDK_ROOT that may contain the SDK library and plugins.
std::vector<std::string> SdkRootCandidates(const std::string& name) {
    std::vector<std::string> paths;
    const char* root = std::getenv("MDL_SDK_ROOT");
    if (root && root[0] != '\0') {
        paths.push_back(std::string(root) + "/lib/" + name);
        paths.push_back(std::string(root) + "/bin/" + name);
    }
    return paths;
}

} // namespace

Loader::Loader(bool auto_load) {
    if (auto_load) {
        load();
    }
}

Loader::~Loader() {
    unload();
}

// Move constructor
Loader::Loader(Loader&& other) noexcept
    : m_library_handle(other.m_library_handle)
    , m_neuray(other.m_neuray)
{
    other.m_library_handle = nullptr;
    other.m_neuray = nullptr;
}

// Move assignment
Loader& Loader::operator=(Loader&& other) noexcept {
    if (this != &other) {
        unload();
        m_library_handle = other.m_library_handle;
        m_neuray = other.m_neuray;
        other.m_library_handle = nullptr;
        other.m_neuray = nullptr;
    }
    return *this;
}

// Load with C-string path
mi::neuraylib::INeuray* Loader::load(const char* library_path) {
    // Already loaded?
    if (is_loaded()) {
        std::cerr << "Warning: MDL SDK already loaded. Call unload() first to reload.\n";
        return m_neuray;
    }
    
    // Determine library name
    std::string lib_name = (library_path && library_path[0] != '\0') 
        ? library_path 
        : DEFAULT_LIB_NAME;
    
    // Try loading from different locations
    std::vector<std::string> search_paths = {
        lib_name,                      // Try current directory / PATH first
        "../../../bin/" + lib_name,    // Install layout (examples)
        "../../bin/" + lib_name,       // Install layout (vcpkg tools)
    };
    for (const auto& path : SdkRootCandidates(lib_name)) {
        search_paths.push_back(path);  // MDL_SDK_ROOT install layout
    }

    bool loaded = false;
    for (const auto& path : search_paths) {
        if (try_load_library(path)) {
            loaded = true;
            break;
        }
    }
    
    if (!loaded) {
        print_error(std::format("Failed to load MDL SDK library: {}", lib_name).c_str());
        return nullptr;
    }
    
    // Get the factory function
    void* factory_symbol = get_factory_function();
    if (!factory_symbol) {
        print_error("Failed to get mi_factory function");
        unload();
        return nullptr;
    }
    
    // Create INeuray interface
    m_neuray = create_neuray(factory_symbol);
    if (!m_neuray) {
        std::cerr << "Error: Failed to create INeuray interface\n";
        unload();
        return nullptr;
    }
    
    return m_neuray;
}

// Load with std::string path
mi::neuraylib::INeuray* Loader::load(const std::string& library_path) {
    return load(library_path.c_str());
}

// Unload the library
void Loader::unload() {
    // Release INeuray interface first
    if (m_neuray) {
        m_neuray->release();
        m_neuray = nullptr;
    }
    
    // Then unload the library
    if (m_library_handle) {
#ifdef _WIN32
        if (!FreeLibrary(m_library_handle)) {
            print_error("Failed to unload MDL SDK library");
        }
#else
        if (dlclose(m_library_handle) != 0) {
            std::cerr << "Error unloading library: " << dlerror() << "\n";
        }
#endif
        m_library_handle = nullptr;
    }
}

// Load plugin with C-string
mi::Sint32 Loader::load_plugin(const char* plugin_name) {
    if (!is_loaded()) {
        std::cerr << "Error: MDL SDK not loaded. Call load() first.\n";
        return -1;
    }
    
    // Get plugin configuration interface
    mi::base::Handle<mi::neuraylib::IPlugin_configuration> plugin_config(
        m_neuray->get_api_component<mi::neuraylib::IPlugin_configuration>());
    
    if (!plugin_config) {
        std::cerr << "Error: Failed to get plugin configuration interface\n";
        return -1;
    }
    
    // Temporarily reduce log level to avoid confusing warnings
    mi::base::Handle<mi::neuraylib::ILogging_configuration> log_config(
        m_neuray->get_api_component<mi::neuraylib::ILogging_configuration>());
    
    mi::base::Message_severity old_level = mi::base::MESSAGE_SEVERITY_INFO;
    if (log_config) {
        old_level = log_config->get_log_level();
        log_config->set_log_level(mi::base::MESSAGE_SEVERITY_ERROR);
    }
    
    // Try loading plugin from different locations
    std::vector<std::string> search_paths = {
        plugin_name,
#ifdef _WIN32
        "../../../bin/" + std::string(plugin_name),
        "../../bin/" + std::string(plugin_name),
#endif
    };
    for (const auto& path : SdkRootCandidates(plugin_name)) {
        search_paths.push_back(path);  // MDL_SDK_ROOT install layout
    }

    mi::Sint32 result = -1;
    for (const auto& path : search_paths) {
        result = plugin_config->load_plugin_library(path.c_str());
        if (result == 0) {
            break;
        }
    }
    
    // Restore log level
    if (log_config) {
        log_config->set_log_level(old_level);
    }
    
    if (result != 0) {
        std::cerr << std::format("Failed to load plugin: {}\n", plugin_name);
    }
    
    return result;
}

// Load plugin with std::string
mi::Sint32 Loader::load_plugin(const std::string& plugin_name) {
    return load_plugin(plugin_name.c_str());
}

// Private: Try to load library from specific path
bool Loader::try_load_library(const std::string& path) {
    m_library_handle = LOAD_LIBRARY(path.c_str());
    return m_library_handle != nullptr;
}

// Private: Get factory function
void* Loader::get_factory_function() {
    if (!m_library_handle) {
        return nullptr;
    }
    
    return GET_FUNCTION(m_library_handle, "mi_factory");
}

// Private: Create INeuray from factory
mi::neuraylib::INeuray* Loader::create_neuray(void* factory_symbol) {
    if (!factory_symbol) {
        return nullptr;
    }
    
    // Call the factory function to create INeuray
    mi::neuraylib::INeuray* neuray = 
        mi::neuraylib::mi_factory<mi::neuraylib::INeuray>(factory_symbol);
    
    if (!neuray) {
        // Try to get version info for better error message
        mi::base::Handle<mi::neuraylib::IVersion> version(
            mi::neuraylib::mi_factory<mi::neuraylib::IVersion>(factory_symbol));
        
        if (version) {
            std::cerr << std::format(
                "Error: Library version {} does not match header version {}\n",
                version->get_product_version(),
                MI_NEURAYLIB_PRODUCT_VERSION_STRING);
        } else {
            std::cerr << "Error: Incompatible MDL SDK library\n";
        }
        return nullptr;
    }
    
    return neuray;
}

// Private: Print platform-specific error
void Loader::print_error(const char* operation) {
#ifdef _WIN32
    DWORD error_code = GetLastError();
    LPSTR buffer = nullptr;
    
    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer,
        0,
        nullptr);
    
    std::cerr << std::format("{} (Error {}): {}\n", 
        operation, 
        error_code, 
        buffer ? buffer : "Unknown error");
    
    if (buffer) {
        LocalFree(buffer);
    }
#else
    const char* error_msg = dlerror();
    std::cerr << std::format("{}: {}\n", 
        operation, 
        error_msg ? error_msg : "Unknown error");
#endif
}

// Helper function for simple usage
mi::neuraylib::INeuray* load_mdl_sdk(const char* library_path) {
    static Loader loader;  // Static to keep library loaded
    return loader.load(library_path);
}

} // namespace Loader