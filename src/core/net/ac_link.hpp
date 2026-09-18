#pragma once

// One socket, three kinds of traffic, and the thing that sorts them out.
//
// **Why it is one socket.** The public address a NAT gives out belongs to the
// socket that sent through it, so the game has to leave by the same door the
// Hello did or the mapping the punch opened is the wrong one. That means the
// control messages to the server, the punch probes to the other console and the
// game itself all arrive on one port, and something has to tell them apart.
// This is that something: datagrams from the server go to `Client`, probes are
// answered here, and what is left is a `net::link` frame from a peer.
//
// **What a punch is.** A console learns its own public address for free -- the
// server reports the source it saw. Knowing the other side's address is not
// enough, because most NATs forward an inbound datagram only from an address
// the host has recently sent to. So both consoles fire at each other at the
// same moment, and each one's outbound probe opens the pinhole the other's
// needs. Simultaneity is the whole mechanism, which is why the server sends
// `PunchNow` to both ends from one `JoinRequest` with a window they count down
// together.
//
// **When it cannot be done.** A symmetric NAT allocates a fresh external port
// per destination, so the address the server saw is not the address the peer
// will see, and there is no hole to punch. Then the server allocates a relay
// and the frames go through it with a token in front. It costs real bandwidth,
// which is why it is the last resort and not the first.
//
// **One relayed peer per session, and that is the server's limit rather than
// this file's.** AlphaComputer keys its relay allocations by session id, so a
// second guest that cannot punch would take the first one's allocation. A host
// therefore refuses a second relayed guest instead of quietly breaking the one
// it already has.
//
// Everything here is non-blocking and nothing here allocates once `begin` has
// run. It exists only for as long as somebody is playing online: a single-player
// session never constructs one.

#include "core/net/ac_client.hpp"
#include "core/net/ac_wire.hpp"
#include "core/net/link.hpp"
#include "core/net/session.hpp"
#include "core/util/types.hpp"

#include <string>
#include <vector>

namespace mc::net::ac {

// The probe two consoles fire at each other, which the server never sees and
// has no opinion about. Four bytes of magic, the punch token both ends were
// given, and which half of the exchange this is.
//
// The token is what makes a probe from the real peer distinguishable from a
// stray datagram, and its full sixteen bytes are matched: a `net::link` frame
// that happened to begin with this magic *and* carry sixteen bytes somebody
// else's server chose is not a thing that happens.
inline constexpr u8 kProbeMagic[4] = {0x41, 0x43, 0x50, 0x4b};  // "ACPK"
inline constexpr usize kProbeSize = 4 + kTokenSize + 1;

// How often each end fires during the window. A dozen attempts across three
// seconds, which is about the cadence a 3DS can sustain without the radio
// getting in its own way.
inline constexpr u32 kProbeIntervalMs = 250;

// How many datagrams may be waiting between one `pump` and the session draining
// them. A frame's worth of arrivals, no more: what does not fit stays in the
// socket's own buffer, which is the right place for backpressure to sit.
inline constexpr int kInboxSlots = 16;

// How many addresses one peer may prove itself from: the room, the router's
// loopback, and the candidates the server offered besides.
inline constexpr int kMaxProven = 4;

// The host, and up to three guests.
inline constexpr int kMaxPeers = link::kMaxGuests;

class Connection : public link::Datagrams {
public:
    Connection();
    ~Connection() override;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Takes the buffers and starts the login. `hosting` decides the node
    // numbering: a host hands out 2 upwards, a guest is only ever talking to
    // node 1.
    void begin(const Endpoint& server, const Login& login, UdpTransport* transport,
               bool hosting, u32 nowMs);

    void end();

    // Once a frame, before anything reads the world. Reads everything the
    // socket has, drives the login and the punches, and leaves game frames for
    // `receive`.
    void pump(u32 nowMs);

    Client& client() { return client_; }
    const Client& client() const { return client_; }

    // The client's events, with the two this object acts on already handled:
    // an introduction becomes a punch in progress, and a relay allocation
    // becomes a peer that talks through it.
    Event takeEvent();

    // ---- what the punch produced ------------------------------------------

    // True once there is at least one peer to talk to.
    bool connected() const;

    // Peers still firing. The Session screen shows this as "Connecting".
    bool punching() const;

    // Whether the peer on `node` went through the relay rather than directly.
    // Counted per link because it is the number that says whether the punch
    // strategy is working at all.
    bool relayed(u16 node) const;

