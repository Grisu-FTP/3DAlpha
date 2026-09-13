#include "core/tick/behaviour.hpp"
#include "core/tick/drop.hpp"
#include "core/tick/rail.hpp"

#include "core/block/registry.hpp"
#include "core/tick/fire.hpp"
#include "core/tick/fluid.hpp"
#include "core/tick/redstone.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"
#include "core/world/tile_entity.hpp"

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

// **All three melts read stored *block* light, not sky light.**
//
// `he.a`, `p.a` and `fd.a` all begin `getstatic by.b` followed by
// `cn.a(Lby;III)I`, and `by`'s static initialiser names its two constants in
// order: `by.a` is "Sky" with a default of 15, `by.b` is "Block" with a
// default of 0. Read out of the jar rather than remembered, because reading it
// as sky light is a bug that hides in plain sight: outdoors the sky light is
// 15, which is above every threshold here, so **every exposed block of ice and
// snow melts on its first random tick** -- and then `TickWorld::snowAndIce`,
// which does check block light, puts it straight back. A winter world spent
// its entire life thawing and refreezing.
//
// Block light is 0 outdoors whatever the hour, so in a1.1.2 nothing melts
// until a player brings a light source near it. That is the whole mechanic.

// `he.a(...)` -- BlockIce.updateTick. The threshold is `11 - lightOpacity[ice]`
// and ice's opacity is 3, so ice melts at a block light above 8.
void iceTick(TickWorld& world, i32 x, int y, i32 z)
{
    const int threshold = 11 - int(block::def(id(mcver::Block::Ice)).opacity);
    if (int(world.blockLightAt(x, y, z)) <= threshold) return;
    dropBlockAsItem(world, x, y, z, id(mcver::Block::Ice), world.dataAt(x, y, z));
    world.setBlockWithNotify(x, y, z, id(mcver::Block::Water));
}

// `p.a(...)` and `fd.a(...)` -- both melt above block light 11, and both leave
// nothing behind.
void snowTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (int(world.blockLightAt(x, y, z)) <= 11) return;
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

// ---- ladder and sign ---------------------------------------------------

// `br.a(Lcn;IIII)V` -- **BlockLadder.onNeighborBlockChange**, and it had no
// entry in this table at all until now: a ladder whose wall was knocked away
// stayed hanging in the air, and so did a sign.
//
// The rule is the metadata's own face and only that one. A ladder at metadata 2
// hangs on the +z wall, 3 on -z, 4 on +x, 5 on -x -- the same numbering
// `block::placementMetadata` writes and `collisionBoxes` reads, which is why
// none of it appears here as a literal beyond the four cases.
void ladderNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const u8 md = world.dataAt(x, y, z);

    bool held = false;
    if (md == 2 && world.opaqueAt(x, y, z + 1)) held = true;
    if (md == 3 && world.opaqueAt(x, y, z - 1)) held = true;
    if (md == 4 && world.opaqueAt(x + 1, y, z)) held = true;
    if (md == 5 && world.opaqueAt(x - 1, y, z)) held = true;
    if (held) return;

    dropBlockAsItem(world, x, y, z, self, md);
    world.setBlockWithNotify(x, y, z, kAir);
}

// `lr.a(Lcn;IIII)V` -- BlockSign.onNeighborBlockChange. A post asks the
// **material** under it to be solid; a wall sign asks the face its metadata
// names to be. `lr` tells the two apart by a boolean it was constructed with --
// `new lr(63, ob.class, true)` and `new lr(68, ob.class, false)` -- which is a
// distinction this table had nowhere to put, so the behaviour column carries
// two names now. That is the same move the two pressure plates already made
// and for the same reason; see data/<version>/blocks.json.
//
// A sign that falls loses its writing, in a1.1.2 too: the tile entity goes with
// the block in both, through `blockRemoved`.
void signNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const bool post = block::def(self).tick == TickBehaviour::SignPost;
    const u8 md = world.dataAt(x, y, z);

    bool broken;
    if (post) {
        // `world.getBlockMaterial(i, j - 1, k).isSolid()`, which is the
        // material's own flag and not `Block.isOpaqueCube` -- so a sign will
        // stand on a slab, a fence or another sign.
        broken = !block::def(world.blockAt(x, y - 1, z)).solid;
    } else {
        broken = true;
        if (md == 2 && block::def(world.blockAt(x, y, z + 1)).solid) broken = false;
        if (md == 3 && block::def(world.blockAt(x, y, z - 1)).solid) broken = false;
        if (md == 4 && block::def(world.blockAt(x + 1, y, z)).solid) broken = false;
        if (md == 5 && block::def(world.blockAt(x - 1, y, z)).solid) broken = false;
    }
    if (!broken) return;

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

