// POSIX TCP for both targets. See tcp_socket.hpp.

#include "core/net/tcp_socket.hpp"

#include "core/net/dns.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace mc::net {

namespace {

bool setNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool wouldBlock(int error)
{
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
}

// **Always the number, and the words only if there are any.** `strerror` is
// newlib's on the console and the socket service's errno values are not all in
// the table it knows, so a failure reported through it alone can arrive as an
// empty string -- which is how a disconnect screen ends up saying nothing at
// all. The number is the part that can be looked up.
std::string describeErrno(const char* what, int error)
{
    const char* words = std::strerror(error);
    char text[128];
    if (words != nullptr && words[0] != '\0') {
        std::snprintf(text, sizeof(text), "%s: %s (errno %d)", what, words, error);
    } else {
        std::snprintf(text, sizeof(text), "%s: errno %d", what, error);
    }
    return text;
}

// **Four ways to turn a name into an address, and a name is tried against all
// of them.**
//
// A numeric address needs no resolver at all, and that is worth insisting on:
// the player who types their PC's address on a touch keyboard should not be
// depending on the console socket service to hand it back.
//
// A *name* used to depend on exactly one call -- `gethostbyname`, which is
// SOC's `GetHostByName` underneath -- on the argument that it is what has
// always worked there. The first hardware report of multiplayer past the
// loopback was **DNS does not work**: a numeric address connected and a name
// never did. So the name now goes through everything the console has, in order
// of how much of it is somebody else's code:
//
//   1. `getaddrinfo`, SOC's other resolver, which is what most 3DS homebrew
//      uses and what the host build answers on;
//   2. `gethostbyname`, SOC's first, kept because a service that answers one
//      and not the other is exactly the sort of thing this stack does;
//   3. **a DNS query of our own, to the console's own name servers** -- the
//      ones DHCP handed it, which on a home network is the router. See
//      core/net/dns.hpp; that is the answer to "can it not just use the 3DS's
//      or the router's DNS".
//
// Each is tried until one answers. The error names what was tried, because a
// player who can reach a numeric address and no name needs to be told which
// half is broken.
bool resolveHost(const char* host, u16 port, sockaddr_in* out, std::string* error)
{
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(port);

    u32 address = 0;
    if (!resolveHostAddress(host, &address, error)) {
        return false;
    }
    out->sin_addr.s_addr = htonl(address);
    return true;
}

}  // namespace

bool resolveHostAddress(const char* host, u32* address, std::string* error)
{
    in_addr numeric{};
    if (inet_aton(host, &numeric) != 0) {
        *address = ntohl(numeric.s_addr);
        return true;
    }

    // `getaddrinfo`, restricted to the one family this can use: an AAAA answer
    // is an address neither socket in this tree has a `sockaddr_in6` to put.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    // No socket type: this answer is used for a stream and for a datagram
    // alike, and an A record does not know the difference.
    hints.ai_socktype = 0;
    addrinfo* list = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &list) == 0 && list != nullptr) {
        bool got = false;
        for (const addrinfo* it = list; it != nullptr && !got; it = it->ai_next) {
            if (it->ai_family == AF_INET && it->ai_addr != nullptr
                && it->ai_addrlen >= socklen_t(sizeof(sockaddr_in))) {
                const sockaddr_in* in = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
                *address = ntohl(in->sin_addr.s_addr);
                got = true;
            }
        }
        ::freeaddrinfo(list);
        if (got) {
            return true;
        }
    }

    const hostent* found = gethostbyname(host);
    if (found != nullptr && found->h_addrtype == AF_INET && found->h_length == 4
        && found->h_addr_list != nullptr && found->h_addr_list[0] != nullptr) {
        in_addr first{};
        std::memcpy(&first, found->h_addr_list[0], 4);
        *address = ntohl(first.s_addr);
        return true;
    }

    // **Ours, and the last one because it is the only one that cannot be
    // blamed on somebody else.** Three seconds across every server the console
    // knows: long enough for a router that is slow and short enough that a
    // player does not think the game has hung.
    constexpr int kDnsTimeoutMs = 3000;
    u32 resolved = 0;
    if (resolveViaDns(host, kDnsTimeoutMs, &resolved)) {
        *address = resolved;
        return true;
    }

    u32 servers[kMaxNameServers];
    const int count = nameServers(servers, kMaxNameServers);
    if (count == 0) {
        *error = std::string("Could not look up '") + host
                 + "'. This console has no DNS server -- an address like 192.168.1.20 "
                   "still works.";
    } else {
        char first[24];
        std::snprintf(first, sizeof(first), "%u.%u.%u.%u", unsigned((servers[0] >> 24) & 0xFF),
                      unsigned((servers[0] >> 16) & 0xFF), unsigned((servers[0] >> 8) & 0xFF),
                      unsigned(servers[0] & 0xFF));
        *error = std::string("Could not look up '") + host + "'. The console's DNS (" + first
                 + ") did not answer; an address like 192.168.1.20 does not need it.";
    }
    return false;
}

