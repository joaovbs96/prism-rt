# Best-effort shim for building PrismRT against classic TBB (2020.x, the
# pre-oneTBB releases some host applications ship). Not CI-tested; see
# "Older dependency stacks" in docs/deployment.md.
#
#   cmake -DTBB_DIR=<prism>/cmake/compat/classic-tbb \
#         -DPRISM_CLASSIC_TBB_ROOT=<tbb-2020.3>/tbb ...
#
# Two modes, by what the TBB root holds:
#  - <root>/cmake/TBBConfig.cmake exists (the official 2020.x release): it is
#    loaded, and defines TBB::tbb and friends;
#  - it does not (a repackaged TBB with only include/, bin/ and lib/): TBB::tbb
#    (and TBB::tbbmalloc, when its library is there) are defined here from
#    include/ and the libraries found under lib/ (lib/, lib/intel64/vc14 and
#    similar) and bin/ (the DLLs); the version comes from
#    include/tbb/tbb_stddef.h.
# Either way TBB::tbb gets NOMINMAX: classic TBB's headers include windows.h,
# whose min/max macros break std::min/std::max in every consumer (PrismRT
# defines NOMINMAX itself; this covers other targets that link TBB::tbb).
if(NOT PRISM_CLASSIC_TBB_ROOT)
    set(TBB_FOUND FALSE)
    set(TBB_NOT_FOUND_MESSAGE "Set PRISM_CLASSIC_TBB_ROOT to the classic TBB root (the directory holding include/ and lib/, usually cmake/ too).")
    return()
endif()

if(EXISTS "${PRISM_CLASSIC_TBB_ROOT}/cmake/TBBConfig.cmake")
    include("${PRISM_CLASSIC_TBB_ROOT}/cmake/TBBConfig.cmake")
    include("${PRISM_CLASSIC_TBB_ROOT}/cmake/TBBConfigVersion.cmake" OPTIONAL)
    if(NOT TBB_VERSION AND PACKAGE_VERSION)
        set(TBB_VERSION "${PACKAGE_VERSION}")
    endif()
