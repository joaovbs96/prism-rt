// Process state of the USD module's header-only code (src/internal_export.h):
// the material service's registries, one per process in prism_usd, shared by
// the CLI's loader, hdPrism and the tests.

#include "../materialx/captured_material.h"
#include "../materialx/prepared_template.h"

namespace prism::materialx {

CompatibleTemplateRegistry& CompatibleTemplateRegistry::Shared()
{
    static auto* value = new CompatibleTemplateRegistry;
    return *value;
}

CapturedMaterialRegistry& CapturedMaterialRegistry::Shared()
{
    static auto* registry = new CapturedMaterialRegistry;
    return *registry;
}

} // namespace prism::materialx
