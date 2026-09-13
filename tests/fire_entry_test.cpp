// **What standing in fire does to an entity** -- the tail of `kh.c(DDD)V`,
// which is the ignition a1.1.2 has and this port did not.
//
// The unit half pins the counter arithmetic (the negative fuse, the 300, the
// hiss) without a world; the rest put an animal, a stack and a cart in a flame
// and watch. See core/entity/fire_entry.hpp.

#include "core/entity/fire_entry.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/mob.hpp"
#include "core/item/creative_palette.hpp"
#include "core/util/math_helper.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

using namespace mc;
using mc::entity::boundingBoxBurning;
using mc::entity::kCaughtFireTicks;
using mc::entity::kEntityFireResistance;
using mc::entity::kPlayerFireResistance;
using mc::entity::MobSurroundings;
using mc::entity::MobSystem;
using mc::entity::MobType;
using mc::entity::updateFireEntry;
using mc::test::SceneWorld;

namespace {

block::BlockId bid(mcver::Block b) { return block::BlockId(b); }

// A stone floor at y = 63 with air above it, lit.
struct Room {
    SceneWorld scene{0, 0};

    Room()
    {
        for (i32 x = -8; x <= 8; ++x) {
            for (i32 z = -8; z <= 8; ++z) {
                scene.place(x, 62, z, bid(mcver::Block::Dirt), 0);
                scene.place(x, 63, z, bid(mcver::Block::Stone), 0);
            }
        }
        scene.lightColumnsFrom(-8, 8, -8, 8, 64);
    }

    tick::TickWorld& w() { return scene.w(); }

    static constexpr double kFloor = 64.0;
};

}  // namespace

TEST(the_fire_counter_is_a_fuse_that_rests_below_zero)
{
    // Out of the fire, the counter is parked at `-fireResistance` and stays
    // there: `else if (fire <= 0) fire = -fireResistance`.
    i16 fire = 0;
    updateFireEntry(&fire, false, false);
    CHECK_EQ(int(fire), -kEntityFireResistance);
    updateFireEntry(&fire, false, false);
    CHECK_EQ(int(fire), -kEntityFireResistance);

    // One tick in a flame takes it to zero, and zero is the tick it catches.
    const entity::FireEntryResult hit = updateFireEntry(&fire, true, false);
    CHECK(hit.damage);
    CHECK(!hit.fizz);
    CHECK_EQ(int(fire), kCaughtFireTicks);

    // **A player's fuse is twenty ticks long**, which is the whole of why you
    // can run through a fire and an animal cannot.
    i16 playerFire = 0;
    updateFireEntry(&playerFire, false, false, kPlayerFireResistance);
    CHECK_EQ(int(playerFire), -kPlayerFireResistance);
    for (int i = 0; i < kPlayerFireResistance - 1; ++i) {
        updateFireEntry(&playerFire, true, false, kPlayerFireResistance);
        CHECK(playerFire < 0);
    }
    updateFireEntry(&playerFire, true, false, kPlayerFireResistance);
    CHECK_EQ(int(playerFire), kCaughtFireTicks);
}

TEST(damage_is_every_tick_in_the_fire_and_water_puts_it_out)
{
    // `dealFireDamage(1)` is outside the `!flag` test, so a wet entity standing
    // in fire still takes it -- it just never catches light.
    i16 fire = i16(-kEntityFireResistance);
    const entity::FireEntryResult wetInFire = updateFireEntry(&fire, true, true);
    CHECK(wetInFire.damage);
    CHECK_EQ(int(fire), -kEntityFireResistance);

    // Alight and then under: the hiss, and the counter goes back to its fuse.
    fire = i16(kCaughtFireTicks);
    const entity::FireEntryResult doused = updateFireEntry(&fire, false, true);
    CHECK(doused.fizz);
    CHECK(!doused.damage);
    CHECK_EQ(int(fire), -kEntityFireResistance);
}

TEST(what_counts_as_a_burning_box_is_fire_or_lava_and_reaches_a_touching_cell)
{
    Room room;
    room.scene.place(0, 64, 0, bid(mcver::Block::Fire), 0);
    room.scene.place(4, 64, 0, bid(mcver::Block::Lava), 0);

    // Inside the fire cell.
    CHECK(boundingBoxBurning(room.w(), AABB{0.3, 64.1, 0.3, 0.7, 64.5, 0.7}));
    // Lava counts too -- `isBoundingBoxBurning` tests three ids, and two of
    // them are the lava pair.
    CHECK(boundingBoxBurning(room.w(), AABB{4.3, 64.1, 4.0, 4.7, 64.5, 4.0}) == false);
    CHECK(boundingBoxBurning(room.w(), AABB{4.3, 64.1, 0.3, 4.7, 64.5, 0.7}));
    // **A box that only touches the plane of the cell still burns**: the scan
    // runs to `floor(max + 1)`, so a box ending exactly on x = 0 reaches the
    // cell at x = 0.
    CHECK(boundingBoxBurning(room.w(), AABB{-0.6, 64.1, 0.3, 0.0, 64.5, 0.7}));
    // Two cells away it does not.
    CHECK(!boundingBoxBurning(room.w(), AABB{2.3, 64.1, 0.3, 2.7, 64.5, 0.7}));
    // And neither does an empty room.
    CHECK(!boundingBoxBurning(room.w(), AABB{0.3, 70.0, 0.3, 0.7, 70.5, 0.7}));
}

