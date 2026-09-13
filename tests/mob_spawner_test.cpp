// The mob spawner block -- `bd`, `bj` and `r`.
//
// This is the *block*, not `az`/`k`: those are the world's own spawners and
// tests/monster_test.cpp covers them. Read core/entity/mob_spawner.hpp first;
// this file checks it and does not repeat the derivation.
//
// There is no oracle suite here for the same reason the mob tests have none --
// every draw comes out of a `Random` a1.1.2 seeds from the wall clock -- so
// what is pinned is the arithmetic that is fixed (the spin rate, the delay
// range, the scatter's shape), the order the draws happen in, and the things a
// player would notice: a spawner nobody is near doing nothing at all, one that
// fills its room and stops, and a miniature that stays inside its cage.

#include "core/block/registry.hpp"
#include "core/entity/mob_spawner.hpp"
#include "core/entity/particle.hpp"
#include "core/item/registry.hpp"
#include "core/render/spawner_mesh.hpp"
#include "core/world/tile_entity.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/tick_world.hpp"
#include "framework.hpp"
#include "scene_world.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace mc;
using mc::block::BlockId;
using mc::entity::kSpawnerDelayBase;
using mc::entity::kSpawnerDelaySpread;
using mc::entity::kSpawnerStartDelay;
using mc::entity::MobSpawnerBlock;
using mc::entity::MobSpawnerCounters;
using mc::entity::MobSpawnerStore;
using mc::entity::MobSystem;
using mc::entity::MobType;
using mc::entity::SpawnContext;
using mc::test::SceneWorld;

namespace {

BlockId bid(mcver::Block b) { return BlockId(b); }

// Every particle the tick makes, in order -- the pair is two calls and the
// burst is forty, so the count alone tells the three cases apart.
struct ParticleCatcher {
    struct Puff {
        int kind;
        double x, y, z;
    };
    std::vector<Puff> puffs;

    void watch(tick::TickWorld& world) { world.setParticleSink(&sink, this); }

    static void sink(void* ctx, int kind, double x, double y, double z, double, double,
                     double)
    {
        static_cast<ParticleCatcher*>(ctx)->puffs.push_back({kind, x, y, z});
    }

    int countOf(entity::ParticleKind kind) const
    {
        int n = 0;
        for (const Puff& p : puffs) {
            if (p.kind == int(kind)) ++n;
        }
        return n;
    }
};

// **A cave with a spawner in it**, which is the only place one ever works: a
// stone floor at y = 40, a roof at y = 46, no light between them, and the
// spawner block in the middle of the floor's air.
struct Room {
    ParticleCatcher particles;
    SceneWorld scene{0, 0};
    MobSystem mobs{4242};
    MobSpawnerStore store;
    JavaRandom rand{99887766LL};

    static constexpr int kFloor = 40;
    static constexpr int kSpawnerY = 41;

    explicit Room(i32 half = 12)
    {
        for (i32 x = -half; x <= half; ++x) {
            for (i32 z = -half; z <= half; ++z) {
                scene.place(x, kFloor, z, bid(mcver::Block::Stone), 0);
                scene.place(x, 46, z, bid(mcver::Block::Stone), 0);
            }
        }
        particles.watch(scene.w());
        scene.w().setEntityQuery(&Room::query, this);
    }

    static bool query(void* ctx, const AABB& box, tick::EntityFilter)
    {
        const Room& self = *static_cast<const Room*>(ctx);
        for (int i = 0; i < self.mobs.count(); ++i) {
            if (self.mobs[i].alive && self.mobs[i].body.box.intersects(box)) {
                return true;
            }
        }
        return false;
    }

    tick::TickWorld& w() { return scene.w(); }

    int place(std::string_view mob, int delay = kSpawnerStartDelay)
    {
        scene.place(0, kSpawnerY, 0, bid(mcver::Block::MobSpawner), 0);
        return store.put(0, kSpawnerY, 0, mob, delay);
    }

    SpawnContext near(double distance = 4.0) const
    {
        SpawnContext context;
        context.playerPresent = true;
        context.playerX = distance;
        context.playerY = double(kSpawnerY);
        context.playerZ = 0.0;
        context.difficulty = 2;
        context.worldSeed = 12345LL;
        return context;
    }

