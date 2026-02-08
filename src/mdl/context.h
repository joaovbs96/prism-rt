#pragma once

#include "loader.h"
#include <mi/mdl_sdk.h>
#include <string>
#include <vector>

namespace mdl {

// Wrapper for MDL SDK initialization and core interfaces
class Context {
public:
    // Initialize MDL SDK with optional library path
    explicit Context(const char* library_path = nullptr);
    
    // Destructor handles cleanup
    ~Context();
    
    // Disable copying
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    
    // Check if initialization succeeded
    bool is_valid() const { return m_is_valid; }
    
    // Get last error message if initialization failed
    const std::string& get_error() const { return m_error_message; }
    
    // Core interface accessors
    mi::neuraylib::INeuray* get_neuray() const { return m_loader.get_neuray(); }
    mi::neuraylib::IDatabase* get_database() const { return m_database.get(); }
    mi::neuraylib::IMdl_configuration* get_config() const { return m_mdl_config.get(); }
    mi::neuraylib::IMdl_impexp_api* get_impexp() const { return m_mdl_impexp.get(); }
    mi::neuraylib::IMdl_factory* get_factory() const { return m_mdl_factory.get(); }
    
    // Convenience: Configure search paths
    bool add_mdl_path(const std::string& path);
    bool add_resource_path(const std::string& path);
    
    // Convenience: Load plugins
    bool load_plugin(const std::string& name);

    // Get configured paths
    std::vector<std::string> get_mdl_paths() const;
    std::vector<std::string> get_resource_paths() const;

private:
    bool start_sdk();
    bool acquire_interfaces();
    
    // Loader handles library loading and INeuray lifetime
    Loader m_loader;
    
    // Smart handles for component interfaces
    mi::base::Handle<mi::neuraylib::IDatabase> m_database;
    mi::base::Handle<mi::neuraylib::IMdl_configuration> m_mdl_config;
    mi::base::Handle<mi::neuraylib::IMdl_impexp_api> m_mdl_impexp;
    mi::base::Handle<mi::neuraylib::IMdl_factory> m_mdl_factory;
    
    bool m_is_valid;
    std::string m_error_message;
};

} // namespace mdl