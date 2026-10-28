#include "shared_mdl_context.h"

#include "../mdl/context.h"

#include <atomic>

namespace prism::materials {
namespace {
std::atomic<mdl::Context*> gSharedMdlContext{nullptr};
}

mdl::Context& SharedMdlContext()
{
    // Intentionally process-lifetime: MDL target code and backend objects use
    // interfaces owned by INeuray, including across renderer replacement.
    static mdl::Context* context = [] {
        mdl::Context* result = new mdl::Context();
        gSharedMdlContext.store(result, std::memory_order_release);
        return result;
    }();
    return *context;
}

mdl::Context* SharedMdlContextIfCreated()
{
    return gSharedMdlContext.load(std::memory_order_acquire);
}

} // namespace prism::materials
