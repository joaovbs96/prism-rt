cmake_minimum_required(VERSION 3.20)
project(PrismRT LANGUAGES CXX)

option(PRISM_BUILD_HYDRA "Build the optional Hydra renderer delegate" OFF)
option(PRISM_COPY_ASSETS "Copy the standalone renderer's runtime assets next to its executable" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep multi-config consumers aligned with the installed RelWithDebInfo
# dependency builds while allowing their Release libraries as a fallback.
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "RelWithDebInfo;Release")
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE "RelWithDebInfo;Release")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Debug;RelWithDebInfo;Release")

set(PROGRAMMING_PROJECTS "C:/Users/joaov/Desktop/Programming_Projects")
set(EXTERNAL_DEPS "${PROGRAMMING_PROJECTS}/external_deps")

if(NOT DEFINED CACHE{TBB_DIR})
    set(TBB_DIR "${PROGRAMMING_PROJECTS}/include/tbb/lib/cmake/tbb"
        CACHE PATH "Path to oneTBB's CMake package" FORCE)
endif()
if(NOT DEFINED CACHE{pxr_DIR})
    set(pxr_DIR "${EXTERNAL_DEPS}/OpenUSD/install"
        CACHE PATH "Path to the OpenUSD installation" FORCE)
endif()
if(NOT DEFINED CACHE{MaterialX_DIR})
    set(MaterialX_DIR "${EXTERNAL_DEPS}/MaterialX/install/lib/cmake/MaterialX"
        CACHE PATH "Path to MaterialX's CMake package" FORCE)
endif()
if(NOT DEFINED CACHE{MDL_SDK_DIR})
    set(MDL_SDK_DIR "${PROGRAMMING_PROJECTS}/mdl-sdk"
        CACHE PATH "Path to the NVIDIA MDL SDK" FORCE)
endif()

set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
add_subdirectory(external/tinygltf)

set(TINYOBJLOADER_BUILD_OBJ_STITCH OFF CACHE INTERNAL "" FORCE)
set(TINYOBJLOADER_BUILD_TEST_LOADER OFF CACHE INTERNAL "" FORCE)
add_subdirectory(external/tinyobj)

