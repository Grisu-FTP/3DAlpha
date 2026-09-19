// The host's half of a playable session: what a guest is told, and what the
// host does with what a guest says.
//
// The claim that matters most here is that a column served over the link and
// rebuilt on the other side is the **same column** -- `serializeRegion`,
// deflate, inflate, `buildColumn` -- because everything a guest sees is
// downstream of that. The rest is the protocol's own arithmetic: that a block
// change reaches whoever holds the column and nobody else, that a guest that
// walks away is stopped being sent ground, and that back-pressure keeps bytes
// rather than dropping them.

#include "framework.hpp"

#include "core/entity/item_entity.hpp"
#include "core/entity/mob.hpp"
#include "core/net/chunk_payload.hpp"
#include "core/net/entities.hpp"
#include "core/net/player_sync.hpp"
#include "core/tick/tick_world.hpp"
#include "core/net/packets.hpp"
#include "core/net/world_server.hpp"
#include "items.hpp"  // generated; see tools/configure.py

#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <thread>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

// A world of a handful of hand-made columns, so a test can say exactly what is
// loaded and what is not.
class FakeWorld : public ServerWorld {
public:
    void add(i32 chunkX, i32 chunkZ)
    {
        auto column = std::make_unique<world::ChunkColumn>(chunkX, chunkZ);
        // Something with structure in it: flat stone with a stripe of dirt, so
        // a column that came back wrong is visible rather than plausible.
        for (int lx = 0; lx < 16; ++lx) {
            for (int lz = 0; lz < 16; ++lz) {
                for (int y = 0; y < 40; ++y) {
                    column->setBlock(lx, y, lz, block::BlockId(1));
                }
                column->setBlock(lx, 40, lz, block::BlockId(3));
                column->setBlock(lx, 41, lz, block::BlockId(2));
                column->setBlockData(lx, 41, lz, u8((lx + lz) & 15));
                column->setBlockLight(lx, 42, lz, u8(lx & 15));
                column->setSkyLight(lx, 42, lz, 15);
            }
        }
        std::vector<u8> scratch;
        refreshHeightMap(*column, &scratch);
        columns_[key(chunkX, chunkZ)] = std::move(column);
    }

    void drop(i32 chunkX, i32 chunkZ) { columns_.erase(key(chunkX, chunkZ)); }

    world::ChunkColumn* mutableColumn(i32 chunkX, i32 chunkZ)
    {
        auto it = columns_.find(key(chunkX, chunkZ));
        return it == columns_.end() ? nullptr : it->second.get();
    }

    const world::ChunkColumn* column(i32 chunkX, i32 chunkZ) const override
    {
        auto it = columns_.find(key(chunkX, chunkZ));
        return it == columns_.end() ? nullptr : it->second.get();
    }

    i64 timeTicks() const override { return 4000; }

    void spawnPoint(i32* x, i32* y, i32* z) const override
    {
        *x = 8;
        *y = 42;
        *z = 8;
    }

    void breakBlock(i32 x, int y, i32 z, int heldItem) override
    {
        broke.push_back({x, y, z, heldItem});
        if (world::ChunkColumn* c = mutableColumn(x >> 4, z >> 4)) {
            c->setBlock(int(x & 15), y, int(z & 15), block::BlockId(0));
        }
    }

    void placeBlock(const PlaceRequest& request) override { placed.push_back(request); }

    void showChat(const std::string& line) override { chat.push_back(line); }

    entity::ItemEntitySystem* items() override { return drops; }

    entity::MobSystem* mobs() override { return herd; }

    entity::ArrowSystem* arrows() override { return quiver; }
    void useItem(const UseRequest& request) override { used.push_back(request); }
    entity::ArrowSystem* quiver = nullptr;
    std::vector<UseRequest> used;

    void attackEntity(const AttackRequest& request) override { hits.push_back(request); }

    void attackHost(const AttackRequest& request) override { hostHits.push_back(request); }

    // `kh.q()`'s lift, as a number a test can state: the feet come out at
    // `groundY` whenever it is set above them.
    double settleFeet(double x, double feetY, double z) const override
    {
        (void)x;
        (void)z;
        return groundY > feetY ? groundY : feetY;
    }

    double groundY = 0.0;

    entity::MobSystem* herd = nullptr;
    std::vector<AttackRequest> hits;
    std::vector<AttackRequest> hostHits;

    void spawnItem(double x, double y, double z, item::ItemId id, int count, double mx,
                   double my, double mz) override
    {
        thrown.push_back(PlaceRequest{});
        thrownItem = int(id);
        thrownCount = count;
        thrownX = x;
        thrownMotionY = my;
        (void)y;
        (void)z;
        (void)mx;
        (void)mz;
    }

    entity::ItemEntitySystem* drops = nullptr;
    std::vector<PlaceRequest> thrown;
    int thrownItem = 0;
    int thrownCount = 0;
    double thrownX = 0.0;
    double thrownMotionY = 0.0;

    struct Pos {
        i32 x;
        int y;
        i32 z;
        int held = 0;
    };
    std::vector<Pos> broke;
    std::vector<PlaceRequest> placed;
    std::vector<std::string> chat;

private:
    static i64 key(i32 x, i32 z) { return i64((u64(u32(x)) << 32) | u64(u32(z))); }
    std::map<i64, std::unique_ptr<world::ChunkColumn>> columns_;
};

// Everything the server sent, per player, as one stream each -- which is what
// the link delivers and what the guest's parser reads.
class Sink {
public:
    static bool send(void* ctx, u8 playerId, const u8* data, usize size)
    {
        auto* self = static_cast<Sink*>(ctx);
        if (self->refuse) {
            return false;
        }
        std::vector<u8>& stream = self->streams[playerId];
        stream.insert(stream.end(), data, data + size);
        return true;
    }

    // The packets in one player's stream, parsed the way the guest parses them.
    std::vector<Packet> packets(u8 playerId)
    {
        std::vector<Packet> out;
        const std::vector<u8>& stream = streams[playerId];
        usize offset = 0;
        for (;;) {
            Packet packet;
            usize consumed = 0;
            if (parsePacket(stream.data() + offset, stream.size() - offset, &packet, &consumed)
                != ParseResult::Ok) {
                break;
            }
            offset += consumed;
            out.push_back(std::move(packet));
        }
        return out;
    }

    int count(u8 playerId, u8 id)
    {
        int n = 0;
        for (const Packet& packet : packets(playerId)) {
            if (packet.id == id) {
                ++n;
            }
        }
        return n;
    }

    // The last packet of a kind, or a KeepAlive when there is none.
    Packet last(u8 playerId, u8 id)
    {
        Packet found;
        found.reset(packet::KeepAlive);
        for (Packet& packet : packets(playerId)) {
            if (packet.id == id) {
                found = std::move(packet);
            }
        }
        return found;
    }

    std::map<u8, std::vector<u8>> streams;
    bool refuse = false;
};

// One guest's packets on their way to the host, as the link would carry them.
void feedPacket(WorldServer& server, u8 playerId, const Packet& packet)
{
    std::vector<u8> bytes;
    encodePacket(packet, &bytes);
    server.feed(playerId, bytes.data(), bytes.size());
}

// A position report the way `la.J()` makes one: x, feet, eye, z.
void movePlayer(WorldServer& server, u8 playerId, double x, double z)
{
    feedPacket(server, playerId, makePositionLook(x, 42.0, 43.62, z, 0.0f, 0.0f, true));
}

