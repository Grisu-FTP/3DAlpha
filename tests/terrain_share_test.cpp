// Terrain generated on one console for another's world.
//
// The claim this file exists to pin is the one the whole scheme rests on: a
// column generated somewhere else, packed, sent through a link that loses
// frames, unpacked and installed is **byte-identical** to one this console
// would have generated itself. Everything else here is what happens when it
// does not arrive, arrives late, or comes from a console that should never
// have been asked.

#include "framework.hpp"

#include "core/net/session.hpp"
#include "core/net/terrain_share.hpp"
#include "version_slots.hpp"

#include <deque>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net::link;

namespace {

constexpr i64 kSeed = 1234567890;

GeneratorId testWorld()
{
    GeneratorId id;
    id.seed = kSeed;
    id.options = 0;
    id.version = generatorVersion();
    return id;
}

// Two ends of one link, with an optional loss rate. Same shape as the one in
// link_test.cpp; kept separate so neither test's wire can quietly change the
// other's.
class Wire {
public:
    class End : public Datagrams {
    public:
        End(Wire* wire, u16 self) : wire_(wire), self_(self) {}

        bool send(u16 node, const u8* data, usize size) override
        {
            (void)node;
            return wire_->carry(self_, data, size);
        }

        bool receive(u8* buffer, usize capacity, usize* size, u16* node) override
        {
            return wire_->take(self_, buffer, capacity, size, node);
        }

    private:
        Wire* wire_;
        u16 self_;
    };

    Wire() : host(this, kHostNode), guest(this, 2) {}

    bool carry(u16 from, const u8* data, usize size)
    {
        ++carried;
        if (dropEvery > 0 && carried % dropEvery == 0) {
            return true;
        }
        (from == kHostNode ? toGuest_ : toHost_)
            .push_back(Frame{from, std::vector<u8>(data, data + size)});
        return true;
    }

    bool take(u16 self, u8* buffer, usize capacity, usize* size, u16* node)
    {
        std::deque<Frame>& queue = self == kHostNode ? toHost_ : toGuest_;
        if (queue.empty()) {
            return false;
        }
        const Frame frame = queue.front();
        queue.pop_front();
        if (frame.bytes.size() > capacity) {
            return false;
        }
        for (usize i = 0; i < frame.bytes.size(); ++i) {
            buffer[i] = frame.bytes[i];
        }
        *size = frame.bytes.size();
        *node = frame.node;
        return true;
    }

    End host;
    End guest;
    int dropEvery = 0;
    int carried = 0;

private:
    struct Frame {
        u16 node = 0;
        std::vector<u8> bytes;
    };

    std::deque<Frame> toHost_;
    std::deque<Frame> toGuest_;
};

// The host's listener: terrain parts go to the pool.
class HostSide : public SessionListener {
public:
    explicit HostSide(TerrainPool* pool) : pool_(pool) {}

    void onTerrainPart(u8 playerId, const u8* body, usize size) override
    {
        (void)playerId;
        pool_->onPart(body, size);
    }

private:
    TerrainPool* pool_;
};

// The guest's: requests go to the responder.
class GuestSide : public SessionListener {
public:
    explicit GuestSide(TerrainResponder* responder) : responder_(responder) {}

    void onTerrainRequest(const u8* body, usize size) override
    {
        responder_->onRequest(body, size);
    }

private:
    TerrainResponder* responder_;
};

// One frame of both consoles: the host puts its requests on the wire, the
// guest answers one column, and both pump their links.
void step(Wire& wire, HostSession& host, GuestSession& guest, TerrainPool& pool,
          TerrainResponder& responder, u32* nowMs, int frames)
{
    std::vector<std::vector<u8>> parts;
    for (int i = 0; i < frames; ++i) {
        *nowMs += 16;

        i32 x = 0;
        i32 z = 0;
        while (pool.nextRequest(&x, &z)) {
            std::vector<u8> body;
            encodeTerrainRequest(x, z, &body);
            if (!host.requestTerrain(body.data(), body.size())) {
                break;
            }
        }

        if (responder.generateOne(&parts)) {
            for (const std::vector<u8>& part : parts) {
                guest.sendTerrainPart(part.data(), part.size());
            }
        }

        host.pump(*nowMs, wire.host);
        guest.pump(*nowMs, wire.guest);
    }
}

// What this console would have generated for the same column.
std::vector<u8> locally(i32 x, i32 z)
{
    mcver::WorldGen provider(kSeed, mcver::WorldGenOptions{});
    std::vector<u8> blocks(kTerrainBytes);
    provider.generateColumn(x, z, blocks.data());
    return blocks;
}

}  // namespace

