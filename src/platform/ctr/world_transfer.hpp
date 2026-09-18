#pragma once

// One console handing a world to another, over the same local wireless a
// session runs on.
//
// **Not a session, and deliberately not built on one.** `HostPlay` and
// `GuestPlay` own a world, a player list, a terrain responder and a packet
// channel; a transfer owns a folder. What the two share is the radio and the
// reliable link on top of it, so that is all this reuses -- `LocalLink` for the
// frames and `net::link::Peer` for the ordering, with core/net/world_copy.hpp
// driving what crosses.
//
// **The exporting console is the one that puts the beacon up.** That mirrors
// Host and Join: the console with the thing offers it, and the console that
// wants it goes looking. The beacon says `LocalKind::WorldOffer`, so a player
// looking for a game is not shown a console that is only handing a folder over.
//
// **The original is never written to.** The sending half opens its world's
// files for reading and nothing else; the receiving half writes only inside a
// staging directory of its own. An abandoned transfer costs the sender nothing
// at all.

#include "core/io/file_system.hpp"
#include "core/net/link.hpp"
#include "core/net/session.hpp"
#include "core/net/world_copy.hpp"
#include "core/util/types.hpp"
#include "platform/ctr/local_link.hpp"

#include <memory>
#include <string>

namespace mc::ctr {

class WorldTransfer {
public:
    explicit WorldTransfer(io::FileSystem& fs);
    ~WorldTransfer();

    WorldTransfer(const WorldTransfer&) = delete;
    WorldTransfer& operator=(const WorldTransfer&) = delete;

    // **Export.** Reads the world's shape off the card, puts the beacon up and
    // waits. False with `*error` in words a player can act on, in which case
    // nothing was put on the air.
    bool offer(const std::string& worldDir, const std::string& worldName,
               const std::string& hostName, std::string* error);

    // **Import.** Connects to an offer a scan found and takes it under
    // `targetName`, which the menu has already sanitised and already checked is
    // free -- asking on a keyboard suspends the console, and a suspended
    // console is one the link would have to be told about in advance.
    bool accept(const LocalSession& session, const std::string& savesDir,
                const std::string& targetName, std::string* error);

    // One frame: the radio in, whatever is due out. True when something the
    // screen shows has changed.
    bool pump();

    bool active() const { return link_.active(); }
    bool sending() const { return sender_ != nullptr; }

    // The transfer is over, one way or the other.
    bool finished() const;
    bool succeeded() const;

    // Why it stopped, empty while it is still going and on success.
    const std::string& reason() const { return reason_; }

    net::copy::Progress progress() const;

    // What the other console calls the world. The sender knows it from the
    // start; the receiver learns it from the offer.
    const std::string& worldName() const { return worldName_; }

    // Stops and tells the other console, if there is still a link to tell it
    // through. Safe to call twice.
    void stop(const char* why);

private:
    // `Peer::receive`'s sink.
    static void onMessage(void* context, net::link::Msg kind, const u8* body, usize size);

    io::FileSystem& fs_;
    LocalLink link_;
    net::link::Peer peer_;

    // Exactly one of the two is set, and which one is what this object is.
    std::unique_ptr<net::copy::Sender> sender_;
    std::unique_ptr<net::copy::Receiver> receiver_;

    std::string worldName_;
    std::string reason_;

    // The node at the other end: the host is 1 and the one guest is 2.
    u16 peerNode_ = 0;

    // **Until the first datagram lands there is nobody to time out.** An export
    // sits on the air waiting for somebody to walk over, which takes as long as
    // it takes; the ten-second silence only means something once the two
    // consoles have spoken.
    bool heard_ = false;
    u32 lastPumpMs_ = 0;
    bool stopped_ = false;
};

}  // namespace mc::ctr
