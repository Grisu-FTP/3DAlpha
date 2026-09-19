// What a multiplayer client reports about its own player -- movement, the
// inventory, digging, the hand -- and how it reads the server's teleport.
// Every expectation is a line of `la.J()`, `nj` or `gy.a(eh)` in the a1.1.2 jar.

#include "framework.hpp"

#include "core/entity/item_entity.hpp"
#include "core/item/inventory.hpp"
#include "core/entity/mob.hpp"
#include "core/net/player_sync.hpp"

using namespace mc;
using namespace mc::net;

namespace {

PlayerPose standing(double x, double feet, double z, float yaw, float pitch)
{
    PlayerPose pose;
    pose.x = x;
    pose.feetY = feet;
    pose.eyeY = feet + 1.62;
    pose.z = z;
    pose.yaw = yaw;
    pose.pitch = pitch;
    pose.onGround = true;
    return pose;
}

}  // namespace

TEST(movement_reports_whichever_half_changed_since_it_was_last_sent)
{
    MovementReporter movement;
    PlayerPose pose = standing(8.5, 64.0, -3.5, 90.0f, 10.0f);

    // Everything starts at zero in `la`, so the first report is both halves.
    CHECK_EQ(movement.tick(pose).id, packet::PlayerPositionLook);
    CHECK_EQ(movement.tick(pose).id, packet::Flying);

    pose.x += 0.1;
    CHECK_EQ(movement.tick(pose).id, packet::PlayerPosition);
    pose.yaw += 5.0f;
    CHECK_EQ(movement.tick(pose).id, packet::PlayerLook);
    pose.z += 0.1;
    pose.pitch -= 1.0f;
    const Packet both = movement.tick(pose);
    CHECK_EQ(both.id, packet::PlayerPositionLook);
    CHECK_EQ(both.real(1), 64.0);
    CHECK_EQ(both.real(2), 65.62);
    CHECK_EQ(movement.tick(pose).id, packet::Flying);

    // A jump moves only the feet and the eye, and that is a move.
    pose.feetY += 0.4;
    pose.eyeY += 0.4;
    CHECK_EQ(movement.tick(pose).id, packet::PlayerPosition);
}

TEST(the_server_s_teleport_carries_the_eye_second_and_the_feet_third)
{
    // `dq(x, y + 1.62, y, z, ...)` on the server: wire order x, eye, feet, z.
    Packet p;
    p.reset(packet::PlayerPositionLook);
    for (double v : {68.5, 67.24, 65.62, 115.5, 180.0, 0.0}) {
        p.pushReal(v);
    }
    p.pushInt(0);

    Teleport t;
    CHECK(readTeleport(p, &t));
    CHECK(t.hasPosition);
    CHECK(t.hasLook);
    CHECK_EQ(t.x, 68.5);
    CHECK_EQ(t.eyeY, 67.24);
    CHECK_EQ(t.z, 115.5);
    CHECK_EQ(t.yaw, 180.0f);

    CHECK(!readTeleport(makeChat("hi"), &t));
}

TEST(the_inventory_goes_every_twentieth_tick_and_only_when_it_changed)
{
    item::Inventory inventory;
    InventoryReporter reporter;
    Packet out[3];

    for (int i = 0; i < 19; ++i) {
        CHECK_EQ(reporter.tick(inventory, out), 0);
    }
    CHECK_EQ(reporter.tick(inventory, out), 3);
    CHECK_EQ(out[0].integer(0), i64(kInventoryMain));
    CHECK_EQ(out[0].stacks.size(), usize(kWireMainSlots));
    CHECK_EQ(out[1].integer(0), i64(kInventoryCrafting));
    CHECK_EQ(out[1].stacks.size(), usize(kWireCraftingSlots));
    CHECK_EQ(out[2].integer(0), i64(kInventoryArmour));

    for (int i = 0; i < 20; ++i) {
        CHECK_EQ(reporter.tick(inventory, out), 0);
    }

    inventory.set(3, 4, 12);
    for (int i = 0; i < 19; ++i) {
        CHECK_EQ(reporter.tick(inventory, out), 0);
    }
    CHECK_EQ(reporter.tick(inventory, out), 3);
    CHECK_EQ(out[0].stacks[3].id, i16(4));
    CHECK_EQ(out[0].stacks[3].count, i8(12));
    CHECK(out[0].stacks[36].empty());

    // And what the server sends lands in the same slots.
    item::Inventory other;
    CHECK(applyInventory(out[0], &other));
    CHECK_EQ(other.at(3).id, i16(4));
    CHECK(!applyInventory(out[1], &other));
}

