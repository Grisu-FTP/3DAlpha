#include "core/tick/fluid.hpp"

#include "core/block/registry.hpp"
#include "core/tick/fire.hpp"
#include "core/tick/tick_world.hpp"

namespace mc::tick {

namespace {

using block::BlockDef;
using block::BlockId;
using block::TickBehaviour;

constexpr BlockId kAir = block::kAir;

BlockId id(mcver::Block b) { return BlockId(b); }

// a1.1.2 compares `Material` object identity. Ours is a dense index generated
// from the same field, so identity becomes equality -- and air's index is 0,
// which no constructed block takes, so "is this air the same material as
// water" is false without a special case.
u8 materialOf(BlockId b) { return block::def(b).material; }

bool sameMaterial(BlockId a, BlockId b) { return materialOf(a) == materialOf(b); }

bool isWaterMaterial(BlockId b) { return materialOf(b) == materialOf(id(mcver::Block::Water)); }
bool isLavaMaterial(BlockId b) { return materialOf(b) == materialOf(id(mcver::Block::Lava)); }

// `jp.<init>`: `this.d = 1; if (material == lava) this.d = 2;` -- how much a
// level drops per block travelled, and the whole reason lava reaches four
// blocks where water reaches seven.
int decayPerBlock(BlockId self) { return isLavaMaterial(self) ? 2 : 1; }

// The pair. a1.1.2 really does write `blockID + 1` and `blockID - 1`: flowing
// and still are registered adjacently on purpose. Reproducing the arithmetic
// keeps the two in step without a second table, and `pairedStill` checks the
// neighbour it lands on actually is this fluid's still form, so a version that
// numbered them differently fails loudly here rather than quietly everywhere.
BlockId pairedStill(BlockId flowing)
{
    const BlockId still = BlockId(flowing + 1);
    const BlockDef& d = block::def(still);
    if (d.tick != TickBehaviour::FluidStill || !sameMaterial(still, flowing)) return flowing;
    return still;
}

BlockId pairedFlowing(BlockId still)
{
    if (still == 0) return still;
    const BlockId flowing = BlockId(still - 1);
    const BlockDef& d = block::def(flowing);
    if (d.tick != TickBehaviour::FluidFlowing || !sameMaterial(flowing, still)) return still;
    return flowing;
}

// `jp.h(Lcn;III)I` -- getFlowDecay. -1 means "not my fluid", which is a
// different answer from "level 0", and conflating the two floods the world.
int flowDecay(const TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const BlockId there = world.blockAt(x, y, z);
    if (!sameMaterial(there, self)) return -1;
    return int(world.dataAt(x, y, z));
}

// `hv.l(Lcn;III)Z` -- blocksFlow. Five blocks stop a fluid without being solid
// -- both doors, a sign post, a ladder and sugar cane -- and a1.1.2 names them
// one by one rather than deriving them, so this does too.
bool blocksFlow(const TickWorld& world, i32 x, int y, i32 z)
{
    const BlockId there = world.blockAt(x, y, z);
    if (there == id(mcver::Block::WoodenDoor) || there == id(mcver::Block::IronDoor) ||
        there == id(mcver::Block::SignPost) || there == id(mcver::Block::Ladder) ||
        there == id(mcver::Block::SugarCane)) {
        return true;
    }
    if (there == kAir) return false;
    return block::def(there).solid;
}

// `hv.m(Lcn;III)Z` -- canFlowInto. Never into its own kind, never into lava
// (water quenching lava is the *lava's* business, in checkForHarden), and never
// into anything that blocks flow.
bool canFlowInto(const TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const BlockId there = world.blockAt(x, y, z);
    if (sameMaterial(there, self)) return false;
    if (isLavaMaterial(there)) return false;
    return !blocksFlow(world, x, y, z);
}

// `hv.f(Lcn;IIII)I` -- getSmallestFlowDecay, which also counts the sources it
// passes. The count is what makes two sources beside each other produce a
// third, so it is an output of this function and not a side effect.
int smallestFlowDecay(const TickWorld& world, i32 x, int y, i32 z, int best, BlockId self,
                      int* adjacentSources)
{
    int decay = flowDecay(world, x, y, z, self);
    if (decay < 0) return best;
    if (decay == 0) ++*adjacentSources;
    if (decay >= 8) decay = 0;
    return (best >= 0 && decay >= best) ? best : decay;
}

// The four horizontal directions, in a1.1.2's own numbering: 0 is -x, 1 is +x,
// 2 is -z, 3 is +z. The numbering is load-bearing because the flow search
// refuses to double back, and "the opposite of 0 is 1" is how it knows.
constexpr i32 kDx[4] = {-1, 1, 0, 0};
constexpr i32 kDz[4] = {0, 0, -1, 1};
constexpr int kOpposite[4] = {1, 0, 3, 2};

// `hv.a(Lcn;IIIII)I` -- calculateFlowCost. How many blocks along this
// direction before the fluid could fall, up to four; 1000 for "not within
// four".
int flowCost(const TickWorld& world, i32 x, int y, i32 z, int cost, int cameFrom, BlockId self)
{
    int lowest = 1000;

    for (int d = 0; d < 4; ++d) {
        if (d == kOpposite[cameFrom]) continue;  // never search back the way we came

        const i32 bx = x + kDx[d];
        const i32 bz = z + kDz[d];

        if (blocksFlow(world, bx, y, bz)) continue;
        // A source of our own kind is not somewhere to flow to.
        if (sameMaterial(world.blockAt(bx, y, bz), self) && world.dataAt(bx, y, bz) == 0) continue;

        // Somewhere to fall: this direction costs what we have spent to reach it.
        if (!blocksFlow(world, bx, y - 1, bz)) return cost;

        if (cost >= 4) continue;
        const int c = flowCost(world, bx, y, bz, cost + 1, d, self);
        if (c < lowest) lowest = c;
    }

    return lowest;
}

// `hv.k(Lcn;III)[Z` -- getOptimalFlowDirections.
void optimalFlowDirections(const TickWorld& world, i32 x, int y, i32 z, BlockId self,
                           bool out[4])
{
    int cost[4];

    for (int d = 0; d < 4; ++d) {
        cost[d] = 1000;

        const i32 bx = x + kDx[d];
        const i32 bz = z + kDz[d];

        if (blocksFlow(world, bx, y, bz)) continue;
        if (sameMaterial(world.blockAt(bx, y, bz), self) && world.dataAt(bx, y, bz) == 0) continue;

        if (!blocksFlow(world, bx, y - 1, bz)) {
            cost[d] = 0;  // a hole right there beats anything further away
            continue;
        }
        cost[d] = flowCost(world, bx, y, bz, 1, d, self);
    }

    int lowest = cost[0];
    for (int d = 1; d < 4; ++d) {
        if (cost[d] < lowest) lowest = cost[d];
    }
    // Every direction tied for the lowest cost, so water reaching a hole two
    // ways goes both ways.
    for (int d = 0; d < 4; ++d) out[d] = (cost[d] == lowest);
}

// `hv.g(Lcn;IIII)V` -- flowIntoBlock. Whatever was there is destroyed; a1.1.2
// drops it as an item and, for lava, plays the quench. Neither exists yet.
void flowIntoBlock(TickWorld& world, i32 x, int y, i32 z, int level, BlockId self)
{
    if (!canFlowInto(world, x, y, z, self)) return;
    // `Block.dropBlockAsItem` for water, `triggerLavaMixEffects` for lava --
    // an item entity and a sound, and this port has neither. The block is
    // destroyed either way, which is the part the world sees.
    world.setBlockAndDataWithNotify(x, y, z, self, u8(level));
}

// `hv.j(Lcn;III)V` -- setStatic: become the still form, keeping the level.
// **No neighbour notification**, which is not an omission: notifying here would
// wake the neighbours, which would set them not-static, which would notify
// back. The original marks the block for redraw and nothing else.
void setStatic(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const u8 level = world.dataAt(x, y, z);
    const BlockId still = pairedStill(self);
    if (still == self) return;
    world.setBlockRaw(x, y, z, still);
    world.setDataRaw(x, y, z, level);
}

// `hn.j(Lcn;III)V` -- setNotStationary: become the flowing form and ask to be
// ticked. The original wraps this in `editingBlocks = true`, which suppresses
// neighbour notification; ours uses the raw setters, which never notify, so the
// flag has nothing to do here.
void setNotStationary(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    const u8 level = world.dataAt(x, y, z);
    const BlockId flowing = pairedFlowing(self);
    if (flowing == self) return;
    world.setBlockRaw(x, y, z, flowing);
    world.setDataRaw(x, y, z, level);
    world.scheduleBlockUpdate(x, y, z, flowing);
}

// `jp.j(Lcn;III)V` -- checkForHarden. Lava beside or above water turns to
// stone: obsidian at a source, cobblestone at any level up to 4. Note there is
// no check below -- lava sitting on water is not quenched.
void checkForHarden(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    if (world.blockAt(x, y, z) != self) return;
    if (!isLavaMaterial(self)) return;

    const bool water = isWaterMaterial(world.blockAt(x, y, z - 1)) ||
                       isWaterMaterial(world.blockAt(x, y, z + 1)) ||
                       isWaterMaterial(world.blockAt(x - 1, y, z)) ||
                       isWaterMaterial(world.blockAt(x + 1, y, z)) ||
                       isWaterMaterial(world.blockAt(x, y + 1, z));
    if (!water) return;

    const int level = int(world.dataAt(x, y, z));
    if (level == 0) {
        world.setBlockWithNotify(x, y, z, id(mcver::Block::Obsidian));
    } else if (level <= 4) {
        world.setBlockWithNotify(x, y, z, id(mcver::Block::Cobblestone));
    }
    // `jp.i` is the fizz and the smoke, which are audio and particles.
}

// `hn.k(Lcn;III)Z` folded over the six neighbours: does anything around this
// cell have a burnable *material*?
bool burnableNeighbour(const TickWorld& world, i32 x, int y, i32 z)
{
    return block::def(world.blockAt(x - 1, y, z)).canBurn ||
           block::def(world.blockAt(x + 1, y, z)).canBurn ||
           block::def(world.blockAt(x, y, z - 1)).canBurn ||
           block::def(world.blockAt(x, y, z + 1)).canBurn ||
           block::def(world.blockAt(x, y - 1, z)).canBurn ||
           block::def(world.blockAt(x, y + 1, z)).canBurn;
}

}  // namespace

void fluidFlowingTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    int level = flowDecay(world, x, y, z, self);
    const int step = decayPerBlock(self);
    bool settle = true;

