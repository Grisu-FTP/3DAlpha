// Entity saves exercise the real level codec and streamer save boundaries:
// an entity-only edit must survive both autosave and closing the world.
#include "framework.hpp"
#include "core/entity/persistence.hpp"
#include "core/entity/player_body.hpp"
#include "core/block/registry.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/render/world_streamer.hpp"
#include "core/world/any_storage.hpp"
#include "impl/storage/alpha_chunkfiles/level_dat.hpp"
#include "low_heap.hpp"
#include "scene_world.hpp"

#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>

using namespace mc;
namespace {
struct Pools {
    entity::PaintingSystem paintings{1};
    entity::ArrowSystem arrows{2};
    entity::BoatSystem boats{3};
    entity::MinecartSystem minecarts{4};
    entity::ItemEntitySystem items{5};
    entity::FallingBlockSystem falling;
    entity::EntityPools bindings()
    {
        return {&paintings, &arrows, &boats, &minecarts, &items, &falling};
    }
};
std::shared_ptr<entity::PersistentEntities> sample()
{
    auto s = std::make_shared<entity::PersistentEntities>();
    entity::Painting p{};
    p.tileX = -18; p.tileY = 65; p.tileZ = -33; p.art = 2;
    p.alive = true; p.checkIn = 79;
    entity::setPaintingDirection(&p, 3);
    s->paintings.push(p);
    entity::Arrow a{};
    a.setPosition(-17.5, 65, -32.5); a.alive = true;
    a.inGround = true; a.tileX = -18; a.tileY = 65; a.tileZ = -33;
    a.inTile = int(mcver::Block::Stone); a.ticksInGround = 48; a.shake = 3;
    s->arrows.push(a);
    entity::Arrow flying{};
    flying.setPosition(10, 70, -20); flying.alive = true;
    flying.motionX = 0.4; flying.motionY = -0.2; flying.yaw = 23; flying.pitch = -10;
    s->arrows.push(flying);
    entity::Boat b{};
    b.setPosition(-5, 63, 8); b.alive = true; b.ridden = true; b.motionZ = 0.2;
    s->boats.push(b);
    for (int i = 0; i < 3; ++i) {
        entity::Minecart c{};
        c.setPosition(-20 + i, 64, -30); c.alive = true;
        c.type = entity::MinecartType(i); c.fuel = i == 2 ? 1234 : 0;
        c.pushX = 0.5; c.motionZ = -0.15;
        s->minecarts.push(c);
    }
    entity::ItemEntity item{};
    item.setPosition(1, 65, 2); item.item = item::ItemId(mcver::Block::Stone);
    item.count = 12; item.damage = 3; item.age = 300; item.pickupDelay = 9;
    s->items.push(item);
    entity::FallingBlock f{};
    f.setPosition(1, 68, 2); f.block = block::BlockId(mcver::Block::Sand);
    f.fallTime = 7; f.motionY = -0.28;
    s->fallingBlocks.push(f);
    return s;
}
struct TempDir {
    char path[64] = "/tmp/3dalpha_entities_XXXXXX";
    TempDir() { if (!::mkdtemp(path)) path[0] = 0; }
    ~TempDir() { if (path[0]) { std::error_code ec; std::filesystem::remove_all(path, ec); } }
};
}

