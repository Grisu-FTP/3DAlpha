// The entities a server owns: other players walking toward where the server
// last put them, and items on the ground driven entirely by their server id.
//
// Every scaling here is the wire's, from the jars: a position is `floor(v * 32)`
// absolute and the same units in a byte when relative, a rotation is a byte of
// a whole turn, and a Pickup Spawn's last three bytes are velocity in 1/128.

#include "framework.hpp"

#include "core/entity/item_entity.hpp"
#include "core/entity/mob.hpp"
#include "core/net/entities.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/render/remote_player_mesh.hpp"
#include "scene_world.hpp"
#include "sound_catcher.hpp"

#include <cmath>

using namespace mc;
using namespace mc::net;

namespace {

Packet namedSpawn(i32 id, const char* name, double x, double y, double z, int yawByte,
                  int pitchByte, int held)
{
    Packet p;
    p.reset(packet::NamedEntitySpawn);
    p.pushInt(id);
    p.pushString(name);
    p.pushInt(i64(std::floor(x * 32.0)));
    p.pushInt(i64(std::floor(y * 32.0)));
    p.pushInt(i64(std::floor(z * 32.0)));
    p.pushInt(yawByte);
    p.pushInt(pitchByte);
    p.pushInt(held);
    return p;
}

Packet relMove(i32 id, int dx, int dy, int dz)
{
    Packet p;
    p.reset(packet::RelEntityMove);
    p.pushInt(id);
    p.pushInt(dx);
    p.pushInt(dy);
    p.pushInt(dz);
    return p;
}

Packet destroy(i32 id)
{
    Packet p;
    p.reset(packet::DestroyEntity);
    p.pushInt(id);
    return p;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

TEST(another_player_appears_where_the_server_put_them_and_faces_where_it_said)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 8.5, 65.62, -3.25, 64, 0, 278), nullptr));
    CHECK_EQ(entities.playerCount(), 1);

    const RemotePlayer& player = entities.player(0);
    CHECK_EQ(player.id, i32(7));
    CHECK(std::string(player.name) == "Grisu");
    CHECK(near(player.x, 8.5));
    CHECK(near(player.z, -3.25));
    // 64 of 256 is a quarter turn.
    CHECK(near(double(player.yaw), 90.0));
    CHECK_EQ(player.heldItem, i16(278));
    // `floor(65.62 * 32) / 32` -- the wire cannot carry the rest of it.
    CHECK(near(player.y, std::floor(65.62 * 32.0) / 32.0));

    // A name longer than the field is cut rather than overrunning it.
    CHECK(entities.apply(namedSpawn(8, std::string(100, 'A').c_str(), 0, 64, 0, 0, 0, 0),
                         nullptr));
    CHECK_EQ(entities.playerCount(), 2);
    CHECK(std::string(entities.player(1).name).size() == usize(RemotePlayer::kMaxNameBytes - 1));
}

TEST(another_player_walks_to_the_new_place_over_three_ticks_rather_than_jumping)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 0.0, 64.0, 0.0, 0, 0, 0), nullptr));

    // One block east: 32 of the wire's units.
    CHECK(entities.apply(relMove(7, 32, 0, 0), nullptr));
    CHECK(near(entities.player(0).x, 0.0));  // not moved yet: that is the tick's job

    entities.tick(nullptr);
    CHECK(near(entities.player(0).x, 1.0 / 3.0));
    entities.tick(nullptr);
    CHECK(near(entities.player(0).x, 1.0 / 3.0 + (1.0 - 1.0 / 3.0) / 2.0));
    entities.tick(nullptr);
    CHECK(near(entities.player(0).x, 1.0));

    // Nothing more arrives, so the body stops rather than drifting on.
    entities.tick(nullptr);
    CHECK(near(entities.player(0).x, 1.0));

    // **Relative moves are against the target, not against where the body has
    // got to**, or a body still catching up would fall further behind.
    CHECK(entities.apply(relMove(7, 32, 0, 0), nullptr));
    CHECK(entities.apply(relMove(7, 32, 0, 0), nullptr));
    for (int i = 0; i < 3; ++i) {
        entities.tick(nullptr);
    }
    CHECK(near(entities.player(0).x, 3.0));

    // Walking sets the limbs going; standing still lets them settle.
    CHECK(entities.player(0).limbAmount > 0.1f);
    for (int i = 0; i < 40; ++i) {
        entities.tick(nullptr);
    }
    CHECK(entities.player(0).limbAmount < 0.01f);
}

