// The guest's end of a local session, as a protocol-2 stream.
//
// What this pins is the thing that makes the whole scheme work and the thing
// that would be hardest to see going wrong on a console: **the link is a byte
// stream and the parser must not care where the pieces were cut**. A protocol-2
// packet has no length prefix, so a channel that lost its place once would stay
// lost forever, and the symptom on hardware would be a world that stops
// arriving rather than an error.

#include "framework.hpp"

#include "core/net/chunk_payload.hpp"
#include "core/net/local_channel.hpp"
#include "core/net/packets.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

// Waits for the next event, or gives up. The channel parses on a thread, so
// "nothing yet" and "nothing ever" are told apart by the clock.
bool waitForEvent(LocalChannel& channel, PacketChannel::Event* out, int millis = 2000)
{
    for (int i = 0; i < millis; ++i) {
        if (channel.poll(out)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::vector<u8> streamOf(const std::vector<Packet>& packets)
{
    std::vector<u8> bytes;
    for (const Packet& packet : packets) {
        encodePacket(packet, &bytes);
    }
    return bytes;
}

Packet loginPacket()
{
    Packet p;
    p.reset(packet::Login);
    p.pushInt(77);
    p.pushString("");
    p.pushString("");
    return p;
}

}  // namespace

TEST(a_channel_starts_waiting_for_a_login_and_plays_once_it_arrives)
{
    LocalChannel channel;
    CHECK(channel.start());
    CHECK(channel.state() == PacketChannel::State::LoggingIn);

    const std::vector<u8> bytes = streamOf({loginPacket()});
    channel.feed(bytes.data(), bytes.size());

    PacketChannel::Event event;
    CHECK(waitForEvent(channel, &event));
    CHECK(event.kind == PacketChannel::Event::Kind::LoggedIn);
    CHECK(event.entityId == 77);
    CHECK(channel.state() == PacketChannel::State::Playing);
    channel.stop();
}

TEST(a_stream_cut_anywhere_yields_the_same_packets)
{
    std::vector<Packet> sent{loginPacket()};
    for (int i = 0; i < 12; ++i) {
        Packet p;
        p.reset(packet::BlockChange);
        p.pushInt(i);
        p.pushInt(41);
        p.pushInt(i * 2);
        p.pushInt(20);
        p.pushInt(0);
        sent.push_back(p);
    }
    sent.push_back(makeChat("a line long enough to straddle several pieces of the stream"));
    const std::vector<u8> bytes = streamOf(sent);

    LocalChannel channel;
    CHECK(channel.start());

    // **One byte at a time**, which is the worst cut there is and the one a
    // packet boundary can never coincide with.
    for (usize i = 0; i < bytes.size(); ++i) {
        channel.feed(bytes.data() + i, 1);
    }

    PacketChannel::Event event;
    CHECK(waitForEvent(channel, &event));
    CHECK(event.kind == PacketChannel::Event::Kind::LoggedIn);

    for (int i = 0; i < 12; ++i) {
        CHECK(waitForEvent(channel, &event));
        CHECK(event.kind == PacketChannel::Event::Kind::Packet);
        CHECK(event.packet.id == packet::BlockChange);
        CHECK(event.packet.integer(0) == i);
        CHECK(event.packet.integer(2) == i * 2);
    }

    CHECK(waitForEvent(channel, &event));
    CHECK(event.packet.id == packet::Chat);
    CHECK(event.packet.text(0)
          == "a line long enough to straddle several pieces of the stream");
    CHECK(channel.packetsIn() == 14);
    channel.stop();
}

TEST(a_map_chunk_arrives_as_a_finished_column)
{
    auto source = std::make_unique<world::ChunkColumn>(2, -3);
    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            for (int y = 0; y < 30; ++y) {
                source->setBlock(lx, y, lz, block::BlockId(1));
            }
            source->setBlock(lx, 30, lz, block::BlockId(2));
            source->setBlockData(lx, 30, lz, u8((lx * lz) & 15));
        }
    }
    std::vector<u8> scratch;
    refreshHeightMap(*source, &scratch);

    struct Ctx {
        const world::ChunkColumn* column;
    } ctx{source.get()};
    auto lookup = [](void* c, i32 x, i32 z) -> const world::ChunkColumn* {
        const world::ChunkColumn* column = static_cast<Ctx*>(c)->column;
        return (column->x == x && column->z == z) ? column : nullptr;
    };

    Packet chunk;
    CHECK(makeMapChunk(lookup, &ctx, 2 * 16, 0, -3 * 16, 16, world::ChunkColumn::kHeight, 16,
                       &chunk));

    const std::vector<u8> bytes = streamOf({chunk});
    LocalChannel channel;
    CHECK(channel.start());
    channel.feed(bytes.data(), bytes.size());

    PacketChannel::Event event;
    CHECK(waitForEvent(channel, &event));
    CHECK(event.kind == PacketChannel::Event::Kind::Column);
    CHECK(event.column != nullptr);
    CHECK(event.column->x == 2);
    CHECK(event.column->z == -3);
    CHECK(event.column->block(5, 30, 7) == block::BlockId(2));
    CHECK(event.column->blockData(5, 30, 7) == u8((5 * 7) & 15));
    CHECK(event.column->block(5, 31, 7) == block::BlockId(0));
    channel.stop();
}

