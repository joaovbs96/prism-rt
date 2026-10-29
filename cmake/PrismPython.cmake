# The Python package prismrt (docs/python.md): the nanobind extension
# modules prismrt._prism (the core with glTF, OBJ and image writing) and
# prismrt._prism_usd (prism_usd), linked to the shared prism.dll and
# prism_usd.dll (never a second copy of the core), the pure-Python package
# from python/prismrt, generated .pyi stubs, and pytest tests under CTest.
#
# The build tree's package is ${CMAKE_BINARY_DIR}/python/prismrt; put
# ${CMAKE_BINARY_DIR}/python on PYTHONPATH. _build_config.py there names the
# directories of prism.dll and its dependencies (os.add_dll_directory) and
# the MaterialX/MDL defaults, so no PATH setup is needed.
#
# nanobind comes from tools/deps.json's version: nanobind_DIR (its cmake
# directory, e.g. a `pip install --target <prefix> nanobind==<version>`),
# else `python -m nanobind --cmake_dir`. PRISM_BUILD_PYTHON defaults to ON
# when nanobind is found.

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps.json" _prism_python_deps)
string(JSON PRISM_NANOBIND_VERSION GET "${_prism_python_deps}" nanobind version)

if(NOT nanobind_DIR AND Python3_EXECUTABLE)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -m nanobind --cmake_dir
        OUTPUT_VARIABLE _prism_nanobind_dir OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _prism_nanobind_result ERROR_QUIET)
    if(_prism_nanobind_result EQUAL 0 AND EXISTS "${_prism_nanobind_dir}")
        set(nanobind_DIR "${_prism_nanobind_dir}" CACHE PATH "nanobind's CMake package directory")
    endif()
endif()
# nanobind's CMake functions use FindPython's Python_* names: the same
# interpreter as the rest of the build (Python3_*).
if(nanobind_DIR AND NOT TARGET Python::Module)
    set(Python_EXECUTABLE "${Python3_EXECUTABLE}")
    find_package(Python 3.11 COMPONENTS Interpreter Development.Module QUIET)
    # As for Python3::Python (CMakeLists.txt): every configuration of the
    # Visual Studio generators links the one Python library.
    if(WIN32 AND TARGET Python::Module)
        foreach(_config DEBUG RELEASE MINSIZEREL RELWITHDEBINFO)
            set_property(TARGET Python::Module PROPERTY IMPORTED_IMPLIB_${_config} "${Python3_LIBRARY_RELEASE}")
        endforeach()
    endif()
endif()
if(nanobind_DIR AND TARGET Python::Module)
    find_package(nanobind CONFIG QUIET)
endif()
if(nanobind_FOUND)
    set(_prism_python_default ON)
else()
    set(_prism_python_default OFF)
endif()
option(PRISM_BUILD_PYTHON "Build the Python package prismrt (nanobind; default: ON when nanobind is found)"
    ${_prism_python_default})
if(NOT PRISM_BUILD_PYTHON)
    if(nanobind_DIR OR nanobind_FOUND)
        message(STATUS "PrismRT: Python bindings off (PRISM_BUILD_PYTHON=OFF)")
    else()
        message(STATUS "PrismRT: Python bindings off (nanobind not found; docs/python.md)")
    endif()
    return()
endif()
if(NOT nanobind_FOUND)
    message(FATAL_ERROR "PRISM_BUILD_PYTHON needs nanobind ${PRISM_NANOBIND_VERSION} (set nanobind_DIR; "
        "docs/python.md) and Python's Development.Module component")
endif()
if(NOT nanobind_VERSION VERSION_EQUAL PRISM_NANOBIND_VERSION)
    message(WARNING "PrismRT: nanobind ${nanobind_VERSION}, tools/deps.json says ${PRISM_NANOBIND_VERSION}")
endif()
message(STATUS "PrismRT: nanobind ${nanobind_VERSION} (Python ${Python_VERSION})")