find_package(OpenImageIO CONFIG REQUIRED)
find_package(embree 4 REQUIRED)
find_package(TBB REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)
find_package(MaterialX REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

# OpenUSD exports Python3::Python for its bindings. On Windows FindPython3
# may expose only the active RelWithDebInfo configuration, while Visual Studio
# generators validate every configuration during generation. Map the missing
# configurations to the same Python installation used to build OpenUSD.
if(TARGET Python3::Python)
    foreach(_config DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
        set_property(TARGET Python3::Python PROPERTY
            IMPORTED_IMPLIB_${_config} "${Python3_LIBRARY_RELEASE}")
        set_property(TARGET Python3::Python PROPERTY
            IMPORTED_LOCATION_${_config} "${Python3_RUNTIME_LIBRARY_RELEASE}")
    endforeach()
endif()

file(GLOB_RECURSE PRISM_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER PRISM_CORE_SOURCES EXCLUDE REGEX "/(main|standalone_cli)\\.cpp$")
list(FILTER PRISM_CORE_SOURCES EXCLUDE REGEX "/hydra/")

add_library(prism_core STATIC ${PRISM_CORE_SOURCES})
add_library(PrismRT::core ALIAS prism_core)
target_include_directories(prism_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj"
    "${MDL_SDK_DIR}/include")
get_target_property(PRISM_TBB_INCLUDE_DIRS TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(prism_core BEFORE PUBLIC ${PRISM_TBB_INCLUDE_DIRS})
target_include_directories(prism_core PUBLIC ${PXR_INCLUDE_DIRS})
target_include_directories(prism_core PUBLIC ${Python3_INCLUDE_DIRS})
target_link_libraries(prism_core PUBLIC
    OpenImageIO::OpenImageIO
    embree
    TBB::tbb
    ${PXR_LIBRARIES}
    MaterialXCore
    MaterialXFormat
    MaterialXGenShader
    MaterialXGenMdl)
if(MSVC)
    target_compile_options(prism_core PUBLIC "/Zc:preprocessor")
endif()

add_executable(PrismRT src/main.cpp src/standalone_cli.cpp)
target_link_libraries(PrismRT PRIVATE PrismRT::core)

if(PRISM_COPY_ASSETS)
    add_custom_target(copy_assets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:PrismRT>/assets"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:PrismRT>/assets/logo"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:PrismRT>/assets/.gitignore"
        COMMENT "Copying runtime assets to executable directory...")
    add_dependencies(PrismRT copy_assets)
endif()

if(PRISM_BUILD_HYDRA)
    add_subdirectory(src/hydra)
    enable_testing()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/compare_images.cpp")
        add_executable(prism_compare_images tests/hydra/compare_images.cpp)
        target_link_libraries(prism_compare_images PRIVATE OpenImageIO::OpenImageIO)
        set_target_properties(prism_compare_images PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/hydra/plugin>")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/render.cpp")
        add_executable(prism_hydra_render tests/hydra/render.cpp)
        target_include_directories(prism_hydra_render PRIVATE ${PXR_INCLUDE_DIRS})
        target_link_libraries(prism_hydra_render PRIVATE
            PrismRT::core usdImagingGL hd ${PXR_LIBRARIES} OpenImageIO::OpenImageIO)
        set_target_properties(prism_hydra_render PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/hydra/plugin>")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda")
            add_test(NAME prism_hydra_render_exercise
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera"
                    "${CMAKE_BINARY_DIR}/hydra/parity_hydra.exr"
                    128 128 16 --exercise)
            set_tests_properties(prism_hydra_render_exercise PROPERTIES ENVIRONMENT
                "PXR_PLUGINPATH_NAME=${CMAKE_BINARY_DIR}/hydra/resources;PRISM_MATERIALX_LIBRARY_PATH=${EXTERNAL_DEPS}/MaterialX/install/libraries;PRISM_MATERIALX_MDL=${EXTERNAL_DEPS}/MaterialX/install/libraries/mdl;MDL_SDK_ROOT=${MDL_SDK_DIR}")
        endif()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/materials_test.cpp")
        add_executable(prism_hydra_materials_test
            tests/hydra/materials_test.cpp
            src/hydra/materials.cpp)
        target_link_libraries(prism_hydra_materials_test PRIVATE PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_hydra_materials_test COMMAND prism_hydra_materials_test)
        set_tests_properties(prism_hydra_materials_test PROPERTIES ENVIRONMENT
            "PRISM_MATERIALX_LIBRARY_PATH=${EXTERNAL_DEPS}/MaterialX/install/libraries;PRISM_MATERIALX_MDL=${EXTERNAL_DEPS}/MaterialX/install/libraries/mdl;MDL_SDK_ROOT=${MDL_SDK_DIR}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/selection_filter_test.cpp")
        add_executable(prism_hydra_selection_filter_test
            tests/hydra/selection_filter_test.cpp
            src/hydra/transport_filter.cpp)
        target_include_directories(prism_hydra_selection_filter_test PRIVATE ${PXR_INCLUDE_DIRS})
        target_link_libraries(prism_hydra_selection_filter_test PRIVATE
            PrismRT::core hd hdx ${PXR_LIBRARIES})
        set_target_properties(prism_hydra_selection_filter_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/hydra/plugin>")
        add_test(NAME prism_hydra_selection_filter_test COMMAND prism_hydra_selection_filter_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/scene_bridge_test.cpp")
        add_executable(prism_hydra_scene_bridge_test
            tests/hydra/scene_bridge_test.cpp
            src/hydra/scene_bridge.cpp
            src/hydra/materials.cpp
            src/hydra/render_buffer.cpp)
        target_include_directories(prism_hydra_scene_bridge_test PRIVATE ${PXR_INCLUDE_DIRS})
        target_link_libraries(prism_hydra_scene_bridge_test PRIVATE
            PrismRT::core hd ${PXR_LIBRARIES})
        set_target_properties(prism_hydra_scene_bridge_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/hydra/plugin>")
        add_test(NAME prism_hydra_scene_bridge_test COMMAND prism_hydra_scene_bridge_test)
        set_tests_properties(prism_hydra_scene_bridge_test PROPERTIES ENVIRONMENT
            "PRISM_MATERIALX_LIBRARY_PATH=${EXTERNAL_DEPS}/MaterialX/install/libraries;PRISM_MATERIALX_MDL=${EXTERNAL_DEPS}/MaterialX/install/libraries/mdl;MDL_SDK_ROOT=${MDL_SDK_DIR}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/material_loading_test.cpp")
        add_executable(prism_hydra_material_loading_test
            tests/hydra/material_loading_test.cpp src/hydra/materials.cpp)
        target_link_libraries(prism_hydra_material_loading_test PRIVATE PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_hydra_material_loading_test COMMAND prism_hydra_material_loading_test)
        set_tests_properties(prism_hydra_material_loading_test PROPERTIES ENVIRONMENT
            "PRISM_MATERIALX_LIBRARY_PATH=${EXTERNAL_DEPS}/MaterialX/install/libraries;PRISM_MATERIALX_MDL=${EXTERNAL_DEPS}/MaterialX/install/libraries/mdl;MDL_SDK_ROOT=${MDL_SDK_DIR}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_sampling.cpp")
        add_executable(prism_environment_sampling_test tests/environment_sampling.cpp)
        target_link_libraries(prism_environment_sampling_test PRIVATE PrismRT::core)
        add_test(NAME prism_environment_sampling_test COMMAND prism_environment_sampling_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_lighting.cpp")
        add_executable(prism_environment_lighting_test tests/environment_lighting.cpp)
        target_link_libraries(prism_environment_lighting_test PRIVATE PrismRT::core)
        add_test(NAME prism_environment_lighting_test COMMAND prism_environment_lighting_test)
    endif()
endif()
