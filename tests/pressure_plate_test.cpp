// Pressure plates: the one block in a1.1.2 that asks the world about entities.
//
// Everything here is `al` -- BlockPressurePlate -- and the seam it needs. The
// plate is worth its own file rather than a corner of tick_test.cpp because it
// is the only behaviour whose input is not a block: it arms from
// `TickWorld::anyEntityIn`, which is a hook nothing else uses, and the two
// plates differ *only* in what they ask that hook for. A test that never sets
// the hook is testing the old behaviour, where a plate could not be pressed at
// all.
//
// The three things pinned:
//
//   * the sense box -- an eighth in on the four sides, a quarter of a block
//     tall -- so standing beside a plate does not press it,
//   * the two filters, which is a dropped item pressing the wooden plate and
//     not the stone one, and
//   * the timer, which is a plate staying down for its twenty ticks after the
//     thing on it has gone.

#include "core/block/registry.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/redstone.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::block::BlockId;
using mc::tick::EntityFilter;
using mc::tick::TickWorld;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// One entity, put where a test wants it, with a kind. Enough to answer the
// only question a1.1.2 asks: `EntityPlayer` is an `EntityLiving`, a dropped
// item is neither, and nothing else in this build exists yet.
struct Scene {
    SceneWorld world{0, 0};

    AABB box{};
    bool present = false;
    bool living = false;  // a player or a mob, as `js.b` selects on