bool TcpSocket::connect(const char* host, u16 port, int timeoutMs, std::string* error)
{
    close();

    sockaddr_in address{};
    if (!resolveHost(host, port, &address, error)) {
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        *error = describeErrno("socket()", errno);
        return false;
    }
    if (!setNonBlocking(fd)) {
        *error = "The socket could not be made non-blocking.";
        ::close(fd);
        return false;
    }

    int result = ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result != 0 && !wouldBlock(errno)) {
        *error = describeErrno("connect()", errno);
        ::close(fd);
        return false;
    }

    if (result != 0) {
        // **Whether a non-blocking connect finished is asked of the socket, not
        // of `SO_ERROR`.** `getpeername` succeeding means there is a peer on the
        // other end, which is the fact wanted and is not open to interpretation;
        // a socket that has not connected answers ENOTCONN.
        //
        // `SO_ERROR` used to be the arbiter here and it cost a working
        // connection on real hardware: the console accepted the socket, the
        // server logged the connection and then logged it lost, and the player
        // was told the connection had failed -- because SOC answered the
        // getsockopt with something non-zero on a socket that was perfectly
        // fine, and `strerror` of it was empty, so even the reason was blank.
        // It is only consulted now to explain a failure `poll` has reported,
        // never to overrule a connection that is demonstrably up.
        constexpr int kSliceMs = 100;
        bool connected = false;
        for (int waited = 0; waited < timeoutMs && !connected; waited += kSliceMs) {
            pollfd p{fd, POLLOUT, 0};
            const int ready = ::poll(&p, 1, kSliceMs);

            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            if (::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &length) == 0) {
                connected = true;
                break;
            }

            if (ready > 0 && (p.revents & (POLLERR | POLLHUP)) != 0) {
                int soError = 0;
                socklen_t size = sizeof(soError);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &size) == 0
                    && soError != 0) {
                    *error = describeErrno("connect", soError);
                } else {
                    *error = "The connection was refused or dropped while it was being made.";
                }
                ::close(fd);
                return false;
            }
        }
        if (!connected) {
            *error = "Connection timed out. Nothing answered at that address and port.";
            ::close(fd);
            return false;
        }
    }

    fd_ = fd;
    return true;
}

TcpSocket::Status TcpSocket::receive(u8* buffer, usize capacity, usize* received)
{
    *received = 0;
    if (fd_ < 0) {
        return Status::Error;
    }
    const ssize_t n = ::recv(fd_, buffer, capacity, 0);
    if (n > 0) {
        *received = usize(n);
        return Status::Ok;
    }
    if (n == 0) {
        return Status::Closed;
    }
    return wouldBlock(errno) ? Status::WouldBlock : Status::Error;
}

TcpSocket::Status TcpSocket::send(const u8* data, usize size, usize* sent)
{
    *sent = 0;
    if (fd_ < 0) {
        return Status::Error;
    }
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const ssize_t n = ::send(fd_, data, size, flags);
    if (n >= 0) {
        *sent = usize(n);
        return Status::Ok;
    }
    if (wouldBlock(errno)) {
        return Status::WouldBlock;
    }
    return errno == EPIPE || errno == ECONNRESET ? Status::Closed : Status::Error;
}

bool TcpSocket::wait(bool wantWrite, int timeoutMs, bool* readable, bool* writable)
{
    *readable = false;
    *writable = false;
    if (fd_ < 0) {
        return false;
    }
    pollfd p{fd_, short(POLLIN | (wantWrite ? POLLOUT : 0)), 0};
    const int result = ::poll(&p, 1, timeoutMs);
    if (result < 0) {
        return errno == EINTR;
    }
    // A hang-up is reported as readable, so the recv that follows finds the
    // end of the stream and says so.
    *readable = (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    *writable = (p.revents & POLLOUT) != 0;
    return true;
}

std::string TcpSocket::lastError(const char* what)
{
    return describeErrno(what, errno);
}

void TcpSocket::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace mc::net
