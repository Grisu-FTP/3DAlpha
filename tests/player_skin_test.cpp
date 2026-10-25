// The skin a player sends and what the other end will accept of it: one exact
// 64 x 32 page, cleaned on the way in, attributed to whoever sent it, and worn
// by the right body. See core/net/player_skin.hpp.

#include "framework.hpp"

#include "core/net/entities.hpp"
#include "core/net/player_skin.hpp"
#include "core/render/remote_player_mesh.hpp"

#include <cmath>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

Packet namedSpawn(i32 id)
{
    Packet p;
    p.reset(packet::NamedEntitySpawn);
    p.pushInt(id);
    p.pushString("Ada");
    for (int i = 0; i < 3; ++i) {
        p.pushInt(0);
    }
    p.pushInt(0);
    p.pushInt(0);
    p.pushInt(0);
    return p;
}

// A page of one colour, every texel at `alpha`.
std::vector<u8> filled(u8 r, u8 g, u8 b, u8 alpha)
{
    std::vector<u8> page(kSkinPageBytes);
    for (usize i = 0; i < kSkinPageBytes; i += 4) {
        page[i] = r;
        page[i + 1] = g;
        page[i + 2] = b;
        page[i + 3] = alpha;
    }
    return page;
}

usize texelAt(int x, int y) { return (usize(y) * kSkinPageWidthTexels + usize(x)) * 4; }

}  // namespace

TEST(a_skin_page_of_any_other_size_is_refused)
{
    const std::vector<u8> page = filled(10, 20, 30, 255);
    u8 out[kSkinPageBytes];
    CHECK(sanitizeSkinPage(page.data(), page.size(), out));
    CHECK(!sanitizeSkinPage(page.data(), page.size() - 1, out));
    CHECK(!sanitizeSkinPage(page.data(), 0, out));
    CHECK(!sanitizeSkinPage(nullptr, kSkinPageBytes, out));
    std::vector<u8> bigger(kSkinPageBytes * 2, 255);
    CHECK(!sanitizeSkinPage(bigger.data(), bigger.size(), out));
}

TEST(a_skin_page_is_cleaned_of_everything_the_model_never_reads)
{
    std::vector<u8> page = filled(200, 100, 50, 255);
    // Texels nothing samples: the strip right of the arms, and a transparent
    // texel with colour in it where a hat might be.
    page[texelAt(60, 24) + 0] = 1;
    page[texelAt(40, 4) + 3] = 0;  // hat: transparent, but carrying colour
    // The head and the legs made invisible by the sender.
    page[texelAt(4, 4) + 3] = 0;
    page[texelAt(4, 20) + 3] = 0;
    // An arm texel transparent, as a narrow-arm skin has.
    page[texelAt(54, 20) + 3] = 0;

    u8 out[kSkinPageBytes];
    CHECK(sanitizeSkinPage(page.data(), page.size(), out));
    // Unused: gone entirely.
    CHECK_EQ(int(out[texelAt(60, 24) + 0]), 0);
    CHECK_EQ(int(out[texelAt(60, 24) + 3]), 0);
    // A transparent hat texel keeps no colour.
    CHECK_EQ(int(out[texelAt(40, 4) + 3]), 0);
    CHECK_EQ(int(out[texelAt(40, 4) + 0]), 0);
    // Head and legs cannot be invisible.
    CHECK_EQ(int(out[texelAt(4, 4) + 3]), 255);
    CHECK_EQ(int(out[texelAt(4, 20) + 3]), 255);
    // The arm is left as it was sent.
    CHECK_EQ(int(out[texelAt(54, 20) + 3]), 0);
    // And an ordinary texel is untouched.
    CHECK_EQ(int(out[texelAt(20, 20) + 0]), 200);
}

TEST(a_player_skin_packet_round_trips_and_is_validated_on_reading)
{
    const std::vector<u8> page = filled(9, 8, 7, 255);
    const Packet packet = makePlayerSkin(77, page.data());

    std::vector<u8> bytes;
    CHECK(encodePacket(packet, &bytes));
    Packet back;
    usize consumed = 0;
    CHECK(parsePacket(bytes.data(), bytes.size(), &back, &consumed) == ParseResult::Ok);
    CHECK_EQ(consumed, bytes.size());

    i32 id = 0;
    u8 out[kSkinPageBytes];
    CHECK(readPlayerSkin(back, &id, out));
    CHECK_EQ(id, i32(77));
    CHECK_EQ(int(out[texelAt(20, 20)]), 9);

    // A short payload is refused, however it got there.
    Packet shortOne = back;
    shortOne.bytes.resize(100);
    CHECK(!readPlayerSkin(shortOne, &id, out));
    // So is some other packet.
    Packet other;
    other.reset(packet::KeepAlive);
    CHECK(!readPlayerSkin(other, &id, out));
}

