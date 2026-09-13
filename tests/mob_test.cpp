// The four peaceful animals: what they are, what they do when nobody is
// watching, what they leave behind and where they come from.
//
// The interesting half of a mob is not the physics -- that is `EntityLiving`'s
// and is the player's, covered bit-exactly by tests/player_body_test.cpp
// against a real JVM. What is new and checkable here is everything around it:
// the table of four, the drops, the sheep's one-shot fleece, the cow's bucket,
// the pig's saddle, the chicken's egg clock, the wander and its pathfinder, the
// spawn rules, the despawn rules, the breeding extension, and the models.
//
// **There is no oracle suite here and there cannot be one for all of it.**
// `tools/genref.java` can drive a real `EntityPig` through a real `World`, but
// the AI it would drive is `Random`-fed at every step and a1.1.2 seeds its world
// random from the wall clock, so a vector comparison would be comparing two
// different coin flips. What is checked instead is the shape: the constants
// against the class file, the branches against what the bytecode says they do,
// and the invariants a play session would notice -- an animal that walks
// through a wall, a sheep that can be sheared twice, a pen that breeds itself
// into a thousand cows.

#include "core/block/registry.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/mob_spawn.hpp"
#include "core/entity/path_finder.hpp"
#include "core/item/registry.hpp"
#include "core/item/use.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/tick_world.hpp"
#include "drop_catcher.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::Mob;
using mc::entity::MobDef;
using mc::entity::mobDef;
using mc::entity::MobSurroundings;
using mc::entity::MobSystem;
using mc::entity::MobType;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// The bare fist, whose `getDamageVsEntity` is 1.
constexpr item::ItemId kEmptyHand = 0;

// Long enough for the ten-tick invulnerability window to lapse, and long enough
// for a corpse to finish lying there. Both are `ge`'s.
constexpr int kHurtGap = entity::kHurtResistantTime + 1;
constexpr int kDeathLinger = entity::kDeathTicks + 2;

// A field: dirt at y = 62, grass at y = 63, air above, lit.
struct Field {
    test::DropCatcher drops;
    SceneWorld scene{0, 0};
    MobSystem mobs{7777};

    explicit Field(i32 half = 24)
    {
        for (i32 x = -half; x <= half; ++x) {
            for (i32 z = -half; z <= half; ++z) {
                scene.place(x, 62, z, bid(mcver::Block::Dirt), 0);
                scene.place(x, 63, z, bid(mcver::Block::Grass), 0);
            }
        }
        // Daylight. An animal spawns only where the light is above eight, and
        // a scene with no lighting pass over it is pitch dark.
        scene.lightColumnsFrom(-half, half, -half, half, 64);
        drops.watch(scene.w());
    }

    tick::TickWorld& w() { return scene.w(); }

    // The surface of the field, which is the top of the grass block.
    static constexpr double kGround = 64.0;

    int add(MobType type, double x, double z, float yaw = 0.0f)
    {
        if (!mobs.spawn(w(), type, x, kGround, z, yaw)) {
            return -1;
        }
        return mobs.count() - 1;
    }

    void run(int ticks, const MobSurroundings& around = MobSurroundings{})
    {
        for (int i = 0; i < ticks; ++i) {
            mobs.tick(w(), around);
        }
    }
};

// A player standing at the origin, near enough that nothing despawns.
MobSurroundings watching(double x = 0.0, double z = 0.0)
{
    MobSurroundings around;
    around.player.present = true;
    around.player.x = x;
    around.player.y = Field::kGround + 1.62;
    around.player.z = z;
    around.player.box = AABB{x - 0.3, Field::kGround, z - 0.3, x + 0.3, Field::kGround + 1.8,
                             z + 0.3};
    return around;
}

}  // namespace

// ---------------------------------------------------------------------------
// The table of four
// ---------------------------------------------------------------------------