    int run(int ticks, const SpawnContext& context, MobSpawnerCounters* counters = nullptr)
    {
        int placed = 0;
        for (int i = 0; i < ticks; ++i) {
            placed += entity::tickMobSpawners(store, w(), mobs, rand, context, counters);
        }
        return placed;
    }
};

}  // namespace

TEST(a_new_spawner_is_a_pig_on_a_twenty_tick_delay)
{
    MobSpawnerStore store;
    const int index = store.put(3, 40, -7);
    CHECK(index >= 0);
    const MobSpawnerBlock& s = store[index];
    CHECK_EQ(int(s.mob), int(MobType::Pig));
    CHECK(s.known);
    CHECK_EQ(std::string(s.entityId), std::string("Pig"));
    CHECK_EQ(s.delay, kSpawnerStartDelay);
    CHECK_EQ(s.yaw, 0.0);
    CHECK_EQ(store.find(3, 40, -7), index);
    CHECK_EQ(store.find(3, 41, -7), -1);
}

TEST(every_name_the_nine_mobs_save_under_comes_back)
{
    for (int i = 0; i < entity::kMobTypeCount; ++i) {
        const MobType type = MobType(i);
        MobType round = MobType::Pig;
        CHECK(entity::mobTypeForSaveId(entity::mobDef(type).saveId, &round));
        CHECK_EQ(int(round), i);
    }
    // In `ew` but not an `ge` this build models, and not in `ew` at all.
    CHECK(!entity::mobTypeForSaveId("Giant", nullptr));
    CHECK(!entity::mobTypeForSaveId("Minecart", nullptr));
    CHECK(!entity::mobTypeForSaveId("", nullptr));
    CHECK(!entity::mobTypeForSaveId("Enderman", nullptr));
}

TEST(rebuilding_a_spawner_in_the_same_hole_starts_it_over)
{
    MobSpawnerStore store;
    const int first = store.put(0, 40, 0, "Zombie", 500);
    CHECK(first >= 0);
    store.at(first).yaw = 123.0;
    // `jt.e` builds a fresh `bd`; nothing of the old one survives.
    const int again = store.put(0, 40, 0);
    CHECK_EQ(again, first);
    CHECK_EQ(int(store[again].mob), int(MobType::Pig));
    CHECK_EQ(store[again].delay, kSpawnerStartDelay);
    CHECK_EQ(store[again].yaw, 0.0);
    CHECK_EQ(store.count(), 1);
}

TEST(a_column_that_leaves_takes_its_spawners_with_it)
{
    MobSpawnerStore store;
    CHECK(store.put(2, 40, 3) >= 0);        // chunk 0, 0
    CHECK(store.put(17, 40, 3) >= 0);       // chunk 1, 0
    CHECK(store.put(-1, 40, -1) >= 0);      // chunk -1, -1
    store.eraseColumn(0, 0);
    CHECK_EQ(store.count(), 2);
    CHECK_EQ(store.find(2, 40, 3), -1);
    CHECK(store.find(17, 40, 3) >= 0);
    CHECK(store.find(-1, 40, -1) >= 0);
}

// ---------------------------------------------------------------------------
// `bd.b()`
// ---------------------------------------------------------------------------

TEST(a_spawner_nobody_is_near_does_nothing_at_all)
{
    Room room;
    room.place("Zombie", 1);
    SpawnContext away = room.near();
    away.playerX = 40.0;

    MobSpawnerCounters counters;
    room.run(200, away, &counters);

    CHECK_EQ(room.store[0].delay, 1);
    CHECK_EQ(room.store[0].yaw, 0.0);
    CHECK_EQ(int(room.particles.puffs.size()), 0);
    CHECK_EQ(counters.visited, 200);
    CHECK_EQ(counters.inRange, 0);
    CHECK_EQ(room.mobs.count(), 0);
}

TEST(range_is_a_sphere_of_sixteen_about_the_cell_centre)
{
    Room room;
    room.place("Zombie", 1000);

    SpawnContext justInside = room.near();
    justInside.playerX = 0.5 + 15.9;
    MobSpawnerCounters in;
    room.run(1, justInside, &in);
    CHECK_EQ(in.inRange, 1);

    SpawnContext justOutside = room.near();
    justOutside.playerX = 0.5 + 16.1;
    MobSpawnerCounters out;
    room.run(1, justOutside, &out);
    CHECK_EQ(out.inRange, 0);
}