TEST(a_turn_past_north_goes_the_short_way_round)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 0.0, 64.0, 0.0, 127, 0, 0), nullptr));
    const float from = entities.player(0).yaw;  // ~178.6 degrees

    Packet look;
    look.reset(packet::EntityLook);
    look.pushInt(7);
    look.pushInt(-127);  // ~-178.6: two and a half degrees away across the wrap
    look.pushInt(0);
    CHECK(entities.apply(look, nullptr));
    entities.tick(nullptr);

    // The short way is a step *past* 180, not a long slide back through zero.
    CHECK(entities.player(0).yaw > from || entities.player(0).yaw < -170.0f);
    CHECK(std::fabs(double(entities.player(0).yaw) - double(from)) < 90.0);
}

TEST(a_player_goes_when_the_server_destroys_them_and_the_pool_closes_up)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(1, "One", 0, 64, 0, 0, 0, 0), nullptr));
    CHECK(entities.apply(namedSpawn(2, "Two", 4, 64, 0, 0, 0, 0), nullptr));
    CHECK(entities.apply(namedSpawn(3, "Three", 8, 64, 0, 0, 0, 0), nullptr));
    CHECK_EQ(entities.playerCount(), 3);

    CHECK(entities.apply(destroy(2), nullptr));
    CHECK_EQ(entities.playerCount(), 2);
    // The survivors are still whole and still reachable by id.
    CHECK(entities.apply(relMove(3, 32, 0, 0), nullptr));
    for (int i = 0; i < 3; ++i) {
        entities.tick(nullptr);
    }
    bool foundThree = false;
    for (int i = 0; i < entities.playerCount(); ++i) {
        if (entities.player(i).id == 3) {
            foundThree = true;
            CHECK(near(entities.player(i).x, 9.0));
        }
        CHECK(entities.player(i).id != 2);
    }
    CHECK(foundThree);

    // Spawning the same id twice is the server re-sending, not a twin.
    CHECK(entities.apply(namedSpawn(1, "One", 20, 64, 0, 0, 0, 0), nullptr));
    CHECK_EQ(entities.playerCount(), 2);
}

TEST(an_item_on_the_ground_is_the_ordinary_pool_driven_by_its_server_id)
{
    test::SceneWorld scene(0, 0);
    entity::ItemEntitySystem items(1234);
    RemoteEntities entities;
    entities.bind(&items);

    Packet spawn;
    spawn.reset(packet::PickupSpawn);
    spawn.pushInt(42);                                  // entity id
    spawn.pushInt(4);                                   // cobblestone
    spawn.pushInt(3);                                   // count
    spawn.pushInt(i64(std::floor(2.5 * 32.0)));         // x
    spawn.pushInt(i64(std::floor(70.0 * 32.0)));        // y
    spawn.pushInt(i64(std::floor(-6.5 * 32.0)));        // z
    spawn.pushInt(13);                                  // motion, 1/128 blocks a tick
    spawn.pushInt(0);
    spawn.pushInt(-13);
    CHECK(entities.apply(spawn, &scene.w()));

    CHECK_EQ(items.count(), 1);
    CHECK_EQ(items[0].item, item::ItemId(4));
    CHECK_EQ(items[0].count, 3);
    CHECK_EQ(items[0].entityId, i32(42));
    CHECK(near(items[0].x, 2.5));
    CHECK(near(items[0].z, -6.5));
    CHECK(near(items[0].motionX, 13.0 / 128.0));
    // **Out of reach of any local pickup**: the server says who collected what.
    CHECK(items[0].pickupDelay > 1000);

    // A teleport for that id moves the item the server owns, not a local one.
    Packet teleport;
    teleport.reset(packet::EntityTeleport);
    teleport.pushInt(42);
    teleport.pushInt(i64(std::floor(3.5 * 32.0)));
    teleport.pushInt(i64(std::floor(71.0 * 32.0)));
    teleport.pushInt(i64(std::floor(-7.5 * 32.0)));
    teleport.pushInt(0);
    teleport.pushInt(0);
    CHECK(entities.apply(teleport, &scene.w()));
    CHECK(near(items[0].x, 3.5));
    CHECK(near(items[0].z, -7.5));
    CHECK(near(items[0].motionX, 0.0));

    // Collect removes it; a1.1.2 animates the flight and this does not.
    Packet collect;
    collect.reset(packet::Collect);
    collect.pushInt(42);
    collect.pushInt(1);
    CHECK(entities.apply(collect, &scene.w()));
    CHECK_EQ(items.count(), 0);
}