// `mi.a(Lcn;IIILkh;)V` -- BlockFarmland.onEntityWalking, and the whole of it
// is one roll in four:
//
//     if (world.rand.nextInt(4) == 0) {
//         world.setBlockWithNotify(i, j, k, Block.dirt.blockID);
//     }
//
// **Nothing else is in it in a1.1.2.** No fall-distance test -- the version
// where a jump is what ruins a field, and a walk never does, is Beta's -- no
// crop test, and no check on what is standing there. So a field is trampled by
// being *walked over*, one roll per footstep, and running or jumping across it
// costs exactly what walking does: `moveEntity` pays out one footstep per
// block of ground covered however fast the ground is covered.
//
// **The draw is `World.rand`**, not the random-tick generator -- this happens
// inside an entity's move, not in a block tick.
void farmlandTrampled(TickWorld& world, i32 x, int y, i32 z)
{
    if (world.random().nextInt(4) != 0) return;
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

// `dh.h(Lcn;III)V` -- tryToFall, and **both of its branches**:
//
// ```
// if (canFallBelow(world, i, j - 1, k) && j >= 0) {
//     EntityFallingSand e = new EntityFallingSand(world, i + 0.5F, j + 0.5F, k + 0.5F, blockID);
//     if (fallInstantly) { while (!e.isDead) e.onUpdate(); }
//     else               { world.spawnEntityInWorld(e); }
// }
// ```
//
// `fallInstantly` is a static on BlockSand, set while a chunk is populated, and
// it is what stops a generated world raining sand as the player walks into it.
// This port reaches the same fork from the other side: **an unset spawn seam
// means nobody is watching**, which is true of world generation and of every
// headless tool here. See `TickWorld::spawnFallingBlock`.
//
// The instant path below is not the entity's tick run in a loop; it is the
// resting place that loop would have found, computed directly. The two agree
// because the entity falls straight down through cells that a moment ago
// answered `canFallBelow` -- which is the same walk. Where they differ is the
// one case the entity has and the loop cannot: landing on something that
// refuses the write and dropping as an item instead.
void fallingTick(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (!canFallInto(world, x, y - 1, z)) return;
    if (y < 1) return;

    // **The entity clears the source cell itself**, on its first tick, so
    // nothing is written here -- the block stays where it is until the thing
    // standing in for it has somewhere to be.
    if (world.spawnFallingBlock(x, y, z, self)) {
        return;
    }

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

// `b.h(Lcn;III)Z` -- BlockChest.isThereANeighborChest, which is a question
// about a *neighbour's* neighbours: is the cell a chest that is already half of
// a pair? `chest` is the id being asked about rather than a constant, so a
// version whose chest is not 54 needs no edit here.
bool chestHasNeighbour(const TickWorld& world, BlockId chest, i32 x, int y, i32 z)
{
    if (world.blockAt(x, y, z) != chest) {
        return false;
    }
    return world.blockAt(x - 1, y, z) == chest || world.blockAt(x + 1, y, z) == chest
           || world.blockAt(x, y, z - 1) == chest || world.blockAt(x, y, z + 1) == chest;
}

// `b.a(Lcn;III)Z` -- **BlockChest.canPlaceBlockAt**, the only override in
// a1.1.2 that asks about its own kind rather than about the ground:
//
// ```
// int n = 0;
// if (getBlockId(i - 1, j, k) == blockID) n++;   // and the other three
// if (n > 1) return false;
// if (isThereANeighborChest(world, i - 1, j, k)) return false;   // and the other three
// return true;
// ```
//
// So one chest may join one chest, and never one that is already joined: a
// double chest is the largest thing that can be *placed*. Note what is missing
// -- the override never calls `super`, and therefore never asks whether the
// cell is free. That is invisible through the placement path, because
// `World.canBlockBePlacedAt` has already asked; it is transcribed as it stands
// rather than tidied, and the base test above is the one that answers.
bool chestCanPlaceAt(const TickWorld& world, BlockId chest, i32 x, int y, i32 z)
{
    constexpr int kSides[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    int neighbours = 0;
    for (const auto& side : kSides) {
        if (world.blockAt(x + side[0], y, z + side[1]) == chest) {
            ++neighbours;
        }
    }
    if (neighbours > 1) {
        return false;
    }
    for (const auto& side : kSides) {
        if (chestHasNeighbour(world, chest, x + side[0], y, z + side[1])) {
            return false;
        }
    }
    return true;
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

bool canPlaceAt(const TickWorld& world, BlockId self, i32 x, int y, i32 z)
{
    // `ly.a(Lcn;III)Z`, which every override calls through `super` before its
    // own condition. A liquid counts as free space, which is how you build into
    // a pond in the original.
    if (!cellIsReplaceable(world, x, y, z)) {
        return false;
    }

    if (self == id(mcver::Block::Fence) && !world.improvedFencePlacement()) {
        // `fh.a(Lcn;III)Z`: not on another fence, and not over a material that
        // is not solid (`gb.a()`, the `solid` column) -- so never in the air.
        // `fh` has no `canBlockStay`, which is why this is placement only.
        const BlockId below = world.blockAt(x, y - 1, z);
        return below != self && block::def(below).solid;
    }

    switch (block::def(self).tick) {
    case TickBehaviour::Plant:
    case TickBehaviour::Sapling:
        // `mq.a(Lcn;III)Z` -- the ground, and **not** the light. `canBlockStay`
        // wants light too and the tick applies it a moment later; the original
        // lets you plant a sapling in the dark and then kills it, and copying
        // only the ground half is what keeps that sequence intact.
        return flowerGround(world.blockAt(x, y - 1, z));
    case TickBehaviour::Mushroom:
        return mushroomGround(world.blockAt(x, y - 1, z));
    case TickBehaviour::Crops:
        return world.blockAt(x, y - 1, z) == id(mcver::Block::Farmland);

    case TickBehaviour::Chest:
        // **At most one chest beside it, and that one not already paired.**
        // The bigger clusters a chest screen can join -- `chestInventoryParts`
        // builds up to five -- are not placeable through this, which is the
        // original's own arrangement: they are reached through the hole in
        // `World.canBlockBePlacedAt`, which takes a cell holding water, lava,
        // fire or a snow layer without ever asking the block. See
        // core/item/use.cpp.
        return chestCanPlaceAt(world, self, x, y, z);

    case TickBehaviour::Cactus:
        // `hy.a(Lcn;III)Z` calls `canBlockStay` outright, so this is the whole
        // rule: sand or another cactus below, and nothing solid on any side.
        return cactusCanStay(world, x, y, z);
    case TickBehaviour::Reed:
        return reedCanStay(world, x, y, z, self);

    case TickBehaviour::Torch:
    case TickBehaviour::RedstoneTorch:
        return torchHasSupport(world, x, y, z);

    case TickBehaviour::SnowLayer:
        return canPlaceSnowAt(world, x, y, z);

    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
    case TickBehaviour::Rail:
    case TickBehaviour::RedstoneWire:
        // A plate, a rail and a wire all want the same thing and say so in
        // three different classes: `world.isBlockNormalCube(i, j - 1, k)`.
        return world.opaqueAt(x, y - 1, z);

    case TickBehaviour::Lever:
    case TickBehaviour::Button:
        // A button needs a wall; a lever will take the floor as well. Neither
        // takes a ceiling.
        return world.opaqueAt(x - 1, y, z) || world.opaqueAt(x + 1, y, z)
               || world.opaqueAt(x, y, z - 1) || world.opaqueAt(x, y, z + 1)
               || (block::def(self).tick == TickBehaviour::Lever
                   && world.opaqueAt(x, y - 1, z));

    case TickBehaviour::Ladder:
        // `br.a(Lcn;III)Z` -- a wall to hang on, and only a wall.
        return world.opaqueAt(x - 1, y, z) || world.opaqueAt(x + 1, y, z)
               || world.opaqueAt(x, y, z - 1) || world.opaqueAt(x, y, z + 1);

    case TickBehaviour::SignPost:
    case TickBehaviour::SignWall:
        // A post stands on a solid block; a wall sign hangs on one. The face is
        // chosen at placement, so either will do here.
        return world.opaqueAt(x, y - 1, z) || world.opaqueAt(x - 1, y, z)
               || world.opaqueAt(x + 1, y, z) || world.opaqueAt(x, y, z - 1)
               || world.opaqueAt(x, y, z + 1);

    case TickBehaviour::Door:
        // `fw.a(Lcn;III)Z` -- a solid floor, and room for the upper half. The
        // second half is why a door will not go under a ceiling.
        return y + 1 < mcver::kWorldHeight && world.opaqueAt(x, y - 1, z)
               && cellIsReplaceable(world, x, y + 1, z);

    default:
        // Everything else takes the base answer, sand and gravel included --
        // they fall rather than refuse.
        return true;
    }
}

u8 attachedPlacementMetadata(const TickWorld& world, BlockId placed, i32 x, int y, i32 z,
                             u8 metadata)
{
    const TickBehaviour behaviour = block::def(placed).tick;
    const int orientation = metadata & 7;
    const auto opaque = [&](int dx, int dy, int dz) {
        return world.opaqueAt(x + dx, y + dy, z + dz);
    };

    switch (behaviour) {
    case TickBehaviour::Torch:
    case TickBehaviour::RedstoneTorch:
    case TickBehaviour::Lever:
    case TickBehaviour::Button: {
        // 1 to 4 hang on the -x, +x, -z and +z walls; 5, and a floor lever's
        // other roll 6, stand on the block below.
        bool held = false;
        switch (orientation) {
        case 1: held = opaque(-1, 0, 0); break;
        case 2: held = opaque(1, 0, 0); break;
        case 3: held = opaque(0, 0, -1); break;
        case 4: held = opaque(0, 0, 1); break;
        case 5:
        case 6: held = opaque(0, -1, 0); break;
        default: break;
        }
        if (held) {
            return metadata;
        }
        // Bit 3 is a lever's "on" and a button's "pressed"; `onBlockPlaced`
        // carries it across. Both are clear on anything put down by hand.
        const u8 kept = u8(metadata & 8);
        if (behaviour == TickBehaviour::Lever) {
            return kept;  // `leverPlaced` walks the chain; see the header
        }
        // `mj.e` and `hu.e` -- onBlockAdded's chain. The button's stops at the
        // walls.
        if (opaque(-1, 0, 0)) return u8(kept | 1);
        if (opaque(1, 0, 0)) return u8(kept | 2);
        if (opaque(0, 0, -1)) return u8(kept | 3);
        if (opaque(0, 0, 1)) return u8(kept | 4);
        if (behaviour != TickBehaviour::Button && opaque(0, -1, 0)) return u8(kept | 5);
        return kept;
    }
    case TickBehaviour::Ladder: {
        // `br.d` -- 2 hangs on +z, 3 on -z, 4 on +x, 5 on -x.
        const bool held = (orientation == 2 && opaque(0, 0, 1))
                          || (orientation == 3 && opaque(0, 0, -1))
                          || (orientation == 4 && opaque(1, 0, 0))
                          || (orientation == 5 && opaque(-1, 0, 0));
        if (held) {
            return metadata;
        }
        if (opaque(0, 0, 1)) return 2;
        if (opaque(0, 0, -1)) return 3;
        if (opaque(1, 0, 0)) return 4;
        if (opaque(-1, 0, 0)) return 5;
        return 0;
    }
    default:
        return metadata;
    }
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
    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
        // The disarming half. A plate under load re-schedules itself, so this
        // runs every twenty ticks for as long as something stands on it and
        // once more after it steps off, which is the tick that lets it up.
        pressurePlateTick(world, x, y, z, self);
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
    // A lever and a door only ever change because someone touches them, and
    // are dispatched on the *neighbour* path below -- the half that works
    // without a player.
    //
    // **TNT is in this list because it does nothing here and not because it is
    // missing.** `q` has no `updateTick` at all: the four things that light it
    // are a break, a fire, a blast and a neighbour going live, and all four are
    // ported. See core/entity/primed_tnt.hpp.
    case TickBehaviour::Lever:
    case TickBehaviour::Door:
    case TickBehaviour::RedstoneWire:
    case TickBehaviour::Rail:
    case TickBehaviour::Ladder:
    case TickBehaviour::SignPost:
    case TickBehaviour::SignWall:
    case TickBehaviour::Tnt:
    case TickBehaviour::Sponge:
    case TickBehaviour::Stairs:
    case TickBehaviour::Furnace:
    case TickBehaviour::None:
    case TickBehaviour::Count:
        break;
    }
}

namespace {

// `oi.e(Lcn;III)V` -- `BlockStep.onBlockAdded`, and **the reason two slabs
// become a double slab**.
//
// It is worth being precise about where this lives, because the obvious place
// is wrong. Nothing in `ItemBlock.onItemUse` merges anything: it offsets by the
// struck face, checks the cell is free, and writes one block. The merge happens
// *afterwards*, in the block's own `onBlockAdded`, which looks down, and if it
// finds a single slab there deletes itself and promotes the one below. So the
// placement path stays ignorant of slabs and this is a tick behaviour.
//
// **The test is on the block below and not on the block being added**, exactly
// as the class file has it -- there is no `this == stairSingle` guard on the
// merge, only on the `super` call above it. So a *double* slab placed on a
// single slab also collapses into one double slab, which is a real a1.1.2 quirk
// and is measurable in a running client. Ours does the same.
void slabPlaced(TickWorld& world, i32 x, int y, i32 z)
{
    if (y <= 0) {
        return;
    }
    if (world.blockAt(x, y - 1, z) != BlockId(mcver::Block::Slab)) {
        return;
    }
    // `cn.d(IIII)Z` both times, which is `setBlockWithNotify` -- the same call
    // the class file makes, so the two writes wake their neighbours exactly
    // where the original's do.
    world.setBlockWithNotify(x, y, z, block::kAir);
    world.setBlockWithNotify(x, y - 1, z, BlockId(mcver::Block::DoubleSlab));
}

// `ku.h(Lcn;III)V`, run from `ku.e` -- BlockFurnace.onBlockAdded, and **the
// whole of which way a furnace faces**. Not the player's heading (a1.1.2 has no
// onBlockPlacedBy) and not the face that was clicked (the furnace has no
// onBlockPlaced): the mouth turns away from an opaque neighbour, and faces +Z
// when nothing says otherwise.
//
// Each rule wants the neighbour on one side opaque *and the opposite one not*,
// and they are asked in this order with the last that holds winning -- so a
// furnace between two walls ignores them and one in a corner answers to the X
// walls. `ly.p` is the opaque-cube array, not the live method.
void furnacePlaced(TickWorld& world, i32 x, int y, i32 z)
{
    const bool negZ = block::def(world.blockAt(x, y, z - 1)).opaqueCube;
    const bool posZ = block::def(world.blockAt(x, y, z + 1)).opaqueCube;
    const bool negX = block::def(world.blockAt(x - 1, y, z)).opaqueCube;
    const bool posX = block::def(world.blockAt(x + 1, y, z)).opaqueCube;

    u8 facing = 3;
    if (negZ && !posZ) facing = 3;
    if (posZ && !negZ) facing = 2;
    if (negX && !posX) facing = 5;
    if (posX && !negX) facing = 4;

    // `cn.b(IIII)V`, which in a1.1.2 is the bare chunk write -- no neighbour
    // is told, because the setBlockWithNotify this runs inside tells them next.
    world.setDataRaw(x, y, z, facing);
}

// `km.j(Lcn;III)Z`: is there a staircase here? Asked of the render type,
// `getRenderType() == 10`, exactly as the jar asks it -- the same way a liquid
// is recognised above.
bool isStairs(const TickWorld& world, i32 x, int y, i32 z)
{
    const BlockId there = world.blockAt(x, y, z);
    return there != kAir && block::def(there).render == block::RenderType::Stairs;
}

// `km.i(Lcn;III)Z`: is the material here solid? Glass and leaves are.
bool solidAt(const TickWorld& world, i32 x, int y, i32 z)
{
    return block::def(world.blockAt(x, y, z)).solid;
}

// `km.h(Lcn;III)V` -- **a staircase works out which way it faces from what is
// around it**, and nothing else decides it: there is no onBlockPlaced on
// BlockStairs and no onBlockPlacedBy anywhere in a1.1.2.
//
// Metadata is the side the step's high half is on: 0 +X, 1 -X, 2 +Z, 3 -Z --
// the numbering `core/block/collision.cpp` builds the two boxes from. Three
// passes, each asked only if the one before found nothing, and inside each the
// last rule that holds wins:
//
//   1. a staircase one step **up** on a side: climb towards it;
//   2. a solid block on one side and not the other: back onto it;
//   3. a staircase one step **down** on a side: climb away from it.
//
// Finding nothing leaves the metadata alone, which for a staircase just put
// down is the 0 setBlockWithNotify wrote.
void stairsShape(TickWorld& world, i32 x, int y, i32 z)
{
    if (!isStairs(world, x, y, z)) {
        return;
    }

    int facing = -1;
    if (isStairs(world, x + 1, y + 1, z)) facing = 0;
    if (isStairs(world, x - 1, y + 1, z)) facing = 1;
    if (isStairs(world, x, y + 1, z + 1)) facing = 2;
    if (isStairs(world, x, y + 1, z - 1)) facing = 3;

    if (facing < 0) {
        if (solidAt(world, x + 1, y, z) && !solidAt(world, x - 1, y, z)) facing = 0;
        if (solidAt(world, x - 1, y, z) && !solidAt(world, x + 1, y, z)) facing = 1;
        if (solidAt(world, x, y, z + 1) && !solidAt(world, x, y, z - 1)) facing = 2;
        if (solidAt(world, x, y, z - 1) && !solidAt(world, x, y, z + 1)) facing = 3;
    }

    if (facing < 0) {
        if (isStairs(world, x - 1, y - 1, z)) facing = 0;
        if (isStairs(world, x + 1, y - 1, z)) facing = 1;
        if (isStairs(world, x, y - 1, z - 1)) facing = 2;
        if (isStairs(world, x, y - 1, z + 1)) facing = 3;
    }

    // `cn.b(IIII)V` again: the bare write. Nothing is notified, which is also
    // why a staircase re-shaping its neighbours below cannot recurse.
    if (facing >= 0) {
        world.setDataRaw(x, y, z, u8(facing));
    }
}

// `km.a(Lcn;IIII)V` -- BlockStairs.onNeighborBlockChange, which `km.e`
// (onBlockAdded) also calls, with 0 for the neighbour. Two things happen
// whenever anything next to a staircase changes:
//
//   * **Something solid on top turns it into the block it is made of** --
//     wooden stairs become planks, cobblestone stairs cobblestone. That is
//     a1.1.2's, and it includes putting the staircase *under* a block in the
//     first place, since onBlockAdded asks the same question.
//   * Otherwise it re-shapes itself **and the eight staircases it could be
//     part of a flight with**: the four one step down and the four one step
//     up. So laying a flight shapes it as it goes.
//
// The original starts with `if (world.multiplayerWorld) return;`, which is
// never true here. And it ends by forwarding the call to the model block,
// whose own onNeighborBlockChange is Block's empty one for both staircases.
void stairsNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const BlockId model = block::modelOf(self);
    if (model != kAir && solidAt(world, x, y + 1, z)) {
        world.setBlockWithNotify(x, y, z, model);
        return;
    }

    stairsShape(world, x, y, z);
    stairsShape(world, x + 1, y - 1, z);
    stairsShape(world, x - 1, y - 1, z);
    stairsShape(world, x, y - 1, z - 1);
    stairsShape(world, x, y - 1, z + 1);
    stairsShape(world, x + 1, y + 1, z);
    stairsShape(world, x - 1, y + 1, z);
    stairsShape(world, x, y + 1, z - 1);
    stairsShape(world, x, y + 1, z + 1);
}

}  // namespace

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
    case TickBehaviour::Lever:
        leverPlaced(world, x, y, z, self);
        break;
    case TickBehaviour::Rail:
        // `if.e(Lcn;III)V` -- the shape is worked out after the write, not
        // chosen at placement. See core/tick/rail.hpp.
        railPlaced(world, x, y, z);
        break;
    case TickBehaviour::Slab:
        slabPlaced(world, x, y, z);
        break;
    case TickBehaviour::Furnace:
        furnacePlaced(world, x, y, z);
        break;
    case TickBehaviour::MobSpawner:
        // `bj` has no `onBlockAdded` of its own; this is `jt.e` -- the
        // BlockContainer one -- building a `bd` and handing it to the world.
        // A spawner put down by hand is `"Pig"` on a twenty-tick delay, and
        // a1.1.2 gives the player no way to change either.
        world.addTileEntity(x, y, z);
        break;
    case TickBehaviour::Stairs:
        // `km.e(Lcn;III)V` -- onNeighborBlockChange(0), then the model
        // block's onBlockAdded, which for planks and cobblestone is empty.
        stairsNeighbourChanged(world, x, y, z, self);
        break;
    default:
        // `ly.e(Lcn;III)V` is empty, and so is this for everything else.
        break;
    }
}