// Frames, with time passing between them. **The millisecond is load-bearing**:
// a column is deflated on a thread of its own, so a loop that pumps as fast as
// it can may never let that thread run and the columns would never come back.
void settle(WorldServer& server, int pumps = 220)
{
    for (int i = 0; i < pumps; ++i) {
        server.pump(i64(i) * 16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

constexpr u8 kGuest = 2;

}  // namespace

TEST(a_joining_guest_is_logged_in_and_placed)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    CHECK(sink.count(kGuest, packet::Login) == 1);
    CHECK(sink.count(kGuest, packet::SpawnPosition) == 1);

    const Packet spawn = sink.last(kGuest, packet::SpawnPosition);
    CHECK(spawn.integer(0) == 8);
    CHECK(spawn.integer(1) == 42);
    CHECK(spawn.integer(2) == 8);

    // The world's time, so the guest's sky is the host's sky.
    CHECK(sink.last(kGuest, packet::TimeUpdate).integer(0) == 4000);

    // And a place to stand, once the column under it has gone.
    const Packet placed = sink.last(kGuest, packet::PlayerPositionLook);
    CHECK(placed.id == packet::PlayerPositionLook);
    // Server to client, the second field is the eye and the third the feet.
    CHECK(placed.real(1) > placed.real(2));
    server.close();
}

TEST(a_served_column_rebuilds_byte_identically)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);

    // Find the Map Chunk for 0,0 and rebuild it the way the guest does.
    std::unique_ptr<world::ChunkColumn> rebuilt;
    for (const Packet& packet : sink.packets(kGuest)) {
        if (packet.id != packet::MapChunk) {
            continue;
        }
        MapChunkRegion region;
        CHECK(inflateMapChunk(packet, &region));
        if (!region.wholeColumn() || region.x != 0 || region.z != 0) {
            continue;
        }
        rebuilt = buildColumn(region);
        break;
    }
    CHECK(rebuilt != nullptr);

    const world::ChunkColumn& original = *world.mutableColumn(0, 0);
    bool same = true;
    for (int lx = 0; lx < 16 && same; ++lx) {
        for (int lz = 0; lz < 16 && same; ++lz) {
            for (int y = 0; y < world::ChunkColumn::kHeight; ++y) {
                if (rebuilt->block(lx, y, lz) != original.block(lx, y, lz)
                    || rebuilt->blockData(lx, y, lz) != original.blockData(lx, y, lz)
                    || rebuilt->blockLight(lx, y, lz) != original.blockLight(lx, y, lz)
                    || rebuilt->skyLight(lx, y, lz) != original.skyLight(lx, y, lz)) {
                    same = false;
                    break;
                }
            }
        }
    }
    CHECK(same);
    server.close();
}

TEST(a_changed_block_reaches_whoever_holds_the_column)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);
    CHECK(sink.count(kGuest, packet::MapChunk) >= 1);

    world.mutableColumn(0, 0)->setBlock(3, 41, 5, block::BlockId(20));
    world.mutableColumn(0, 0)->setBlockData(3, 41, 5, 0);
    server.blockChanged(3, 41, 5);
    server.pump(9000);

    const Packet change = sink.last(kGuest, packet::BlockChange);
    CHECK(change.id == packet::BlockChange);
    CHECK(change.integer(0) == 3);
    CHECK(change.integer(1) == 41);
    CHECK(change.integer(2) == 5);
    CHECK(change.integer(3) == 20);
    server.close();
}

TEST(a_change_in_a_column_nobody_holds_is_not_sent)
{
    FakeWorld world;
    world.add(0, 0);
    world.add(40, 40);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);

    const int before = sink.count(kGuest, packet::BlockChange);
    world.mutableColumn(40, 40)->setBlock(1, 41, 1, block::BlockId(20));
    server.blockChanged(40 * 16 + 1, 41, 40 * 16 + 1);
    server.pump(9000);
    CHECK(sink.count(kGuest, packet::BlockChange) == before);
    server.close();
}

TEST(several_changes_in_one_column_become_one_multi_block_change)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);

    for (int i = 0; i < 5; ++i) {
        world.mutableColumn(0, 0)->setBlock(i, 41, 2, block::BlockId(20));
        server.blockChanged(i, 41, 2);
    }
    // The same block twice is still one entry: this is the fluid's path.
    server.blockChanged(0, 41, 2);
    server.pump(9000);

    const Packet change = sink.last(kGuest, packet::MultiBlockChange);
    CHECK(change.id == packet::MultiBlockChange);
    CHECK(change.changeCoords.size() == 5);
    CHECK(change.changeIds.size() == 5);
    CHECK(change.changeIds[0] == 20);
    server.close();
}

TEST(a_dig_that_says_the_block_gave_breaks_it_here)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    // Status 1 is "still digging" and must do nothing: the host does not count
    // digs for anybody.
    feedPacket(server, kGuest, makeDig(1, 4, 41, 6, 1));
    CHECK(world.broke.empty());

    feedPacket(server, kGuest, makeDig(3, 4, 41, 6, 1));
    CHECK(world.broke.size() == 1);
    CHECK(world.broke[0].x == 4);
    CHECK(world.broke[0].y == 41);
    CHECK(world.broke[0].z == 6);
    server.close();
}

TEST(a_dig_carries_what_the_guest_was_holding)
{
    // `in.c(III)Z` ends `if (removed && player.canHarvestBlock(block))`, so the
    // break the host runs has to know the digger's tool or it can only ever
    // drop what a bare hand drops -- which is what a guest's mining did.
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    feedPacket(server, kGuest, makeHoldingChange(257));  // an iron pickaxe
    feedPacket(server, kGuest, makeDig(3, 4, 41, 6, 1));
    CHECK(world.broke.size() == 1);
    CHECK(world.broke[0].held == 257);

    // And it follows the hand rather than being latched at the first dig.
    feedPacket(server, kGuest, makeHoldingChange(0));
    feedPacket(server, kGuest, makeDig(3, 5, 41, 6, 1));
    CHECK(world.broke.size() == 2);
    CHECK(world.broke[1].held == 0);
    server.close();
}

TEST(a_place_carries_the_face_and_where_the_guest_is_standing)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    movePlayer(server, kGuest, 6.5, 7.5);
    feedPacket(server, kGuest, makePlace(4, 4, 41, 6, 1));

    CHECK(world.placed.size() == 1);
    CHECK(world.placed[0].item == 4);
    CHECK(world.placed[0].face == 1);
    CHECK(world.placed[0].x == 4);
    CHECK(world.placed[0].playerX > 6.4 && world.placed[0].playerX < 6.6);
    CHECK(world.placed[0].playerFeetY > 41.9 && world.placed[0].playerFeetY < 42.1);

    // **An empty hand is -1 on the wire and still reaches the block.**
    // `activeBlockOrUseItem` asks `Block.blockActivated` before it looks at
    // the stack, which is how a bare hand opens a chest, flips a lever and
    // lights redstone ore. It arrives as item 0, which is what
    // `item::rightClick` calls "nothing held".
    feedPacket(server, kGuest, makePlace(-1, 4, 41, 6, 1));
    CHECK(world.placed.size() == 2);
    CHECK(world.placed[1].item == 0);
    server.close();
}

