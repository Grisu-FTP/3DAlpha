// The A query and its answer. See dns.hpp.

#include "core/net/dns.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace mc::net {

namespace {

NameServerSource gSource = nullptr;
void* gSourceCtx = nullptr;

// RFC 1035's three limits, and they are the whole of what `buildDnsQuery`
// refuses.
constexpr usize kMaxLabel = 63;
constexpr usize kMaxName = 255;

constexpr usize kHeaderBytes = 12;
constexpr u16 kTypeA = 1;
constexpr u16 kClassIn = 1;

// Two bits set in the top of an offset mean "the rest of this name is at that
// offset" -- the compression pointer every answer uses for the name it is
// answering about.
constexpr u8 kPointerMask = 0xC0;

u16 readU16(const u8* p) { return u16((u16(p[0]) << 8) | p[1]); }

// Steps over a name at `pos`, compressed or not, and answers where it ends --
// or 0 for a malformed one. **It does not follow the pointer**: the caller
// only wants the bytes after the name, and following one is how a parser is
// made to loop forever by a hostile packet.
usize skipName(const u8* message, usize size, usize pos)
{
    while (pos < size) {
        const u8 length = message[pos];
        if ((length & kPointerMask) == kPointerMask) {
            // A pointer is two bytes and ends the name.
            return pos + 2 <= size ? pos + 2 : 0;
        }
        if (length == 0) {
            return pos + 1;
        }
        if ((length & kPointerMask) != 0) {
            return 0;  // a reserved length prefix
        }
        pos += usize(length) + 1;
    }
    return 0;
}

}  // namespace

void setNameServerSource(NameServerSource source, void* ctx)
{
    gSource = source;
    gSourceCtx = ctx;
}

int nameServers(u32* out, int max)
{
    if (gSource == nullptr || out == nullptr || max <= 0) {
        return 0;
    }
    const int found = gSource(gSourceCtx, out, max);
    return found < 0 ? 0 : (found > max ? max : found);
}

bool buildDnsQuery(std::string_view host, u16 id, u8* out, usize* size)
{
    if (out == nullptr || size == nullptr || host.empty() || host.size() > kMaxName) {
        return false;
    }

    usize at = 0;
    const auto put = [&](u8 byte) {
        if (at < kMaxDnsMessage) {
            out[at] = byte;
        }
        ++at;
    };

    put(u8(id >> 8));
    put(u8(id & 0xFF));
    // **Recursion desired, and nothing else.** A home router is a forwarder;
    // asking it to do the walk is the whole point of asking it rather than a
    // root server.
    put(0x01);
    put(0x00);
    put(0x00);
    put(0x01);  // one question
    for (int i = 0; i < 6; ++i) {
        put(0x00);  // no answers, no authority, no additional
    }

    // The name as length-prefixed labels. A trailing dot is a fully qualified
    // name and is written the same way -- the empty label it implies is the
    // terminator this appends anyway.
    usize start = 0;
    while (start <= host.size()) {
        usize dot = host.find('.', start);
        if (dot == std::string_view::npos) {
            dot = host.size();
        }
        const usize length = dot - start;
        if (length == 0) {
            if (dot == host.size()) {
                break;  // the trailing dot
            }
            return false;  // an empty label in the middle
        }
        if (length > kMaxLabel) {
            return false;
        }
        put(u8(length));
        for (usize i = 0; i < length; ++i) {
            put(u8(host[start + i]));
        }
        start = dot + 1;
    }
    put(0x00);

    put(u8(kTypeA >> 8));
    put(u8(kTypeA & 0xFF));
    put(u8(kClassIn >> 8));
    put(u8(kClassIn & 0xFF));

    if (at > kMaxDnsMessage) {
        return false;
    }
    *size = at;
    return true;
}

bool parseDnsAnswer(const u8* message, usize size, u16 id, u32* address)
{
    if (message == nullptr || address == nullptr || size < kHeaderBytes) {
        return false;
    }
    if (readU16(message) != id) {
        return false;
    }
    const u16 flags = readU16(message + 2);
    if ((flags & 0x8000) == 0) {
        return false;  // not a response
    }
    if ((flags & 0x000F) != 0) {
        return false;  // NXDOMAIN, SERVFAIL, REFUSED -- an answer, and a no
    }

    const u16 questions = readU16(message + 4);
    const u16 answers = readU16(message + 6);
    if (answers == 0) {
        return false;
    }

    usize pos = kHeaderBytes;
    for (u16 q = 0; q < questions; ++q) {
        pos = skipName(message, size, pos);
        if (pos == 0 || pos + 4 > size) {
            return false;
        }
        pos += 4;  // qtype and qclass
    }

    for (u16 a = 0; a < answers; ++a) {
        pos = skipName(message, size, pos);
        if (pos == 0 || pos + 10 > size) {
            return false;
        }
        const u16 type = readU16(message + pos);
        const u16 cls = readU16(message + pos + 2);
        const u16 length = readU16(message + pos + 8);
        pos += 10;
        if (pos + usize(length) > size) {
            return false;
        }
        if (type == kTypeA && cls == kClassIn && length == 4) {
            *address = (u32(message[pos]) << 24) | (u32(message[pos + 1]) << 16)
                       | (u32(message[pos + 2]) << 8) | u32(message[pos + 3]);
            return true;
        }
        pos += usize(length);
    }
    // Answers, but none of them an A record -- a CNAME the server did not
    // follow. Nothing here can chase it, and saying so is better than hanging.
    return false;
}

namespace {

// One question to one server. The id is the caller's so the answer can be
// matched to it; a reply carrying another id is somebody else's and is read
// past rather than trusted.
bool askServer(u32 server, std::string_view host, u16 id, int timeoutMs, u32* address)
{
    u8 query[kMaxDnsMessage];
    usize querySize = 0;
    if (!buildDnsQuery(host, id, query, &querySize)) {
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kDnsPort);
    to.sin_addr.s_addr = htonl(server);

    bool ok = false;
    if (::sendto(fd, query, querySize, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to))
        == long(querySize)) {
        // **One wait and one read.** A resolver that did not answer inside the
        // budget is a resolver to move on from, not one to ask twice: the next
        // server in the list is the better use of the time.
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, timeoutMs) > 0 && (p.revents & POLLIN) != 0) {
            u8 reply[kMaxDnsMessage];
            const long got = ::recv(fd, reply, sizeof(reply), 0);
            if (got > 0) {
                ok = parseDnsAnswer(reply, usize(got), id, address);
            }
        }
    }

    ::close(fd);
    return ok;
}

}  // namespace

bool resolveViaDns(std::string_view host, int timeoutMs, u32* address)
{
    u32 servers[kMaxNameServers];
    const int count = nameServers(servers, kMaxNameServers);
    if (count == 0 || address == nullptr) {
        return false;
    }

    // **The id is the name's own hash and the attempt number**, not a draw:
    // core has no generator to reach for here and the id's only job is to
    // reject a stray packet, which any value nobody else is using does.
    u16 seed = 0x3D5A;
    for (const char c : host) {
        seed = u16(seed * 31u + u8(c));
    }

    // The budget is per server, so two servers that are both down cost twice
    // the timeout -- which is the price of not giving up on the second.
    const int each = timeoutMs / count > 0 ? timeoutMs / count : timeoutMs;
    for (int i = 0; i < count; ++i) {
        if (askServer(servers[i], host, u16(seed + i), each, address)) {
            return true;
        }
    }
    return false;
}

}  // namespace mc::net
