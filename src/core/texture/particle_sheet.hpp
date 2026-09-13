#pragma once

// **`particles.png`, the third sheet a particle can come off.**
//
// Two of a1.1.2's twelve particle kinds are cut out of images this project
// already uploads -- a block's tile off `terrain.png`, an item's icon off
// `gui/items.png` -- and the other ten are sprites on a sheet of their own that
// nothing else in the game reads. See core/entity/particle.hpp for the split.
//
// **A texture of its own rather than a page of the entity sheet.** The entity
// sheet is 256 x 256 with fifteen 64 x 32 pages spare, and a 128 x 128 image
// does not fit in what is left of it without growing the sheet to 512 tall --
// 256 KB of texture memory to save a bind. This is 64 KB and one bind, which is
// the same trade `initFont` already made for a sheet of exactly the same size,
// and the particle pass has to change texture between its three spans anyway.
//
// **The grid is 16 x 16 and that is `EntityFX.renderParticle`'s, not ours.**
// The render divides the tile index by 16 twice and steps `0.0624375F`, so the
// sheet is a sixteenth on a side per tile whatever its resolution -- 8 px each
// at the original's 128. An HD pack is scaled down to 128 for the same reason
// an HD terrain.png is scaled to 256: the tile arithmetic is compiled in.
//
// **Nothing here is required.** A pack with no `particles.png` gets the
// generated stand-in below, the same way a pack with no `gui/items.png` gets
// its icons from terrain tiles, and Dev Art gets the stand-in always.

#include "core/io/file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/types.hpp"

#include <string_view>
#include <vector>

namespace mc::texture {

// 16 tiles of 8 px, which is the original's sheet and the size every pack's is
// scaled to.
inline constexpr int kParticleTilesPerEdge = 16;
inline constexpr int kParticleTilePixels = 8;
inline constexpr int kParticleSheetEdge = kParticleTilesPerEdge * kParticleTilePixels;
inline constexpr usize kParticleSheetBytes =
    usize(kParticleSheetEdge) * kParticleSheetEdge * 4;

// The same ceiling the font gets, and for the same reason: -fno-exceptions
// makes a failed allocation an abort, so anything that might not fit has to be
// refused before it is asked for.
inline constexpr usize kMaxParticlePixels = 1024u * 1024u;

// The file, at the **root** of the pack. a1.1.2 keeps it beside `terrain.png`
// and `char.png` rather than under `misc/`, which is where later versions moved
// it; see docs/assets.md.
inline constexpr char kParticleFile[] = "particles.png";

// Builds the sheet. Always fills `out` with `kParticleSheetBytes`: a pack that
// has no usable `particles.png` gets the stand-in rather than a hole, so a
// caller has nothing to check and a console with a broken pack still draws
// smoke.
//
// `packPath` empty means Dev Art, which has no art of its own here either.
void buildParticleSheet(io::FileSystem& fs, std::string_view packPath,
                        std::vector<u8>* out);

// **The stand-in, and it is deliberately plain.** Every sprite a1.1.2 keeps on
// this sheet is white or grey -- the colour comes from `particleRed/Green/Blue`
// at the quad, not from the image -- so a stand-in that is a white disc per
// tile produces smoke that is grey, dust that is red and a flame that is
// orange, all through the same tints the real sheet goes through. What it does
// not produce is a *shape*, and that is the honest part: a placeholder fire
// should read as a dot, not as a worse fire.
//
// Only the tiles a1.1.2 names are drawn. The rest are left transparent, so a
// particle pointed at a tile nothing should reach shows as nothing rather than
// as a square somebody chose.
void buildDevArtParticles(std::vector<u8>* out);

}  // namespace mc::texture