    if (level > 0) {
        int best = -100;
        int adjacentSources = 0;
        best = smallestFlowDecay(world, x - 1, y, z, best, self, &adjacentSources);
        best = smallestFlowDecay(world, x + 1, y, z, best, self, &adjacentSources);
        best = smallestFlowDecay(world, x, y, z - 1, best, self, &adjacentSources);
        best = smallestFlowDecay(world, x, y, z + 1, best, self, &adjacentSources);

        int next = best + step;
        if (next >= 8 || best < 0) next = -1;

        // Anything falling from above overrides the horizontal answer: a
        // column of falling water is level 8+ all the way down.
        const int above = flowDecay(world, x, y + 1, z, self);
        if (above >= 0) next = above >= 8 ? above : above + 8;

        // **Infinite water.** Two source neighbours make a third source, but
        // only over something that holds it up: an opaque cube, or more of the
        // same fluid that is itself a source.
        if (adjacentSources >= 2 && isWaterMaterial(self)) {
            if (world.opaqueAt(x, y - 1, z)) {
                next = 0;
            } else if (sameMaterial(world.blockAt(x, y - 1, z), self) &&
                       world.dataAt(x, y, z) == 0) {
                next = 0;
            }
        }

        // Lava only *thins* three times in four. Water settles at its tick
        // rate; lava crawls, and this is where the difference lives -- not in
        // the tick rate, which is already 30 against water's 5.
        if (isLavaMaterial(self) && level < 8 && next < 8 && next > level &&
            rand.nextInt(4) != 0) {
            next = level;
            settle = false;
        }

        if (next != level) {
            level = next;
            if (level < 0) {
                world.setBlockWithNotify(x, y, z, kAir);
            } else {
                world.setDataRaw(x, y, z, u8(level));
                world.scheduleBlockUpdate(x, y, z, self);
                world.notifyNeighbours(x, y, z, self);
            }
        } else if (settle) {
            setStatic(world, x, y, z, self);
        }
    } else {
        // A source has nothing to recompute, so it becomes still immediately
        // and stops costing anything. This is why an ocean is free.
        setStatic(world, x, y, z, self);
    }

