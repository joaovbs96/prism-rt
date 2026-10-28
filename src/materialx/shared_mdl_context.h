#pragma once

// The process-lifetime MDL runtime (docs/sdk_design.md, section 2.1 row 1):
// one mdl::Context (INeuray) per process, shared by every renderer and both
// front ends. Renderer objects may be created and destroyed repeatedly while
// compiled materials still reference SDK state, so it outlives them.

namespace mdl { class Context; }

namespace prism::materials {

mdl::Context& SharedMdlContext();
// The shared context if it was created, else null (without creating it).
mdl::Context* SharedMdlContextIfCreated();

} // namespace prism::materials