TEST(a_guest_line_reaches_the_host_and_the_other_guests)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    server.addPlayer(3, "other");
    settle(server, 40);

    feedPacket(server, kGuest, makeChat("hello"));
    server.pump(9000);

    CHECK(world.chat.size() == 1);
    CHECK(world.chat[0] == "<guest> hello");
    CHECK(sink.last(3, packet::Chat).text(0) == "<guest> hello");
    // ...and not back to whoever said it: a1.1.2's own client already printed
    // it, and an echo would double every line.
    CHECK(sink.count(kGuest, packet::Chat) == 0);
    server.close();
}

TEST(the_guests_are_told_where_everybody_is)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    server.addPlayer(3, "other");
    settle(server, 40);

    // Both guests were told about the host and about each other.
    CHECK(sink.count(kGuest, packet::NamedEntitySpawn) >= 2);

    server.setHostPose(100.0, 64.0, 200.0, 90.0f, 0.0f);
    server.pump(20000);

    const Packet moved = sink.last(kGuest, packet::EntityTeleport);
    CHECK(moved.id == packet::EntityTeleport);
    CHECK(moved.integer(1) == 100 * 32);
    CHECK(moved.integer(3) == 200 * 32);
    server.close();
}

TEST(a_guest_that_leaves_is_destroyed_for_the_others)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    server.addPlayer(3, "other");
    settle(server, 40);
    CHECK(server.playerCount() == 2);

    server.removePlayer(kGuest);
    server.pump(20000);
    CHECK(server.playerCount() == 1);
    CHECK(sink.count(3, packet::DestroyEntity) == 1);
    server.close();
}

TEST(a_guest_outside_what_the_host_has_loaded_is_starved_rather_than_served)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);
    CHECK(!server.starved(kGuest));

    // Far past the one column this world has: they are standing on nothing,
    // and nothing more can be sent to them.
    movePlayer(server, kGuest, 4000.0, 4000.0);
    const int before = sink.count(kGuest, packet::MapChunk);
    settle(server, 40);

    CHECK(server.starved(kGuest));
    CHECK(sink.count(kGuest, packet::MapChunk) == before);

    // ...and walking back is enough to be served again.
    movePlayer(server, kGuest, 8.5, 8.5);
    settle(server, 40);
    CHECK(!server.starved(kGuest));
    server.close();
}

// **Where the world has to be loaded, which is not only where the camera is.**
//
// This list is the whole of what a host needs in order to stop being limited to
// its own render distance: `WorldStreamer::setServedAreas` takes it and holds
// the ground under every guest outside the grid. The two things that can be
// wrong about it are both checked here -- that the host itself is not in it
// (its ground is the grid's own), and that a guest who has not echoed their
// Position & Look is not in it either, because there is nowhere to load around
// a player who has not said where they are.
TEST(the_host_is_told_where_to_load_for_every_guest_that_has_landed)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.setHostPose(8.5, 42.0, 8.5, 0.0f, 0.0f);

    WorldServer::ViewCentre centres[WorldServer::kMaxPlayers];
    // Nobody has joined, and the host is never in the list.
    CHECK_EQ(server.viewCentres(centres, WorldServer::kMaxPlayers), 0);

    server.addPlayer(kGuest, "guest");
    settle(server);
    CHECK_EQ(server.viewCentres(centres, WorldServer::kMaxPlayers), 1);

    // A guest thirty chunks out is thirty chunks out: the list follows them
    // wherever they go, which is the point of it.
    movePlayer(server, kGuest, 30.0 * 16.0 + 8.5, -12.0 * 16.0 + 8.5);
    settle(server, 4);
    CHECK_EQ(server.viewCentres(centres, WorldServer::kMaxPlayers), 1);
    CHECK_EQ((long long) centres[0].chunkX, 30LL);
    CHECK_EQ((long long) centres[0].chunkZ, -12LL);

    // The cap is honoured rather than overrun.
    CHECK_EQ(server.viewCentres(centres, 0), 0);

    server.removePlayer(kGuest);
    CHECK_EQ(server.viewCentres(centres, WorldServer::kMaxPlayers), 0);
    server.close();
}

TEST(a_link_that_will_not_take_bytes_keeps_them_rather_than_dropping_them)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    sink.refuse = true;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    CHECK(sink.streams[kGuest].empty());
    CHECK(server.outboundBytes() > 0);

    sink.refuse = false;
    settle(server, 40);
    CHECK(!sink.streams[kGuest].empty());
    CHECK(sink.count(kGuest, packet::Login) == 1);
    server.close();
}

TEST(a_world_with_nobody_in_it_does_nothing_with_a_block_change)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    for (int i = 0; i < 100; ++i) {
        server.blockChanged(i & 15, 41, 0);
    }
    server.pump(1000);
    CHECK(sink.streams.empty());
    server.close();
}

TEST(a_block_changed_while_its_column_is_in_flight_still_reaches_the_guest)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");

    // Two frames with no sleep between them: the first logs the guest in and
    // the second hands the column to the oven. The edit below therefore lands
    // in the window this test exists for -- after the photograph was taken and
    // before the guest holds anything.
    server.pump(0);
    server.pump(16);

    world.mutableColumn(0, 0)->setBlock(9, 41, 11, block::BlockId(20));
    world.mutableColumn(0, 0)->setBlockData(9, 41, 11, 0);
    server.blockChanged(9, 41, 11);
    settle(server);

    // **Read it the way the guest reads it**: the last whole column, then
    // every block change after it. Which road the edit took does not matter;
    // what matters is that the two consoles end up agreeing.
    std::unique_ptr<world::ChunkColumn> view;
    const std::vector<Packet> stream = sink.packets(kGuest);
    for (const Packet& packet : stream) {
        if (packet.id == packet::MapChunk) {
            MapChunkRegion region;
            CHECK(inflateMapChunk(packet, &region));
            if (region.wholeColumn() && region.x == 0 && region.z == 0) {
                view = buildColumn(region);
            }
        } else if (packet.id == packet::BlockChange && view != nullptr) {
            const i32 x = i32(packet.integer(0));
            const i32 z = i32(packet.integer(2));
            if ((x >> 4) == 0 && (z >> 4) == 0) {
                view->setBlock(int(x & 15), int(packet.integer(1)), int(z & 15),
                               block::BlockId(packet.integer(3)));
            }
        } else if (packet.id == packet::MultiBlockChange && view != nullptr
                   && packet.integer(0) == 0 && packet.integer(1) == 0) {
            for (usize i = 0; i < packet.changeCoords.size(); ++i) {
                const u16 coded = u16(packet.changeCoords[i]);
                view->setBlock((coded >> 12) & 15, coded & 255, (coded >> 8) & 15,
                               block::BlockId(packet.changeIds[i]));
            }
        }
    }

    CHECK(view != nullptr);
    CHECK(view->block(9, 41, 11) == block::BlockId(20));
    server.close();
}

// ---- items on the ground ---------------------------------------------------

namespace {

// A tick world over one column, which is what an item needs in order to fall.
struct ItemWorldFixture {
    FakeWorld world;
    entity::ItemEntitySystem drops{1};
    std::unique_ptr<tick::TickWorld> ticks;

    ItemWorldFixture()
    {
        for (i32 cx = -1; cx <= 1; ++cx) {
            for (i32 cz = -1; cz <= 1; ++cz) {
                world.add(cx, cz);
            }
        }
        tick::TickAccess access;
        access.ctx = &world;
        access.column = [](void* ctx, i32 cx, i32 cz) -> world::ChunkColumn* {
            return static_cast<FakeWorld*>(ctx)->mutableColumn(cx, cz);
        };
        ticks = std::make_unique<tick::TickWorld>(access, 12345LL);
        world.drops = &drops;
    }
};

}  // namespace

