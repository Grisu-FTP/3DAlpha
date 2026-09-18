#pragma once

// The console's half of a local session: the 3DS's own local wireless (UDS),
// the beacon a host puts on the air, and the frames the link layer talks
// through.
//
// **UDS, not StreetPass.** The two get spoken of together because both are
// "the wireless that does not need the internet", but they are different
// services and only one of them is a game link: StreetPass (CECD) is a mailbox
// the system exchanges while the console is asleep -- one small message per
// title per meeting, hours apart -- while UDS is the local-play radio that
// Mario Kart and Smash run on, and it carries frames between running consoles
// in the same room. A session needs the second one.
//
// **What UDS gives us**, and why core/net/link.hpp is shaped the way it is: a
// host `udsCreateNetwork`s and everyone else `udsConnectNetwork`s to it, each
// console gets a node id (the host is 1), and `udsSendTo` puts one frame of at
// most 0x5C6 bytes on the air for one node. Frames are not ordered, not
// acknowledged and not retried -- that is the link layer's job, and this file
// is only the radio.
//
// **The beacon carries the session's name.** A host's network is visible to a
// scan before anybody joins, and the appdata rides along with it, so the join
// list can show the world's name and who is hosting without connecting to
// anything first. That is what makes the Join screen a list rather than a
// blind attempt.

#include "core/net/link.hpp"
#include "core/util/types.hpp"

#include <3ds.h>

#include <string>
#include <vector>

namespace mc::ctr {

// **What a beacon is offering**, which is not always a game.
//
// A world on its way to another console uses the same radio, the same
// passphrase and the same frames as a session -- what it does not use is any of
// the session's meaning: nobody joins, nothing ticks, and the two consoles say
// goodbye when the last file lands. So the beacon says which it is, and the two
// lists stay separate: a player looking for somebody to play with is not shown
// a console that is only handing a folder over, and the other way round.
enum class LocalKind : u8 {
    Session = 0,
    WorldOffer = 1,
};

// A host that answered a scan: everything the Join list shows, and the network
// struct `LocalLink::join` needs to reach it.
struct LocalSession {
    udsNetworkStruct network{};
    LocalKind kind = LocalKind::Session;
    std::string worldName;
    std::string hostName;
    u8 players = 0;
    u8 maxPlayers = 0;

    // Whether the beacon is one of ours *and* speaks this build's protocol. An
    // incompatible one is still listed -- a player who can see the session in
    // front of them should be told why it cannot be joined, not left to wonder
    // why nothing appears.
    bool compatible = false;
};

// Brings the UDS service up. False with `*error` in words a player can act on:
// the wireless switch, mostly.
bool startLocalWireless(std::string* error);
void stopLocalWireless();
bool localWirelessReady();

// **Somebody is about to hand the CPU to a library applet**, which suspends
// this application outright: no thread runs, nothing is pulled off the radio,
// and no keep-alive goes out. From the other console's side that is
// indistinguishable from having walked out of range, and it lasts as long as
// it takes to type -- which is how opening the software keyboard came to get a
// guest thrown out of a session.
//
// So every keyboard in this build says so first, and whichever session is open
// puts a warning on the wire before the applet starts. See
// `net::link::Msg::Away`.
//
// **A hook rather than a parameter**, because there is one radio in a console
// and the keyboards are scattered through the menu and the overlay: the
// alternative is a session pointer threaded through every screen that has a
// text field, to be used by none of them.
using LinkPauseFn = void (*)(void* ctx, u32 expectedMs);
void setLinkPauseHook(LinkPauseFn hook, void* ctx);

// Called immediately before `swkbdInputText` and anything else that hands the
// console over. Does nothing when no session is open.
void linkPausing(u32 expectedMs);

// Every 3DAlpha beacon in range, of both kinds -- the caller filters on
// `LocalSession::kind`, because the radio has no business knowing which of the
// two screens asked. Takes about a second; the caller draws something first. An
// empty list with no error is the ordinary "nobody is hosting" answer.
bool scanLocalSessions(std::vector<LocalSession>* out, std::string* error);

// One console's end of a local session. Hosting creates the network and puts
// the beacon up; joining connects to one. Either way what comes out is a
// `Datagrams` for core/net/session.hpp to run over.
class LocalLink : public net::link::Datagrams {
public:
    ~LocalLink() override;

    LocalLink(const LocalLink&) = delete;
    LocalLink& operator=(const LocalLink&) = delete;
    LocalLink() = default;

    bool host(const std::string& worldName, const std::string& hostName, LocalKind kind,
              std::string* error);
    bool join(const LocalSession& session, std::string* error);

    // Tears the network down. Safe to call on a link that never came up.
    void leave();

    bool active() const { return active_; }
    bool hosting() const { return hosting_; }

    // This console's node id -- 1 when hosting, 2 upwards when not.
    u16 node() const { return node_; }

    // Rewrites the beacon with a new player count, so a console scanning from
    // the next room sees "2/4" rather than what the session looked like when it
    // opened. Hosts only, and cheap enough to call when the count changes.
    void advertise(int players);

    // Frames dropped by the radio rather than by the air: the send buffer was
    // full. Worth showing on the debug page, because it is the one loss the
    // link layer cannot tell from a bad room.
    u32 sendOverflows() const { return overflows_; }

    bool send(u16 node, const u8* data, usize size) override;
    bool receive(u8* buffer, usize capacity, usize* size, u16* node) override;

private:
    udsBindContext bind_{};
    udsNetworkStruct network_{};
    bool active_ = false;
    bool hosting_ = false;
    bool bound_ = false;
    u16 node_ = 0;
    u8 maxNodes_ = 0;
    LocalKind kind_ = LocalKind::Session;
    std::string worldName_;
    std::string hostName_;
    u32 overflows_ = 0;
};

}  // namespace mc::ctr
