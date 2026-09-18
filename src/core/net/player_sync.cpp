// EntityClientPlayerMP's per-tick reports. See player_sync.hpp.

#include "core/net/player_sync.hpp"

#include <cmath>

namespace mc::net {

namespace {

bool sameStack(const WireStack& a, const WireStack& b)
{
    if (a.empty() || b.empty()) {
        return a.empty() == b.empty();
    }
    return a.id == b.id && a.count == b.count && a.damage == b.damage;
}

WireStack wireOf(const item::ItemStack& stack)
{
    if (stack.empty()) {
        return WireStack{};
    }
    return WireStack{stack.id, stack.count, stack.damage};
}

}  // namespace

Packet MovementReporter::tick(const PlayerPose& pose)
{
    const bool moved = pose.x - x_ != 0.0 || pose.feetY - feetY_ != 0.0
                       || pose.eyeY - eyeY_ != 0.0 || pose.z - z_ != 0.0;
    const bool turned = double(pose.yaw - yaw_) != 0.0 || double(pose.pitch - pitch_) != 0.0;

    Packet out;
    if (moved && turned) {
        out = makePositionLook(pose.x, pose.feetY, pose.eyeY, pose.z, pose.yaw, pose.pitch,
                               pose.onGround);
    } else if (moved) {
        out = makePosition(pose.x, pose.feetY, pose.eyeY, pose.z, pose.onGround);
    } else if (turned) {
        out = makeLook(pose.yaw, pose.pitch, pose.onGround);
    } else {
        out = makeFlying(pose.onGround);
    }

    if (moved) {
        x_ = pose.x;
        feetY_ = pose.feetY;
        eyeY_ = pose.eyeY;
        z_ = pose.z;
    }
    if (turned) {
        yaw_ = pose.yaw;
        pitch_ = pose.pitch;
    }
    return out;
}

bool readTeleport(const Packet& packet, Teleport* out)
{
    *out = Teleport{};
    switch (packet.id) {
    case packet::Flying:
        return true;
    case packet::PlayerPosition:
        out->hasPosition = true;
        break;
    case packet::PlayerLook:
        out->hasLook = true;
        out->yaw = float(packet.real(0));
        out->pitch = float(packet.real(1));
        return true;
    case packet::PlayerPositionLook:
        out->hasPosition = true;
        out->hasLook = true;
        out->yaw = float(packet.real(4));
        out->pitch = float(packet.real(5));
        break;
    default:
        return false;
    }
    out->x = packet.real(0);
    out->eyeY = packet.real(1);
    out->z = packet.real(3);
    return true;
}

int InventoryReporter::tick(const item::Inventory& inventory, Packet out[3])
{
    if (++ticks_ != kPeriodTicks) {
        return 0;
    }
    ticks_ = 0;

    WireStack main[kWireMainSlots];
    WireStack armour[kWireArmourSlots];
    toWire(inventory, main, armour);

    bool same = haveSnapshot_;
    for (int i = 0; same && i < kWireMainSlots; ++i) {
        same = sameStack(main[i], main_[i]);
    }
    for (int i = 0; same && i < kWireArmourSlots; ++i) {
        same = sameStack(armour[i], armour_[i]);
    }
    if (same) {
        return 0;
    }

    const WireStack crafting[kWireCraftingSlots];
    out[0] = makeInventory(kInventoryMain, main, kWireMainSlots);
    out[1] = makeInventory(kInventoryCrafting, crafting, kWireCraftingSlots);
    out[2] = makeInventory(kInventoryArmour, armour, kWireArmourSlots);

    for (int i = 0; i < kWireMainSlots; ++i) main_[i] = main[i];
    for (int i = 0; i < kWireArmourSlots; ++i) armour_[i] = armour[i];
    haveSnapshot_ = true;
    return 3;
}

void toWire(const item::Inventory& inventory, WireStack* main, WireStack* armour)
{
    for (int i = 0; i < kWireMainSlots; ++i) {
        main[i] = i < item::kMainSlots ? wireOf(inventory.main[i]) : WireStack{};
    }
    for (int i = 0; i < kWireArmourSlots; ++i) {
        armour[i] = i < item::kArmourSlots ? wireOf(inventory.armour[i]) : WireStack{};
    }
}

bool applyInventory(const Packet& packet, item::Inventory* inventory)
{
    if (packet.id != packet::PlayerInventory) {
        return false;
    }
    const auto assign = [](item::ItemStack* slots, int count, const std::vector<WireStack>& wire) {
        for (int i = 0; i < count; ++i) {
            item::ItemStack& slot = slots[i];
            const WireStack w = usize(i) < wire.size() ? wire[usize(i)] : WireStack{};
            if (w.empty()) {
                slot.id = item::kEmptyItemId;
                slot.count = 0;
                slot.damage = 0;
            } else {
                slot.id = w.id;
                slot.count = w.count;
                slot.damage = w.damage;
            }
        }
    };

    switch (packet.integer(0)) {
    case kInventoryMain:
        assign(inventory->main, item::kMainSlots, packet.stacks);
        return true;
    case kInventoryArmour:
        assign(inventory->armour, item::kArmourSlots, packet.stacks);
        return true;
    default:
        return false;
    }
}

int DigReporter::start(i32 x, int y, i32 z, int face, bool broke, Packet out[2])
{
    digging_ = true;
    out[0] = makeDig(0, x, y, z, face);
    if (!broke) {
        return 1;
    }
    out[1] = makeDig(3, x, y, z, face);
    return 2;
}

int DigReporter::progress(i32 x, int y, i32 z, int face, bool broke, Packet out[2])
{
    digging_ = true;
    out[0] = makeDig(1, x, y, z, face);
    if (!broke) {
        return 1;
    }
    out[1] = makeDig(3, x, y, z, face);
    return 2;
}

int DigReporter::stop(Packet out[1])
{
    if (!digging_) {
        return 0;
    }
    digging_ = false;
    out[0] = makeDig(2, 0, 0, 0, 0);
    return 1;
}

bool HeldItemReporter::changed(int itemId, Packet* out)
{
    if (itemId == last_) {
        return false;
    }
    last_ = itemId;
    *out = makeHoldingChange(itemId);
    return true;
}

Packet pickupSpawnFor(const entity::ItemEntity& item)
{
    // `eo.b(D)I` is a floor; the motion is a plain `(byte)(int)`, which cuts
    // towards zero and wraps. Clamped before the cast, where Java's d2i
    // saturates and C++'s would be undefined.
    const auto fixed = [](double v) {
        const double scaled = std::floor(v * 32.0);
        return i32(scaled < -2147483648.0 ? -2147483648.0 : (scaled > 2147483647.0 ? 2147483647.0 : scaled));
    };
    const auto motion = [](double v) {
        double scaled = v * 128.0;
        scaled = scaled < -2147483648.0 ? -2147483648.0 : (scaled > 2147483647.0 ? 2147483647.0 : scaled);
        return int(i8(u8(u32(i32(scaled)))));
    };
    return makePickupSpawn(0, int(item.item), item.count, fixed(item.x), fixed(item.y), fixed(item.z),
                           motion(item.motionX), motion(item.motionY), motion(item.motionZ));
}

}  // namespace mc::net