TEST(what_this_client_cannot_draw_yet_is_counted_rather_than_dropped_in_silence)
{
    RemoteEntities entities;
    Packet mob;
    mob.reset(packet::MobSpawn);
    for (int v : {5, 91, 0, 64 * 32, 0, 0, 0}) {
        mob.pushInt(v);
    }
    CHECK(entities.apply(mob, nullptr));
    CHECK_EQ(entities.playerCount(), 0);
    CHECK_EQ(entities.unhandledSpawns(), u32(1));

    // A packet that is not about an entity is left for the caller.
    CHECK(!entities.apply(makeChat("hello"), nullptr));
}

TEST(a_drawn_player_stands_on_their_own_feet_the_right_way_up)
{
    // The same shape as `a_drawn_animal_stands_on_its_own_feet`, and for the
    // same reason: a model built out of `ModelPart`s is in a frame whose +Y
    // points at the ground, so placing one without the renderer's flip draws it
    // upside down and buried -- which is what hardware showed.
    constexpr double kGround = 64.0;
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 0.5, kGround, 0.5, 0, 0, 0), nullptr));

    static std::vector<mesh::DetailVertex> verts(render::kRemotePlayerVerticesEach * 2);
    const int written = render::buildRemotePlayers(entities, 0.0, kGround, 0.0, 1.0f,
                                                   verts.data(), int(verts.size()));
    CHECK(written > 0);

    double lowest = 1e9;
    double highest = -1e9;
    for (int i = 0; i < written; ++i) {
        const double y = kGround + double(verts[i].y) / double(render::kEntityUnitsPerBlock);
        lowest = y < lowest ? y : lowest;
        highest = y > highest ? y : highest;
    }
    // Feet on the ground, head about a body's height above it -- a player is
    // 1.8 tall and the hat layer adds a little.
    CHECK(std::fabs(lowest - kGround) < 0.02);
    CHECK(highest > kGround + 1.6);
    CHECK(highest < kGround + 2.1);
}

