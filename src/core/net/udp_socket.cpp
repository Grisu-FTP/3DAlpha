// POSIX UDP for both targets. See udp_socket.hpp.

#include "core/net/udp_socket.hpp"

#include "core/net/tcp_socket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace mc::net {

namespace {

bool setNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void toSockaddr(const ac::Endpoint& endpoint, sockaddr_in* out)
{
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(endpoint.port);
    std::memcpy(&out->sin_addr, endpoint.addr, 4);
}

ac::Endpoint fromSockaddr(const sockaddr_in& address)
{
    ac::Endpoint out;
    out.v6 = false;
    std::memcpy(out.addr, &address.sin_addr, 4);
    out.port = ntohs(address.sin_port);
    return out;
}

// **Which of this console's addresses the peer would see.** Asked by connecting
// a throwaway datagram socket at the server and reading back what the routing
// table chose; no packet is sent, because connecting a UDP socket only fixes
// the destination. A console with two interfaces answers with the one the
// server is actually reachable through, which is the only one worth offering as
// a local candidate.
bool localAddressToward(const ac::Endpoint& server, u32* address)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_in to{};
    toSockaddr(server, &to);
    bool got = false;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in mine{};
        socklen_t length = sizeof(mine);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&mine), &length) == 0) {
            *address = ntohl(mine.sin_addr.s_addr);
            got = *address != 0;
        }
    }
    ::close(fd);
    return got;
}

// How many ports to offer before giving up, and where the fixed ones start.
//
// **The first attempt is port 0**, which is what every other platform answers
// with an ephemeral port. A 3DS may not: `SOCU:Bind` is stricter than the BSD
// call libctru wraps it in, and nothing in this tree can test which way it goes
// without a console. So the fixed ports below are the fallback, chosen from the
// IANA dynamic range where nothing else is registered.
constexpr int kBindAttempts = 6;
constexpr u16 kFirstFixedPort = 49730;

}  // namespace

UdpSocket::~UdpSocket()
{
    close();
}

bool UdpSocket::open(const ac::Endpoint& server, u32 bindAddress, std::string* error)
{
    close();

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        *error = TcpSocket::lastError("socket()");
        return false;
    }

    // **Not connected**: this socket talks to the server, to a peer and
    // possibly to a relay, and a connected UDP socket would refuse two of the
    // three.
    //
    // **The port is asked for and not assumed.** A 3DS refuses more of this
    // than a PC does, so the ports below are tried in order and the first one
    // the service accepts wins: the ephemeral one every other platform gives
    // out for free, then a handful of fixed ones. Which of the two a console
    // takes is not something the tests can answer -- see the comment on the
    // list -- so both are offered rather than one being guessed at.
    u16 port = 0;
    bool bound = false;
    std::string firstFailure;
    for (int attempt = 0; attempt < kBindAttempts && !bound; ++attempt) {
        const u16 wanted = attempt == 0 ? u16(0) : u16(kFirstFixedPort + attempt - 1);
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(bindAddress);
        local.sin_port = htons(wanted);
        if (::bind(fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0) {
            bound = true;
            port = wanted;
            break;
        }
        if (firstFailure.empty()) {
            firstFailure = TcpSocket::lastError("bind()");
        }
    }

    if (!bound) {
        // **What this almost always is.** `SOCU:Bind` fails when the console
        // has no address of its own yet, which is the wireless being off, an
        // association that has not finished, or a DHCP lease that never
        // arrived -- and it reports that as a plain invalid argument. Saying so
        // is worth more to a player than the errno, which is kept for whoever
        // reads a log.
        *error = bindAddress == 0
                     ? "Could not open a network port. " + firstFailure
                     : "Could not open a network port. Check the wireless switch and that "
                       "the console has joined a network. (" + firstFailure + ")";
        ::close(fd);
        return false;
    }

    fd_ = fd;

    if (!setNonBlocking(fd_)) {
        *error = TcpSocket::lastError("fcntl()");
        close();
        return false;
    }

    // The port the stack chose, when it was asked to choose. When a port was
    // named there is nothing to ask about, which matters on a console whose
    // `getsockname` is one more service call that can decline.
    if (port == 0) {
        sockaddr_in chosen{};
        socklen_t length = sizeof(chosen);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&chosen), &length) == 0) {
            port = ntohs(chosen.sin_port);
        }
    }

    // The local candidate. Absent is not a failure: the server simply has one
    // fewer address to offer, and two consoles in the same room fall back to
    // going out to the internet and back.
    local_ = ac::Endpoint();
    if (port != 0) {
        if (bindAddress != 0) {
            // Already known -- it is what was bound, and asking the routing
            // table again would be a second answer to a settled question.
            local_ = ac::endpointV4(bindAddress, port);
        } else {
            u32 address = 0;
            if (localAddressToward(server, &address)) {
                local_ = ac::endpointV4(address, port);
            }
        }
    }
    return true;
}

void UdpSocket::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    local_ = ac::Endpoint();
}

bool UdpSocket::send(const ac::Endpoint& to, const u8* data, usize size)
{
    if (fd_ < 0 || to.v6) {
        // No v6 here yet: the wire carries the addresses and this socket does
        // not, which is a gap worth being explicit about rather than one that
        // shows up as a silent failure to connect.
        return false;
    }
    sockaddr_in address{};
    toSockaddr(to, &address);
    const ssize_t sent = ::sendto(fd_, data, size, 0,
                                  reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (sent < 0) {
        ++sendFailures_;
        return false;
    }
    return true;
}

bool UdpSocket::receive(u8* buffer, usize capacity, usize* size, ac::Endpoint* from)
{
    if (fd_ < 0) {
        return false;
    }
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    const ssize_t got = ::recvfrom(fd_, buffer, capacity, 0,
                                   reinterpret_cast<sockaddr*>(&address), &length);
    if (got < 0) {
        return false;
    }
    // A datagram from something that is not IPv4 is not one of ours.
    if (length < socklen_t(sizeof(address)) || address.sin_family != AF_INET) {
        return false;
    }
    *size = usize(got);
    *from = fromSockaddr(address);
    return true;
}

}  // namespace mc::net