TEST(the_four_animals_are_the_four_the_jar_lists)
{
    // Sizes, health, drops and sound names, straight off the class files:
    // `mv`, `bo`, `am` and `mz`.
    const MobDef& sheep = mobDef(MobType::Sheep);
    // Widened from the class file's floats, which is what the fields hold:
    // 0.9f is not 0.9 and the difference is real.
    CHECK_EQ(double(sheep.width), double(0.9f));
    CHECK_EQ(double(sheep.height), double(1.3f));
    CHECK_EQ(sheep.health, 10);
    // **A sheep drops nothing when it dies.** `ge.g()` answers zero and `bo`
    // does not override it; the wool is `attackEntityFrom`'s.
    CHECK_EQ(int(sheep.dropItem), 0);
    CHECK_EQ(int(sheep.shearDrop), int(mcver::Block::Wool));

    const MobDef& pig = mobDef(MobType::Pig);
    CHECK_EQ(double(pig.width), double(0.9f));
    CHECK_EQ(double(pig.height), double(0.9f));
    CHECK_EQ(int(pig.dropItem), int(mcver::Item::RawPorkchop));

    const MobDef& cow = mobDef(MobType::Cow);
    CHECK_EQ(int(cow.dropItem), int(mcver::Item::Leather));
    // The cow is the only one with a `getSoundVolume` of its own.
    CHECK_EQ(double(cow.soundVolume), double(0.4f));

    const MobDef& chicken = mobDef(MobType::Chicken);
    CHECK_EQ(double(chicken.width), double(0.3f));
    CHECK_EQ(double(chicken.height), double(0.4f));
    CHECK_EQ(chicken.health, 4);
    CHECK_EQ(int(chicken.dropItem), int(mcver::Item::Feather));
}

TEST(a_spawned_animal_stands_on_the_ground_with_its_own_box)
{
    Field field;
    const int index = field.add(MobType::Cow, 0.5, 0.5);
    CHECK(index >= 0);
    const Mob& cow = field.mobs[index];
    const MobDef& def = mobDef(MobType::Cow);
    CHECK_EQ(cow.body.y, Field::kGround);
    CHECK_EQ(cow.body.box.minY, Field::kGround);
    CHECK(std::abs((cow.body.box.maxY - cow.body.box.minY) - double(def.height)) < 1e-9);
    CHECK(std::abs((cow.body.box.maxX - cow.body.box.minX) - double(def.width)) < 1e-6);
    // **`yOffset` is zero for a mob**, so the position it was given is its
    // feet -- unlike the player, whose `posY` is the eye.
    CHECK_EQ(cow.body.posY, Field::kGround);
}

// ---------------------------------------------------------------------------
// Being hit
// ---------------------------------------------------------------------------

TEST(a_pig_dies_in_ten_punches_and_leaves_nought_to_two_chops)
{
    Field field;
    const int pig = field.add(MobType::Pig, 0.5, 0.5);

    // Ten health, one damage a punch, and the ten-tick invulnerability window
    // means a punch every tick would not land -- so the tick between hits is
    // what the five-tick repeat already gives a player.
    int hits = 0;
    while (field.mobs[pig].health > 0 && hits < 100) {
        CHECK(field.mobs.attack(field.w(), pig, 1, true));
        ++hits;
        if (field.mobs[pig].health <= 0) {
            break;  // the corpse is checked below; do not tick it away first
        }
        field.run(kHurtGap, watching());
    }
    CHECK_EQ(hits, 10);
    CHECK(field.mobs[pig].health <= 0);

    // Nought to two raw porkchops, and nothing else.
    for (const test::Drop& drop : field.drops.drops) {
        CHECK_EQ(int(drop.item), int(mcver::Item::RawPorkchop));
        CHECK_EQ(drop.count, 1);
    }
    CHECK(field.drops.drops.size() <= 2);

    // The corpse lies there for twenty ticks and then is gone.
    CHECK_EQ(field.mobs.count(), 1);
    field.run(kDeathLinger, watching());
    CHECK_EQ(field.mobs.count(), 0);
}

TEST(a_second_hit_inside_the_invulnerability_window_does_only_its_difference)
{
    Field field;
    const int cow = field.add(MobType::Cow, 0.5, 0.5);

    CHECK(field.mobs.attack(field.w(), cow, 3, true));
    CHECK_EQ(field.mobs[cow].health, 7);
    // Inside the window: a *smaller* hit does nothing at all.
    CHECK(!field.mobs.attack(field.w(), cow, 1, true));
    CHECK_EQ(field.mobs[cow].health, 7);
    // A bigger one lands its difference, not its whole value.
    CHECK(field.mobs.attack(field.w(), cow, 5, true));
    CHECK_EQ(field.mobs[cow].health, 5);
}