namespace {

bool isChest(const TickWorld& world, i32 x, int y, i32 z)
{
    return block::def(world.blockAt(x, y, z)).tick == TickBehaviour::Chest;
}

// `b.a(Lcn;IIILdm;)Z` -- **BlockChest.blockActivated**. A chest with an opaque
// block on it does not open, and neither does one beside a chest that has one;
// the click is taken either way. Otherwise the screen opens on this chest, and
// `chestInventoryParts` is what it joins to.
bool chestActivated(TickWorld& world, i32 x, int y, i32 z)
{
    if (world.opaqueAt(x, y + 1, z)) {
        return true;
    }
    constexpr int kSides[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& side : kSides) {
        const i32 nx = x + side[0];
        const i32 nz = z + side[1];
        if (isChest(world, nx, y, nz) && world.opaqueAt(nx, y + 1, nz)) {
            return true;
        }
    }
    world.openContainer(TickWorld::ContainerKind::Chest, x, y, z);
    return true;
}

}  // namespace

int chestInventoryParts(const TickWorld& world, i32 x, int y, i32 z,
                        ChestPart out[kMaxChestParts])
{
    // `hs` nests: the -x and -z neighbours are put *before* what has been built
    // so far, the +x and +z ones after, in that order of asking.
    ChestPart parts[kMaxChestParts];
    int count = 0;
    parts[count++] = ChestPart{x, y, z};
    const auto prepend = [&](i32 px, i32 pz) {
        for (int i = count; i > 0; --i) {
            parts[i] = parts[i - 1];
        }
        parts[0] = ChestPart{px, y, pz};
        ++count;
    };
    const auto append = [&](i32 px, i32 pz) { parts[count++] = ChestPart{px, y, pz}; };
    if (isChest(world, x - 1, y, z)) {
        prepend(x - 1, z);
    }
    if (isChest(world, x + 1, y, z)) {
        append(x + 1, z);
    }
    if (isChest(world, x, y, z - 1)) {
        prepend(x, z - 1);
    }
    if (isChest(world, x, y, z + 1)) {
        append(x, z + 1);
    }
    for (int i = 0; i < count; ++i) {
        out[i] = parts[i];
    }
    return count;
}

