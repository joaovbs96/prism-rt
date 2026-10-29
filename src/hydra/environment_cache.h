#pragma once

// The delegate's dome environments come from the revision layer it shares
// with the public Scene (src/scene/environment_cache.h).

#include "../scene/environment_cache.h"

namespace prism::hydra {

using revision::CachedTransformedEnvironment;
using revision::EnvironmentCache;
using revision::LocalEnvironment;

} // namespace prism::hydra