TEST(a_sheep_is_shorn_once_and_a_hit_from_nobody_never_shears_it)
{
    Field field;
    const int sheep = field.add(MobType::Sheep, 0.5, 0.5);

    // Suffocation, drowning and the void all arrive as `attackEntityFrom(null,
    // n)`, and `bo` checks the attacker is an `EntityLiving` before it drops
    // anything -- so a sheep that drowns keeps its coat.
    CHECK(field.mobs.attack(field.w(), sheep, 1, false));
    CHECK(field.drops.drops.empty());
    CHECK(!field.mobs[sheep].flag);

    field.run(kHurtGap, watching());
    CHECK(field.mobs.attack(field.w(), sheep, 1, true));
    CHECK(field.mobs[sheep].flag);
    const int wool = field.drops.total();
    CHECK(wool >= 1 && wool <= 3);
    for (const test::Drop& drop : field.drops.drops) {
        CHECK_EQ(int(drop.item), int(mcver::Block::Wool));
    }

    // Shearing is once, whatever happens afterwards -- including dying.
    field.drops.drops.clear();
    for (int hit = 0; hit < 20 && field.mobs[sheep].health > 0; ++hit) {
        field.mobs.attack(field.w(), sheep, 1, true);
        field.run(kHurtGap, watching());
    }
    CHECK(field.drops.drops.empty());
}

// ---------------------------------------------------------------------------
// The right click
// ---------------------------------------------------------------------------

TEST(a_bucket_held_at_a_cow_comes_back_full_of_milk)
{
    Field field;
    const int cow = field.add(MobType::Cow, 0.5, 0.5);

    const MobSystem::Interaction milked =
        field.mobs.interact(field.w(), cow, u16(mcver::Item::Bucket));
    CHECK(milked.taken);
    CHECK_EQ(int(milked.becomes), int(mcver::Item::MilkBucket));

    // An empty hand gets nothing, and neither does a pig with a bucket.
    CHECK(!field.mobs.interact(field.w(), cow, 0).taken);
    const int pig = field.add(MobType::Pig, 2.5, 0.5);
    CHECK(!field.mobs.interact(field.w(), pig, u16(mcver::Item::Bucket)).taken);
}

TEST(a_saddle_goes_on_a_pig_once_and_then_the_pig_carries_you)
{
    Field field;
    const int pig = field.add(MobType::Pig, 0.5, 0.5);

    MobSystem::Interaction answer =
        field.mobs.interact(field.w(), pig, u16(mcver::Item::Saddle));
    CHECK(answer.taken);
    CHECK_EQ(int(answer.becomes), 0);  // spent
    CHECK(field.mobs[pig].flag);
    CHECK_EQ(field.mobs.riddenIndex(), -1);

    // **A saddle held out to a saddled pig mounts it**, because `interact` is
    // asked before the held item is -- so the second saddle is not eaten.
    answer = field.mobs.interact(field.w(), pig, u16(mcver::Item::Saddle));
    CHECK(answer.taken);
    CHECK_EQ(int(answer.becomes), int(mcver::Item::Saddle));
    CHECK_EQ(field.mobs.riddenIndex(), pig);

    const entity::RiderSeat seat = field.mobs.seat();
    CHECK(seat.valid);
    // `getMountedYOffset` is `height * 0.75`, and nothing overrides it.
    CHECK(std::abs(seat.y - (Field::kGround + double(mobDef(MobType::Pig).height) * 0.75))
          < 1e-9);

    field.mobs.dismount();
    CHECK_EQ(field.mobs.riddenIndex(), -1);

    // A saddle is no use on anything else.
    const int cow = field.add(MobType::Cow, 2.5, 0.5);
    CHECK(!field.mobs.interact(field.w(), cow, u16(mcver::Item::Saddle)).taken);
}

TEST(an_unsaddled_pig_cannot_be_ridden)
{
    Field field;
    const int pig = field.add(MobType::Pig, 0.5, 0.5);
    CHECK(!field.mobs.mount(pig));
    CHECK_EQ(field.mobs.riddenIndex(), -1);
}

// ---------------------------------------------------------------------------
// The chicken's egg
// ---------------------------------------------------------------------------

