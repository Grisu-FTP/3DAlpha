#pragma once

// A non-blocking TCP connection over the BSD socket API, which is the one
// network interface both targets share.
//
// The 3DS gets its sockets from the SOC service, and libctru puts the usual
// `socket`/`connect`/`poll`/`getaddrinfo` names over it; the host has them
// natively. So this is written once against POSIX and nothing in it knows which
// it is running on. What *does* differ -- SOC has to be started with a buffer
// of its own before the first call here -- is the platform's job, before a
// session is started.
//
// Every call returns rather than blocking longer than it was told to, because
// the only thread that calls it is the session's, and that thread has to notice
// when it is asked to stop.

#include "core/util/types.hpp"

#include <string>

namespace mc::net {

class TcpSocket {
public:
    enum class Status {
        Ok,
        WouldBlock,  // nothing to read, or no room to write, right now
        Closed,      // the peer closed its end
        Error,
    };

    TcpSocket() = default;
    ~TcpSocket() { close(); }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    // Resolves `host` and connects, giving up after `timeoutMs`. On failure
    // `*error` says why in words a player can act on.
    bool connect(const char* host, u16 port, int timeoutMs, std::string* error);

    Status receive(u8* buffer, usize capacity, usize* received);
    Status send(const u8* data, usize size, usize* sent);

    // Waits up to `timeoutMs` for the socket to have something to read, or --
    // when `wantWrite` -- room to write. False only on an error.
    bool wait(bool wantWrite, int timeoutMs, bool* readable, bool* writable);

    void close();
    bool isOpen() const { return fd_ >= 0; }

    // The last `errno` in words and in numbers, for a caller reporting a failed
    // read or write. The number is there because the console's `strerror` does
    // not know every value its socket service produces.
    static std::string lastError(const char* what);

private:
    int fd_ = -1;
};

// **A host name into an IPv4 address, through everything this console has.**
// `*address` comes back in host byte order.
//
// Shared with core/net/udp_socket.hpp rather than written twice: the four-step
// cascade below it -- numeric, `getaddrinfo`, `gethostbyname`, then a DNS query
// of our own to the console's own name servers -- exists because the first
// hardware report of multiplayer past the loopback was that a numeric address
// connected and a name never did. A second copy of that would be a second thing
// to get wrong.
//
// **It blocks**, for as long as a name lookup takes, which on a console with a
// slow router is seconds. No frame loop may call it; the callers that need a
// name put this on a worker. See platform/ctr/online.hpp.
bool resolveHostAddress(const char* host, u32* address, std::string* error);

}  // namespace mc::net