bool blockActivated(TickWorld& world, i32 x, int y, i32 z)
{
    const BlockId self = world.blockAt(x, y, z);
    if (self == kAir) {
        return false;
    }

    // Dispatched on the behaviour column, never on an id -- the same rule the
    // rest of this file follows, and the reason a version whose lever is a
    // different number needs no edit here.
    switch (block::def(self).tick) {
    case TickBehaviour::Door:
        return doorActivated(world, x, y, z, self);
    case TickBehaviour::Lever:
        return leverActivated(world, x, y, z, self);
    case TickBehaviour::Button:
        return buttonActivated(world, x, y, z, self);
    case TickBehaviour::RedstoneOre:
        redstoneOreActivated(world, x, y, z, self);
        return false;  // and that `false` is the original's -- see the header
    case TickBehaviour::Chest:
        return chestActivated(world, x, y, z);
    case TickBehaviour::Workbench:
        // `cs.a(Lcn;IIILdm;)Z` -- `player.displayWorkbenchGUI(); return true;`
        world.openContainer(TickWorld::ContainerKind::Workbench, x, y, z);
        return true;
    case TickBehaviour::Furnace:
        // `ku.a(Lcn;IIILdm;)Z` -- the screen on this furnace's tile entity.
        world.openContainer(TickWorld::ContainerKind::Furnace, x, y, z);
        return true;
    default:
        // `ly.a(Lcn;IIILdm;)Z` returns false, and so does a staircase, which
        // forwards to the block it is modelled on.
        return false;
    }
}