    // Downwards first and unconditionally: a fluid that can fall does not
    // spread sideways at all.
    if (canFlowInto(world, x, y - 1, z, self)) {
        world.setBlockAndDataWithNotify(x, y - 1, z, self,
                                        u8(level >= 8 ? level : level + 8));
        return;
    }

    if (level < 0) return;
    // A source spreads sideways; anything else only does so when it is
    // standing on something.
    if (level != 0 && !blocksFlow(world, x, y - 1, z)) return;

    bool dirs[4];
    optimalFlowDirections(world, x, y, z, self, dirs);

    int outLevel = level + step;
    if (level >= 8) outLevel = 1;  // falling water spreads at full strength
    if (outLevel >= 8) return;

    for (int d = 0; d < 4; ++d) {
        if (dirs[d]) flowIntoBlock(world, x + kDx[d], y, z + kDz[d], outLevel, self);
    }
}

void fluidStillTick(TickWorld& world, i32 x, int y, i32 z, BlockId self, JavaRandom& rand)
{
    // Water's still form does not tick randomly at all -- `BlockStationary`'s
    // constructor turns that off and turns it back on only for lava.
    if (!isLavaMaterial(self)) return;

    const int tries = rand.nextInt(3);
    for (int i = 0; i < tries; ++i) {
        // x and z wander, y climbs by one every step. Lava therefore lights
        // things above it, in a widening cone.
        x += rand.nextInt(3) - 1;
        ++y;
        z += rand.nextInt(3) - 1;

        const BlockId there = world.blockAt(x, y, z);
        if (there == kAir) {
            // `hn.k(Lcn;III)Z` is `Material.getCanBurn()` -- the **wider**
            // fourteen-block set, not the fire tables. A chest or a fence
            // catches from lava and is in neither of BlockFire's arrays.
            if (burnableNeighbour(world, x, y, z)) {
                world.setBlockWithNotify(x, y, z, id(mcver::Block::Fire));
                return;
            }
        } else if (block::def(there).solid) {
            return;
        }
    }
}

void fluidNeighbourChanged(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    checkForHarden(world, x, y, z, self);

    // `hn.a(Lcn;IIII)V`: a still block woken by a neighbour goes back to
    // flowing and schedules itself. Harden may have replaced it, so the id is
    // re-read rather than assumed -- which is the original's own check.
    if (block::def(self).tick != TickBehaviour::FluidStill) return;
    if (world.blockAt(x, y, z) != self) return;
    setNotStationary(world, x, y, z, self);
}

void fluidPlaced(TickWorld& world, i32 x, int y, i32 z, BlockId self)
{
    checkForHarden(world, x, y, z, self);
    if (world.blockAt(x, y, z) != self) return;
    if (block::def(self).tick == TickBehaviour::FluidFlowing) {
        world.scheduleBlockUpdate(x, y, z, self);
    }
}

}  // namespace mc::tick