TEST(a_mob_the_server_owns_is_drawn_here_and_decided_there)
{
    test::SceneWorld scene(0, 0);
    entity::MobSystem herd(99);
    RemoteEntities entities;
    entities.bindMobs(&herd);

    Packet spawn;
    spawn.reset(packet::MobSpawn);
    spawn.pushInt(77);                             // entity id
    spawn.pushInt(91);                             // sheep, cow *and* chicken
    spawn.pushInt(i64(std::floor(4.5 * 32.0)));
    spawn.pushInt(i64(std::floor(64.0 * 32.0)));
    spawn.pushInt(i64(std::floor(-2.5 * 32.0)));
    spawn.pushInt(64);                             // a quarter turn
    spawn.pushInt(0);
    CHECK(entities.apply(spawn, &scene.w()));

    CHECK_EQ(herd.count(), 1);
    // **A chicken, and that is the jar's answer rather than a guess**: sheep,
    // cow and chicken all register as 91 and `ew`'s map keeps the last one, so
    // this is what a real a1.1.2 client draws for all three.
    CHECK(herd[0].type == entity::MobType::Chicken);
    CHECK(herd[0].remote);
    CHECK_EQ(herd[0].entityId, i32(77));
    CHECK(near(herd[0].body.x, 4.5));
    CHECK(near(herd[0].body.z, -2.5));
    CHECK(near(double(herd[0].yaw), 90.0));

    // **The local mind is off.** Ticking the world moves a single-player animal
    // about; this one stays exactly where the server put it.
    entity::MobSurroundings around;
    for (int i = 0; i < 40; ++i) {
        herd.tick(scene.w(), around);
    }
    CHECK(near(herd[0].body.x, 4.5));
    CHECK(near(herd[0].body.z, -2.5));
    CHECK_EQ(herd.count(), 1);

    // **It moves when, and only when, the server says so -- and it walks.**
    // `gy` hands every entity move to `setPositionAndRotation2` with three
    // increments and `ge.j()` divides what is left by what is left on the
    // clock, so one block arrives as a third, then a half of the rest, then
    // the rest. A packet does not teleport an animal; it sets it going.
    CHECK(entities.apply(relMove(77, 32, 0, 0), &scene.w()));
    CHECK(near(herd[0].body.x, 4.5));  // nothing until the next tick
    herd.tick(scene.w(), around);
    CHECK(near(herd[0].body.x, 4.5 + 1.0 / 3.0));
    herd.tick(scene.w(), around);
    herd.tick(scene.w(), around);
    CHECK(near(herd[0].body.x, 5.5));

    // And having arrived it stops, however long nothing more is said.
    for (int i = 0; i < 20; ++i) {
        herd.tick(scene.w(), around);
    }
    CHECK(near(herd[0].body.x, 5.5));

    Packet destroyed;
    destroyed.reset(packet::DestroyEntity);
    destroyed.pushInt(77);
    CHECK(entities.apply(destroyed, &scene.w()));
    CHECK_EQ(herd.count(), 0);

    // A giant has no model here, so it is counted rather than drawn wrong.
    Packet giant;
    giant.reset(packet::MobSpawn);
    giant.pushInt(78);
    giant.pushInt(53);
    for (int i = 0; i < 5; ++i) {
        giant.pushInt(0);
    }
    CHECK(entities.apply(giant, &scene.w()));
    CHECK_EQ(herd.count(), 0);
    CHECK_EQ(entities.unhandledSpawns(), u32(1));
}

namespace {

Packet mobSpawn(i32 id, int wireType)
{
    Packet spawn;
    spawn.reset(packet::MobSpawn);
    spawn.pushInt(id);
    spawn.pushInt(wireType);
    spawn.pushInt(i64(std::floor(4.5 * 32.0)));
    spawn.pushInt(i64(std::floor(64.0 * 32.0)));
    spawn.pushInt(i64(std::floor(-2.5 * 32.0)));
    spawn.pushInt(0);
    spawn.pushInt(0);
    return spawn;
}

}  // namespace