    Scene()
    {
        // A floor at y = 63 and a plate on top of it at y = 64, so the plate
        // has the opaque cube underneath that `al.a(Lcn;IIII)V` insists on.
        for (i32 x = -2; x <= 2; ++x) {
            for (i32 z = -2; z <= 2; ++z) {
                world.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
        world.w().setEntityQuery(&Scene::query, this);
    }

    void plate(mcver::Block which) { world.place(0, 64, 0, bid(which), 0); }

    u8 metadata() { return world.w().dataAt(0, 64, 0); }

    // The player's own box, near enough: 0.6 across and 1.8 tall, centred on
    // (x, z) with its feet at `feetY`.
    void standAt(double x, double feetY, double z, bool isLiving)
    {
        box = AABB{x - 0.3, feetY, z - 0.3, x + 0.3, feetY + 1.8, z + 0.3};
        present = true;
        living = isLiving;
    }

    // A dropped item: a quarter of a block, centred, and never living.
    void dropAt(double x, double centreY, double z)
    {
        box = AABB{x - 0.125, centreY - 0.125, z - 0.125,
                   x + 0.125, centreY + 0.125, z + 0.125};
        present = true;
        living = false;
    }

    void leave() { present = false; }

    // The collision scan `moveEntity` runs, then one world tick. That pair is
    // what a real frame does, in that order.
    void step(int ticks)
    {
        const TickWorld::Centre centre{0, 0};
        for (int i = 0; i < ticks; ++i) {
            if (present) {
                mc::tick::entityCollidedWithBlocks(world.w(), box);
            }
            world.w().tick(&centre, 1, 0);
        }
    }

    static bool query(void* ctx, const AABB& probe, EntityFilter filter)
    {
        const Scene* self = static_cast<const Scene*>(ctx);
        if (!self->present || !self->box.intersects(probe)) {
            return false;
        }
        return filter == EntityFilter::Everything || self->living;
    }
};

}  // namespace

TEST(a_plate_stays_up_in_a_world_with_no_entities_in_it)
{
    // The behaviour before the hook existed, and it has to survive: every
    // headless tool in this project runs a world nobody is standing in.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.step(40);
    CHECK_EQ(int(s.metadata()), 0);
}

TEST(standing_on_a_wooden_plate_presses_it)
{
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.standAt(0.5, 64.0, 0.5, true);
    s.step(1);
    CHECK_EQ(int(s.metadata()), 1);
}

TEST(standing_on_a_stone_plate_presses_it)
{
    // `js.b` is "mobs", and a player is an `EntityLiving`.
    Scene s;
    s.plate(mcver::Block::StonePressurePlate);
    s.standAt(0.5, 64.0, 0.5, true);
    s.step(1);
    CHECK_EQ(int(s.metadata()), 1);
}

TEST(a_pressed_plate_powers_the_block_above_it_and_nothing_else)
{
    // `al.c`: direct power goes straight up. Side 1 is +y in the original's
    // numbering, which is how the block above asks.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.standAt(0.5, 64.0, 0.5, true);
    s.step(1);

    CHECK(s.world.w().providesPowerTo(0, 64, 0, 1));
    CHECK(!s.world.w().providesPowerTo(0, 64, 0, 0));
    CHECK(!s.world.w().providesPowerTo(0, 64, 0, 4));

    // ...and indirect power goes everywhere, which is what a wire beside it
    // reads.
    CHECK(s.world.w().indirectlyProvidesPowerTo(0, 64, 0, 4));
}

TEST(a_plate_comes_back_up_twenty_ticks_after_it_is_stepped_off)
{
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.standAt(0.5, 64.0, 0.5, true);
    s.step(1);
    CHECK_EQ(int(s.metadata()), 1);

    s.leave();

    // **Not immediately.** The plate is disarmed by its own scheduled update,
    // which it booked at its tick rate of 20 -- so it stays down for most of a
    // second after the thing on it has gone.
    s.step(10);
    CHECK_EQ(int(s.metadata()), 1);

    s.step(30);
    CHECK_EQ(int(s.metadata()), 0);
}

TEST(a_plate_held_down_keeps_re_scheduling_itself)
{
    // The `if (flag1) world.scheduleBlockUpdate(...)` at the end of `al.h`. A
    // plate that stopped re-booking would come up under somebody's feet.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.standAt(0.5, 64.0, 0.5, true);
    s.step(200);
    CHECK_EQ(int(s.metadata()), 1);
}

TEST(a_dropped_item_presses_the_wooden_plate)
{
    // `js.a` -- "everything" -- and this is the whole observable difference
    // between the two plates.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.dropAt(0.5, 64.125, 0.5);
    s.step(1);
    CHECK_EQ(int(s.metadata()), 1);
}

TEST(a_dropped_item_does_not_press_the_stone_plate)
{
    Scene s;
    s.plate(mcver::Block::StonePressurePlate);
    s.dropAt(0.5, 64.125, 0.5);
    s.step(1);
    CHECK_EQ(int(s.metadata()), 0);
}

TEST(standing_in_the_next_cell_does_not_press_a_plate)
{
    // The sense box is inset an eighth on all four sides, so a body flush
    // against the plate's cell boundary is outside it. A player at x = 1.2 has
    // their box reaching back to 0.9, which is inside the *cell* and outside
    // the box -- which is exactly the case the inset exists for.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.standAt(1.2, 64.0, 0.5, true);
    s.step(4);
    CHECK_EQ(int(s.metadata()), 0);
}

TEST(a_body_more_than_a_quarter_block_above_a_plate_does_not_press_it)
{
    // The box is 0.25 tall. A player standing on a block *beside* the plate,
    // one level up, must not press it through the floor.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.standAt(0.5, 64.5, 0.5, true);
    s.step(4);
    CHECK_EQ(int(s.metadata()), 0);
}

TEST(a_plate_drops_when_the_block_under_it_goes_away)
{
    // `al.a(Lcn;IIII)V`, which was already here -- pinned again because the
    // switch it lives in now has two labels instead of one.
    Scene s;
    s.plate(mcver::Block::WoodenPressurePlate);
    s.world.w().setBlockWithNotify(0, 63, 0, block::kAir);
    CHECK_EQ(int(s.world.w().blockAt(0, 64, 0)), int(block::kAir));
}

TEST(both_plates_still_need_a_solid_block_underneath_to_be_placed)
{
    Scene s;
    CHECK(mc::tick::canPlaceAt(s.world.w(), bid(mcver::Block::WoodenPressurePlate), 0, 64, 0));
    CHECK(mc::tick::canPlaceAt(s.world.w(), bid(mcver::Block::StonePressurePlate), 0, 64, 0));
    CHECK(!mc::tick::canPlaceAt(s.world.w(), bid(mcver::Block::WoodenPressurePlate), 0, 70, 0));
    CHECK(!mc::tick::canPlaceAt(s.world.w(), bid(mcver::Block::StonePressurePlate), 0, 70, 0));
}