TEST(entity_persistence_restores_all_pools_and_simulation_state)
{
    world::LevelData level;
    level.entities = sample();
    std::vector<u8> bytes;
    CHECK(alpha::encodeLevelDat(level, &bytes));
    world::LevelData decoded;
    CHECK(alpha::decodeLevelDat(bytes, &decoded));
    CHECK(decoded.entities);
    Pools pools;
    decoded.entities->restore(pools.bindings());
    CHECK_EQ(pools.paintings.count(), 1);
    CHECK_EQ(pools.paintings[0].tileX, -18);
    CHECK_EQ(pools.paintings[0].art, 2);
    CHECK_EQ(pools.paintings[0].direction, 3);
    CHECK_EQ(pools.paintings[0].box.minX, level.entities->paintings[0].box.minX);
    CHECK_EQ(pools.arrows.count(), 2);
    CHECK(pools.arrows[0].inGround);
    CHECK_EQ(pools.arrows[0].ticksInGround, 48);
    CHECK_EQ(pools.arrows[1].motionY, -0.2);
    CHECK_EQ(pools.arrows[1].prevX, pools.arrows[1].x);
    CHECK_EQ(pools.arrows[1].prevYaw, 23);
    CHECK_EQ(pools.boats.count(), 1);
    CHECK(!pools.boats[0].ridden);
    CHECK_EQ(pools.boats.riddenIndex(), -1);
    CHECK_EQ(pools.minecarts.count(), 3);
    CHECK(pools.minecarts[2].type == entity::MinecartType::Furnace);
    CHECK_EQ(pools.minecarts[2].fuel, 1234);
    CHECK_EQ(pools.minecarts[2].motionZ, -0.15);
    CHECK_EQ(pools.items[0].count, 12);
    CHECK_EQ(pools.items[0].damage, 3);
    CHECK_EQ(pools.items[0].age, 300);
    CHECK_EQ(pools.items[0].pickupDelay, 9);
    CHECK_EQ(pools.falling[0].fallTime, 7);
    CHECK_EQ(pools.falling[0].motionY, -0.28);

    // Opening a world restores entities before any columns are resident.
    test::SceneWorld scene{100, 100};
    pools.falling.tick(scene.w());
    CHECK_EQ(pools.falling.count(), 1);
    CHECK_EQ(pools.falling[0].fallTime, 7);
}

TEST(entity_persistence_rejects_truncation_and_drops_nonfinite_entities)
{
    world::LevelData level;
    auto state = sample();
    level.entities = state;
    std::vector<u8> bytes;
    CHECK(alpha::encodeLevelDat(level, &bytes));
    world::LevelData decoded;
    for (usize n = 0; n < bytes.size(); ++n) {
        CHECK(!alpha::decodeLevelDat(ConstByteSpan(bytes.data(), n), &decoded));
        CHECK(!decoded.entities);
    }

    // **One bad entity costs that entity, not the world.** A minecart stack
    // that overflowed to NaN used to make the whole level.dat unreadable.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    (*state).arrows[0].x = nan;
    (*state).minecarts[1].motionZ = std::numeric_limits<double>::infinity();
    (*state).minecarts[2].z = nan;
    bytes.clear();
    CHECK(alpha::encodeLevelDat(level, &bytes));
    CHECK(alpha::decodeLevelDat(bytes, &decoded));
    CHECK(decoded.entities);
    CHECK_EQ(decoded.entities->arrows.count(), 1);
    CHECK_EQ(decoded.entities->arrows[0].motionY, -0.2);
    CHECK_EQ(decoded.entities->minecarts.count(), 1);
    CHECK(decoded.entities->minecarts[0].type == entity::MinecartType::Rideable);
    CHECK_EQ(decoded.entities->paintings.count(), 1);
    CHECK_EQ(decoded.entities->boats.count(), 1);
    CHECK_EQ(decoded.entities->items.count(), 1);
    CHECK_EQ(decoded.entities->fallingBlocks.count(), 1);
}

TEST(entity_persistence_puts_a_nonfinite_player_back_at_spawn)
{
    // Riding a cart that went NaN took the player with it, and the save kept
    // them there: a camera that drew nothing and collided with nothing.
    world::LevelData level;
    level.spawnX = 12;
    level.spawnY = 70;
    level.spawnZ = -34;
    level.player.present = true;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    level.player.pos[0] = nan;
    level.player.pos[1] = 64.0;
    level.player.pos[2] = nan;
    level.player.motion[2] = nan;
    level.player.rotation[0] = std::numeric_limits<float>::quiet_NaN();
    level.player.rotation[1] = 15.0f;
    item::ItemStack stack;
    stack.slot = 3;
    stack.id = 1;
    stack.count = 64;
    level.player.inventory.push_back(stack);
    std::vector<u8> bytes;
    CHECK(alpha::encodeLevelDat(level, &bytes));

    world::LevelData decoded;
    CHECK(alpha::decodeLevelDat(bytes, &decoded));
    CHECK(decoded.player.present);
    CHECK_EQ(decoded.player.pos[0], 12.0);
    CHECK_EQ(decoded.player.pos[1], 70.0 + double(entity::kEyeHeight));
    CHECK_EQ(decoded.player.pos[2], -34.0);
    CHECK_EQ(decoded.player.motion[2], 0.0);
    CHECK_EQ(decoded.player.rotation[0], 0.0f);
    CHECK_EQ(decoded.player.rotation[1], 15.0f);
    CHECK_EQ(decoded.player.inventory.size(), usize(1));
    CHECK_EQ(decoded.player.inventory[0].count, 64);
}

