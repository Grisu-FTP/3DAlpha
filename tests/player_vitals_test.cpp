// The player's health against the class files it was transcribed from.
//
// Every expected number here is arithmetic done by hand on the bytecode quoted
// in core/entity/player_vitals.hpp -- not a value read back from a running
// game. The oracle run (`tools/genref.java --survival`) is the stronger check
// and replaces these where it covers them; these stay because each one pins a
// single rule by name, so a regression says which rule it broke.

#include "core/entity/player_body.hpp"
#include "core/entity/player_vitals.hpp"
#include "core/item/inventory.hpp"
#include "core/item/registry.hpp"
#include "core/tick/tick_world.hpp"
#include "core/world/level_data.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py
#include "items.hpp"   // generated; see tools/configure.py

using namespace mc;
using mc::entity::Attacker;
using mc::entity::DamageSource;
using mc::entity::Harm;
using mc::entity::PlayerBody;
using mc::entity::PlayerContext;
using mc::entity::PlayerVitals;
using mc::test::SceneWorld;

namespace {

// A player standing in open air, well above anything, at the origin.
struct Fixture {
    SceneWorld scene{0, 0};
    PlayerBody body;
    item::Inventory inventory;
    PlayerVitals vitals{1234};

    Fixture() { body.setFeet(8.5, 80.0, 8.5); }

    PlayerContext context(int difficulty = 2)
    {
        PlayerContext ctx{scene.w(), body, inventory};
        ctx.difficulty = difficulty;
        return ctx;
    }

    // Enough ticks for the ten-tick half of the window to run out.
    void waitOutWindow()
    {
        PlayerContext ctx = context();
        for (int i = 0; i < entity::kPlayerHurtResistantTime; ++i) {
            vitals.tick(ctx, false);
        }
    }

    void wear(mcver::Item piece)
    {
        const int slot = item::Inventory::armourSlotFor(item::ItemId(piece));
        inventory.set(slot, item::ItemId(piece), 1);
    }
};

Attacker monsterAt(double x, double z)
{
    Attacker a;
    a.source = DamageSource::Monster;
    a.x = x;
    a.z = z;
    return a;
}

}  // namespace

TEST(a_fall_of_three_blocks_is_free_and_every_part_block_after_it_costs_one)
{
    Fixture f;
    PlayerContext ctx = f.context();
    CHECK(!f.vitals.fall(ctx, 3.0f).landed);
    CHECK_EQ(int(f.vitals.health), 20);

    // ceil(3.5 - 3) = 1
    CHECK(f.vitals.fall(ctx, 3.5f).landed);
    CHECK_EQ(int(f.vitals.health), 19);

    f.waitOutWindow();
    // ceil(10 - 3) = 7
    f.vitals.fall(ctx, 10.0f);
    CHECK_EQ(int(f.vitals.health), 12);
}

TEST(a_second_hit_inside_the_window_does_nothing_however_large)
{
    Fixture f;
    PlayerContext ctx = f.context();
    CHECK(f.vitals.attack(ctx, 2, Attacker{}).landed);
    CHECK_EQ(int(f.vitals.health), 18);

    // `dm` refuses before `ge` gets to compare sizes: no "bigger hit lands".
    CHECK(!f.vitals.attack(ctx, 15, Attacker{}).landed);
    CHECK_EQ(int(f.vitals.health), 18);

    // `aW > 10` is the test, so after ten ticks the window is 10 and a hit lands.
    for (int i = 0; i < 9; ++i) {
        f.vitals.tick(ctx, false);
    }
    CHECK(!f.vitals.attack(ctx, 1, Attacker{}).landed);
    f.vitals.tick(ctx, false);
    CHECK(f.vitals.attack(ctx, 1, Attacker{}).landed);
    CHECK_EQ(int(f.vitals.health), 17);
}

