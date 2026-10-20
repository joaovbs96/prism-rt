cmake_minimum_required(VERSION 3.20)

# A user-wide `vcpkg integrate install` injects include/library paths into every
# Visual Studio project unless the generated project opts out. Dependencies for
# this build are selected explicitly through CMAKE_PREFIX_PATH/toolchain inputs;
# allowing MSBuild integration as well can mix a different vcpkg ABI into them.
if(CMAKE_GENERATOR MATCHES "^Visual Studio")
    list(APPEND CMAKE_VS_GLOBALS "VcpkgEnabled=false")
endif()

project(PrismRT LANGUAGES CXX)

option(PRISM_BUILD_HYDRA "Build the optional Hydra renderer delegate" OFF)
option(PRISM_COPY_ASSETS "Copy the standalone renderer's runtime assets next to its executable" ON)
option(PRISM_INSTALL_USDVIEW_PLUGIN "Install the usdview selection extension" ON)
option(PRISM_INSTALL_ASSETS "Install bundled HDRI, material and texture assets" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(APPLE)
    # Installed binaries resolve dependencies through the directories they were
    # linked against, plus the install prefix library directory.
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)
    set(CMAKE_INSTALL_RPATH "@loader_path/../lib")
endif()

# Keep multi-config consumers aligned with the installed RelWithDebInfo
# dependency builds while allowing their Release libraries as a fallback.
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "RelWithDebInfo;Release")
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE "RelWithDebInfo;Release")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Debug;RelWithDebInfo;Release")
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release")
if(MSVC)
    # OpenSubdiv headers use M_PI.
    add_compile_definitions(_USE_MATH_DEFINES)
    # USD template-heavy sources exceed the default object section limit.
    add_compile_options(/bigobj)
endif()
if(NOT MSVC)
    # System packages such as Apple's OpenGL framework only provide a
    # configuration-less location; an empty entry selects it as last resort.
    foreach(_config MINSIZEREL RELEASE DEBUG RELWITHDEBINFO)
        set(CMAKE_MAP_IMPORTED_CONFIG_${_config} "${CMAKE_MAP_IMPORTED_CONFIG_${_config}};")
    endforeach()
endif()

# Optional per-machine hints live in the ignored cmake/PrismLocalDependencies.cmake.
# Normal package discovery through CMAKE_PREFIX_PATH remains the portable default.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrismLocalDependencies.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrismLocalDependencies.cmake")
endif()
set(MDL_SDK_DIR "" CACHE PATH "Optional NVIDIA MDL SDK root")
set(PRISM_MATERIALX_LIBRARY_PATH "" CACHE PATH "MaterialX standard library directory")
set(PRISM_MATERIALX_MDL "" CACHE PATH "MaterialX MDL library directory")

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
find_package(OpenSubdiv CONFIG REQUIRED)
# Shared OpenSubdiv builds export osdCPU, static builds osdCPU_static.
if(TARGET OpenSubdiv::osdCPU)
    set(PRISM_OSD_CPU_TARGET OpenSubdiv::osdCPU)
else()
    set(PRISM_OSD_CPU_TARGET OpenSubdiv::osdCPU_static)
endif()
find_package(MaterialX REQUIRED)
find_package(Python3 COMPONENTS Development Interpreter REQUIRED)

# OpenUSD built against a Python whose interpreter statically links libpython
# (PXR_PY_UNDEFINED_DYNAMIC_LOOKUP) leaves Python symbols unresolved in its
# libraries. Executables must then provide libpython themselves, while the
# hdPrism plugin must not, since usdview's interpreter already provides it.
if(APPLE)
    set(_prism_link_python_default ON)
else()
    set(_prism_link_python_default OFF)
endif()
option(PRISM_LINK_PYTHON_IN_EXECUTABLES
    "Link libpython into executables (OpenUSD built with undefined dynamic Python lookup)"
    ${_prism_link_python_default})
if(PRISM_LINK_PYTHON_IN_EXECUTABLES AND NOT WIN32 AND Python3_LIBRARIES)
    foreach(_python_library IN LISTS Python3_LIBRARIES)
        get_filename_component(_python_library_dir "${_python_library}" DIRECTORY)
        string(APPEND CMAKE_EXE_LINKER_FLAGS
            " \"${_python_library}\" -Wl,-rpath,\"${_python_library_dir}\"")
    endforeach()
endif()

