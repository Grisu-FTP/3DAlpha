#pragma once

// The link two 3DAlpha sessions talk over when they have found each other
// directly -- console to console -- rather than through a Java server.
//
// **Why this is not protocol 2.** A session between two of these is not a
// client and a server that have never met: both ends run the same build, share
// the same registries, and are one room apart. Protocol 2 was written for a
// TCP stream on a LAN in 2010, and its cost here is not the packet table --
// core/net/packets.hpp already has that, and `Msg::GamePacket` below is the
// seam that carries one when world sync arrives -- but its shape. A stream
// makes every dropped frame stall everything behind it, and local wireless
// drops frames for a walk across a room. So the link is datagrams, and the
// two things that need different guarantees get them: an edit must arrive and
// must arrive in order, a player's pose must arrive *soon* and is worthless
// once a newer one exists.
//
// **What the console gives us.** Local wireless (UDS) is a frame service, not
// a socket: each `udsSendTo` is one frame of at most 0x5C6 bytes to one node,
// delivered whole or not at all, with no ordering and no retry. That is
// exactly the substrate this layer is written against, and `Datagrams` is the
// only thing here that knows it -- see src/platform/ctr/local_link.hpp for the
// console's implementation and the tests for the one that loses frames on
// purpose.
//
// **What `Peer` adds**, and nothing more: sequence numbers, a 32-datagram
// window of acknowledgements riding on every frame, retransmission on a
// round-trip estimate rather than a fixed timer, in-order delivery of the
// reliable half, and a silence a caller can act on. No connection setup of its
// own -- that is a `Hello`/`Welcome` exchange in core/net/session.hpp, which is
// where a link becomes a session.

#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::net::link {

// Bumped when a message's body changes shape. Two builds that disagree refuse
// each other at the handshake rather than misreading each other's bytes.
inline constexpr u16 kProtocol = 2;

// `UDS_DATAFRAME_MAXSIZE` is 0x5C6 (1478). Frames are kept under that with
// room to spare: the margin costs nothing on a link this short and means a
// header that grows by a field later does not silently start truncating.
inline constexpr usize kMaxDatagram = 1400;

// flags, seq, ack, ackBits.
inline constexpr usize kHeaderSize = 9;
inline constexpr usize kMaxPayload = kMaxDatagram - kHeaderSize;

// One message's own header: kind and length.
inline constexpr usize kMessageHeader = 3;
inline constexpr usize kMaxMessage = kMaxPayload - kMessageHeader;

// How many reliable datagrams may be in flight. The window is the retransmit
// buffer as well, so it is also what the link costs in memory per peer --
// 32 * 1400 is 44 KB, which is the price of a burst of world data crossing a
// room without waiting for an acknowledgement between each frame.
inline constexpr int kWindow = 32;

// Nothing to say for this long and the link says it anyway, because an empty
// datagram still carries acknowledgements and still proves the console is
// there.
inline constexpr u32 kKeepAliveMs = 500;

// No datagram at all for this long and the peer is gone. Ten seconds is long
// for a link this short on purpose: a 3DS that has just been closed and opened,
// or walked behind someone, comes back inside it.
inline constexpr u32 kTimeoutMs = 10000;

// **How long a console may be excused for, when it says so in advance.**
//
// A 3DS running a library applet -- the software keyboard most of all -- is
// *suspended*: not slow, stopped. No thread runs, nothing is pulled off the
// radio, and no keep-alive goes out, so from the other console's side it looks
// exactly like somebody who has walked out of range. Typing a line of text
// takes longer than `kTimeoutMs` without trying, and what that looked like was
// the host throwing the guest out for opening a keyboard.
//
// So a console that knows it is about to stop says so, and its peers hold the
// timeout open for as long as it asked for, up to this. It is a grace and not
// a new timeout: a console that really did vanish while a keyboard was open is
// still dropped, a minute later instead of ten seconds later.
inline constexpr u32 kMaxAwayMs = 60000;

// What `aboutToBlock` asks for when it has nothing better to say. A keyboard
// is open for as long as somebody is typing, and nobody types for a minute.
inline constexpr u32 kAppletAwayMs = 45000;

// A pump gap longer than this is taken as **this** console having been stopped
// rather than having been slow, and the time inside it is not counted against
// anybody. The longest an ordinary frame has ever taken here is a fraction of
// this; a suspension is two orders of magnitude above it.
inline constexpr u32 kStallMs = 1000;