namespace {

// `ng.b(Lcn;III)V` -- **BlockSponge.onBlockRemoval**, which is the only thing
// a sponge does in a1.1.2.
//
// The absorption everyone remembers is not in this version. `ng.e` --
// onBlockAdded -- walks the same 5 x 5 x 5 box, compares each cell's material
// against water, and **the body of that comparison is empty**: the branch
// target is the next instruction. The removal below is a real method with real
// work in it, and it is all there is. So a sponge here is a decorative block
// that wakes its neighbours when it goes, which is what a1.1.2 ships.
void spongeRemoved(TickWorld& world, i32 x, int y, i32 z)
{
    constexpr int kReach = 2;
    for (i32 bx = x - kReach; bx <= x + kReach; ++bx) {
        for (int by = y - kReach; by <= y + kReach; ++by) {
            for (i32 bz = z - kReach; bz <= z + kReach; ++bz) {
                // `world.notifyBlocksOfNeighborChange(i, j, k, world.getBlockId(i, j, k))`
                // -- the *cell's own* id as the "what changed" argument, which
                // is unusual and is the class file's.
                world.notifyNeighbours(bx, by, bz, world.blockAt(bx, by, bz));
            }
        }
    }
}

// `b.b(Lcn;III)V` -- **BlockChest.onBlockRemoval**: every stack comes out, in
// slot order, from one random point inside the cell per stack and in clumps of
// 10 to 30, each clump thrown on a small Gaussian with a lift of 0.2 -- and then
// `jt.b` forgets the tile entity.
//
// **The draws come off a generator of the chest's own**, because the original's
// does: `b` holds a `new Random()` and never touches `World.rand`, so a spill
// must not move the block-tick stream. It also stands in for the new entity's
// own generator, which the Gaussians come from there.
void chestRemoved(TickWorld& world, i32 x, int y, i32 z)
{
    std::vector<world::TileEntity>* list = world.tileEntitiesAt(x, z);
    if (list == nullptr) {
        return;
    }
    const world::TileEntity* tile = world::findTileEntity(*list, x, y, z);
    if (tile != nullptr && tile->kind == world::TileEntityKind::Chest) {
        static JavaRandom spill(0x6368657374LL);
        for (int slot = 0; slot < world::kChestSlots; ++slot) {
            for (const item::ItemStack& stack : tile->items) {
                if (stack.slot != slot || stack.empty()) {
                    continue;
                }
                const float fx = spill.nextFloat() * 0.8f + 0.1f;
                const float fy = spill.nextFloat() * 0.8f + 0.1f;
                const float fz = spill.nextFloat() * 0.8f + 0.1f;
                int left = stack.count;
                while (left > 0) {
                    int clump = spill.nextInt(21) + 10;
                    if (clump > left) {
                        clump = left;
                    }
                    left -= clump;
                    const double mx = double(float(spill.nextGaussian()) * 0.05f);
                    const double my = double(float(spill.nextGaussian()) * 0.05f + 0.2f);
                    const double mz = double(float(spill.nextGaussian()) * 0.05f);
                    world.spawnItemStack(double(float(x) + fx), double(float(y) + fy),
                                         double(float(z) + fz), u16(stack.id), clump,
                                         stack.damage, mx, my, mz);
                }
            }
        }
    }
    world::eraseTileEntity(*list, x, y, z);
}

}  // namespace