else()
    set(_prism_tbb_root "${PRISM_CLASSIC_TBB_ROOT}")
    set(_prism_tbb_include "${_prism_tbb_root}/include")
    if(NOT EXISTS "${_prism_tbb_include}/tbb/tbb.h" AND EXISTS "${_prism_tbb_root}/tbb/include/tbb/tbb.h")
        set(_prism_tbb_include "${_prism_tbb_root}/tbb/include")
    endif()
    if(NOT EXISTS "${_prism_tbb_include}/tbb/tbb.h")
        set(TBB_FOUND FALSE)
        set(TBB_NOT_FOUND_MESSAGE "PRISM_CLASSIC_TBB_ROOT='${_prism_tbb_root}' has neither cmake/TBBConfig.cmake nor include/tbb/tbb.h.")
        return()
    endif()
    get_filename_component(_prism_tbb_base "${_prism_tbb_include}" DIRECTORY)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_prism_tbb_arch intel64)
    else()
        set(_prism_tbb_arch ia32)
    endif()
    set(_prism_tbb_subdirs
        lib/${_prism_tbb_arch}/vc14 lib/${_prism_tbb_arch}/vc14_uwp lib/${_prism_tbb_arch}/vc_mt lib/${_prism_tbb_arch}
        lib/x64 lib/${_prism_tbb_arch}/Release lib/Release lib)
    set(_prism_tbb_binsubdirs
        bin/${_prism_tbb_arch}/vc14 bin/${_prism_tbb_arch}/vc_mt bin/${_prism_tbb_arch} bin/x64 bin/Release bin lib)

    # Version from the header (TBB_VERSION_MAJOR 2020 / TBB_VERSION_MINOR 3).
    set(TBB_VERSION "")
    if(EXISTS "${_prism_tbb_include}/tbb/tbb_stddef.h")
        file(STRINGS "${_prism_tbb_include}/tbb/tbb_stddef.h" _prism_tbb_major REGEX "^#define[ \t]+TBB_VERSION_MAJOR[ \t]+[0-9]+")
        file(STRINGS "${_prism_tbb_include}/tbb/tbb_stddef.h" _prism_tbb_minor REGEX "^#define[ \t]+TBB_VERSION_MINOR[ \t]+[0-9]+")
        file(STRINGS "${_prism_tbb_include}/tbb/tbb_stddef.h" _prism_tbb_interface REGEX "^#define[ \t]+TBB_INTERFACE_VERSION[ \t]+[0-9]+")
        string(REGEX REPLACE ".*TBB_VERSION_MAJOR[ \t]+([0-9]+).*" "\\1" _prism_tbb_major "${_prism_tbb_major}")
        string(REGEX REPLACE ".*TBB_VERSION_MINOR[ \t]+([0-9]+).*" "\\1" _prism_tbb_minor "${_prism_tbb_minor}")
        string(REGEX REPLACE ".*TBB_INTERFACE_VERSION[ \t]+([0-9]+).*" "\\1" _prism_tbb_interface "${_prism_tbb_interface}")
        if(_prism_tbb_major MATCHES "^[0-9]+$" AND _prism_tbb_minor MATCHES "^[0-9]+$")
            set(TBB_VERSION "${_prism_tbb_major}.${_prism_tbb_minor}")
        endif()
        if(_prism_tbb_interface MATCHES "^[0-9]+$")
            set(TBB_INTERFACE_VERSION "${_prism_tbb_interface}")
        endif()
    endif()

    set(_prism_tbb_all_found TRUE)
    set(TBB_IMPORTED_TARGETS "")
    foreach(_prism_tbb_component IN ITEMS tbb tbbmalloc)
        set(_prism_tbb_hints "")
        set(_prism_tbb_binhints "")
        foreach(_prism_tbb_sub IN LISTS _prism_tbb_subdirs)
            list(APPEND _prism_tbb_hints "${_prism_tbb_base}/${_prism_tbb_sub}")
        endforeach()
        foreach(_prism_tbb_sub IN LISTS _prism_tbb_binsubdirs)
            list(APPEND _prism_tbb_binhints "${_prism_tbb_base}/${_prism_tbb_sub}")
        endforeach()
        # Fresh lookups each time (find_* caches a NOTFOUND otherwise).
        unset(_prism_tbb_release_lib CACHE)
        unset(_prism_tbb_debug_lib CACHE)
        unset(_prism_tbb_release_dll CACHE)
        unset(_prism_tbb_debug_dll CACHE)
        find_library(_prism_tbb_release_lib NAMES ${_prism_tbb_component} HINTS ${_prism_tbb_hints} NO_DEFAULT_PATH)
        find_library(_prism_tbb_debug_lib NAMES ${_prism_tbb_component}_debug HINTS ${_prism_tbb_hints} NO_DEFAULT_PATH)
        find_file(_prism_tbb_release_dll NAMES ${_prism_tbb_component}.dll HINTS ${_prism_tbb_binhints} NO_DEFAULT_PATH)
        find_file(_prism_tbb_debug_dll NAMES ${_prism_tbb_component}_debug.dll HINTS ${_prism_tbb_binhints} NO_DEFAULT_PATH)
        if(NOT _prism_tbb_release_lib AND _prism_tbb_debug_lib)
            set(_prism_tbb_release_lib "${_prism_tbb_debug_lib}")
            set(_prism_tbb_release_dll "${_prism_tbb_debug_dll}")
        endif()
        if(NOT _prism_tbb_release_lib)
            if(_prism_tbb_component STREQUAL "tbb")
                set(_prism_tbb_all_found FALSE)
            endif()
            continue()
        endif()
        if(NOT TARGET TBB::${_prism_tbb_component})
            if(_prism_tbb_release_dll)
                add_library(TBB::${_prism_tbb_component} SHARED IMPORTED)
                set_target_properties(TBB::${_prism_tbb_component} PROPERTIES
                    IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
                    IMPORTED_LOCATION_RELEASE "${_prism_tbb_release_dll}"
                    IMPORTED_IMPLIB_RELEASE "${_prism_tbb_release_lib}")
                if(_prism_tbb_debug_dll AND _prism_tbb_debug_lib)
                    set_target_properties(TBB::${_prism_tbb_component} PROPERTIES
                        IMPORTED_LOCATION_DEBUG "${_prism_tbb_debug_dll}"
                        IMPORTED_IMPLIB_DEBUG "${_prism_tbb_debug_lib}")
                else()
                    # No debug runtime shipped: Debug builds use the release one.
                    set_target_properties(TBB::${_prism_tbb_component} PROPERTIES
                        IMPORTED_LOCATION_DEBUG "${_prism_tbb_release_dll}"
                        IMPORTED_IMPLIB_DEBUG "${_prism_tbb_release_lib}")
                endif()
            else()
                add_library(TBB::${_prism_tbb_component} UNKNOWN IMPORTED)
                set_target_properties(TBB::${_prism_tbb_component} PROPERTIES
                    IMPORTED_CONFIGURATIONS "RELEASE;DEBUG"
                    IMPORTED_LOCATION_RELEASE "${_prism_tbb_release_lib}"
                    IMPORTED_LOCATION_DEBUG "${_prism_tbb_debug_lib}")
                if(NOT _prism_tbb_debug_lib)
                    set_target_properties(TBB::${_prism_tbb_component} PROPERTIES
                        IMPORTED_LOCATION_DEBUG "${_prism_tbb_release_lib}")
                endif()
            endif()
            set_target_properties(TBB::${_prism_tbb_component} PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${_prism_tbb_include}"
                MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RELEASE"
                MAP_IMPORTED_CONFIG_MINSIZEREL "RELEASE")
        endif()
        list(APPEND TBB_IMPORTED_TARGETS TBB::${_prism_tbb_component})
        set(TBB_${_prism_tbb_component}_FOUND TRUE)
    endforeach()
    unset(_prism_tbb_release_lib CACHE)
    unset(_prism_tbb_debug_lib CACHE)
    unset(_prism_tbb_release_dll CACHE)
    unset(_prism_tbb_debug_dll CACHE)
    if(NOT _prism_tbb_all_found)
        set(TBB_FOUND FALSE)
        set(TBB_NOT_FOUND_MESSAGE "No tbb library (tbb.lib) under '${_prism_tbb_base}/lib' (looked in lib/, lib/${_prism_tbb_arch}/vc14, ...).")
        return()
    endif()
endif()

if(WIN32 AND TARGET TBB::tbb)
    set_property(TARGET TBB::tbb APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS NOMINMAX)
endif()