set(PRISM_USD_ROOT "${pxr_DIR}")
set(PRISM_OPENSUBDIV_ROOT "" CACHE PATH "OpenSubdiv runtime installation prefix used by OpenUSD")
if(NOT PRISM_OPENSUBDIV_ROOT AND OpenSubdiv_DIR)
    get_filename_component(_opensubdiv_dir "${OpenSubdiv_DIR}" DIRECTORY)
    get_filename_component(_opensubdiv_dir "${_opensubdiv_dir}" DIRECTORY)
    get_filename_component(PRISM_OPENSUBDIV_ROOT "${_opensubdiv_dir}" DIRECTORY)
endif()
set(PRISM_MATERIALX_ROOT "" CACHE PATH "MaterialX installation prefix for runtime data")
if(NOT PRISM_MATERIALX_ROOT AND MaterialX_DIR)
    get_filename_component(_mtlx_runtime_dir "${MaterialX_DIR}" DIRECTORY)
    get_filename_component(_mtlx_runtime_dir "${_mtlx_runtime_dir}" DIRECTORY)
    get_filename_component(PRISM_MATERIALX_ROOT "${_mtlx_runtime_dir}" DIRECTORY)
endif()
set(PRISM_MATERIALX_LIBRARY_PATH "" CACHE PATH "MaterialX standard library directory")
set(PRISM_MATERIALX_MDL "" CACHE PATH "MaterialX MDL library directory")
if(NOT PRISM_MATERIALX_LIBRARY_PATH AND PRISM_MATERIALX_ROOT AND
   EXISTS "${PRISM_MATERIALX_ROOT}/libraries")
    set(PRISM_MATERIALX_LIBRARY_PATH "${PRISM_MATERIALX_ROOT}/libraries")
endif()
if(NOT PRISM_MATERIALX_MDL AND PRISM_MATERIALX_LIBRARY_PATH AND
   EXISTS "${PRISM_MATERIALX_LIBRARY_PATH}/mdl")
    set(PRISM_MATERIALX_MDL "${PRISM_MATERIALX_LIBRARY_PATH}/mdl")
endif()
set(PRISM_TBB_REDIST "" CACHE PATH "oneTBB runtime directory")
set(PRISM_EMBREE_RUNTIME_DIR "" CACHE PATH "Embree runtime directory")
set(PRISM_EMBREE_BUILD_RUNTIME "" CACHE PATH "Embree build runtime directory")
set(PRISM_VCPKG_BIN "" CACHE PATH "Runtime DLL directory for vcpkg dependencies")
if(NOT PRISM_TBB_REDIST AND DEFINED ENV{PRISM_TBB_REDIST})
    set(PRISM_TBB_REDIST "$ENV{PRISM_TBB_REDIST}")
endif()
if(NOT PRISM_EMBREE_RUNTIME_DIR AND DEFINED ENV{PRISM_EMBREE_RUNTIME_DIR})
    set(PRISM_EMBREE_RUNTIME_DIR "$ENV{PRISM_EMBREE_RUNTIME_DIR}")
elseif(NOT PRISM_EMBREE_RUNTIME_DIR AND DEFINED ENV{PRISM_EMBREE_ROOT})
    set(PRISM_EMBREE_RUNTIME_DIR "$ENV{PRISM_EMBREE_ROOT}")
endif()
if(NOT PRISM_EMBREE_BUILD_RUNTIME AND DEFINED ENV{PRISM_EMBREE_BUILD_RUNTIME})
    set(PRISM_EMBREE_BUILD_RUNTIME "$ENV{PRISM_EMBREE_BUILD_RUNTIME}")
endif()
if(NOT PRISM_VCPKG_BIN AND DEFINED ENV{PRISM_VCPKG_BIN})
    set(PRISM_VCPKG_BIN "$ENV{PRISM_VCPKG_BIN}")
endif()
if(NOT PRISM_VCPKG_BIN AND OpenImageIO_DIR)
    get_filename_component(_oiio_prefix "${OpenImageIO_DIR}" DIRECTORY)
    get_filename_component(_oiio_prefix "${_oiio_prefix}" DIRECTORY)
    if(EXISTS "${_oiio_prefix}/bin")
        set(PRISM_VCPKG_BIN "${_oiio_prefix}/bin")
    endif()
endif()
set(PRISM_PYTHON_ROOT "" CACHE PATH "Python runtime root used by OpenUSD")
if(NOT PRISM_PYTHON_ROOT AND Python3_EXECUTABLE)
    get_filename_component(PRISM_PYTHON_ROOT "${Python3_EXECUTABLE}" DIRECTORY)
