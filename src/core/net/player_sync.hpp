#pragma once

// What a multiplayer client tells the server about its own player every tick:
// where it is, which way it faces, and -- once a second -- what it carries.
//
// **`la.J()`, EntityClientPlayerMP's tick, is the whole of it.** It compares the
// player with the values it last *sent*, not with last tick: position is four
// numbers (x, feet, eye, z) and look is two. Both changed is a Position & Look,
// one of them is a Position or a Look, and neither is a bare Flying packet --
// **something goes every tick**, which is also what keeps the server's read
// timeout from ever firing during play. Only the half that went out is
// remembered, so a look that did not change is never re-sent with a move.
//
// The inventory is a1.1.2's multiplayer model, which is not a server-owned one:
// every twentieth tick the client compares its inventory with a snapshot and,
// if anything differs, sends all three arrays -- main (-1), crafting (-2) and
// armour (-3) -- and the server simply adopts them. The server's only pushes
// are the whole inventory at login and Add To Inventory for a pickup.

#include "core/entity/item_entity.hpp"
#include "core/item/inventory.hpp"
#include "core/net/packets.hpp"
#include "core/util/types.hpp"

namespace mc::net {

// `eu`'s three arrays as the wire carries them. The main one is 37 long in
// a1.1.2; the last slot is never filled by play.
inline constexpr int kWireMainSlots = 37;
inline constexpr int kWireCraftingSlots = 4;
inline constexpr int kWireArmourSlots = 4;
inline constexpr int kInventoryMain = -1;
inline constexpr int kInventoryCrafting = -2;
inline constexpr int kInventoryArmour = -3;

struct PlayerPose {
    double x = 0.0;
    double feetY = 0.0;  // `boundingBox.minY`
    double eyeY = 0.0;   // `posY`, feet plus 1.62
    double z = 0.0;
    float yaw = 0.0f;    // degrees, the game's own convention
    float pitch = 0.0f;
    bool onGround = false;
};

class MovementReporter {
public:
    // The packet `la.J()` would send for this tick.
    Packet tick(const PlayerPose& pose);

    // Forgets what was sent, so the next tick reports everything -- for a new
    // session. The fields start at zero in the original too, which is why its
    // first tick is always a Position & Look.
    void reset() { *this = MovementReporter(); }

private:
    double x_ = 0.0;
    double feetY_ = 0.0;
    double eyeY_ = 0.0;
    double z_ = 0.0;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
};

// A Player Position & Look from the server, as `gy.a(eh)` reads it: the second
// wire field is the eye height it sets `posY` to and the third is ignored.
struct Teleport {
    double x = 0.0;
    double eyeY = 0.0;
    double z = 0.0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool hasPosition = false;
    bool hasLook = false;
};

// Reads any of 0x0A..0x0D coming from the server. False for another packet.
bool readTeleport(const Packet& packet, Teleport* out);

class InventoryReporter {
public:
    static constexpr int kPeriodTicks = 20;

    // One tick. Returns the number of packets written to `out` -- three when
    // the twentieth tick finds the inventory changed, otherwise none.
    int tick(const item::Inventory& inventory, Packet out[3]);

    void reset() { *this = InventoryReporter(); }

private:
    int ticks_ = 0;
    bool haveSnapshot_ = false;
    WireStack main_[kWireMainSlots];
    WireStack armour_[kWireArmourSlots];
};

// The inventory's main and armour arrays in wire form. Crafting is always
// empty: the 2x2 grid does not persist here.
void toWire(const item::Inventory& inventory, WireStack* main, WireStack* armour);

// Applies a Player Inventory from the server. False when the type is not one
// this inventory holds (crafting) or the packet is not a Player Inventory.
bool applyInventory(const Packet& packet, item::Inventory* inventory);

// **What digging tells the server: `nj`, PlayerControllerMP.** The server does
// its own count -- 0.2.1 adds the block's strength once per status-1 packet and
// breaks it when that reaches one -- so the client has to say, every tick it is
// digging, that it still is. The client breaks the block locally when its own
// count gets there and says so with a 3; the server's echo is what makes that
// edit stick. See core/net/pending_edits.hpp.
class DigReporter {
public:
    // `nj.a(IIII)`: a click on a block. `broke` is whether the click itself
    // finished it -- a block with no hardness -- which is `nj.b(IIII)`'s
    // status 3 straight after.
    int start(i32 x, int y, i32 z, int face, bool broke, Packet out[2]);

    // `nj.c(IIII)`: one tick of holding the button on a block, and a 3 behind
    // it on the tick the block gives.
    int progress(i32 x, int y, i32 z, int face, bool broke, Packet out[2]);

    // `nj.a()`: the button came up or the aim left every block. Sends nothing
    // unless a dig was under way.
    int stop(Packet out[1]);

    bool digging() const { return digging_; }

private:
    bool digging_ = false;
};

// `nj.e()`: the id in the hand, reported whenever it is not the one last
// reported -- ahead of a dig tick and ahead of a place. Zero is an empty hand.
class HeldItemReporter {
public:
    bool changed(int itemId, Packet* out);

private:
    int last_ = 0;
};

// `ha(dx)`: an item the player threw, as the Pickup Spawn that asks the server
// to make it. A multiplayer client never keeps a thrown item for itself --
// `la.a(dx)` sends it and lets it go -- and the stack's damage does not survive
// the trip, because the packet has nowhere to put it.
Packet pickupSpawnFor(const entity::ItemEntity& item);

}  // namespace mc::net