TEST(difficulty_scales_a_monster_or_an_arrow_and_nothing_else)
{
    {
        Fixture f;
        PlayerContext ctx = f.context(0);
        CHECK(!f.vitals.attack(ctx, 7, monsterAt(0.0, 0.0)).landed);
        CHECK_EQ(int(f.vitals.health), 20);
        // A fall is the same on Peaceful.
        CHECK(f.vitals.attack(ctx, 7, Attacker{}).landed);
        CHECK_EQ(int(f.vitals.health), 13);
    }
    {
        Fixture f;
        PlayerContext ctx = f.context(1);
        f.vitals.attack(ctx, 7, monsterAt(0.0, 0.0));  // 7 / 3 + 1 = 3
        CHECK_EQ(int(f.vitals.health), 17);
    }
    {
        Fixture f;
        PlayerContext ctx = f.context(2);
        Attacker arrow;
        arrow.source = DamageSource::Arrow;
        f.vitals.attack(ctx, 4, arrow);
        CHECK_EQ(int(f.vitals.health), 16);
    }
    {
        Fixture f;
        PlayerContext ctx = f.context(3);
        f.vitals.attack(ctx, 7, monsterAt(0.0, 0.0));  // 7 * 3 / 2 = 10
        CHECK_EQ(int(f.vitals.health), 10);
    }
    {
        Fixture f;
        PlayerContext ctx = f.context(3);
        Attacker slime;
        slime.source = DamageSource::Other;
        f.vitals.attack(ctx, 7, slime);  // not a `dq`: unscaled
        CHECK_EQ(int(f.vitals.health), 13);
    }
}

TEST(armour_absorbs_in_twenty_fifths_and_carries_the_remainder)
{
    Fixture f;
    f.wear(mcver::Item::IronHelmet);
    f.wear(mcver::Item::IronChestplate);
    f.wear(mcver::Item::IronLeggings);
    f.wear(mcver::Item::IronBoots);
    // 3 + 8 + 6 + 3 = 20 points, unworn: (20 - 1) * total / total + 1 = 20.
    CHECK_EQ(entity::armourValue(f.inventory), 20);

    PlayerContext ctx = f.context();
    // 4 * (25 - 20) + 0 = 20 -> nothing lands, 20 carried. The armour still wears.
    CHECK(!f.vitals.attack(ctx, 4, Attacker{}).landed);
    CHECK_EQ(int(f.vitals.health), 20);
    CHECK_EQ(f.vitals.armourCarry, 20);
    const item::ItemStack& helmet =
        f.inventory.at(item::Inventory::armourSlotFor(item::ItemId(mcver::Item::IronHelmet)));
    CHECK_EQ(int(helmet.damage), 4);

    // Worn now: the value is (19 * remaining) / total + 1 with every piece down
    // by 4, which still rounds to 19 -- so 4 * 6 + 20 = 44 -> 1 lands, 19 carried.
    const int value = entity::armourValue(f.inventory);
    CHECK_EQ(value, 19);
    CHECK(f.vitals.attack(ctx, 4, Attacker{}).landed);
    CHECK_EQ(int(f.vitals.health), 19);
    CHECK_EQ(f.vitals.armourCarry, 19);
}

TEST(a_piece_worn_past_its_durability_is_gone)
{
    item::ItemStack boots;
    boots.id = item::ItemId(mcver::Item::LeatherBoots);
    boots.count = 1;
    const int durability = item::def(boots.id).durability;
    boots.damage = i16(durability);
    CHECK(!entity::wearStack(boots, 0));
    CHECK(entity::wearStack(boots, 1));
    CHECK_EQ(int(boots.damage), 0);
}

TEST(drowning_takes_two_when_air_reaches_minus_twenty_and_starts_again_at_zero)
{
    Fixture f;
    // Still water from well below the feet to well above the eye.
    for (int y = 76; y <= 86; ++y) {
        for (int x = 6; x <= 10; ++x) {
            for (int z = 6; z <= 10; ++z) {
                f.scene.place(x, y, z, block::BlockId(mcver::Block::Water), 0);
            }
        }
    }
    CHECK(entity::playerEyeInWater(f.scene.w(), f.body));

    PlayerContext ctx = f.context();
    f.vitals.fire = 100;
    // 300 down to -20 is 320 ticks, the last of which is the blow.
    for (int i = 0; i < 319; ++i) {
        CHECK(!f.vitals.tick(ctx, true).landed);
    }
    CHECK_EQ(int(f.vitals.air), -19);
    CHECK_EQ(int(f.vitals.fire), 0);
    CHECK(f.vitals.tick(ctx, true).landed);
    CHECK_EQ(int(f.vitals.health), 18);
    CHECK_EQ(int(f.vitals.air), 0);
    // The next one is twenty ticks later.
    for (int i = 0; i < 19; ++i) {
        CHECK(!f.vitals.tick(ctx, true).landed);
    }
    CHECK(f.vitals.tick(ctx, true).landed);
    CHECK_EQ(int(f.vitals.health), 16);
}

