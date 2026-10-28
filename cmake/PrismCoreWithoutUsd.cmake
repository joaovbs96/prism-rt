# The core library has no USD (docs/sdk_design.md, sections 4.1 and 7): no
# pxr target is in its link closure and no pxr include directory is on its
# include path, so a core source that includes a pxr header does not compile.
#
# prism_check_core_without_usd(<target>) fails the configure when that does
# not hold; prism_core_without_usd_test (prism_add_core_without_usd_test)
# checks the generated build settings again (include directories as the generator
# evaluates them, transitive usage requirements included).

# Targets <target> links, directly or through the link interfaces of the
# targets it links (generator expressions unwrapped).
function(_prism_link_closure target out)
    set(seen "")
    set(queue "${target}")
    while(queue)
        list(POP_FRONT queue item)
        if(item IN_LIST seen OR NOT TARGET "${item}")
            continue()
        endif()
        list(APPEND seen "${item}")
        get_target_property(_aliased "${item}" ALIASED_TARGET)
        if(_aliased)
            list(APPEND queue "${_aliased}")
            continue()
        endif()
        get_target_property(_imported "${item}" IMPORTED)
        get_target_property(_type "${item}" TYPE)
        set(_links "")
        if(NOT _imported AND NOT _type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(_direct "${item}" LINK_LIBRARIES)
            if(_direct)
                list(APPEND _links ${_direct})
            endif()
        endif()
        get_target_property(_interface "${item}" INTERFACE_LINK_LIBRARIES)
        if(_interface)
            list(APPEND _links ${_interface})
        endif()
        foreach(_link IN LISTS _links)
            # $<LINK_ONLY:x>, $<$<CONFIG:..>:x>, ...: keep the names inside.
            string(REGEX REPLACE "\\$<[^:>]*:" "" _link "${_link}")
            string(REPLACE ">" "" _link "${_link}")
            string(REPLACE "," ";" _parts "${_link}")
            list(APPEND queue ${_parts})
        endforeach()
    endwhile()
    set(${out} "${seen}" PARENT_SCOPE)
endfunction()

function(prism_check_core_without_usd target)
    _prism_link_closure(${target} _closure)
    set(_pxr_targets "")
    foreach(_lib IN LISTS PXR_LIBRARIES)
        if(TARGET "${_lib}")
            list(APPEND _pxr_targets "${_lib}")
        endif()
    endforeach()
    set(_offenders "")
    set(_includes "")
    foreach(_item IN LISTS _closure)
        if(_item IN_LIST _pxr_targets)
            list(APPEND _offenders "${_item}")
        endif()
        get_target_property(_dirs "${_item}" INTERFACE_INCLUDE_DIRECTORIES)
        if(_dirs)
            list(APPEND _includes ${_dirs})
        endif()
    endforeach()
    get_target_property(_own "${target}" INCLUDE_DIRECTORIES)
    if(_own)
        list(APPEND _includes ${_own})
    endif()
    foreach(_dir IN LISTS PXR_INCLUDE_DIRS)
        file(TO_CMAKE_PATH "${_dir}" _dir)
        foreach(_include IN LISTS _includes)
            file(TO_CMAKE_PATH "${_include}" _include)
            if(_include STREQUAL _dir)
                list(APPEND _offenders "include directory ${_dir}")
            endif()
        endforeach()
    endforeach()
    if(_offenders)
        list(REMOVE_DUPLICATES _offenders)
        message(FATAL_ERROR "The core library '${target}' must not depend on USD, but it reaches: ${_offenders}")
    endif()
    list(LENGTH _closure _count)
    message(STATUS "PrismRT: the core library '${target}' links no USD (${_count} targets in its link closure)")

    # The generator's view: include directories with transitive usage
    # requirements, checked by a test (catches pxr include paths that reach
    # the core through generator expressions).
    set(_settings "${CMAKE_BINARY_DIR}/core_without_usd/$<CONFIG>/settings.cmake")
    file(GENERATE OUTPUT "${_settings}" CONTENT
"set(CORE_INCLUDES [==[$<TARGET_PROPERTY:${target},INCLUDE_DIRECTORIES>]==])
set(CORE_LINKS [==[${_closure}]==])
set(PXR_TARGETS [==[${_pxr_targets}]==])
set(PXR_INCLUDES [==[${PXR_INCLUDE_DIRS}]==])
")
endfunction()

# The test of those settings (call where testing is enabled).
function(prism_add_core_without_usd_test)
    add_test(NAME prism_core_without_usd_test
        COMMAND ${CMAKE_COMMAND} -DSETTINGS=${CMAKE_BINARY_DIR}/core_without_usd/$<CONFIG>/settings.cmake
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/CheckCoreWithoutUsd.cmake")
endfunction()
