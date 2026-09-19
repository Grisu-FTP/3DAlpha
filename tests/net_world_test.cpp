// Map Chunk payloads, WorldClient's revert list, and the server list: the parts
// of multiplayer that touch the world and the card rather than the socket.

#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/net/chunk_payload.hpp"
#include "core/net/pending_edits.hpp"
#include "core/net/server_list.hpp"
#include "core/world/chunk.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace mc;
using namespace mc::net;
using world::ChunkColumn;

namespace {

// A column with something different in every plane, so a plane landing in the
// wrong place or a run landing in the wrong column shows up.
std::unique_ptr<ChunkColumn> patterned(i32 cx, i32 cz)
{
    auto column = std::make_unique<ChunkColumn>(cx, cz);
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < ChunkColumn::kHeight; ++y) {
                const int v = x * 7 + z * 13 + y * 3 + cx * 5 + cz * 11;
                column->setBlock(x, y, z, y < 60 ? u16(1 + (v & 3)) : 0);
                column->setBlockData(x, y, z, u8(v & 15));
                column->setBlockLight(x, y, z, u8((v >> 1) & 15));
                column->setSkyLight(x, y, z, u8((v >> 2) & 15));
            }
        }
    }
    return column;
}

struct Columns {
    std::map<std::pair<i32, i32>, std::unique_ptr<ChunkColumn>> map;

    static ChunkColumn* find(void* ctx, i32 cx, i32 cz)
    {
        auto& self = *static_cast<Columns*>(ctx);
        auto it = self.map.find({cx, cz});
        return it == self.map.end() ? nullptr : it->second.get();
    }
    static const ChunkColumn* findConst(void* ctx, i32 cx, i32 cz) { return find(ctx, cx, cz); }
};

