#pragma once

// The menu's backdrop: a1.1.2's own, which is `dirt.png` tiled and darkened.
//
// Read out of the jar rather than remembered. `GuiScreen.drawBackground` --
// `bh.class` -- binds `/dirt.png`, sets a scale of 32.0f, and draws one quad
// over the whole screen whose texture coordinates are width/32 and height/32
// with a **vertex colour of 0x404040**. The loading screen (`gr.class`) does
// the same thing with the same two constants, which is a second witness for
// both.
//
// Two decisions here are ours:
//
//   * **The darkening is baked into the texels** rather than applied as a tint
//     at draw time. citro2d's image tint interpolates towards a colour instead
//     of multiplying by it, so a faithful 0x404040 multiply is not something
//     the draw call can express -- and the backdrop is the only thing this
//     texture is for, so multiplying once at load costs nothing and is exact.
//   * **The tile is stored at 32x32**, the size it is drawn at. A vanilla
//     16x16 dirt.png is replicated to it, which is pixel for pixel what the
//     GPU would do magnifying it 2x with nearest filtering, and an HD pack's
//     dirt lands at 1:1 instead of being thrown away. It also makes the
//     texture a power of two whatever the pack shipped, which C3D_TexInit
//     requires.
//
// A pack with no dirt.png falls back to the dirt tile of its own terrain.png,
// which is also what serves Dev Art -- generated art has no dirt.png and never
// will. Which tile that is comes from the block table rather than from a
// number written here; see the implementation.

#include "core/io/file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/types.hpp"

#include <string_view>
#include <vector>

namespace mc::texture {

// The tile as stored, and the GUI pixels it covers on screen. The two are equal
// on purpose -- see the note above -- but they are different facts and the menu
// reads the second one.
inline constexpr int kBackgroundEdge = 32;
inline constexpr int kBackgroundTilePixels = 32;
inline constexpr usize kBackgroundBytes = usize(kBackgroundEdge) * kBackgroundEdge * 4;

// a1.1.2's vertex colour for the backdrop quad, as one channel of 0x404040.
inline constexpr u8 kBackgroundShade = 0x40;

// Builds the backdrop tile for a pack, falling back to `atlas`'s dirt tile when
// the pack has no dirt.png. NotFound only when neither is available, which
// means an empty atlas -- a caller that has not built one yet.
PackError buildBackground(io::FileSystem& fs, std::string_view packPath,
                          const AtlasImage& atlas, std::vector<u8>* out);

}  // namespace mc::texture
