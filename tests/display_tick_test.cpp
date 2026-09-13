// `Block.randomDisplayTick` and the thousand darts that drive it: which blocks
// answer, what each one throws, and the four lit/unlit pairs where the answer
// depends on which of the two ids it is.
//
// The positions inside a block are random in the jar too, so what is pinned
// here is the count, the kind, and the cell the particles end up in.

#include "core/block/registry.hpp"
#include "core/entity/particle.hpp"
#include "core/tick/display.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::entity::ParticleKind;
using mc::entity::ParticleSystem;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

struct Scene {
    SceneWorld world{0, 0};
    ParticleSystem fx{1234};
    JavaRandom rand{4321};

    Scene()
    {
        for (i32 x = -2; x <= 2; ++x) {
            for (i32 z = -2; z <= 2; ++z) {
                world.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
        entity::bindParticles(world.w(), &fx);
    }

    // One `randomDisplayTick` on the block at (0, 64, 0), whatever it is.
    void run()
    {
        tick::blockDisplayTick(world.w(), 0, 64, 0, world.w().blockAt(0, 64, 0), rand);
    }

    int countOf(ParticleKind kind) const
    {
        int n = 0;
        for (int i = 0; i < fx.count(); ++i) {
            n += fx[i].kind == kind ? 1 : 0;
        }
        return n;
    }
};

}  // namespace

TEST(a_torch_throws_one_smoke_and_one_flame)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Torch), 5);
    s.run();
    CHECK_EQ(s.fx.count(), 2);
    CHECK_EQ(s.countOf(ParticleKind::Smoke), 1);
    CHECK_EQ(s.countOf(ParticleKind::Flame), 1);

    // `mj` uses the block centre exactly for a floor torch -- the jitter is
    // `bg`'s and not its -- and puts the tip 0.7 of a block up.
    for (int i = 0; i < s.fx.count(); ++i) {
        CHECK_EQ(s.fx[i].x, 64.5 - 64.0);
        CHECK_EQ(s.fx[i].z, 0.5);
        CHECK(s.fx[i].y > 64.6 && s.fx[i].y < 64.8);
    }
}

TEST(a_wall_torch_smokes_from_its_tip_and_not_its_cell)
{
    Scene s;
    // Metadata 1 is "on the wall to the west", so the stick leans east and the
    // flame sits 0.27 that way and 0.22 up.
    s.world.place(0, 64, 0, bid(mcver::Block::Torch), 1);
    s.run();
    CHECK_EQ(s.fx.count(), 2);
    for (int i = 0; i < s.fx.count(); ++i) {
        CHECK(s.fx[i].x < 0.3);
        CHECK(s.fx[i].y > 64.9);
    }
}

TEST(only_the_lit_redstone_torch_glitters_and_it_makes_no_flame)
{
    Scene lit;
    lit.world.place(0, 64, 0, bid(mcver::Block::RedstoneTorch), 5);
    lit.run();
    CHECK_EQ(lit.fx.count(), 1);
    CHECK_EQ(lit.countOf(ParticleKind::Reddust), 1);

    // `bg`'s `this.a` is a constructor argument: 76 is on and 75 is off, and
    // an off torch's display tick returns without drawing anything.
    Scene unlit;
    unlit.world.place(0, 64, 0, bid(mcver::Block::UnlitRedstoneTorch), 5);
    unlit.run();
    CHECK_EQ(unlit.fx.count(), 0);
}

TEST(only_a_lit_furnace_burns_and_only_when_it_faces_somewhere)
{
    Scene lit;
    lit.world.place(0, 64, 0, bid(mcver::Block::LitFurnace), 3);
    lit.run();
    CHECK_EQ(lit.fx.count(), 2);
    CHECK_EQ(lit.countOf(ParticleKind::Smoke), 1);
    CHECK_EQ(lit.countOf(ParticleKind::Flame), 1);

    Scene cold;
    cold.world.place(0, 64, 0, bid(mcver::Block::Furnace), 3);
    cold.run();
    CHECK_EQ(cold.fx.count(), 0);

    // **Four branches and no default.** A lit furnace whose metadata is not
    // one of the four facings throws nothing, which is the jar's own hole and
    // not one left here.
    //
    // The 0 has to be written *after* the placement: `onBlockAdded` turns a
    // furnace to face its neighbours, so a placed one never has metadata 0 and
    // this state is only reachable in a hand-edited world.
    Scene facingNowhere;
    facingNowhere.world.place(0, 64, 0, bid(mcver::Block::LitFurnace), 0);
    facingNowhere.world.w().setDataRaw(0, 64, 0, 0);
    facingNowhere.run();
    CHECK_EQ(facingNowhere.fx.count(), 0);
}

TEST(a_redstone_wire_glitters_only_while_it_is_carrying_something)
{
    Scene dark;
    dark.world.place(0, 64, 0, bid(mcver::Block::RedstoneWire), 0);
    dark.run();
    CHECK_EQ(dark.fx.count(), 0);

    // The level has to be written after the placement for the same reason the
    // furnace's facing does: `onBlockAdded` recomputes a wire's power from what
    // is around it, and nothing here is powering this one.
    Scene live;
    live.world.place(0, 64, 0, bid(mcver::Block::RedstoneWire), 0);
    live.world.w().setDataRaw(0, 64, 0, 9);
    live.run();
    CHECK_EQ(live.fx.count(), 1);
    CHECK_EQ(live.countOf(ParticleKind::Reddust), 1);
    // Flat on the floor of its cell: `y + 0.0625`, one texel up.
    CHECK(live.fx[0].y > 64.0 && live.fx[0].y < 64.1);
}

