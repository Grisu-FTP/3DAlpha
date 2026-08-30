#include "core/tick/behaviour.hpp"

#include "core/block/registry.hpp"
#include "core/tick/fire.hpp"
#include "core/tick/fluid.hpp"
#include "core/tick/redstone.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {

namespace {

using block::BlockId;
using block::TickBehaviour;

constexpr BlockId kAir = block::kAir;

BlockId id(mcver::Block b) { return BlockId(b); }

bool isLiquid(BlockId b)
{
    return block::def(b).render == block::RenderType::Fluid;
}

// Water, either state. a1.1.2 compares materials rather than ids, which is
// how a flowing block and its source count as the same thing.
bool isWater(BlockId b)
{
    return b == BlockId(mcver::Block::Water) || b == BlockId(mcver::Block::FlowingWater);
}

// `ly.a(Lcn;III)Z` -- Block.canPlaceBlockAt: the cell has to be air or a
// liquid. Every placement in the game funnels through this before its own
// extra conditions.
bool cellIsReplaceable(const TickWorld& world, i32 x, int y, i32 z)
{
    const BlockId there = world.blockAt(x, y, z);
    return there == kAir || isLiquid(there);
}

// a1.1.2 drops the block as an item entity. There are no entities yet, so this
// is where that would go; naming it keeps the call sites honest and makes the
// eventual entity work a search for one symbol rather than for a comment.
void dropBlockAsItem(TickWorld&, i32, int, i32, BlockId, u8) {}

// ---- flowers, mushrooms, saplings, crops: BlockFlower and its subclasses ---

// `mq.b(I)Z` -- BlockFlower.canThisPlantGrowOnThisBlockID. Grass, dirt and
// farmland, and nothing else.
bool flowerGround(BlockId ground)
{
    return ground == id(mcver::Block::Grass) || ground == id(mcver::Block::Dirt) ||
           ground == id(mcver::Block::Farmland);
}

// `ky.b(I)Z` -- BlockMushroom overrides it with a plain lookup into
// `Block.opaqueCubeLookup`, the array and not the live method. That is why a
// mushroom will sit on a leaf block; see the note on BlockDef::opaqueCube.
bool mushroomGround(BlockId ground) { return block::def(ground).opaqueCube; }

// `mq.g(Lcn;III)Z` -- BlockFlower.canBlockStay.
bool flowerCanStay(const TickWorld& world, i32 x, int y, i32 z)
{
    if (world.lightValue(x, y, z) < 8 && !world.canSeeSky(x, y, z)) return false;
    return flowerGround(world.blockAt(x, y - 1, z));
}

// `ky.g(Lcn;III)Z` -- the mushroom wants darkness where the flower wants light.
bool mushroomCanStay(const TickWorld& world, i32 x, int y, i32 z)
{
    if (world.lightValue(x, y, z) > 13) return false;
    return mushroomGround(world.blockAt(x, y - 1, z));
}

// `hd.b(I)Z` -- wheat grows on farmland alone.
bool cropsCanStay(const TickWorld& world, i32 x, int y, i32 z)
{
    if (world.lightValue(x, y, z) < 8 && !world.canSeeSky(x, y, z)) return false;
    return world.blockAt(x, y - 1, z) == id(mcver::Block::Farmland);
}

bool plantCanStay(const TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    switch (block::def(self).tick) {
    case TickBehaviour::Mushroom: return mushroomCanStay(world, x, y, z);
    case TickBehaviour::Crops:    return cropsCanStay(world, x, y, z);
    default:                      return flowerCanStay(world, x, y, z);
    }
}

// `mq.h(Lcn;III)V` -- checkFlowerChange: if it cannot stay, it drops and the
// cell becomes air.
void checkPlantChange(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (plantCanStay(world, x, y, z, self)) return;
    dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
    world.setBlockWithNotify(x, y, z, kAir);
}

// `hd.i(Lcn;III)F` -- BlockCrops.getGrowthRate. A crop grows faster on wet,
// tilled ground, faster still with tilled ground all around it, and half as
// fast when it is in a row or a column of its own kind rather than a block on
// its own. The 1/4 weighting of the diagonals and the halving are both the
// original's; nothing here is rounded or simplified.
float cropGrowthRate(const TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    float rate = 1.0f;

    const BlockId zm = world.blockAt(x, y, z - 1);
    const BlockId zp = world.blockAt(x, y, z + 1);
    const BlockId xm = world.blockAt(x - 1, y, z);
    const BlockId xp = world.blockAt(x + 1, y, z);
    const BlockId xmzm = world.blockAt(x - 1, y, z - 1);
    const BlockId xpzm = world.blockAt(x + 1, y, z - 1);
    const BlockId xpzp = world.blockAt(x + 1, y, z + 1);
    const BlockId xmzp = world.blockAt(x - 1, y, z + 1);

    const bool alongX = xm == self || xp == self;
    const bool alongZ = zm == self || zp == self;
    const bool diagonal = xmzm == self || xpzm == self || xpzp == self || xmzp == self;

    for (i32 bx = x - 1; bx <= x + 1; ++bx) {
        for (i32 bz = z - 1; bz <= z + 1; ++bz) {
            float weight = 0.0f;
            if (world.blockAt(bx, y - 1, bz) == id(mcver::Block::Farmland)) {
                weight = 1.0f;
                if (world.dataAt(bx, y - 1, bz) > 0) weight = 3.0f;
            }
            if (bx != x || bz != z) weight /= 4.0f;
            rate += weight;
        }
    }

    if (diagonal || (alongX && alongZ)) rate /= 2.0f;
    return rate;
}

// ---- grass ------------------------------------------------------------

// `my.a(Lcn;IIILjava/util/Random;)V` -- BlockGrass.updateTick.
void grassTick(TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand)
{
    if (world.lightValue(x, y + 1, z) < 4 && solidOrLiquid(world.blockAt(x, y + 1, z))) {
        // Dies on a one-in-four roll rather than immediately, which is what
        // makes a covered lawn go patchy instead of vanishing in one tick.
        if (rand.nextInt(4) != 0) return;
        world.setBlockWithNotify(x, y, z, id(mcver::Block::Dirt));
        return;
    }

    if (world.lightValue(x, y + 1, z) < 9) return;

    // Spread. Note the asymmetry, which is the original's: the target is
    // picked from a 3x5x3 box centred one block *below* this one, so grass
    // climbs one block a step and falls three.
    const i32 tx = x + rand.nextInt(3) - 1;
    const int ty = y + rand.nextInt(5) - 3;
    const i32 tz = z + rand.nextInt(3) - 1;

    if (world.blockAt(tx, ty, tz) != id(mcver::Block::Dirt)) return;
    if (world.lightValue(tx, ty + 1, tz) < 4) return;
    if (solidOrLiquid(world.blockAt(tx, ty + 1, tz))) return;
    world.setBlockWithNotify(tx, ty, tz, id(mcver::Block::Grass));
}

// ---- ice, snow --------------------------------------------------------

// `he.a(...)` -- BlockIce.updateTick. The threshold is `11 - lightOpacity[ice]`
// and ice's opacity is 3, so ice melts at a stored sky light above 8.
void iceTick(TickWorld& world, i32 x, int y, i32 z)
{
    const int threshold = 11 - int(block::def(id(mcver::Block::Ice)).opacity);
    if (int(world.skyLightAt(x, y, z)) <= threshold) return;
    dropBlockAsItem(world, x, y, z, id(mcver::Block::Ice), world.dataAt(x, y, z));
    world.setBlockWithNotify(x, y, z, id(mcver::Block::Water));
}

// `p.a(...)` and `fd.a(...)` -- both melt above stored sky light 11, and both
// leave nothing behind.
void snowTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (int(world.skyLightAt(x, y, z)) <= 11) return;
    dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
    world.setBlockWithNotify(x, y, z, kAir);
}

