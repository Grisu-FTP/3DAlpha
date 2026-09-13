#include "core/tick/fire.hpp"

#include "core/block/registry.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {

namespace {

using block::BlockId;

// `og.b(Lnm;III)Z` -- canBlockCatchFire: a fire block asks this of its
// neighbours, and it reads `chanceToEncourageFire`, not the material.
bool canCatchFire(const TickWorld& world, i32 x, int y, i32 z)
{
    return block::def(world.blockAt(x, y, z)).burnEncourage > 0;
}

// `og.h(Lcn;III)Z` -- canNeighborBurn. Six neighbours, in the original's own
// order, though nothing observable depends on it here.
bool neighbourCanBurn(const TickWorld& world, i32 x, int y, i32 z)
{
    return canCatchFire(world, x + 1, y, z) || canCatchFire(world, x - 1, y, z) ||
           canCatchFire(world, x, y - 1, z) || canCatchFire(world, x, y + 1, z) ||
           canCatchFire(world, x, y, z - 1) || canCatchFire(world, x, y, z + 1);
}

// `og.g(Lcn;IIII)I` -- getChanceToEncourageFire, folded into a running maximum.
int encourage(const TickWorld& world, i32 x, int y, i32 z, int best)
{
    const int v = int(block::def(world.blockAt(x, y, z)).burnEncourage);
    return v > best ? v : best;
}

// `og.i(Lcn;III)I` -- getChanceOfNeighborsEncouragingFire. Only an empty cell
// can be encouraged, and what encourages it is the most flammable of its six
// neighbours rather than the sum of them.
int neighboursEncouraging(const TickWorld& world, i32 x, int y, i32 z)
{
    if (world.blockAt(x, y, z) != block::kAir) return 0;
    int best = 0;
    best = encourage(world, x + 1, y, z, best);
    best = encourage(world, x - 1, y, z, best);
    best = encourage(world, x, y - 1, z, best);
    best = encourage(world, x, y + 1, z, best);
    best = encourage(world, x, y, z - 1, best);
    best = encourage(world, x, y, z + 1, best);
    return best;
}

// `og.a(Lcn;IIIILjava/util/Random;)V` -- tryToCatchBlockOnFire. `bound` is the
// direction's chance and a *lower* bound means more likely, because the roll
// has to land under the target's own `burnCatch`.
void tryToCatch(TickWorld& world, i32 x, int y, i32 z, int bound, BlockId self,
                JavaRandom& rand)
{
    const int ability = int(block::def(world.blockAt(x, y, z)).burnCatch);
    if (rand.nextInt(bound) >= ability) return;

    const bool wasTnt = world.blockAt(x, y, z) == BlockId(mcver::Block::Tnt);

    // Half the time the block is replaced by fire and half the time it simply
    // goes, which is what makes a burning structure develop holes rather than
    // become a solid block of flame.
    if (rand.nextInt(2) == 0) {
        world.setBlockWithNotify(x, y, z, self);
    } else {
        world.setBlockWithNotify(x, y, z, block::kAir);
    }

    if (wasTnt) {
        // `Block.tnt.onBlockDestroyedByPlayer(world, x, y, z, 0)`, which primes
        // it -- **after** the branch above has already written fire or air over
        // the cell, exactly as `og.a` has it. So a fire that spreads into TNT
        // leaves the block gone either way and lights a full 80-tick fuse on
        // top of it; the metadata argument is a literal 0 and nothing reads it.
        tntDestroyedByPlayer(world, x, y, z);
    }
}

}  // namespace

bool fireCanBeAt(const TickWorld& world, i32 x, int y, i32 z)
{
    // `og.a(Lcn;III)Z`: solid ground under it, or anything burnable beside it.
    return world.opaqueAt(x, y - 1, z) || neighbourCanBurn(world, x, y, z);
}

void fireTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    int age = int(world.dataAt(x, y, z));

    // Ageing and re-scheduling come first and unconditionally, so a fire that
    // is about to go out this tick still counted.
    if (age < 15) {
        world.setDataRaw(x, y, z, u8(age + 1));
        world.scheduleBlockUpdate(x, y, z, self);
    }

    if (!neighbourCanBurn(world, x, y, z)) {
        // Out of fuel. It still survives on solid ground while it is young,
        // which is what makes a fire lit on stone linger for a few seconds
        // instead of vanishing on the next tick.
        if (!world.opaqueAt(x, y - 1, z) || age > 3) {
            world.setBlockWithNotify(x, y, z, block::kAir);
        }
        return;
    }

    // Fully aged, with nothing burnable directly beneath: one roll in four to
    // go out. This is the only way a fed fire ever dies.
    if (!canCatchFire(world, x, y - 1, z) && age == 15 && rand.nextInt(4) == 0) {
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }

    // Consuming and jumping happen on every other tick, and not at all in the
    // first three: `age % 2 == 0 && age > 2`.
    if (age % 2 != 0 || age <= 2) return;

    // Downwards is the most likely direction and upwards the second: the
    // number is the bound of a roll that must come in *under* the target's
    // ability to catch, so smaller is likelier.
    tryToCatch(world, x + 1, y, z, 300, self, rand);
    tryToCatch(world, x - 1, y, z, 300, self, rand);
    tryToCatch(world, x, y - 1, z, 200, self, rand);
    tryToCatch(world, x, y + 1, z, 250, self, rand);
    tryToCatch(world, x, y, z - 1, 300, self, rand);
    tryToCatch(world, x, y, z + 1, 300, self, rand);

    // And the jump into empty air: a 3 x 3 column reaching one block below and
    // four above, with the odds falling off with height above the first.
    for (i32 bx = x - 1; bx <= x + 1; ++bx) {
        for (i32 bz = z - 1; bz <= z + 1; ++bz) {
            for (int by = y - 1; by <= y + 4; ++by) {
                if (bx == x && by == y && bz == z) continue;

                int bound = 100;
                if (by > y + 1) bound += (by - (y + 1)) * 100;

                const int enc = neighboursEncouraging(world, bx, by, bz);
                if (enc <= 0) continue;
                if (rand.nextInt(bound) > enc) continue;
                world.setBlockWithNotify(bx, by, bz, self);
            }
        }
    }
}

void fireNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    (void) self;
    if (fireCanBeAt(world, x, y, z)) return;
    world.setBlockWithNotify(x, y, z, block::kAir);
}

void firePlaced(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (!fireCanBeAt(world, x, y, z)) {
        world.setBlockWithNotify(x, y, z, block::kAir);
        return;
    }
    world.scheduleBlockUpdate(x, y, z, self);
}

}  // namespace mc::tick
