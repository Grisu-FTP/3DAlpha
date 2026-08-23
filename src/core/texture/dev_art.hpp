#pragma once

// The generated placeholder atlas -- "Dev Art", which is now one selectable
// texture pack among whatever else is on the card rather than the only thing
// there is.
//
// **These colours are ours, not Mojang's.** No asset is extracted, decoded or
// shipped here. The atlas is generated so the game is playable with nothing on
// the card and -- more useful during development -- so a wrong texture index is
// *visible*: every tile gets a stable distinct colour rather than a plausible
// one.
//
// It lives in core rather than in the ctr layer for two reasons. It is what
// makes Dev Art a pack rather than an `#if` in the renderer, and it puts two
// derivations that were previously untested under the host suite: the fluid
// tile groups and the torch carve, both of which are read out of the block
// table and the mesher's own geometry rather than written down.

#include "core/texture/atlas_image.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::texture {

// Fills `rgba` with kAtlasEdge^2 pixels, R,G,B,A in memory order, top row
// first -- the same convention core/texture/png.hpp uses, so the two sources of
// an atlas are interchangeable. Resizes the vector.
void buildDevArt(std::vector<u8>* rgba);

}  // namespace mc::texture