TEST(a_spawner_in_range_smokes_and_turns_every_tick)
{
    Room room;
    room.place("Zombie", 1000);
    room.run(1, room.near());

    // One `smoke` and one `flame`, and nothing else.
    CHECK_EQ(int(room.particles.puffs.size()), 2);
    CHECK_EQ(room.particles.countOf(entity::ParticleKind::Smoke), 1);
    CHECK_EQ(room.particles.countOf(entity::ParticleKind::Flame), 1);
    // Both at the same point, somewhere in the cell.
    const auto& a = room.particles.puffs[0];
    const auto& b = room.particles.puffs[1];
    CHECK_EQ(a.x, b.x);
    CHECK_EQ(a.y, b.y);
    CHECK_EQ(a.z, b.z);
    CHECK(a.x >= 0.0 && a.x < 1.0);
    CHECK(a.y >= double(Room::kSpawnerY) && a.y < double(Room::kSpawnerY) + 1.0);
    CHECK(a.z >= 0.0 && a.z < 1.0);
}

TEST(the_spin_is_a_thousand_over_the_delay_plus_two_hundred)
{
    Room room;
    room.place("Zombie", 1000);
    const double before = room.store[0].yaw;
    room.run(1, room.near());
    const MobSpawnerBlock& s = room.store[0];
    // The delay is spent *after* the spin, so the rate uses the old value.
    const double expected = double(1000.0f / (1000.0f + 200.0f));
    CHECK_EQ(s.prevYaw, before);
    CHECK(std::fabs(s.yaw - expected) < 1e-9);
    CHECK_EQ(s.delay, 999);
}

TEST(the_spin_speeds_up_as_the_delay_runs_out)
{
    Room slow;
    slow.place("Zombie", 600);
    slow.run(1, slow.near());

    Room fast;
    fast.place("Zombie", 1);
    fast.run(1, fast.near());

    // 1000/800 against 1000/201 -- about four times as fast on the tick before
    // it fires, and the renderer multiplies both by ten.
    CHECK(fast.store[0].yaw > slow.store[0].yaw * 3.0);
    CHECK(slow.store[0].yaw > 0.0);
}

TEST(the_spin_wraps_at_three_sixty_and_carries_the_previous_angle_with_it)
{
    // A delay of 800 makes the step exactly one degree, so the wrap is easy to
    // read: 359.99 + 1 is 360.99, which is over.
    Room room;
    const int index = room.place("Zombie", 800);
    room.store.at(index).yaw = 359.99;
    room.run(1, room.near());
    const MobSpawnerBlock& s = room.store[index];
    // `d = c` happens first, so the previous angle is 359.99 and not whatever
    // the frame before left; then **both** come down by 360, which is the part
    // that matters -- the pair the renderer interpolates stays one degree apart
    // instead of spinning a whole turn backwards in a single frame.
    CHECK(std::fabs(s.yaw - 0.99) < 1e-9);
    CHECK(std::fabs(s.prevYaw - -0.01) < 1e-9);
    CHECK(std::fabs((s.yaw - s.prevYaw) - 1.0) < 1e-9);
}

TEST(a_minus_one_delay_seeds_itself_rather_than_firing)
{
    Room room;
    room.place("Zombie", -1);
    MobSpawnerCounters counters;
    room.run(1, room.near(), &counters);
    const int delay = room.store[0].delay;
    CHECK(delay >= kSpawnerDelayBase);
    CHECK(delay < kSpawnerDelayBase + kSpawnerDelaySpread);
    CHECK_EQ(counters.fired, 0);
    CHECK_EQ(room.mobs.count(), 0);
}

TEST(a_delay_runs_down_one_tick_at_a_time)
{
    Room room;
    room.place("Zombie", 5);
    const SpawnContext context = room.near();
    for (int expected = 4; expected >= 0; --expected) {
        room.run(1, context);
        CHECK_EQ(room.store[0].delay, expected);
    }
    // The sixth tick is the firing, and it seeds a fresh delay.
    MobSpawnerCounters counters;
    room.run(1, context, &counters);
    CHECK_EQ(counters.fired, 1);
}