// ---- torch ------------------------------------------------------------

// `mj.a(Lcn;III)Z` -- BlockTorch.canPlaceBlockAt: any of the five faces that
// is an opaque cube will hold it. The sixth, the ceiling, will not.
bool torchHasSupport(const TickWorld& world, i32 x, int y, i32 z)
{
    return world.opaqueAt(x - 1, y, z) || world.opaqueAt(x + 1, y, z) ||
           world.opaqueAt(x, y, z - 1) || world.opaqueAt(x, y, z + 1) ||
           world.opaqueAt(x, y - 1, z);
}

// `mj.h(Lcn;III)Z` -- checkIfAttachedToBlock.
bool torchCheckSupport(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (torchHasSupport(world, x, y, z)) return true;
    dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
    world.setBlockWithNotify(x, y, z, kAir);
    return false;
}

// `mj.a(Lcn;IIII)V` -- onNeighborBlockChange. Metadata 1..5 names the face the
// torch is on; if that face has stopped being an opaque cube, it falls.
void torchNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (!torchCheckSupport(world, x, y, z, self)) return;

    const u8 md = world.dataAt(x, y, z);
    bool loose = false;
    if (!world.opaqueAt(x - 1, y, z) && md == 1) loose = true;
    if (!world.opaqueAt(x + 1, y, z) && md == 2) loose = true;
    if (!world.opaqueAt(x, y, z - 1) && md == 3) loose = true;
    if (!world.opaqueAt(x, y, z + 1) && md == 4) loose = true;
    if (!world.opaqueAt(x, y - 1, z) && md == 5) loose = true;
    if (!loose) return;

    dropBlockAsItem(world, x, y, z, self, md);
    world.setBlockWithNotify(x, y, z, kAir);
}

