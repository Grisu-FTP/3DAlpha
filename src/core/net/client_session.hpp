#pragma once

// One connection to a protocol-2 server, run on a thread of its own and talked
// to through two queues.
//
// **Why a thread.** Everything the socket does can stall: resolving a name,
// connecting, a server that stops reading. And the heaviest thing a server
// sends -- a zlib-compressed column, dozens of them at login -- has to be
// inflated before the world can use it, which docs/protocol-a1.1.2.md already
// ruled off core 0's frame. So the session thread owns the socket, the parser
// and the inflate, and hands the main thread finished work: a packet, a built
// `ChunkColumn`, or a region of raw planes to write in.
//
// **What it decides for itself** is only what a1.1.2's NetClientHandler decides
// before a world exists: answer the handshake with a login when the server says
// `-` (offline), and end the connection on a kick, a malformed stream, or a
// silence of a minute (`ii`'s 1200 ticks). Everything else is the game's.
//
// **It keeps the connection alive while the game is not.** The pause menu stops
// the world, and 0.2.1 drops a client it has heard nothing from for a minute --
// the same 1200-tick counter, on its side. a1.1.2 never met that because it has
// no pause in multiplayer; this has one, so the thread sends a Keep Alive
// whenever a second goes by with nothing else sent.

#include "core/net/chunk_payload.hpp"
#include "core/net/packet_channel.hpp"
#include "core/net/packets.hpp"
#include "core/net/tcp_socket.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mc::net {

class ClientSession : public PacketChannel {
public:
    // The channel's, named here so the call sites that grew up around this
    // class keep reading the way they did.
    using State = PacketChannel::State;
    using Event = PacketChannel::Event;

    ClientSession() = default;
    ~ClientSession() override { stop("Quitting"); }

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Starts the thread, which connects and logs in as `username`. False only
    // when the thread itself could not be started.
    bool start(const std::string& host, u16 port, const std::string& username);

    // Queues a packet for the thread to send. Cheap: an encode into a buffer
    // whose capacity is kept, under a lock nobody holds for long.
    void send(const Packet& packet) override;

    // Takes the oldest event, if there is one.
    bool poll(Event* out) override;

    // Sends a Disconnect with `reason` when still connected, gives the thread a
    // moment to flush it, and joins. Safe to call twice.
    void stop(const char* reason);

    State state() const override { return state_.load(); }

    // Bytes in and out, and packets parsed, for the debug page.
    u64 bytesIn() const override { return bytesIn_.load(); }
    u64 bytesOut() const override { return bytesOut_.load(); }
    u32 packetsIn() const override { return packetsIn_.load(); }

private:
    static void threadEntry(void* self);
    void run();
    bool pumpReceive(usize* received);
    bool pumpSend(bool* sentAnything);
    bool handle(const Packet& packet);
    void push(Event&& event);
    void fail(const char* title, const std::string& detail);

    std::string host_;
    u16 port_ = 0;
    std::string username_;

    TcpSocket socket_;
    std::vector<u8> in_;
    usize inStart_ = 0;
    std::vector<u8> writing_;
    usize writeOffset_ = 0;
    Packet parsed_;

    std::mutex lock_;
    std::deque<Event> events_;
    std::vector<u8> outbox_;
    std::string stopReason_;
    bool stopRequested_ = false;

    std::atomic<State> state_{State::Idle};
    std::atomic<u64> bytesIn_{0};
    std::atomic<u64> bytesOut_{0};
    std::atomic<u32> packetsIn_{0};

    std::thread thread_;
    void* platformThread_ = nullptr;
    bool closedReported_ = false;
};

}  // namespace mc::net