TEST(a_fired_spawner_fills_a_dark_room_with_its_own_mob)
{
    Room room;
    room.place("Zombie", 0);
    MobSpawnerCounters counters;
    const int placed = room.run(1, room.near(), &counters);

    CHECK_EQ(counters.fired, 1);
    CHECK(placed > 0);
    CHECK(placed <= entity::kSpawnerAttempts);
    CHECK_EQ(room.mobs.count(), placed);
    for (int i = 0; i < room.mobs.count(); ++i) {
        CHECK_EQ(int(room.mobs[i].type), int(MobType::Zombie));
        // Inside the scatter, which is four blocks flat and one either way up.
        CHECK(std::fabs(room.mobs[i].body.x) <= 4.0);
        CHECK(std::fabs(room.mobs[i].body.z) <= 4.0);
        CHECK(room.mobs[i].body.y >= double(Room::kSpawnerY - 1));
        CHECK(room.mobs[i].body.y <= double(Room::kSpawnerY + 1));
    }
    // A fresh delay, and the burst: twenty smoke/flame pairs a spawn, plus the
    // idle pair, plus twenty `explode` puffs a mob.
    CHECK(room.store[0].delay >= kSpawnerDelayBase);
    CHECK(room.store[0].delay < kSpawnerDelayBase + kSpawnerDelaySpread);
    CHECK_EQ(room.particles.countOf(entity::ParticleKind::Smoke),
             1 + placed * entity::kSpawnerBurstPairs);
    CHECK_EQ(room.particles.countOf(entity::ParticleKind::Flame),
             1 + placed * entity::kSpawnerBurstPairs);
    CHECK_EQ(room.particles.countOf(entity::ParticleKind::Explode),
             placed * entity::kExplosionPuffs);
}

TEST(a_lit_room_refuses_every_monster_and_the_spawner_keeps_trying)
{
    Room room;
    room.scene.lightColumnsFrom(-12, 12, -12, 12, Room::kFloor + 1);
    room.place("Zombie", 0);
    MobSpawnerCounters counters;
    const int placed = room.run(1, room.near(), &counters);

    CHECK_EQ(placed, 0);
    CHECK_EQ(room.mobs.count(), 0);
    CHECK_EQ(counters.refused, entity::kSpawnerAttempts);
    // **No fresh delay.** Only a spawn or a crowd seeds one, so a spawner in a
    // lit room fires every tick and keeps failing -- which is what makes
    // torching a dungeon a partial fix in this version rather than a cure.
    CHECK_EQ(room.store[0].delay, 0);
}

TEST(six_of_a_kind_in_the_room_stops_it_and_costs_it_a_delay)
{
    Room room;
    room.place("Zombie", 0);
    for (int i = 0; i < entity::kSpawnerNearbyLimit; ++i) {
        CHECK(room.mobs.spawn(room.w(), MobType::Zombie, double(i) - 2.0,
                              double(Room::kFloor + 1), 0.0, 0.0f));
    }
    MobSpawnerCounters counters;
    const int placed = room.run(1, room.near(), &counters);

    CHECK_EQ(placed, 0);
    CHECK_EQ(counters.crowded, 1);
    CHECK_EQ(room.mobs.count(), entity::kSpawnerNearbyLimit);
    CHECK(room.store[0].delay >= kSpawnerDelayBase);
}

TEST(the_crowd_check_counts_only_the_spawners_own_kind)
{
    Room room;
    room.place("Zombie", 0);
    // Six skeletons are six of the wrong class, so `getEntitiesWithinAABB`
    // returns none of them -- `Zombie.class` is the filter, not `IMob`.
    for (int i = 0; i < entity::kSpawnerNearbyLimit; ++i) {
        CHECK(room.mobs.spawn(room.w(), MobType::Skeleton, double(i) - 2.0,
                              double(Room::kFloor + 1), 0.0, 0.0f));
    }
    MobSpawnerCounters counters;
    room.run(1, room.near(), &counters);
    CHECK_EQ(counters.crowded, 0);
}

