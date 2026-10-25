// A player's skin on the wire and in the room. See player_skin.hpp.

#include "core/net/player_skin.hpp"

#include <cstring>

namespace mc::net {
namespace {

struct Rect {
    int x0, y0, x1, y1;  // half open, in texels
};

// What `ModelBiped`'s boxes sample out of a 64 x 32 page: the head and the hat
// beside it across the top, and under them the legs, the body and the arms.
// Everything else in the page is never read.
constexpr Rect kUsed[] = {
    {0, 0, 64, 16},   // head (0, 0) and hat (32, 0)
    {0, 16, 16, 32},  // legs
    {16, 16, 40, 32}, // body
    {40, 16, 56, 32}, // arms
};

// The parts that must not be transparent: an arm is left alone, because a
// narrow-arm skin is empty in the last two columns of its unwrap on purpose.
constexpr Rect kOpaque[] = {
    {0, 0, 32, 16},    // head
    {0, 16, 16, 32},   // legs
    {16, 16, 40, 32},  // body
};

bool inside(const Rect& rect, int x, int y)
{
    return x >= rect.x0 && x < rect.x1 && y >= rect.y0 && y < rect.y1;
}

}  // namespace

bool copyPlayerPage(const std::vector<u8>& sheet, u8 out[kSkinPageBytes])
{
    if (sheet.size() != texture::kEntitySheetBytes) {
        return false;
    }
    int originX = 0;
    int originY = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &originX, &originY);
    for (int y = 0; y < kSkinPageHeightTexels; ++y) {
        const u8* from =
            &sheet[(usize(originY + y) * texture::kEntitySheetWidth + usize(originX)) * 4];
        std::memcpy(out + usize(y) * kSkinPageWidthTexels * 4, from,
                    usize(kSkinPageWidthTexels) * 4);
    }
    return true;
}

bool sanitizeSkinPage(const u8* data, usize size, u8 out[kSkinPageBytes])
{
    if (data == nullptr || size != kSkinPageBytes) {
        return false;
    }
    for (int y = 0; y < kSkinPageHeightTexels; ++y) {
        for (int x = 0; x < kSkinPageWidthTexels; ++x) {
            const usize at = (usize(y) * kSkinPageWidthTexels + usize(x)) * 4;
            bool used = false;
            for (const Rect& rect : kUsed) {
                used = used || inside(rect, x, y);
            }
            bool opaque = false;
            for (const Rect& rect : kOpaque) {
                opaque = opaque || inside(rect, x, y);
            }
            u8 r = data[at + 0];
            u8 g = data[at + 1];
            u8 b = data[at + 2];
            u8 a = data[at + 3];
            if (!used) {
                r = g = b = a = 0;
            } else if (opaque) {
                a = 255;
            } else if (a == 0) {
                r = g = b = 0;
            }
            out[at + 0] = r;
            out[at + 1] = g;
            out[at + 2] = b;
            out[at + 3] = a;
        }
    }
    return true;
}

Packet makePlayerSkin(i32 entityId, const u8 page[kSkinPageBytes])
{
    Packet p;
    p.reset(packet::PlayerSkin);
    p.pushInt(entityId);
    p.bytes.assign(page, page + kSkinPageBytes);
    return p;
}

bool readPlayerSkin(const Packet& packet, i32* entityId, u8 page[kSkinPageBytes])
{
    if (packet.id != packet::PlayerSkin || !sanitizeSkinPage(packet.bytes.data(),
                                                             packet.bytes.size(), page)) {
        return false;
    }
    *entityId = i32(packet.integer(0));
    return true;
}

void PlayerSkins::store(i32 entityId, const u8 page[kSkinPageBytes])
{
    Slot* target = nullptr;
    for (Slot& slot : slots_) {
        if (slot.used && slot.entityId == entityId) {
            target = &slot;
            break;
        }
    }
    if (target == nullptr) {
        for (Slot& slot : slots_) {
            if (!slot.used) {
                target = &slot;
                break;
            }
        }
    }
    if (target == nullptr) {
        target = &slots_[0];
        for (Slot& slot : slots_) {
            if (slot.stamp < target->stamp) {
                target = &slot;
            }
        }
    }
    target->stamp = ++clock_;
    if (target->used && target->entityId == entityId
        && std::memcmp(target->page, page, kSkinPageBytes) == 0) {
        return;
    }
    target->used = true;
    target->entityId = entityId;
    std::memcpy(target->page, page, kSkinPageBytes);
    ++target->revision;
}

int PlayerSkins::slotOf(i32 entityId) const
{
    for (int i = 0; i < kSlots; ++i) {
        if (slots_[i].used && slots_[i].entityId == entityId) {
            return i;
        }
    }
    return -1;
}

void PlayerSkins::release(i32 entityId)
{
    for (Slot& slot : slots_) {
        if (slot.used && slot.entityId == entityId) {
            slot.used = false;
            ++slot.revision;
        }
    }
}

void PlayerSkins::clear()
{
    for (Slot& slot : slots_) {
        slot.used = false;
        ++slot.revision;
    }
}

}  // namespace mc::net
