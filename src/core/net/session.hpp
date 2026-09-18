#pragma once

// A 3DAlpha session as the two ends see it: one console hosting a world it has
// open, and up to three others that found it and joined.
//
// This is the half above core/net/link.hpp -- who is in the session rather than
// how the bytes get there -- and it is deliberately free of everything the
// console provides. It never touches UDS, a clock, a world or a screen: it is
// given the time, handed a `Datagrams` to talk through, and reports what
// happened to a listener. That is what lets the whole handshake, the player
// list, the drop-outs and the retransmissions be tested on the host against a
// `Datagrams` that loses frames on purpose.
//
// **The topology is a star, with the host at the middle.** Local wireless lets
// any node send to any other, but a session has one console that owns the
// world, so everything goes through it: a guest talks only to the host, and
// the host relays what the others need. That keeps one copy of the truth and
// means a guest never has to know what to do when two consoles disagree.
//
// **The world crosses as `Msg::GamePacket` and this file never looks inside
// one.** What a session carries is who is here; what the world is belongs to
// core/net/world_server.hpp on one side and core/net/local_channel.hpp on the
// other, and both of them speak the protocol-2 packets core/net/packets.hpp
// already encodes. Keeping the two apart is what lets the whole handshake be
// tested against a `Datagrams` that loses frames without a world anywhere near
// it.
//
// The message is a piece of a **byte stream**, not a packet: a protocol-2
// packet has no length prefix, so the two ends rely on this file's promise
// that reliable messages arrive whole and in the order they were queued.

#include "core/net/link.hpp"
#include "core/util/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mc::net::link {

// Four consoles in one session: the host and three guests. Local wireless
// allows sixteen nodes, but the limit here is the world -- the host is
// simulating every guest's surroundings on one ARM11 core, and this is the
// number that leaves it a game to play.
inline constexpr int kMaxGuests = 3;

// UDS's own numbering: the host is node 1, and the guests are 2 upwards. The
// session's player ids follow it, so an id is also where a frame came from.
inline constexpr u16 kHostNode = 1;
inline constexpr u8 kHostPlayerId = 1;

// What a session tells the game around it. Every one of these arrives on the
// thread that called `pump`.
class SessionListener {
public:
    virtual ~SessionListener() = default;

    // A guest finished the handshake, or was told about one who had.
    virtual void onPlayerJoined(u8 playerId, const std::string& name) { (void)playerId; (void)name; }

    // They left, were dropped, or the link to them went quiet.
    virtual void onPlayerLeft(u8 playerId, const std::string& reason)
    {
        (void)playerId;
        (void)reason;
    }

    virtual void onChat(u8 playerId, const std::string& text) { (void)playerId; (void)text; }
    virtual void onPose(const Pose& pose) { (void)pose; }

    // How the host's world is played -- at the Welcome, and again whenever the
    // host changes it. Guests only.
    virtual void onWorldRules(const WorldRules& rules) { (void)rules; }

    // A protocol-2 packet from the other end. See the note at the top.
    virtual void onGamePacket(u8 playerId, const u8* data, usize size)
    {
        (void)playerId;
        (void)data;
        (void)size;
    }

    // **Terrain, in both directions.** The session carries these and does not
    // look inside them: what a column is, how it is packed and who may make
    // one is core/net/terrain_share.hpp's business, and keeping it out of here
    // is what stops the session depending on the world generator.
    virtual void onTerrainRequest(const u8* body, usize size)
    {
        (void)body;
        (void)size;
    }

    virtual void onTerrainPart(u8 playerId, const u8* body, usize size)
    {
        (void)playerId;
        (void)body;
        (void)size;
    }
};

// ---------------------------------------------------------------------------

// The console that owns the world.
class HostSession {
public:
    HostSession();
    ~HostSession();

    HostSession(const HostSession&) = delete;
    HostSession& operator=(const HostSession&) = delete;