    // The name the server gave the console at the other end, for the lobby.
    const std::string& peerName() const { return peerName_; }

    // Why the last introduction came to nothing, or empty.
    const std::string& message() const { return message_; }

    // Frames dropped because the inbox was full when they arrived. Worth
    // showing on the debug page: unlike a lost datagram, this one is ours.
    u32 overflows() const { return overflows_; }

    // **One line on what the link to `node` did**, for the screen that says a
    // session ended: which path it took, frames each way, and what arrived from
    // addresses nobody proved -- with the last such address, because a frame
    // from the right console on an unexpected path looks exactly like that.
    // Nothing a player acts on; everything a report from a console needs.
    std::string report(u16 node) const;

    // ---- link::Datagrams ---------------------------------------------------

    bool send(u16 node, const u8* data, usize size) override;
    bool receive(u8* buffer, usize capacity, usize* size, u16* node) override;

private:
    struct Peer {
        bool used = false;
        u16 node = 0;
        u64 sessionId = 0;
        u8 punchToken[kTokenSize] = {};

        Candidate candidates[kMaxCandidates];
        int candidateCount = 0;

        // The address the first proving probe came from, which is where this
        // end sends. **Fixed once it answers**: two consoles on one network
        // prove themselves on two paths -- across the room, and out through the
        // router and back -- and following whichever probe landed last let the
        // two ends settle on different ones.
        Endpoint addr;
        bool connected = false;

        // **Every address that has carried this peer's punch token**, which is
        // where its frames are accepted from. A datagram from anywhere else
        // carries nothing that proves who sent it, so a NAT that rebinds
        // mid-session still ends the session through the link's own timeout
        // rather than being followed to an address a stranger could have named.
        Endpoint proven[kMaxProven];
        int provenCount = 0;

        bool relayed = false;
        Endpoint relay;
        u8 relayToken[kTokenSize] = {};
        // **Whether anything has come back through the relay yet.** Until it
        // has, this end keeps sending the relay a hello: AlphaComputer's relay
        // learns each end's address only from that end's own datagrams and
        // drops what it cannot deliver, and a host says nothing until a guest
        // has -- so without it the two wait on each other for ever.
        bool relayHeard = false;
        u32 lastRelayHelloMs = 0;

        u32 deadlineMs = 0;
        u32 lastProbeMs = 0;
        bool reported = false;

        // Game frames each way, for `report`.
        u32 framesIn = 0;
        u32 framesOut = 0;
    };

    Peer* peerForNode(u16 node);
    const Peer* peerForNode(u16 node) const;
    Peer* peerForAddress(const Endpoint& from);
    Peer* peerForToken(const u8 token[kTokenSize]);
    Peer* freePeer();

    void introduce(const ServerMsg& msg, u32 nowMs);
    void allocateRelay(const ServerMsg& msg, u32 nowMs);
    void firePunches(u32 nowMs);
    void sendProbe(Peer& peer, const Endpoint& to, u8 kind);
    void sendRelayHello(Peer& peer, u32 nowMs);
    bool handleProbe(const u8* data, usize size, const Endpoint& from, u32 nowMs);

    // Hands `size` bytes to the inbox for `node`, or counts an overflow.
    void enqueue(const u8* data, usize size, u16 node);

    // One datagram to a peer, through the relay when that is how it is reached.
    bool sendTo(Peer& peer, const u8* data, usize size);

    Client client_;
    UdpTransport* transport_ = nullptr;
    Endpoint server_;
    bool hosting_ = false;
    u32 nowMs_ = 0;

    Peer peers_[kMaxPeers];
    std::string peerName_;
    std::string message_;

    // A flat ring: slot `i`'s bytes live at `i * link::kMaxDatagram`.
    std::vector<u8> inbox_;
    usize inboxSize_[kInboxSlots] = {};
    u16 inboxNode_[kInboxSlots] = {};
    int inboxHead_ = 0;
    int inboxCount_ = 0;
    u32 overflows_ = 0;

    // Datagrams that were neither the server, nor a probe, nor from a proven
    // address -- and where the last of them came from.
    u32 strays_ = 0;
    Endpoint lastStray_;

    // `send`'s relay frame -- the token and the payload -- as a member rather
    // than 1.4 KB of stack on a 32 KB main thread.
    std::vector<u8> outgoing_;
};

}  // namespace mc::net::ac