// **A hit on somebody else's animal, as the watching console sees it.** Protocol
// 2 never said an animal was hurt, so a guest's punch drew no red and made no
// noise, and a kill vanished without the fall. The host's Entity Status is
// later versions' `handleHealthUpdate`: 2 is the flail, the tint and the hurt
// noise; 3 the death noise and twenty ticks lying there, then the puff.
TEST(a_server_mobs_hit_and_death_are_seen_and_heard_here)
{
    test::SceneWorld scene(0, 0);
    test::SoundCatcher heard;
    heard.watch(scene.w());
    entity::MobSystem herd(99);
    RemoteEntities entities;
    entities.bindMobs(&herd);
    CHECK(entities.apply(mobSpawn(77, 90), &scene.w()));  // a pig
    CHECK_EQ(herd.count(), 1);
    const entity::MobDef& pig = entity::mobDef(entity::MobType::Pig);

    CHECK(entities.apply(makeEntityStatus(77, entity::kStatusHurt), &scene.w()));
    CHECK_EQ(int(herd[0].hurtTime), entity::kHurtTime);
    CHECK_EQ(int(herd[0].maxHurtTime), entity::kHurtTime);
    CHECK(herd[0].limbYaw == 1.5f);
    CHECK_EQ(heard.countOf(pig.hurtSound), 1);
    CHECK(herd[0].health > 0);  // the damage is the host's to count

    // The tint runs down on this console's own ticks.
    entity::MobSurroundings around;
    for (int i = 0; i < entity::kHurtTime; ++i) {
        herd.tick(scene.w(), around);
    }
    CHECK_EQ(int(herd[0].hurtTime), 0);

    CHECK(entities.apply(makeEntityStatus(77, entity::kStatusDead), &scene.w()));
    CHECK(herd[0].health <= 0);
    CHECK_EQ(heard.countOf(pig.deathSound), 1);
    // A second word of the same death is not a second death.
    CHECK(entities.apply(makeEntityStatus(77, entity::kStatusDead), &scene.w()));
    CHECK_EQ(heard.countOf(pig.deathSound), 1);

    for (int i = 0; i < entity::kDeathTicks; ++i) {
        herd.tick(scene.w(), around);
    }
    CHECK_EQ(herd.count(), 1);
    CHECK_EQ(int(herd[0].deathTime), entity::kDeathTicks);  // lying there, falling over
    herd.tick(scene.w(), around);
    CHECK_EQ(herd.count(), 0);  // and gone in the puff

    // The host's own Destroy Entity for it then finds nothing, harmlessly.
    Packet destroyed;
    destroyed.reset(packet::DestroyEntity);
    destroyed.pushInt(77);
    CHECK(entities.apply(destroyed, &scene.w()));

    // A status for an animal this console never met is nothing.
    CHECK(entities.apply(makeEntityStatus(123, entity::kStatusHurt), &scene.w()));
}

// **Another player's swing**, which a1.1.2 draws and this did not: `gy.a(hf)`
// starts `dm.w()` on whoever the Arm Animation names, and the counter then
// runs in eighths, one a tick, back to rest after eight.
TEST(another_players_arm_swings_when_the_server_says_so)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 0.0, 64.0, 0.0, 0, 0, 0), nullptr));
    CHECK(entities.apply(makeArmSwing(7), nullptr));
    const RemotePlayer& player = entities.player(0);

    entities.tick(nullptr);
    CHECK(player.swinging);
    CHECK(player.swing == 0.0f);  // `-1`, so the first tick lands on zero
    entities.tick(nullptr);
    CHECK(player.swing == 1.0f / 8.0f);
    CHECK(player.swingProgress(0.5f) == 1.0f / 16.0f);
    for (int i = 0; i < 6; ++i) {
        entities.tick(nullptr);
    }
    CHECK(player.swing == 7.0f / 8.0f);
    entities.tick(nullptr);
    CHECK(!player.swinging);
    CHECK(player.swing == 0.0f);
    // The last eighth runs 7/8 -> 1 rather than back through the swing.
    CHECK(player.swingProgress(0.5f) == 15.0f / 16.0f);

    // A swing for somebody who is not here is nothing.
    CHECK(entities.apply(makeArmSwing(99), nullptr));
}