void blockRemoved(TickWorld& world, i32 x, int y, i32 z, BlockId old)
{
    switch (block::def(old).tick) {
    case TickBehaviour::RedstoneWire:
        wireRemoved(world, x, y, z, old);
        break;
    case TickBehaviour::RedstoneTorch:
        redstoneTorchRemoved(world, x, y, z, old);
        break;
    case TickBehaviour::Sponge:
        spongeRemoved(world, x, y, z);
        break;
    case TickBehaviour::SignPost:
    case TickBehaviour::SignWall:
    case TickBehaviour::MobSpawner:
        // `lr` and `bj` both inherit `jt.b` -- BlockContainer.onBlockRemoval --
        // which is `super.b` (empty) and then `world.removeBlockTileEntity`.
        // Here, and not in the break path, so that a sign whose support goes
        // forgets its text the same way a sign the player breaks does, and a
        // spawner blown up by a creeper forgets its mob.
        world.removeTileEntity(x, y, z);
        break;
    case TickBehaviour::Chest:
        chestRemoved(world, x, y, z);
        break;
    case TickBehaviour::Furnace:
        // `ku` has no `onBlockRemoval` of its own: `jt.b` forgets the entry and
        // **nothing in it is spilled** -- a1.1.2 loses a broken furnace's
        // contents. The lit/unlit swap goes through here too and puts the entry
        // back itself; see core/tick/furnace.cpp.
        if (std::vector<world::TileEntity>* list = world.tileEntitiesAt(x, z)) {
            world::eraseTileEntity(*list, x, y, z);
        }
        break;
    default:
        // `ly.b(Lcn;III)V` is empty, and so is this for everything else.
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
    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
        switchNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Door:
        doorNeighbourChanged(world, x, y, z, self, fromId);
        break;
    case TickBehaviour::Rail:
        railNeighbourChanged(world, x, y, z, self, fromId);
        break;
    case TickBehaviour::Ladder:
        ladderNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::Stairs:
        stairsNeighbourChanged(world, x, y, z, self);
        break;
    case TickBehaviour::SignPost:
    case TickBehaviour::SignWall:
        signNeighbourChanged(world, x, y, z, self);
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
    case TickBehaviour::Tnt:
        // `q.a(Lcn;IIIII)V`: powered TNT lights itself. The fourth and last of
        // a1.1.2's four ignition paths -- see core/tick/redstone.hpp.
        tntNeighbourChanged(world, x, y, z, fromId);
        break;
    default:
        break;
    }
}

