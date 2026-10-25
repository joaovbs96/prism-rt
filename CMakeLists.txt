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

# Versions come from tools/deps.json; checked below once the packages are found.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrismDeps.cmake")

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
# Colour management (src/color); vcpkg also builds it for OpenImageIO.
find_package(OpenColorIO CONFIG REQUIRED)
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
prism_check_dependency_versions()

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
    OpenColorIO::OpenColorIO
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
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrismParity.cmake")
    set(PRISM_HYDRA_RESOURCES_DIR "${CMAKE_BINARY_DIR}/hydra/resources")
    add_subdirectory(src/hydra)
    enable_testing()
    # Keep the original MaterialX compatibility regressions in clean builds.
    add_subdirectory(tests "${CMAKE_BINARY_DIR}/materialx-tests")

    # vcpkg manifests and the docs agree with tools/deps.json.
    add_test(NAME prism_deps_consistency_test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_deps.py")

    # Render tests pin their sampling: a fixed sample count without adaptive
    # sampling (the default since adaptive sampling is on, render_defaults.h),
    # so their images and run times do not follow the defaults.
    set(_prism_fixed_sampling --adaptive-threshold 0)
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
                    128 128 16 --exercise ${_prism_fixed_sampling})
            set_tests_properties(prism_hydra_render_exercise PROPERTIES ENVIRONMENT "${_prism_test_environment}")
            add_test(NAME prism_hydra_transparent_background_export
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera"
                    "${CMAKE_BINARY_DIR}/hydra/parity_hydra_transparent.exr"
                    128 128 16 --hide-dome-background ${_prism_fixed_sampling})
            set_tests_properties(prism_hydra_transparent_background_export PROPERTIES
                ENVIRONMENT "${_prism_test_environment}")
            # A changed integrator setting must reach the render device: a
            # render switched to depth 1 matches one made at depth 1.
            add_test(NAME prism_hydra_depth1_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/parity_depth1.exr"
                    64 64 8 --max-depth 1 ${_prism_fixed_sampling})
            add_test(NAME prism_hydra_depth_change_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/parity_depth_changed.exr"
                    64 64 8 --change-depth 1 ${_prism_fixed_sampling})
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
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/cutout_medium.exr" 64 64 16 ${_prism_fixed_sampling})
            add_test(NAME prism_hydra_cutout_clear_render
                COMMAND prism_hydra_render
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/cutout_clear.usda"
                    "/World/Camera" "${CMAKE_BINARY_DIR}/hydra/cutout_clear.exr" 64 64 16 ${_prism_fixed_sampling})
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
                    64 64 16 --animation-exercise ${_prism_fixed_sampling})
            set_tests_properties(prism_hydra_animation_exercise PROPERTIES
                ENVIRONMENT "${_prism_test_environment}")
        endif()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/render_settings_test.cpp")
        # UsdRender settings, products and vars (docs/hydra.md).
        add_executable(prism_render_settings_test tests/hydra/render_settings_test.cpp)
        target_include_directories(prism_render_settings_test PRIVATE ${PXR_INCLUDE_DIRS})
        target_link_libraries(prism_render_settings_test PRIVATE
            PrismRT::core usdImagingGL hd ${PXR_LIBRARIES} OpenImageIO::OpenImageIO)
        set_target_properties(prism_render_settings_test PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/hydra/plugin>")
        add_test(NAME prism_render_settings_test
            COMMAND prism_render_settings_test --unit "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures"
                "${CMAKE_BINARY_DIR}/render_settings")
        add_test(NAME prism_render_settings_hydra_test
            COMMAND prism_render_settings_test --hydra "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures")
        set_tests_properties(prism_render_settings_test prism_render_settings_hydra_test PROPERTIES
            ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu")
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

    add_executable(prism_texture_streaming_test tests/texture_streaming.cpp)
    target_link_libraries(prism_texture_streaming_test PRIVATE PrismRT::core)
    add_test(NAME prism_texture_streaming_test COMMAND prism_texture_streaming_test)

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
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_features.cpp")
        add_executable(prism_scene_features_test tests/scene_features.cpp)
        target_link_libraries(prism_scene_features_test PRIVATE OpenImageIO::OpenImageIO)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/color_management.cpp")
        add_executable(prism_color_management_test tests/color_management.cpp)
        target_link_libraries(prism_color_management_test PRIVATE PrismRT::core)
        add_test(NAME prism_color_management_test COMMAND prism_color_management_test)
        set_tests_properties(prism_color_management_test PROPERTIES
            ENVIRONMENT "${_prism_test_environment}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/camera_rays.cpp")
        add_executable(prism_camera_rays_test tests/camera_rays.cpp)
        target_link_libraries(prism_camera_rays_test PRIVATE PrismRT::core)
        add_test(NAME prism_camera_rays_test COMMAND prism_camera_rays_test)
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/usd_spheres.cpp")
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
        add_executable(prism_openpbr_transmission_test tests/openpbr_transmission.cpp)
        target_link_libraries(prism_openpbr_transmission_test PRIVATE PrismRT::core)
        add_test(NAME prism_openpbr_transmission_test COMMAND prism_openpbr_transmission_test)
        set_tests_properties(prism_openpbr_transmission_test PROPERTIES
            ENVIRONMENT "${_prism_test_environment}" SKIP_RETURN_CODE 77)
        add_executable(prism_dielectric_albedo_test tests/dielectric_albedo.cpp)
        target_link_libraries(prism_dielectric_albedo_test PRIVATE PrismRT::core)
        add_test(NAME prism_dielectric_albedo_test COMMAND prism_dielectric_albedo_test)
        set_tests_properties(prism_dielectric_albedo_test PROPERTIES
            ENVIRONMENT "${_prism_test_environment}" SKIP_RETURN_CODE 77)
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
                --camera /World/Camera --width 32 --height 32 --spp 2 ${_prism_fixed_sampling} --device ${_device}
                --output "${CMAKE_BINARY_DIR}/cli_clean_exit_${_device}.png")
        set_tests_properties(prism_cli_clean_exit_${_device} PROPERTIES
            ENVIRONMENT "${_prism_test_environment}")
    endforeach()
    if(PRISM_ENABLE_VULKAN)
        add_test(NAME prism_cli_gpu_failure_vulkan
            COMMAND ${CMAKE_COMMAND} -DPRISMRT=$<TARGET_FILE:PrismRT>
                -DSCENE=${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes/CornellBox.usda
                -DOUTPUT=${CMAKE_BINARY_DIR}/cli_gpu_failure.png
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_gpu_failure.cmake)
        set_tests_properties(prism_cli_gpu_failure_vulkan PROPERTIES
            ENVIRONMENT "${_prism_test_environment}")
    endif()
    # --time-limit / prism:timeLimitSeconds: a sample count no render reaches
    # stops at the limit, in both frontends (docs/hydra.md).
    add_test(NAME prism_cli_time_limit
        COMMAND PrismRT --scene "${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes/CornellBox.usda"
            --camera /World/Camera --width 32 --height 32 --spp 100000000 --adaptive-threshold 0
            --time-limit 2 --device cpu --output "${CMAKE_BINARY_DIR}/cli_time_limit.exr")
    set_tests_properties(prism_cli_time_limit PROPERTIES ENVIRONMENT "${_prism_test_environment}"
        PASS_REGULAR_EXPRESSION "stopped by the time limit" TIMEOUT 120)
    # The former cpu-reference device is rejected with a plain message.
    add_test(NAME prism_cli_cpu_reference_removed
        COMMAND PrismRT --scene "${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes/CornellBox.usda"
            --camera /World/Camera --device cpu-reference --output "${CMAKE_BINARY_DIR}/cli_cpu_reference.exr")
    set_tests_properties(prism_cli_cpu_reference_removed PROPERTIES ENVIRONMENT "${_prism_test_environment}"
        PASS_REGULAR_EXPRESSION "the cpu-reference device was removed; use cpu")
    if(TARGET prism_hydra_render)
        add_test(NAME prism_hydra_time_limit
            COMMAND prism_hydra_render "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/parity.usda"
                /World/Camera "${CMAKE_BINARY_DIR}/hydra/time_limit.exr" 32 32 100000000
                --adaptive-threshold 0 --time-limit 2)
        set_tests_properties(prism_hydra_time_limit PROPERTIES
            ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu"
            PASS_REGULAR_EXPRESSION "Render stat timeLimitReached: (1|true)" TIMEOUT 120)
    endif()
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

# GPU texture streaming converges to the image of whole textures: a textured
# plane rendered with streamed mip levels from an empty cache (textures
# prepared during the run) and again from the warm cache, against whole
# textures. Exact requests (threshold 0) and settled residency make the
# streamed renders deterministic; they match the whole-texture image within
# the comparison's 1% (only lookups the feedback did not see read a coarser
# level). On the CPU device all three are the same render.
if(TARGET prism_hydra_render AND TARGET prism_compare_images)
    set(_prism_streaming "${CMAKE_BINARY_DIR}/hydra/streaming")
    add_test(NAME prism_hydra_streaming_fixture
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/make_streaming_fixture.py"
            "${_prism_streaming}")
    add_test(NAME prism_hydra_streaming_clear_cache
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_prism_streaming}/cache")
    set_tests_properties(prism_hydra_streaming_fixture prism_hydra_streaming_clear_cache PROPERTIES
        FIXTURES_SETUP prism_streaming_fixture)
    set(_prism_streaming_env ${_prism_test_environment} "PRISM_CACHE_DIR=${_prism_streaming}/cache"
        "PRISM_GPU_STREAMING_THRESHOLD=0")
    add_test(NAME prism_hydra_streaming_cold
        COMMAND prism_hydra_render "${_prism_streaming}/streaming.usda" /World/Camera
            "${_prism_streaming}/cold.exr" 160 120 16 ${_prism_fixed_sampling})
    add_test(NAME prism_hydra_streaming_warm
        COMMAND prism_hydra_render "${_prism_streaming}/streaming.usda" /World/Camera
            "${_prism_streaming}/warm.exr" 160 120 16 ${_prism_fixed_sampling})
    add_test(NAME prism_hydra_streaming_whole
        COMMAND prism_hydra_render "${_prism_streaming}/streaming.usda" /World/Camera
            "${_prism_streaming}/whole.exr" 160 120 16 --no-texture-streaming ${_prism_fixed_sampling})
    set_tests_properties(prism_hydra_streaming_cold PROPERTIES
        ENVIRONMENT "${_prism_streaming_env}" FIXTURES_REQUIRED prism_streaming_fixture
        FIXTURES_SETUP prism_streaming_renders)
    set_tests_properties(prism_hydra_streaming_warm PROPERTIES
        ENVIRONMENT "${_prism_streaming_env}"
        DEPENDS prism_hydra_streaming_cold FIXTURES_SETUP prism_streaming_renders)
    set_tests_properties(prism_hydra_streaming_whole PROPERTIES
        ENVIRONMENT "${_prism_streaming_env};PRISM_GPU_TEXTURE_BUDGET_MIB=9000"
        DEPENDS prism_hydra_streaming_warm FIXTURES_SETUP prism_streaming_renders)
    add_test(NAME prism_hydra_streaming_cold_matches_whole
        COMMAND prism_compare_images "${_prism_streaming}/whole.exr" "${_prism_streaming}/cold.exr")
    add_test(NAME prism_hydra_streaming_warm_matches_whole
        COMMAND prism_compare_images "${_prism_streaming}/whole.exr" "${_prism_streaming}/warm.exr")
    add_test(NAME prism_hydra_streaming_cold_matches_warm
        COMMAND prism_compare_images "${_prism_streaming}/warm.exr" "${_prism_streaming}/cold.exr" --exact)
    set_tests_properties(prism_hydra_streaming_cold_matches_whole prism_hydra_streaming_warm_matches_whole
        prism_hydra_streaming_cold_matches_warm PROPERTIES FIXTURES_REQUIRED prism_streaming_renders)
endif()

# Colour management (docs/color_management.md), on the CPU device:
# - each tagged colour of the colour fixture reaches the image converted into
#   the working space exactly once, through both loaders and both working
#   spaces, and the loaders agree;
# - an 8-bit output is the linear output through the view, applied once;
# - linear Rec.709 with the gamma 2.2 view reproduces images stored from the
#   renderer before colour management (tests/hydra/fixtures/legacy_rec709,
#   bit-identical on the machine that made them; regenerate them with the
#   options below only for intended image changes).
if(TARGET prism_hydra_render AND TARGET prism_compare_images AND TARGET prism_color_management_test)
    set(_prism_color "${CMAKE_BINARY_DIR}/color")
    file(MAKE_DIRECTORY "${_prism_color}")
    set(_prism_color_fixture "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/color_management.usda")
    set(_prism_color_env ${_prism_test_environment} "PRISM_DEVICE=cpu"
        "PRISM_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
    # Both loaders render the colour fixture (the parity harness, which also
    # compares them); each checks its patches.
    foreach(_space acescg lin_rec709)
        prism_add_parity_fixture(NAME color_${_space} SCENE tests/hydra/fixtures/color_management.usda
            WIDTH 128 HEIGHT 64 SPP 4 ARGS --max-depth 1 --working-space ${_space})
        foreach(_loader cli hydra)
            add_test(NAME prism_color_${_loader}_${_space}_patches
                COMMAND prism_color_management_test --check-patches
                    "${prism_parity_color_${_space}_dir}/${_loader}/image.exr" ${_space})
            set_tests_properties(prism_color_${_loader}_${_space}_patches PROPERTIES
                ENVIRONMENT "${_prism_color_env}" FIXTURES_REQUIRED prism_parity_color_${_space})
        endforeach()
    endforeach()
    add_test(NAME prism_color_view_render
        COMMAND PrismRT --scene "${_prism_color_fixture}" --camera /World/Camera
            --width 128 --height 64 --spp 4 --max-depth 1 ${_prism_fixed_sampling} --device cpu --output "${_prism_color}/cli_view.png")
    set_tests_properties(prism_color_view_render PROPERTIES
        ENVIRONMENT "${_prism_color_env}" FIXTURES_SETUP prism_color_view)
    add_test(NAME prism_color_view_once_test
        COMMAND prism_color_management_test --check-view "${prism_parity_color_acescg_dir}/cli/image.exr"
            "${_prism_color}/cli_view.png" ACEScg "sRGB - Display" "ACES 2.0 - SDR 100 nits (Rec.709)")
    set_tests_properties(prism_color_view_once_test PROPERTIES
        ENVIRONMENT "${_prism_color_env}" FIXTURES_REQUIRED "prism_color_view;prism_parity_color_acescg")

    set(_prism_legacy "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/legacy_rec709")
    set(_prism_legacy_view --working-space lin_rec709 --display "Gamma 2.2 Rec.709 - Display" --view Un-tone-mapped)
    foreach(_output CornellBox.png QuadTest.png QuadTest.exr)
        string(REGEX REPLACE "\\..*$" "" _scene "${_output}")
        string(REPLACE "." "_" _name "${_output}")
        add_test(NAME prism_color_legacy_cli_${_name}_render
            COMMAND PrismRT --scene "${CMAKE_CURRENT_SOURCE_DIR}/assets/scenes/${_scene}.usda"
                --camera /World/Camera --width 96 --height 64 --spp 16 ${_prism_fixed_sampling} --device cpu ${_prism_legacy_view}
                --output "${_prism_color}/legacy_cli_${_output}")
        add_test(NAME prism_color_legacy_cli_${_name}_test
            COMMAND prism_compare_images "${_prism_legacy}/cli_${_output}" "${_prism_color}/legacy_cli_${_output}")
        set_tests_properties(prism_color_legacy_cli_${_name}_render PROPERTIES
            ENVIRONMENT "${_prism_color_env}" FIXTURES_SETUP prism_color_legacy_${_name})
        set_tests_properties(prism_color_legacy_cli_${_name}_test PROPERTIES
            FIXTURES_REQUIRED prism_color_legacy_${_name})
    endforeach()
    foreach(_fixture parity lights)
        add_test(NAME prism_color_legacy_hydra_${_fixture}_render
            COMMAND prism_hydra_render "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/${_fixture}.usda"
                /World/Camera "${_prism_color}/legacy_hydra_${_fixture}.exr" 64 64 16 --working-space lin_rec709 ${_prism_fixed_sampling})
        add_test(NAME prism_color_legacy_hydra_${_fixture}_test
            COMMAND prism_compare_images "${_prism_legacy}/hydra_${_fixture}.exr"
                "${_prism_color}/legacy_hydra_${_fixture}.exr")
        set_tests_properties(prism_color_legacy_hydra_${_fixture}_render PROPERTIES
            ENVIRONMENT "${_prism_color_env}" FIXTURES_SETUP prism_color_legacy_hydra_${_fixture})
        set_tests_properties(prism_color_legacy_hydra_${_fixture}_test PROPERTIES
            FIXTURES_REQUIRED prism_color_legacy_hydra_${_fixture})
    endforeach()
endif()

# CLI-vs-Hydra parity (cmake/PrismParity.cmake, docs/hydra.md): every scene
# feature both loaders support has a fixture here, rendered by both on the CPU
# device and compared. Add one prism_add_parity_fixture call per fixture.
if(TARGET prism_hydra_render AND TARGET prism_compare_images)
    prism_add_parity_fixture(NAME parity SCENE tests/hydra/fixtures/parity.usda)
    # Lights are sampled in another order: noise, not bias (the mean agrees).
    prism_add_parity_fixture(NAME lights SCENE tests/hydra/fixtures/lights.usda
        TOLERANCE 0.1 MEAN_TOLERANCE 0.01)
    prism_add_parity_fixture(NAME cutout_clear SCENE tests/hydra/fixtures/cutout_clear.usda)
    prism_add_parity_fixture(NAME cutout_medium SCENE tests/hydra/fixtures/cutout_medium.usda)
    prism_add_parity_fixture(NAME sss_furnace SCENE tests/hydra/fixtures/sss_furnace.usda)
    prism_add_parity_fixture(NAME preview_mtlx SCENE tests/hydra/fixtures/preview_mtlx_parity.usda)
    prism_add_parity_fixture(NAME cornell_box SCENE assets/scenes/CornellBox.usda)
    prism_add_parity_fixture(NAME quad_test SCENE assets/scenes/QuadTest.usda)
    prism_add_parity_fixture(NAME in_one_weekend SCENE assets/scenes/InOneWeekend.usda)
    # Render settings: material binding purposes and prism:* settings of the
    # chosen settings prim, and RenderProducts with their RenderVars.
    foreach(_settings Preview AllPurpose)
        string(TOLOWER "${_settings}" _name)
        prism_add_parity_fixture(NAME binding_${_name} SCENE tests/hydra/fixtures/render_settings_binding.usda
            WIDTH 32 HEIGHT 32 SPP 4 ARGS --render-settings /Render/${_settings})
    endforeach()
    prism_add_parity_fixture(NAME render_products SCENE tests/hydra/fixtures/render_products.usda
        PRODUCTS render_products_beauty.exr render_products_beauty.png render_products_side.exr
        EXACT_LAYERS render_products_beauty.exr:Normal render_products_side.exr:Neye)
    prism_add_parity_fixture(NAME render_crop SCENE tests/hydra/fixtures/render_settings_crop.usda
        PRODUCTS crop_full.exr crop_window.exr crop_wide.exr ARGS --spp 16
        EXACT_LAYERS crop_window.exr:Normal)
    # UsdLux and geometry features (docs/hydra.md, "Scene features"): both
    # loaders render the fixture, prism_scene_features_test checks each render.
    # <feature>:<fixture>
    set(_prism_scene_features
        camera_visibility:light_camera_visibility
        dome_temperature:dome_color_temperature
        invisible_ids:pointinstancer_invisible_ids
        display_color:display_color_interpolation
        glass_furnace:glass_furnace)
    foreach(_entry IN LISTS _prism_scene_features)
        string(REPLACE ":" ";" _parts "${_entry}")
        list(GET _parts 0 _feature)
        list(GET _parts 1 _scene)
        prism_add_parity_fixture(NAME ${_scene} SCENE tests/hydra/fixtures/${_scene}.usda)
        if(TARGET prism_scene_features_test)
            foreach(_frontend cli hydra)
                add_test(NAME prism_scene_features_${_feature}_${_frontend}
                    COMMAND prism_scene_features_test ${_feature}
                        "${prism_parity_${_scene}_dir}/${_frontend}/image.exr")
                set_tests_properties(prism_scene_features_${_feature}_${_frontend} PROPERTIES
                    FIXTURES_REQUIRED prism_parity_${_scene})
            endforeach()
        endif()
    endforeach()
    # prism:honorDoubleSided (the fixture's render settings turn it on),
    # with a mirrored (negative scale) card.
    prism_add_parity_fixture(NAME double_sided SCENE tests/hydra/fixtures/double_sided.usda
        ARGS --render-settings /Render/Settings)
    if(TARGET prism_scene_features_test)
        foreach(_frontend cli hydra)
            add_test(NAME prism_scene_features_double_sided_${_frontend}
                COMMAND prism_scene_features_test double_sided "${prism_parity_double_sided_dir}/${_frontend}/image.exr")
            set_tests_properties(prism_scene_features_double_sided_${_frontend} PROPERTIES
                FIXTURES_REQUIRED prism_parity_double_sided)
        endforeach()
    endif()
    # prism:sssFallback (and --sss-fallback) from the render settings, on both
    # loaders; the check shows the setting took effect.
    prism_add_parity_fixture(NAME sss_fallback SCENE tests/hydra/fixtures/sss_fallback.usda
        ARGS --render-settings /Render/Settings)
    if(TARGET prism_scene_features_test)
        foreach(_frontend cli hydra)
            add_test(NAME prism_scene_features_sss_fallback_${_frontend}
                COMMAND prism_scene_features_test sss_fallback "${prism_parity_sss_fallback_dir}/${_frontend}/image.exr")
            set_tests_properties(prism_scene_features_sss_fallback_${_frontend} PROPERTIES
                FIXTURES_REQUIRED prism_parity_sss_fallback)
        endforeach()
    endif()
    # prism:subdivisionLevel from the render settings and prism_hydra_render's
    # --subdivision-level; the CLI against the delegate below.
    set(_prism_subdivision "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/subdivision_level.usda")
    foreach(_case "hull;/Render/Hull;12;" "default;/Render/Default;192;" "option;/Render/Hull;48;--subdivision-level;1")
        list(GET _case 0 _name)
        list(GET _case 1 _settings)
        list(GET _case 2 _triangles)
        list(SUBLIST _case 3 -1 _options)
        add_test(NAME prism_hydra_subdivision_level_${_name}
            COMMAND prism_hydra_render "${_prism_subdivision}" /World/Camera
                "${CMAKE_BINARY_DIR}/hydra/subdivision_${_name}.exr" 32 32 4 ${_prism_fixed_sampling}
                --render-settings ${_settings} ${_options})
        set_tests_properties(prism_hydra_subdivision_level_${_name} PROPERTIES
            ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu"
            PASS_REGULAR_EXPRESSION "Render stat triangleCount: ${_triangles}[^0-9]")
    endforeach()
    # The standalone loader's scene features at the delegate's level (phase 7,
    # docs/hydra.md "Scene features"): subdivision at the render settings'
    # level and the option's, ...
    foreach(_case "hull;/Render/Hull;" "default;/Render/Default;" "option;/Render/Hull;--subdivision-level;1")
        list(GET _case 0 _name)
        list(GET _case 1 _settings)
        list(SUBLIST _case 2 -1 _options)
        prism_add_parity_fixture(NAME subdivision_${_name} SCENE tests/hydra/fixtures/subdivision_level.usda
            WIDTH 32 HEIGHT 32 SPP 4 ARGS --render-settings ${_settings} ${_options})
    endforeach()
    # ... load-time displacement and implicit surfaces (refined), a UsdPreviewSurface
    # emitter on refined meshes, mesh holes, leftHanded and mirrored meshes,
    # GeomSubset bindings, uniform and constant normals, purposes (both
    # includedPurposes) and visibility, ...
    prism_add_parity_fixture(NAME displacement SCENE tests/hydra/fixtures/displacement.usda)
    prism_add_parity_fixture(NAME implicit_surfaces SCENE tests/hydra/fixtures/implicit_surfaces.usda)
    prism_add_parity_fixture(NAME emissive SCENE tests/hydra/fixtures/emissive.usda)
    foreach(_settings Default Proxy)
        string(TOLOWER "${_settings}" _name)
        prism_add_parity_fixture(NAME mesh_features_${_name} SCENE tests/hydra/fixtures/mesh_features.usda
            WIDTH 96 HEIGHT 64 ARGS --render-settings /Render/${_settings})
    endforeach()
    # ... PointInstancers (nested, invisibleIds) of implicit cubes, native
    # instances with their own bindings, per-instance primvars for MDL scene
    # data lookups (geompropvalue), ...
    prism_add_parity_fixture(NAME instances SCENE tests/hydra/fixtures/instances.usda)
    prism_add_parity_fixture(NAME instance_primvars SCENE tests/hydra/fixtures/instance_primvars.usda)
    # The prototype panel and its two instances overlap in one plane: which
    # surface wins there depends on each loader's geometry (the CLI keeps
    # uninstanced meshes in world space), so they differ at those pixels only.
    prism_add_parity_fixture(NAME native_instance_bindings SCENE tests/hydra/fixtures/native_instance_bindings.usda
        TOLERANCE 0.03 MEAN_TOLERANCE 0.002)
    if(TARGET prism_scene_features_test)
        foreach(_frontend cli hydra)
            add_test(NAME prism_scene_features_instance_primvars_${_frontend}
                COMMAND prism_scene_features_test instance_primvars
                    "${prism_parity_instance_primvars_dir}/${_frontend}/image.exr")
            set_tests_properties(prism_scene_features_instance_primvars_${_frontend} PROPERTIES
                FIXTURES_REQUIRED prism_parity_instance_primvars)
        endforeach()
    endif()
    # ... light and shadow linking (spatial light selection weighs the lights
    # at each shading point, so the loaders' slightly different geometry
    # picks lights differently: noise, the mean agrees), ...
    prism_add_parity_fixture(NAME linking SCENE tests/hydra/fixtures/linking.usda
        TOLERANCE 0.02 MEAN_TOLERANCE 0.002)
    # ... animated attributes, transforms and skinning at a time (--time).
    # animated: the CLI keeps vertex-interpolated texture coordinates per
    # point, so its tangent frames are smoothed where the delegate's are per
    # triangle; the sampled directions differ (noise, the mean agrees).
    prism_add_parity_fixture(NAME animated_time SCENE tests/hydra/fixtures/animated.usda ARGS --time 2
        TOLERANCE 0.03 MEAN_TOLERANCE 0.002)
    prism_add_parity_fixture(NAME animated_xform_time SCENE tests/hydra/fixtures/animated_xform.usda ARGS --time 2)
    foreach(_time 1 2)
        prism_add_parity_fixture(NAME skinned_${_time} SCENE tests/hydra/fixtures/skinned.usda ARGS --time ${_time})
    endforeach()
    # Camera exposure, and prim, element and instance ids.
    prism_add_parity_fixture(NAME camera_exposure SCENE tests/hydra/fixtures/camera_exposure.usda)
    prism_add_parity_fixture(NAME id_aovs SCENE tests/hydra/fixtures/id_aovs.usda PRODUCTS id_aovs.exr
        EXACT_LAYERS id_aovs.exr:ElementId id_aovs.exr:InstanceId RELABEL_LAYERS id_aovs.exr:PrimId)
    # The CLI on Vulkan against the delegate on the CPU: instanced prototypes,
    # placement transforms and instance primvars in the GPU scene.
    if(PRISM_ENABLE_VULKAN)
        prism_add_parity_fixture(NAME instance_primvars_vulkan SCENE tests/hydra/fixtures/instance_primvars.usda
            CLI_DEVICE vulkan)
    endif()
    # Light inputs:diffuse / inputs:specular: the full image and its
    # diffuse-only and specular-only variants. As for "lights", the loaders
    # order the lights differently: noise, not bias (the mean agrees).
    foreach(_variant light_lobes light_lobes_diffuse light_lobes_specular)
        prism_add_parity_fixture(NAME ${_variant} SCENE tests/hydra/fixtures/${_variant}.usda SPP 64
            TOLERANCE 0.05 MEAN_TOLERANCE 0.01)
    endforeach()
    if(TARGET prism_scene_features_test)
        foreach(_frontend cli hydra)
            add_test(NAME prism_scene_features_light_lobes_${_frontend}
                COMMAND prism_scene_features_test light_lobes
                    "${prism_parity_light_lobes_dir}/${_frontend}/image.exr"
                    "${prism_parity_light_lobes_diffuse_dir}/${_frontend}/image.exr"
                    "${prism_parity_light_lobes_specular_dir}/${_frontend}/image.exr")
            set_tests_properties(prism_scene_features_light_lobes_${_frontend} PROPERTIES
                FIXTURES_REQUIRED "prism_parity_light_lobes;prism_parity_light_lobes_diffuse;prism_parity_light_lobes_specular")
        endforeach()
        # Each lobe is unbiased: next-event estimation (which splits the BSDF
        # evaluation) and BSDF sampling alone (which scales light hits by the
        # sample's diffuse fraction) agree. No sample clamp: it would bias the
        # BSDF-only render.
        foreach(_variant light_lobes_diffuse light_lobes_specular)
            foreach(_sampling mis bsdf)
                add_test(NAME prism_scene_features_${_variant}_${_sampling}_render
                    COMMAND PrismRT --scene "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/${_variant}.usda"
                        --camera /World/Camera --width 32 --height 32 --spp 1024 ${_prism_fixed_sampling} --device cpu
                        --max-sample-radiance 0 --environment-sampling ${_sampling}
                        --output "${CMAKE_BINARY_DIR}/scene_features/${_variant}_${_sampling}.exr")
                set_tests_properties(prism_scene_features_${_variant}_${_sampling}_render PROPERTIES
                    ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu"
                    FIXTURES_SETUP prism_scene_features_${_variant}_sampling)
            endforeach()
            add_test(NAME prism_scene_features_${_variant}_consistency
                COMMAND prism_scene_features_test lobes_consistency
                    "${CMAKE_BINARY_DIR}/scene_features/${_variant}_mis.exr"
                    "${CMAKE_BINARY_DIR}/scene_features/${_variant}_bsdf.exr")
            set_tests_properties(prism_scene_features_${_variant}_consistency PROPERTIES
                FIXTURES_REQUIRED prism_scene_features_${_variant}_sampling)
        endforeach()
        # Delta paths (smooth glass, null crossings) are never MIS weighted:
        # with environment sampling on or off the furnace renders the same.
        foreach(_sampling mis bsdf)
            add_test(NAME prism_scene_features_glass_furnace_${_sampling}_render
                COMMAND PrismRT --scene "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/fixtures/glass_furnace.usda"
                    --camera /World/Camera --width 64 --height 64 --spp 64 ${_prism_fixed_sampling} --device cpu
                    --environment-sampling ${_sampling}
                    --output "${CMAKE_BINARY_DIR}/scene_features/glass_furnace_${_sampling}.exr")
            set_tests_properties(prism_scene_features_glass_furnace_${_sampling}_render PROPERTIES
                ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu"
                FIXTURES_SETUP prism_scene_features_glass_furnace_sampling)
        endforeach()
        add_test(NAME prism_scene_features_glass_furnace_sampling
            COMMAND prism_compare_images "${CMAKE_BINARY_DIR}/scene_features/glass_furnace_bsdf.exr"
                "${CMAKE_BINARY_DIR}/scene_features/glass_furnace_mis.exr" --tolerance 0.005 --mean-tolerance 0.001)
        set_tests_properties(prism_scene_features_glass_furnace_sampling PROPERTIES
            FIXTURES_REQUIRED prism_scene_features_glass_furnace_sampling)
    endif()
    if(TARGET prism_render_settings_test)
        # Named channels and sane values in both frontends' products; the
        # crop matches its region of the full frame (Hydra accumulates it
        # progressively, a pass per sample).
        foreach(_frontend cli hydra)
            add_test(NAME prism_render_products_${_frontend}_channels
                COMMAND prism_render_settings_test --check-products
                    "${prism_parity_render_products_dir}/${_frontend}")
            set_tests_properties(prism_render_products_${_frontend}_channels PROPERTIES
                FIXTURES_REQUIRED prism_parity_render_products)
            add_test(NAME prism_render_crop_${_frontend}_test
                COMMAND prism_render_settings_test --check-crop
                    "${prism_parity_render_crop_dir}/${_frontend}/crop_full.exr"
                    "${prism_parity_render_crop_dir}/${_frontend}/crop_window.exr")
            set_tests_properties(prism_render_crop_${_frontend}_test PROPERTIES
                FIXTURES_REQUIRED prism_parity_render_crop)
        endforeach()
    endif()
    # The validation scripts, headless on the CPU device.
    add_test(NAME prism_hydra_validate_script
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/validate.py"
            --build "${CMAKE_BINARY_DIR}" --config $<CONFIG> --spp 64 --device cpu
            --standalone $<TARGET_FILE:PrismRT> --hydra $<TARGET_FILE:prism_hydra_render>
            --compare $<TARGET_FILE:prism_compare_images>)
    add_test(NAME prism_hydra_validate_exports_script
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/hydra/validate_exports.py"
            --build "${CMAKE_BINARY_DIR}" --config $<CONFIG> --spp 64 --inoneweekend-spp 256 --device cpu
            --standalone $<TARGET_FILE:PrismRT> --hydra $<TARGET_FILE:prism_hydra_render>
            --compare $<TARGET_FILE:prism_compare_images>)
    set_tests_properties(prism_hydra_validate_script prism_hydra_validate_exports_script PROPERTIES
        ENVIRONMENT "${_prism_test_environment};PRISM_DEVICE=cpu" TIMEOUT 1800)
endif()

# glTF materials as MaterialX gltf_pbr (docs/gltf.md), with the synthetic
# fixtures of tests/gltf (tests/gltf/make_fixtures.py):
# - the converter's document for each mapped feature, its validity and MDL
#   generation (prism_gltf_material_test);
# - every fixture renders on the CPU with every material compiled (none falls
#   back to the error material), and normal maps shade like geometry rotated
#   to their normals;
# - Vulkan against the CPU device on the spheres and the textured fixture
#   (textures, texture transform, TEXCOORD_1 and COLOR_0 primvars).
if(PRISM_BUILD_HYDRA)
    set(_prism_gltf_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/gltf")
    set(_prism_gltf "${CMAKE_BINARY_DIR}/gltf")
    file(MAKE_DIRECTORY "${_prism_gltf}")
    set(_prism_gltf_env ${_prism_test_environment} "PRISM_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
    add_executable(prism_gltf_material_test tests/gltf_material.cpp)
    target_link_libraries(prism_gltf_material_test PRIVATE PrismRT::core)
    add_test(NAME prism_gltf_material_test COMMAND prism_gltf_material_test "${_prism_gltf_fixtures}")
    set_tests_properties(prism_gltf_material_test PROPERTIES ENVIRONMENT "${_prism_gltf_env}")

    set(_prism_gltf_spheres --scene "${_prism_gltf_fixtures}/metal_rough_spheres.gltf"
        --camera-position 0,0,12 --camera-target 0,0,0 --vfov 30 --width 96 --height 48
        --env "${_prism_gltf_fixtures}/studio_env.hdr" --spp 512 --max-sample-radiance 0)
    set(_prism_gltf_textured --scene "${_prism_gltf_fixtures}/textured.gltf"
        --camera-position 0,0,5 --camera-target 0,0,0 --vfov 35 --width 96 --height 48
        --env "${_prism_gltf_fixtures}/studio_env.hdr")
    set(_prism_gltf_no_error "magenta error material")
    add_test(NAME prism_gltf_spheres_render
        COMMAND PrismRT ${_prism_gltf_spheres} --device cpu --output "${_prism_gltf}/spheres_cpu.exr")
    set_tests_properties(prism_gltf_spheres_render PROPERTIES
        ENVIRONMENT "${_prism_gltf_env}" FIXTURES_SETUP prism_gltf_spheres_cpu
        FAIL_REGULAR_EXPRESSION "${_prism_gltf_no_error}")
    add_test(NAME prism_gltf_textured_render
        COMMAND PrismRT ${_prism_gltf_textured} --spp 64 --device cpu --output "${_prism_gltf}/textured_cpu.exr")
    set_tests_properties(prism_gltf_textured_render PROPERTIES ENVIRONMENT "${_prism_gltf_env}"
        FIXTURES_SETUP prism_gltf_textured_cpu FAIL_REGULAR_EXPRESSION "${_prism_gltf_no_error}")
    # Without the MaterialX libraries no material compiles: each one warns and
    # renders with the magenta error material.
    add_test(NAME prism_gltf_error_material_render
        COMMAND PrismRT --scene "${_prism_gltf_fixtures}/metal_rough_spheres.gltf"
            --camera-position 0,0,12 --camera-target 0,0,0 --vfov 30 --width 48 --height 24 --spp 4
            ${_prism_fixed_sampling} --device cpu --working-space lin_rec709 --env-color 0,0,0
            --output "${_prism_gltf}/error_material_cpu.exr")
    set_tests_properties(prism_gltf_error_material_render PROPERTIES
        ENVIRONMENT "${_prism_gltf_env};PRISM_MATERIALX_LIBRARY_PATH="
        FIXTURES_SETUP prism_gltf_error_material
        PASS_REGULAR_EXPRESSION "material 0 '[^']*' failed to load \\(no MDL context or MaterialX libraries\\); rendering it with the magenta error material")
    add_test(NAME prism_gltf_error_material_test
        COMMAND prism_gltf_material_test --check-error-material "${_prism_gltf}/error_material_cpu.exr")
    set_tests_properties(prism_gltf_error_material_test PROPERTIES FIXTURES_REQUIRED prism_gltf_error_material)
    foreach(_scene features.gltf embedded.glb)
        string(REGEX REPLACE "\\..*$" "" _name "${_scene}")
        add_test(NAME prism_gltf_${_name}_render
            COMMAND PrismRT --scene "${_prism_gltf_fixtures}/${_scene}" --camera-position 0,0,14
                --camera-target 0,0,0 --vfov 30 --width 64 --height 32 --spp 4 --device cpu
                --env "${_prism_gltf_fixtures}/studio_env.hdr" --output "${_prism_gltf}/${_name}_cpu.exr")
        set_tests_properties(prism_gltf_${_name}_render PROPERTIES ENVIRONMENT "${_prism_gltf_env}"
            FAIL_REGULAR_EXPRESSION "${_prism_gltf_no_error}")
    endforeach()
    # Normal maps: the V flip and tangent frame against rotated geometry.
    add_test(NAME prism_gltf_normal_map_render
        COMMAND PrismRT --scene "${_prism_gltf_fixtures}/normal_map.gltf" --camera-position 0,0,6
            --camera-target 0,0,0 --vfov 40 --width 120 --height 80 --spp 128 --max-sample-radiance 0
            --device cpu --env "${_prism_gltf_fixtures}/studio_env.hdr" --output "${_prism_gltf}/normal_map_cpu.exr")
    set_tests_properties(prism_gltf_normal_map_render PROPERTIES ENVIRONMENT "${_prism_gltf_env}"
        FIXTURES_SETUP prism_gltf_normal_map FAIL_REGULAR_EXPRESSION "${_prism_gltf_no_error}")
    add_test(NAME prism_gltf_normal_map_test
        COMMAND prism_gltf_material_test --check-normal-map "${_prism_gltf}/normal_map_cpu.exr")
    set_tests_properties(prism_gltf_normal_map_test PROPERTIES FIXTURES_REQUIRED prism_gltf_normal_map)
    # KHR_materials_volume: Beer-Lambert absorption through a closed sphere
    # (the layer DocumentLoader adds under gltf_pbr's transmission).
    set(_prism_gltf_volume --scene "${_prism_gltf_fixtures}/volume_sphere.gltf" --camera-position 0,0,6
        --camera-target 0,0,0 --vfov 30 --width 64 --height 64 --spp 64 --max-sample-radiance 0
        --env-color 1,1,1 --working-space lin_rec709)
    set(_prism_gltf_volume_devices cpu)
    if(PRISM_ENABLE_VULKAN)
        list(APPEND _prism_gltf_volume_devices vulkan)
    endif()
    foreach(_device IN LISTS _prism_gltf_volume_devices)
        add_test(NAME prism_gltf_volume_${_device}_render
            COMMAND PrismRT ${_prism_gltf_volume} --device ${_device} --output "${_prism_gltf}/volume_${_device}.exr")
        set_tests_properties(prism_gltf_volume_${_device}_render PROPERTIES ENVIRONMENT "${_prism_gltf_env}"
            FIXTURES_SETUP prism_gltf_volume_${_device}
            FAIL_REGULAR_EXPRESSION "${_prism_gltf_no_error};cannot render the scene on the GPU")
        add_test(NAME prism_gltf_volume_${_device}_test
            COMMAND prism_gltf_material_test --check-volume "${_prism_gltf}/volume_${_device}.exr")
        set_tests_properties(prism_gltf_volume_${_device}_test PROPERTIES FIXTURES_REQUIRED prism_gltf_volume_${_device})
    endforeach()
    if(TARGET prism_compare_images)
        if(PRISM_ENABLE_VULKAN)
            add_test(NAME prism_gltf_spheres_vulkan_render
                COMMAND PrismRT ${_prism_gltf_spheres} --device vulkan
                    --output "${_prism_gltf}/spheres_vulkan.exr")
            add_test(NAME prism_gltf_textured_vulkan_render
                COMMAND PrismRT ${_prism_gltf_textured} --spp 64 --device vulkan
                    --output "${_prism_gltf}/textured_vulkan.exr")
            foreach(_scene spheres textured)
                set_tests_properties(prism_gltf_${_scene}_vulkan_render PROPERTIES ENVIRONMENT "${_prism_gltf_env}"
                    FIXTURES_SETUP prism_gltf_${_scene}_vulkan
                    FAIL_REGULAR_EXPRESSION "${_prism_gltf_no_error};cannot render the scene on the GPU")
            endforeach()
            add_test(NAME prism_gltf_spheres_vulkan_parity
                COMMAND prism_compare_images "${_prism_gltf}/spheres_cpu.exr"
                    "${_prism_gltf}/spheres_vulkan.exr" --tolerance 0.1 --mean-tolerance 0.02)
            set_tests_properties(prism_gltf_spheres_vulkan_parity PROPERTIES
                FIXTURES_REQUIRED "prism_gltf_spheres_cpu;prism_gltf_spheres_vulkan")
            add_test(NAME prism_gltf_textured_vulkan_parity
                COMMAND prism_compare_images "${_prism_gltf}/textured_cpu.exr"
                    "${_prism_gltf}/textured_vulkan.exr" --tolerance 0.1 --mean-tolerance 0.02)
            set_tests_properties(prism_gltf_textured_vulkan_parity PROPERTIES
                FIXTURES_REQUIRED "prism_gltf_textured_cpu;prism_gltf_textured_vulkan")
        endif()
    endif()
endif()

# OBJ/MTL materials as MaterialX open_pbr_surface (docs/obj.md), with the
# synthetic fixtures of tests/obj (tests/obj/make_fixtures.py):
# - the converter's document for each mapped statement, its validity and MDL
#   generation (prism_obj_material_test);
# - every fixture renders on the CPU with every material compiled, and the
#   renders show what the statements ask for: textures, cut-outs, bump and
#   normal maps (textured), Tf-tinted refraction (glass), Ke as radiance
#   (emissive);
# - Vulkan against the CPU device on the textured fixture.
if(PRISM_BUILD_HYDRA)
    set(_prism_obj_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/obj")
    set(_prism_obj "${CMAKE_BINARY_DIR}/obj")
    file(MAKE_DIRECTORY "${_prism_obj}")
    set(_prism_obj_env ${_prism_test_environment} "PRISM_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache")
    add_executable(prism_obj_material_test tests/obj_material.cpp)
    target_link_libraries(prism_obj_material_test PRIVATE PrismRT::core)
    add_test(NAME prism_obj_material_test COMMAND prism_obj_material_test "${_prism_obj_fixtures}")
    set_tests_properties(prism_obj_material_test PROPERTIES ENVIRONMENT "${_prism_obj_env}")

    set(_prism_obj_view --camera-position 0,0,6 --camera-target 0,0,0 --vfov 45 --width 96 --height 96
        ${_prism_fixed_sampling})
    set(_prism_obj_studio --env "${CMAKE_CURRENT_SOURCE_DIR}/tests/gltf/studio_env.hdr")
    # <fixture>:<spp>:<check or ->:<lighting>
    foreach(_case "textured:64:textured:studio" "pbr:16:-:studio" "glass:64:glass:white" "emissive:16:emissive:black"
                  "displaced:16:-:studio")
        string(REPLACE ":" ";" _parts "${_case}")
        list(GET _parts 0 _name)
        list(GET _parts 1 _spp)
        list(GET _parts 2 _check)
        list(GET _parts 3 _light)
        if(_light STREQUAL "studio")
            set(_lighting ${_prism_obj_studio})
        elseif(_light STREQUAL "white")
            set(_lighting --env-color 1,1,1 --working-space lin_rec709)
        else()
            set(_lighting --env-color 0,0,0 --working-space lin_rec709)
        endif()
        set(_prism_obj_${_name}_args --scene "${_prism_obj_fixtures}/${_name}.obj" ${_prism_obj_view}
            --spp ${_spp} ${_lighting})
        add_test(NAME prism_obj_${_name}_render
            COMMAND PrismRT ${_prism_obj_${_name}_args} --device cpu --output "${_prism_obj}/${_name}_cpu.exr")
        set_tests_properties(prism_obj_${_name}_render PROPERTIES ENVIRONMENT "${_prism_obj_env}"
            FIXTURES_SETUP prism_obj_${_name}_cpu
            FAIL_REGULAR_EXPRESSION "magenta error material;is not supported")
        if(_name STREQUAL "displaced")
            # MTL disp: the ridged quad is diced and displaced at load time.
            set_tests_properties(prism_obj_${_name}_render PROPERTIES
                PASS_REGULAR_EXPRESSION "displaced the faces of 1 material\\(s\\) into [0-9]+ triangles")
        endif()
        if(NOT _check STREQUAL "-")
            add_test(NAME prism_obj_${_name}_test
                COMMAND prism_obj_material_test --check-${_check} "${_prism_obj}/${_name}_cpu.exr")
            set_tests_properties(prism_obj_${_name}_test PROPERTIES FIXTURES_REQUIRED prism_obj_${_name}_cpu)
        endif()
    endforeach()
    if(PRISM_ENABLE_VULKAN AND TARGET prism_compare_images)
        add_test(NAME prism_obj_textured_vulkan_render
            COMMAND PrismRT ${_prism_obj_textured_args} --device vulkan --output "${_prism_obj}/textured_vulkan.exr")
        set_tests_properties(prism_obj_textured_vulkan_render PROPERTIES ENVIRONMENT "${_prism_obj_env}"
            FIXTURES_SETUP prism_obj_textured_vulkan
            FAIL_REGULAR_EXPRESSION "magenta error material;cannot render the scene on the GPU")
        add_test(NAME prism_obj_textured_vulkan_parity
            COMMAND prism_compare_images "${_prism_obj}/textured_cpu.exr" "${_prism_obj}/textured_vulkan.exr"
                --tolerance 0.1 --mean-tolerance 0.02)
        set_tests_properties(prism_obj_textured_vulkan_parity PROPERTIES
            FIXTURES_REQUIRED "prism_obj_textured_cpu;prism_obj_textured_vulkan")
    endif()
endif()

# Every test is labelled cpu or gpu. GPU tests are those that may open a render
# device (the Hydra delegate's default "auto" picks Vulkan or Metal when one
# is available), plus the comparisons whose fixtures are GPU renders, since
# CTest runs a required fixture's setup tests even when their label is
# excluded. GPU tests share RESOURCE_LOCK gpu, so `ctest -LE gpu -j N` can run
# beside GPU work and a parallel run never puts two GPU tests on the device.
set(_prism_gpu_test_regex
    "^prism_hydra_(render_exercise|transparent_background_export|depth1_render|depth_change_render|setting_change_test|cutout_(medium|clear)_render|cutout_medium_test|animation_exercise|pass_camera_test|scene_bridge_test|streaming_(cold|warm|whole)|streaming_.*_matches_.*)$"
    "|^prism_(gpu_shader_compile_test|device_selection_test|cli_clean_exit_vulkan|cli_gpu_failure_vulkan)$"
    "|^prism_(gltf|obj)_.*_vulkan_(render|parity|test)$"
    "|^prism_parity_.*_(vulkan|metal)_(cli|hydra|match)$")
string(JOIN "" _prism_gpu_test_regex ${_prism_gpu_test_regex})
get_property(_prism_tests DIRECTORY PROPERTY TESTS)
foreach(_test IN LISTS _prism_tests)
    # Renders have no time limit of their own (prism_hydra_render waits for
    # convergence): every test gets one.
    get_test_property(${_test} TIMEOUT _prism_timeout)
    if(NOT _prism_timeout)
        set_tests_properties(${_test} PROPERTIES TIMEOUT 600)
    endif()
    if(_test MATCHES "${_prism_gpu_test_regex}")
        set_tests_properties(${_test} PROPERTIES LABELS gpu RESOURCE_LOCK gpu)
    else()
        set_tests_properties(${_test} PROPERTIES LABELS cpu)
    endif()
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