TEST(entity_persistence_has_no_count_limit)
{
    // The pools have no cap, so neither has the save: a list longer than any
    // pool's first segment round-trips whole. It used to be refused outright,
    // and with it the whole level.dat.
    Pools pools;
    test::SceneWorld scene{0, 0};
    const int many = entity::ArrowSystem::kInitialCapacity * 3 + 5;
    for (int i = 0; i < many; ++i) {
        CHECK(pools.arrows.shoot(scene.w(), 0.5, 70.0, 0.5, float(i), -30.0f));
    }
    auto state = std::make_shared<entity::PersistentEntities>();
    CHECK(state->capture(pools.bindings()));
    CHECK_EQ(state->arrows.count(), many);

    world::LevelData level;
    level.entities = state;
    std::vector<u8> bytes;
    CHECK(alpha::encodeLevelDat(level, &bytes));
    world::LevelData decoded;
    CHECK(alpha::decodeLevelDat(bytes, &decoded));
    CHECK(decoded.entities);
    CHECK_EQ(decoded.entities->arrows.count(), many);

    Pools reopened;
    decoded.entities->restore(reopened.bindings());
    CHECK_EQ(reopened.arrows.count(), many);
    CHECK_EQ(int(reopened.arrows.refused()), 0);
    CHECK_EQ(reopened.arrows[many - 1].yaw, pools.arrows[many - 1].yaw);
}

TEST(entity_persistence_restores_what_the_heap_will_hold_and_counts_the_rest)
{
    // A save made with more memory to spare than this console has still opens:
    // the pool takes what the heap will let it and counts the rest as refused,
    // rather than the level failing to load or the process ending.
    auto state = std::make_shared<entity::PersistentEntities>();
    const int many = entity::BoatSystem::kInitialCapacity * 4;
    for (int i = 0; i < many; ++i) {
        entity::Boat b{};
        b.setPosition(double(i), 63, 0);
        b.alive = true;
        CHECK(state->boats.push(b));
    }
    Pools pools;
    {
        test::LowHeap low;
        state->restore(pools.bindings());
    }
    CHECK_EQ(pools.boats.count(), entity::BoatSystem::kInitialCapacity);
    CHECK_EQ(int(pools.boats.refused()), many - entity::BoatSystem::kInitialCapacity);
}

TEST(entity_persistence_autosave_close_and_deletion_in_both_world_formats)
{
    for (auto format : {world::WorldFormat::Folder, world::WorldFormat::Packed}) {
        TempDir temp;
        CHECK(temp.path[0]);
        io::PosixFileSystem fs;
        {
            world::AnyStorage storage(fs);
            CHECK(storage.create(temp.path, 123, 1000, format) == world::OpenResult::Ok);
            CHECK(storage.close(1000));
        }
        Pools pools;
        render::WorldStreamer streamer;
        CHECK(streamer.open(temp.path, 1, 1000));
        streamer.bindEntities(pools.bindings());
        sample()->restore(pools.bindings());
        CHECK_EQ(streamer.dirtyColumns(), 0);
        streamer.setAutosaveSeconds(1);
        streamer.tickSaves(2000);
        // The queued snapshot owns its data, even if the live pool is cleared.
        pools.minecarts.clear();
        {
            world::AnyStorage saved(fs);
            CHECK(saved.open(temp.path, 2000) == world::OpenResult::Ok);
            CHECK(saved.level().entities);
            CHECK_EQ(saved.level().entities->minecarts.count(), 3);
            CHECK(saved.close(2000));
        }
        streamer.close(3000);
        CHECK(streamer.open(temp.path, 1, 4000));
        streamer.bindEntities(pools.bindings());
        CHECK_EQ(pools.minecarts.count(), 0);
        CHECK_EQ(pools.paintings.count(), 1);
        CHECK_EQ(pools.arrows.count(), 2);
        pools.paintings.clear(); pools.arrows.clear(); pools.boats.clear();
        pools.items.clear(); pools.falling.clear();
        streamer.saveNow(5000);
        streamer.close(5000);
        CHECK(streamer.open(temp.path, 1, 6000));
        streamer.bindEntities(pools.bindings());
        CHECK_EQ(pools.paintings.count(), 0);
        CHECK_EQ(pools.arrows.count(), 0);
        CHECK_EQ(pools.items.count(), 0);
        streamer.close(6000);
    }
}
