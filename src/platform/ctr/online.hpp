#pragma once

// Everything this console needs to be *on* AlphaComputer, in one object the
// menu can hold: its identity, its socket, the login, and the link a session
// runs over once two consoles have been introduced.
//
// **It does not exist until somebody asks for it.** `Menu` holds a null pointer
// until the player opens the Profile screen or picks Internet, and a game that
// is never played online never constructs one, never opens a socket, never
// resolves a name and never waits for anything. That is the rule this file is
// written to keep: single player does not pay for multiplayer, not in a
// millisecond and not in a frame.
//
// **Nothing here blocks the frame either.** The one call that would -- turning
// `ac.grisu-ftp.de` into an address, which on a console with a slow router is
// seconds -- happens on a worker, and the screen says what it is doing while it
// runs. Everything after that is a non-blocking socket pumped once a frame.

#include "core/net/ac_client.hpp"
#include "core/net/ac_identity.hpp"
#include "core/net/ac_link.hpp"
#include "core/net/udp_socket.hpp"
#include "core/util/types.hpp"
#include "platform/ctr/session_link.hpp"

#include <memory>
#include <string>

namespace mc::ctr {

// **It is the session's link as well as the login**, rather than handing one
// out: the two are the same socket and the same frame of work, and splitting
// them would mean the game loop pumping one of them and the menu the other.
class Online : public SessionLink {
public:
    Online();
    ~Online() override;

    Online(const Online&) = delete;
    Online& operator=(const Online&) = delete;

    // Where the login has got to, in the words the Profile screen prints.
    enum class Stage {
        Off,
        // The name is being looked up on a worker. The only slow step, and the
        // one a player is told about.
        Resolving,
        // **The name answered and the console has no address of its own yet.**
        // Not an error: an address arrives with the association and with DHCP,
        // and the Profile screen can easily be opened in the second before it
        // does. Waited out rather than refused, with a bound.
        WaitingForAddress,
        Connecting,
        Ready,
        Failed,
    };

    // Brings up SOC, reads or makes the identity key, and starts the lookup.
    // Calling it again with the same URL does nothing; with a different one it
    // starts over. `hosting` only decides how peers are numbered and can be
    // changed later with `setHosting`.
    void start(const std::string& serverUrl, bool hosting);

    // Everything down, socket included. Tells the server the session is closing
    // if there is one open.
    void stop();

    // Once a frame, from whichever loop is running -- the menu's or the game's.
    void pump(u32 nowMs);

    void setHosting(bool hosting);

    Stage stage() const { return stage_; }
    bool ready() const { return stage_ == Stage::Ready; }

    // What to put on screen: the server's own words for a refusal, or this
    // build's for a failure that never reached it.
    const std::string& message() const { return message_; }

    // The host name the URL resolved to, for the line that says where this is
    // connecting.
    const std::string& host() const { return host_; }

    // The address that name resolved to, host byte order; 0 before the lookup
    // has answered. World sharing connects to it rather than looking the name
    // up a second time.
    u32 serverAddress() const { return serverAddress_; }

    // This console's identity string, and whether its key was made just now.
    const std::string& identity() const { return identity_.name; }
    bool keyIsNew() const { return identity_.created; }

    net::ac::Client& client() { return connection_.client(); }
    const net::ac::Client& client() const { return connection_.client(); }
    net::ac::Connection& connection() { return connection_; }
    SessionLink& link() { return *this; }

    // ---- SessionLink -------------------------------------------------------

    // Ends the session *and* the login: a console that has left a world has no
    // reason to keep a socket open, and the next screen that wants one starts
    // over in a second.
    void leave() override;
    bool active() const override;
    void service(u32 nowMs) override;
    void notePlaying(u32 nowMs) override;
    u32 sendOverflows() const override;
    bool send(u16 node, const u8* data, usize size) override;
    bool receive(u8* buffer, usize capacity, usize* size, u16* node) override;

    // The event the connection has for the screen, with the introduction ones
    // already acted on.
    net::ac::Event takeEvent();

private:
    // The name lookup, and the little that crosses between the two threads.
    struct Lookup;

    static void resolveEntry(void* arg);
    void beginLookup();
    void finishLookup();

    // Opens the socket and starts the login, once the console has an address to
    // bind. False while it still has none.
    bool openSocket(u32 nowMs);
    void fail(const std::string& reason);

    net::ac::Connection connection_;
    net::UdpSocket socket_;

    net::ac::Identity identity_;
    std::string serverUrl_;
    std::string host_;
    u16 port_ = 0;
    bool hosting_ = false;

    Stage stage_ = Stage::Off;
    std::string message_;

    std::unique_ptr<Lookup> lookup_;
    void* lookupThread_ = nullptr;
    // In `WaitingForAddress` before the lookup rather than after it: the
    // console had no address when `start` was called, so the name is looked
    // up once it does.
    bool lookupPending_ = false;

    // The answer the lookup gave, kept because the socket may not be openable
    // for another second or two after it arrives.
    u32 serverAddress_ = 0;
    u32 waitingSinceMs_ = 0;
};

}  // namespace mc::ctr
