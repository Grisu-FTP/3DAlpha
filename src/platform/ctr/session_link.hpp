#pragma once

// What a session runs over, whichever radio that turns out to be.
//
// `core/net/session.hpp` needs only `net::link::Datagrams` -- frames to a node
// and frames from one -- and it was written that way on purpose. What the two
// console-side halves, `HostPlay` and `GuestPlay`, additionally want is a
// handful of things a `Datagrams` has no business knowing: whether the link is
// still up, how to take it down, and, for a host on local wireless, how to
// rewrite the beacon when the player count changes.
//
// That used to be `LocalLink` spelled out in both of them, which is what made
// them local-wireless-only. This is the same three questions as an interface,
// so that the other implementation -- a UDP socket that has been introduced to
// another console over the internet, see platform/ctr/online.hpp -- drops into
// the same slot. Everything above it, the handshake, the player list, the world
// server and the terrain sharing, is unchanged and untouched by which one it is.

#include "core/net/link.hpp"
#include "core/util/types.hpp"

namespace mc::ctr {

class SessionLink : public net::link::Datagrams {
public:
    ~SessionLink() override = default;

    // Tears it down. Safe to call on a link that never came up, and safe to
    // call twice.
    virtual void leave() = 0;

    // False once there is nothing to talk through -- the radio is down, or the
    // socket is closed.
    virtual bool active() const = 0;

    // A host telling the room how many players are in the session. **Local
    // wireless only**: a beacon is something a scan in the same room can see,
    // and an internet session is found through the server's directory instead,
    // which learns the count from the joins it brokered.
    virtual void advertise(int players) { (void)players; }

    // **Carry the link forward one frame**, before anything reads it. Local
    // wireless has nothing to do here -- the radio is pulled from inside
    // `receive` -- but an internet link has to service its socket, its
    // keep-alive and any punch still in progress, and the game loop above has
    // no idea which of the two it is holding. So it asks, every frame, and one
    // of the two answers by doing nothing.
    virtual void service(u32 nowMs) { (void)nowMs; }

    // Frames this console could not put on the wire at all. Not the same as a
    // lost frame, and worth showing separately on the debug page: this one is
    // ours.
    virtual u32 sendOverflows() const { return 0; }
};

}  // namespace mc::ctr