// ---- sugar cane and cactus -------------------------------------------

// `jm.a(Lcn;III)Z` -- BlockReed.canBlockStay: on itself, or on grass or dirt
// with water diagonally adjacent below.
bool reedCanStay(const TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const BlockId below = world.blockAt(x, y - 1, z);
    if (below == self) return true;
    if (below != id(mcver::Block::Grass) && below != id(mcver::Block::Dirt)) return false;

    // The four cells around the block *below*, which is why cane grows at the
    // waterline and not one block back from it.
    return isWater(world.blockAt(x - 1, y - 1, z)) || isWater(world.blockAt(x + 1, y - 1, z)) ||
           isWater(world.blockAt(x, y - 1, z - 1)) || isWater(world.blockAt(x, y - 1, z + 1));
}

// `hy.g(Lcn;III)Z` -- BlockCactus.canBlockStay: nothing solid on any of the
// four sides, and sand or another cactus below.
bool cactusCanStay(const TickWorld& world, i32 x, int y, i32 z)
{
    if (block::def(world.blockAt(x - 1, y, z)).solid) return false;
    if (block::def(world.blockAt(x + 1, y, z)).solid) return false;
    if (block::def(world.blockAt(x, y, z - 1)).solid) return false;
    if (block::def(world.blockAt(x, y, z + 1)).solid) return false;
    const BlockId below = world.blockAt(x, y - 1, z);
    return below == id(mcver::Block::Cactus) || below == id(mcver::Block::Sand);
}

// `jm.a(...)` and `hy.a(...)` -- the growth half of both is the same code with
// a different block, which is a1.1.2 duplicating it rather than sharing it.
void columnGrowTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (world.blockAt(x, y + 1, z) != kAir) return;

    int height = 1;
    while (world.blockAt(x, y - height, z) == self) ++height;
    if (height >= 3) return;

    const u8 age = world.dataAt(x, y, z);
    if (age == 15) {
        world.setBlockWithNotify(x, y + 1, z, self);
        world.setDataRaw(x, y, z, 0);
    } else {
        world.setDataRaw(x, y, z, u8(age + 1));
    }
}

// ---- farmland ---------------------------------------------------------

// `mi.i(Lcn;III)Z` -- BlockFarmland.isWaterNearby: a 9x2x9 box, one block up
// and none down, which is why a covered irrigation channel still works.
bool farmlandWaterNearby(const TickWorld& world, i32 x, int y, i32 z)
{
    for (i32 bx = x - 4; bx <= x + 4; ++bx) {
        for (int by = y; by <= y + 1; ++by) {
            for (i32 bz = z - 4; bz <= z + 4; ++bz) {
                if (isWater(world.blockAt(bx, by, bz))) return true;
            }
        }
    }
    return false;
}