TEST(an_item_on_the_ground_is_announced_and_then_destroyed)
{
    ItemWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    CHECK(fixture.drops.spawn(*fixture.ticks, 8.5, 45.0, 8.5, item::ItemId(4), 3, 0));
    server.pump(9000);

    const Packet spawned = sink.last(kGuest, packet::PickupSpawn);
    CHECK(spawned.id == packet::PickupSpawn);
    CHECK(spawned.integer(0) != 0);  // the host stamped an id on it
    CHECK(spawned.integer(1) == 4);
    CHECK(spawned.integer(2) == 3);
    CHECK(server.itemsSpawned() == 1);

    // ...and the same item is not announced twice.
    server.pump(9100);
    CHECK(server.itemsSpawned() == 1);

    fixture.drops.clear();
    server.pump(9200);
    CHECK(sink.count(kGuest, packet::DestroyEntity) == 1);
    server.close();
}

TEST(a_guest_standing_on_an_item_picks_it_up)
{
    ItemWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);

    // Right where the guest is, and old enough to be taken.
    CHECK(fixture.drops.spawn(*fixture.ticks, 8.5, 42.5, 8.5, item::ItemId(5), 7, 0));
    fixture.drops.at(0)->pickupDelay = 0;
    movePlayer(server, kGuest, 8.5, 8.5);
    server.pump(9000);
    server.pump(9100);

    CHECK(fixture.drops.count() == 0);
    CHECK(sink.count(kGuest, packet::Collect) == 1);
    const Packet added = sink.last(kGuest, packet::AddToInventory);
    CHECK(added.id == packet::AddToInventory);
    CHECK(added.integer(0) == 5);
    CHECK(added.integer(1) == 7);
    server.close();
}

TEST(an_item_a_guest_threw_is_spawned_here)
{
    ItemWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    feedPacket(server, kGuest,
               makePickupSpawn(0, 6, 2, 100 * 32, 45 * 32, 200 * 32, 0, 25, 0));
    CHECK(fixture.world.thrown.size() == 1);
    CHECK(fixture.world.thrownItem == 6);
    CHECK(fixture.world.thrownCount == 2);
    CHECK(fixture.world.thrownX > 99.9 && fixture.world.thrownX < 100.1);
    // 25/128 of a block per tick upward, the way `ha(dx)` packs it.
    CHECK(fixture.world.thrownMotionY > 0.19 && fixture.world.thrownMotionY < 0.20);

    // An empty stack is not an item.
    feedPacket(server, kGuest, makePickupSpawn(0, -1, 1, 0, 0, 0, 0, 0, 0));
    CHECK(fixture.world.thrown.size() == 1);
    server.close();
}

TEST(a_joiner_the_host_has_never_seen_starts_at_the_spawn_point)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);

    const Packet placed = sink.last(kGuest, packet::PlayerPositionLook);
    CHECK(placed.id == packet::PlayerPositionLook);
    CHECK(placed.real(0) > 8.0 && placed.real(0) < 9.0);
    CHECK(placed.real(3) > 8.0 && placed.real(3) < 9.0);
    // **`spawnY + 1`, which is where `dm`'s constructor stands a fresh player,
    // and not `spawnY` -- which is a block coordinate and therefore the block
    // the player would be standing *inside*.** Field 2 is the feet.
    CHECK(placed.real(2) > 42.9 && placed.real(2) < 43.1);
    server.close();
}

TEST(a_joiner_is_stood_on_top_of_whatever_has_grown_over_the_spawn_point)
{
    // A spawn point from a world made before anything searched for one: it
    // names y = 42 and there are eight more blocks of hillside on top of it.
    // `kh.q()` walks a fresh player up out of that, and this is the server
    // doing it for somebody else -- at the one moment it can, which is once
    // the column under them has been sent.
    FakeWorld world;
    world.add(0, 0);
    world.groundY = 50.0;
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server);

    const Packet placed = sink.last(kGuest, packet::PlayerPositionLook);
    CHECK(placed.id == packet::PlayerPositionLook);
    CHECK(placed.real(2) > 49.9 && placed.real(2) < 50.1);
    // The eye follows the feet, because the server refuses a stance outside
    // 0.1..1.65 and would otherwise have sent one two blocks tall.
    CHECK(placed.real(1) - placed.real(2) > 1.61);
    CHECK(placed.real(1) - placed.real(2) < 1.63);
    server.close();
}

TEST(a_player_is_named_on_the_wire_by_the_id_the_session_gave_them)
{
    // **The whole of how two consoles agree on who is what colour.** Nothing
    // says so on the wire: the host numbers its players with their session ids
    // and starts everything else above them, so both ends reach the same
    // answer from the id they already have. See `net::playerColour`.
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "guest");
    settle(server, 40);

    CHECK(sink.last(kGuest, packet::Login).integer(0) == i64(kGuest));

    bool sawHost = false;
    for (const Packet& packet : sink.packets(kGuest)) {
        if (packet.id == packet::NamedEntitySpawn && packet.text(0) == "host") {
            sawHost = true;
            CHECK(packet.integer(0) == i64(kFirstPlayerEntityId));
        }
    }
    CHECK(sawHost);
    server.close();
}

// ---- where a joiner lands --------------------------------------------------

TEST(a_player_the_host_has_seen_before_comes_back_where_they_left)
{
    FakeWorld world;
    for (i32 cx = -1; cx <= 1; ++cx) {
        for (i32 cz = -1; cz <= 1; ++cz) {
            world.add(cx, cz);
        }
    }
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server);

    // They walk away from where they were put, and leave.
    movePlayer(server, kGuest, 20.25, -12.5);
    server.pump(9000);
    server.removePlayer(kGuest);

    // ...and come back.
    sink.streams.clear();
    server.addPlayer(kGuest, "Ada");
    settle(server);

    const Packet placed = sink.last(kGuest, packet::PlayerPositionLook);
    CHECK(placed.id == packet::PlayerPositionLook);
    CHECK(placed.real(0) > 20.2 && placed.real(0) < 20.3);
    CHECK(placed.real(3) > -12.6 && placed.real(3) < -12.4);

    // Somebody the host has never seen starts where the world does.
    sink.streams.clear();
    server.addPlayer(3, "Bea");
    settle(server);
    const Packet fresh = sink.last(3, packet::PlayerPositionLook);
    CHECK(fresh.id == packet::PlayerPositionLook);
    CHECK(fresh.real(0) > 8.0 && fresh.real(0) < 9.0);
    server.close();
}

