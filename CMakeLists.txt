cmake_minimum_required(VERSION 3.20)
project(PrismRT)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

set(TINYGLTF_HEADER_ONLY ON CACHE INTERNAL "" FORCE)
set(TINYGLTF_INSTALL OFF CACHE INTERNAL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

add_executable(PrismRT src/main.cpp)

find_package(OpenImageIO CONFIG REQUIRED)
target_link_libraries(PrismRT OpenImageIO::OpenImageIO)

find_package(embree 4 REQUIRED)
target_link_libraries(PrismRT embree)

find_package(TBB REQUIRED)
target_link_libraries(PrismRT TBB::tbb)

message(STATUS "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")
message(STATUS "${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

target_include_directories(PrismRT PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/tinygltf")

target_include_directories(PrismRT PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobj")

# Copies DLLs our executable depends on to the build folder automatically
add_custom_command(TARGET PrismRT POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:PrismRT> $<TARGET_RUNTIME_DLLS:PrismRT>
  COMMAND_EXPAND_LISTS
)