// `mi.h(Lcn;III)Z` -- BlockFarmland.isCropsNearby, with a radius of zero: it
// asks only about the block directly above. The loop is in the original and
// its bounds are `x - 0 .. x + 0`, which is a1.1.2 leaving room it never used.
bool farmlandCropsAbove(const TickWorld& world, i32 x, int y, i32 z)
{
    return world.blockAt(x, y + 1, z) == id(mcver::Block::Wheat);
}

// `mi.a(Lcn;IIILjava/util/Random;)V`.
void farmlandTick(TickWorld& world, i32 x, int y, i32 z, JavaRandom& rand)
{
    if (rand.nextInt(5) != 0) return;

    if (farmlandWaterNearby(world, x, y, z)) {
        world.setDataRaw(x, y, z, 7);
        return;
    }

    const u8 wetness = world.dataAt(x, y, z);
    if (wetness > 0) {
        world.setDataRaw(x, y, z, u8(wetness - 1));
        return;
    }
    // Dry, and nothing planted: it goes back to being dirt. Farmland under a
    // crop never reverts, which is why a field survives a drought.
    if (farmlandCropsAbove(world, x, y, z)) return;
    world.setBlockWithNotify(x, y, z, id(mcver::Block::Dirt));
}

// ---- sand and gravel --------------------------------------------------

// `dh.a_(Lcn;III)Z` -- BlockSand.canFallBelow: air, fire, water or lava.
bool canFallInto(const TickWorld& world, i32 x, int y, i32 z)
{
    const BlockId below = world.blockAt(x, y, z);
    if (below == kAir) return true;
    if (below == id(mcver::Block::Fire)) return true;
    return isLiquid(below);
}

// `dh.h(Lcn;III)V` -- tryToFall.
//
// **The one deviation in this file that changes what a player sees.** a1.1.2
// spawns an `EntityFallingSand` and lets it fall under gravity, taking a few
// tenths of a second for a long drop. There is no entity system yet (see
// docs/status.md M3), so the block is moved to its resting place in one tick
// instead. The resting place is the same one the entity would have found --
// the loop below is the entity's landing test, run to completion -- so the
// world ends up identical and only the animation is missing. This is a
// placeholder with a known replacement, not a design choice.
void fallingTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (!canFallInto(world, x, y - 1, z)) return;
    if (y < 1) return;

    int rest = y;
    while (rest > 0 && canFallInto(world, x, rest - 1, z)) --rest;
    if (rest == y) return;

    world.setBlockWithNotify(x, y, z, kAir);
    world.setBlockWithNotify(x, rest, z, self);
}

// ---- leaves -----------------------------------------------------------

// `iz.g(Lcn;IIII)I` -- BlockLeaves.getDistanceToWood, folded into a running
// maximum. A log answers 16; another leaf block answers its own recorded
// distance when that is higher than what we have; anything else leaves the
// running value alone.
int leafReach(const TickWorld& world, i32 x, int y, i32 z, int best, BlockId self)
{
    const BlockId there = world.blockAt(x, y, z);
    if (there == id(mcver::Block::Log)) return 16;
    if (there != self) return best;
    const int recorded = int(world.dataAt(x, y, z));
    if (recorded != 0 && recorded > best) return recorded;
    return best;
}

// `iz.h(Lcn;III)V` -- the decay pass itself.
//
// The metadata on a leaf block is a *countdown to a log*: 16 next to one, one
// less at each step away, and 1 when there is no log within reach at all. A
// pass reads the five neighbours -- below, both z, both x, and deliberately
// **not** above -- takes the best answer, subtracts one, and writes it back;
// if the value changed, the six neighbours are woken so the change walks out
// through the canopy. Leaves whose value settles at 1 are the ones that decay,
// which is checked in `leavesTick`.
//
// **The budget is not optional.** Two adjacent leaf blocks can each decide the
// other needs re-running, so the walk has to be bounded or a canopy overflows
// the stack -- and on a 3DSX the main thread gets 32 KB that nothing in the
// binary can enlarge. The original bounds it the same way, with `iz.c`: a
// counter on the shared Block object, `if (this.c++ >= 100) return;`, reset at
// the entry points. That is copied exactly, as a budget threaded through the
// walk rather than as a field, because a field shared by every leaf in the
// world is the one part of it that is an artefact of Java rather than a rule.
constexpr int kLeafPassBudget = 100;