// **Another player's crouch**, which protocol 2 has no way to say and a 3DAlpha
// host says with b1.2's Entity Action: 1 down, 2 up. Drawn with `cr.j`'s pose
// and an eighth of a block lower, as b1.2's `RenderPlayer` draws it -- so the
// drawn-up legs still reach the ground.
TEST(another_player_crouches_when_the_server_says_so)
{
    constexpr double kGround = 64.0;
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 0.5, kGround, 0.5, 0, 0, 0), nullptr));
    const RemotePlayer& player = entities.player(0);
    CHECK(!player.sneaking);

    const auto extent = [&](double* lowest, double* highest) {
        static std::vector<mesh::DetailVertex> verts(render::kRemotePlayerVerticesEach);
        const int written = render::buildRemotePlayers(entities, 0.0, kGround, 0.0, 1.0f,
                                                       verts.data(), int(verts.size()));
        CHECK_EQ(written, render::kRemotePlayerVerticesEach);
        *lowest = 1e9;
        *highest = -1e9;
        for (int i = 0; i < written; ++i) {
            const double y = kGround + double(verts[i].y) / double(render::kEntityUnitsPerBlock);
            *lowest = y < *lowest ? y : *lowest;
            *highest = y > *highest ? y : *highest;
        }
    };
    double standLow = 0.0;
    double standHigh = 0.0;
    extent(&standLow, &standHigh);

    CHECK(entities.apply(makeEntityAction(7, kActionCrouch), nullptr));
    CHECK(player.sneaking);
    double crouchLow = 0.0;
    double crouchHigh = 0.0;
    extent(&crouchLow, &crouchHigh);
    // The legs are drawn up three pixels and the body lowered two: the feet
    // end a pixel above the ground. The top is the hat, which does not drop
    // the pixel the head does, so it comes down by the eighth alone.
    CHECK(std::fabs(crouchLow - (kGround + 1.0 / 16.0)) < 0.02);
    CHECK(std::fabs(crouchHigh - (standHigh - 0.125)) < 0.02);

    CHECK(entities.apply(makeEntityAction(7, kActionUncrouch), nullptr));
    CHECK(!player.sneaking);

    // A crouch for somebody who is not here is nothing.
    CHECK(entities.apply(makeEntityAction(99, kActionCrouch), nullptr));
}

// **Another player's death**, which only their own console knows about and a
// 3DAlpha host passes on as an Entity Status 3. `dm` overrides none of `ge`'s
// dying, so a player goes the way an animal does: `random.hurt`, twenty ticks of
// falling over in red, and the puff. A Named Entity Spawn brings them back.
TEST(another_player_falls_over_dies_and_comes_back_when_they_respawn)
{
    constexpr double kGround = 64.0;
    test::SceneWorld scene(0, 0);
    test::SoundCatcher heard;
    heard.watch(scene.w());
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7, "Grisu", 0.5, kGround, 0.5, 0, 0, 0), &scene.w()));
    CHECK(entities.apply(makeEntityAction(7, kActionCrouch), &scene.w()));

    CHECK(entities.apply(makeEntityStatus(7, entity::kStatusDead), &scene.w()));
    const RemotePlayer& player = entities.player(0);
    CHECK(player.dead);
    CHECK(!player.sneaking);  // a body falling over does not stay crouched
    CHECK_EQ(heard.countOf("random.hurt"), 1);
    // A second word of the same death is not a second death, and a corpse
    // does not crouch.
    CHECK(entities.apply(makeEntityStatus(7, entity::kStatusDead), &scene.w()));
    CHECK_EQ(heard.countOf("random.hurt"), 1);
    CHECK(entities.apply(makeEntityAction(7, kActionCrouch), &scene.w()));
    CHECK(!player.sneaking);

    for (int i = 0; i < RemoteEntities::kDeathTicks; ++i) {
        entities.tick(&scene.w());
    }
    CHECK_EQ(entities.playerCount(), 1);
    CHECK_EQ(player.deathTime, RemoteEntities::kDeathTicks);

    // Flat on its side by now, and red.
    static std::vector<mesh::DetailVertex> verts(render::kRemotePlayerVerticesEach);
    const int written = render::buildRemotePlayers(entities, 0.0, kGround, 0.0, 1.0f,
                                                   verts.data(), int(verts.size()));
    CHECK_EQ(written, render::kRemotePlayerVerticesEach);
    double highest = -1e9;
    for (int i = 0; i < written; ++i) {
        const double y = kGround + double(verts[i].y) / double(render::kEntityUnitsPerBlock);
        highest = y > highest ? y : highest;
        CHECK_EQ(int(verts[i].g), int(render::kHurtChannel));
    }
    CHECK(highest < kGround + 0.6);

    // One more tick and the body goes. Its name goes with it.
    entities.tick(&scene.w());
    CHECK_EQ(entities.playerCount(), 0);

    // The host's answer to a Respawn is a fresh spawn: standing, alive, and
    // wherever they came back.
    CHECK(entities.apply(namedSpawn(7, "Grisu", 20.5, kGround, 0.5, 0, 0, 0), &scene.w()));
    CHECK_EQ(entities.playerCount(), 1);
    CHECK(!entities.player(0).dead);
    CHECK_EQ(entities.player(0).deathTime, 0);

    // **A spawn for somebody already here replaces them** rather than
    // updating them, so a crouch does not survive it either.
    CHECK(entities.apply(makeEntityAction(7, kActionCrouch), &scene.w()));
    CHECK(entities.apply(namedSpawn(7, "Grisu", 20.5, kGround, 0.5, 0, 0, 0), &scene.w()));
    CHECK_EQ(entities.playerCount(), 1);
    CHECK(!entities.player(0).sneaking);
}