TEST(a_guest_waiting_on_ground_that_never_comes_is_put_down_beside_the_host)
{
    FakeWorld world;
    // The spawn point's column does not exist and never will, so the placement
    // there can never go out. Only the host's own ground is loaded.
    world.add(40, 40);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.setHostPose(40 * 16 + 8.5, 42.0, 40 * 16 + 8.5, 0.0f, 0.0f);
    server.addPlayer(kGuest, "Ada");

    // Well inside the patience window: still waiting, and not displaced.
    for (int i = 0; i < 60; ++i) {
        server.pump(i64(i) * 16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(sink.count(kGuest, packet::PlayerPositionLook) == 0);
    CHECK(!server.displaced(kGuest));

    // Past it: put down where the host is, and said so.
    for (int i = 0; i < 200; ++i) {
        server.pump(20000 + i64(i) * 16);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(server.displaced(kGuest));
    const Packet placed = sink.last(kGuest, packet::PlayerPositionLook);
    CHECK(placed.id == packet::PlayerPositionLook);
    CHECK(placed.real(0) > 40 * 16.0);

    // **And a place they were put is not a place they chose.** Remembering it
    // would pin them there on the next join even once the real ground existed.
    server.removePlayer(kGuest);
    sink.streams.clear();
    server.addPlayer(kGuest, "Ada");
    server.pump(40000);
    CHECK(!server.displaced(kGuest));
    server.close();
}

// ---- the pack --------------------------------------------------------------

TEST(what_a_guest_was_carrying_is_handed_back_when_they_return)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    // a1.1.2's client pushes all three arrays every twentieth tick and the
    // server adopts them. See core/net/player_sync.hpp.
    WireStack pack[kWireMainSlots];
    pack[0].id = 4;
    pack[0].count = 32;
    pack[0].damage = 0;
    WireStack worn[kWireArmourSlots];
    feedPacket(server, kGuest, makeInventory(kInventoryMain, pack, kWireMainSlots));
    feedPacket(server, kGuest, makeInventory(kInventoryArmour, worn, kWireArmourSlots));

    server.removePlayer(kGuest);
    sink.streams.clear();
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    bool handedBack = false;
    for (const Packet& packet : sink.packets(kGuest)) {
        if (packet.id == packet::PlayerInventory && packet.integer(0) == kInventoryMain) {
            CHECK(packet.stacks.size() == usize(kWireMainSlots));
            CHECK(packet.stacks[0].id == 4);
            CHECK(packet.stacks[0].count == 32);
            handedBack = true;
        }
    }
    CHECK(handedBack);
    server.close();
}

// ---- what is alive ---------------------------------------------------------

namespace {

struct MobWorldFixture {
    FakeWorld world;
    entity::MobSystem herd{7};
    entity::ItemEntitySystem drops{1};
    std::unique_ptr<tick::TickWorld> ticks;

    MobWorldFixture()
    {
        for (i32 cx = -1; cx <= 1; ++cx) {
            for (i32 cz = -1; cz <= 1; ++cz) {
                world.add(cx, cz);
            }
        }
        tick::TickAccess access;
        access.ctx = &world;
        access.column = [](void* ctx, i32 cx, i32 cz) -> world::ChunkColumn* {
            return static_cast<FakeWorld*>(ctx)->mutableColumn(cx, cz);
        };
        ticks = std::make_unique<tick::TickWorld>(access, 12345LL);
        world.herd = &herd;
        world.drops = &drops;
    }
};

}  // namespace

TEST(a_sheep_reaches_the_guest_as_a_sheep)
{
    // **The one place the wire is ours rather than the jar's.** a1.1.2
    // registers sheep, cow and chicken all as 91 and the last wins, so a real
    // client draws a chicken for each. Between two 3DAlpha consoles that is a
    // hole, not a behaviour. See `RemoteEntities::mobTypeFor`.
    entity::MobType out = entity::MobType::Pig;
    CHECK(RemoteEntities::mobTypeFor(RemoteEntities::wireTypeFor(entity::MobType::Sheep), &out));
    CHECK(out == entity::MobType::Sheep);
    CHECK(RemoteEntities::mobTypeFor(RemoteEntities::wireTypeFor(entity::MobType::Cow), &out));
    CHECK(out == entity::MobType::Cow);

    // ...and the ids the jar does define still mean what the jar says.
    CHECK(RemoteEntities::wireTypeFor(entity::MobType::Creeper) == 50);
    CHECK(RemoteEntities::wireTypeFor(entity::MobType::Pig) == 90);
    CHECK(RemoteEntities::mobTypeFor(91, &out));
    CHECK(out == entity::MobType::Chicken);
}

// **A slime's size is not in `ez` and cannot be guessed.** `ma`'s constructor
// draws `1 << nextInt(3)` and `gy` never overwrites it, so against a real
// a1.1.2 server the two ends disagree about how big a slime is, how much it
// hurts to stand on and how far it hops. 55 keeps meaning exactly that; the
// three ids of ours say which of the sizes it actually is.
TEST(a_slime_reaches_the_guest_the_size_it_really_is)
{
    entity::MobType out = entity::MobType::Pig;
    int size = -1;

    // The jar's id says nothing: 0 is "you pick", which is what `ma` does.
    CHECK(RemoteEntities::mobTypeFor(55, &out, &size));
    CHECK(out == entity::MobType::Slime);
    CHECK_EQ(size, 0);

    entity::MobSystem herd{3};
    entity::Mob mob;
    mob.type = entity::MobType::Slime;
    for (int wanted : {1, 2, entity::kSlimeMaxSize}) {
        herd.setSlimeSize(mob, wanted);
        size = -1;
        CHECK(RemoteEntities::mobTypeFor(RemoteEntities::wireTypeForMob(mob), &out, &size));
        CHECK(out == entity::MobType::Slime);
        CHECK_EQ(size, wanted);
    }

    // Everything that is not a slime goes through unchanged.
    entity::Mob sheep;
    sheep.type = entity::MobType::Sheep;
    CHECK_EQ(RemoteEntities::wireTypeForMob(sheep),
             RemoteEntities::wireTypeFor(entity::MobType::Sheep));
}

TEST(an_animal_is_announced_moved_and_destroyed)
{
    MobWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    CHECK(fixture.herd.spawn(*fixture.ticks, entity::MobType::Sheep, 8.5, 42.0, 8.5, 0.0f));
    server.pump(9000);

    const Packet spawned = sink.last(kGuest, packet::MobSpawn);
    CHECK(spawned.id == packet::MobSpawn);
    CHECK(spawned.integer(0) != 0);
    CHECK(spawned.integer(1) == RemoteEntities::wireTypeFor(entity::MobType::Sheep));
    CHECK(server.mobsSpawned() == 1);

    // The same animal is not announced twice.
    server.pump(9100);
    CHECK(server.mobsSpawned() == 1);

    // It walks.
    fixture.herd.at(0).body.setFeet(12.5, 42.0, 14.5);
    server.pump(9200);
    const Packet moved = sink.last(kGuest, packet::EntityTeleport);
    CHECK(moved.id == packet::EntityTeleport);
    CHECK(moved.integer(1) == 12 * 32 + 16);

    const int destroyedBefore = sink.count(kGuest, packet::DestroyEntity);
    fixture.herd.clear();
    server.pump(9300);
    CHECK(sink.count(kGuest, packet::DestroyEntity) == destroyedBefore + 1);
    server.close();
}

// **A hit and a death here are told to the guests**, which protocol 2 has no
// packet for: `packet::EntityStatus`, 2 then 3, as they happen.
TEST(an_animal_hit_or_killed_here_is_reported_to_the_guests)
{
    MobWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    CHECK(fixture.herd.spawn(*fixture.ticks, entity::MobType::Pig, 8.5, 42.0, 8.5, 0.0f));
    server.pump(9000);
    const i32 mobId = fixture.herd.at(0).entityId;
    CHECK_EQ(sink.count(kGuest, packet::EntityStatus), 0);

    CHECK(fixture.herd.attack(*fixture.ticks, 0, 1, true));
    server.pump(9010);  // not on the move clock: a hit goes straight away
    CHECK_EQ(sink.count(kGuest, packet::EntityStatus), 1);
    const Packet hurt = sink.last(kGuest, packet::EntityStatus);
    CHECK(hurt.integer(0) == i64(mobId));
    CHECK(hurt.integer(1) == entity::kStatusHurt);

    // Said once per hit, however many pumps it stays red for.
    server.pump(9100);
    server.pump(9200);
    CHECK_EQ(sink.count(kGuest, packet::EntityStatus), 1);

    // One blow that hurts and kills: 2, then 3.
    fixture.herd.at(0).hurtResistant = 0;
    CHECK(fixture.herd.attack(*fixture.ticks, 0, 100, true));
    server.pump(9300);
    CHECK_EQ(sink.count(kGuest, packet::EntityStatus), 3);
    const Packet dead = sink.last(kGuest, packet::EntityStatus);
    CHECK(dead.integer(1) == entity::kStatusDead);
    server.pump(9400);
    CHECK_EQ(sink.count(kGuest, packet::EntityStatus), 3);
    server.close();
}

// The host has no stream of its own, so its swing has to be put on the wire by
// the server it is running -- `la.w()`'s Arm Animation, to every guest.
TEST(the_hosts_swing_is_shown_to_its_guests)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    const int before = sink.count(kGuest, packet::ArmAnimation);
    server.hostSwing();
    server.pump(9000);
    CHECK_EQ(sink.count(kGuest, packet::ArmAnimation), before + 1);
    const Packet swing = sink.last(kGuest, packet::ArmAnimation);
    CHECK(swing.integer(0) != 0);
    CHECK(swing.integer(0) != i64(kGuest));  // the host, not the guest
    server.close();
}

// **A guest's crouch, death and respawn reach everybody else**, host included.
// Only the guest's own console knows any of them -- the crouch is its input and
// the health is its own -- so it says so, and the host passes it on.
TEST(a_guests_crouch_death_and_respawn_are_passed_to_the_others)
{
    constexpr u8 kOther = 3;
    struct Seen {
        std::vector<Packet> packets;
        static void take(void* ctx, const Packet& packet)
        {
            static_cast<Seen*>(ctx)->packets.push_back(packet);
        }
        int count(u8 id, i64 entity) const
        {
            int n = 0;
            for (const Packet& packet : packets) {
                n += packet.id == id && packet.integer(0) == entity ? 1 : 0;
            }
            return n;
        }
    };
    Seen seen;

    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.setLocalSink(&Seen::take, &seen);
    server.addPlayer(kGuest, "Ada");
    server.addPlayer(kOther, "Bea");
    settle(server, 60);

    feedPacket(server, kGuest, makeEntityAction(kGuest, kActionCrouch));
    feedPacket(server, kGuest, makeEntityAction(kGuest, kActionCrouch));  // not news
    server.pump(20000);
    CHECK_EQ(sink.count(kOther, packet::EntityAction), 1);
    const Packet crouch = sink.last(kOther, packet::EntityAction);
    CHECK(crouch.integer(0) == i64(kGuest));
    CHECK(crouch.integer(1) == i64(kActionCrouch));
    CHECK_EQ(sink.count(kGuest, packet::EntityAction), 0);  // not back to themselves
    CHECK_EQ(seen.count(packet::EntityAction, kGuest), 1);  // and the host draws it

    // A death, which a guest may say only about itself.
    feedPacket(server, kGuest, makeEntityStatus(kOther, entity::kStatusDead));
    feedPacket(server, kGuest, makeEntityStatus(kGuest, entity::kStatusHurt));
    server.pump(20100);
    CHECK_EQ(sink.count(kOther, packet::EntityStatus), 0);
    feedPacket(server, kGuest, makeEntityStatus(kGuest, entity::kStatusDead));
    feedPacket(server, kGuest, makeEntityStatus(kGuest, entity::kStatusDead));
    server.pump(20200);
    CHECK_EQ(sink.count(kOther, packet::EntityStatus), 1);
    CHECK(sink.last(kOther, packet::EntityStatus).integer(1) == i64(entity::kStatusDead));
    CHECK_EQ(seen.count(packet::EntityStatus, kGuest), 1);

    // Back at the spawn point: the position first, as `NetPlay::tick` sends
    // it, and then the Respawn, answered with a fresh spawn standing there.
    const int spawnsBefore = sink.count(kOther, packet::NamedEntitySpawn);
    movePlayer(server, kGuest, 40.5, 8.5);
    feedPacket(server, kGuest, makeRespawn());
    feedPacket(server, kGuest, makeRespawn());  // alive already
    server.pump(20300);
    CHECK_EQ(sink.count(kOther, packet::NamedEntitySpawn), spawnsBefore + 1);
    const Packet again = sink.last(kOther, packet::NamedEntitySpawn);
    CHECK(again.integer(0) == i64(kGuest));
    CHECK(again.integer(1) == i64(std::floor(40.5 * 32.0)));
    CHECK(seen.count(packet::NamedEntitySpawn, kGuest) >= 2);
    server.close();
}

// The host has no stream, so its stance goes out through the server it runs --
// and a console that joins later is told how everybody already stands.
TEST(the_hosts_crouch_and_death_reach_its_guests_and_a_later_joiner)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    server.setHostStance(false, true);  // standing is what everyone assumes
    server.setHostStance(true, true);
    server.setHostStance(true, true);
    server.pump(9000);
    CHECK_EQ(sink.count(kGuest, packet::EntityAction), 1);
    const Packet crouch = sink.last(kGuest, packet::EntityAction);
    CHECK(crouch.integer(0) == i64(kFirstPlayerEntityId));
    CHECK(crouch.integer(1) == i64(kActionCrouch));

    // A guest joining now sees the host crouched.
    constexpr u8 kLate = 3;
    server.addPlayer(kLate, "Bea");
    settle(server, 40);
    CHECK_EQ(sink.count(kLate, packet::EntityAction), 1);
    CHECK(sink.last(kLate, packet::EntityAction).integer(0) == i64(kFirstPlayerEntityId));

    server.setHostStance(true, false);
    server.setHostStance(false, false);
    server.pump(20000);
    CHECK_EQ(sink.count(kGuest, packet::EntityStatus), 1);
    CHECK(sink.last(kGuest, packet::EntityStatus).integer(0) == i64(kFirstPlayerEntityId));
    CHECK_EQ(sink.count(kLate, packet::EntityStatus), 1);

    // ...and one joining while the host lies dead is told that instead.
    constexpr u8 kLater = 4;
    server.addPlayer(kLater, "Cy");
    settle(server, 40);
    CHECK_EQ(sink.count(kLater, packet::EntityStatus), 1);
    CHECK_EQ(sink.count(kLater, packet::EntityAction), 0);

    const int spawns = sink.count(kGuest, packet::NamedEntitySpawn);
    server.setHostStance(false, true);
    server.pump(30000);
    CHECK_EQ(sink.count(kGuest, packet::NamedEntitySpawn), spawns + 1);
    CHECK(sink.last(kGuest, packet::NamedEntitySpawn).integer(0) == i64(kFirstPlayerEntityId));
    server.close();
}