bool sameColumn(const ChunkColumn& a, const ChunkColumn& b)
{
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < ChunkColumn::kHeight; ++y) {
                if (a.block(x, y, z) != b.block(x, y, z) || a.blockData(x, y, z) != b.blockData(x, y, z)
                    || a.blockLight(x, y, z) != b.blockLight(x, y, z)
                    || a.skyLight(x, y, z) != b.skyLight(x, y, z)) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

TEST(a_whole_column_survives_the_server_s_layout_and_a_deflate)
{
    Columns source;
    source.map[{-3, 7}] = patterned(-3, 7);

    Packet packet;
    CHECK(makeMapChunk(&Columns::findConst, &source, -48, 0, 112, 16, ChunkColumn::kHeight, 16,
                       &packet));
    CHECK_EQ(packet.integer(3), i64(15));
    CHECK_EQ(packet.integer(4), i64(127));

    MapChunkRegion region;
    CHECK(inflateMapChunk(packet, &region));
    CHECK(region.wholeColumn());
    CHECK_EQ(region.data.size(), usize(16 * 16 * 128 * 5 / 2));

    // The first run is (x=0, z=0) bottom to top, and the metadata plane starts
    // straight after the last block run.
    const ChunkColumn& original = *source.map[{-3, 7}];
    CHECK_EQ(region.data[5], u8(original.block(0, 5, 0)));
    CHECK_EQ(region.data[128 + 1], u8(original.block(0, 1, 1)));
    CHECK_EQ(region.data[16 * 16 * 128] & 15, int(original.blockData(0, 0, 0)));

    const std::unique_ptr<ChunkColumn> built = buildColumn(region);
    CHECK(built != nullptr);
    CHECK_EQ(built->x, i32(-3));
    CHECK_EQ(built->z, i32(7));
    CHECK(sameColumn(*built, original));
    // Everything at y >= 60 is air, so no column's height map may reach past it.
    CHECK(built->heightMap[0] <= 60);
    CHECK(built->heightMap[0] > 0);
}

TEST(a_region_with_an_odd_height_across_a_chunk_border_lands_where_the_server_took_it)
{
    Columns source;
    source.map[{0, 0}] = patterned(0, 0);
    source.map[{1, 0}] = patterned(1, 0);

    // x 14..17 crosses from chunk 0 into chunk 1; y 3..7 is five tall, so each
    // nibble run copies two bytes and the fifth nibble is not sent.
    Packet packet;
    CHECK(makeMapChunk(&Columns::findConst, &source, 14, 3, 2, 4, 5, 3, &packet));

    Columns target;
    target.map[{0, 0}] = std::make_unique<ChunkColumn>(0, 0);
    target.map[{1, 0}] = std::make_unique<ChunkColumn>(1, 0);

    MapChunkRegion region;
    CHECK(inflateMapChunk(packet, &region));
    CHECK(!region.wholeColumn());
    std::vector<u8> scratch;
    CHECK_EQ(applyMapChunk(region, &Columns::find, &target, &scratch), 2);

    const ChunkColumn& a = *target.map[{0, 0}];
    const ChunkColumn& b = *target.map[{1, 0}];
    const ChunkColumn& srcA = *source.map[{0, 0}];
    const ChunkColumn& srcB = *source.map[{1, 0}];
    CHECK_EQ(a.block(14, 3, 2), srcA.block(14, 3, 2));
    CHECK_EQ(a.block(15, 7, 4), srcA.block(15, 7, 4));
    CHECK_EQ(b.block(0, 5, 3), srcB.block(0, 5, 3));
    CHECK_EQ(b.block(1, 7, 4), srcB.block(1, 7, 4));
    CHECK_EQ(a.block(13, 3, 2), u16(0));
    CHECK_EQ(b.block(2, 3, 2), u16(0));
    CHECK_EQ(a.block(14, 8, 2), u16(0));

    // Nibble bytes start at (y0 >> 1): y=2 and y=3, then y=4 and y=5. y=6 and
    // y=7 are never sent -- `(7 - 3) / 2` is two bytes -- and the y=2 nibble
    // outside the box is carried along with its pair.
    CHECK_EQ(a.blockData(14, 2, 2), srcA.blockData(14, 2, 2));
    CHECK_EQ(a.blockData(14, 5, 2), srcA.blockData(14, 5, 2));
    CHECK_EQ(a.blockData(14, 6, 2), u8(0));
    CHECK_EQ(b.skyLight(1, 4, 4), srcB.skyLight(1, 4, 4));
}

TEST(a_region_over_a_chunk_the_client_does_not_have_still_steps_over_its_bytes)
{
    Columns source;
    source.map[{0, 0}] = patterned(0, 0);
    source.map[{0, 1}] = patterned(0, 1);

    Packet packet;
    CHECK(makeMapChunk(&Columns::findConst, &source, 4, 10, 12, 2, 8, 8, &packet));

    Columns target;
    target.map[{0, 1}] = std::make_unique<ChunkColumn>(0, 1);
    MapChunkRegion region;
    CHECK(inflateMapChunk(packet, &region));
    std::vector<u8> scratch;
    CHECK_EQ(applyMapChunk(region, &Columns::find, &target, &scratch), 1);
    const ChunkColumn& got = *target.map[std::make_pair(0, 1)];
    const ChunkColumn& want = *source.map[std::make_pair(0, 1)];
    CHECK_EQ(got.block(5, 17, 3), want.block(5, 17, 3));
    CHECK_EQ(got.blockLight(4, 11, 0), want.blockLight(4, 11, 0));
}

TEST(a_pending_edit_is_put_back_after_eighty_ticks_unless_the_server_speaks_first)
{
    struct Log {
        std::vector<std::tuple<i32, int, i32, u16>> reverted;
        static void revert(void* ctx, i32 x, int y, i32 z, u16 block, u8)
        {
            static_cast<Log*>(ctx)->reverted.emplace_back(x, y, z, block);
        }
    };

    PendingEdits edits;
    Log log;
    edits.record(10, 64, -5, 1, 0);   // broke stone
    edits.record(10, 64, -5, 0, 0);   // placed into the hole
    edits.record(40, 70, 40, 3, 0);   // somewhere else

    for (int t = 0; t < 79; ++t) {
        edits.tick(&Log::revert, &log);
    }
    CHECK(log.reverted.empty());

    edits.confirm(40, 70, 40, 40, 70, 40);
    CHECK_EQ(edits.count(), 2);

    edits.tick(&Log::revert, &log);
    CHECK_EQ(edits.count(), 0);
    CHECK_EQ(log.reverted.size(), usize(2));
    // In the order recorded, so the older value is written last and stands.
    CHECK(std::get<3>(log.reverted[0]) == 1);
    CHECK(std::get<3>(log.reverted[1]) == 0);
}

TEST(the_server_list_survives_the_card_and_an_address_means_what_it_says)
{
    char dir[] = "/tmp/3dalpha_servers_XXXXXX";
    CHECK(::mkdtemp(dir) != nullptr);
    const std::string path = std::string(dir) + "/servers.txt";

    io::PosixFileSystem fs;
    std::vector<ServerEntry> list;
    CHECK(!loadServerList(fs, path.c_str(), &list));
    CHECK(list.empty());

    list.push_back(ServerEntry{"Home = best", "192.168.1.20"});
    list.push_back(ServerEntry{"Friends", "mc.example.org", 25570});
    CHECK(saveServerList(fs, path.c_str(), list));

    std::vector<ServerEntry> back;
    CHECK(loadServerList(fs, path.c_str(), &back));
    CHECK_EQ(back.size(), usize(2));
    CHECK(back[0].name == "Home = best");
    CHECK(back[0].address == "192.168.1.20");
    // The port row a server never left is the default, and is written anyway.
    CHECK_EQ(back[0].port, u16(25565));
    CHECK(back[1].address == "mc.example.org");
    CHECK_EQ(back[1].port, u16(25570));

    // **A list written before the port had a row of its own** -- the host and
    // the port in one `address=` -- is split on the way in rather than
    // refused, and the split survives the next save.
    const std::string legacy = "name=Old\naddress=mc.example.net:25571\n";
    CHECK(fs.writeFileAtomic(path.c_str(),
                             ConstByteSpan(reinterpret_cast<const u8*>(legacy.data()),
                                           legacy.size())));
    std::vector<ServerEntry> old;
    CHECK(loadServerList(fs, path.c_str(), &old));
    CHECK_EQ(old.size(), usize(1));
    CHECK(old[0].address == "mc.example.net");
    CHECK_EQ(old[0].port, u16(25571));
    std::remove(path.c_str());
    ::rmdir(dir);

    std::string host;
    u16 port = 0;
    CHECK(parseAddress("  localhost ", &host, &port));
    CHECK(host == "localhost");
    CHECK_EQ(port, u16(25565));
    CHECK(parseAddress("10.0.0.2:1234", &host, &port));
    CHECK(host == "10.0.0.2");
    CHECK_EQ(port, u16(1234));
    CHECK(parseAddress("[::1]:25566", &host, &port));
    CHECK(host == "::1");
    CHECK_EQ(port, u16(25566));
    CHECK(parseAddress("fe80::1", &host, &port));
    CHECK(host == "fe80::1");
    CHECK_EQ(port, u16(25565));
    CHECK(!parseAddress("", &host, &port));
    CHECK(!parseAddress("host:", &host, &port));
    CHECK(!parseAddress("host:70000", &host, &port));
    CHECK(!parseAddress(":25565", &host, &port));

    // **An address with no port leaves the caller's own alone**, which is what
    // lets a Port row survive an Address row that says nothing about it.
    port = 25570;
    CHECK(parseAddress("example.org", &host, &port, port));
    CHECK_EQ(port, u16(25570));

    CHECK(parsePort("25570", &port));
    CHECK_EQ(port, u16(25570));
    CHECK(!parsePort("0", &port));
    CHECK(!parsePort("65536", &port));
    CHECK(!parsePort("", &port));
    // A refused port leaves the row on what it had.
    CHECK_EQ(port, u16(25570));
}

TEST(the_friend_list_name_is_the_login_name_as_far_as_the_game_allows)
{
    CHECK(usernameFrom("Grisu") == "Grisu");
    CHECK(usernameFrom("Big Steve") == "Big_Steve");
    CHECK(usernameFrom("a/b:c") == "a_b_c");
    CHECK(usernameFrom("") == kFallbackUsername);
    CHECK(usernameFrom("   ") == kFallbackUsername);
    // Characters the a1.1.2 font has no glyph for.
    CHECK(usernameFrom("\xE3\x81\x82\xE3\x81\x84") == kFallbackUsername);
    CHECK(usernameFrom("Ab\xE3\x81\x82") == "Ab_");
    CHECK(usernameFrom("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "ABCDEFGHIJKLMNOP");
    // A display name from AlphaComputer keeps its 24.
    CHECK(usernameFrom("ABCDEFGHIJKLMNOPQRSTUVWXYZ", kMaxDisplayNameChars)
          == "ABCDEFGHIJKLMNOPQRSTUVWX");
    CHECK(usernameFrom("Grisu the Builder", kMaxDisplayNameChars) == "Grisu_the_Builder");

    const u16 screenName[11] = {'M', 'i', 'i', 0xD83D, 0xDE00, 0, 'x'};
    CHECK(utf16ToUtf8(screenName, 11) == "Mii\xF0\x9F\x98\x80");
}
