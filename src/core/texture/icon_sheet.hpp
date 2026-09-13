#pragma once

// `gui/icons.png` -- **the sheet the hearts come off**, and what `lu`
// (GuiIngame) draws from it.
//
// a1.1.2's in-game overlay binds this sheet once and cuts 9 x 9 cells out of it
// by absolute texel: the heart's empty container and a flashing one, full and
// half hearts and their flashing outlines, the armour row's three states, and
// the air bubble whole and popping. The cells below are the literals `lu.a(FZII)V`
// passes to `drawTexturedModalRect`, so they are the jar's answer to where each
// thing is and nothing here is a layout of our own.
//
// **A pack's sheet is scaled to 256 x 256**, the size a1.1.2's own is, so the
// cells stay valid for a high-resolution pack in the same way the terrain
// atlas's tile indices do. **A pack without one, and Dev Art, get a generated
// stand-in** painted into the same cells -- the player supplies textures, and a
// console with none on the card still has to show health.

#include "core/util/types.hpp"

#include <vector>

namespace mc::texture {

inline constexpr int kIconSheetEdge = 256;
inline constexpr usize kIconSheetBytes = usize(kIconSheetEdge) * usize(kIconSheetEdge) * 4;

// Every cell `lu` cuts is 9 x 9.
inline constexpr int kIconCellSize = 9;

// The top-left texel of one cell.
struct IconCell {
    u8 u;
    u8 v;
};

// Hearts. The container is drawn first under every heart; `Flash` is the one
// used while the hurt window blinks.
inline constexpr IconCell kIconHeartContainer{16, 0};
inline constexpr IconCell kIconHeartContainerFlash{25, 0};
inline constexpr IconCell kIconHeartFull{52, 0};
inline constexpr IconCell kIconHeartHalf{61, 0};
// The health the last hit took away, outlined while the window blinks.
inline constexpr IconCell kIconHeartFlashFull{70, 0};
inline constexpr IconCell kIconHeartFlashHalf{79, 0};

// The armour row.
inline constexpr IconCell kIconArmourEmpty{16, 9};
inline constexpr IconCell kIconArmourHalf{25, 9};
inline constexpr IconCell kIconArmourFull{34, 9};

// The air row.
inline constexpr IconCell kIconBubble{16, 18};
inline constexpr IconCell kIconBubblePopping{25, 18};

// Fills `rgba` with a 256 x 256 RGBA stand-in -- transparent everywhere but
// the cells above, which are painted as recognisable hearts, chestplates and
// bubbles. Resizes the vector. Ours, not Mojang's: see core/texture/dev_art.hpp.
void buildIconStandIn(std::vector<u8>* rgba);

}  // namespace mc::texture
