#pragma once

// The control protocol a console speaks to AlphaComputer, the rendezvous server
// that introduces two consoles so that neither has to forward a port.
//
// **This is not protocol 2 and it is not core/net/link.hpp either.** It carries
// no world: the largest message in it is a list of open sessions. Its whole job
// is to say who this console is, what address the server sees it at, and which
// other console to start firing packets at -- after which the server is out of
// the path and core/net/link.hpp takes over on the very same socket. That last
// detail is not an optimisation: the public address a NAT gives out belongs to
// the socket that sent the packet, so the game must go out of the one that said
// Hello or the mapping the punch opened is the wrong one.
//
// **Big-endian, one message per datagram, four bytes of magic in front.** UDP
// delivers a whole datagram or none of it, so there is no framing here and no
// length prefix; a short read is a broken sender rather than a partial message.
// The magic is what stops a stray packet on a public port being parsed at all.
//
// The shape is AlphaComputer's `src/wire/`, and the two must agree exactly --
// tests/ac_wire_test.cpp checks this encoder against bytes that Rust produced,
// rather than against itself. `kProtocol` is refused at `Hello` before any
// later field is read, so two builds that disagree stop rather than misreading
// each other.

#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::net::ac {

// Bumped whenever a message changes shape. 1 was the server's first cut; 2
// added Unlink, so that a console can leave an account from the console that
// joined it rather than only from the website; 3 added `transferPort` to
// AuthOk, which is where world sharing listens -- see core/net/world_share.hpp.
// 4 added StillPlaying/PlaytimeAck, which is how playtime is counted: the
// console says it is in a world and **the server decides what that is worth**.
inline constexpr u16 kProtocol = 4;

// "ACMP".
inline constexpr u8 kMagic[4] = {0x41, 0x43, 0x4d, 0x50};

// A control datagram is never large, and is well under any plausible path MTU
// so that none of these is ever fragmented.
inline constexpr usize kMaxDatagram = 1200;

inline constexpr usize kTokenSize = 16;
inline constexpr usize kNonceSize = 32;
inline constexpr usize kPublicKeySize = 32;
inline constexpr usize kSignatureSize = 64;

inline constexpr usize kJoinCodeLen = 6;
inline constexpr usize kLinkCodeLen = 8;

inline constexpr usize kMaxName = 32;
inline constexpr usize kMaxId = 64;
inline constexpr usize kMaxReason = 128;
inline constexpr int kMaxSessions = 16;
inline constexpr int kMaxCandidates = 4;

// The control port and the relay port a stock AlphaComputer listens on. The
// website is a different process behind a reverse proxy, which is why the
// profile setting is a URL and this is a number that does not appear in it.
inline constexpr u16 kDefaultPort = 7717;

// An address as this protocol writes one: a family byte, four or sixteen
// bytes, and a port. Kept as bytes rather than as the platform's `sockaddr` so
// that core never sees a socket header.
struct Endpoint {
    bool v6 = false;
    u8 addr[16] = {};
    u16 port = 0;

    // Zero is not an address and port 0 is not a port; either means "none".
    bool valid() const;
};

bool operator==(const Endpoint& a, const Endpoint& b);
inline bool operator!=(const Endpoint& a, const Endpoint& b) { return !(a == b); }

// `1.2.3.4:5678` or `[::1]:5678`, for a screen or a log.
std::string endpointText(const Endpoint& endpoint);

Endpoint endpointV4(u32 address, u16 port);

// Which of the two the server offered. The local one is tried first: two
// consoles on the same wireless network should talk across the room rather
// than out to the internet and back, which many home routers will not do at
// all.
enum class CandidateKind : u8 {
    Local = 0,
    Reflexive = 1,
};

struct Candidate {
    Endpoint endpoint;
    CandidateKind kind = CandidateKind::Reflexive;
};

// One row of the session browser. A directory entry, not a world.
struct SessionInfo {
    u64 id = 0;
    std::string joinCode;
    std::string hostName;
    std::string worldName;
    // Which port is hosting -- `3ds` today. Cross-play is allowed and a player
    // should still be able to see what they are joining.
    std::string game;
    u8 players = 0;
    u8 maxPlayers = 0;
    bool locked = false;
};

// ---------------------------------------------------------------------------
// What this console sends.
// ---------------------------------------------------------------------------

