set(PRISM_HYDRA_SOURCES
    plugin.cpp
    render_buffer.cpp
    render_pass.cpp
    scene_bridge.cpp
    task_delegate.cpp
    transport_filter.cpp
    native_tasks.cpp
    materials.cpp)

add_library(hdPrism SHARED ${PRISM_HYDRA_SOURCES})
target_link_libraries(hdPrism PRIVATE PrismRT::core hd hdx ${PXR_LIBRARIES})
target_include_directories(hdPrism PRIVATE ${PXR_INCLUDE_DIRS} ${Python3_INCLUDE_DIRS})
if(MSVC)
    target_compile_options(hdPrism PRIVATE "/Zc:preprocessor")
endif()

set(PRISM_HYDRA_PLUGIN_DIR "${CMAKE_BINARY_DIR}/hydra/plugin")
set(PRISM_HYDRA_RESOURCES_DIR "${CMAKE_BINARY_DIR}/hydra/resources")
set_target_properties(hdPrism PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PRISM_HYDRA_PLUGIN_DIR}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PRISM_HYDRA_PLUGIN_DIR}>"
    ARCHIVE_OUTPUT_DIRECTORY "$<1:${PRISM_HYDRA_PLUGIN_DIR}>")
file(MAKE_DIRECTORY "${PRISM_HYDRA_RESOURCES_DIR}")
configure_file(plugInfo.json "${PRISM_HYDRA_RESOURCES_DIR}/plugInfo.json" COPYONLY)