TEST(a_head_above_the_water_breathes_and_air_refills_at_once)
{
    Fixture f;
    PlayerContext ctx = f.context();
    f.vitals.air = 5;
    f.vitals.tick(ctx, false);
    CHECK_EQ(int(f.vitals.air), entity::kPlayerMaxAir);
}

TEST(fire_costs_one_every_twenty_ticks_and_water_puts_it_out)
{
    Fixture f;
    PlayerContext ctx = f.context();
    f.vitals.fire = 40;
    CHECK(f.vitals.tick(ctx, false).landed);  // 40 % 20 == 0
    CHECK_EQ(int(f.vitals.health), 19);
    for (int i = 0; i < 19; ++i) {
        CHECK(!f.vitals.tick(ctx, false).landed);
    }
    CHECK(f.vitals.tick(ctx, false).landed);  // 20
    CHECK_EQ(int(f.vitals.health), 18);

    f.vitals.fire = 60;
    f.vitals.tick(ctx, true);
    CHECK_EQ(int(f.vitals.fire), 0);
}

TEST(lava_takes_ten_and_sets_the_player_alight)
{
    Fixture f;
    for (int y = 78; y <= 82; ++y) {
        f.scene.place(8, y, 8, block::BlockId(mcver::Block::Lava), 0);
    }
    PlayerContext ctx = f.context();
    CHECK(f.vitals.tick(ctx, false).landed);
    CHECK_EQ(int(f.vitals.health), 10);
    CHECK_EQ(int(f.vitals.fire), entity::kPlayerLavaFireTicks);
}

TEST(below_the_void_line_the_eye_takes_four_a_tick)
{
    Fixture f;
    f.body.setFeet(8.5, -70.0, 8.5);
    PlayerContext ctx = f.context();
    CHECK(f.vitals.tick(ctx, false).landed);
    CHECK_EQ(int(f.vitals.health), 16);
}

TEST(suffocation_is_asked_a_tenth_above_the_eye)
{
    Fixture f;
    // Feet at 80, eye at 81.62, probe at 81.74: the cell at y = 81.
    f.scene.place(8, 81, 8, block::BlockId(mcver::Block::Stone), 0);
    CHECK(entity::playerInsideOpaqueBlock(f.scene.w(), f.body));
    PlayerContext ctx = f.context();
    CHECK(f.vitals.tick(ctx, false).landed);
    CHECK_EQ(int(f.vitals.health), 19);
}

// **The order the frame loop has to run these two in**, pinned here because the
// loop itself is 3DS-only and this is the rule it broke.
//
// `respawnBody` stands the body at `spawnY + 1`, and `spawnY` is 64 whatever
// the ground is doing there, so the feet start inside the hillside as often as
// not. `kh.q()`'s lift is what takes it out -- and in a1.1.2 that runs from the
// constructor, before anything ticks the player at all. Run `y()` first instead
// and the eye is in stone for one tick, which is one point of suffocation: the
// half a heart a fresh Survival world used to open with.
TEST(an_unlifted_spawn_point_suffocates_and_the_lift_is_what_stops_it)
{
    // A solid column with its surface at y = 70, and a spawn point at 64 well
    // under it -- the shape `spawnY`'s fixed 64 makes out of any hill.
    Fixture buried;
    for (int y = 60; y <= 70; ++y) {
        for (int x = 7; x <= 9; ++x) {
            for (int z = 7; z <= 9; ++z) {
                buried.scene.place(x, y, z, block::BlockId(mcver::Block::Stone), 0);
            }
        }
    }
    buried.body.setFeet(8.5, 65.0, 8.5);
    CHECK(entity::playerInsideOpaqueBlock(buried.scene.w(), buried.body));
    PlayerContext buriedCtx = buried.context();
    CHECK(buried.vitals.tick(buriedCtx, false).landed);
    CHECK_EQ(int(buried.vitals.health), 19);

    // The same spawn point, lifted first as `liftIntoTheWorld` lifts it.
    Fixture lifted;
    for (int y = 60; y <= 70; ++y) {
        for (int x = 7; x <= 9; ++x) {
            for (int z = 7; z <= 9; ++z) {
                lifted.scene.place(x, y, z, block::BlockId(mcver::Block::Stone), 0);
            }
        }
    }
    lifted.body.setFeet(8.5, 65.0, 8.5);
    CHECK(lifted.body.liftOutOfGround(lifted.scene.w(), 128) > 0);
    CHECK(!entity::playerInsideOpaqueBlock(lifted.scene.w(), lifted.body));
    PlayerContext liftedCtx = lifted.context();
    CHECK(!lifted.vitals.tick(liftedCtx, false).landed);
    CHECK_EQ(int(lifted.vitals.health), 20);
}

