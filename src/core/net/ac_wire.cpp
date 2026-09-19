#include "core/net/ac_wire.hpp"

#include <cstdio>
#include <cstring>

namespace mc::net::ac {

namespace {

// The domain separator the server checks against. A byte-for-byte copy of
// `identity::auth::AUTH_DOMAIN`, trailing NUL included.
constexpr char kAuthDomain[] = "alphacomputer/auth/v1";
constexpr usize kAuthDomainSize = sizeof(kAuthDomain);  // the NUL is part of it

class Writer {
public:
    explicit Writer(std::vector<u8>* out) : out_(out) {}

    void u8v(u8 v) { out_->push_back(v); }
    void boolean(bool v) { out_->push_back(v ? 1 : 0); }

    void u16v(u16 v)
    {
        out_->push_back(u8(v >> 8));
        out_->push_back(u8(v));
    }

    void u64v(u64 v)
    {
        for (int i = 7; i >= 0; --i) {
            out_->push_back(u8(v >> (8 * i)));
        }
    }

    void bytes(const u8* data, usize size) { out_->insert(out_->end(), data, data + size); }

    // A u16 length and plain UTF-8 -- *not* Java's modified form. This is our
    // protocol and not Minecraft's, and the server reads it with `str::from_utf8`.
    //
    // Truncation is on a character boundary rather than on a byte, which is the
    // bug the beacon hit cutting Mii names: half a character is not shorter
    // text, it is text the other end refuses.
    void string(std::string_view text, usize max)
    {
        usize end = text.size() < max ? text.size() : max;
        // A continuation byte at the cut means the cut landed inside a
        // character; back up until it does not. `end == size` is already a
        // boundary and must not be indexed.
        while (end > 0 && end < text.size() && (u8(text[end]) & 0xc0) == 0x80) {
            --end;
        }
        u16v(u16(end));
        bytes(reinterpret_cast<const u8*>(text.data()), end);
    }

    void optString(bool present, std::string_view text, usize max)
    {
        boolean(present);
        if (present) {
            string(text, max);
        }
    }

    void endpoint(const Endpoint& value)
    {
        if (value.v6) {
            u8v(6);
            bytes(value.addr, 16);
        } else {
            u8v(4);
            bytes(value.addr, 4);
        }
        u16v(value.port);
    }

    void optEndpoint(bool present, const Endpoint& value)
    {
        boolean(present);
        if (present) {
            endpoint(value);
        }
    }

private:
    std::vector<u8>* out_;
};

class Reader {
public:
    Reader(const u8* data, usize size) : data_(data), size_(size) {}

    usize remaining() const { return size_ - pos_; }

    bool take(usize n, const u8** out)
    {
        if (n > remaining()) {
            return false;
        }
        *out = data_ + pos_;
        pos_ += n;
        return true;
    }

    bool u8v(u8* out)
    {
        const u8* p = nullptr;
        if (!take(1, &p)) {
            return false;
        }
        *out = *p;
        return true;
    }

    bool boolean(bool* out)
    {
        u8 v = 0;
        if (!u8v(&v)) {
            return false;
        }
        *out = v != 0;
        return true;
    }

    bool u16v(u16* out)
    {
        const u8* p = nullptr;
        if (!take(2, &p)) {
            return false;
        }
        *out = u16((u16(p[0]) << 8) | u16(p[1]));
        return true;
    }

    bool u64v(u64* out)
    {
        const u8* p = nullptr;
        if (!take(8, &p)) {
            return false;
        }
        u64 value = 0;
        for (int i = 0; i < 8; ++i) {
            value = (value << 8) | u64(p[i]);
        }
        *out = value;
        return true;
    }

    bool array(usize n, u8* out)
    {
        const u8* p = nullptr;
        if (!take(n, &p)) {
            return false;
        }
        std::memcpy(out, p, n);
        return true;
    }

    // The ceiling is checked *before* the read, so a length header that
    // promises more than the field allows is refused without touching the
    // buffer.
    bool string(usize max, std::string* out)
    {
        u16 length = 0;
        if (!u16v(&length)) {
            return false;
        }
        if (usize(length) > max) {
            return false;
        }
        const u8* p = nullptr;
        if (!take(length, &p)) {
            return false;
        }
        out->assign(reinterpret_cast<const char*>(p), length);
        return true;
    }

    bool optString(usize max, bool* present, std::string* out)
    {
        if (!boolean(present)) {
            return false;
        }
        if (!*present) {
            out->clear();
            return true;
        }
        return string(max, out);
    }