// **A guest whose link has fallen behind is not sent positions it cannot use.**
// Every teleport used to be queued, in order, however far behind the link was,
// so a slow link grew a queue of places animals used to be -- the guest saw
// them frozen and its digs were confirmed after the client had put the block
// back. Now the queue stops growing with them, and once it drains the guest is
// given where everything is *now*.
TEST(a_guest_that_falls_behind_gets_current_positions_not_a_queue_of_old_ones)
{
    MobWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    for (int n = 0; n < 10; ++n) {
        CHECK(fixture.herd.spawn(*fixture.ticks, entity::MobType::Pig, 4.5 + n, 42.0, 8.5,
                                 0.0f));
    }
    server.pump(9000);
    CHECK(server.mobsSpawned() == 10);

    // The link takes nothing, and the herd walks for ten seconds.
    sink.refuse = true;
    i64 now = 9000;
    for (int step = 1; step <= 200; ++step) {
        now += 50;
        for (int n = 0; n < fixture.herd.count(); ++n) {
            fixture.herd.at(n).body.setFeet(4.5 + n, 42.0, 8.5 + step * 0.05);
        }
        server.pump(now);
    }
    // Two hundred moves of ten animals would be some 46 KB of teleports.
    CHECK(server.outboundBytes() < 16u * 1024u);

    // The link comes back. What arrives is where they are now.
    sink.refuse = false;
    for (int i = 0; i < 20; ++i) {
        now += 50;
        server.pump(now);
    }
    const i64 finalZ = i64(std::floor((8.5 + 200 * 0.05) * 32.0));
    for (int n = 0; n < fixture.herd.count(); ++n) {
        const i32 id = fixture.herd.at(n).entityId;
        bool current = false;
        for (const Packet& packet : sink.packets(kGuest)) {
            if (packet.id == packet::EntityTeleport && packet.integer(0) == i64(id)) {
                current = packet.integer(3) == finalZ;  // the last one wins
            }
        }
        CHECK(current);
    }
    server.close();
}