TEST(healing_is_capped_at_twenty_and_opens_half_a_window)
{
    Fixture f;
    f.vitals.health = 15;
    f.vitals.heal(8);
    CHECK_EQ(int(f.vitals.health), 20);
    CHECK_EQ(int(f.vitals.hurtResistant), 10);

    f.vitals.health = 0;
    f.vitals.heal(5);
    CHECK_EQ(int(f.vitals.health), 0);
}

TEST(peaceful_gives_one_back_every_twenty_ticks)
{
    Fixture f;
    PlayerContext ctx = f.context(0);
    f.vitals.health = 10;
    for (int i = 0; i < 19; ++i) {
        f.vitals.tick(ctx, false);
    }
    CHECK_EQ(int(f.vitals.health), 10);
    f.vitals.tick(ctx, false);  // ticksExisted 20
    CHECK_EQ(int(f.vitals.health), 11);

    Fixture normal;
    PlayerContext normalCtx = normal.context(2);
    normal.vitals.health = 10;
    for (int i = 0; i < 40; ++i) {
        normal.vitals.tick(normalCtx, false);
    }
    CHECK_EQ(int(normal.vitals.health), 10);
}

TEST(death_empties_the_inventory_and_lays_the_body_down)
{
    Fixture f;
    f.inventory.set(0, item::ItemId(mcver::Item::Stone), 12);
    f.wear(mcver::Item::LeatherHelmet);
    PlayerContext ctx = f.context();
    f.vitals.health = 3;
    const Harm hit = f.vitals.attack(ctx, 5, Attacker{});
    CHECK(hit.landed);
    CHECK(hit.died);
    CHECK(!f.vitals.alive());
    CHECK(f.inventory.at(0).empty());
    CHECK(f.inventory.empty());
    CHECK_EQ(double(f.body.yOffset), double(entity::kPlayerDeathEyeHeight));
    CHECK_EQ(double(f.body.width), double(entity::kPlayerDeathSize));

    // Nothing hurts the dead.
    CHECK(!f.vitals.attack(ctx, 5, Attacker{}).landed);

    f.vitals.respawn();
    CHECK(f.vitals.alive());
    CHECK_EQ(int(f.vitals.health), 20);
    CHECK_EQ(int(f.vitals.deathTime), 0);
}

TEST(an_invulnerable_player_takes_nothing_and_wears_nothing)
{
    Fixture f;
    f.wear(mcver::Item::IronBoots);
    f.vitals.invulnerable = true;
    PlayerContext ctx = f.context();
    CHECK(!f.vitals.attack(ctx, 50, monsterAt(0.0, 0.0)).landed);
    CHECK_EQ(int(f.vitals.health), 20);
    const item::ItemStack& boots =
        f.inventory.at(item::Inventory::armourSlotFor(item::ItemId(mcver::Item::IronBoots)));
    CHECK_EQ(int(boots.damage), 0);
}

TEST(a_hit_with_an_attacker_knocks_the_player_away_from_it)
{
    Fixture f;
    PlayerContext ctx = f.context();
    f.vitals.attack(ctx, 1, monsterAt(f.body.x - 3.0, f.body.z));
    CHECK(f.body.motionX > 0.0);
    CHECK_EQ(f.body.motionY, 0.4000000059604645);
}

TEST(the_vitals_round_trip_through_level_data)
{
    PlayerVitals vitals;
    vitals.health = 7;
    vitals.hurtTime = 4;
    vitals.deathTime = 0;
    vitals.attackTime = 2;
    vitals.air = 123;
    vitals.fire = -20;
    world::PlayerData data;
    vitals.save(&data);

    PlayerVitals back;
    back.load(data);
    CHECK_EQ(int(back.health), 7);
    CHECK_EQ(int(back.hurtTime), 4);
    CHECK_EQ(int(back.attackTime), 2);
    CHECK_EQ(int(back.air), 123);
    CHECK_EQ(int(back.fire), -20);
}