enum class ClientKind : u8 {
    Hello = 0x01,
    Auth = 0x03,
    Keepalive = 0x10,
    HostSession = 0x20,
    CloseSession = 0x22,
    ListSessions = 0x24,
    JoinRequest = 0x26,
    PunchResult = 0x28,
    RequestLinkCode = 0x2A,
    // Protocol 2. Leaving an account from the console that is on it; before
    // this the only way off was the website, which a player holding a 3DS does
    // not necessarily have to hand.
    Unlink = 0x2C,
    // Protocol 4. "I am in a world right now", which is a different claim from
    // Keepalive's "I am reachable": a console sitting on the online menu is
    // reachable for hours and has played none of them. Same shape as a
    // Keepalive -- the token and nothing else -- because the server needs no
    // more than to know which console said it and when.
    StillPlaying = 0x12,
};

// One struct with every field rather than a variant: the set is small, the
// build has no RTTI and no exceptions, and a flat struct is what the encoder
// switch wants anyway. Only the fields `kind` names are read.
struct ClientMsg {
    ClientKind kind = ClientKind::Keepalive;

    u16 protocol = kProtocol;
    std::string identity;
    u8 publicKey[kPublicKeySize] = {};

    u8 signature[kSignatureSize] = {};
    bool hasPlatformName = false;
    std::string platformName;

    u8 token[kTokenSize] = {};

    std::string worldName;
    std::string game;
    u8 maxPlayers = 0;
    bool locked = false;

    bool hasLocal = false;
    Endpoint local;

    bool hasGameFilter = false;

    bool hasJoinCode = false;
    std::string joinCode;
    u64 sessionId = 0;

    bool connected = false;
};

// Appends the whole datagram, magic and all. False when a field would not fit,
// which is a caller bug rather than a wire condition -- every message here is
// far inside `kMaxDatagram`.
bool encodeClient(const ClientMsg& msg, std::vector<u8>* out);

// ---------------------------------------------------------------------------
// What the server sends back.
// ---------------------------------------------------------------------------

enum class ServerKind : u8 {
    Challenge = 0x02,
    AuthOk = 0x04,
    AuthFail = 0x05,
    KeepaliveAck = 0x11,
    SessionOpened = 0x21,
    SessionList = 0x25,
    PunchNow = 0x27,
    RelayAllocated = 0x29,
    LinkCode = 0x2B,
    // Protocol 2, the answer to `Unlink`.
    Unlinked = 0x2D,
    // Protocol 4, the answer to `StillPlaying`.
    PlaytimeAck = 0x13,
    Error = 0xFF,
};

struct ServerMsg {
    ServerKind kind = ServerKind::Error;

    u8 nonce[kNonceSize] = {};
    Endpoint reflexive;

    u8 token[kTokenSize] = {};
    std::string displayName;
    bool hasAccount = false;
    std::string accountHandle;
    // True when this login *created* the binding rather than proving it. The
    // key fingerprint is worth showing once at that moment, so a player has
    // something to compare against if the identity is ever disputed.
    bool firstClaim = false;
    std::string keyFingerprint;
    // Protocol 3. The TCP port world sharing listens on, on the host this
    // console reached the server at; 0 when the server does not share worlds.
    u16 transferPort = 0;

    std::string reason;

    u64 sessionId = 0;
    std::string joinCode;

    std::vector<SessionInfo> sessions;

    std::string peerName;
    std::vector<Candidate> candidates;
    u8 punchToken[kTokenSize] = {};
    u16 windowMs = 0;

    Endpoint relay;

    std::string code;
    u16 expiresInS = 0;

    // Protocol 4. **The console does not decide either of these.** It says it
    // is playing and the server answers with what that ping bought and what
    // the running total is -- zero for the first ping of a stretch, and capped
    // after a gap too long to have been play, so a lid closed for an hour buys
    // half a minute. See AlphaComputer's `PLAYTIME_MAX_STEP`.
    u16 creditedS = 0;
    u32 totalS = 0;
};

// False for anything that is not a whole, well-formed message of a kind this
// build knows: no magic, a truncated field, a string longer than its ceiling, a
// list count past its ceiling, or trailing bytes. **Every one of those is a
// refusal and none of them is a crash** -- this parses datagrams that arrived
// from the open internet, and the count in a list never sizes an allocation
// before it has been checked.
bool decodeServer(const u8* data, usize size, ServerMsg* out);

// ---------------------------------------------------------------------------

// What a console signs to prove it holds the key bound to its identity:
// `"alphacomputer/auth/v1\0" || u16 length || identity || nonce`.
//
// **The identity is inside the payload on purpose.** Without it, an answer
// collected for one identity could be presented as the answer for another that
// shares a key. The domain string is there for the same kind of reason: this
// key will sign other things later, and a signature gathered for one purpose
// must not be replayable as another.
void authPayload(std::string_view identity, const u8 nonce[kNonceSize], std::vector<u8>* out);

}  // namespace mc::net::ac