TEST(an_entity_id_this_build_cannot_make_is_inert)
{
    Room room;
    const int index = room.place("Giant", 0);
    CHECK(index >= 0);
    CHECK(!room.store[index].known);
    CHECK_EQ(std::string(room.store[index].entityId), std::string("Giant"));

    MobSpawnerCounters counters;
    room.run(5, room.near(), &counters);

    CHECK_EQ(counters.fired, 5);
    CHECK_EQ(counters.unknown, 5);
    CHECK_EQ(room.mobs.count(), 0);
    // It still smokes and still turns -- the return is after both.
    CHECK(room.store[index].yaw > 0.0);
    CHECK_EQ(room.particles.countOf(entity::ParticleKind::Smoke), 5);
    // And it does not seed a delay, so it is still at zero.
    CHECK_EQ(room.store[index].delay, 0);
}

TEST(an_animal_spawner_needs_grass_and_light_where_a_monster_needs_neither)
{
    Room room;
    room.place("Pig", 0);
    MobSpawnerCounters dark;
    room.run(1, room.near(), &dark);
    // `ag.a()Z` wants grass under the feet and a light level above eight; a
    // dark stone floor is neither.
    CHECK_EQ(room.mobs.count(), 0);
    CHECK_EQ(dark.refused, entity::kSpawnerAttempts);

    Room field;
    for (i32 x = -12; x <= 12; ++x) {
        for (i32 z = -12; z <= 12; ++z) {
            field.scene.place(x, Room::kFloor, z, bid(mcver::Block::Grass), 0);
        }
    }
    field.scene.lightColumnsFrom(-12, 12, -12, 12, Room::kFloor + 1);
    field.place("Pig", 0);
    const int placed = field.run(1, field.near());
    CHECK(placed > 0);
    CHECK_EQ(int(field.mobs[0].type), int(MobType::Pig));
}

// ---------------------------------------------------------------------------
// The block
// ---------------------------------------------------------------------------

TEST(placing_the_block_builds_a_tile_entity_and_breaking_it_forgets_one)
{
    struct Hooks {
        MobSpawnerStore* store;
        static void added(void* ctx, i32 x, int y, i32 z)
        {
            static_cast<Hooks*>(ctx)->store->put(x, y, z);
        }
        static void removed(void* ctx, i32 x, int y, i32 z)
        {
            static_cast<Hooks*>(ctx)->store->erase(x, y, z);
        }
    };

    SceneWorld scene{0, 0};
    MobSpawnerStore store;
    Hooks hooks{&store};
    scene.w().setTileEntityAddedSink(&Hooks::added, &hooks);
    scene.w().setTileEntityRemovedSink(&Hooks::removed, &hooks);

    // **Through the world and not through the dispatch directly**, because
    // that is the claim: every way the block can appear runs `jt.e` --
    // BlockContainer.onBlockAdded -- and every way it can go runs `jt.b`.
    scene.place(4, 30, 5, bid(mcver::Block::MobSpawner), 0);
    CHECK_EQ(store.count(), 1);
    CHECK_EQ(int(store[0].mob), int(MobType::Pig));
    CHECK_EQ(store[0].delay, kSpawnerStartDelay);

    scene.place(4, 30, 5, block::kAir, 0);
    CHECK_EQ(store.count(), 0);

    // A block that is not a spawner reaches neither sink.
    scene.place(4, 30, 5, bid(mcver::Block::Stone), 0);
    CHECK_EQ(store.count(), 0);
}

// ---------------------------------------------------------------------------
// What a world file holds
// ---------------------------------------------------------------------------

namespace {

// A tile entity as a column decodes it. The NBT that produced it is the chunk
// codec's, and tests/tile_entity_test.cpp is where that half is checked; this
// file starts from the decoded list, which is what the store is handed.
world::TileEntity spawnerTile(i32 x, int y, i32 z, const char* mob, i16 delay)
{
    world::TileEntity tile =
        world::makeTileEntity(x, y, z, world::TileEntityKind::MobSpawner);
    tile.entityId = mob;
    tile.delay = delay;
    return tile;
}

}  // namespace

