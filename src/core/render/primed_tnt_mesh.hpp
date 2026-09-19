#pragma once

// **Primed TNT as geometry** -- `hw.a(Ljd;DDDFF)V`, RenderTNTPrimed, which is
// two draws of one cube and a `glScalef` between them.
//
// It sits beside core/render/falling_block_mesh.hpp and follows the same rules:
// the caller owns the storage, nothing here allocates, the geometry comes out
// relative to an origin the caller picks, and the vertex written is
// `mesh::DetailVertex`.
//
// **The swell.** `hw` scales the cube by `1 + f * 0.3` where
// `f = clamp(1 - (fuse - partialTick + 1) / 10, 0, 1)` raised to the **fourth**
// power -- squared twice, at offsets 67-79 of the class file. The fourth power
// is the whole character of it: a fuse of 80 sits at its own size for nearly
// four seconds and then swells in the last half. Outside those ten ticks the
// scale is skipped entirely rather than set to 1, which is the same number.
//
// **The flash is a second pass and not a colour.** `hw` draws the textured
// cube, then -- on every other group of five ticks, `fuse / 5 % 2 == 0` --
// draws it again with the texture and lighting off, blended white at
// `(1 - (fuse - partialTick + 1) / 100) * 0.8`. So the flash starts at about
// two tenths and reaches eight tenths on the tick it goes off: TNT does not
// blink evenly, it blinks *harder*.
//
// That second pass cannot be vertex colour here. The detail pipeline modulates
// the atlas by the vertex colour, and modulation can only darken -- there is no
// product of the TNT texture and a vertex colour that is white. So the flash is
// built separately, as position-only `render::OutlineVertex` triangles for the
// pipeline that already draws the selection box: texture-free, blended, and
// tinted from a uniform. `buildPrimedTntFlash` writes those, and because the
// tint is a uniform rather than a vertex attribute, **one entity is one draw**
// -- which is why it is capped and the cap is `kPrimedTntFlashBudget`.
//
// The one thing the original does that this does not is draw the block through
// `RenderBlocks.renderBlockAsItem`, so a primed block of some shape other than
// a cube would come out that shape. TNT is the only thing a1.1.2 primes and it
// is a cube.

#include "core/entity/primed_tnt.hpp"
#include "core/mesh/vertex.hpp"
#include "core/render/outline.hpp"
#include "core/util/types.hpp"

namespace mc::render {

// Six faces of four vertices.
inline constexpr int kPrimedTntVerticesEach = 6 * 4;

// **What one frame draws**, which is not what exists: the pool has no cap, and
// past this many in range the nearest are drawn -- see draw_budget.hpp. A wall
// of TNT lights every block in it at once, so this is larger than the falling
// blocks' budget.
inline constexpr int kPrimedTntDrawBudget = 96;
inline constexpr int kPrimedTntMaxVertices =
    kPrimedTntDrawBudget * kPrimedTntVerticesEach;

// Two triangles a face, because the flash pass has no index buffer behind it.
inline constexpr int kPrimedTntFlashVerticesEach = 6 * 6;

// **One draw call each**, so this is a budget on draw calls and not on memory.
// Sixteen is already more than a chain reaction shows at once: the flash is on
// for five ticks in ten, and a ripple lights its blocks a tick apart.
inline constexpr int kPrimedTntFlashBudget = 16;
inline constexpr int kPrimedTntFlashMaxVertices =
    kPrimedTntFlashBudget * kPrimedTntFlashVerticesEach;

// `1 + f * 0.3`, and the ten ticks it is measured over.
inline constexpr float kPrimedTntSwell = 0.3f;
inline constexpr float kPrimedTntSwellTicks = 10.0f;

// `(1 - (fuse - partial + 1) / 100) * 0.8`, and the five-tick blink.
inline constexpr float kPrimedTntFlashFade = 100.0f;
inline constexpr float kPrimedTntFlashPeak = 0.8f;
inline constexpr int kPrimedTntFlashPeriod = 5;

// `hw`'s scale for one entity: `1 + clamp(1 - (fuse - partial + 1) / 10, 0, 1)^4
// * 0.3`. Exposed because the flash pass has to agree with the textured one
// down to the last bit, and because it is the piece worth testing.
float primedTntScale(int fuse, float partial);

// `1 + (fuse - partial + 1) / 100` subtracted from one, times 0.8, and clamped
// -- the alpha of the white pass. Negative fuses (the tick it goes off) clamp
// at the peak rather than running past it.
float primedTntFlashAlpha(int fuse, float partial);

// True when `hw` draws its second pass at all: `fuse / 5 % 2 == 0`. **Java's
// division truncates towards zero**, so a fuse of -1 -- which this port never
// draws, the entity being gone -- would land in the same bucket as 0.
inline bool primedTntFlashing(int fuse) { return (fuse / kPrimedTntFlashPeriod) % 2 == 0; }

// Fills `out` with the quads for every live entity -- the nearest, when `max`
// cannot hold them all -- and returns how many vertices it wrote. `partial` is
// the fraction of a tick elapsed.
//
// Written in `kEntityUnitsPerBlock` and drawn out to the 63 blocks `kh.a(D)Z`
// gives a 0.98 box -- see core/render/entity_range.hpp. Past that it is
// **skipped rather than clamped**, exactly as an item and a particle are.
int buildPrimedTnt(const entity::PrimedTntSystem& system, double originX, double originY,
                   double originZ, float partial, mesh::DetailVertex* out, int max);

// One entity's white overlay, as triangles for the outline pipeline. Returns
// the vertices written, which is `kPrimedTntFlashVerticesEach` or nothing.
// **The caller draws these one entity at a time** and sets the tint from
// `primedTntFlashAlpha` between draws; see the header.
int buildPrimedTntFlash(const entity::PrimedTnt& entity, double originX, double originY,
                        double originZ, float partial, OutlineVertex* out, int max);

}  // namespace mc::render
