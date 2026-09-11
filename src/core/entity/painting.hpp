#pragma once

// **A painting on a wall** -- `jc`, which is `EntityPainting`, and the third
// entity in this project after the dropped item and the falling block.
//
// "Paintings don't work" turned out to mean something precise: item 321 has an
// `onItemUse` in this version and it does not place a block. It builds an
// entity, asks that entity whether the wall it is on will hold it, and spawns
// it. A build with no entity to spawn does nothing at all, silently, which is
// exactly what was reported.
//
// **It is the cheapest entity in the game and that is why it is here first.**
// It has no `onUpdate` worth the name, no motion, no gravity and no collision
// sweep -- it hangs where it was put until something breaks it. Everything
// interesting about it is in two methods, and both are transcribed here:
//
//   * `b(int)` -- setDirection -- which turns a tile coordinate and a facing
//     into a position and a bounding box, and
//   * `i()` -- onValidSurface -- which is the rule for whether a wall will
//     take it.
//
// **The art is picked at random from the ones that fit.** The constructor walks
// every `er` in declaration order, tries each on this wall, keeps the ones that
// pass, and picks one. That is why a 1 x 1 gap gives you one of the seven small
// pictures and a 4 x 4 wall can give you Pigscene -- and why placing on a wall
// too small for anything leaves `art` at the last one tried and the placement
// then fails. The order matters and is the generated table's order.
//
// The pool survives world saves through core/entity/persistence.hpp. Native
// Java chunk entity import/export is still pending.
//
//   * **What breaks one, and what it leaves.** `jc.e_()`'s hundred-tick
//     `onValidSurface` check removes a painting whose wall has gone and
//     **spawns an `EntityItem` holding item 321 as it does** -- that spawn is
//     the method's own, not `Entity.dropItem` and not anything Survival gates,
//     so it is here. `jc.a(Lkh;I)Z` -- attackEntityFrom -- is the same two
//     lines with no damage accumulated at all: **one hit takes a painting
//     down**, whatever it was hit with.
//
// Drawing is elsewhere, as it is for every entity here: core/render/painting_mesh.hpp
// turns one of these into quads and this file has never heard of a camera.

#include "core/entity/painting_art.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

#include "paintings.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// The art table, as the generator emitted it. Exposed so the renderer can look
// one up without including the generated header itself.
inline constexpr int kPaintingArtCount = mcver::kPaintingCount;
const PaintingArt& paintingArt(int index);

// **Which way a painting faces.** a1.1.2 stores this as an int 0..3 and the
// `Dir` tag in a save file is that int, so it is not an enumeration of ours to
// reorder. The mapping is `b(int)`'s own:
//
//   0 -- hangs on a block's -Z side and faces -Z
//   1 -- -X
//   2 -- +Z
//   3 -- +X
//
// `ItemPainting.onItemUse` maps the struck face to it: face 2 gives 0, face 4
// gives 1, face 3 gives 2, face 5 gives 3, and the two horizontal faces refuse.
inline constexpr int kPaintingDirections = 4;

// `0.5625F` -- nine sixteenths, which is half a block plus one sixteenth. It is
// what stands a painting *off* the wall rather than inside it, and the extra
// sixteenth is the canvas's own thickness.
inline constexpr double kPaintingStandoff = 0.5625;

// The half-thickness of the canvas, `0.5F / 32.0F`. A painting's box is a
// thirty-second of a block deep.
inline constexpr double kPaintingHalfDepth = 0.5 / 32.0;

// `-0.00625F`, subtracted from every face of the bounding box -- so the box is
// very slightly *smaller* than the canvas. Without it a painting flush against
// a wall would collide with the block behind it and refuse its own placement.
inline constexpr double kPaintingBoxInset = -0.00625;

// `onUpdate`'s counter: the wall is re-checked every hundred ticks, not every
// tick. Five seconds is the original's own answer to "how long may a painting
// hang in the air after its wall is mined".
inline constexpr int kPaintingCheckInterval = 100;