    bool endpoint(Endpoint* out)
    {
        u8 family = 0;
        if (!u8v(&family)) {
            return false;
        }
        *out = Endpoint();
        if (family == 4) {
            out->v6 = false;
            if (!array(4, out->addr)) {
                return false;
            }
        } else if (family == 6) {
            out->v6 = true;
            if (!array(16, out->addr)) {
                return false;
            }
        } else {
            return false;
        }
        return u16v(&out->port);
    }

    bool optEndpoint(bool* present, Endpoint* out)
    {
        if (!boolean(present)) {
            return false;
        }
        if (!*present) {
            *out = Endpoint();
            return true;
        }
        return endpoint(out);
    }

    // A u8 count, refused against its ceiling before anything is reserved.
    // **Never size an allocation from a number a stranger sent.**
    bool count(int max, int* out)
    {
        u8 value = 0;
        if (!u8v(&value)) {
            return false;
        }
        if (int(value) > max) {
            return false;
        }
        *out = int(value);
        return true;
    }

private:
    const u8* data_;
    usize size_;
    usize pos_ = 0;
};

}  // namespace

bool Endpoint::valid() const
{
    if (port == 0) {
        return false;
    }
    const usize width = v6 ? 16u : 4u;
    for (usize i = 0; i < width; ++i) {
        if (addr[i] != 0) {
            return true;
        }
    }
    return false;
}

bool operator==(const Endpoint& a, const Endpoint& b)
{
    if (a.v6 != b.v6 || a.port != b.port) {
        return false;
    }
    return std::memcmp(a.addr, b.addr, a.v6 ? 16 : 4) == 0;
}

Endpoint endpointV4(u32 address, u16 port)
{
    Endpoint out;
    out.v6 = false;
    out.addr[0] = u8(address >> 24);
    out.addr[1] = u8(address >> 16);
    out.addr[2] = u8(address >> 8);
    out.addr[3] = u8(address);
    out.port = port;
    return out;
}

std::string endpointText(const Endpoint& endpoint)
{
    char buffer[64];
    if (!endpoint.v6) {
        std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u:%u", endpoint.addr[0],
                      endpoint.addr[1], endpoint.addr[2], endpoint.addr[3], endpoint.port);
        return buffer;
    }
    // Long form, without the `::` run compression: this is a debug line on a
    // console, not a URL, and the compressed form is another parser to get
    // wrong for no reader's benefit.
    std::string out = "[";
    for (int i = 0; i < 8; ++i) {
        std::snprintf(buffer, sizeof(buffer), i == 0 ? "%x" : ":%x",
                      (unsigned(endpoint.addr[i * 2]) << 8) | unsigned(endpoint.addr[i * 2 + 1]));
        out += buffer;
    }
    std::snprintf(buffer, sizeof(buffer), "]:%u", endpoint.port);
    out += buffer;
    return out;
}

bool encodeClient(const ClientMsg& msg, std::vector<u8>* out)
{
    out->clear();
    Writer w(out);
    w.bytes(kMagic, sizeof(kMagic));
    w.u8v(u8(msg.kind));

    switch (msg.kind) {
    case ClientKind::Hello:
        w.u16v(msg.protocol);
        w.string(msg.identity, kMaxId);
        w.bytes(msg.publicKey, kPublicKeySize);
        break;
    case ClientKind::Auth:
        w.bytes(msg.signature, kSignatureSize);
        w.optString(msg.hasPlatformName, msg.platformName, kMaxName);
        break;
    case ClientKind::Keepalive:
    case ClientKind::CloseSession:
    case ClientKind::RequestLinkCode:
    case ClientKind::Unlink:
        w.bytes(msg.token, kTokenSize);
        break;
    case ClientKind::HostSession:
        w.bytes(msg.token, kTokenSize);
        w.string(msg.worldName, kMaxName);
        w.string(msg.game, kMaxName);
        w.u8v(msg.maxPlayers);
        w.boolean(msg.locked);
        w.optEndpoint(msg.hasLocal, msg.local);
        break;
    case ClientKind::ListSessions:
        w.bytes(msg.token, kTokenSize);
        w.optString(msg.hasGameFilter, msg.game, kMaxName);
        break;
    case ClientKind::JoinRequest:
        w.bytes(msg.token, kTokenSize);
        w.optString(msg.hasJoinCode, msg.joinCode, kJoinCodeLen);
        w.u64v(msg.sessionId);
        w.optEndpoint(msg.hasLocal, msg.local);
        break;
    case ClientKind::PunchResult:
        w.bytes(msg.token, kTokenSize);
        w.u64v(msg.sessionId);
        w.boolean(msg.connected);
        break;
    }

    if (out->size() > kMaxDatagram) {
        out->clear();
        return false;
    }
    return true;
}

