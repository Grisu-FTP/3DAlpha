#pragma once

// The console's side of AlphaComputer: logging in, staying logged in, opening a
// session, finding one, and the two account errands the Profile screen runs.
//
// **Nothing here runs unless somebody asks for it.** No login happens at boot,
// none happens when a world is opened, and none happens on the way to the title
// screen. A player who never touches multiplayer never sends a packet and never
// waits for one: `begin` is called from the Profile screen and from the
// Internet rows of the multiplayer menu, and from nowhere else. That is a
// requirement and not an optimisation -- a single-player game that pauses for a
// server it does not need is a worse game than one with no server at all.
//
// **It never blocks either.** Every call returns immediately; `pump` reads
// whatever the transport already has and sends whatever is due. The one thing
// that would block -- turning a host name into an address -- happens before
// this object is given an `Endpoint`, on the caller's own terms.
//
// **It does not own the socket.** The same socket carries the control messages
// here, the punch probes, and afterwards the game itself, because the public
// address a NAT hands out belongs to the socket that spoke through it. So the
// transport is borrowed, `onDatagram` is fed only what came from the server's
// address, and core/net/ac_link.hpp is the thing that does the sorting.

#include "core/net/ac_wire.hpp"
#include "core/util/types.hpp"

#include <string>
#include <vector>

namespace mc::net::ac {

// The socket, as core is willing to see one: two calls that never block and an
// address this console believes it has. One implementation is a BSD socket, the
// other is the test's.
class UdpTransport {
public:
    virtual ~UdpTransport() = default;

    // False when the datagram could not be handed to the stack at all, which is
    // a broken socket rather than a lost packet -- a packet that is merely lost
    // returns true here and is noticed by its answer never arriving.
    virtual bool send(const Endpoint& to, const u8* data, usize size) = 0;

    // The next datagram, or false when there is none waiting. Never blocks.
    virtual bool receive(u8* buffer, usize capacity, usize* size, Endpoint* from) = 0;

    // This console's own address on its own network, which is offered to the
    // server as the local candidate so that two consoles in one room talk
    // across it. Invalid when the platform cannot say.
    virtual Endpoint localEndpoint() const = 0;
};

// How often a logged-in console says it is still there. Two jobs, and the
// second is the one that matters: holding the NAT mapping open, and letting the
// server re-learn the address when the mapping moves -- which it does every
// time a lid closes and opens.
inline constexpr u32 kKeepAliveMs = 10000;

// An unanswered Hello, Auth or request is sent again after this. A 3DS on
// hotel wireless is not fast; this is long enough not to double up on an answer
// that is merely late.
inline constexpr u32 kRetryMs = 900;

// How long a login or a request may go unanswered before it is given up on and
// the screen is told. Six seconds is about as long as anybody will watch a
// menu that says nothing.
inline constexpr u32 kRequestTimeoutMs = 6000;

// **A gap between two pumps longer than this is a suspension, not silence.** A
// keyboard applet or the HOME menu stops the frame loop, and nothing is read
// while it is stopped; a hitch this long in a running loop is already a bug of
// its own. See `Client::pump`.
inline constexpr u32 kSuspendGapMs = 2000;

// **A server that forgets this console again this soon after a re-login is not
// forgetting it by accident**, and a second re-login would only loop. See the
// AuthFail branch of `Client::onDatagram`.
inline constexpr u32 kReloginGuardMs = 10000;

// No datagram from the server for this long and the session is treated as gone.
// Longer than three keep-alives, so a single dropped one costs nothing.
inline constexpr u32 kServerTimeoutMs = 45000;

// Where the login has got to. The Profile screen draws one line per state, so
// the names are the words a player reads.
enum class State {
    // `begin` has not been called. The ordinary state of this object in a
    // single-player session, and the reason it costs nothing to exist.
    Idle,
    Connecting,
    // Logged in: the token is good, the display name is known, and the account
    // handle is known to be present or absent.
    Ready,
    // The server said no, or never answered. `message` says which.
    Failed,
};

// What the last errand did, so a screen can say something without inspecting a
// message kind.
enum class Event : u8 {
    None,
    LoggedIn,
    LinkCodeIssued,
    Unlinked,
    SessionOpened,
    SessionsListed,
    // The server has introduced this console to another one. Both ends get this
    // at the same moment; core/net/ac_link.hpp does the firing.
    PunchNow,
    RelayAllocated,
    // The server refused something. `message()` is its own words.
    Refused,
};

struct Login {
    // `3ds:000123456789`. See core/net/ac_identity.hpp for where it comes from
    // and why a friend code is an identifier and not a credential.
    std::string identity;
    u8 seed[32] = {};
    u8 publicKey[kPublicKeySize] = {};

    // The friend list's screen name, which the server keeps as the fallback
    // display name. Absent is ordinary: `FRD_GetMyScreenName` fails outright on
    // a console whose friend service was never set up.
    bool hasPlatformName = false;
    std::string platformName;
};

class Client {
public:
    Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Starts the login. Cheap: it queues a Hello and returns.
    void begin(const Endpoint& server, const Login& login, UdpTransport* transport, u32 nowMs);

    // Back to Idle, sending nothing. The caller that is closing a session
    // should close it first -- see `closeSession`.
    void end();

    // One frame's worth: re-sends what is due, expires what has waited too
    // long, and sends a keep-alive when one is owed. Datagrams arrive through
    // `onDatagram` rather than being read here, because the socket is shared.
    void pump(u32 nowMs);