// Retransmission bounds around the measured round trip. The floor is above a
// local round trip so a late acknowledgement is not mistaken for a loss; the
// ceiling stops one bad sample from stalling the link for a second.
inline constexpr u32 kMinRetryMs = 60;
inline constexpr u32 kMaxRetryMs = 800;
inline constexpr u32 kInitialRttMs = 90;

// What one end says to the other. The reliable/unreliable split is not encoded
// here -- the sender chooses per message -- but every kind below has one
// sensible answer, named in its comment.
enum class Msg : u8 {
    // Reliable. The joiner's opening message: protocol and the name to play as.
    Hello = 0,
    // Reliable. The host's answer: protocol, the id the joiner plays as, and
    // what the session is.
    Welcome = 1,
    // Reliable. The host's refusal, with a reason to put on screen.
    Reject = 2,
    // Reliable. A line of chat; the host stamps the speaker's id on the way
    // through.
    Chat = 5,
    // Reliable. Somebody arrived or left; the host tells everyone else.
    Join = 6,
    Leave = 7,
    // Unreliable. Where a player is, as of one tick. The newest wins and a
    // late one is dropped, which is why this one is never resent.
    Pose = 8,
    // Reliable. This end is leaving on purpose. Its body is a `Reject`: the
    // two messages differ in when they arrive -- one turns a console away, the
    // other says goodbye to one that was let in -- and not in what they carry.
    Bye = 9,
    // 3 and 4 were a ping and its answer, and are not here: every datagram
    // already carries acknowledgements, so `Peer::rttMs` has the round trip
    // without a message of its own and the keep-alive already proves the
    // console is still there.
    // Reliable. The host asking a guest to generate terrain for one column,
    // and the answer, cut into parts because a column does not fit a datagram.
    // See core/net/terrain_share.hpp for what crosses and why it is only the
    // terrain.
    TerrainRequest = 11,
    TerrainPart = 12,
    // Reliable. How the host's world is played, when it changes -- the pause
    // menu can turn a Survival world Creative without anybody leaving it. The
    // same two bytes the Welcome carries; see `WorldRules`.
    Rules = 13,
    // Reliable. **I am about to stop answering, and for roughly this long.**
    // Sent before a console hands the CPU to a library applet, which suspends
    // it outright -- see `kMaxAwayMs`. Two bytes of milliseconds.
    Away = 14,
    // Reliable. **The seam for world sync.** A protocol-2 packet, verbatim,
    // so the half of the client that already applies them -- ClientSession's
    // parser, NetPlay, the streamer's `supplyColumn` -- can be pointed at this
    // link without a second implementation of chunks, blocks and entities.
    GamePacket = 10,
    // Reliable, all five. **A whole world crossing the room**, which is not a
    // session at all: no player joins, nothing ticks, and the two consoles say
    // goodbye when the last file lands. See core/net/world_copy.hpp for the
    // exchange and for why the payload needs no sequence numbers of its own.
    WorldOffer = 20,
    WorldAnswer = 21,
    WorldFile = 22,
    WorldData = 23,
    WorldResult = 24,
};

const char* nameOf(Msg kind);

// Messages are handed over as they are read out of a datagram: the body points
// into the caller's own buffer and is valid only for the call.
using MessageSink = void (*)(void* ctx, Msg kind, const u8* body, usize size);

// The only thing in this file that knows what the frames travel on. One
// implementation is the console's local wireless; the other is the test's,
// which drops and reorders on purpose.
class Datagrams {
public:
    virtual ~Datagrams() = default;

    // One frame to one node. False when the frame could not be handed over at
    // all, which is a broken link rather than a lost frame -- a frame that is
    // simply lost returns true here and is noticed by its acknowledgement
    // never arriving.
    virtual bool send(u16 node, const u8* data, usize size) = 0;

    // The next frame that arrived, or false when there is none waiting. Never
    // blocks.
    virtual bool receive(u8* buffer, usize capacity, usize* size, u16* node) = 0;
};

// One end of one link. The host owns one of these per joiner; a joiner owns
// one, for the host.
class Peer {
public:
    Peer();

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;

    // Back to the state a fresh one is in, keeping the buffers.
    void reset(u32 nowMs);

