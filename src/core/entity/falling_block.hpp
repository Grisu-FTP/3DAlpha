#pragma once

// **A block on its way down** -- `ff`, which is EntityFallingSand, and the
// second entity in this project that is not a particle.
//
// It exists because sand and gravel were not falling; they were *teleporting*.
// `core/tick/behaviour.cpp`'s `fallingTick` moved the block to its resting place
// in one tick and said so in a comment: "a placeholder with a known
// replacement". This is the replacement. Knock the support out from under a
// pillar of sand and it now comes down over about half a second instead of
// disappearing from the top and reappearing at the bottom in the same frame.
//
// **The instant path is still here, and it is not a fallback -- it is a1.1.2's
// own.** `dh.a` is a static boolean on BlockSand, `fallInstantly`, set while a
// chunk is being populated: with it true, `tryToFall` ticks the entity to a
// standstill instead of spawning it, so a generated world has no sand falling
// out of the sky when a player walks up to it. This port reaches the same fork
// from the other side -- an unset spawn seam means nobody is watching, which is
// true of world generation and of every headless tool here -- and the *result*
// is identical because the entity's landing test and the loop it replaced find
// the same cell.
//
// **What it carries is a block id and nothing else.** `ff`'s only state is
// `blockID` and `fallTime`; there is no metadata field, so a falling block
// lands at metadata 0. That is the original's and it is why nothing here has a
// metadata column: sand and gravel are the only two blocks that fall, and
// neither uses its metadata.
//
// The tick, transcribed from `ff.e_()`:
//
// ```
// if (blockID == 0) { setDead(); return; }
// prevPos = pos;
// fallTime++;
// motionY -= 0.04;
// moveEntity(motionX, motionY, motionZ);
// motionX *= 0.98; motionY *= 0.98; motionZ *= 0.98;
// int i = floor(posX), j = floor(posY), k = floor(posZ);
// if (world.getBlockId(i, j, k) == blockID) world.setBlockWithNotify(i, j, k, 0);
// if (onGround) {
//     motionX *= 0.7; motionZ *= 0.7; motionY *= -0.5;
//     setDead();
//     if (!(world.canBlockBePlacedAt(blockID, i, j, k, true)
//           && world.setBlockWithNotify(i, j, k, blockID))) {
//         entityDropItem(blockID, 1);
//     }
// } else if (fallTime > 100) {
//     entityDropItem(blockID, 1);
//     setDead();
// }
// ```
//
// Three things in it are easy to miss:
//
//   * **It clears its own source cell from inside the tick**, not at spawn --
//     `if the block under me is still the block I am, remove it`. So the first
//     tick of a falling column is what empties the top of it, and a column of
//     three collapses one cell at a time rather than all at once.
//   * **The landing writes at `floor(posY)`**, and `posY` is the box's centre
//     plus `yOffset`, which for this entity is half its height. A block that
//     came to rest on a floor therefore writes into the cell it is sitting in.
//   * **It gives up after 100 ticks** and drops as an item. Five seconds of
//     falling means it is inside something it cannot leave; nothing in an
//     ordinary world reaches it.

#include "core/block/block_def.hpp"
#include "core/util/aabb.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `setSize(0.98F, 0.98F)`, and `yOffset` is half the height -- so the box is
// centred on the position horizontally and *also* vertically, which is not what
// a player's box does.
inline constexpr double kFallingBlockSize = 0.98000001907348633;   // (double)0.98f
inline constexpr double kFallingBlockHalf = kFallingBlockSize / 2.0;

// `motionY -= 0.04` and `*= 0.98`. Both are the class file's own literals: the
// pull is a float widened and the drag is the double `0.9800000190734863`.
inline constexpr double kFallingBlockPull = 0.03999999910593033;
inline constexpr double kFallingBlockDrag = 0.9800000190734863;

// The three factors applied on the tick it lands. They change nothing visible
// -- the entity dies on the same line -- and are transcribed because leaving
// them out would be a silent difference rather than a stated one.
inline constexpr double kFallingBlockLandDrag = 0.699999988079071;
inline constexpr double kFallingBlockBounce = -0.5;

// `if (fallTime > 100)`, which is five seconds.
inline constexpr int kFallingBlockGiveUp = 100;

struct FallingBlock {
    // The box is the authority and the position is derived from it, for the
    // reason `ItemEntity` gives at length.
    AABB box{};

    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;
    double motionX = 0.0, motionY = 0.0, motionZ = 0.0;

    block::BlockId block = 0;
    int fallTime = 0;

    // `(sky << 4) | block` where it is, resampled every tick as the item
    // entities resample theirs.
    u8 light = 0;
    // `kh.aT` -- the fire counter. See core/entity/fire_entry.hpp.
    i16 fire = 0;

    bool onGround = false;

    bool alive() const { return block != 0; }

    void setPosition(double px, double py, double pz)
    {
        x = prevX = px;
        y = prevY = py;
        z = prevZ = pz;
        box = AABB{px - kFallingBlockHalf, py - kFallingBlockHalf, pz - kFallingBlockHalf,
                   px + kFallingBlockHalf, py + kFallingBlockHalf, pz + kFallingBlockHalf};
    }
};

// **No cap**, as the original has none. A collapsing pile spawns one entity per
// cell per tick and each lives well under a second; the first sixteen are held
// from construction and past that the pool grows until the heap says stop (see
// core/util/segmented_pool.hpp). Only then is a spawn refused and counted, and
// `tick::fallingTick` takes the instant path for that cell: the block still
// ends up where it belongs, it just gets there without the animation.
class FallingBlockSystem {
public:
    // Held from construction, so ordinary play never allocates; not a limit.
    static constexpr int kInitialCapacity = 16;

    // `dh.h(Lcn;III)V`'s `new ff(world, i + 0.5F, j + 0.5F, k + 0.5F, blockID)`.
    // Note the halves are floats in the class file and the constructor widens
    // them, which for these values is exact.
    bool spawn(const tick::TickWorld& world, i32 x, int y, i32 z, block::BlockId id);

    // One 20 Hz tick of `ff.e_()` for every live entity. **Takes a mutable
    // world**, unlike every other entity here: this one clears the cell it came
    // from and writes the cell it lands in, which is the whole of what it is
    // for.
    void tick(tick::TickWorld& world);

    void clear() { items_.clear(); }

    int count() const { return items_.size(); }
    const FallingBlock& operator[](int i) const { return items_[i]; }

    // Spawns the pool has refused, for the debug page.
    u32 refused() const { return refused_; }

private:
    friend struct PersistentEntities;
    FallingBlock* allocate();
    void removeAt(int index);

    SegmentedPool<FallingBlock, kInitialCapacity> items_;
    u32 refused_ = 0;
};

}  // namespace mc::entity