void leafPass(TickWorld& world, i32 x, int y, i32 z, BlockId self, int& budget)
{
    if (budget++ >= kLeafPassBudget) return;

    int best = block::def(world.blockAt(x, y - 1, z)).solid ? 16 : 0;

    int recorded = int(world.dataAt(x, y, z));
    if (recorded == 0) {
        recorded = 1;
        world.setDataRaw(x, y, z, 1);
    }

    best = leafReach(world, x, y - 1, z, best, self);
    best = leafReach(world, x, y, z - 1, best, self);
    best = leafReach(world, x, y, z + 1, best, self);
    best = leafReach(world, x - 1, y, z, best, self);
    best = leafReach(world, x + 1, y, z, best, self);

    int next = best - 1;
    if (next < 10) next = 1;
    if (next == recorded) return;

    world.setDataRaw(x, y, z, u8(next));
    // `iz.f` on each of the six: a neighbouring leaf whose recorded value was
    // exactly one more than ours re-runs its own pass.
    const i32 dx[6] = {0, 0, 0, 0, -1, 1};
    const int dy[6] = {-1, 1, 0, 0, 0, 0};
    const i32 dz[6] = {0, 0, -1, 1, 0, 0};
    for (int i = 0; i < 6; ++i) {
        const i32 nx = x + dx[i];
        const int ny = y + dy[i];
        const i32 nz = z + dz[i];
        if (world.blockAt(nx, ny, nz) != self) continue;
        const int theirs = int(world.dataAt(nx, ny, nz));
        if (theirs != 0 && theirs != recorded - 1) continue;
        leafPass(world, nx, ny, nz, self, budget);
    }
}

// The entry point, which is where the original resets its counter.
void leafPass(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    int budget = 0;
    leafPass(world, x, y, z, self, budget);
}

// `iz.a(Lcn;IIILjava/util/Random;)V`.
void leavesTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    const u8 md = world.dataAt(x, y, z);
    if (md == 0) {
        leafPass(world, x, y, z, self);
    } else if (md == 1) {
        // `iz.i` -- out of reach of any log, so it goes.
        dropBlockAsItem(world, x, y, z, self, md);
        world.setBlockWithNotify(x, y, z, kAir);
    } else if (rand.nextInt(10) == 0) {
        leafPass(world, x, y, z, self);
    }
}

// ---- sapling ----------------------------------------------------------

// `dt.a(Lcn;IIILjava/util/Random;)V` -- BlockSapling.updateTick.
//
// **Half implemented, and the missing half is named.** The metadata counter
// and the light and roll conditions are exactly the original's. What is not
// wired is the last step: at metadata 15 a1.1.2 clears the cell and runs
// `WorldGenTrees`, or `WorldGenBigTree` on a one-in-ten roll, putting the
// sapling back if the generator declines. Both generators exist in this tree
// -- `src/impl/worldgen/alpha_nobiome/trees.cpp` and `big_tree.cpp` -- but
// they write through `PopulationView`, a window sized and positioned for
// population, and handing them a live world is an adapter rather than a call.
// Until that exists the counter saturates at 15 and the sapling stays a
// sapling, which is visibly wrong and deliberately not hidden.
void saplingTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    checkPlantChange(world, x, y, z, self);
    if (world.blockAt(x, y, z) != self) return;

    if (world.lightValue(x, y + 1, z) < 9) return;
    if (rand.nextInt(5) != 0) return;

    const u8 age = world.dataAt(x, y, z);
    if (age < 15) world.setDataRaw(x, y, z, u8(age + 1));
}

// `hd.a(Lcn;IIILjava/util/Random;)V` -- BlockCrops.updateTick.
void cropsTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    checkPlantChange(world, x, y, z, self);
    if (world.blockAt(x, y, z) != self) return;

    if (world.lightValue(x, y + 1, z) < 9) return;

    const u8 age = world.dataAt(x, y, z);
    if (age >= 7) return;

    const float rate = cropGrowthRate(world, x, y, z, self);
    if (rand.nextInt(int(100.0f / rate)) != 0) return;
    world.setDataRaw(x, y, z, u8(age + 1));
}

}  // namespace