struct Painting {
    // The block the painting was hung on, which is **not** where it is: the
    // position is derived from these three and the direction every time the
    // direction is set. Kept because `onValidSurface` works from them.
    i32 tileX = 0, tileY = 0, tileZ = 0;
    int direction = 0;
    int art = 0;

    // The centre of the canvas, in blocks.
    double x = 0.0, y = 0.0, z = 0.0;
    AABB box{};

    // `EntityPainting.onUpdate`'s countdown to the next wall check.
    int checkIn = 0;

    // `(sky << 4) | block` where it hangs, resampled as the renderer's light is.
    u8 light = 0;

    // **One light byte per 16 x 16 cell**, because the original lights a
    // painting cell by cell rather than as a whole: `RenderPainting` calls
    // `setLightmap` at the centre of every cell before it emits that cell's
    // quads. On a four-block canvas half in a doorway that is the difference
    // between a picture and a flat rectangle.
    //
    // Sixteen is the ceiling and not a guess: the largest art in a1.1.2 is
    // 64 x 64 texels, which is four blocks by four.
    static constexpr int kMaxCells = 4 * 4;
    u8 cellLight[kMaxCells] = {};

    bool alive = false;

    const PaintingArt& artwork() const { return paintingArt(art); }
};

// `b(int)` -- **setDirection**. Fills in `direction`, `x/y/z` and `box` from the
// tile, the direction and the art already chosen.
//
// Split out of the struct because the renderer and the tests both want to ask
// "where would a painting of this art, on this tile, facing this way, be?"
// without building one.
void setPaintingDirection(Painting* painting, int direction);

// `i()` -- **onValidSurface**. True when every block the canvas covers is solid
// and nothing already occupies the space.
//
// `existing` is the pool, so the "is there already a painting here" half of the
// original's entity scan has something to scan; pass a null pool and that half
// is skipped, which is what the placement probe wants.
class PaintingSystem;
bool paintingFits(const tick::TickWorld& world, const Painting& painting,
                  const PaintingSystem* existing);

// **No cap**, as the original has none -- a wall of paintings is a thing
// players build. The first thirty-two are held from construction; past that
// the pool grows until the heap says stop (core/util/segmented_pool.hpp). What
// is *drawn* has its own budget, nearest first -- see core/render/painting_mesh.hpp.
class PaintingSystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 32;


    explicit PaintingSystem(i64 seed) : rand_(seed) {}

    // `ItemPainting.onItemUse` end to end: build one on the struck face, let
    // the constructor choose from the art that fits, and spawn it if the wall
    // will hold it.
    //
    // Returns true when a painting was placed -- which is what tells the caller
    // to take one off the stack. **The original returns true either way**; that
    // is `onItemUse`'s "I handled this click" and not "I placed something", and
    // conflating the two is what would silently eat an item on a bad wall.
    bool place(const tick::TickWorld& world, i32 tileX, i32 tileY, i32 tileZ, int face);

    // One 20 Hz tick: the hundred-tick wall check, and nothing else. A painting
    // whose wall has gone is removed and leaves a painting item where it hung.
    void tick(const tick::TickWorld& world);

    // `jc.a(Lkh;I)Z` -- **attackEntityFrom**, which for a painting ignores the
    // damage entirely: it dies and drops its item. Returns false only for an
    // index that is not a live painting.
    bool attack(const tick::TickWorld& world, int index);

    void clear() { paintings_.clear(); }

    int count() const { return paintings_.size(); }
    const Painting& operator[](int i) const { return paintings_[i]; }

    // Spawns the pool has refused, on the debug page for the same reason the
    // particle pool's is.
    u32 refused() const { return refused_; }

private:
    friend struct PersistentEntities;

    // Spawn the item and take the painting out of the pool, which `tick` and
    // `attack` both do and which must not advance a caller's loop index: the
    // last painting is swapped into the hole.
    void dropAndRemove(const tick::TickWorld& world, int index);

    SegmentedPool<Painting, kInitialCapacity> paintings_;
    u32 refused_ = 0;
    JavaRandom rand_;
};

}  // namespace mc::entity
