#pragma once

// Internal symbols exported from the shared libraries (include/prism/export.h)
// because the front ends and tests still use them: hdPrism builds its own
// SceneManager and drives the progressive engine, the CLI parses device and
// denoise names, and the tests reach into materials, devices and caches.
// They are not API: this header is not installed, and the list shrinks as
// Hydra moves onto the public Session (docs/DECISIONS.md, 2026-10-29).
//
// Header-only code that holds state (function-local statics, inline or
// thread_local variables) must not live in these headers: each module would
// get its own copy. Such state is defined in the library's .cpp files
// (src/module_state.cpp, src/usd/usd_module_state.cpp) and reached through
// exported functions.

#include "../include/prism/export.h"

#if defined(PRISM_BUILDING_CORE)
#define PRISM_INTERNAL_API PRISM_EXPORT_SYMBOL
#else
#define PRISM_INTERNAL_API PRISM_IMPORT_SYMBOL
#endif

#if defined(PRISM_BUILDING_USD)
#define PRISM_USD_INTERNAL_API PRISM_EXPORT_SYMBOL
#else
#define PRISM_USD_INTERNAL_API PRISM_IMPORT_SYMBOL
#endif