endif()
foreach(_runtime_path IN ITEMS PRISM_USD_ROOT PRISM_OPENSUBDIV_ROOT PRISM_MATERIALX_ROOT PRISM_MATERIALX_LIBRARY_PATH PRISM_MATERIALX_MDL MDL_SDK_DIR PRISM_TBB_REDIST PRISM_EMBREE_RUNTIME_DIR PRISM_EMBREE_BUILD_RUNTIME PRISM_VCPKG_BIN PRISM_PYTHON_ROOT)
    file(TO_CMAKE_PATH "${${_runtime_path}}" ${_runtime_path}_JSON)
endforeach()
configure_file(cmake/PrismRuntime.json.in "${CMAKE_BINARY_DIR}/prism-runtime.json" @ONLY)

# OpenUSD exports Python3::Python for its bindings. On Windows FindPython3
# may expose only the active RelWithDebInfo configuration, while Visual Studio
# generators validate every configuration during generation. Map the missing
# configurations to the same Python installation used to build OpenUSD.
if(WIN32 AND TARGET Python3::Python)
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
# Device backends are added below when enabled.
list(FILTER PRISM_CORE_SOURCES EXCLUDE REGEX "/device/(vulkan|metal)/")

# GPU shader sources, embedded into the core library.
file(GLOB_RECURSE PRISM_SHADER_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*")
set(PRISM_EMBEDDED_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/generated/prism_embedded_shaders.cpp")
add_custom_command(
    OUTPUT "${PRISM_EMBEDDED_SHADERS}"
    COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/shaders
            -DOUTPUT=${PRISM_EMBEDDED_SHADERS}
            -DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/src/device/gpu/shader_sources.h -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake
    DEPENDS ${PRISM_SHADER_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedShaders.cmake"
    COMMENT "Embedding GPU shader sources")

add_library(prism_core STATIC ${PRISM_CORE_SOURCES} "${PRISM_EMBEDDED_SHADERS}")
add_library(PrismRT::core ALIAS prism_core)

# GPU devices compile their HLSL kernel with DXC at run time.
find_program(PRISM_DXC_EXECUTABLE dxc HINTS "$ENV{VULKAN_SDK}/bin" DOC "DXC compiler for the GPU devices")
if(PRISM_DXC_EXECUTABLE)
    target_compile_definitions(prism_core PRIVATE PRISM_DXC_EXECUTABLE="${PRISM_DXC_EXECUTABLE}")
endif()

# Metal device: DXIL from DXC, converted by Metal shader converter.
if(APPLE)
    set(_prism_metal_default ON)
else()
    set(_prism_metal_default OFF)
endif()
option(PRISM_ENABLE_METAL "Build the Metal render device (macOS)" ${_prism_metal_default})
if(PRISM_ENABLE_METAL)
    set(PRISM_METAL_SHADER_CONVERTER_DIR "/usr/local" CACHE PATH "Metal shader converter root (include, lib)")
    find_library(PRISM_METAL_IRCONVERTER_LIBRARY metalirconverter
        PATHS "${PRISM_METAL_SHADER_CONVERTER_DIR}/lib" NO_DEFAULT_PATH)
    if(NOT PRISM_METAL_IRCONVERTER_LIBRARY OR NOT PRISM_DXC_EXECUTABLE)
        message(FATAL_ERROR "The Metal device needs Metal shader converter (PRISM_METAL_SHADER_CONVERTER_DIR) "
            "and DXC (PRISM_DXC_EXECUTABLE); or set PRISM_ENABLE_METAL=OFF.")
    endif()
    set(_prism_metal_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/device/metal/metal_device.mm")
    set_source_files_properties(${_prism_metal_sources} PROPERTIES
        LANGUAGE CXX COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
    target_sources(prism_core PRIVATE ${_prism_metal_sources})
    target_include_directories(prism_core PRIVATE "${PRISM_METAL_SHADER_CONVERTER_DIR}/include")
    target_compile_definitions(prism_core PRIVATE PRISM_HAS_METAL=1)
    target_link_libraries(prism_core PUBLIC ${PRISM_METAL_IRCONVERTER_LIBRARY}
        "-framework Metal" "-framework Foundation")
    get_filename_component(_prism_msc_lib_dir "${PRISM_METAL_IRCONVERTER_LIBRARY}" DIRECTORY)
    list(APPEND CMAKE_BUILD_RPATH "${_prism_msc_lib_dir}")
    list(APPEND CMAKE_INSTALL_RPATH "${_prism_msc_lib_dir}")
endif()

# Vulkan device: SPIR-V from DXC, ray queries.
# On macOS no driver exposes ray queries; the device then builds (as a
# compile check) and reports itself unavailable.
find_package(Vulkan QUIET)
if(Vulkan_FOUND AND PRISM_DXC_EXECUTABLE)
    set(_prism_vulkan_default ON)
else()
    set(_prism_vulkan_default OFF)
endif()
option(PRISM_ENABLE_VULKAN "Build the Vulkan render device" ${_prism_vulkan_default})
if(PRISM_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    if(MSVC)
        # FindVulkan only sets a configuration-less IMPORTED_LOCATION, which the
        # MSVC config mapping above does not fall back to.
        foreach(_config MINSIZEREL RELEASE DEBUG RELWITHDEBINFO)
            set_property(TARGET Vulkan::Vulkan PROPERTY IMPORTED_LOCATION_${_config} "${Vulkan_LIBRARY}")
        endforeach()
    endif()
    if(NOT PRISM_DXC_EXECUTABLE)
        message(FATAL_ERROR "The Vulkan device needs DXC (PRISM_DXC_EXECUTABLE); or set PRISM_ENABLE_VULKAN=OFF.")
    endif()
    target_sources(prism_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/device/vulkan/vulkan_device.cpp"
                                      "${CMAKE_CURRENT_SOURCE_DIR}/src/device/vulkan/vulkan_material_calls.cpp")
    target_compile_definitions(prism_core PRIVATE PRISM_HAS_VULKAN=1)
    target_link_libraries(prism_core PUBLIC Vulkan::Vulkan)
endif()
target_include_directories(prism_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf"
    "${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")
if(MDL_SDK_DIR)
    target_include_directories(prism_core PUBLIC "${MDL_SDK_DIR}/include")
endif()
get_target_property(PRISM_TBB_INCLUDE_DIRS TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(prism_core BEFORE PUBLIC ${PRISM_TBB_INCLUDE_DIRS})
target_include_directories(prism_core PUBLIC ${PXR_INCLUDE_DIRS})
target_include_directories(prism_core PUBLIC ${Python3_INCLUDE_DIRS})
target_link_libraries(prism_core PUBLIC
    OpenImageIO::OpenImageIO
    embree
    TBB::tbb
    ${PXR_LIBRARIES}
    ${PRISM_OSD_CPU_TARGET}
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
    set(PRISM_HYDRA_RESOURCES_DIR "${CMAKE_BINARY_DIR}/hydra/resources")
    add_subdirectory(src/hydra)
    enable_testing()
    # Keep the original MaterialX compatibility regressions in clean builds.
    add_subdirectory(tests "${CMAKE_BINARY_DIR}/materialx-tests")

    # Tests that compare renders exactly must not depend on whether the CPU
    # preview covered a slow GPU preparation.
    set(_prism_test_environment "PXR_PLUGINPATH_NAME=${PRISM_HYDRA_RESOURCES_DIR}"
        "PRISM_GPU_PREPARATION_PREVIEW=0")
    if(PRISM_MATERIALX_LIBRARY_PATH)
        list(APPEND _prism_test_environment
            "PRISM_MATERIALX_LIBRARY_PATH=${PRISM_MATERIALX_LIBRARY_PATH}")
    endif()
    if(PRISM_MATERIALX_MDL)
        list(APPEND _prism_test_environment "PRISM_MATERIALX_MDL=${PRISM_MATERIALX_MDL}")
    endif()
    if(MDL_SDK_DIR)
        list(APPEND _prism_test_environment "MDL_SDK_ROOT=${MDL_SDK_DIR}")
    endif()

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
            set_tests_properties(prism_hydra_render_exercise PROPERTIES ENVIRONMENT "${_prism_test_environment}")
            add_test(NAME prism_hydra_transparent_background_export
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera"
                    "${CMAKE_BINARY_DIR}/hydra/parity_hydra_transparent.exr"
                    128 128 16 --hide-dome-background)
            set_tests_properties(prism_hydra_transparent_background_export PROPERTIES
                ENVIRONMENT "${_prism_test_environment}")
            # A changed integrator setting must reach the render device: a
            # render switched to depth 1 matches one made at depth 1.
            add_test(NAME prism_hydra_depth1_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/parity_depth1.exr"
                    64 64 8 --max-depth 1)
            add_test(NAME prism_hydra_depth_change_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/parity_depth_changed.exr"
                    64 64 8 --change-depth 1)
            add_test(NAME prism_hydra_setting_change_test
                COMMAND prism_compare_images
                    "${CMAKE_BINARY_DIR}/hydra/parity_depth1.exr"
                    "${CMAKE_BINARY_DIR}/hydra/parity_depth_changed.exr" --exact)
            set_tests_properties(prism_hydra_depth1_render prism_hydra_depth_change_render PROPERTIES
                ENVIRONMENT "${_prism_test_environment}" FIXTURES_SETUP prism_depth_change)
            set_tests_properties(prism_hydra_setting_change_test PROPERTIES
                ENVIRONMENT "${_prism_test_environment}" FIXTURES_REQUIRED prism_depth_change)
        endif()
        # A path leaving closed glass through a cutout hole leaves its medium:
        # a negligible medium then renders like none at all.
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/cutout_medium.usda")
            add_test(NAME prism_hydra_cutout_medium_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/cutout_medium.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/cutout_medium.exr" 64 64 16)
            add_test(NAME prism_hydra_cutout_clear_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/cutout_clear.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/cutout_clear.exr" 64 64 16)
            add_test(NAME prism_hydra_cutout_medium_test
                COMMAND prism_compare_images
                    "${CMAKE_BINARY_DIR}/hydra/cutout_clear.exr" "${CMAKE_BINARY_DIR}/hydra/cutout_medium.exr")
            set_tests_properties(prism_hydra_cutout_medium_render prism_hydra_cutout_clear_render PROPERTIES
                ENVIRONMENT "${_prism_test_environment}" FIXTURES_SETUP prism_cutout_medium)
            set_tests_properties(prism_hydra_cutout_medium_test PROPERTIES
                ENVIRONMENT "${_prism_test_environment}" FIXTURES_REQUIRED prism_cutout_medium)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/animated.usda")
            add_test(NAME prism_hydra_animation_exercise
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/animated.usda"
                    "/World/Camera"
                    "${CMAKE_BINARY_DIR}/hydra/animated_hydra.exr"
                    64 64 16 --animation-exercise)
            set_tests_properties(prism_hydra_animation_exercise PROPERTIES
                ENVIRONMENT "${_prism_test_environment}")
        endif()
    endif()
    add_executable(prism_hydra_ext_computation_test
        tests/hydra/ext_computation_test.cpp)
    target_include_directories(prism_hydra_ext_computation_test PRIVATE ${PXR_INCLUDE_DIRS})
    target_link_libraries(prism_hydra_ext_computation_test PRIVATE hd hdsi usdImaging ${PXR_LIBRARIES})
    add_test(NAME prism_hydra_ext_computation_test COMMAND prism_hydra_ext_computation_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/skinned.usda")
    set_tests_properties(prism_hydra_ext_computation_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_hydra_pass_camera_test tests/hydra/pass_camera_test.cpp)
    target_include_directories(prism_hydra_pass_camera_test PRIVATE ${PXR_INCLUDE_DIRS})
    target_link_libraries(prism_hydra_pass_camera_test PRIVATE PrismRT::core hd usdImagingGL ${PXR_LIBRARIES})
    add_test(NAME prism_hydra_pass_camera_test COMMAND prism_hydra_pass_camera_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda")
    set_tests_properties(prism_hydra_pass_camera_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_hydra_primary_coverage_test tests/hydra/primary_coverage_test.cpp)
    target_link_libraries(prism_hydra_primary_coverage_test PRIVATE PrismRT::core)
    add_test(NAME prism_hydra_primary_coverage_test COMMAND prism_hydra_primary_coverage_test)
    set_tests_properties(prism_hydra_primary_coverage_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_hydra_pick_bounds_test tests/hydra/pick_bounds_test.cpp)
    target_link_libraries(prism_hydra_pick_bounds_test PRIVATE PrismRT::core)
    add_test(NAME prism_hydra_pick_bounds_test COMMAND prism_hydra_pick_bounds_test)
    set_tests_properties(prism_hydra_pick_bounds_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_hydra_environment_cache_test tests/hydra/environment_cache_test.cpp)
    target_link_libraries(prism_hydra_environment_cache_test PRIVATE PrismRT::core)
    add_test(NAME prism_hydra_environment_cache_test COMMAND prism_hydra_environment_cache_test)
    set_tests_properties(prism_hydra_environment_cache_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_hydra_fallback_material_test
        tests/hydra/fallback_material_test.cpp)
    target_link_libraries(prism_hydra_fallback_material_test PRIVATE PrismRT::core)
    add_test(NAME prism_hydra_fallback_material_test
        COMMAND prism_hydra_fallback_material_test)
    set_tests_properties(prism_hydra_fallback_material_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/materials_test.cpp")
        add_executable(prism_hydra_materials_test
            tests/hydra/materials_test.cpp
            src/hydra/materials.cpp)
        target_link_libraries(prism_hydra_materials_test PRIVATE PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_hydra_materials_test COMMAND prism_hydra_materials_test)
        set_tests_properties(prism_hydra_materials_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")
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
            tests/hydra/b2_double_primvar_integration_test.cpp
            tests/hydra/b3_fallback_opacity_integration_test.cpp
            tests/hydra/c2_geometric_aov_integration_test.cpp
            tests/hydra/snapshot_scaling_test.cpp
            tests/hydra/snapshot_counts_test.cpp
            tests/hydra/task_delegate_settings_test.cpp
            tests/hydra/coverage_integration_test.cpp
            tests/hydra/picking_integration_test.cpp
            tests/hydra/environment_cache_integration_test.cpp
            tests/hydra/visibility_retention_test.cpp
            tests/hydra/dependency_updates_test.cpp
            tests/hydra/structural_updates_test.cpp
            tests/hydra/accumulation_integration_test.cpp
        tests/hydra/progressive_integration_test.cpp
            tests/hydra/progress_status_test.cpp
            tests/hydra/gpu_preparation_preview_test.cpp
            src/hydra/scene_bridge.cpp
            src/hydra/materials.cpp
            src/hydra/render_buffer.cpp
            src/hydra/task_delegate.cpp
            src/hydra/render_pass.cpp
            src/hydra/transport_filter.cpp
            src/hydra/native_tasks.cpp)
        target_sources(prism_hydra_scene_bridge_test PRIVATE
            tests/hydra/multisampled_presentation_test.cpp)
        target_include_directories(prism_hydra_scene_bridge_test PRIVATE ${PXR_INCLUDE_DIRS})
        target_link_libraries(prism_hydra_scene_bridge_test PRIVATE
            PrismRT::core hd hdx ${PXR_LIBRARIES})
        set_target_properties(prism_hydra_scene_bridge_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/hydra/plugin>")
        add_test(NAME prism_hydra_scene_bridge_test COMMAND prism_hydra_scene_bridge_test)
        set_tests_properties(prism_hydra_scene_bridge_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")
    endif()

    add_executable(prism_hydra_render_buffer_test
        tests/hydra/render_buffer_test.cpp src/hydra/render_buffer.cpp)
    target_include_directories(prism_hydra_render_buffer_test PRIVATE ${PXR_INCLUDE_DIRS})
    target_link_libraries(prism_hydra_render_buffer_test PRIVATE PrismRT::core hd ${PXR_LIBRARIES})
    add_test(NAME prism_hydra_render_buffer_test COMMAND prism_hydra_render_buffer_test)
    set_tests_properties(prism_hydra_render_buffer_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_material_generation_cache_test tests/material_generation_cache.cpp)
    target_link_libraries(prism_material_generation_cache_test PRIVATE PrismRT::core)
    add_test(NAME prism_material_generation_cache_test COMMAND prism_material_generation_cache_test)
    set_tests_properties(prism_material_generation_cache_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}" SKIP_RETURN_CODE 77)
    add_executable(prism_captured_material_graph_test tests/captured_material_graph.cpp)
    target_include_directories(prism_captured_material_graph_test PRIVATE ${PXR_INCLUDE_DIRS})
    target_link_libraries(prism_captured_material_graph_test PRIVATE
        PrismRT::core hd ${PXR_LIBRARIES})
    add_test(NAME prism_captured_material_graph_test COMMAND prism_captured_material_graph_test)
    set_tests_properties(prism_captured_material_graph_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")
    add_executable(prism_generated_mdl_cache_test tests/generated_mdl_cache.cpp)
    target_link_libraries(prism_generated_mdl_cache_test PRIVATE PrismRT::core)
    add_test(NAME prism_generated_mdl_cache_test COMMAND prism_generated_mdl_cache_test)
    set_tests_properties(prism_generated_mdl_cache_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")
    add_test(NAME prism_generated_mdl_fresh_process_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_mdl_fresh_process.py"
            $<TARGET_FILE:prism_material_generation_cache_test>)
    set_tests_properties(prism_generated_mdl_fresh_process_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}" SKIP_RETURN_CODE 77)
    add_test(NAME prism_generated_mdl_concurrent_process_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/generated_mdl_concurrent_process.py"
            $<TARGET_FILE:prism_generated_mdl_cache_test>)
    set_tests_properties(prism_generated_mdl_concurrent_process_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")
    add_executable(prism_texture_cache_benchmark tests/texture_cache_benchmark.cpp)
    target_link_libraries(prism_texture_cache_benchmark PRIVATE PrismRT::core)
    add_executable(prism_texture_digest tests/texture_digest.cpp)
    target_link_libraries(prism_texture_digest PRIVATE PrismRT::core)

    add_executable(prism_gpu_shader_compile_test tests/gpu_shader_compile.cpp)
    target_link_libraries(prism_gpu_shader_compile_test PRIVATE PrismRT::core)
    if(PRISM_ENABLE_VULKAN)
        target_compile_definitions(prism_gpu_shader_compile_test PRIVATE PRISM_HAS_VULKAN=1)
    endif()
    add_test(NAME prism_gpu_shader_compile_test COMMAND prism_gpu_shader_compile_test
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda")
    set_tests_properties(prism_gpu_shader_compile_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_gpu_codegen_dump tests/gpu_codegen_dump.cpp)
    target_link_libraries(prism_gpu_codegen_dump PRIVATE PrismRT::core)

    add_executable(prism_device_selection_test tests/device_selection.cpp)
    target_link_libraries(prism_device_selection_test PRIVATE PrismRT::core)
    add_test(NAME prism_device_selection_test COMMAND prism_device_selection_test)

    add_executable(prism_ray_footprint_test tests/ray_footprint.cpp)
    target_link_libraries(prism_ray_footprint_test PRIVATE PrismRT::core)
    add_test(NAME prism_ray_footprint_test COMMAND prism_ray_footprint_test)

    add_executable(prism_mdl_texture_runtime_test tests/mdl_texture_runtime.cpp)
    target_link_libraries(prism_mdl_texture_runtime_test PRIVATE PrismRT::core)
    add_test(NAME prism_mdl_texture_runtime_test COMMAND prism_mdl_texture_runtime_test)
    set_tests_properties(prism_mdl_texture_runtime_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_mdl_texture_target_test tests/mdl_texture_target.cpp)
    target_link_libraries(prism_mdl_texture_target_test PRIVATE PrismRT::core)
    add_test(NAME prism_mdl_texture_target_test COMMAND prism_mdl_texture_target_test)
    set_tests_properties(prism_mdl_texture_target_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")

    add_executable(prism_texture_service_test tests/texture_service.cpp)
    target_link_libraries(prism_texture_service_test PRIVATE PrismRT::core)
    add_test(NAME prism_texture_service_test COMMAND prism_texture_service_test)

    add_executable(prism_texture_writer_test tests/texture_writer.cpp)
    target_link_libraries(prism_texture_writer_test PRIVATE PrismRT::core)
    add_test(NAME prism_texture_writer_test COMMAND prism_texture_writer_test)
    set_tests_properties(prism_texture_writer_test PROPERTIES TIMEOUT 300)

    add_executable(prism_resolved_material_resource_test
        tests/resolved_material_resource.cpp)
    target_include_directories(prism_resolved_material_resource_test PRIVATE ${PXR_INCLUDE_DIRS})
    target_link_libraries(prism_resolved_material_resource_test PRIVATE
        PrismRT::core ${PXR_LIBRARIES})
    add_test(NAME prism_resolved_material_resource_test
        COMMAND prism_resolved_material_resource_test)
    set_tests_properties(prism_resolved_material_resource_test PROPERTIES
        ENVIRONMENT "${_prism_test_environment}")

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_sampling.cpp")
        add_executable(prism_environment_sampling_test tests/environment_sampling.cpp)
        target_link_libraries(prism_environment_sampling_test PRIVATE PrismRT::core)
        add_test(NAME prism_environment_sampling_test COMMAND prism_environment_sampling_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/subdivision.cpp")
        add_executable(prism_subdivision_test tests/subdivision.cpp)
        target_link_libraries(prism_subdivision_test PRIVATE PrismRT::core)
        add_test(NAME prism_subdivision_test COMMAND prism_subdivision_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/lights.cpp")
        add_executable(prism_lights_test tests/lights.cpp)
        target_link_libraries(prism_lights_test PRIVATE PrismRT::core)
        add_test(NAME prism_lights_test COMMAND prism_lights_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/camera_rays.cpp")
        add_executable(prism_camera_rays_test tests/camera_rays.cpp)
        target_link_libraries(prism_camera_rays_test PRIVATE PrismRT::core)
        add_test(NAME prism_camera_rays_test COMMAND prism_camera_rays_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/render_cost_benchmark.cpp")
        add_executable(prism_render_cost_benchmark
            tests/render_cost_benchmark.cpp)
        target_link_libraries(prism_render_cost_benchmark PRIVATE
            PrismRT::core ${PXR_LIBRARIES})
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/environment_lighting.cpp")
        add_executable(prism_environment_lighting_test tests/environment_lighting.cpp)
        target_link_libraries(prism_environment_lighting_test PRIVATE PrismRT::core)
        add_test(NAME prism_environment_lighting_test COMMAND prism_environment_lighting_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_export_test.cpp")
        add_executable(prism_usd_spheres_test tests/usd_spheres.cpp)
        target_link_libraries(prism_usd_spheres_test PRIVATE PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_usd_spheres_test COMMAND prism_usd_spheres_test)
        add_executable(prism_standalone_material_reuse_test tests/standalone_material_reuse.cpp)
        target_link_libraries(prism_standalone_material_reuse_test PRIVATE
            PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_standalone_material_reuse_test
            COMMAND prism_standalone_material_reuse_test)
        set_tests_properties(prism_standalone_material_reuse_test PROPERTIES
            ENVIRONMENT "${_prism_test_environment}" SKIP_RETURN_CODE 77)
        add_executable(prism_scene_export_test tests/scene_export_test.cpp)
        target_link_libraries(prism_scene_export_test PRIVATE PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_scene_export_test COMMAND prism_scene_export_test)
    endif()
    # The CLI must exit cleanly: process-wide caches holding MDL handles once outlived
    # libmdl_sdk and crashed in static destruction after the image was written.
    set(_prism_cli_exit_devices cpu)
    if(PRISM_ENABLE_VULKAN)
        list(APPEND _prism_cli_exit_devices vulkan)
    endif()
    foreach(_device IN LISTS _prism_cli_exit_devices)
        add_test(NAME prism_cli_clean_exit_${_device}
            COMMAND PrismRT --scene "${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes/CornellBox.usda"
                --camera /World/Camera --width 32 --height 32 --spp 2 --device ${_device}
                --output "${CMAKE_BINARY_DIR}/cli_clean_exit_${_device}.png")
        set_tests_properties(prism_cli_clean_exit_${_device} PROPERTIES
            ENVIRONMENT "${_prism_test_environment}")
    endforeach()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/material_loading_test.cpp")
        add_executable(prism_hydra_material_loading_test
            tests/hydra/material_loading_test.cpp src/hydra/materials.cpp)
        target_link_libraries(prism_hydra_material_loading_test PRIVATE PrismRT::core ${PXR_LIBRARIES})
        add_test(NAME prism_hydra_material_loading_test COMMAND prism_hydra_material_loading_test)
        set_tests_properties(prism_hydra_material_loading_test PROPERTIES ENVIRONMENT "${_prism_test_environment}")
    endif()
endif()

# Test resources stay inside the build tree, independent of user cache state.
get_property(_prism_tests DIRECTORY PROPERTY TESTS)
foreach(_test IN LISTS _prism_tests)
    set_property(TEST ${_test} APPEND PROPERTY ENVIRONMENT
        "PRISM_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
endforeach()

include(GNUInstallDirs)
install(TARGETS PrismRT RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES prism_env.ps1 run_usdview.ps1 prism_env.sh
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(PROGRAMS run_usdview.sh DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_BINARY_DIR}/prism-runtime.json"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(PRISM_INSTALL_ASSETS)
    install(DIRECTORY assets/environment assets/materials assets/textures
        DESTINATION "${CMAKE_INSTALL_BINDIR}/assets")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes")
    install(DIRECTORY assets/scenes/
        DESTINATION "${CMAKE_INSTALL_BINDIR}/assets/scenes")
endif()
if(PRISM_BUILD_HYDRA)
    install(TARGETS hdPrism
        RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/usd/plugins/prism/plugin"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/usd/plugins/prism/plugin")
    install(FILES "${CMAKE_BINARY_DIR}/hydra/resources/plugInfo.json"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/usd/plugins/prism/resources")
    if(PRISM_INSTALL_USDVIEW_PLUGIN)
        install(DIRECTORY tools/usdview_plugins/prismUsdview/
            DESTINATION "${CMAKE_INSTALL_LIBDIR}/usd/plugins/prism/usdview_plugins/prismUsdview"
            PATTERN "__pycache__" EXCLUDE
            PATTERN "*.pyc" EXCLUDE)
    endif()
endif()