// `ge.y()`'s idle noise is `onEntityUpdate`, which a multiplayer entity still
// runs -- only `b_()` is suppressed -- so a cow on somebody else's console moos.
TEST(a_server_mob_makes_its_idle_noise_here)
{
    test::SceneWorld scene(0, 0);
    test::SoundCatcher heard;
    heard.watch(scene.w());
    entity::MobSystem herd(5);
    RemoteEntities entities;
    entities.bindMobs(&herd);
    CHECK(entities.apply(mobSpawn(80, 93), &scene.w()));  // ours: a cow
    CHECK(herd[0].type == entity::MobType::Cow);

    entity::MobSurroundings around;
    for (int i = 0; i < 2400; ++i) {
        herd.tick(scene.w(), around);
    }
    CHECK(heard.countOf(entity::mobDef(entity::MobType::Cow).livingSound) > 0);
    CHECK_EQ(herd.count(), 1);
}

TEST(every_console_works_out_the_same_colour_for_the_same_player)
{
    // Nothing on the wire says what colour anybody is. The host numbers its
    // players with the session's own player ids and starts every other entity
    // above them, so both ends reach the same answer from the id they already
    // have -- which is what makes the name over a head and the arrow on the
    // map the same colour on two screens. See `net::playerColour`.
    u8 hostR = 0, hostG = 0, hostB = 0;
    playerColour(kFirstPlayerEntityId, &hostR, &hostG, &hostB);

    u8 againR = 0, againG = 0, againB = 0;
    playerColour(kFirstPlayerEntityId, &againR, &againG, &againB);
    CHECK_EQ(int(hostR), int(againR));
    CHECK_EQ(int(hostG), int(againG));
    CHECK_EQ(int(hostB), int(againB));

    // No two players in a session share one.
    for (i32 a = kFirstPlayerEntityId; a < kFirstFreeEntityId; ++a) {
        for (i32 b = a + 1; b < kFirstFreeEntityId; ++b) {
            u8 ar = 0, ag = 0, ab = 0;
            u8 br = 0, bg = 0, bb = 0;
            playerColour(a, &ar, &ag, &ab);
            playerColour(b, &br, &bg, &bb);
            CHECK(ar != br || ag != bg || ab != bb);
        }
    }

    // An id that is not a player's -- an item, an animal, or anything a
    // session hosted by something other than this port hands out -- is white
    // rather than borrowed from somebody.
    u8 r = 0, g = 0, b = 0;
    playerColour(kFirstFreeEntityId + 3, &r, &g, &b);
    CHECK_EQ(int(r), 255);
    CHECK_EQ(int(g), 255);
    CHECK_EQ(int(b), 255);
}