TEST(a_chunks_tile_entities_hand_over_their_spawners)
{
    std::vector<world::TileEntity> tiles;
    tiles.push_back(spawnerTile(-37, 22, 194, "Skeleton", 143));
    // A chest in the same room, which this must step over rather than trip on.
    tiles.push_back(world::makeTileEntity(-35, 22, 194, world::TileEntityKind::Chest));
    tiles.push_back(spawnerTile(8, 9, 10, "Creeper", -1));

    MobSpawnerStore store;
    CHECK_EQ(entity::readMobSpawners(tiles, store), 2);
    CHECK_EQ(store.count(), 2);

    const int first = store.find(-37, 22, 194);
    CHECK(first >= 0);
    CHECK_EQ(int(store[first].mob), int(MobType::Skeleton));
    CHECK_EQ(store[first].delay, 143);

    const int second = store.find(8, 9, 10);
    CHECK(second >= 0);
    CHECK_EQ(int(store[second].mob), int(MobType::Creeper));
    CHECK_EQ(store[second].delay, -1);

    // **The list is untouched**, which is what lets the reconcile-then-write
    // pass at the save decide what goes back rather than this.
    CHECK_EQ(int(tiles.size()), 3);
    CHECK_EQ(tiles[1].kind, world::TileEntityKind::Chest);
}

TEST(a_name_from_a_later_version_is_kept_and_made_inert)
{
    std::vector<world::TileEntity> tiles;
    tiles.push_back(spawnerTile(1, 2, 3, "Enderman", 20));

    MobSpawnerStore store;
    CHECK_EQ(entity::readMobSpawners(tiles, store), 1);
    CHECK(!store[0].known);
    CHECK_EQ(std::string(store[0].entityId), std::string("Enderman"));
}

TEST(a_chunk_with_no_tile_entities_hands_over_nothing)
{
    MobSpawnerStore store;
    const std::vector<world::TileEntity> empty;
    CHECK_EQ(entity::readMobSpawners(empty, store), 0);
    CHECK_EQ(store.count(), 0);
}

// **A spawner goes back into the column it stands in, and into no other.**
// This is the half that did not exist: a spawner this build places used to
// reload as `bd`'s default `"Pig"`.
TEST(a_spawner_is_written_back_into_its_own_column)
{
    MobSpawnerStore store;
    CHECK(store.put(20, 41, 37, "Skeleton", 143) >= 0);   // chunk (1, 2)
    CHECK(store.put(-3, 12, 8, "Creeper", -1) >= 0);      // chunk (-1, 0)

    world::ChunkColumn column(1, 2);
    column.setBlock(4, 41, 5, bid(mcver::Block::MobSpawner));
    CHECK_EQ(entity::writeMobSpawners(store, column), 1);
    CHECK_EQ(int(column.tileEntities.size()), 1);

    const world::TileEntity* tile = world::findTileEntity(column.tileEntities, 20, 41, 37);
    CHECK(tile != nullptr);
    CHECK_EQ(tile->kind, world::TileEntityKind::MobSpawner);
    CHECK_EQ(tile->entityId, std::string("Skeleton"));
    CHECK_EQ(tile->delay, 143);
    CHECK_EQ(tile->id, std::string("MobSpawner"));

    // The other column's spawner is untouched by this one's save.
    world::ChunkColumn other(-1, 0);
    other.setBlock(13, 12, 8, bid(mcver::Block::MobSpawner));
    CHECK_EQ(entity::writeMobSpawners(store, other), 1);
    CHECK_EQ(world::findTileEntity(other.tileEntities, -3, 12, 8)->entityId,
             std::string("Creeper"));

    // And a second save overwrites rather than duplicates.
    CHECK_EQ(entity::writeMobSpawners(store, column), 1);
    CHECK_EQ(int(column.tileEntities.size()), 1);
}

// A spawner block the store never took -- the heap refused it, or the column
// arrived after the store filled -- keeps what the file said rather than
// being reset to `"Pig"`.
TEST(a_spawner_the_store_never_took_keeps_its_mob)
{
    world::ChunkColumn column(0, 0);
    column.setBlock(4, 41, 5, bid(mcver::Block::MobSpawner));
    column.tileEntities.push_back(spawnerTile(4, 41, 5, "Skeleton", 143));

    MobSpawnerStore store;
    CHECK_EQ(entity::writeMobSpawners(store, column), 0);
    CHECK_EQ(world::reconcileTileEntities(column), 0);
    CHECK_EQ(int(column.tileEntities.size()), 1);
    CHECK_EQ(column.tileEntities[0].entityId, std::string("Skeleton"));
}

// ---------------------------------------------------------------------------
// `r` -- the renderer
// ---------------------------------------------------------------------------

