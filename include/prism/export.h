#pragma once

// Symbol visibility of the PrismRT libraries (docs/sdk_design.md, section 4.1).
//
// `prism` (the core) and `prism_usd` are shared libraries: one copy of their
// state per process (the MDL SDK runtime, Embree/TBB state, the caches), which
// hdPrism, the CLI and the Python module all link. Only what is marked is
// visible outside them:
// - PRISM_API: the public API (include/prism/*), from the core.
// - PRISM_USD_API: the USD module's entry points (prism/usd.h).
// Internal symbols that the front ends and tests still need use
// PRISM_INTERNAL_API / PRISM_USD_INTERNAL_API (src/internal_export.h, not
// installed).
//
// The build defines PRISM_BUILDING_CORE when compiling the core and
// PRISM_BUILDING_USD when compiling prism_usd; PRISM_STATIC makes every macro
// empty (sources compiled into a test directly).

#if defined(PRISM_STATIC)
#define PRISM_EXPORT_SYMBOL
#define PRISM_IMPORT_SYMBOL
#elif defined(_WIN32)
#define PRISM_EXPORT_SYMBOL __declspec(dllexport)
#define PRISM_IMPORT_SYMBOL __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define PRISM_EXPORT_SYMBOL __attribute__((visibility("default")))
#define PRISM_IMPORT_SYMBOL __attribute__((visibility("default")))
#else
#define PRISM_EXPORT_SYMBOL
#define PRISM_IMPORT_SYMBOL
#endif

#if defined(PRISM_BUILDING_CORE)
#define PRISM_API PRISM_EXPORT_SYMBOL
#else
#define PRISM_API PRISM_IMPORT_SYMBOL
#endif

#if defined(PRISM_BUILDING_USD)
#define PRISM_USD_API PRISM_EXPORT_SYMBOL
#else
#define PRISM_USD_API PRISM_IMPORT_SYMBOL
#endif

#if defined(_MSC_VER)
// Exported classes hold standard-library members and derive from standard
// types (std::runtime_error). Both sides are built with the same toolset and
// runtime, so these two "needs dll-interface" warnings are noise; nothing
// else is silenced.
#pragma warning(disable : 4251 4275)
#endif