TEST(an_animal_standing_in_fire_catches_and_burns)
{
    Room room;
    MobSystem mobs{4242};
    CHECK(mobs.spawn(room.w(), MobType::Pig, 0.5, Room::kFloor, 0.5, 0.0f));

    // **One tick in the open first**, which is what parks the counter at its
    // fuse of -1. An entity that has never ticked still has the constructor's
    // zero, and zero is the one value that cannot catch -- see the case below.
    MobSurroundings nobody;
    mobs.tick(room.w(), nobody);
    CHECK_EQ(int(mobs[0].fire), -kEntityFireResistance);

    room.scene.place(0, 64, 0, bid(mcver::Block::Fire), 0);
    const i16 full = mobs[0].health;
    mobs.tick(room.w(), nobody);

    // Alight on the first tick: `fireResistance` is 1 for everything but a
    // player, so the fuse is one tick long.
    CHECK(mobs.count() > 0);
    CHECK_EQ(int(mobs[0].fire), kCaughtFireTicks);
    CHECK(mobs[0].health < full);

    // And it dies of it. A pig has ten health and the ten-tick invulnerability
    // window means roughly one a second, so this is seconds rather than ticks.
    for (int i = 0; i < 400 && mobs.count() > 0; ++i) {
        mobs.tick(room.w(), nobody);
    }
    CHECK_EQ(mobs.count(), 0);
}

TEST(an_animal_beside_a_fire_does_not_catch)
{
    Room room;
    MobSystem mobs{4242};
    CHECK(mobs.spawn(room.w(), MobType::Pig, 0.5, Room::kFloor, 0.5, 0.0f));
    room.scene.place(3, 64, 0, bid(mcver::Block::Fire), 0);

    MobSurroundings nobody;
    const i16 full = mobs[0].health;
    for (int i = 0; i < 40; ++i) {
        mobs.tick(room.w(), nobody);
    }
    CHECK(mobs.count() > 0);
    // The pig may have wandered; what matters is that nothing lit it where it
    // started and that it is not quietly losing health.
    if (mobs[0].body.x < 2.0) {
        CHECK(mobs[0].fire <= 0);
        CHECK_EQ(int(mobs[0].health), int(full));
    }
}

TEST(a_counter_at_zero_burns_without_ever_catching)
{
    // **A quirk of the original, and it is the increment-then-test.** `fire++`
    // runs *before* `if (fire == 0)`, so a counter sitting at exactly zero goes
    // to one and sails past the value that would set it to 300. Zero is where
    // the constructor leaves it and where `kh.y()`'s water branch puts it, so
    // an entity that is dropped straight into a flame, or that steps out of a
    // river into one, burns at 1 rather than at 300.
    //
    // It still takes `dealFireDamage(1)` every tick and still counts as alight
    // -- everything that asks tests `fire > 0` -- so what is lost is only the
    // fifteen seconds it would have kept burning after leaving the fire.
    i16 fire = 0;
    for (int i = 0; i < 5; ++i) {
        const entity::FireEntryResult burn = updateFireEntry(&fire, true, false);
        CHECK(burn.damage);
    }
    CHECK_EQ(int(fire), 5);
    CHECK(fire != kCaughtFireTicks);
}

TEST(a_boat_in_a_fire_chars_and_then_breaks)
{
    // A boat is 1.5 across, so a fire in the cell beside it reaches it -- which
    // is the whole point of `isBoundingBoxBurning` taking the box and not the
    // position. `dc.a(Lkh;I)Z` takes ten of the forty points a hull survives,
    // so four seconds of flame is a pile of planks.
    Room room;
    entity::BoatSystem boats{5};
    CHECK(boats.place(room.w(), 0, 63, 0));
    CHECK_EQ(boats.count(), 1);

    entity::VehicleRider nobody;
    boats.tick(room.w(), nobody);
    CHECK(boats.count() > 0);
    CHECK_EQ(int(boats[0].fire), -kEntityFireResistance);

    const int by = int(MathHelper::floorDouble(boats[0].box.minY));
    room.scene.place(1, by, 0, bid(mcver::Block::Fire), 0);
    boats.tick(room.w(), nobody);
    CHECK(boats.count() > 0);
    CHECK_EQ(int(boats[0].fire), kCaughtFireTicks);
    CHECK(boats[0].damage > 0);

    for (int i = 0; i < 200 && boats.count() > 0; ++i) {
        boats.tick(room.w(), nobody);
    }
    CHECK_EQ(boats.count(), 0);
}
