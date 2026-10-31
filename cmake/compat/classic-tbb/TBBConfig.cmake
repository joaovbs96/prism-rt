# Best-effort shim for building PrismRT against classic TBB (2020.x, the
# pre-oneTBB releases some host applications ship). Not CI-tested; see
# "Older dependency stacks" in docs/deployment.md.
#
#   cmake -DTBB_DIR=<prism>/cmake/compat/classic-tbb \
#         -DPRISM_CLASSIC_TBB_ROOT=<tbb-2020.3>/tbb ...
#
# It loads the TBBConfig.cmake that ships with classic TBB (which defines
# TBB::tbb and friends), reports its version, and adds NOMINMAX to TBB::tbb:
# classic TBB's headers include windows.h, whose min/max macros break
# std::min/std::max in every consumer (PrismRT defines NOMINMAX itself; this
# covers other targets that link TBB::tbb).
if(NOT PRISM_CLASSIC_TBB_ROOT)
    set(TBB_FOUND FALSE)
    set(TBB_NOT_FOUND_MESSAGE "Set PRISM_CLASSIC_TBB_ROOT to the classic TBB root (the directory holding include/ and cmake/).")
    return()
endif()
include("${PRISM_CLASSIC_TBB_ROOT}/cmake/TBBConfig.cmake")
include("${PRISM_CLASSIC_TBB_ROOT}/cmake/TBBConfigVersion.cmake" OPTIONAL)
if(NOT TBB_VERSION AND PACKAGE_VERSION)
    set(TBB_VERSION "${PACKAGE_VERSION}")
endif()
if(WIN32 AND TARGET TBB::tbb)
    set_property(TARGET TBB::tbb APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS NOMINMAX)
endif()