TEST(what_the_game_sends_comes_back_out_as_bytes_for_the_link)
{
    LocalChannel channel;
    CHECK(channel.start());

    channel.send(makeChat("hello"));
    channel.send(makeDig(3, 1, 2, 3, 4));

    std::vector<u8> out;
    channel.takeOutbound(&out);
    CHECK(!out.empty());
    CHECK(channel.bytesOut() == out.size());

    // ...and nothing is handed over twice.
    std::vector<u8> again;
    channel.takeOutbound(&again);
    CHECK(again.empty());

    usize offset = 0;
    Packet parsed;
    usize consumed = 0;
    CHECK(parsePacket(out.data(), out.size(), &parsed, &consumed) == ParseResult::Ok);
    CHECK(parsed.id == packet::Chat);
    CHECK(parsed.text(0) == "hello");
    offset += consumed;
    CHECK(parsePacket(out.data() + offset, out.size() - offset, &parsed, &consumed)
          == ParseResult::Ok);
    CHECK(parsed.id == packet::BlockDig);
    CHECK(parsed.integer(0) == 3);
    channel.stop();
}

TEST(a_kick_closes_the_channel_with_the_hosts_reason)
{
    LocalChannel channel;
    CHECK(channel.start());
    const std::vector<u8> bytes = streamOf({makeDisconnect("the host closed the world")});
    channel.feed(bytes.data(), bytes.size());

    PacketChannel::Event event;
    CHECK(waitForEvent(channel, &event));
    CHECK(event.kind == PacketChannel::Event::Kind::Closed);
    CHECK(event.detail == "the host closed the world");
    CHECK(channel.state() == PacketChannel::State::Closed);
    channel.stop();
}

TEST(a_link_that_ends_says_so_once)
{
    LocalChannel channel;
    CHECK(channel.start());
    channel.closeWith("Session ended", "The host closed the world.");
    channel.closeWith("Session ended", "and again");

    PacketChannel::Event event;
    CHECK(waitForEvent(channel, &event));
    CHECK(event.kind == PacketChannel::Event::Kind::Closed);
    CHECK(event.title == "Session ended");
    CHECK(!channel.poll(&event));
}

TEST(a_packet_id_that_is_not_in_the_table_ends_the_stream)
{
    LocalChannel channel;
    CHECK(channel.start());
    const u8 nonsense[] = {0x7F, 0x00, 0x00};
    channel.feed(nonsense, sizeof(nonsense));

    PacketChannel::Event event;
    CHECK(waitForEvent(channel, &event));
    CHECK(event.kind == PacketChannel::Event::Kind::Closed);
    CHECK(channel.state() == PacketChannel::State::Closed);
    channel.stop();
}