TEST(a_chicken_lays_an_egg_between_five_and_ten_minutes)
{
    Field field;
    const int chicken = field.add(MobType::Chicken, 0.5, 0.5);
    const i32 first = field.mobs[chicken].eggTime;
    CHECK(first >= entity::kEggTimeBase);
    CHECK(first < entity::kEggTimeBase + entity::kEggTimeSpread);

    // Run past the clock and check exactly one egg came out, and that the next
    // one is a fresh draw rather than the same number again.
    field.run(int(first) + 1, watching());
    int eggs = 0;
    for (const test::Drop& drop : field.drops.drops) {
        if (drop.item == u16(mcver::Item::Egg)) {
            ++eggs;
        }
    }
    CHECK_EQ(eggs, 1);
    CHECK(field.mobs[chicken].eggTime >= entity::kEggTimeBase);
}

TEST(a_falling_chicken_keeps_three_fifths_of_its_descent)
{
    // `mz.j()`: `if (!onGround && motionY < 0) motionY *= 0.6`. Dropped from a
    // height, a chicken should be much slower than gravity alone would make it.
    Field field;
    const int chicken = field.add(MobType::Chicken, 0.5, 0.5);
    field.mobs.at(chicken).body.setFeet(0.5, Field::kGround + 20.0, 0.5);
    field.mobs.at(chicken).body.onGround = false;

    double fastest = 0.0;
    for (int i = 0; i < 40 && field.mobs[chicken].body.y > Field::kGround; ++i) {
        field.run(1, watching());
        const double speed = -field.mobs[chicken].body.motionY;
        if (speed > fastest) {
            fastest = speed;
        }
    }
    // Terminal speed with the 0.6 damping settles near 0.12 a tick; without it
    // a forty-tick fall reaches about 0.6.
    CHECK(fastest < 0.2);
}

// ---------------------------------------------------------------------------
// Wandering
// ---------------------------------------------------------------------------

TEST(an_animal_left_alone_walks_and_stays_on_the_ground)
{
    Field field;
    const int cow = field.add(MobType::Cow, 0.5, 0.5);
    const double startX = field.mobs[cow].body.x;
    const double startZ = field.mobs[cow].body.z;

    double lowest = Field::kGround;
    double travelled = 0.0;
    for (int i = 0; i < 600; ++i) {
        field.run(1, watching(0.0, 0.0));
        if (field.mobs.count() == 0) {
            break;
        }
        const Mob& mob = field.mobs[cow];
        if (mob.body.y < lowest) {
            lowest = mob.body.y;
        }
        const double dx = mob.body.x - startX;
        const double dz = mob.body.z - startZ;
        const double distance = std::sqrt(dx * dx + dz * dz);
        if (distance > travelled) {
            travelled = distance;
        }
    }
    CHECK_EQ(field.mobs.count(), 1);
    // It never fell through the field...
    CHECK(std::abs(lowest - Field::kGround) < 1e-6);
    // ...and it did go somewhere. Thirty seconds of wandering with a path
    // search about every four seconds covers several blocks.
    CHECK(travelled > 1.0);
}

TEST(a_penned_animal_stays_in_its_pen)
{
    // Four walls two blocks high around a 5 x 5 of grass. Nothing inside may
    // get out: the wander picks cells outside the pen and the pathfinder has to
    // refuse them.
    Field field;
    for (i32 x = -3; x <= 3; ++x) {
        for (i32 z = -3; z <= 3; ++z) {
            if (x == -3 || x == 3 || z == -3 || z == 3) {
                field.scene.place(x, 64, z, bid(mcver::Block::Stone), 0);
                field.scene.place(x, 65, z, bid(mcver::Block::Stone), 0);
            }
        }
    }
    const int pig = field.add(MobType::Pig, 0.5, 0.5);
    for (int i = 0; i < 400; ++i) {
        field.run(1, watching());
        const Mob& mob = field.mobs[pig];
        CHECK(mob.body.box.minX > -3.0);
        CHECK(mob.body.box.maxX < 3.0);
        CHECK(mob.body.box.minZ > -3.0);
        CHECK(mob.body.box.maxZ < 3.0);
        CHECK(mob.body.y >= Field::kGround - 1e-6);
    }
}