TEST(only_the_glowing_redstone_ore_glitters_and_only_on_its_open_faces)
{
    // Buried on all six sides: every mote lands inside the block and is
    // refused, so an ore in a wall does not sparkle through it.
    Scene buried;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dz = -1; dz <= 1; ++dz) {
                buried.world.place(dx, 64 + dy, dz, bid(mcver::Block::Stone), 0);
            }
        }
    }
    buried.world.place(0, 64, 0, bid(mcver::Block::LitRedstoneOre), 0);
    buried.run();
    CHECK_EQ(buried.fx.count(), 0);

    // Open on every side: **five motes, not six**, and the missing one is the
    // jar's own. `ai.i` tests each mote with `py < 0.0` where every other axis
    // is tested against the block's own coordinate, so the mote pushed a
    // sixteenth *below* the floor is at y 63.94 -- outside the block, but not
    // below zero -- and is refused. An ore at the bottom of the world would
    // sparkle downward and no other one does. Copied, not corrected.
    Scene exposed;
    exposed.world.place(0, 64, 0, bid(mcver::Block::LitRedstoneOre), 0);
    exposed.run();
    CHECK_EQ(exposed.fx.count(), 5);
    CHECK_EQ(exposed.countOf(ParticleKind::Reddust), 5);

    // The unlit ore has no display tick at all.
    Scene dull;
    dull.world.place(0, 64, 0, bid(mcver::Block::RedstoneOre), 0);
    dull.run();
    CHECK_EQ(dull.fx.count(), 0);
}

TEST(a_fire_on_a_floor_smokes_upward_only)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Fire), 0);
    // The stone floor at y 63 is already there, and opaque ground is the whole
    // of the first branch -- it returns, so the walls are never asked.
    s.world.place(-1, 64, 0, bid(mcver::Block::Planks), 0);
    s.run();
    CHECK_EQ(s.countOf(ParticleKind::LargeSmoke), 3);
    for (int i = 0; i < s.fx.count(); ++i) {
        // In the upper half of the cell.
        CHECK(s.fx[i].y >= 64.5 && s.fx[i].y <= 65.0);
    }
}

TEST(a_fire_hanging_on_a_wall_smokes_off_that_wall)
{
    Scene s;
    // No floor under it, so the first branch is not taken and the sides are.
    // **The wall goes up first**: `onBlockAdded` puts a fire out at once if it
    // has neither ground under it nor fuel beside it, so a fire placed into
    // bare air is gone before it can be asked for a particle.
    s.world.w().setBlockAndDataWithNotify(0, 63, 0, block::kAir, 0);
    s.world.place(1, 64, 0, bid(mcver::Block::Planks), 0);
    s.world.place(0, 64, 0, bid(mcver::Block::Fire), 0);
    s.run();
    // Two puffs off the one burnable side and none off the other three.
    CHECK_EQ(s.countOf(ParticleKind::LargeSmoke), 2);
    for (int i = 0; i < s.fx.count(); ++i) {
        CHECK(s.fx[i].x > 0.85);
    }
}

TEST(lava_pops_and_water_does_not)
{
    // One dart in a hundred, so a hundred tries is enough to see one and the
    // kind is what matters.
    Scene lava;
    lava.world.place(0, 64, 0, bid(mcver::Block::Lava), 0);
    for (int i = 0; i < 2000 && lava.fx.count() == 0; ++i) {
        lava.run();
    }
    CHECK(lava.fx.count() > 0);
    CHECK_EQ(lava.countOf(ParticleKind::Lava), lava.fx.count());
    // Out of the *top* of the cell -- `this.maxY`, which is 1.0 for a fluid.
    CHECK(lava.fx[0].y >= 65.0);

    // Water trickles: a sound and no particle at all.
    Scene water;
    water.world.place(0, 64, 0, bid(mcver::Block::Water), 3);
    for (int i = 0; i < 2000; ++i) {
        water.run();
    }
    CHECK_EQ(water.fx.count(), 0);
}

TEST(lava_under_a_lid_does_not_pop)
{
    Scene s;
    s.world.place(0, 64, 0, bid(mcver::Block::Lava), 0);
    s.world.place(0, 65, 0, bid(mcver::Block::Stone), 0);
    for (int i = 0; i < 2000; ++i) {
        s.run();
    }
    CHECK_EQ(s.fx.count(), 0);
}

TEST(the_darts_reach_the_blocks_round_the_player_and_no_further)
{
    Scene s;
    // A ring of torches at the very edge of the 15-block reach and one just
    // past it. `x + nextInt(16) - nextInt(16)` spans -15..15.
    s.world.place(15, 64, 0, bid(mcver::Block::Torch), 5);
    s.world.place(-15, 64, 0, bid(mcver::Block::Torch), 5);

    for (int i = 0; i < 20; ++i) {
        tick::displayTick(s.world.w(), 0, 64, 0, s.rand);
    }
    CHECK(s.fx.count() > 0);
    for (int i = 0; i < s.fx.count(); ++i) {
        CHECK(s.fx[i].x >= -16.0 && s.fx[i].x <= 16.0);
    }
}

TEST(a_world_with_no_particle_sink_draws_no_darts_at_all)
{
    SceneWorld world{0, 0};
    world.place(0, 64, 0, bid(mcver::Block::Torch), 5);

    // The world harness installs no sink, and the loop has to be free there:
    // it neither reads a block nor touches the world's generator.
    JavaRandom rand{7};
    const i64 before = 0;
    (void) before;
    tick::displayTick(world.w(), 0, 64, 0, rand);
    // Nothing to check but that it returned; the point is that the 6,000 draws
    // off `world.random()` did not happen, which the next line proves.
    JavaRandom fresh{7};
    CHECK_EQ((long long) rand.nextInt(), (long long) fresh.nextInt());
}