    // A datagram that came from the server's address. False when it was not a
    // well-formed message -- worth counting, not worth acting on.
    bool onDatagram(const u8* data, usize size, u32 nowMs);

    State state() const { return state_; }
    bool ready() const { return state_ == State::Ready; }

    // Whatever should be on screen: a refusal, a failure, or nothing.
    const std::string& message() const { return message_; }

    // The name this console plays under, as the server resolves it: the account
    // name once linked, the friend-list name with a tag before that.
    const std::string& displayName() const { return displayName_; }

    // Empty when this console is not linked to an account, which is what the
    // Profile screen's button reads to decide between Link and Unlink.
    const std::string& accountHandle() const { return accountHandle_; }

    // Shown once, on the login that created the binding, so a player has
    // something to compare against if the identity is ever disputed.
    bool firstClaim() const { return firstClaim_; }
    const std::string& keyFingerprint() const { return keyFingerprint_; }

    // This console's public address, as the server saw it. Refreshed on every
    // keep-alive answer, so it follows a NAT that rebinds.
    const Endpoint& reflexive() const { return reflexive_; }

    // The errand that just finished, taken once. Everything a screen reacts to
    // arrives through here.
    Event takeEvent();

    // ---- the errands -------------------------------------------------------

    // Eight characters to type into the website, and how long they last.
    void requestLinkCode();
    const std::string& linkCode() const { return linkCode_; }
    u16 linkCodeSeconds() const { return linkCodeSeconds_; }

    // Leaves the account this console is on. Protocol 2.
    void unlink();

    // Opens a session others can join. `locked` keeps it out of the public
    // browser, which is this build's default: a world is joined with the six
    // characters its host reads out, not walked into by strangers.
    void hostSession(const std::string& worldName, u8 maxPlayers, bool locked);
    void closeSession();
    u64 sessionId() const { return sessionId_; }
    const std::string& joinCode() const { return joinCode_; }

    void listSessions();
    const std::vector<SessionInfo>& sessions() const { return sessions_; }

    // By the code a player typed, or by a row of the list.
    void joinByCode(const std::string& code);
    void joinById(u64 sessionId);

    // Whether the punch worked. **The server has to be told**: it cannot see a
    // link it is not part of, and a failure is what allocates the relay.
    void reportPunch(u64 sessionId, bool connected);

    // Set by the PunchNow and RelayAllocated events, and read by
    // core/net/ac_link.hpp, which is what does something with them.
    const ServerMsg& introduction() const { return introduction_; }

private:
    // What is waiting for an answer. One at a time: the server answers each
    // message it is sent, and a console with two errands in flight cannot tell
    // which `Error` belongs to which.
    enum class Pending : u8 {
        None,
        Hello,
        Auth,
        LinkCode,
        Unlink,
        Host,
        Close,
        List,
        Join,
    };

    void sendPending(u32 nowMs);
    void startRequest(Pending pending, u32 nowMs);
    void fail(const std::string& reason);
    bool transmit(const ClientMsg& msg);

    // A request that arrives while the login is still going is kept until the
    // login lands rather than being refused: the Profile screen's Link button
    // is pressed on the frame the screen opens, which is well before a 3DS on
    // wireless has finished saying hello.
    void queue(Pending pending);

    UdpTransport* transport_ = nullptr;
    Endpoint server_;
    Login login_;

    State state_ = State::Idle;
    std::string message_;

    u8 token_[kTokenSize] = {};
    u8 nonce_[kNonceSize] = {};
    std::string displayName_;
    std::string accountHandle_;
    std::string keyFingerprint_;
    bool firstClaim_ = false;
    Endpoint reflexive_;

    Pending pending_ = Pending::None;
    Pending queued_ = Pending::None;
    u32 sentAtMs_ = 0;
    u32 startedAtMs_ = 0;
    u32 lastHeardMs_ = 0;
    u32 keepAliveAtMs_ = 0;
    // The time the last `pump` was given: this object's idea of "now" between
    // pumps, which is when a request asked for in between is started.
    u32 clockMs_ = 0;

    // **The handshake's retransmissions, and the refusals they cause.** The
    // server answers each Auth once: the first to land takes the challenge and
    // gets AuthOk, and every copy after it gets "say hello first". A second
    // Hello likewise replaces the challenge the first one was answered with, so
    // an Auth signed over the first nonce is refused. Neither refusal means the
    // console may not log in -- both mean a datagram was slow -- so they are
    // counted here rather than believed. See `onDatagram`'s AuthFail.
    int helloSends_ = 0;
    int authSends_ = 0;
    bool handshakeRestarted_ = false;
    int staleAuthFails_ = 0;
    u32 staleAuthUntilMs_ = 0;

    // When the last automatic re-login began, for `kReloginGuardMs`.
    bool relogged_ = false;
    u32 reloggedAtMs_ = 0;

    Event event_ = Event::None;

    std::string linkCode_;
    u16 linkCodeSeconds_ = 0;

    std::string hostWorldName_;
    u8 hostMaxPlayers_ = 0;
    bool hostLocked_ = true;
    u64 sessionId_ = 0;
    std::string joinCode_;

    std::vector<SessionInfo> sessions_;

    std::string joinCodeWanted_;
    u64 joinIdWanted_ = 0;
    bool joinByCode_ = false;

    ServerMsg introduction_;

    std::vector<u8> scratch_;
};

}  // namespace mc::net::ac
