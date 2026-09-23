#pragma once

// **`water.png`, the sheet drawn over the view while the head is under water.**
//
// `ItemRenderer.renderOverlays` (`jh.b(F)V`) binds `/water.png` and runs
// `jh.c(F)V` whenever the player `isInsideOfMaterial(water)`: one quad over
// the whole view, the image repeated four times across it, scrolled by the
// player's yaw and pitch. See core/render/water_overlay.hpp for the geometry.
//
// **A file of its own at the pack's root**, not a tile of `terrain.png` --
// a1.1.2's is 16 x 16, blue and about half transparent. A pack's is scaled to
// 16 on a side, as an HD `terrain.png` is scaled to 256, because the quad's
// UVs repeat it a fixed four times whatever its resolution.
//
// **Uploaded already repeated**, 4 x 4 copies on a 64 x 64 sheet. The quad's
// UVs run over four repeats plus a scroll, and the detail vertex's UVs are s16
// in 1/16384 units -- two texture widths either way -- so four repeats of a
// 16 x 16 texture do not fit in them and one repeat of a 64 x 64 one does.
// 16 KB of texture for that is cheaper than a second vertex format.
//
// **Nothing here is required.** A pack with no `water.png`, and Dev Art, get a
// generated stand-in: mottled blue at roughly the original's colour and
// opacity, so a player with no pack still sees that they are under water.

#include "core/io/file_system.hpp"
#include "core/util/types.hpp"

#include <string_view>
#include <vector>

namespace mc::texture {

// The image as the original draws it, and how many times the sheet repeats it.
inline constexpr int kWaterOverlayTileEdge = 16;
inline constexpr int kWaterOverlayRepeats = 4;
inline constexpr int kWaterOverlayEdge = kWaterOverlayTileEdge * kWaterOverlayRepeats;
inline constexpr usize kWaterOverlayBytes =
    usize(kWaterOverlayEdge) * kWaterOverlayEdge * 4;

// The ceiling every optional pack image gets: -fno-exceptions makes a failed
// allocation an abort, so anything that might not fit is refused first.
inline constexpr usize kMaxWaterOverlayPixels = 1024u * 1024u;

// At the root of the pack, beside `terrain.png`; see docs/assets.md.
inline constexpr char kWaterOverlayFile[] = "water.png";

// Builds the 64 x 64 sheet. Always fills `out` with `kWaterOverlayBytes`: a
// pack with no usable `water.png` gets the stand-in. `packPath` empty means
// Dev Art.
void buildWaterOverlay(io::FileSystem& fs, std::string_view packPath, std::vector<u8>* out);

// The stand-in alone, already repeated onto the 64 x 64 sheet.
void buildDevArtWaterOverlay(std::vector<u8>* out);

}  // namespace mc::texture
