cmake_minimum_required(VERSION 3.20)
project(USDPathTracer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

add_executable(USDPathTracer src/main.cpp)

find_package(OpenImageIO CONFIG REQUIRED)
target_link_libraries(USDPathTracer OpenImageIO::OpenImageIO)

find_package(embree 4 REQUIRED)
target_link_libraries(USDPathTracer embree)

find_package(TBB REQUIRED)
target_link_libraries(USDPathTracer TBB::tbb)

# Copies DLLs our executable depends on to the build folder automatically
add_custom_command(TARGET USDPathTracer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:USDPathTracer> $<TARGET_RUNTIME_DLLS:USDPathTracer>
  COMMAND_EXPAND_LISTS
)