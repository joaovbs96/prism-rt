#include "context.h"

#include <format>

#include <mi/base/plugin.h>

namespace mdl {

Context::Context(const char* library_path)
    : m_is_valid(false)
{
    // Load MDL SDK library using our Loader
    mi::neuraylib::INeuray* neuray = m_loader.load(library_path);
    
    if (!neuray) {
        m_error_message = "Failed to load MDL SDK library";
        if (library_path) {
            m_error_message += std::format(" from: {}", library_path);
        }
        return;
    }
    
    // Start the SDK
    if (!start_sdk()) {
        m_loader.unload();
        return;
    }
    
    // Acquire all core interfaces
    if (!acquire_interfaces()) {
        neuray->shutdown();
        m_loader.unload();
        return;
    }
    
    m_is_valid = true;
}

Context::~Context() {
    if (m_is_valid) {
        // Release interface handles (automatic via mi::base::Handle)
        m_mdl_factory.reset();
        m_mdl_impexp.reset();
        m_mdl_config.reset();
        m_database.reset();
        
        // Shutdown SDK
        mi::neuraylib::INeuray* neuray = m_loader.get_neuray();
        if (neuray) {
            neuray->shutdown();
        }
        
        // Loader destructor will handle unloading and INeuray release
    }
}

bool Context::start_sdk() {
    mi::neuraylib::INeuray* neuray = m_loader.get_neuray();
    if (!neuray) {
        m_error_message = "INeuray not loaded";
        return false;
    }
    
    mi::Sint32 result = neuray->start();
    if (result != 0) {
        m_error_message = std::format("Failed to start MDL SDK (error code: {})", result);
        return false;
    }
    
    return true;
}

bool Context::acquire_interfaces() {
    mi::neuraylib::INeuray* neuray = m_loader.get_neuray();
    if (!neuray) {
        m_error_message = "INeuray not available";
        return false;
    }
    
    // Get IDatabase
    m_database = neuray->get_api_component<mi::neuraylib::IDatabase>();
    if (!m_database) {
        m_error_message = "Failed to get IDatabase interface";
        return false;
    }
    
    // Get IMdl_configuration
    m_mdl_config = neuray->get_api_component<mi::neuraylib::IMdl_configuration>();
    if (!m_mdl_config) {
        m_error_message = "Failed to get IMdl_configuration interface";
        return false;
    }
    
    // Get IMdl_impexp_api
    m_mdl_impexp = neuray->get_api_component<mi::neuraylib::IMdl_impexp_api>();
    if (!m_mdl_impexp) {
        m_error_message = "Failed to get IMdl_impexp_api interface";
        return false;
    }
    
    // Get IMdl_factory
    m_mdl_factory = neuray->get_api_component<mi::neuraylib::IMdl_factory>();
    if (!m_mdl_factory) {
        m_error_message = "Failed to get IMdl_factory interface";
        return false;
    }
    
    return true;
}

bool Context::add_mdl_path(const std::string& path) {
    if (!m_is_valid) return false;
    
    mi::Sint32 result = m_mdl_config->add_mdl_path(path.c_str());
    return result == 0;
}

bool Context::add_resource_path(const std::string& path) {
    if (!m_is_valid) return false;
    
    mi::Sint32 result = m_mdl_config->add_resource_path(path.c_str());
    return result == 0;
}

bool Context::load_plugin(const std::string& name) {
    if (!m_is_valid) return false;
    
    std::string plugin_name = name + MI_BASE_DLL_FILE_EXT;
    mi::Sint32 result = m_loader.load_plugin(plugin_name);
    
    // 0 = success, 1 = already loaded (both are OK)
    return result == 0 || result == 1;
}

std::vector<std::string> Context::get_mdl_paths() const {
    std::vector<std::string> paths;
    
    if (!m_is_valid) return paths;
    
    mi::Size count = m_mdl_config->get_mdl_paths_length();
    for (mi::Size i = 0; i < count; ++i) {
        mi::base::Handle<const mi::IString> path(m_mdl_config->get_mdl_path(i));
        if (path) {
            paths.push_back(path->get_c_str());
        }
    }
    
    return paths;
}

std::vector<std::string> Context::get_resource_paths() const {
    std::vector<std::string> paths;
    
    if (!m_is_valid) return paths;
    
    mi::Size count = m_mdl_config->get_resource_paths_length();
    for (mi::Size i = 0; i < count; ++i) {
        mi::base::Handle<const mi::IString> path(m_mdl_config->get_resource_path(i));
        if (path) {
            paths.push_back(path->get_c_str());
        }
    }
    
    return paths;
}

} // namespace mdl