TEST(skins_are_kept_for_the_player_they_name_and_forgotten_with_them)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7), nullptr));
    const std::vector<u8> page = filled(1, 2, 3, 255);

    // Nobody is tracked under id 9, so its skin has nobody to be worn by.
    CHECK(entities.apply(makePlayerSkin(9, page.data()), nullptr));
    CHECK_EQ(entities.skins().slotOf(9), -1);

    CHECK(entities.apply(makePlayerSkin(7, page.data()), nullptr));
    const int slot = entities.skins().slotOf(7);
    CHECK(slot >= 0);
    const u32 revision = entities.skins().revision(slot);

    // The same page again changes nothing, so nothing is uploaded again.
    CHECK(entities.apply(makePlayerSkin(7, page.data()), nullptr));
    CHECK_EQ(entities.skins().revision(slot), revision);

    // A respawn is a new spawn, and the skin stays.
    CHECK(entities.apply(namedSpawn(7), nullptr));
    CHECK_EQ(entities.skins().slotOf(7), slot);

    Packet destroy;
    destroy.reset(packet::DestroyEntity);
    destroy.pushInt(7);
    CHECK(entities.apply(destroy, nullptr));
    CHECK_EQ(entities.skins().slotOf(7), -1);
}

TEST(a_body_with_its_own_skin_samples_its_own_page)
{
    RemoteEntities entities;
    CHECK(entities.apply(namedSpawn(7), nullptr));
    std::vector<mesh::DetailVertex> before(render::kRemotePlayerVerticesEach);
    CHECK_EQ(render::buildRemotePlayers(entities, 0, 0, 0, 0.0f, before.data(),
                                        int(before.size())),
             render::kRemotePlayerVerticesEach);

    const std::vector<u8> page = filled(1, 2, 3, 255);
    CHECK(entities.apply(makePlayerSkin(7, page.data()), nullptr));
    std::vector<mesh::DetailVertex> after(render::kRemotePlayerVerticesEach);
    CHECK_EQ(render::buildRemotePlayers(entities, 0, 0, 0, 0.0f, after.data(),
                                        int(after.size())),
             render::kRemotePlayerVerticesEach);

    int slotX = 0, slotY = 0, baseX = 0, baseY = 0;
    CHECK(texture::remoteSkinOrigin(entities.skins().slotOf(7), &slotX, &slotY));
    texture::skinOrigin(texture::EntitySkin::OtherPlayer, &baseX, &baseY);
    const int unit = mesh::kUvUnitsPerAtlas / texture::kEntitySheetWidth;
    for (usize v = 0; v < before.size(); ++v) {
        CHECK_EQ(int(after[v].u) - int(before[v].u), (slotX - baseX) * unit);
        CHECK_EQ(int(after[v].v) - int(before[v].v), (slotY - baseY) * unit);
        // Where the body is does not move.
        CHECK_EQ(int(after[v].x), int(before[v].x));
    }
}

TEST(the_remote_skin_slots_sit_in_free_rows_of_the_sheet_and_inside_it)
{
    for (int slot = 0; slot < texture::kRemoteSkinSlots; ++slot) {
        int x = 0, y = 0;
        CHECK(texture::remoteSkinOrigin(slot, &x, &y));
        CHECK(x >= 0 && x + texture::kSkinPageWidth <= texture::kEntitySheetWidth);
        CHECK(y >= 160 && y + texture::kSkinPageHeight <= texture::kEntitySheetHeight);
        // No existing page reaches this far down.
        for (int skin = 0; skin < texture::kEntitySkinCount; ++skin) {
            int ox = 0, oy = 0;
            texture::skinOrigin(texture::EntitySkin(skin), &ox, &oy);
            CHECK(oy + texture::kSkinPageHeight <= 160);
        }
    }
    int x = 0, y = 0;
    CHECK(!texture::remoteSkinOrigin(-1, &x, &y));
    CHECK(!texture::remoteSkinOrigin(texture::kRemoteSkinSlots, &x, &y));
}