namespace {

// `ly.b(Lcn;IIILkh;)V` -- onEntityCollidedWithBlock. Empty on the base class,
// overridden by exactly one block in a1.1.2.
//
// **The entity is not a parameter, and that is the original's.** `al.b` throws
// its `kh` away and asks the world what is in the box instead, so an item
// bumping a stone plate makes the plate look for mobs, find none, and do
// nothing -- rather than the item being filtered out before it gets here.
void entityCollidedWithBlock(TickWorld& world, i32 x, int y, i32 z, block::BlockId self)
{
    switch (block::def(self).tick) {
    case TickBehaviour::PressurePlateAll:
    case TickBehaviour::PressurePlateMobs:
        pressurePlateCollided(world, x, y, z, self);
        break;
    default:
        break;
    }
}

}  // namespace

void entityCollidedWithBlocks(TickWorld& world, const AABB& box)
{
    const i32 x0 = MathHelper::floorDouble(box.minX);
    const int y0 = MathHelper::floorDouble(box.minY);
    const i32 z0 = MathHelper::floorDouble(box.minZ);
    const i32 x1 = MathHelper::floorDouble(box.maxX);
    const int y1 = MathHelper::floorDouble(box.maxY);
    const i32 z1 = MathHelper::floorDouble(box.maxZ);

    for (i32 x = x0; x <= x1; ++x) {
        for (int y = y0; y <= y1; ++y) {
            for (i32 z = z0; z <= z1; ++z) {
                // `if (l > 0)`, which is "not air" and also "not a hole in the
                // block table" -- both come back as id 0 here.
                const block::BlockId self = world.blockAt(x, y, z);
                if (self == block::kAir) {
                    continue;
                }
                entityCollidedWithBlock(world, x, y, z, self);
            }
        }
    }
}

void entityWalkedOnBlock(TickWorld& world, i32 x, int y, i32 z)
{
    // `if (l > 0)` guards the call in `moveEntity` and the cell is read again
    // here, one step later than the mover read it: the same re-read
    // `entityCollidedWithBlocks` does, and for the same reason -- the block
    // may have gone in between, and a behaviour must not be dispatched off a
    // stale id.
    const block::BlockId self = world.blockAt(x, y, z);
    if (self == block::kAir) return;

    switch (block::def(self).tick) {
    case TickBehaviour::Farmland:
        farmlandTrampled(world, x, y, z);
        break;
    case TickBehaviour::RedstoneOre:
        // `ai.a(Lcn;IIILkh;)V` is `glow()` and then the base class, which is
        // the same `glow()` a right-click runs -- and it is already written,
        // including the "only the unlit one" test inside it.
        redstoneOreActivated(world, x, y, z, self);
        break;
    default:
        // Everything else inherits `ly`'s empty override. A staircase forwards
        // to the block it is modelled on -- planks and cobblestone, both of
        // which inherit it too -- so it is no exception either.
        break;
    }
}

}  // namespace mc::tick
