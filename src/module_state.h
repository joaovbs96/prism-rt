#pragma once

// Process state of header-only core code, defined once in the core library
// (src/module_state.cpp) so that every module of the process (the core,
// prism_usd, hdPrism, the CLI, tests) shares it (src/internal_export.h).

#include "internal_export.h"

#include <cstdint>

namespace prism {

// A process-wide serial for temporary file names (beside the process id):
// two modules publishing the same cache artifact at once must not pick the
// same name.
PRISM_INTERNAL_API uint64_t NextTemporaryFileSerial();

} // namespace prism