TEST(a_guests_punch_reaches_the_animal_here)
{
    MobWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    CHECK(fixture.herd.spawn(*fixture.ticks, entity::MobType::Pig, 8.5, 42.0, 8.5, 0.0f));
    server.pump(9000);
    const i32 mobId = fixture.herd.at(0).entityId;
    CHECK(mobId != 0);

    movePlayer(server, kGuest, 9.5, 8.5);
    feedPacket(server, kGuest, makeHoldingChange(268));  // a wooden sword
    feedPacket(server, kGuest, makeUseEntity(1, mobId, true));

    CHECK(fixture.world.hits.size() == 1);
    CHECK(fixture.world.hits[0].entityId == mobId);
    CHECK(fixture.world.hits[0].item == 268);
    CHECK(fixture.world.hits[0].playerX > 9.4 && fixture.world.hits[0].playerX < 9.6);

    // A right click is not a hit, and nothing sends one yet.
    feedPacket(server, kGuest, makeUseEntity(1, mobId, false));
    CHECK(fixture.world.hits.size() == 1);
    server.close();
}

TEST(an_animal_that_was_already_alive_is_announced_to_a_console_that_joins_later)
{
    // `knownMobs_` records what the *session* has been told, not what each
    // player has. An animal is announced once, when it appears -- so every
    // animal already in the world was invisible to a guest for the rest of its
    // life, which is most of them: a guest joins a world that has been
    // running.
    MobWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");

    CHECK(fixture.herd.spawn(*fixture.ticks, entity::MobType::Pig, 8.5, 42.0, 8.5, 0.0f));
    server.pump(1000);  // announced to nobody: nobody is here

    server.addPlayer(kGuest, "Ada");
    settle(server, 60);

    const Packet spawned = sink.last(kGuest, packet::MobSpawn);
    CHECK(spawned.id == packet::MobSpawn);
    CHECK(spawned.integer(0) == i64(fixture.herd.at(0).entityId));
    server.close();
}

// ---- players hitting each other --------------------------------------------

TEST(the_host_is_told_about_the_guests_it_is_describing_to_everybody_else)
{
    // A host is not a client of its own server, so every packet about a guest
    // used to go out over the radio and none of it came back -- which is why
    // the host could see the world it was serving and nobody in it. See
    // `WorldServer::setLocalSink`.
    struct Seen {
        std::vector<Packet> packets;
        static void take(void* ctx, const Packet& packet)
        {
            static_cast<Seen*>(ctx)->packets.push_back(packet);
        }
    };
    Seen seen;

    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.setLocalSink(&Seen::take, &seen);
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    bool spawned = false;
    for (const Packet& packet : seen.packets) {
        if (packet.id == packet::NamedEntitySpawn && packet.integer(0) == i64(kGuest)) {
            spawned = true;
            CHECK(packet.text(0) == "Ada");
        }
    }
    CHECK(spawned);

    // ...and where they walked to.
    const usize before = seen.packets.size();
    movePlayer(server, kGuest, 40.5, 8.5);
    server.pump(20000);
    bool moved = false;
    for (usize i = before; i < seen.packets.size(); ++i) {
        if (seen.packets[i].id == packet::EntityTeleport
            && seen.packets[i].integer(0) == i64(kGuest)) {
            moved = true;
        }
    }
    CHECK(moved);

    // ...and that they left.
    server.removePlayer(kGuest);
    bool destroyed = false;
    for (const Packet& packet : seen.packets) {
        if (packet.id == packet::DestroyEntity && packet.integer(0) == i64(kGuest)) {
            destroyed = true;
        }
    }
    CHECK(destroyed);
    server.close();
}

TEST(a_guests_punch_at_the_host_is_handed_to_the_host_not_to_the_world)
{
    // The one entity in the session this server does not own. There is no
    // health on this wire, so the blow is passed to whoever holds the host's
    // vitals rather than resolved here.
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.setHostPose(4.5, 43.0, 8.5, 0.0f, 0.0f);
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    movePlayer(server, kGuest, 6.5, 8.5);
    feedPacket(server, kGuest, makeHoldingChange(268));  // a wooden sword
    feedPacket(server, kGuest, makeUseEntity(kGuest, kFirstPlayerEntityId, true));

    CHECK(world.hostHits.size() == 1);
    CHECK(world.hostHits[0].item == 268);
    CHECK(world.hostHits[0].playerX > 6.4 && world.hostHits[0].playerX < 6.6);
    // Not the animal path: a player is never resolved by the world.
    CHECK(world.hits.empty());
    server.close();
}

TEST(a_punch_between_two_guests_is_forwarded_to_the_one_who_was_hit)
{
    constexpr u8 kOther = 3;
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    server.addPlayer(kOther, "Bea");
    settle(server, 60);

    feedPacket(server, kGuest, makeUseEntity(kGuest, kOther, true));
    server.pump(20000);  // the outbox reaches the link in the next pump

    const Packet blow = sink.last(kOther, packet::UseEntity);
    CHECK(blow.id == packet::UseEntity);
    CHECK(blow.integer(0) == i64(kGuest));   // who swung
    CHECK(blow.integer(1) == i64(kOther));   // and at whom
    CHECK(blow.integer(2) == 1);
    // The attacker is not told about their own blow, and neither is the world.
    CHECK(sink.count(kGuest, packet::UseEntity) == 0);
    CHECK(world.hits.empty());
    CHECK(world.hostHits.empty());
    server.close();
}

TEST(the_host_can_swing_at_a_guest_and_the_guest_hears_about_it)
{
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    CHECK(server.hostAttack(kGuest, 268));
    server.pump(20000);

    // The held item first, for the reason every hit reports one: the blow
    // lands as hard as whatever the other end thinks is in the hand.
    const Packet held = sink.last(kGuest, packet::BlockItemSwitch);
    CHECK(held.integer(0) == i64(kFirstPlayerEntityId));
    CHECK(held.integer(1) == 268);

    const Packet blow = sink.last(kGuest, packet::UseEntity);
    CHECK(blow.id == packet::UseEntity);
    CHECK(blow.integer(0) == i64(kFirstPlayerEntityId));
    CHECK(blow.integer(1) == i64(kGuest));

    // Nobody who is not here.
    CHECK(!server.hostAttack(99, 268));
    server.close();
}