set(PRISM_PYTHON_PACKAGE_DIR "${CMAKE_BINARY_DIR}/python/prismrt")
file(GLOB PRISM_PYTHON_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/python/prismrt/*.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/python/prismrt/py.typed")

# Python 3.11: no stable ABI (nanobind's linked stable ABI needs 3.12+; docs/DECISIONS.md).
nanobind_add_module(_prism NB_STATIC "${CMAKE_CURRENT_SOURCE_DIR}/python/src/module.cpp")
# glTF and OBJ first (the tinyobjloader link-order note in CMakeLists.txt).
target_link_libraries(_prism PRIVATE prism_gltf prism_obj prism prism_io)
nanobind_add_module(_prism_usd NB_STATIC "${CMAKE_CURRENT_SOURCE_DIR}/python/src/usd_module.cpp")
target_link_libraries(_prism_usd PRIVATE prism prism_usd)
if(WIN32)
    target_link_libraries(_prism_usd PRIVATE psapi)
endif()
foreach(_prism_python_module IN ITEMS _prism _prism_usd)
    target_include_directories(${_prism_python_module} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/python/src")
    set_target_properties(${_prism_python_module} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PRISM_PYTHON_PACKAGE_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${PRISM_PYTHON_PACKAGE_DIR}")
    foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        set_target_properties(${_prism_python_module} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${_config} "${PRISM_PYTHON_PACKAGE_DIR}"
            RUNTIME_OUTPUT_DIRECTORY_${_config} "${PRISM_PYTHON_PACKAGE_DIR}")
    endforeach()
endforeach()

# The pure-Python package beside the extensions.
set(_prism_python_copied "")
foreach(_source IN LISTS PRISM_PYTHON_SOURCES)
    get_filename_component(_name "${_source}" NAME)
    add_custom_command(OUTPUT "${PRISM_PYTHON_PACKAGE_DIR}/${_name}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_source}" "${PRISM_PYTHON_PACKAGE_DIR}/${_name}"
        DEPENDS "${_source}" COMMENT "prismrt: ${_name}")
    list(APPEND _prism_python_copied "${PRISM_PYTHON_PACKAGE_DIR}/${_name}")
endforeach()

# The build tree's DLL directories and environment defaults (prismrt/_dll.py).
set(_prism_python_dll_dirs "$<TARGET_FILE_DIR:prism>" "$<TARGET_FILE_DIR:prism_usd>" "$<TARGET_FILE_DIR:usd>")
foreach(_dir IN ITEMS "${PRISM_VCPKG_BIN}" "${PRISM_TBB_REDIST}" "${PRISM_EMBREE_RUNTIME_DIR}"
        "${PRISM_EMBREE_BUILD_RUNTIME}" "${PRISM_OIDN_RUNTIME}" "${MDL_SDK_DIR}/bin" "${MDL_SDK_DIR}/nt-x86-64/lib"
        "${PRISM_USD_ROOT}/bin" "${PRISM_USD_ROOT}/lib" "${PRISM_OPENSUBDIV_ROOT}/bin"
        "${PRISM_MATERIALX_ROOT}/bin" "${PRISM_MATERIALX_ROOT}/lib")
    if(_dir AND EXISTS "${_dir}" AND NOT _dir MATCHES "^/")
        list(APPEND _prism_python_dll_dirs "${_dir}")
    endif()
endforeach()
set(_prism_python_dll_list "")
foreach(_dir IN LISTS _prism_python_dll_dirs)
    string(APPEND _prism_python_dll_list "    r\"${_dir}\",\n")
endforeach()
# The MSVC runtime prism.dll needs: 14.<compiler minor> (19.4x <-> 14.4x).
set(_prism_msvc_runtime "None")
if(MSVC AND CMAKE_CXX_COMPILER_VERSION MATCHES "^19\\.([0-9]+)")
    set(_prism_msvc_runtime "(14, ${CMAKE_MATCH_1})")
endif()
# Per configuration (the DLL directories are), copied into the package by
# the configuration that builds it.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/python_config/$<CONFIG>/_build_config.py" CONTENT
"# Generated by cmake/PrismPython.cmake for this build tree (not packaged).
DLL_DIRECTORIES = [
${_prism_python_dll_list}]
ENVIRONMENT = {
    \"PRISM_MATERIALX_LIBRARY_PATH\": r\"${PRISM_MATERIALX_LIBRARY_PATH}\",
    \"PRISM_MATERIALX_MDL\": r\"${PRISM_MATERIALX_MDL}\",
    \"MDL_SDK_ROOT\": r\"${MDL_SDK_DIR}\",
}
USD_VERSION = (${PXR_MAJOR_VERSION}, ${PXR_MINOR_VERSION}, ${PXR_PATCH_VERSION})
USD_LIBRARY = r\"$<TARGET_FILE:usd>\"
MSVC_RUNTIME_MINIMUM = ${_prism_msvc_runtime}
")
add_custom_command(TARGET _prism POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/python_config/$<CONFIG>/_build_config.py"
        "${PRISM_PYTHON_PACKAGE_DIR}/_build_config.py"
    COMMENT "prismrt: _build_config.py")

# Stubs (nanobind's stubgen imports the built module through the package).
set(_prism_stub_python_path "${CMAKE_BINARY_DIR}/python")
nanobind_add_stub(_prism_stub MODULE prismrt._prism OUTPUT "${PRISM_PYTHON_PACKAGE_DIR}/_prism.pyi"
    PYTHON_PATH "${_prism_stub_python_path}" DEPENDS _prism ${_prism_python_copied})
nanobind_add_stub(_prism_usd_stub MODULE prismrt._prism_usd OUTPUT "${PRISM_PYTHON_PACKAGE_DIR}/_prism_usd.pyi"
    PYTHON_PATH "${_prism_stub_python_path}" DEPENDS _prism_usd _prism_stub ${_prism_python_copied})
add_custom_target(prismrt_python ALL DEPENDS ${_prism_python_copied} _prism _prism_usd _prism_stub _prism_usd_stub)

# Tests: pytest under CTest (label python; the Vulkan test also gpu). They
# need pytest and NumPy in this Python (docs/python.md).
if(PRISM_BUILD_HYDRA)
    set(_prism_python_test_env ${_prism_test_environment}
        "PRISMRT_TEST_PACKAGE=${CMAKE_BINARY_DIR}/python"
        "PRISMRT_TEST_PXR=${PRISM_USD_ROOT}/lib/python"
        "PRISMRT_TEST_CLI=$<TARGET_FILE:PrismRT>"
        "PRISMRT_TEST_COMPARE=$<TARGET_FILE:prism_compare_images>"
        "PRISMRT_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        "PRISMRT_TEST_OUTPUT=${CMAKE_BINARY_DIR}/python_tests")
    foreach(_prism_python_test IN ITEMS api render progress exit usd preview vulkan)
        set(_name prism_python_${_prism_python_test})
        if(_prism_python_test STREQUAL "vulkan")
            if(NOT PRISM_ENABLE_VULKAN)
                continue()
            endif()
            set(_name prism_python_render_vulkan)
        endif()
        add_test(NAME ${_name}
            COMMAND "${Python3_EXECUTABLE}" -m pytest -q -p no:cacheprovider -rs
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_${_prism_python_test}.py")
        set(_env ${_prism_python_test_env})
        if(NOT _prism_python_test STREQUAL "vulkan")
            list(APPEND _env "PRISM_DISABLE_VULKAN=1")  # CPU tests open no GPU device
        endif()
        set_tests_properties(${_name} PROPERTIES ENVIRONMENT "${_env}" LABELS python
            TIMEOUT 600 SKIP_REGULAR_EXPRESSION "[0-9]+ skipped, 0 passed|^[0-9]+ skipped in")
    endforeach()
endif()

# The wheel (tools/build_wheel.py, Windows so far): built from this tree,
# installed into a fresh venv in the build directory and rendered there with
# a sanitized environment, against the build tree's images (CPU, exact).
# It needs pip to reach NumPy (tools/deps.json's version; pip's cache works).
if(WIN32 AND TARGET prism_compare_images)
    add_test(NAME prism_python_wheel_install
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/wheel_install_test.py")
    set_tests_properties(prism_python_wheel_install PROPERTIES LABELS python TIMEOUT 1800
        ENVIRONMENT "PRISMRT_TEST_BUILD=${CMAKE_BINARY_DIR};PRISMRT_TEST_CONFIG=$<CONFIG>;PRISMRT_TEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR};PRISMRT_TEST_COMPARE=$<TARGET_FILE:prism_compare_images>;PRISMRT_TEST_USD=1")
endif()