bool solidOrLiquid(BlockId b)
{
    const block::BlockDef& d = block::def(b);
    return d.solid || d.render == block::RenderType::Fluid;
}

bool canPlaceSnowAt(const TickWorld& world, i32 x, int y, i32 z)
{
    // `fd.a(Lcn;III)Z`: the block below must exist, be an opaque cube, and
    // have a solid material. All three, because a1.1.2 asks all three.
    const BlockId below = world.blockAt(x, y - 1, z);
    if (below == kAir) return false;
    if (!block::def(below).opaque) return false;
    return block::def(below).solid;
}

void updateTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    switch (block::def(self).tick) {
    case TickBehaviour::Grass:
        grassTick(world, x, y, z, rand);
        break;
    case TickBehaviour::Sapling:
        saplingTick(world, x, y, z, self, rand);
        break;
    case TickBehaviour::Leaves:
        leavesTick(world, x, y, z, self, rand);
        break;
    case TickBehaviour::Plant:
    case TickBehaviour::Mushroom:
        checkPlantChange(world, x, y, z, self);
        break;
    case TickBehaviour::Crops:
        cropsTick(world, x, y, z, self, rand);
        break;
    case TickBehaviour::Farmland:
        farmlandTick(world, x, y, z, rand);
        break;
    case TickBehaviour::Reed:
        if (!reedCanStay(world, x, y, z, self)) {
            dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
            world.setBlockWithNotify(x, y, z, kAir);
            break;
        }
        columnGrowTick(world, x, y, z, self);
        break;
    case TickBehaviour::Cactus:
        if (!cactusCanStay(world, x, y, z)) {
            dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
            world.setBlockWithNotify(x, y, z, kAir);
            break;
        }
        columnGrowTick(world, x, y, z, self);
        break;
    case TickBehaviour::Ice:
        iceTick(world, x, y, z);
        break;
    case TickBehaviour::SnowLayer:
    case TickBehaviour::SnowBlock:
        snowTick(world, x, y, z, self);
        break;
    case TickBehaviour::Torch:
        // `mj.a(...)`: a torch with metadata 0 has not been given a face yet,
        // which is what a world file straight out of generation contains.
        if (world.dataAt(x, y, z) == 0) torchNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Falling:
        fallingTick(world, x, y, z, self);
        break;

    case TickBehaviour::Fire:
        fireTick(world, x, y, z, self, rand);
        break;
    case TickBehaviour::RedstoneTorch:
        redstoneTorchTick(world, x, y, z, self, rand);
        break;
    case TickBehaviour::RedstoneOre:
        redstoneOreTick(world, x, y, z, self);
        break;
    case TickBehaviour::Button:
        buttonTick(world, x, y, z, self);
        break;
    case TickBehaviour::FluidFlowing:
        fluidFlowingTick(world, x, y, z, self, rand);
        break;
    case TickBehaviour::FluidStill:
        fluidStillTick(world, x, y, z, self, rand);
        break;

    // Not yet ported, each for a reason that is a whole subsystem rather than
    // a rule: the first eight need redstone power propagation, and Fire needs
    // the per-block flammability tables. Falling through here is the same as
    // the original doing nothing, which is wrong but not unsafe.
    // A pressure plate's updateTick looks for entities standing on it, and
    // there are none; a lever and a door only ever change because someone
    // touches them. Both are dispatched on the *neighbour* path below, which
    // is the half that works without a player.
    case TickBehaviour::PressurePlate:
    case TickBehaviour::Lever:
    case TickBehaviour::Door:
    case TickBehaviour::RedstoneWire:
    case TickBehaviour::Rail:
    case TickBehaviour::Ladder:
    case TickBehaviour::Sign:
    case TickBehaviour::Tnt:
    case TickBehaviour::Sponge:
    case TickBehaviour::Stairs:
    case TickBehaviour::None:
    case TickBehaviour::Count:
        break;
    }
}