TEST(the_pathfinder_finds_its_way_around_a_wall)
{
    // A wall across the field with a gap in it. A path from one side to the
    // other has to go through the gap, so the route's cells must include it.
    Field field;
    // **Two blocks tall**, and the second one is the point: a one-block wall is
    // a step, and the pathfinder's `getSafePoint` climbs it -- which is what a
    // mob does too. **Long, too**: a short wall is walked around, and going
    // round is a shorter path than going through, so a test built on a stub of
    // a wall proves nothing.
    for (i32 z = -20; z <= 20; ++z) {
        if (z == 0) {
            continue;  // the gap
        }
        field.scene.place(0, 64, z, bid(mcver::Block::Stone), 0);
        field.scene.place(0, 65, z, bid(mcver::Block::Stone), 0);
    }

    entity::PathFinder finder;
    entity::PathRoute route;
    const AABB start{-4.45, Field::kGround, 0.05, -3.55, Field::kGround + 0.9, 0.95};
    CHECK(finder.find(field.w(), start, 0.9f, 0.9f, 3.5, Field::kGround, 0.5,
                      entity::kPathSearchRange, &route));
    CHECK(route.count > 0);

    bool throughGap = false;
    bool throughWall = false;
    for (int i = 0; i < route.count; ++i) {
        if (route.steps[i].x == 0) {
            if (route.steps[i].z == 0) {
                throughGap = true;
            } else if (route.steps[i].z >= -20 && route.steps[i].z <= 20) {
                throughWall = true;
            }
        }
    }
    CHECK(throughGap);
    CHECK(!throughWall);
}

TEST(the_pathfinder_refuses_a_target_it_cannot_reach_and_allocates_nothing_twice)
{
    // Sealed in a box: no route out, so the search has to answer no rather than
    // walk the whole arena.
    Field field;
    for (i32 x = -2; x <= 2; ++x) {
        for (i32 z = -2; z <= 2; ++z) {
            for (int y = 64; y <= 66; ++y) {
                if (x == -2 || x == 2 || z == -2 || z == 2 || y == 66) {
                    field.scene.place(x, y, z, bid(mcver::Block::Stone), 0);
                }
            }
        }
    }
    entity::PathFinder finder;
    entity::PathRoute route;
    const AABB start{0.05, Field::kGround, 0.05, 0.95, Field::kGround + 0.9, 0.95};
    CHECK(!finder.find(field.w(), start, 0.9f, 0.9f, 9.5, Field::kGround, 9.5,
                       entity::kPathSearchRange, &route));
    CHECK_EQ(int(route.count), 0);
    CHECK_EQ(finder.searches(), 1u);

    // A second search reuses the same arena -- the node count is a high-water
    // mark, not a total, so it must not climb without bound.
    const int high = finder.highWater();
    CHECK(!finder.find(field.w(), start, 0.9f, 0.9f, 9.5, Field::kGround, 9.5,
                       entity::kPathSearchRange, &route));
    CHECK_EQ(finder.highWater(), high);
}

TEST(two_animals_in_the_same_spot_push_each_other_apart)
{
    // `ge.j()`'s tail: everything within a box a fifth of a block wider gets
    // shoved. Without it a herd stands in one cell and reads as one animal.
    Field field;
    field.add(MobType::Cow, 0.5, 0.5);
    field.add(MobType::Cow, 0.6, 0.5);

    field.run(20, watching(8.0, 8.0));
    const double dx = field.mobs[0].body.x - field.mobs[1].body.x;
    const double dz = field.mobs[0].body.z - field.mobs[1].body.z;
    CHECK(std::sqrt(dx * dx + dz * dz) > 0.3);
}

// ---------------------------------------------------------------------------
// Despawning
// ---------------------------------------------------------------------------

TEST(an_animal_further_than_128_blocks_from_the_player_goes)
{
    Field field;
    field.add(MobType::Pig, 0.5, 0.5);
    // The player is 200 blocks away, which is past the 16,384 squared.
    MobSurroundings far = watching(200.0, 0.0);
    for (int i = 0; i < 200 && field.mobs.count() > 0; ++i) {
        field.run(1, far);
    }
    CHECK_EQ(field.mobs.count(), 0);
    // **Despawning leaves nothing** -- it is `setEntityDead`, not a death.
    CHECK(field.drops.drops.empty());
}

