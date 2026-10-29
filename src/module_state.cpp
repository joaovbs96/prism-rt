// Process state of header-only core code (src/module_state.h,
// src/internal_export.h). Defined here, in the core library, so that the core,
// prism_usd, hdPrism, the CLI and the tests of one process share one copy
// instead of each module instantiating its own function-local statics.

#include "module_state.h"

#include "material.h"
#include "materialx/material_resource.h"
#include "progress.h"
#include "resource_limits.h"

#include <atomic>

namespace prism {

uint64_t NextTemporaryFileSerial()
{
    static std::atomic<uint64_t> serial{0};
    return ++serial;
}

namespace {
thread_local Progress* t_currentProgress = nullptr;
}

Progress* Progress::Current()
{
    return t_currentProgress;
}

Progress* Progress::Exchange(Progress* model)
{
    Progress* previous = t_currentProgress;
    t_currentProgress = model;
    return previous;
}

PreparationAdmission& PreparationAdmission::Shared()
{
    static PreparationAdmission value;
    return value;
}

namespace materialx::resource_detail {

Registry& GetRegistry()
{
    static auto* registry = new Registry;
    return *registry;
}

} // namespace materialx::resource_detail

} // namespace prism

ErrorMaterial* ErrorMaterial::Shared()
{
    static ErrorMaterial material;
    return &material;
}