    // Opens the session. `worldName` and `hostName` are what a joiner is told
    // it has reached. The buffers every guest needs are taken here, once, so
    // nobody arriving mid-game makes the world allocate.
    // `world` is what a guest needs to generate terrain for this world, and
    // what it is checked against before it is asked to. Leave its `version`
    // empty to run a session that never delegates anything.
    void open(const std::string& worldName, const std::string& hostName,
              const GeneratorId& world, u32 nowMs, SessionListener* listener);

    // **How this world is played.** Told to every guest in the Welcome, so it
    // has to be set before anyone joins -- which means after the world is open
    // and its settings have been read. A session that never sets them welcomes
    // guests into the first gamemode and Peaceful, which is what a world with
    // no settings file is anyway. See `WorldRules`.
    void setWorldRules(const WorldRules& rules);
    const WorldRules& worldRules() const { return rules_; }

    // Tells everyone why and stops. Sends the message before it goes, which is
    // what stops a guest sitting on a ten-second timeout for a host that quit
    // in front of them.
    void close(const std::string& reason, u32 nowMs, Datagrams& datagrams);

    bool isOpen() const { return open_; }

    // Carries the session forward: reads whatever arrived, answers it, and
    // puts this end's own traffic on the wire. Called once a frame.
    void pump(u32 nowMs, Datagrams& datagrams);

    // **This console is about to stop answering, on purpose.** Sent to every
    // guest and put on the wire before this returns, because the thing it is
    // warning about happens on the next line: a library applet suspends the
    // whole application, so there is no later. See `Msg::Away`.
    void announceAway(u32 ms, u32 nowMs, Datagrams& datagrams);

    // A line the host typed, or one the game generated. Reaches every guest.
    void say(const std::string& text);

    // Where the host's own player is, this tick. Unreliable by design: the
    // next one is along in a tick and is worth more than this one resent.
    void reportPose(const Pose& pose);

    // A protocol-2 packet for one guest, or for all of them when `playerId` is
    // zero. The seam described at the top of this file.
    bool sendGamePacket(u8 playerId, const u8* data, usize size);

    // **Asks somebody to make a column.** The guest chosen is the one with the
    // shortest round trip that is still able to generate -- "quickest
    // responding", measured rather than guessed, because every datagram
    // already carries the acknowledgements `Peer::rttMs` is built from. False
    // when nobody in the session can.
    bool requestTerrain(const u8* body, usize size);

    // Whether any guest is able to generate terrain for this world.
    bool anyGenerator() const;

    int guestCount() const;

    // The names in the session, host first, for a screen that lists them.
    void players(std::vector<Player>* out) const;

    // The worst round trip to any guest, which is the number worth showing:
    // it is what the slowest player in the room is living with.
    u32 worstRttMs() const;

private:
    struct Guest {
        Peer peer;
        u16 node = 0;
        u8 playerId = 0;
        bool welcomed = false;
        // Their build agrees with ours about what this world generates, so
        // they may be asked for terrain. A guest that said otherwise plays
        // normally and is simply never asked.
        bool generates = false;
        // The rules changed and this guest has not been told yet. The window
        // can be full at the moment the pause menu closes, and a change that
        // was dropped there would leave one player flying in a Survival world
        // with nothing to say so. Retried every pump until it goes.
        bool rulesOwed = false;
        std::string name;
        u32 firstHeardMs = 0;
    };

    // Time this console spent suspended, not counted against anybody. See
    // `Peer::forgive` and `kStallMs`.
    void forgiveStall(u32 nowMs);

    Guest* guestForNode(u16 node, u32 nowMs);
    void drop(Guest& guest, const std::string& reason, u32 nowMs, Datagrams& datagrams);
    void onMessage(Guest& guest, Msg kind, const u8* body, usize size);
    void tellOthers(const Guest& about, Msg kind, const std::vector<u8>& body);

    static void sink(void* ctx, Msg kind, const u8* body, usize size);