// **There is no way to keep one.** Wheat is bread and nothing else in this
// version, so a right click with it is refused and the animal is no more
// permanent for having been offered any.
TEST(nothing_a_player_holds_stops_an_animal_despawning)
{
    Field field;
    const int pig = field.add(MobType::Pig, 0.5, 0.5);
    CHECK(!field.mobs.interact(field.w(), pig, u16(mcver::Item::Wheat)).taken);

    MobSurroundings far = watching(200.0, 0.0);
    for (int i = 0; i < 200 && field.mobs.count() > 0; ++i) {
        field.run(1, far);
    }
    CHECK_EQ(field.mobs.count(), 0);
}

// ---------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------

TEST(an_animal_spawns_only_on_lit_grass_with_room_to_stand)
{
    Field field;
    // Grass, lit, clear: yes.
    CHECK(entity::canAnimalSpawnAt(field.w(), field.mobs, MobType::Cow, 0.5, Field::kGround,
                                   0.5));
    // Stone under it: no.
    field.scene.place(4, 63, 4, bid(mcver::Block::Stone), 0);
    CHECK(!entity::canAnimalSpawnAt(field.w(), field.mobs, MobType::Cow, 4.5, Field::kGround,
                                    4.5));
    // Grass but no headroom: no.
    field.scene.place(6, 64, 6, bid(mcver::Block::Stone), 0);
    CHECK(!entity::canAnimalSpawnAt(field.w(), field.mobs, MobType::Cow, 6.5, Field::kGround,
                                    6.5));
    // Grass, clear, but standing in water: no.
    field.scene.place(8, 64, 8, bid(mcver::Block::Water), 0);
    CHECK(!entity::canAnimalSpawnAt(field.w(), field.mobs, MobType::Cow, 8.5, Field::kGround,
                                    8.5));
    // Somewhere an animal already is: no.
    field.add(MobType::Cow, 10.5, 10.5);
    CHECK(!entity::canAnimalSpawnAt(field.w(), field.mobs, MobType::Cow, 10.5, Field::kGround,
                                    10.5));
}

TEST(the_spawner_fills_a_field_and_then_stops_at_fifteen)
{
    Field field{40};
    JavaRandom rand(4242);
    entity::SpawnContext where;
    where.playerPresent = true;
    // Far enough that nothing spawns within 24 blocks of the player, and the
    // world spawn is further still.
    where.playerX = 0.0;
    where.playerY = Field::kGround;
    where.playerZ = 0.0;
    where.spawnX = 500;
    where.spawnY = 64;
    where.spawnZ = 500;

    int total = 0;
    for (int tick = 0; tick < 4000 && field.mobs.animalCount() < entity::kAnimalSpawnCap;
         ++tick) {
        total += entity::spawnAnimals(field.w(), field.mobs, rand, where);
    }
    CHECK(total > 0);
    CHECK(field.mobs.animalCount() >= entity::kAnimalSpawnCap);

    // Every one of them is on grass, and none is inside the player's 24 blocks.
    for (int i = 0; i < field.mobs.count(); ++i) {
        const Mob& mob = field.mobs[i];
        const i32 bx = i32(std::floor(mob.body.x));
        const int by = int(std::floor(mob.body.y));
        const i32 bz = i32(std::floor(mob.body.z));
        CHECK_EQ(int(field.w().blockAt(bx, by - 1, bz)), int(mcver::Block::Grass));
        const double dx = mob.body.x - where.playerX;
        const double dz = mob.body.z - where.playerZ;
        CHECK(dx * dx + dz * dz >= 24.0 * 24.0 - 1.0);
    }

    // **The cap is on spawning, not on the pool**: past it, nothing new.
    const int settled = field.mobs.animalCount();
    for (int tick = 0; tick < 200; ++tick) {
        CHECK_EQ(entity::spawnAnimals(field.w(), field.mobs, rand, where), 0);
    }
    CHECK_EQ(field.mobs.animalCount(), settled);
}

