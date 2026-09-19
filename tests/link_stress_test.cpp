// The link under a session's real load, for minutes rather than a handful of
// frames: the host streaming game packets while the guest uploads terrain, a
// wire that loses, delays and reorders, and ends that read at most sixteen
// datagrams a frame. Asserted is what a player feels: the host's stream to the
// guest never stops, and never falls behind -- a queue that only grows is a
// guest watching the world further and further in the past. See
// `link::kFlushBurst`.

#include "framework.hpp"

#include "core/net/link.hpp"

#include <deque>
#include <vector>

using namespace mc;
using namespace mc::net::link;

namespace {

// A tiny deterministic generator, so a failure reproduces.
struct Lcg {
    u32 state;
    u32 next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    bool chance(u32 percent) { return next() % 100 < percent; }
};

struct InFlight {
    u32 arriveMs;
    std::vector<u8> bytes;
};

// One direction of a lossy, delaying wire, with a receive buffer that drops
// what does not fit -- a socket's, or a relay's queue.
class Lane : public Datagrams {
public:
    Lane(Lcg* rand, u32 lossPercent, u32 maxDelayMs, usize bufferFrames)
        : rand_(rand), loss_(lossPercent), delay_(maxDelayMs), buffer_(bufferFrames)
    {
    }

    u32 nowMs = 0;

    bool send(u16 node, const u8* data, usize size) override
    {
        (void)node;
        if (rand_->chance(loss_)) {
            return true;
        }
        const u32 arrive = nowMs + (delay_ > 0 ? rand_->next() % delay_ : 0);
        flight_.push_back(InFlight{arrive, std::vector<u8>(data, data + size)});
        return true;
    }

    // Moves whatever has arrived by now into the receive buffer, dropping what
    // the buffer cannot hold.
    void land()
    {
        for (usize i = 0; i < flight_.size();) {
            if (i32(nowMs - flight_[i].arriveMs) >= 0) {
                if (landed_.size() < buffer_) {
                    landed_.push_back(std::move(flight_[i].bytes));
                }
                flight_[i] = std::move(flight_.back());
                flight_.pop_back();
                continue;
            }
            ++i;
        }
    }

    bool receive(u8* buffer, usize capacity, usize* size, u16* node) override
    {
        if (landed_.empty()) {
            return false;
        }
        std::vector<u8> bytes = std::move(landed_.front());
        landed_.pop_front();
        if (bytes.size() > capacity) {
            return false;
        }
        for (usize i = 0; i < bytes.size(); ++i) {
            buffer[i] = bytes[i];
        }
        *size = bytes.size();
        *node = 1;
        return true;
    }

private:
    Lcg* rand_;
    u32 loss_;
    u32 delay_;
    usize buffer_;
    std::vector<InFlight> flight_;
    std::deque<std::vector<u8>> landed_;
};

struct Received {
    u32 gamePackets = 0;
    u32 lastSerial = 0;
    bool outOfOrder = false;
    static void sink(void* ctx, Msg kind, const u8* body, usize size)
    {
        auto* self = static_cast<Received*>(ctx);
        if (kind != Msg::GamePacket || size < 4) {
            return;
        }
        const u32 serial = u32(body[0]) << 24 | u32(body[1]) << 16 | u32(body[2]) << 8 | body[3];
        if (serial != self->lastSerial + 1) {
            self->outOfOrder = true;
        }
        self->lastSerial = serial;
        ++self->gamePackets;
    }
};

}  // namespace

namespace {

struct StressResult {
    u32 delivered = 0;
    u32 backlog = 0;
    u32 longestStallMs = 0;
    bool outOfOrder = false;
};

StressResult stress(u32 lossPercent, u32 maxDelayMs, usize bufferFrames)
{
    Lcg rand{12345};
    Lane toGuest(&rand, lossPercent, maxDelayMs, bufferFrames);
    Lane toHost(&rand, lossPercent, maxDelayMs, bufferFrames);
    Peer host;
    Peer guest;
    host.reset(0);
    guest.reset(0);

    Received atGuest;
    Received atHost;
    u32 serial = 0;
    u32 backlog = 0;  // what the host's outbox is holding, in pieces
    std::vector<u8> piece(1024, 0x5A);
    std::vector<u8> terrain(1024, 0x33);
    std::vector<u8> pose(20, 0x11);

    u32 lastProgressMs = 0;
    u32 lastCount = 0;
    u32 longestStallMs = 0;
    u8 buffer[kMaxDatagram];

    for (u32 now = 0; now < 10u * 60u * 1000u; now += 33) {
        toGuest.nowMs = now;
        toHost.nowMs = now;
        toGuest.land();
        toHost.land();

        // Sixteen a frame each way, as `ac::Connection` reads them.
        usize size = 0;
        u16 node = 0;
        for (int i = 0; i < 16 && toGuest.receive(buffer, sizeof(buffer), &size, &node); ++i) {
            guest.receive(buffer, size, now, &Received::sink, &atGuest);
        }
        for (int i = 0; i < 16 && toHost.receive(buffer, sizeof(buffer), &size, &node); ++i) {
            host.receive(buffer, size, now, &Received::sink, &atHost);
        }

        // The host: two pieces of stream a frame, kept until they fit, as
        // `WorldServer::flush` keeps them; and a pose.
        backlog += 2;
        while (backlog > 0) {
            ++serial;
            piece[0] = u8(serial >> 24);
            piece[1] = u8(serial >> 16);
            piece[2] = u8(serial >> 8);
            piece[3] = u8(serial);
            if (!host.queue(Msg::GamePacket, piece.data(), piece.size(), true)) {
                --serial;
                break;
            }
            --backlog;
        }
        host.queue(Msg::Pose, pose.data(), pose.size(), false);

        // The guest: a column's worth of terrain every five frames, whatever
        // does not fit dropped as `answerTerrain` drops it; and a pose.
        if ((now / 33) % 5 == 0) {
            for (int i = 0; i < 20; ++i) {
                guest.queue(Msg::TerrainPart, terrain.data(), terrain.size(), true);
            }
        }
        guest.queue(Msg::Pose, pose.data(), pose.size(), false);

        host.flush(now, toGuest, 1);
        guest.flush(now, toHost, 1);

        if (atGuest.gamePackets != lastCount) {
            lastCount = atGuest.gamePackets;
            lastProgressMs = now;
        }
        if (now - lastProgressMs > longestStallMs) {
            longestStallMs = now - lastProgressMs;
        }
    }

    StressResult result;
    result.delivered = atGuest.gamePackets;
    result.backlog = backlog;
    result.longestStallMs = longestStallMs;
    result.outOfOrder = atGuest.outOfOrder;
    return result;
}

}  // namespace

// Two pieces of stream a frame is some 60 KB/s -- more than a session sends --
// so a backlog that stays small here is one that stays small in a game.
TEST(the_hosts_stream_keeps_moving_under_a_sessions_load_for_ten_minutes)
{
    const StressResult result = stress(5, 60, 24);
    CHECK(!result.outOfOrder);
    CHECK(result.delivered > 30000u);
    CHECK(result.longestStallMs < 5000u);
    CHECK(result.backlog < 100u);
}

// **The case that grew without bound**: a receiver that keeps six datagrams and
// drops the rest, on a wire losing one in seven. It used to end ten minutes
// 4,264 pieces behind and falling further.
TEST(the_hosts_stream_keeps_up_with_a_small_receive_buffer_and_heavy_loss)
{
    const StressResult result = stress(15, 60, 6);
    CHECK(!result.outOfOrder);
    CHECK(result.longestStallMs < 5000u);
    CHECK(result.backlog < 100u);
}
