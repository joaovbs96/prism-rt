#pragma once

// The refinement tile schedule moved to the core session (the progressive
// pass loop, src/session/progressive_engine.h).
#include "../session/refinement_tiles.h"

namespace prism::hydra {
using prism::session::MakeRefinementTiles;
using prism::session::RefinementTile;
using prism::session::RefinementTileSize;
} // namespace prism::hydra