TEST(digging_says_start_every_tick_and_stop_and_says_when_it_broke)
{
    DigReporter dig;
    Packet out[2];

    CHECK_EQ(dig.stop(out), 0);

    CHECK_EQ(dig.start(10, 64, -3, 1, false, out), 1);
    CHECK_EQ(out[0].integer(0), i64(0));
    CHECK_EQ(out[0].integer(1), i64(10));
    CHECK_EQ(out[0].integer(4), i64(1));

    CHECK_EQ(dig.progress(10, 64, -3, 1, false, out), 1);
    CHECK_EQ(out[0].integer(0), i64(1));

    CHECK_EQ(dig.progress(10, 64, -3, 1, true, out), 2);
    CHECK_EQ(out[1].integer(0), i64(3));
    CHECK_EQ(out[1].integer(3), i64(-3));

    CHECK_EQ(dig.stop(out), 1);
    CHECK_EQ(out[0].integer(0), i64(2));
    CHECK_EQ(out[0].integer(1), i64(0));
    CHECK_EQ(dig.stop(out), 0);

    // A torch goes on the click.
    CHECK_EQ(dig.start(1, 2, 3, 5, true, out), 2);
    CHECK_EQ(out[1].integer(0), i64(3));

    HeldItemReporter held;
    Packet h;
    CHECK(!held.changed(0, &h));
    CHECK(held.changed(276, &h));
    CHECK_EQ(h.id, packet::BlockItemSwitch);
    CHECK_EQ(h.integer(0), i64(0));
    CHECK_EQ(h.integer(1), i64(276));
    CHECK(!held.changed(276, &h));
    CHECK(held.changed(0, &h));
}

// **The stance goes on a change and never again**, like the held item: b1.2's
// `of.W()` sends 1 when the crouch starts and 2 when it ends, the death is this
// player's own 3, and `of.u()`'s Respawn brings them back standing.
TEST(a_crouch_a_death_and_a_respawn_are_each_reported_once)
{
    StanceReporter stance;
    Packet out[StanceReporter::kMaxPackets];

    // Nothing before the Login has given this player an id.
    CHECK_EQ(stance.tick(0, true, true, out), 0);

    CHECK_EQ(stance.tick(5, false, true, out), 0);  // standing, as everyone assumes
    CHECK_EQ(stance.tick(5, true, true, out), 1);
    CHECK_EQ(out[0].id, packet::EntityAction);
    CHECK_EQ(out[0].integer(0), i64(5));
    CHECK_EQ(out[0].integer(1), i64(kActionCrouch));
    CHECK_EQ(stance.tick(5, true, true, out), 0);
    CHECK_EQ(stance.tick(5, false, true, out), 1);
    CHECK_EQ(out[0].integer(1), i64(kActionUncrouch));

    // Died crouching: the death alone, and nothing more while dead.
    CHECK_EQ(stance.tick(5, true, true, out), 1);
    CHECK_EQ(stance.tick(5, true, false, out), 1);
    CHECK_EQ(out[0].id, packet::EntityStatus);
    CHECK_EQ(out[0].integer(0), i64(5));
    CHECK_EQ(out[0].integer(1), i64(entity::kStatusDead));
    CHECK_EQ(stance.tick(5, false, false, out), 0);
    CHECK_EQ(stance.tick(5, true, false, out), 0);

    // Back, and already crouching on the first tick: the respawn first, then
    // the crouch against the standing body the respawn stands up.
    CHECK_EQ(stance.tick(5, true, true, out), 2);
    CHECK_EQ(out[0].id, packet::Respawn);
    CHECK_EQ(out[1].id, packet::EntityAction);
    CHECK_EQ(out[1].integer(1), i64(kActionCrouch));
    CHECK_EQ(stance.tick(5, true, true, out), 0);
}

TEST(a_thrown_item_is_sent_in_32nds_floored_and_its_motion_in_128ths_truncated)
{
    entity::ItemEntity item;
    item.item = 280;
    item.count = 3;
    item.x = -0.01;
    item.y = 64.99;
    item.z = 12.5;
    item.motionX = 0.3;
    item.motionY = -0.2;
    item.motionZ = 1.5;  // 192, which a byte holds as -64

    const Packet p = pickupSpawnFor(item);
    CHECK_EQ(p.id, packet::PickupSpawn);
    CHECK_EQ(p.integer(1), i64(280));
    CHECK_EQ(p.integer(2), i64(3));
    CHECK_EQ(p.integer(3), i64(-1));
    CHECK_EQ(p.integer(4), i64(2079));
    CHECK_EQ(p.integer(5), i64(400));
    CHECK_EQ(p.integer(6), i64(38));
    CHECK_EQ(p.integer(7), i64(-25));
    CHECK_EQ(p.integer(8), i64(-64));
}