TEST(nothing_spawns_in_the_dark)
{
    // `ag.a()Z` wants a light level **above** eight, and the light it reads is
    // the day's -- which is why animals stop appearing at dusk. The field is
    // built lit; putting it out must stop the spawner dead.
    Field field{40};
    field.scene.lightColumnsFrom(-40, 40, -40, 40, 0);
    for (i32 x = -40; x <= 40; ++x) {
        for (i32 z = -40; z <= 40; ++z) {
            for (int y = 60; y < 80; ++y) {
                field.scene.setSkyLight(x, y, z, 0);
            }
        }
    }
    CHECK(!entity::canAnimalSpawnAt(field.w(), field.mobs, MobType::Cow, 0.5, Field::kGround,
                                    0.5));

    JavaRandom rand(99);
    entity::SpawnContext where;
    where.playerPresent = true;
    where.playerX = 0.0;
    where.playerY = Field::kGround;
    where.playerZ = 0.0;
    where.spawnX = 500;
    where.spawnY = 64;
    where.spawnZ = 500;
    for (int tick = 0; tick < 2000; ++tick) {
        CHECK_EQ(entity::spawnAnimals(field.w(), field.mobs, rand, where), 0);
    }
}

// ---------------------------------------------------------------------------
// The crosshair, the click, and the pools
// ---------------------------------------------------------------------------

TEST(the_crosshair_finds_an_animal_and_the_click_hits_it)
{
    Field field;
    const int cow = field.add(MobType::Cow, 0.5, 3.5);

    item::EntityPools pools;
    pools.mobs = &field.mobs;

    const entity::RayHit noBlock =
        entity::rayTrace(field.w(), 0.5, Field::kGround + 1.0, 0.5, 0.0, 0.0, 1.0);
    const item::EntityTarget target = item::pickEntity(pools, 0.5, Field::kGround + 1.0, 0.5,
                                                       0.0, 0.0, 1.0, noBlock);
    CHECK(target.found());
    CHECK_EQ(int(target.kind), int(item::EntityTarget::Kind::Mob));
    CHECK_EQ(target.index, cow);

    item::Effects effects;
    effects.entities = pools;
    item::Attacker attacker;
    attacker.present = true;
    attacker.x = 0.5;
    attacker.z = 0.5;
    CHECK(item::attackEntity(field.w(), target, kEmptyHand, effects, attacker));
    CHECK_EQ(field.mobs[cow].health, 9);
    // Knocked away from the attacker, which here is straight along +z.
    CHECK(field.mobs[cow].body.motionZ > 0.0);

    // And the right click reaches `interact`.
    const item::EntityInteraction answer =
        item::interactWithEntity(field.w(), target, pools, u16(mcver::Item::Bucket));
    CHECK(answer.taken);
    CHECK_EQ(int(answer.becomes), int(mcver::Item::MilkBucket));
}

// ---------------------------------------------------------------------------
// The models
// ---------------------------------------------------------------------------

TEST(every_animal_poses_the_boxes_its_class_file_builds)
{
    Field field;
    const int sheep = field.add(MobType::Sheep, 0.5, 0.5);
    const int pig = field.add(MobType::Pig, 2.5, 0.5);
    const int cow = field.add(MobType::Cow, 4.5, 0.5);
    const int chicken = field.add(MobType::Chicken, 6.5, 0.5);

    render::ModelPart parts[render::kMobMaxParts];
    texture::EntitySkin skins[render::kMobMaxParts];

    // A sheep with its fleece is two models of six; shorn, one.
    CHECK_EQ(render::poseMob(field.mobs[sheep], 1.0f, parts, skins, render::kMobMaxParts), 12);
    CHECK_EQ(int(skins[0]), int(texture::EntitySkin::Sheep));
    CHECK_EQ(int(skins[6]), int(texture::EntitySkin::SheepFur));
    field.mobs.at(sheep).flag = true;
    CHECK_EQ(render::poseMob(field.mobs[sheep], 1.0f, parts, skins, render::kMobMaxParts), 6);

    // A pig is six, and a saddled one twelve.
    CHECK_EQ(render::poseMob(field.mobs[pig], 1.0f, parts, skins, render::kMobMaxParts), 6);
    CHECK_EQ(int(skins[0]), int(texture::EntitySkin::Pig));
    field.mobs.at(pig).flag = true;
    CHECK_EQ(render::poseMob(field.mobs[pig], 1.0f, parts, skins, render::kMobMaxParts), 12);
    CHECK_EQ(int(skins[6]), int(texture::EntitySkin::Saddle));

    // A cow has horns and an udder: nine.
    CHECK_EQ(render::poseMob(field.mobs[cow], 1.0f, parts, skins, render::kMobMaxParts), 9);
    // The udder is laid flat like the body -- a quarter turn about x.
    CHECK(std::abs(parts[8].angleX - 3.1415927f / 2.0f) < 1e-5f);

    // A chicken has eight: head, bill, chin, body, two legs and two wings.
    CHECK_EQ(render::poseMob(field.mobs[chicken], 1.0f, parts, skins, render::kMobMaxParts), 8);
    CHECK_EQ(int(skins[0]), int(texture::EntitySkin::Chicken));
}

