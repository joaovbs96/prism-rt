set(PRISM_HYDRA_SOURCES
    plugin.cpp
    render_buffer.cpp
    render_pass.cpp
    scene_bridge.cpp
    task_delegate.cpp
    transport_filter.cpp
    native_tasks.cpp
    mtlx_color_space_adapter.cpp)

add_library(hdPrism SHARED ${PRISM_HYDRA_SOURCES})
target_link_libraries(hdPrism PRIVATE prism prism_usd hd hdx ${PXR_LIBRARIES})
target_include_directories(hdPrism PRIVATE ${PXR_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS})
if(MSVC)
    target_compile_options(hdPrism PRIVATE "/Zc:preprocessor")
endif()

set(PRISM_HYDRA_PLUGIN_DIR "${CMAKE_BINARY_DIR}/hydra/plugin")
set(PRISM_HYDRA_RESOURCES_DIR "${CMAKE_BINARY_DIR}/hydra/resources")
set_target_properties(hdPrism PROPERTIES
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PRISM_HYDRA_PLUGIN_DIR}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PRISM_HYDRA_PLUGIN_DIR}>"
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${PRISM_HYDRA_PLUGIN_DIR}>")
file(MAKE_DIRECTORY "${PRISM_HYDRA_RESOURCES_DIR}")
set(PRISM_HYDRA_LIBRARY_FILE "hdPrism${CMAKE_SHARED_LIBRARY_SUFFIX}")
configure_file(plugInfo.json.in "${PRISM_HYDRA_RESOURCES_DIR}/plugInfo.json" @ONLY)