    // Queues a message. Reliable ones are kept until the peer acknowledges
    // them; unreliable ones go out with the next datagram or not at all.
    //
    // False when the window is full (reliable) or the message is longer than a
    // datagram can hold. A caller that sees false for a reliable message should
    // try again rather than drop it -- see `stalled`.
    bool queue(Msg kind, const u8* body, usize size, bool reliable);
    bool queue(Msg kind, const std::vector<u8>& body, bool reliable)
    {
        return queue(kind, body.data(), body.size(), reliable);
    }

    // The next datagram to put on the wire, into `out`. Call until it returns
    // false, which is when this end has nothing more to say this moment.
    bool nextDatagram(u32 nowMs, u8* out, usize capacity, usize* size);

    // Everything queued and everything due for retransmission, handed to
    // `datagrams` for `node`. False when the datagrams themselves failed.
    bool flush(u32 nowMs, Datagrams& datagrams, u16 node);

    // One datagram off the wire. Messages reach `sink` in the order the sender
    // queued them; a reliable datagram that arrived early is held until the
    // gap before it is filled. False when the datagram is malformed, which
    // ends the link -- see the note in the implementation.
    bool receive(const u8* data, usize size, u32 nowMs, MessageSink sink, void* ctx);

    // Smoothed round trip, the number the retransmission timer is built on.
    u32 rttMs() const { return rttMs_; }

    // Nothing has been heard for `kTimeoutMs`, and no grace is outstanding.
    bool timedOut(u32 nowMs) const;

    // This peer said it was about to be suspended. Held open for `ms`, capped
    // at `kMaxAwayMs`. See `Msg::Away`.
    void grantAway(u32 nowMs, u32 ms);

    // **Time this console never saw**, and must not hold against the peer.
    // Pushes the timeout, the retransmission clocks and the keep-alive forward
    // by the gap, so a console coming back from a suspension does not decide
    // on its first frame that everybody else has gone -- and does not answer
    // that frame with a burst of retransmissions for datagrams whose
    // acknowledgements were simply never pulled.
    void forgive(u32 gapMs);

    // The window is full of unacknowledged datagrams, so `queue` is refusing
    // reliable messages. Either the peer has stopped answering or this end is
    // sending faster than the link carries.
    bool stalled() const;

    // Reliable datagrams still waiting for an acknowledgement.
    int inFlight() const;

    // Counters, for the debug page and the tests.
    u32 datagramsSent() const { return sent_; }
    u32 datagramsReceived() const { return received_; }
    u32 retransmits() const { return retransmits_; }
    u32 dropped() const { return dropped_; }

private:
    struct Slot {
        u16 seq = 0;
        usize size = 0;
        u32 sentMs = 0;
        u8 tries = 0;
        bool inUse = false;
    };

    // Seals whatever is in `pending_` into a window slot. False when the
    // window is full.
    bool sealPending(u32 nowMs);
    void writeHeader(u8* out, bool reliable, u16 seq) const;
    void ackDatagram(u16 seq, u32 ackBits, u32 nowMs);
    void noteRtt(u32 sample);
    bool deliver(const u8* body, usize size, MessageSink sink, void* ctx);
    u32 retryMs() const;

    // The retransmit window. One flat buffer; a slot's bytes live at
    // `index * kMaxDatagram`.
    std::vector<u8> window_;
    Slot slots_[kWindow];

    // The reliable datagram being filled, and the unreliable one.
    std::vector<u8> pending_;
    std::vector<u8> unreliable_;

    // `flush`'s frame, a member rather than 1400 bytes of stack: this is
    // called once a frame from the game loop, whose stack is 32 KB.
    std::vector<u8> outgoing_;

    // Reliable datagrams that arrived before the gap in front of them was
    // filled, keyed by sequence.
    std::vector<u8> holding_;
    usize holdingSize_[kWindow] = {};
    bool held_[kWindow] = {};

    u16 nextSeq_ = 0;     // the sequence the next reliable datagram gets
    u16 expected_ = 0;    // the next reliable sequence to deliver
    u16 ackLatest_ = 0;   // the highest sequence seen from the peer
    u32 ackBits_ = 0;     // the 32 before it
    bool sawAny_ = false;  // whether `ackLatest_` means anything yet

    u32 rttMs_ = kInitialRttMs;
    u32 lastHeardMs_ = 0;
    u32 lastSentMs_ = 0;
    u32 awayUntilMs_ = 0;
    bool away_ = false;

