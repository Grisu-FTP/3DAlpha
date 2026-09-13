// `ff` -- EntityFallingSand. See falling_block.hpp for what is transcribed and
// what the instant path is.

#include "core/entity/falling_block.hpp"

#include "core/block/registry.hpp"
#include "core/entity/fire_entry.hpp"
#include "core/entity/sweep.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

namespace mc::entity {
namespace {

using block::BlockId;
using block::TickBehaviour;

// The same `(sky << 4) | block` byte every mesh vertex carries.
u8 packedLightAt(const tick::TickWorld& world, double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);
    return u8((world.skyLightAt(bx, by, bz) << 4) | world.blockLightAt(bx, by, bz));
}

// `cn.a(IIIIZ)Z` -- World.canBlockBePlacedAt, **with `flag` true**, which is
// the one call site that passes it: a falling block asks whether it may become
// a block again, and it is allowed to land inside the player. The flag nulls
// the collision box before the `checkIfAABBIsClear` test, so the whole entity
// half of the method is skipped and what is left is the replaceable list and
// the block's own rule.
//
// `core/item/use.cpp` has the `flag == false` version of the same method. They
// are not shared because sharing them would mean a parameter whose only reader
// is a branch that the two callers never agree on -- and the halves that differ
// are exactly the halves that are easy to get wrong.
bool canLandAt(const tick::TickWorld& world, BlockId id, i32 x, int y, i32 z)
{
    if (y < 0 || y >= mcver::kWorldHeight) {
        return false;
    }
    switch (block::def(world.blockAt(x, y, z)).tick) {
    case TickBehaviour::FluidFlowing:
    case TickBehaviour::FluidStill:
    case TickBehaviour::Fire:
    case TickBehaviour::SnowLayer:
        return true;
    default:
        break;
    }
    return world.blockAt(x, y, z) == block::kAir && tick::canPlaceAt(world, id, x, y, z);
}

}  // namespace

FallingBlock* FallingBlockSystem::allocate()
{
    FallingBlock* e = items_.push();
    if (e == nullptr) {
        ++refused_;
    }
    return e;
}

void FallingBlockSystem::removeAt(int index)
{
    items_.swapRemove(index);
}

bool FallingBlockSystem::spawn(const tick::TickWorld& world, i32 x, int y, i32 z, BlockId id)
{
    if (id == block::kAir) {
        return false;
    }
    FallingBlock* e = allocate();
    if (e == nullptr) {
        return false;
    }

    e->block = id;
    e->fallTime = 0;
    e->onGround = false;
    // `i + 0.5F`, and the constructor sets every motion component to zero --
    // there is no hop and no scatter. A falling block starts still.
    e->setPosition(double(x) + 0.5, double(y) + 0.5, double(z) + 0.5);
    e->light = packedLightAt(world, e->x, e->y, e->z);
    return true;
}

void FallingBlockSystem::tick(tick::TickWorld& world)
{
    for (int i = 0; i < items_.size();) {
        FallingBlock& e = items_[i];
        // Restored entities can precede the chunks beneath them.
        if (!world.chunkResident(MathHelper::floorDouble(e.x) >> 4,
                                 MathHelper::floorDouble(e.z) >> 4)) {
            ++i;
            continue;
        }

        e.prevX = e.x;
        e.prevY = e.y;
        e.prevZ = e.z;
        ++e.fallTime;
        e.motionY -= kFallingBlockPull;

        // `moveEntity`, through the sweep the player, the particles and the
        // dropped items all share. No step up: this entity's `stepHeight` is
        // the base class's zero.
        AABB box = e.box;
        double dx = e.motionX;
        double dy = e.motionY;
        double dz = e.motionZ;
        const double wantY = dy;

        const BlockRange range = sweepRange(box.extend(dx, dy, dz));
        dy = clipAxis(world, range, box, kAxisY, dy);
        box = box.offset(0.0, dy, 0.0);
        dx = clipAxis(world, range, box, kAxisX, dx);
        box = box.offset(dx, 0.0, 0.0);
        dz = clipAxis(world, range, box, kAxisZ, dz);
        box = box.offset(0.0, 0.0, dz);

        e.box = box;
        e.x = (box.minX + box.maxX) / 2.0;
        e.y = (box.minY + box.maxY) / 2.0;
        e.z = (box.minZ + box.maxZ) / 2.0;
        e.onGround = wantY != dy && wantY < 0.0;

        if (e.motionX != dx) e.motionX = 0.0;
        if (e.motionY != dy) e.motionY = 0.0;
        if (e.motionZ != dz) e.motionZ = 0.0;


        // **`moveEntity`'s tail**, which for a falling block is the counter and the
        // hiss and nothing else: `kh.a(Lkh;I)Z` is `return false` and `ff`
        // does not override it, so fire chars one in flight without harming it.
        // See core/entity/fire_entry.hpp.
        {
            const FireEntryResult burn = updateFireEntry(
                &e.fire, boundingBoxBurning(world, e.box), fireWetProbe(world, e.box));
            if (burn.fizz) {
                // **The pitch comes off the world's generator**, because this
                // pool has none of its own and `ff` draws nothing else. The
                // jar's own draw is off `kh.aQ`, which is an *unseeded*
                // `new Random()` and reproduces nothing in either build -- so
                // one stream is as faithful as another here. See
                // core/tick/fluid.cpp, which says the same about `jp.i`.
                world.playSoundAt(kFizzSound, e.x, e.y, e.z, 0.7f,
                                  fizzPitch(world.random()));
            }
        }

        e.motionX *= kFallingBlockDrag;
        e.motionY *= kFallingBlockDrag;
        e.motionZ *= kFallingBlockDrag;

        const i32 bx = MathHelper::floorDouble(e.x);
        const int by = int(MathHelper::floorDouble(e.y));
        const i32 bz = MathHelper::floorDouble(e.z);

        // **The source cell is cleared from inside the tick.** The block is
        // still in the world when the entity spawns, and this is what takes it
        // out -- on the first tick, once the entity has moved far enough that
        // `floor(posY)` still names the cell it came from. A column therefore
        // empties from the top a cell at a time.
        e.light = packedLightAt(world, e.x, e.y, e.z);
        if (world.blockAt(bx, by, bz) == e.block) {
            world.setBlockWithNotify(bx, by, bz, block::kAir);
        }

        if (e.onGround) {
            e.motionX *= kFallingBlockLandDrag;
            e.motionZ *= kFallingBlockLandDrag;
            e.motionY *= kFallingBlockBounce;

            // **The write can fail, and then the block becomes an item.** A
            // torch, a plant or another entity in the way is enough; the
            // original drops rather than overwriting, which is what stops sand
            // falling into a doorway from eating the door.
            if (!canLandAt(world, e.block, bx, by, bz)
                || !world.setBlockWithNotify(bx, by, bz, e.block)) {
                tick::dropBlockAsItem(world, bx, by, bz, e.block, 0);
            }
            removeAt(i);
            continue;
        }

        if (e.fallTime > kFallingBlockGiveUp) {
            tick::dropBlockAsItem(world, bx, by, bz, e.block, 0);
            removeAt(i);
            continue;
        }

        ++i;
    }
    items_.trim();
}

}  // namespace mc::entity
