#pragma once

// The one socket an online session lives on, over the BSD calls both targets
// share.
//
// **One socket for everything, and that is not a saving.** The public address a
// NAT hands out belongs to the socket that sent through it, so the control
// messages, the punch probes and the game itself all have to leave by the same
// door -- a second socket would be a second mapping, and the address the server
// reported would be for the wrong one. See core/net/ac_link.hpp.
//
// **Nothing here blocks.** `open` binds and does not connect; `send` and
// `receive` return immediately. The one thing that would block is turning a
// host name into an address, which happens once, before this is opened, on
// somebody else's thread -- see `resolveHostAddress` in core/net/tcp_socket.hpp.

#include "core/net/ac_client.hpp"
#include "core/net/ac_wire.hpp"
#include "core/util/types.hpp"

#include <string>

namespace mc::net {

class UdpSocket : public ac::UdpTransport {
public:
    UdpSocket() = default;
    ~UdpSocket() override;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds a port and works out this console's own address, which is what is
    // offered to the server as the local candidate.
    //
    // **`bindAddress` is the platform's answer and not a default**, in host
    // byte order. A 3DS binds its own address, because `SOCU:Bind` refuses
    // `INADDR_ANY` -- libctru's own sockets example uses `gethostid()` and that
    // is not decoration. The host build passes 0 and gets the every-interface
    // bind that means on a PC.
    //
    // `server` is used only to work out which of several local addresses is the
    // one facing it, and only when `bindAddress` did not already say.
    // `*error` is in words a player can act on.
    bool open(const ac::Endpoint& server, u32 bindAddress, std::string* error);

    void close();
    bool isOpen() const { return fd_ >= 0; }

    bool send(const ac::Endpoint& to, const u8* data, usize size) override;
    bool receive(u8* buffer, usize capacity, usize* size, ac::Endpoint* from) override;
    ac::Endpoint localEndpoint() const override { return local_; }

    // Datagrams this console could not put on the wire at all -- the send
    // buffer was full, which on a console is the one loss the link layer cannot
    // tell from a bad connection. For the debug page.
    u32 sendFailures() const { return sendFailures_; }

private:
    int fd_ = -1;
    ac::Endpoint local_;
    u32 sendFailures_ = 0;
};

}  // namespace mc::net