TEST(a_delegated_column_is_byte_identical_to_one_generated_here)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    TerrainPool pool;
    TerrainResponder responder;
    HostSide hostSide(&pool);
    GuestSide guestSide(&responder);
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostSide);
    guest.join("Ada", testWorld(), now, &guestSide);
    step(wire, host, guest, pool, responder, &now, 4);
    CHECK(guest.state() == GuestSession::State::Playing);

    // The guest was told what world to generate for.
    CHECK(guest.world().seed == kSeed);
    responder.open(guest.world());

    CHECK(pool.want(12, -7));
    step(wire, host, guest, pool, responder, &now, 40);

    std::vector<u8> delivered(kTerrainBytes, 0xEE);
    CHECK(TerrainPool::supply(&pool, 12, -7, delivered.data()));
    CHECK(delivered == locally(12, -7));
    CHECK(pool.used() == 1);
}

TEST(a_delegated_column_survives_a_link_that_loses_frames)
{
    Wire wire;
    wire.dropEvery = 3;
    HostSession host;
    GuestSession guest;
    TerrainPool pool;
    TerrainResponder responder;
    HostSide hostSide(&pool);
    GuestSide guestSide(&responder);
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostSide);
    guest.join("Ada", testWorld(), now, &guestSide);
    step(wire, host, guest, pool, responder, &now, 200);
    CHECK(guest.state() == GuestSession::State::Playing);
    responder.open(guest.world());

    CHECK(pool.want(-3, 5));
    step(wire, host, guest, pool, responder, &now, 400);

    std::vector<u8> delivered(kTerrainBytes, 0xEE);
    CHECK(TerrainPool::supply(&pool, -3, 5, delivered.data()));
    CHECK(delivered == locally(-3, 5));
}

TEST(the_pool_answers_no_for_a_column_nobody_has_sent)
{
    TerrainPool pool;
    std::vector<u8> blocks(kTerrainBytes, 0x11);
    // The generator asks, nothing is there, and it generates the column itself.
    CHECK(!TerrainPool::supply(&pool, 0, 0, blocks.data()));
    CHECK(blocks[0] == 0x11);  // untouched, so the caller's buffer is safe to use
    CHECK(pool.used() == 0);
}

TEST(a_column_that_arrives_after_the_host_made_it_is_dropped)
{
    // The answer to a column the host has already generated itself. Driven
    // directly rather than through a session, because "the generator got there
    // first" is a race and a test should not be one.
    TerrainResponder responder;
    responder.open(testWorld());

    std::vector<u8> ask;
    encodeTerrainRequest(1, 1, &ask);
    CHECK(responder.onRequest(ask.data(), ask.size()));

    std::vector<std::vector<u8>> parts;
    CHECK(responder.generateOne(&parts));
    CHECK(!parts.empty());

    TerrainPool pool;
    for (const std::vector<u8>& part : parts) {
        pool.onPart(part.data(), part.size());
    }

    CHECK(pool.late() == 1);
    std::vector<u8> blocks(kTerrainBytes, 0x22);
    CHECK(!TerrainPool::supply(&pool, 1, 1, blocks.data()));
    CHECK(blocks[0] == 0x22);
}

TEST(the_pool_stops_asking_once_it_is_full)
{
    TerrainPool pool(2);
    CHECK(pool.want(0, 0));
    CHECK(pool.want(1, 0));
    CHECK(!pool.want(2, 0));  // back-pressure, not a queue that grows
    CHECK(pool.waiting() == 2);

    // Asking for one already asked for is not a second slot.
    CHECK(pool.want(0, 0));
    CHECK(pool.waiting() == 2);
}

TEST(a_console_on_another_build_is_never_asked_for_terrain)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    TerrainPool pool;
    TerrainResponder responder;
    HostSide hostSide(&pool);
    GuestSide guestSide(&responder);
    u32 now = 1000;

    GeneratorId theirs = testWorld();
    theirs.version = "some-other-build";

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostSide);
    guest.join("Ada", theirs, now, &guestSide);
    step(wire, host, guest, pool, responder, &now, 4);

    // They are in the session and can play.
    CHECK(guest.state() == GuestSession::State::Playing);
    CHECK(host.guestCount() == 1);
    // But the host will not hand them work.
    CHECK(!host.anyGenerator());
    std::vector<u8> body;
    encodeTerrainRequest(0, 0, &body);
    CHECK(!host.requestTerrain(body.data(), body.size()));
}

TEST(a_responder_with_no_world_refuses_every_request)
{
    TerrainResponder responder;
    std::vector<u8> body;
    encodeTerrainRequest(4, 4, &body);
    CHECK(!responder.onRequest(body.data(), body.size()));

    std::vector<std::vector<u8>> parts;
    CHECK(!responder.generateOne(&parts));
    CHECK(responder.answered() == 0);
}
