cmake_minimum_required(VERSION 3.20)
project(PrismRT)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# tiny gltf
set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
set(TINYGLTF_BUILD_LOADER_EXAMPLE OFF CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")

# tiny obj
set(TINYOBJLOADER_BUILD_OBJ_STITCH OFF CACHE INTERNAL "" FORCE)
set(TINYOBJLOADER_BUILD_TEST_LOADER OFF CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

# Automatically collect all source files from src/ and subdirectories
file(GLOB_RECURSE PRISM_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE PRISM_HEADERS 
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

# Print found files for debugging
message(STATUS "Found source files:")
foreach(SOURCE_FILE ${PRISM_SOURCES})
    message(STATUS "  ${SOURCE_FILE}")
endforeach()

# Create executable with all found sources
add_executable(PrismRT ${PRISM_SOURCES} ${PRISM_HEADERS})

# OIIO
find_package(OpenImageIO CONFIG REQUIRED)
target_link_libraries(PrismRT OpenImageIO::OpenImageIO)

# Embree
find_package(embree 4 REQUIRED)
target_link_libraries(PrismRT embree)

# oneTBB
find_package(TBB REQUIRED)
target_link_libraries(PrismRT TBB::tbb)

# OpenUSD Dependency
# TODO: OpenGL shouldn't be needed and yet USD is requiring it?
find_package(OpenGL REQUIRED)

find_package(pxr REQUIRED)

message(STATUS "PXR_LIBRARIES contains: ${PXR_LIBRARIES}")
string(REPLACE ";" "|" PXR_LIBRARIES_DEBUG "${PXR_LIBRARIES}")
message(STATUS "PXR_LIBRARIES debug: ${PXR_LIBRARIES_DEBUG}")

target_link_libraries(PrismRT ${PXR_LIBRARIES})
target_include_directories(PrismRT PUBLIC ${PXR_INCLUDE_DIR})

# Other libraries
target_include_directories(PrismRT PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")
target_include_directories(PrismRT PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

# MDL includes - there's no linking, DLLs are loaded via code (see src/mdl/loader.h)
target_include_directories(PrismRT PUBLIC ${MDL_SDK_DIR}/include)

if(MSVC)
    target_compile_options(PrismRT PUBLIC "/Zc:preprocessor")
endif()

# Copies DLLs our executable depends on to the build folder automatically
add_custom_command(TARGET PrismRT POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:PrismRT> $<TARGET_RUNTIME_DLLS:PrismRT>
  COMMAND_EXPAND_LISTS
)

add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:PrismRT>/assets
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:PrismRT>/assets/logo
    COMMAND ${CMAKE_COMMAND} -E rm -rf $<TARGET_FILE_DIR:PrismRT>/assets/.gitignore
    COMMENT "Copying runtime assets to executable directory..."
)

add_dependencies(PrismRT copy_assets)