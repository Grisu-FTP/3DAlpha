#include "core/net/ac_identity.hpp"

#include "core/net/ac_wire.hpp"
#include "core/util/sha1.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

namespace mc::net::ac {

namespace {

// A decimal port, which is 1..65535 and nothing else. Port 0 is not a port and
// neither is 70000; both leave the colon where it was, so what fails is the
// name lookup rather than a connection to somewhere unintended.
bool parsePort(std::string_view text, u16* out)
{
    if (text.empty() || text.size() > 5) {
        return false;
    }
    u32 value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + u32(c - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    *out = u16(value);
    return true;
}

}  // namespace

u8 friendCodeChecksum(u32 principalId)
{
    const u8 little[4] = {u8(principalId), u8(principalId >> 8), u8(principalId >> 16),
                          u8(principalId >> 24)};
    u8 digest[util::kSha1DigestSize];
    util::sha1(little, sizeof(little), digest);
    return u8(digest[0] >> 1);
}

u64 friendCodeFor(u32 principalId)
{
    return (u64(friendCodeChecksum(principalId)) << 32) | u64(principalId);
}

std::string identityFor(u32 principalId)
{
    if (principalId == 0) {
        return std::string();
    }
    char digits[24];
    std::snprintf(digits, sizeof(digits), "%012llu",
                  static_cast<unsigned long long>(friendCodeFor(principalId)));
    return std::string("3ds:") + digits;
}

bool loadOrCreateIdentity(io::FileSystem& fs, const char* keyPath, u32 principalId,
                          RandomFn random, void* randomContext, Identity* out,
                          std::string* error)
{
    *out = Identity();
    out->name = identityFor(principalId);
    if (out->name.empty()) {
        *error = "This console has no friend code to play under.";
        return false;
    }

    std::vector<u8> bytes;
    if (fs.readFile(keyPath, &bytes, util::kEd25519SeedSize + 1)
        && bytes.size() == util::kEd25519SeedSize) {
        std::memcpy(out->seed, bytes.data(), util::kEd25519SeedSize);
        util::ed25519PublicKey(out->seed, out->publicKey);
        return true;
    }

    // **A file of the wrong length is not overwritten.** It is either somebody
    // else's file or a key that was written badly, and replacing it would take
    // this console's identity away from it for good with no way back. Say so
    // and stop.
    if (!bytes.empty()) {
        *error = "The identity key on this card is not the right size.";
        return false;
    }

    if (random == nullptr || !random(randomContext, out->seed, util::kEd25519SeedSize)) {
        *error = "This console could not generate a key.";
        return false;
    }
    if (!fs.writeFileAtomic(keyPath,
                            ConstByteSpan(out->seed, util::kEd25519SeedSize))) {
        // Refusing rather than playing with a key that is only in memory: a key
        // that is not on the card claims an identity on the first login and
        // then cannot prove it on the second, which is the one failure in this
        // design that an operator has to clean up by hand.
        *error = "Could not write the identity key to the card.";
        return false;
    }
    util::ed25519PublicKey(out->seed, out->publicKey);
    out->created = true;
    return true;
}

bool parseServerUrl(const std::string& url, ServerAddress* out)
{
    *out = ServerAddress();

    std::string_view text(url);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }

    // **The scheme goes first.** Trimming the trailing slash before this would
    // turn `https://` into `https:`, which then reads as a host called `https`
    // -- a URL with no host in it has to fail, not resolve to nonsense.
    const usize scheme = text.find("://");
    if (scheme != std::string_view::npos) {
        text.remove_prefix(scheme + 3);
    }
    // Credentials, if somebody pasted a URL with them. Dropped rather than
    // refused: they mean nothing to a UDP port and are not worth a message.
    const usize at = text.find('@');
    if (at != std::string_view::npos) {
        text.remove_prefix(at + 1);
    }
    // Everything from the path onwards is the website's business, not ours.
    const usize path = text.find_first_of("/?#");
    if (path != std::string_view::npos) {
        text = text.substr(0, path);
    }

    u16 port = kDefaultPort;
    if (!text.empty() && text.front() == '[') {
        const usize close = text.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        std::string_view rest = text.substr(close + 1);
        out->host.assign(text.substr(1, close - 1));
        if (!rest.empty() && rest.front() == ':') {
            if (!parsePort(rest.substr(1), &port)) {
                return false;
            }
        } else if (!rest.empty()) {
            return false;
        }
    } else {
        const usize colon = text.rfind(':');
        // **More than one colon and no brackets is an address, not an address
        // and a port.** Cutting `2001:db8::5` at its last colon would leave a
        // host of `2001:db8:` and a port of 5, which is a plausible-looking
        // answer and the wrong one.
        const bool manyColons =
            colon != std::string_view::npos && text.find(':') != colon;
        if (colon != std::string_view::npos && !manyColons
            && parsePort(text.substr(colon + 1), &port)) {
            text = text.substr(0, colon);
        }
        out->host.assign(text);
    }

    if (out->host.empty()) {
        return false;
    }
    out->port = port;
    return true;
}

}  // namespace mc::net::ac
