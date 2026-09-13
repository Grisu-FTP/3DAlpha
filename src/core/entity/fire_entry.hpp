#pragma once

// **What sets an entity alight, and it is not the fire block** -- the tail of
// `kh.c(DDD)V` (Entity.moveEntity), which every moving entity in a1.1.2 runs
// and which this port had for the hiss alone.
//
// ```java
// ySize *= 0.4F;
// boolean flag = g_();                            // handleWaterMovement
// if (worldObj.isBoundingBoxBurning(boundingBox)) {
//     dealFireDamage(1);                          // attackEntityFrom(null, 1)
//     if (!flag) {
//         fire++;
//         if (fire == 0) {
//             fire = 300;
//         }
//     }
// } else if (fire <= 0) {
//     fire = -fireResistance;
// }
// if (flag && fire > 0) {
//     playSoundAtEntity(this, "random.fizz", 0.7F, 1.6F + (rand - rand) * 0.4F);
//     fire = -fireResistance;
// }
// ```
//
// **This corrects a conclusion that was written down twice.** `core/entity/
// item_entity.cpp` and `core/entity/mob.cpp` both said that nothing in a1.1.2
// sets an entity's fire counter except the lava branch of `kh.y()`, on the
// evidence that `og` -- BlockFire -- has no `onEntityCollidedWithBlock` and no
// class outside `kh` writes `kh.aT`. Both halves of that are true and the
// conclusion is wrong: **`kh` writes its own counter, in `moveEntity`, four
// times**, and the ignition is a *box test against the world* rather than a
// callback from the block. Standing in fire really does burn, and the port
// really did leave it out -- a pig could sleep in a bonfire.
//
// **The negative counter is a fuse, not a flag.** `fire` rests at
// `-fireResistance`, and a tick in fire increments it; catching fire is the
// tick it reaches **zero**, which is then set to 300. `fireResistance` is 1 for
// every entity in the game except `dm` -- EntityPlayer -- which sets 20 in its
// constructor. So an animal ignites on its first tick in a flame and a player
// gets a whole second of grace, which is why you can run through a fire and a
// cow cannot. It is also why `level.dat` reads `Fire: -20` for a player who has
// never burned.
//
// **`dealFireDamage(1)` is every tick, and the counter's own damage is on top
// of it.** `kh.a(I)V` is `attackEntityFrom(null, 1)`, so what the tick costs
// depends on what the entity does with a hit: an `EntityLiving` has a ten-tick
// invulnerability window and takes about one a second, a dropped item has five
// health and no window and is gone in five ticks, a boat and a minecart take
// ten points of their forty-point damage counter, and an arrow, a falling block
// and a block of primed TNT take nothing at all -- `kh.a(Lkh;I)Z` is `return
// false` and those three do not override it. So fire *burns up* an item and
// *chars* everything else.
//
// **What counts as burning is three block ids**, not a material and not a
// behaviour: `cn.c(Lcf;)Z` -- isBoundingBoxBurning -- walks the cells the box
// touches and tests each against `Block.fire`, `Block.lavaStill` and
// `Block.lavaMoving`. Asked here as "the block that ticks as fire, or anything
// whose material is lava", which is the same set without naming an id: lava's
// material belongs to those two blocks and nothing else in the version.
//
// **The box is the entity's own, uninset**, and the scan runs to
// `floor(max + 1)` on each axis -- so an entity whose box merely *touches* the
// plane of a fire cell is burning. That is a whole block of generosity compared
// with the water probe next door, and it is why fire catches things that look
// like they are standing beside it.
//
// **The wet test here does not take `g_()`'s push**, and that is a deviation
// this file inherits rather than introduces: `core/entity/mob.cpp` made the
// call when it wrote the hiss half, on the grounds that `PlayerBody::move` runs
// no tail for the player, so taking the current a fourth time for animals alone
// would drift a cow downstream faster than the player swimming beside it. The
// probe is `g_()`'s inset box asked without the mutation. Everything else in
// the tail is the jar's.

