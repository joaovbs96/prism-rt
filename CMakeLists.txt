cmake_minimum_required(VERSION 3.20)
project(PrismRT)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # This line is optional. It's just for certain IDEs to use

# tiny gltf
set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")

# tiny obj
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

add_executable(PrismRT src/main.cpp)

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

# Copies DLLs our executable depends on to the build folder automatically
add_custom_command(TARGET PrismRT POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:PrismRT> $<TARGET_RUNTIME_DLLS:PrismRT>
  COMMAND_EXPAND_LISTS
)