void blockAdded(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    switch (block::def(self).tick) {
    case TickBehaviour::FluidFlowing:
    case TickBehaviour::FluidStill:
        fluidPlaced(world, x, y, z, self);
        break;
    case TickBehaviour::Falling:
        // `dh.e(Lcn;III)V` -- sand asks to be ticked the moment it appears,
        // which is how a column of it collapses from the bottom up.
        world.scheduleBlockUpdate(x, y, z, self);
        break;
    case TickBehaviour::Fire:
        firePlaced(world, x, y, z, self);
        break;
    case TickBehaviour::RedstoneWire:
        wirePlaced(world, x, y, z, self);
        break;
    case TickBehaviour::RedstoneTorch:
        redstoneTorchPlaced(world, x, y, z, self);
        break;
    default:
        // `ly.e(Lcn;III)V` is empty, and so is this for everything else.
        break;
    }
}

void blockRemoved(TickWorld& world, i32 x, int y, i32 z, BlockId old)
{
    switch (block::def(old).tick) {
    case TickBehaviour::RedstoneWire:
        wireRemoved(world, x, y, z, old);
        break;
    case TickBehaviour::RedstoneTorch:
        redstoneTorchRemoved(world, x, y, z, old);
        break;
    default:
        // `ly.b(Lcn;III)V` is empty, and so is this for everything else. The
        // remaining overrides -- a chest spilling its contents, a sign
        // dropping -- belong to behaviours that are not ported.
        break;
    }
}

void neighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId fromId)
{
    const BlockId self = world.blockAt(x, y, z);
    if (self == kAir) return;

    switch (block::def(self).tick) {
    case TickBehaviour::Sapling:
    case TickBehaviour::Plant:
    case TickBehaviour::Mushroom:
    case TickBehaviour::Crops:
        checkPlantChange(world, x, y, z, self);
        break;
    case TickBehaviour::Leaves:
        leafPass(world, x, y, z, self);
        break;
    case TickBehaviour::Torch:
        torchNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Reed:
        if (!reedCanStay(world, x, y, z, self)) {
            dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
            world.setBlockWithNotify(x, y, z, kAir);
        }
        break;
    case TickBehaviour::Cactus:
        if (!cactusCanStay(world, x, y, z)) {
            dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
            world.setBlockWithNotify(x, y, z, kAir);
        }
        break;
    case TickBehaviour::SnowLayer:
        // `fd.h`: it needs something under it, and unlike the torch it has no
        // metadata to say which face.
        if (!canPlaceSnowAt(world, x, y, z)) {
            dropBlockAsItem(world, x, y, z, self, world.dataAt(x, y, z));
            world.setBlockWithNotify(x, y, z, kAir);
        }
        break;
    case TickBehaviour::Farmland:
        // `mi.a(Lcn;IIII)V`: something solid landed on it.
        if (block::def(world.blockAt(x, y + 1, z)).solid) {
            world.setBlockWithNotify(x, y, z, id(mcver::Block::Dirt));
        }
        break;
    case TickBehaviour::Falling:
        // `dh.a(Lcn;IIII)V`: sand does not fall on the spot, it asks to be
        // ticked at its own rate of 3, which is what gives a collapsing pile
        // its stagger.
        world.scheduleBlockUpdate(x, y, z, self);
        break;
    case TickBehaviour::FluidFlowing:
    case TickBehaviour::FluidStill:
        fluidNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Fire:
        fireNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::RedstoneWire:
        wireNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Lever:
    case TickBehaviour::Button:
    case TickBehaviour::PressurePlate:
        switchNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Door:
        doorNeighbourChanged(world, x, y, z, self, fromId);
        break;
    case TickBehaviour::RedstoneTorch:
        // `bg.a(Lcn;IIII)V`: the support check first -- a redstone torch is a
        // torch -- and then it asks to be re-evaluated at its rate of 2, which
        // is the delay every redstone circuit is built out of.
        torchNeighbourChanged(world, x, y, z, self);
        if (world.blockAt(x, y, z) == self) {
            redstoneTorchNeighbourChanged(world, x, y, z, self);
        }
        break;
    default:
        break;
    }
}

}  // namespace mc::tick