TEST(the_miniature_stays_inside_its_cage)
{

    MobSpawnerStore store;
    const int index = store.put(0, 0, 0, "Zombie", 500);
    CHECK(index >= 0);

    // Through a full turn, the model's eight corners have to stay within a
    // block or so of the cell -- 0.4375 of a 1.8-block biped leaning 30
    // degrees is what has to fit.
    for (int step = 0; step < 36; ++step) {
        store.at(index).yaw = double(step);
        store.at(index).prevYaw = double(step);
        const render::Placement place =
            render::placeSpawnerMob(store[index], 0.0, 0.0, 0.0, 1.0f);
        // The model's own extent in model units: 24 high, +-8 wide.
        for (int corner = 0; corner < 8; ++corner) {
            const float u = (corner & 1) ? 8.0f : -8.0f;
            const float v = (corner & 2) ? 0.0f : 24.0f;
            const float w = (corner & 4) ? 8.0f : -8.0f;
            const double px = place.x + double(place.ax[0] * u + place.ay[0] * v
                                               + place.az[0] * w);
            const double py = place.y + double(place.ax[1] * u + place.ay[1] * v
                                               + place.az[1] * w);
            const double pz = place.z + double(place.ax[2] * u + place.ay[2] * v
                                               + place.az[2] * w);
            CHECK(px > -0.8 && px < 1.8);
            CHECK(py > -0.6 && py < 1.6);
            CHECK(pz > -0.8 && pz < 1.8);
        }
    }
}

TEST(the_miniature_turns_ten_times_the_tile_entitys_angle)
{

    MobSpawnerStore store;
    const int index = store.put(0, 0, 0, "Zombie", 500);
    // Nine degrees of tile-entity spin is ninety of model spin, which takes the
    // model's own +x axis onto -z (see `placeAt`'s convention).
    store.at(index).yaw = 0.0;
    store.at(index).prevYaw = 0.0;
    const render::Placement at0 = render::placeSpawnerMob(store[index], 0, 0, 0, 1.0f);
    store.at(index).yaw = 9.0;
    store.at(index).prevYaw = 9.0;
    const render::Placement at9 = render::placeSpawnerMob(store[index], 0, 0, 0, 1.0f);

    // |ax| is unchanged by a turn about Y; its direction has swung a quarter.
    const float len0 = std::sqrt(at0.ax[0] * at0.ax[0] + at0.ax[2] * at0.ax[2]);
    const float len9 = std::sqrt(at9.ax[0] * at9.ax[0] + at9.ax[2] * at9.ax[2]);
    CHECK(std::fabs(len0 - len9) < 1e-6f);
    const float dot = at0.ax[0] * at9.ax[0] + at0.ax[2] * at9.ax[2];
    CHECK(std::fabs(dot) < 1e-5f);
}

TEST(the_frame_interpolates_between_the_two_angles)
{

    MobSpawnerStore store;
    const int index = store.put(0, 0, 0, "Zombie", 500);
    store.at(index).prevYaw = 0.0;
    store.at(index).yaw = 18.0;
    const render::Placement half = render::placeSpawnerMob(store[index], 0, 0, 0, 0.5f);
    store.at(index).prevYaw = 9.0;
    store.at(index).yaw = 9.0;
    const render::Placement still = render::placeSpawnerMob(store[index], 0, 0, 0, 1.0f);
    CHECK(std::fabs(half.ax[0] - still.ax[0]) < 1e-6f);
    CHECK(std::fabs(half.ax[2] - still.ax[2]) < 1e-6f);
}

TEST(an_unknown_mob_draws_nothing_and_a_known_one_draws_a_model)
{

    MobSpawnerStore store;
    store.put(0, 0, 0, "Enderman", 500);
    std::vector<mesh::DetailVertex> out(usize(render::kSpawnerMaxVertices));
    CHECK_EQ(render::buildSpawnerMobs(store, 0, 0, 0, 1.0f, out.data(), int(out.size())), 0);

    store.put(0, 0, 0, "Zombie", 500);
    const int written =
        render::buildSpawnerMobs(store, 0, 0, 0, 1.0f, out.data(), int(out.size()));
    // A biped is seven boxes.
    CHECK_EQ(written, 7 * render::kBoxVertices);
}
