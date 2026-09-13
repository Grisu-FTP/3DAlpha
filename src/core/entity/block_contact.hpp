#pragma once

// **What the block an entity is standing in does to it** -- the loop in the
// tail of `kh.c(DDD)V` (Entity.moveEntity) that runs just before the fire test
// core/entity/fire_entry.hpp already ports, and out of the same method:
//
// ```java
// int i = MathHelper.floor_double(boundingBox.minX);
// int j = MathHelper.floor_double(boundingBox.minY);
// int k = MathHelper.floor_double(boundingBox.minZ);
// int l = MathHelper.floor_double(boundingBox.maxX);
// int m = MathHelper.floor_double(boundingBox.maxY);
// int n = MathHelper.floor_double(boundingBox.maxZ);
// for (int x = i; x <= l; ++x)
//     for (int y = j; y <= m; ++y)
//         for (int z = k; z <= n; ++z) {
//             int id = worldObj.getBlockId(x, y, z);
//             if (id > 0) {
//                 Block.blocksList[id].onEntityCollidedWithBlock(worldObj, x, y, z, this);
//             }
//         }
// ```
//
// **The bounds are plain floors of the box and the loops are inclusive**, with
// none of the thousandth-of-a-block inset later versions add -- so an entity
// resting exactly on the top of a cell is still *in* that cell as far as this
// loop is concerned. That is not a detail: an item lying on a cactus sits at
// y = 0.9375, which floors into the cactus's own cell, and it is the whole
// reason a cactus destroys what lands on it.
//
// **One class answers this call with damage in a1.1.2 and it is the cactus**:
// `hy.b(Lcn;IIILkh;)V` is `entity.attackEntityFrom(null, 1)` and nothing else
// -- no metadata test, no cooldown, no sound. See `block::Contact`, which is
// the column this asks, and which also records the pressure plate's override;
// the plate is driven from its own sense pass in core/tick/redstone.cpp and is
// deliberately not dispatched here.
//
// **What a touch costs is the entity's business, not the block's**, exactly as
// with fire: a dropped stack has five health and no invulnerability window and
// so is gone five ticks after it lands on a cactus, an animal's ten-tick window
// turns a point a tick into about a heart a second, a boat or a minecart takes
// ten of its forty points per hit, and an arrow, a falling block or a block of
// primed TNT takes nothing at all because `kh.a(Lkh;I)Z` is `return false` for
// all three. So this returns **how many hits landed** and leaves the caller to
// put each one through its own `attackEntityFrom`.
//
// A box spanning two cactus cells really is hit twice in a tick; the loop makes
// one call per cell and none of them is a no-op.

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/util/aabb.hpp"
#include "core/util/math_helper.hpp"
#include "core/util/types.hpp"

namespace mc::entity {

// `attackEntityFrom(null, 1)` -- the cactus's whole override, and the only
// amount any block deals in this version.
inline constexpr int kContactDamage = 1;

// How many one-point hits the blocks overlapping `box` deal this tick. Zero
// for the overwhelming majority of moves: the column is `Contact::None` for
// every block in a1.1.2 but three, and two of those are pressure plates.
//
// Nothing here touches the world, so a pool can run it inside its own loop and
// apply the hits in whatever order its own removal rules need.
template <class Access>
int blockContactHits(const Access& world, const AABB& box)
{
    const i32 x0 = MathHelper::floorDouble(box.minX);
    const i32 x1 = MathHelper::floorDouble(box.maxX);
    const int y0 = MathHelper::floorDouble(box.minY);
    const int y1 = MathHelper::floorDouble(box.maxY);
    const i32 z0 = MathHelper::floorDouble(box.minZ);
    const i32 z1 = MathHelper::floorDouble(box.maxZ);

    int hits = 0;
    for (i32 bx = x0; bx <= x1; ++bx) {
        for (int by = y0; by <= y1; ++by) {
            for (i32 bz = z0; bz <= z1; ++bz) {
                if (block::def(world.blockAt(bx, by, bz)).contact
                    == block::Contact::Hurt) {
                    ++hits;
                }
            }
        }
    }
    return hits;
}

}  // namespace mc::entity
