#pragma once

// **A player's skin, as it crosses the link and as the other end keeps it.**
//
// a1.1.2 fetched a skin from a Mojang URL by name, and that service is gone, so
// here each console sends the skin it is wearing to the host and the host
// passes it on (`packet::PlayerSkin`, `0xE3`). Only a 3DAlpha/AlphaU host or
// guest ever sends or understands it.
//
// **The wire carries pixels, not a file.** The payload is exactly one 64 x 32
// RGBA page -- `kSkinPageBytes` -- and nothing else: no PNG header, no chunks,
// no length-prefixed anything inside it. So there is no decoder on the receiving
// side for a crafted file to attack, no way to ask for a large allocation, and
// nowhere for a second payload to hide. A sender that has a PNG, or a 64 x 64
// or HD skin, has already been through `applyPlayerSkin`, whose `decodePng`
// carries the pixel ceiling and the shape checks; what is sent is its result.
//
// **What arrives is still not trusted.** `sanitizeSkinPage` is applied to every
// page on its way in -- on the host to a guest's, on a guest to the host's --
// and it:
//
//   * refuses anything but the exact size;
//   * clears every texel the model never samples (the unwrap's unused corners
//     and the strip beside the arms), so nothing is carried there;
//   * zeroes the colour of every fully transparent texel, so a "transparent"
//     texel is not a place to hold data either;
//   * makes the head, body and legs opaque, so a skin cannot make its wearer
//     invisible to everyone else. (An arm is left as sent: a narrow-arm skin is
//     transparent in its last two columns on purpose.)
//
// **The entity id in the packet is believed only from the host.** A guest's own
// packet is attributed to that guest by the host, whatever id it says, and is
// accepted once per connection -- a skin is chosen on the main menu, so there is
// never a second one and a flood of them is simply dropped.

#include "core/net/packets.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::net {

inline constexpr int kSkinPageWidthTexels = texture::kSkinPageWidth;
inline constexpr int kSkinPageHeightTexels = texture::kSkinPageHeight;
inline constexpr usize kSkinPageBytes =
    usize(kSkinPageWidthTexels) * usize(kSkinPageHeightTexels) * 4;

// Copies the player's page out of an entity sheet. False when the sheet is not
// the size `buildEntitySkins` makes.
bool copyPlayerPage(const std::vector<u8>& sheet, u8 out[kSkinPageBytes]);

// Validates and cleans `data` into `out` as the header describes. False, with
// `out` untouched, when `size` is not `kSkinPageBytes`.
bool sanitizeSkinPage(const u8* data, usize size, u8 out[kSkinPageBytes]);

// `0xE3`: the entity the skin belongs to, and the page.
Packet makePlayerSkin(i32 entityId, const u8 page[kSkinPageBytes]);

// The id the packet names and the page, already sanitised. False for another
// packet id or a payload that is not exactly a page.
bool readPlayerSkin(const Packet& packet, i32* entityId, u8 page[kSkinPageBytes]);

// **The skins of the other people in the room, one page each.**
//
// Eight, as many as `RemoteEntities` tracks. A page is addressed by the slot
// that holds it; `texture::remoteSkinOrigin` says where that slot's page is in
// the entity sheet, and `revision` is what the renderer compares with the last
// one it uploaded -- it moves whenever the slot's bytes or owner change.
class PlayerSkins {
public:
    static constexpr int kSlots = texture::kRemoteSkinSlots;

    // Keeps `page` (already sanitised) for `entityId`, replacing that entity's
    // previous one or the least recently stored slot when all are taken. A page
    // identical to the one held changes nothing.
    void store(i32 entityId, const u8 page[kSkinPageBytes]);

    // The slot wearing `entityId`'s skin, or -1.
    int slotOf(i32 entityId) const;

    const u8* page(int slot) const
    {
        return slot >= 0 && slot < kSlots ? slots_[slot].page : nullptr;
    }
    u32 revision(int slot) const
    {
        return slot >= 0 && slot < kSlots ? slots_[slot].revision : 0;
    }

    // That entity has gone; its slot is free for the next.
    void release(i32 entityId);
    void clear();

private:
    struct Slot {
        bool used = false;
        i32 entityId = 0;
        u32 revision = 0;
        u32 stamp = 0;
        u8 page[kSkinPageBytes] = {};
    };
    Slot slots_[kSlots];
    u32 clock_ = 0;
};

}  // namespace mc::net
