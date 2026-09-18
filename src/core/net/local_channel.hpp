#pragma once

// The guest's end of a local session, as a protocol-2 stream.
//
// **The link layer already gives a byte stream, so this is `ClientSession`
// with the socket taken out.** `Msg::GamePacket` is delivered reliably and in
// order (core/net/link.hpp), which is exactly what TCP promises and exactly
// what `parsePacket` needs -- a packet has no length prefix, so the bytes have
// to arrive whole and in order or the parser loses its place forever. What
// crosses is therefore not "a packet per message" but a stream cut into
// message-sized pieces, and the pieces are glued back together here.
//
// **The thread is the point.** A column arrives zlib-compressed and has to be
// inflated before the world can use it, and CONTRIBUTING's rule that no
// decompression runs on core 0 does not stop being true because the bytes came
// off the radio instead of off a socket. So the main thread does two memcpys a
// frame -- `feed` in, `takeOutbound` out -- and this thread does the parsing,
// the inflating and the column building, handing back the same finished
// `PacketChannel::Event`s a server connection hands back.
//
// Nothing in here knows about UDS, and nothing in here knows about the world;
// it is the join between core/net/session.hpp and platform/ctr/net_play.hpp.

#include "core/net/packet_channel.hpp"
#include "core/util/types.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc::net {

class LocalChannel : public PacketChannel {
public:
    LocalChannel() = default;
    ~LocalChannel() override;

    LocalChannel(const LocalChannel&) = delete;
    LocalChannel& operator=(const LocalChannel&) = delete;

    // Starts the parse thread. The channel begins in `LoggingIn`: the link's
    // own handshake is done by now, but the *world's* is not -- the host still
    // has to send a Login, and until it does there is nothing to play.
    // False only when the thread could not be started.
    bool start();

    // Bytes off the link, in order. Main thread.
    void feed(const u8* data, usize size);

    // Bytes for the link, in order, appended to `out`. Main thread.
    void takeOutbound(std::vector<u8>* out);

    // The session ended for a reason the link knows and this does not -- the
    // host quit, the radio went down, the player left. Queues the disconnect
    // screen and stops the thread. Safe to call twice.
    void closeWith(const std::string& title, const std::string& detail);

    // Stops the thread without queueing anything. Safe to call twice.
    void stop();

    void send(const Packet& packet) override;
    bool poll(Event* out) override;
    State state() const override { return state_.load(); }
    u64 bytesIn() const override { return bytesIn_.load(); }
    u64 bytesOut() const override { return bytesOut_.load(); }
    u32 packetsIn() const override { return packetsIn_.load(); }

private:
    static void threadEntry(void* self);
    void run();

    // One parsed packet, on the thread. False ends the stream.
    bool handle(const Packet& packet);
    void push(Event&& event);
    void fail(const char* title, const std::string& detail);

    std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Event> events_;
    // Bytes the main thread has handed over and the parse thread has not taken.
    std::vector<u8> inbox_;
    // Bytes `send` has encoded and the main thread has not put on the link.
    std::vector<u8> outbox_;
    bool stopRequested_ = false;

    // The thread's own, and touched by nothing else.
    std::vector<u8> in_;
    usize inStart_ = 0;
    Packet parsed_;

    std::atomic<State> state_{State::Idle};
    std::atomic<u64> bytesIn_{0};
    std::atomic<u64> bytesOut_{0};
    std::atomic<u32> packetsIn_{0};

    std::thread thread_;
    void* platformThread_ = nullptr;
    bool closedReported_ = false;
};

}  // namespace mc::net