    u32 sent_ = 0;
    u32 received_ = 0;
    u32 retransmits_ = 0;
    u32 dropped_ = 0;
};

// ---- message bodies --------------------------------------------------------
//
// Every body is built and read here rather than at the call sites, so a field
// that moves moves once. All of them are big-endian through core/net/wire.hpp,
// which is what the rest of this port's wire code already uses.

struct Hello {
    u16 protocol = kProtocol;
    std::string name;
    // **This console's generator, not the world's.** The seed and the world's
    // switches are the host's to state; all a joiner can say is which build's
    // arithmetic it would be using. The host compares it with its own and
    // decides whether this console may ever be asked to make terrain.
    std::string generator;
};

// Who generated what, so two consoles can agree they mean the same world
// before either relies on the other's arithmetic. Carried in the Welcome, and
// the one thing a guest needs before it can make terrain for the host.
//
// It is not a security check -- a console that lies about it is trusted
// anyway, see terrain_share.hpp -- but it catches the failure that will
// actually happen, which is somebody on a different build.
struct GeneratorId {
    i64 seed = 0;
    // The world's generation switches, flattened. `terrain_share.hpp` packs
    // and unpacks them; nothing here knows what the bits mean.
    u8 options = 0;
    // The build's own name for its generator, which changes when its output
    // does.
    std::string version;

    bool matches(const GeneratorId& other) const
    {
        return seed == other.seed && options == other.options && version == other.version;
    }
};

// **How the host's world is played**, which a guest has no other way to learn.
//
// It is not in the protocol-2 stream because protocol 2 has nowhere to put it:
// a1.1.2 has exactly one way to play and its Login packet says nothing about
// rules. It is not a preference on the joining console either -- a guest in a
// Survival world must not be flying, and a guest in a Creative one must not be
// starving -- so it travels with the Welcome, which is ours and is the first
// thing a guest is told about the world it has reached.
//
// The values are `settings::Gamemode` and `settings::Difficulty` as bytes.
// core/net/ does not include core/settings/, so they cross as numbers and the
// two ends agree on the enum; an unknown value is treated as the first.
struct WorldRules {
    u8 gamemode = 0;
    u8 difficulty = 0;
};

struct Welcome {
    u16 protocol = kProtocol;
    u8 playerId = 0;
    std::string worldName;
    std::string hostName;
    // Empty `version` means the host is not offering terrain generation to
    // its guests -- which is what it says to a console whose build it does
    // not recognise.
    GeneratorId world;
    WorldRules rules;
};

// The body of both `Reject` and `Bye`: why, in words a screen can show without
// a table of codes of its own.
struct Reject {
    std::string reason;
};

// Position in 1/32 of a block, which is a1.1.2's own absolute-integer unit
// (`MathHelper.floor_double(x * 32)`), and the two angles as a byte turn.
struct Pose {
    u8 playerId = 0;
    u16 tick = 0;
    i32 x = 0;
    i32 y = 0;
    i32 z = 0;
    i8 yaw = 0;
    i8 pitch = 0;
    u8 flags = 0;  // bit0 on ground, bit1 sneaking
};

struct Chat {
    u8 playerId = 0;
    std::string text;
};

struct Player {
    u8 playerId = 0;
    std::string name;
};

void encode(const Hello& value, std::vector<u8>* out);
void encode(const Welcome& value, std::vector<u8>* out);
void encode(const WorldRules& value, std::vector<u8>* out);
void encode(const Reject& value, std::vector<u8>* out);
void encode(const Pose& value, std::vector<u8>* out);
void encode(const Chat& value, std::vector<u8>* out);
void encode(const Player& value, std::vector<u8>* out);
void encode(const GeneratorId& value, std::vector<u8>* out);

// False when the body is short or malformed, which the caller treats as a
// broken peer. `*out` is untouched on failure.
bool decode(const u8* body, usize size, Hello* out);
bool decode(const u8* body, usize size, Welcome* out);
bool decode(const u8* body, usize size, WorldRules* out);
bool decode(const u8* body, usize size, Reject* out);
bool decode(const u8* body, usize size, Pose* out);
bool decode(const u8* body, usize size, Chat* out);
bool decode(const u8* body, usize size, Player* out);
bool decode(const u8* body, usize size, GeneratorId* out);

}  // namespace mc::net::link