namespace {

Packet vehicleSpawn(i32 id, int type, double x, double y, double z)
{
    Packet p;
    p.reset(packet::VehicleSpawn);
    p.pushInt(id);
    p.pushInt(type);
    p.pushInt(i64(std::floor(x * 32.0)));
    p.pushInt(i64(std::floor(y * 32.0)));
    p.pushInt(i64(std::floor(z * 32.0)));
    return p;
}

Packet teleportTo(i32 id, double x, double y, double z, int yawByte, int pitchByte)
{
    Packet p;
    p.reset(packet::EntityTeleport);
    p.pushInt(id);
    p.pushInt(i64(std::floor(x * 32.0)));
    p.pushInt(i64(std::floor(y * 32.0)));
    p.pushInt(i64(std::floor(z * 32.0)));
    p.pushInt(yawByte);
    p.pushInt(pitchByte);
    return p;
}

}  // namespace

TEST(an_arrow_from_a_3dalpha_host_is_drawn_where_the_host_says)
{
    // Type 60 is ours between two consoles -- a1.1.2's `gy.a(kj)` knows boats
    // and carts only -- and the guest's copy is a drawing: it goes where each
    // teleport says, one tick later, and never simulates a thing.
    test::SceneWorld scene(0, 0);
    entity::ArrowSystem arrows(5);
    RemoteEntities entities;
    entities.bindArrows(&arrows);

    CHECK(entities.apply(vehicleSpawn(77, kObjectArrow, 2.5, 70.0, -6.5), &scene.w()));
    CHECK_EQ(arrows.count(), 1);
    CHECK(arrows[0].remote);
    CHECK_EQ(arrows[0].entityId, i32(77));
    CHECK_EQ(arrows[0].shooterPlayer, 0);  // nothing here may collect it
    CHECK(near(arrows[0].x, 2.5));

    // The heading arrives with the first teleport and is faced at once.
    CHECK(entities.apply(teleportTo(77, 2.5, 70.0, -6.5, 64, 0), &scene.w()));
    arrows.tick(scene.w());
    CHECK(std::abs(arrows[0].yaw - 90.0f) < 1.5f);
    CHECK(std::abs(arrows[0].prevYaw - 90.0f) < 1.5f);

    // Moved: applied on the next tick, drawn from where it was.
    CHECK(entities.apply(teleportTo(77, 4.0, 69.5, -6.5, 64, 0), &scene.w()));
    CHECK(near(arrows[0].x, 2.5));
    arrows.tick(scene.w());
    CHECK(near(arrows[0].x, 4.0));
    CHECK(near(arrows[0].prevX, 2.5));

    // Across the back of the circle: 127 and then -128 bytes are the short
    // way round, not a full turn.
    CHECK(entities.apply(teleportTo(77, 4.0, 69.5, -6.5, 127, 0), &scene.w()));
    arrows.tick(scene.w());
    CHECK(entities.apply(teleportTo(77, 4.0, 69.5, -6.5, -128, 0), &scene.w()));
    arrows.tick(scene.w());
    CHECK(std::abs(arrows[0].yaw - arrows[0].prevYaw) < 5.0f);

    // A cart is still received and not drawn.
    CHECK(entities.apply(vehicleSpawn(78, 10, 0.0, 70.0, 0.0), &scene.w()));
    CHECK_EQ(arrows.count(), 1);

    // Collected by somebody, or spent: gone either way.
    CHECK(entities.apply(destroy(77), &scene.w()));
    CHECK_EQ(arrows.count(), 0);
    CHECK(entities.apply(vehicleSpawn(79, kObjectArrow, 0.0, 70.0, 0.0), &scene.w()));
    Packet collect;
    collect.reset(packet::Collect);
    collect.pushInt(79);
    collect.pushInt(2);
    CHECK(entities.apply(collect, &scene.w()));
    CHECK_EQ(arrows.count(), 0);
}
