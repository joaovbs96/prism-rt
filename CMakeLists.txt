cmake_minimum_required(VERSION 3.20)
project(PrismRT)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# fixes "IMPORTED_IMPLIB not set for imported target" issues
# needs to be set before anything else
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL RelWithDebInfo Release)
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE RelWithDebInfo Release)
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG RelWithDebInfo Release)

# ── Path configuration ───────────────────────────────────────────────────────
# Pin these BEFORE any find_package() calls so CMake doesn't discover stale
# installs elsewhere on the system (e.g. an old OpenUSD at
# C:/.../Programming_Projects/OpenUSD instead of external_deps/OpenUSD).
#
# These can be overridden from the command line:
#   cmake -DTBB_DIR="..." -Dpxr_DIR="..." ...
set(PROGRAMMING_PROJECTS "C:/Users/joaov/Desktop/Programming_Projects")
set(EXTERNAL_DEPS "${PROGRAMMING_PROJECTS}/external_deps")

# oneTBB — point at TBBConfig.cmake so find_package(TBB) finds oneTBB,
# NOT any legacy TBB headers that might be bundled in the USD install.
if(NOT DEFINED CACHE{TBB_DIR})
    set(TBB_DIR "${PROGRAMMING_PROJECTS}/include/tbb/lib/cmake/tbb"
        CACHE PATH "Path to TBBConfig.cmake (oneTBB)" FORCE)
endif()

# OpenUSD — point at the correct install under external_deps
if(NOT DEFINED CACHE{pxr_DIR})
    set(pxr_DIR "${EXTERNAL_DEPS}/OpenUSD/install"
        CACHE PATH "Path to pxrConfig.cmake" FORCE)
endif()

# MaterialX
if(NOT DEFINED CACHE{MaterialX_DIR})
    set(MaterialX_DIR "${EXTERNAL_DEPS}/MaterialX/install/lib/cmake/MaterialX"
        CACHE PATH "Path to MaterialXConfig.cmake" FORCE)
endif()

# MDL SDK (header-only, loaded via code)
if(NOT DEFINED CACHE{MDL_SDK_DIR})
    set(MDL_SDK_DIR "${PROGRAMMING_PROJECTS}/mdl-sdk"
        CACHE PATH "Path to MDL SDK root" FORCE)
endif()

# ── Third-party subprojects ──────────────────────────────────────────────────

# tiny gltf
set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")

# tiny obj
set(TINYOBJLOADER_BUILD_OBJ_STITCH OFF CACHE INTERNAL "" FORCE)
set(TINYOBJLOADER_BUILD_TEST_LOADER OFF CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

# ── Source files ─────────────────────────────────────────────────────────────
file(GLOB_RECURSE PRISM_SOURCES 
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE PRISM_HEADERS 
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# Print found files for debugging
message(STATUS "Found source files:")
foreach(SOURCE_FILE ${PRISM_SOURCES})
    message(STATUS "  ${SOURCE_FILE}")
endforeach()

# Create executable with all found sources
add_executable(PrismRT ${PRISM_SOURCES} ${PRISM_HEADERS})

# ── Dependencies ─────────────────────────────────────────────────────────────

# OIIO
find_package(OpenImageIO CONFIG REQUIRED)
target_link_libraries(PrismRT OpenImageIO::OpenImageIO)

# Embree
find_package(embree 4 REQUIRED)
target_link_libraries(PrismRT embree)

# oneTBB — TBB_DIR was pinned above so this finds the correct oneTBB
find_package(TBB REQUIRED CONFIG)
target_link_libraries(PrismRT TBB::tbb)

# OpenUSD — pxr_DIR was pinned above so this finds external_deps/OpenUSD
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)

message(STATUS "PXR_LIBRARIES contains: ${PXR_LIBRARIES}")
string(REPLACE ";" "|" PXR_LIBRARIES_DEBUG "${PXR_LIBRARIES}")
message(STATUS "PXR_LIBRARIES debug: ${PXR_LIBRARIES_DEBUG}")

target_link_libraries(PrismRT ${PXR_LIBRARIES})

# IMPORTANT: Add oneTBB includes BEFORE USD includes so that if USD installed
# any legacy TBB headers under its include dir, the compiler finds oneTBB's
# headers first. The BEFORE keyword prepends to the include search path.
get_target_property(TBB_INCLUDE_DIRS TBB::tbb INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(PrismRT BEFORE PUBLIC ${TBB_INCLUDE_DIRS})
target_include_directories(PrismRT PUBLIC ${PXR_INCLUDE_DIR})

# MaterialX
find_package(MaterialX REQUIRED)
target_link_libraries(PrismRT 
    MaterialXCore
    MaterialXFormat
    MaterialXGenShader
    MaterialXGenMdl
)

# Other libraries
target_include_directories(PrismRT PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")
target_include_directories(PrismRT PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

# MDL includes - there's no linking, DLLs are loaded via code (see src/mdl/loader.h)
target_include_directories(PrismRT PUBLIC ${MDL_SDK_DIR}/include)

if(MSVC)
    target_compile_options(PrismRT PUBLIC "/Zc:preprocessor")
endif()

add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:PrismRT>/assets
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:PrismRT>/assets/logo
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:PrismRT>/assets/.gitignore
    COMMENT "Copying runtime assets to executable directory..."
)

add_dependencies(PrismRT copy_assets)