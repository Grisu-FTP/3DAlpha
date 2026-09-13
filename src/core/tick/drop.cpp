// `dropBlockAsItem`, and the generated table under it. See drop.hpp.

#include "core/tick/drop.hpp"

#include "core/entity/primed_tnt.hpp"
#include "core/tick/tick_world.hpp"

#include "drops.hpp"  // generated; see tools/configure.py
#include "items.hpp"  // generated; see tools/configure.py

namespace mc::tick {
namespace {

const mcver::BlockDrop& dropRule(block::BlockId id)
{
    // A block past the table -- which cannot happen for a world this build
    // wrote, and can for one it read -- drops nothing rather than dropping
    // itself. Inventing an item for an id we have no definition of is the one
    // answer that could put a block into a save that does not exist.
    static constexpr mcver::BlockDrop kNothing{0, 0, 0, 0, 0, 0, -1};
    return id < mcver::kDropTableSize ? mcver::kBlockDrops[id] : kNothing;
}

// `Block.quantityDropped(Random)`, replayed from the three columns that
// characterise it. The draw order is the method's: a spread and a one-in-N are
// never both present, so at most one `nextInt` happens here.
int quantityDropped(const mcver::BlockDrop& rule, JavaRandom& rand)
{
    if (rule.countSpread != 0) {
        return int(rule.countMin) + rand.nextInt(int(rule.countSpread));
    }
    if (rule.countOneIn != 0) {
        return rand.nextInt(int(rule.countOneIn)) == 0 ? int(rule.countMin) : 0;
    }
    return int(rule.countMin);
}

// `Block.idDropped(int, Random)`. The roll comes first in the one block that
// has one -- gravel -- because the class file's first statement is the
// `nextInt`, and a draw skipped is a different world downstream.
u16 idDropped(const mcver::BlockDrop& rule, u8 metadata, JavaRandom& rand)
{
    if (rule.altOneIn != 0) {
        return rand.nextInt(int(rule.altOneIn)) == 0 ? rule.altItem : rule.item;
    }
    if (rule.metadataRow >= 0) {
        return mcver::kDropByMetadata[rule.metadataRow][metadata & 15];
    }
    return rule.item;
}

}  // namespace

void dropBlockAsItem(TickWorld& world, i32 x, int y, i32 z, block::BlockId self, u8 metadata,
                     float chance)
{
    const mcver::BlockDrop& rule = dropRule(self);
    JavaRandom& rand = world.random();

    const int count = quantityDropped(rule, rand);
    for (int n = 0; n < count; ++n) {
        // `if (world.rand.nextFloat() > f) continue;`. At the 1.0 every caller
        // but the explosion passes it never continues -- and still costs its
        // draw, which is why it is written out rather than folded away: folding
        // it away is a different random stream. At the explosion's 0.3 it is
        // the whole of "most of what a creeper takes down is gone".
        const float roll = rand.nextFloat();
        if (roll > chance) {
            continue;
        }

        const u16 id = idDropped(rule, metadata, rand);
        if (id == 0) {
            continue;
        }

        // `float f = 0.7F;` and the same expression three times: a point
        // somewhere in the middle 70 % of the cell.
        constexpr float kSpread = 0.7f;
        constexpr double kEdge = double(1.0f - kSpread) * 0.5;
        const double dx = double(rand.nextFloat() * kSpread) + kEdge;
        const double dy = double(rand.nextFloat() * kSpread) + kEdge;
        const double dz = double(rand.nextFloat() * kSpread) + kEdge;

        world.spawnItem(double(x) + dx, double(y) + dy, double(z) + dz, id, 1);
    }
}

void blockDestroyedByPlayer(TickWorld& world, i32 x, int y, i32 z, block::BlockId self,
                            u8 metadata)
{
    // Dispatch is on the behaviour, never on the id, for the reason
    // behaviour.hpp gives -- and the switch is this short because a1.1.2 has
    // only three overrides and one of them does nothing here. See drop.hpp.
    if (block::def(self).tick == block::TickBehaviour::Tnt) {
        tntDestroyedByPlayer(world, x, y, z);
        return;
    }
    if (block::def(self).tick != block::TickBehaviour::Crops) {
        return;
    }

    // `hd.b(Lcn;IIII)V`:
    //
    // ```
    // for (int i1 = 0; i1 < 3; i1++) {
    //     if (world.rand.nextInt(15) > l) continue;
    //     float f = 0.7F;
    //     float f1 = world.rand.nextFloat() * f + (1.0F - f) * 0.5F;   // and y, z
    //     EntityItem e = new EntityItem(world, i + f1, j + f2, k + f3,
    //                                   new ItemStack(Item.seeds));
    //     e.delayBeforeCanPickup = 10;
    //     world.spawnEntityInWorld(e);
    // }
    // ```
    //
    // **The roll is `nextInt(15) <= metadata`**, so each of the three passes
    // succeeds (stage + 1) times in fifteen: one in fifteen for a crop planted
    // this tick, eight in fifteen for a ripe one -- 0.2 and 1.6 seeds on
    // average. A ripe crop is the most generous, not a certain three. That is
    // one draw per iteration whichever way it lands, and three more when it
    // lands.
    //
    // **The offsets are float the whole way down**, unlike `dropBlockAsItem`'s,
    // which widens each term before adding: here the block coordinate is
    // widened to float and the sum is taken there. It is a different number in
    // the last few bits and it is what the class file does.
    JavaRandom& rand = world.random();
    for (int n = 0; n < 3; ++n) {
        if (rand.nextInt(15) > int(metadata)) {
            continue;
        }
        constexpr float kSpread = 0.7f;
        constexpr float kEdge = (1.0f - kSpread) * 0.5f;
        const float ox = rand.nextFloat() * kSpread + kEdge;
        const float oy = rand.nextFloat() * kSpread + kEdge;
        const float oz = rand.nextFloat() * kSpread + kEdge;
        world.spawnItem(double(float(x) + ox), double(float(y) + oy), double(float(z) + oz),
                        u16(mcver::Item::Seeds), 1);
    }
}


void tntDestroyedByPlayer(TickWorld& world, i32 x, int y, i32 z)
{
    world.spawnPrimedTnt(x, y, z, entity::kPrimedTntFuse);
    // `playSoundAtEntity`, which is `posY - yOffset` -- see drop.hpp.
    world.playSoundAt("random.fuse", double(x) + 0.5,
                      double(y) + 0.5 - entity::kPrimedTntHalf, double(z) + 0.5, 1.0f,
                      1.0f);
}

void tntDestroyedByExplosion(TickWorld& world, i32 x, int y, i32 z)
{
    // `world.rand.nextInt(tnt.fuse / 4) + tnt.fuse / 8` on a fuse the
    // constructor has just set to 80, and the draw is made either way.
    const int fuse = world.random().nextInt(entity::kPrimedTntRelitSpread)
                     + entity::kPrimedTntRelitFloor;
    world.spawnPrimedTnt(x, y, z, fuse);
}

}  // namespace mc::tick