// ---- arrows -----------------------------------------------------------------

namespace {

// A floor at y = 42 over nine columns, and a quiver on it.
struct ArrowWorldFixture {
    FakeWorld world;
    entity::ArrowSystem quiver{77};
    std::unique_ptr<tick::TickWorld> ticks;

    ArrowWorldFixture()
    {
        for (i32 cx = -1; cx <= 1; ++cx) {
            for (i32 cz = -1; cz <= 1; ++cz) {
                world.add(cx, cz);
            }
        }
        tick::TickAccess access;
        access.ctx = &world;
        access.column = [](void* ctx, i32 cx, i32 cz) -> world::ChunkColumn* {
            return static_cast<FakeWorld*>(ctx)->mutableColumn(cx, cz);
        };
        ticks = std::make_unique<tick::TickWorld>(access, 12345LL);
        world.quiver = &quiver;
    }

    // Down into the floor from three blocks up, and ticked until it is stuck
    // and still.
    void landAt(double x, double z, i32 shooter)
    {
        quiver.shoot(*ticks, x, 45.0, z, 0.0f, 90.0f, shooter);
        for (int t = 0; t < 40; ++t) {
            quiver.tick(*ticks);
        }
    }
};

}  // namespace

TEST(a_guests_bow_shot_reaches_the_host_as_theirs)
{
    // a1.1.2 never sends a bow shot anywhere; a 3DAlpha guest says it as a
    // Place at (-1, 255, -1) facing 255, and the host fires from where they
    // stand, the way they face, in their name.
    FakeWorld world;
    world.add(0, 0);
    Sink sink;
    WorldServer server;
    server.open(&world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 60);

    feedPacket(server, kGuest, makePositionLook(6.5, 42.0, 43.62, 8.5, 90.0f, -10.0f, true));
    feedPacket(server, kGuest, makeUseItem(261));
    CHECK_EQ(int(world.used.size()), 1);
    CHECK_EQ(world.used[0].item, 261);
    CHECK_EQ(world.used[0].shooterEntityId, i32(kGuest));
    CHECK(std::abs(world.used[0].eyeY - 43.62) < 1e-6);
    CHECK(std::abs(world.used[0].eyeX - 6.5) < 1e-6);
    CHECK(std::abs(world.used[0].yawDegrees - 90.0f) < 1e-4f);
    CHECK(std::abs(world.used[0].pitchDegrees + 10.0f) < 1e-4f);
    // It is not a placement.
    CHECK(world.placed.empty());

    // ...and a real click on a face still is.
    feedPacket(server, kGuest, makePlace(4, 3, 41, 5, 1));
    CHECK_EQ(int(world.placed.size()), 1);
    CHECK_EQ(int(world.used.size()), 1);
    server.close();
}

TEST(an_arrow_is_announced_to_the_guests_moved_and_destroyed)
{
    ArrowWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server, 40);

    CHECK(fixture.quiver.shoot(*fixture.ticks, 8.5, 50.0, 8.5, 0.0f, 0.0f));
    server.pump(9000);

    // A Vehicle Spawn under the arrow's type, then its heading.
    const Packet spawned = sink.last(kGuest, packet::VehicleSpawn);
    CHECK(spawned.id == packet::VehicleSpawn);
    CHECK_EQ(int(spawned.integer(1)), kObjectArrow);
    const i32 id = i32(spawned.integer(0));
    CHECK(id != 0);
    CHECK_EQ(fixture.quiver[0].entityId, id);
    const int teleportsAtSpawn = sink.count(kGuest, packet::EntityTeleport);
    CHECK(teleportsAtSpawn >= 1);

    // In flight, it is placed again every tick it moves.
    fixture.quiver.tick(*fixture.ticks);
    server.pump(9100);
    CHECK(sink.count(kGuest, packet::EntityTeleport) > teleportsAtSpawn);
    const Packet moved = sink.last(kGuest, packet::EntityTeleport);
    CHECK_EQ(i32(moved.integer(0)), id);
    CHECK(moved.integer(3) > i64(8.5 * 32.0));  // it went along +z

    // Announced once, not every frame.
    server.pump(9200);
    CHECK_EQ(sink.count(kGuest, packet::VehicleSpawn), 1);

    fixture.quiver.clear();
    server.pump(9300);
    const Packet gone = sink.last(kGuest, packet::DestroyEntity);
    CHECK(gone.id == packet::DestroyEntity);
    CHECK_EQ(i32(gone.integer(0)), id);
    server.close();
}

TEST(a_guest_takes_back_their_own_stuck_arrow_and_nobody_elses)
{
    ArrowWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server);

    // Theirs and the host's, both at their feet.
    fixture.landAt(8.5, 8.5, i32(kGuest));
    fixture.landAt(8.8, 8.5, entity::kLocalShooter);
    CHECK_EQ(fixture.quiver.count(), 2);
    CHECK(fixture.quiver[0].inGround && fixture.quiver[1].inGround);

    movePlayer(server, kGuest, 8.5, 8.5);
    server.pump(9000);
    server.pump(9100);

    CHECK_EQ(fixture.quiver.count(), 1);
    CHECK_EQ(fixture.quiver[0].shooterPlayer, entity::kLocalShooter);
    CHECK_EQ(sink.count(kGuest, packet::Collect), 1);
    const Packet added = sink.last(kGuest, packet::AddToInventory);
    CHECK(added.id == packet::AddToInventory);
    CHECK_EQ(int(added.integer(0)), int(mcver::Item::Arrow));
    CHECK_EQ(int(added.integer(1)), 1);
    server.close();
}

TEST(an_arrow_that_strikes_a_guest_is_named_to_them)
{
    ArrowWorldFixture fixture;
    Sink sink;
    WorldServer server;
    server.open(&fixture.world, &Sink::send, &sink, "host");
    server.addPlayer(kGuest, "Ada");
    settle(server);
    movePlayer(server, kGuest, 8.5, 8.5);
    server.pump(9000);

    // The guest is a target where they stand.
    entity::RemoteTarget targets[WorldServer::kMaxPlayers];
    const int n = server.arrowTargets(targets, WorldServer::kMaxPlayers);
    CHECK_EQ(n, 1);
    CHECK_EQ(targets[0].entityId, i32(kGuest));
    CHECK(targets[0].box.minY > 41.9 && targets[0].box.minY < 42.1);

    // Loosed at point blank, so it strikes before any sync has named it.
    CHECK(fixture.quiver.shoot(*fixture.ticks, 8.5, 43.0, 6.0, 0.0f, 0.0f));
    entity::Arrow& arrow = *fixture.quiver.at(0);
    CHECK_EQ(arrow.entityId, 0);
    server.arrowStruck(i32(kGuest), arrow);
    server.pump(9100);

    CHECK(arrow.entityId != 0);
    const Packet spawned = sink.last(kGuest, packet::VehicleSpawn);
    CHECK_EQ(i32(spawned.integer(0)), arrow.entityId);
    const Packet blow = sink.last(kGuest, packet::UseEntity);
    CHECK(blow.id == packet::UseEntity);
    CHECK_EQ(i32(blow.integer(0)), arrow.entityId);  // "that arrow hit you"
    CHECK_EQ(i32(blow.integer(1)), i32(kGuest));
    server.close();
}
