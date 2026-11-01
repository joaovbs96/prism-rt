# src/color/ against another OpenColorIO (2.2.1), without the rest of Prism.
# docs/deployment.md, "Older dependency stacks".
#
#   cmake -S tests/ocio22 -B build/ocio22 -DOCIO_ROOT=<ocio install> ^
#         [-DOCIO_EXT_LIB_DIR=<dir with the static yaml-cpp, expat, ... libs>] ^
#         -DTBB_DIR=<oneTBB package config>
#   cmake --build build/ocio22 --config Release
#   build/ocio22/Release/prism_ocio22_fallback.exe
#
# With a shared OpenColorIO (OCIO_ROOT has lib/cmake/OpenColorIO and its
# dependencies are found), leave OCIO_EXT_LIB_DIR empty; with a static one,
# point it at the directory holding its dependencies' static libraries.
cmake_minimum_required(VERSION 3.20)
project(PrismOcio22 C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OCIO_ROOT "" CACHE PATH "OpenColorIO install prefix")
set(OCIO_EXT_LIB_DIR "" CACHE PATH "Static dependencies of a static OpenColorIO")
option(OCIO_ZLIB_IMPORT_SHIM "Satisfy a static OpenColorIO's __imp_ zlib references from zlibstatic.lib" OFF)
find_package(TBB CONFIG REQUIRED)

add_executable(prism_ocio22_fallback
    ocio22_fallback.cpp
    ../../src/color/color_management.cpp)
target_compile_definitions(prism_ocio22_fallback PRIVATE PRISM_BUILDING_CORE NOMINMAX)
target_include_directories(prism_ocio22_fallback PRIVATE "${OCIO_ROOT}/include")
if(OCIO_EXT_LIB_DIR)
    file(GLOB _ocio_ext_libs "${OCIO_EXT_LIB_DIR}/*.lib")
    if(OCIO_ZLIB_IMPORT_SHIM)
        target_sources(prism_ocio22_fallback PRIVATE zlib_import_shim.c)
    endif()
    target_compile_definitions(prism_ocio22_fallback PRIVATE OpenColorIO_SKIP_IMPORTS)
    target_link_libraries(prism_ocio22_fallback PRIVATE "${OCIO_ROOT}/lib/OpenColorIO.lib" ${_ocio_ext_libs})
else()
    find_package(OpenColorIO CONFIG REQUIRED PATHS "${OCIO_ROOT}" NO_DEFAULT_PATH)
    target_link_libraries(prism_ocio22_fallback PRIVATE OpenColorIO::OpenColorIO)
endif()
target_link_libraries(prism_ocio22_fallback PRIVATE TBB::tbb)
