#pragma once

// A protocol-2 stream, whichever wire it runs over.
//
// **This is an interface because there are now two wires and only one client.**
// `ctr::NetPlay` is the whole of what a multiplayer client does to a world --
// the columns, the teleports, the block changes, the other players, the digs
// and the places -- and none of it cares whether the bytes came off a TCP
// socket to a Java server or off the radio from the console next door. So the
// socket moved behind this and the client stopped naming it.
//
// **Both implementations put a thread between the wire and the game**, and for
// the same reason: the heaviest thing that arrives is a zlib-compressed column
// and docs/protocol-a1.1.2.md ruled that inflate off core 0's frame long before
// there was a second wire. What comes out is finished work -- a parsed packet,
// a built `ChunkColumn`, or a region of raw planes -- which is why `Event` is
// part of the interface rather than part of either session.
//
// See core/net/client_session.hpp for the socket and core/net/local_channel.hpp
// for the radio.

#include "core/net/chunk_payload.hpp"
#include "core/net/packets.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

#include <memory>
#include <string>

namespace mc::net {

class PacketChannel {
public:
    enum class State : u8 {
        Idle,
        Connecting,  // resolving and connecting
        LoggingIn,   // handshake sent; waiting for the server's Login
        Playing,     // logged in
        Closed,
    };

    struct Event {
        enum class Kind : u8 {
            LoggedIn,  // `entityId` is the player's
            Packet,    // `packet` is anything the channel did not consume itself
            Column,    // `column` is a whole column from a Map Chunk
            Region,    // `region` is a partial Map Chunk, already inflated
            Closed,    // `title` and `detail` are a1.1.2's disconnect screen
        };

        Kind kind = Kind::Packet;
        i32 entityId = 0;
        net::Packet packet;
        std::unique_ptr<world::ChunkColumn> column;
        MapChunkRegion region;
        std::string title;
        std::string detail;
    };

    virtual ~PacketChannel() = default;

    // Queues a packet for the wire. Cheap: an encode into a buffer whose
    // capacity is kept, under a lock nobody holds for long.
    virtual void send(const Packet& packet) = 0;

    // Takes the oldest event, if there is one.
    virtual bool poll(Event* out) = 0;

    virtual State state() const = 0;

    // For the debug page.
    virtual u64 bytesIn() const = 0;
    virtual u64 bytesOut() const = 0;
    virtual u32 packetsIn() const = 0;
};

}  // namespace mc::net