    std::unique_ptr<Guest> guests_[kMaxGuests];
    SessionListener* listener_ = nullptr;
    std::string worldName_;
    std::string hostName_;
    GeneratorId world_;
    WorldRules rules_;

    // The frame being read and the body being built, kept between pumps: this
    // runs in the game loop, where neither a kilobyte of stack nor an
    // allocation per pose belongs.
    std::vector<u8> inbox_;
    std::vector<u8> body_;
    bool open_ = false;

    // Set around `Peer::receive` so the sink knows whose message it is.
    Guest* current_ = nullptr;
    u32 nowMs_ = 0;
    // When `pump` last ran, so a gap it did not see can be spotted.
    u32 pumpedAtMs_ = 0;
    bool currentBroken_ = false;
    std::string currentReason_;
};

// ---------------------------------------------------------------------------

// The console that joined one.
class GuestSession {
public:
    enum class State : u8 {
        Idle,
        // The Hello is on the wire and this end is waiting for an answer.
        Joining,
        Playing,
        // The host said no, or stopped answering, or this end left. `reason`
        // is what to put on the disconnected screen.
        Finished,
    };

    GuestSession();
    ~GuestSession();

    GuestSession(const GuestSession&) = delete;
    GuestSession& operator=(const GuestSession&) = delete;

    // Sends the Hello. `name` is what the host lists this console as.
    // `world` is this console's own generator identity, so the host can tell
    // whether it may be asked to make terrain.
    void join(const std::string& name, const GeneratorId& world, u32 nowMs,
              SessionListener* listener);

    // Leaves on purpose, telling the host first.
    void leave(const std::string& reason, u32 nowMs, Datagrams& datagrams);

    void pump(u32 nowMs, Datagrams& datagrams);

    State state() const { return state_; }
    const std::string& reason() const { return reason_; }
    const std::string& worldName() const { return worldName_; }
    const std::string& hostName() const { return hostName_; }
    u8 playerId() const { return playerId_; }
    u32 rttMs() const { return peer_.rttMs(); }

    // **This console is about to stop answering, on purpose.** See
    // `HostSession::announceAway`, which is the same warning the other way.
    void announceAway(u32 ms, u32 nowMs, Datagrams& datagrams);

    void say(const std::string& text);
    void reportPose(const Pose& pose);
    bool sendGamePacket(const u8* data, usize size);

    // One part of a terrain column, on its way back to the host.
    bool sendTerrainPart(const u8* body, usize size);

    // What the host said its world generates, once the Welcome has arrived.
    // An empty `version` means it is not asking anyone for terrain.
    const GeneratorId& world() const { return world_; }

    // **How the host's world is played**, once the Welcome has arrived. The
    // joining console adopts these rather than its own: see `WorldRules`.
    const WorldRules& worldRules() const { return rules_; }

    // Everyone the host has named, this console included.
    const std::vector<Player>& players() const { return players_; }

private:
    void finish(const std::string& reason);
    void forgiveStall(u32 nowMs);
    void onMessage(Msg kind, const u8* body, usize size);
    static void sink(void* ctx, Msg kind, const u8* body, usize size);

    Peer peer_;
    std::vector<u8> inbox_;
    std::vector<u8> body_;
    SessionListener* listener_ = nullptr;
    u32 nowMs_ = 0;
    // When `pump` last ran. See `HostSession::pumpedAtMs_`.
    u32 pumpedAtMs_ = 0;
    State state_ = State::Idle;
    std::string name_;
    std::string reason_;
    std::string worldName_;
    std::string hostName_;
    // What the host said its world generates, and what this console can.
    GeneratorId world_;
    GeneratorId own_;
    WorldRules rules_;
    std::vector<Player> players_;
    u8 playerId_ = 0;
    u32 joinedAtMs_ = 0;
    bool broken_ = false;
};

}  // namespace mc::net::link