#include "core/block/block_def.hpp"
#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/math_helper.hpp"
#include "core/util/types.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

namespace mc::entity {

// `kh.aS` -- **fireResistance**, which is the depth of the fuse and not a
// resistance to damage. One for everything the jar constructs except the
// player.
inline constexpr int kEntityFireResistance = 1;
inline constexpr int kPlayerFireResistance = 20;

// `fire = 300` -- fifteen seconds, and the same number `mb.j()` gives a zombie
// that catches the dawn.
inline constexpr int kCaughtFireTicks = 300;

// `random.fizz` at 0.7, and the pitch is the caller's draw. See
// docs/audio-a1.1.2.md, where this row was written down before the branch that
// plays it existed for anything but an animal.
inline constexpr const char* kFizzSound = "random.fizz";

inline float fizzPitch(JavaRandom& rand)
{
    return 1.6f + (rand.nextFloat() - rand.nextFloat()) * 0.4f;
}

// Lava's material, which is what stands in for the two lava ids.
inline constexpr u8 kFireLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;

// `cn.c(Lcf;)Z` -- **World.isBoundingBoxBurning**. Every cell the box touches,
// with the same `floor(max + 1)` bound `isMaterialInBox` uses and none of its
// surface arithmetic: a fire block or a lava block anywhere in the volume is
// enough.
template <class Access>
bool boundingBoxBurning(const Access& world, const AABB& box)
{
    const i32 x0 = MathHelper::floorDouble(box.minX);
    const i32 x1 = MathHelper::floorDouble(box.maxX + 1.0);
    const int y0 = MathHelper::floorDouble(box.minY);
    const int y1 = MathHelper::floorDouble(box.maxY + 1.0);
    const i32 z0 = MathHelper::floorDouble(box.minZ);
    const i32 z1 = MathHelper::floorDouble(box.maxZ + 1.0);

    for (i32 bx = x0; bx < x1; ++bx) {
        for (int by = y0; by < y1; ++by) {
            for (i32 bz = z0; bz < z1; ++bz) {
                const block::BlockDef& d = block::def(world.blockAt(bx, by, bz));
                if (d.tick == block::TickBehaviour::Fire || d.material == kFireLavaMaterial) {
                    return true;
                }
            }
        }
    }
    return false;
}

// The water half of the tail, asked without `g_()`'s push -- see the note at
// the top of this file.
template <class Access>
bool fireWetProbe(const Access& world, const AABB& box)
{
    constexpr u8 kWater = mcver::kBlocks[int(mcver::Block::Water)].material;
    return block::isMaterialInBox(world, box.expand(0.0, block::kWaterProbeInset, 0.0),
                                  kWater);
}

struct FireEntryResult {
    // `dealFireDamage(1)` fired this tick: one point, through whatever the
    // caller's `attackEntityFrom` is. **The caller may be dead afterwards.**
    bool damage = false;
    // A burning entity went under: the caller plays `kFizzSound` at 0.7 and
    // `fizzPitch`, wherever it puts its sounds.
    bool fizz = false;
};

// The tail itself, over one entity's counter. `burning` is
// `boundingBoxBurning` and `wet` is `fireWetProbe`, both asked of the box the
// move left behind; `fireResistance` is `kEntityFireResistance` for everything
// but a player.
//
// Nothing here touches the world, so a pool can run it inside its own loop and
// act on the two flags in whatever order its own removal rules need.
inline FireEntryResult updateFireEntry(i16* fire, bool burning, bool wet,
                                       int fireResistance = kEntityFireResistance)
{
    FireEntryResult out;
    if (fire == nullptr) {
        return out;
    }
    if (burning) {
        out.damage = true;
        if (!wet) {
            ++*fire;
            if (*fire == 0) {
                *fire = i16(kCaughtFireTicks);
            }
        }
    } else if (*fire <= 0) {
        *fire = i16(-fireResistance);
    }
    if (wet && *fire > 0) {
        out.fizz = true;
        *fire = i16(-fireResistance);
    }
    return out;
}

}  // namespace mc::entity