TEST(a_drawn_animal_stands_on_its_own_feet)
{
    // The composed transform is the part of `RenderLiving` that can be wrong in
    // an interesting way: the model is built upside down, in 1/16 units, about
    // a point 24 units above its feet. The lowest vertex it produces should
    // land on the ground the entity is standing on.
    Field field;
    const int cow = field.add(MobType::Cow, 0.5, 0.5);
    field.mobs.at(cow).body.snapRenderPosition();

    // **The origin has to be near the animal.** A `DetailVertex` position is a
    // signed short in 1/1024 of a block, so the builder skips anything more
    // than about 31 blocks from the origin -- the renderer passes the eye's own
    // block for exactly this reason.
    static std::vector<mesh::DetailVertex> verts(render::kMobMaxVertices);
    const int written = render::buildMobs(field.mobs, 0.0, Field::kGround, 0.0, 1.0f,
                                          verts.data(), render::kMobMaxVertices);
    CHECK(written > 0);

    double lowest = 1e9;
    double highest = -1e9;
    for (int i = 0; i < written; ++i) {
        const double y = Field::kGround
                         + double(verts[i].y) / double(mesh::kDetailUnitsPerBlock);
        if (y < lowest) {
            lowest = y;
        }
        if (y > highest) {
            highest = y;
        }
    }
    // The feet are an eighth of a model unit above the ground; the top of a cow
    // is about 1.4 blocks up -- taller than its collision box, because a head
    // sticks out of it.
    CHECK(std::abs(lowest - Field::kGround) < 0.02);
    CHECK(highest > Field::kGround + 1.0);
    CHECK(highest < Field::kGround + 2.0);
}

TEST(a_hurt_animal_is_tinted_and_a_well_one_is_not)
{
    Field field;
    const int pig = field.add(MobType::Pig, 0.5, 0.5);
    field.mobs.at(pig).body.snapRenderPosition();

    static std::vector<mesh::DetailVertex> verts(render::kMobMaxVertices);
    int written = render::buildMobs(field.mobs, 0.0, Field::kGround, 0.0, 1.0f, verts.data(),
                                    render::kMobMaxVertices);
    CHECK(written > 0);
    CHECK_EQ(int(verts[0].g), 255);

    field.mobs.attack(field.w(), pig, 1, true);
    written = render::buildMobs(field.mobs, 0.0, Field::kGround, 0.0, 1.0f, verts.data(),
                                render::kMobMaxVertices);
    CHECK(written > 0);
    CHECK(verts[0].g < 255);
    CHECK_EQ(int(verts[0].r), 255);
}

TEST(every_animal_page_is_inside_the_sheet)
{
    const texture::EntitySkin pages[] = {
        texture::EntitySkin::Pig,      texture::EntitySkin::Saddle,
        texture::EntitySkin::Cow,      texture::EntitySkin::Sheep,
        texture::EntitySkin::SheepFur, texture::EntitySkin::Chicken,
    };
    for (texture::EntitySkin page : pages) {
        int x = 0;
        int y = 0;
        texture::skinOrigin(page, &x, &y);
        CHECK(x >= 0 && y >= 0);
        CHECK(x + texture::kSkinPageWidth <= texture::kEntitySheetWidth);
        CHECK(y + texture::kSkinPageHeight <= texture::kEntitySheetHeight);
        // And none of them lands on top of a page that was there before the
        // sheet grew -- everything above y = 64 is the old layout.
        CHECK(y >= 64);
    }
}