bool decodeServer(const u8* data, usize size, ServerMsg* out)
{
    *out = ServerMsg();

    Reader r(data, size);
    const u8* magic = nullptr;
    if (!r.take(sizeof(kMagic), &magic) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    u8 kind = 0;
    if (!r.u8v(&kind)) {
        return false;
    }

    switch (ServerKind(kind)) {
    case ServerKind::Challenge:
        out->kind = ServerKind::Challenge;
        if (!r.array(kNonceSize, out->nonce) || !r.endpoint(&out->reflexive)) {
            return false;
        }
        break;
    case ServerKind::AuthOk:
        out->kind = ServerKind::AuthOk;
        if (!r.array(kTokenSize, out->token) || !r.string(kMaxName, &out->displayName)
            || !r.optString(kMaxName, &out->hasAccount, &out->accountHandle)
            || !r.boolean(&out->firstClaim) || !r.string(32, &out->keyFingerprint)
            || !r.u16v(&out->transferPort)) {
            return false;
        }
        break;
    case ServerKind::AuthFail:
        out->kind = ServerKind::AuthFail;
        if (!r.string(kMaxReason, &out->reason)) {
            return false;
        }
        break;
    case ServerKind::KeepaliveAck:
        out->kind = ServerKind::KeepaliveAck;
        if (!r.endpoint(&out->reflexive)) {
            return false;
        }
        break;
    case ServerKind::SessionOpened:
        out->kind = ServerKind::SessionOpened;
        if (!r.u64v(&out->sessionId) || !r.string(kJoinCodeLen, &out->joinCode)) {
            return false;
        }
        break;
    case ServerKind::SessionList: {
        out->kind = ServerKind::SessionList;
        int shown = 0;
        if (!r.count(kMaxSessions, &shown)) {
            return false;
        }
        out->sessions.resize(usize(shown));
        for (SessionInfo& info : out->sessions) {
            if (!r.u64v(&info.id) || !r.string(kJoinCodeLen, &info.joinCode)
                || !r.string(kMaxName, &info.hostName) || !r.string(kMaxName, &info.worldName)
                || !r.string(kMaxName, &info.game) || !r.u8v(&info.players)
                || !r.u8v(&info.maxPlayers) || !r.boolean(&info.locked)) {
                return false;
            }
        }
        break;
    }
    case ServerKind::PunchNow: {
        out->kind = ServerKind::PunchNow;
        if (!r.u64v(&out->sessionId) || !r.string(kMaxName, &out->peerName)) {
            return false;
        }
        int shown = 0;
        if (!r.count(kMaxCandidates, &shown)) {
            return false;
        }
        out->candidates.resize(usize(shown));
        for (Candidate& candidate : out->candidates) {
            u8 kindByte = 0;
            if (!r.u8v(&kindByte) || kindByte > u8(CandidateKind::Reflexive)) {
                return false;
            }
            candidate.kind = CandidateKind(kindByte);
            if (!r.endpoint(&candidate.endpoint)) {
                return false;
            }
        }
        if (!r.array(kTokenSize, out->punchToken) || !r.u16v(&out->windowMs)) {
            return false;
        }
        break;
    }
    case ServerKind::RelayAllocated:
        out->kind = ServerKind::RelayAllocated;
        if (!r.u64v(&out->sessionId) || !r.endpoint(&out->relay)
            || !r.array(kTokenSize, out->token)) {
            return false;
        }
        break;
    case ServerKind::LinkCode:
        out->kind = ServerKind::LinkCode;
        if (!r.string(kLinkCodeLen, &out->code) || !r.u16v(&out->expiresInS)) {
            return false;
        }
        break;
    case ServerKind::Unlinked:
        out->kind = ServerKind::Unlinked;
        if (!r.string(kMaxName, &out->displayName)) {
            return false;
        }
        break;
    case ServerKind::Error:
        out->kind = ServerKind::Error;
        if (!r.string(kMaxReason, &out->reason)) {
            return false;
        }
        break;
    default:
        return false;
    }

    // Trailing bytes mean this build and the server disagree about the shape,
    // which is worth refusing rather than ignoring.
    return r.remaining() == 0;
}

void authPayload(std::string_view identity, const u8 nonce[kNonceSize], std::vector<u8>* out)
{
    out->clear();
    out->reserve(kAuthDomainSize + 2 + identity.size() + kNonceSize);
    out->insert(out->end(), kAuthDomain, kAuthDomain + kAuthDomainSize);
    out->push_back(u8(identity.size() >> 8));
    out->push_back(u8(identity.size()));
    out->insert(out->end(), identity.begin(), identity.end());
    out->insert(out->end(), nonce, nonce + kNonceSize);
}

}  // namespace mc::net::ac
