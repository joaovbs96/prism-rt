# Dependency versions come from tools/deps.json, the single version list (see
# "How to upgrade a dependency" in docs/deployment.md). This module reads it,
# helps find versioned package directories, and checks that the packages CMake
# found are the versions the list names.

set(PRISM_DEPS_JSON "${CMAKE_CURRENT_LIST_DIR}/../tools/deps.json")
get_filename_component(PRISM_DEPS_JSON "${PRISM_DEPS_JSON}" ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PRISM_DEPS_JSON}")
file(READ "${PRISM_DEPS_JSON}" _prism_deps_json)

# PRISM_DEP_<COMPONENT>_VERSION for every component of deps.json.
foreach(_component openusd materialx opensubdiv onetbb embree oidn vk_oidn mdl_sdk python vulkan_sdk)
    string(TOUPPER "${_component}" _upper)
    string(JSON PRISM_DEP_${_upper}_VERSION GET "${_prism_deps_json}" ${_component} version)
endforeach()
string(JSON PRISM_DEP_OPENIMAGEIO_VERSION GET "${_prism_deps_json}" vcpkg openimageio)
string(JSON PRISM_DEP_OPENCOLORIO_VERSION GET "${_prism_deps_json}" vcpkg opencolorio)

# Strict on Windows, whose CI and local stacks are built from deps.json. Other
# platforms (the macOS/Metal build) may use system packages, so they only warn.
if(WIN32)
    set(_prism_strict_default ON)
else()
    set(_prism_strict_default OFF)
endif()
option(PRISM_STRICT_DEPENDENCY_VERSIONS
    "Fail the configure when a dependency's version differs from tools/deps.json (OFF: warn)"
    ${_prism_strict_default})

# <out> = the lib/cmake/embree-<version> directory under <root>: the one
# deps.json names when present, else the newest.
function(prism_find_embree_dir out root)
    set(_dir "${root}/lib/cmake/embree-${PRISM_DEP_EMBREE_VERSION}")
    if(NOT EXISTS "${_dir}")
        file(GLOB _dirs LIST_DIRECTORIES true "${root}/lib/cmake/embree-*")
        set(_dir "")
        if(_dirs)
            list(SORT _dirs COMPARE NATURAL ORDER DESCENDING)
            list(GET _dirs 0 _dir)
        endif()
    endif()
    set(${out} "${_dir}" PARENT_SCOPE)
endfunction()

# True when one dotted version is a component-wise prefix of the other
# ("2022.2" matches "2022.2.0"; "26.05" does not match "26.08").
function(_prism_versions_compatible out found expected)
    string(REPLACE "." ";" _a "${found}")
    string(REPLACE "." ";" _b "${expected}")
    list(LENGTH _a _na)
    list(LENGTH _b _nb)
    if(_na LESS _nb)
        set(_n ${_na})
    else()
        set(_n ${_nb})
    endif()
    set(_ok TRUE)
    if(_n EQUAL 0)
        set(_ok FALSE)
    else()
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE ${_last})
            list(GET _a ${_i} _x)
            list(GET _b ${_i} _y)
            # Numeric comparison, so "08" equals "8".
            if(NOT _x EQUAL _y)
                set(_ok FALSE)
            endif()
        endforeach()
    endif()
    set(${out} ${_ok} PARENT_SCOPE)
endfunction()

set(_prism_dependency_mismatches "")
function(prism_check_dependency_version name found expected)
    if(NOT found)
        message(STATUS "PrismRT: ${name} version unknown (tools/deps.json: ${expected})")
        return()
    endif()
    _prism_versions_compatible(_ok "${found}" "${expected}")
    if(_ok)
        message(STATUS "PrismRT: ${name} ${found}")
    else()
        set(_prism_dependency_mismatches
            "${_prism_dependency_mismatches}\n  ${name}: found ${found}, tools/deps.json says ${expected}"
            PARENT_SCOPE)
    endif()
endfunction()

# Call after the find_package calls.
macro(prism_check_dependency_versions)
    set(_prism_dependency_mismatches "")
    if(DEFINED PXR_MINOR_VERSION)
        if(PXR_PATCH_VERSION LESS 10)
            set(_prism_found_pxr "${PXR_MINOR_VERSION}.0${PXR_PATCH_VERSION}")
        else()
            set(_prism_found_pxr "${PXR_MINOR_VERSION}.${PXR_PATCH_VERSION}")
        endif()
        prism_check_dependency_version(OpenUSD "${_prism_found_pxr}" "${PRISM_DEP_OPENUSD_VERSION}")
    endif()
    prism_check_dependency_version(MaterialX "${MaterialX_VERSION}" "${PRISM_DEP_MATERIALX_VERSION}")
    prism_check_dependency_version(OpenSubdiv "${OpenSubdiv_VERSION}" "${PRISM_DEP_OPENSUBDIV_VERSION}")
    prism_check_dependency_version(oneTBB "${TBB_VERSION}" "${PRISM_DEP_ONETBB_VERSION}")
    prism_check_dependency_version(Embree "${embree_VERSION}" "${PRISM_DEP_EMBREE_VERSION}")
    prism_check_dependency_version(OpenImageIO "${OpenImageIO_VERSION}" "${PRISM_DEP_OPENIMAGEIO_VERSION}")
    if(PRISM_ENABLE_OIDN)
        prism_check_dependency_version("Open Image Denoise" "${OpenImageDenoise_VERSION}" "${PRISM_DEP_OIDN_VERSION}")
    endif()
    if(PRISM_ENABLE_VK_OIDN)
        prism_check_dependency_version(vk_oidn "${vk_oidn_VERSION}" "${PRISM_DEP_VK_OIDN_VERSION}")
    endif()
    prism_check_dependency_version(OpenColorIO "${OpenColorIO_VERSION}" "${PRISM_DEP_OPENCOLORIO_VERSION}")
    if(Python3_VERSION_MAJOR)
        prism_check_dependency_version(Python
            "${Python3_VERSION_MAJOR}.${Python3_VERSION_MINOR}" "${PRISM_DEP_PYTHON_VERSION}")
    endif()
    if(MDL_SDK_DIR AND EXISTS "${MDL_SDK_DIR}/include/mi/neuraylib/version.h")
        file(STRINGS "${MDL_SDK_DIR}/include/mi/neuraylib/version.h" _prism_mdl_line
            REGEX "#define MI_NEURAYLIB_PRODUCT_VERSION_STRING")
        string(REGEX MATCH "\"([^\"]+)\"" _prism_mdl_match "${_prism_mdl_line}")
        prism_check_dependency_version("MDL SDK" "${CMAKE_MATCH_1}" "${PRISM_DEP_MDL_SDK_VERSION}")
    endif()
    if(_prism_dependency_mismatches)
        set(_prism_message "Dependency versions differ from tools/deps.json:${_prism_dependency_mismatches}\n"
            "Point the build at the stack deps.json names (tools/build_deps_local.ps1 builds it), "
            "or pass -DPRISM_STRICT_DEPENDENCY_VERSIONS=OFF to continue anyway.")
        if(PRISM_STRICT_DEPENDENCY_VERSIONS)
            message(FATAL_ERROR ${_prism_message})
        else()
            message(WARNING ${_prism_message})
        endif()
    endif()
endmacro()
