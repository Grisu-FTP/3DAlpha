// The ACMP codec, against bytes the server produced.
//
// **Nothing in tests/ac_wire_vectors.hpp came from this encoder.** It is
// printed by `cargo run --example wire_vectors` in the AlphaComputer
// repository, by the same code the running server encodes with. A round-trip
// test would prove only that this file agrees with itself, which is exactly the
// thing that is never in doubt when two implementations of one protocol
// disagree.
//
// The other half of the file is the decoder fed what it will actually be fed:
// truncations, bit flips and counts that promise more than the datagram holds.
// This parser reads packets that arrived from the open internet on a console
// with no memory protection to speak of, and the rule it keeps is that every
// malformed input is a refusal rather than a read past the end.

#include "ac_wire_vectors.hpp"
#include "core/net/ac_wire.hpp"
#include "framework.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net::ac;

namespace {

ClientMsg withToken(ClientKind kind)
{
    ClientMsg msg;
    msg.kind = kind;
    std::memset(msg.token, 0x11, kTokenSize);
    return msg;
}

bool encodes(const ClientMsg& msg, const u8* expected, usize size)
{
    std::vector<u8> out;
    if (!encodeClient(msg, &out)) {
        return false;
    }
    return out.size() == size && std::memcmp(out.data(), expected, size) == 0;
}

#define CHECK_ENCODES(msg, vector) CHECK(encodes(msg, vector, sizeof(vector)))

Endpoint v4(u8 a, u8 b, u8 c, u8 d, u16 port)
{
    Endpoint out;
    out.addr[0] = a;
    out.addr[1] = b;
    out.addr[2] = c;
    out.addr[3] = d;
    out.port = port;
    return out;
}

bool decodes(const u8* data, usize size, ServerMsg* out)
{
    return decodeServer(data, size, out);
}

}  // namespace

// The version the vectors were generated at is the version this build speaks.
// If this fails, the header was regenerated against a server that moved and
// core/net/ac_wire.hpp did not.
TEST(the_vectors_were_generated_for_this_protocol_version)
{
    CHECK_EQ(int(kProtocol), int(test::kAcVectorProtocol));
}

TEST(every_client_message_encodes_to_the_servers_own_bytes)
{
    ClientMsg hello;
    hello.kind = ClientKind::Hello;
    hello.identity = "3ds:000123456789";
    std::memset(hello.publicKey, 0x2a, kPublicKeySize);
    CHECK_ENCODES(hello, test::kAcHello);

    ClientMsg auth;
    auth.kind = ClientKind::Auth;
    std::memset(auth.signature, 0x5c, kSignatureSize);
    auth.hasPlatformName = true;
    auth.platformName = "Grisu";
    CHECK_ENCODES(auth, test::kAcAuthNamed);

    // A console whose friend service is unset has no name to report, and the
    // absent form is a different shape rather than an empty string.
    auth.hasPlatformName = false;
    auth.platformName.clear();
    CHECK_ENCODES(auth, test::kAcAuthAnonymous);

    CHECK_ENCODES(withToken(ClientKind::Keepalive), test::kAcKeepalive);
    CHECK_ENCODES(withToken(ClientKind::CloseSession), test::kAcCloseSession);
    CHECK_ENCODES(withToken(ClientKind::RequestLinkCode), test::kAcRequestLinkCode);
    CHECK_ENCODES(withToken(ClientKind::Unlink), test::kAcUnlink);

    ClientMsg host = withToken(ClientKind::HostSession);
    host.kind = ClientKind::HostSession;
    host.worldName = "New World";
    host.game = "3ds";
    host.maxPlayers = 4;
    host.locked = true;
    host.hasLocal = true;
    host.local = v4(192, 168, 1, 50, 7717);
    CHECK_ENCODES(host, test::kAcHostSession);

    ClientMsg list = withToken(ClientKind::ListSessions);
    list.hasGameFilter = true;
    list.game = "3ds";
    CHECK_ENCODES(list, test::kAcListSessions);

    ClientMsg byCode = withToken(ClientKind::JoinRequest);
    byCode.hasJoinCode = true;
    byCode.joinCode = "PIGLIN";
    byCode.hasLocal = true;
    byCode.local = v4(192, 168, 1, 50, 7717);
    CHECK_ENCODES(byCode, test::kAcJoinByCode);

    ClientMsg byId = withToken(ClientKind::JoinRequest);
    byId.sessionId = 0x0102030405060708ULL;
    CHECK_ENCODES(byId, test::kAcJoinById);

    ClientMsg punch = withToken(ClientKind::PunchResult);
    punch.sessionId = 42;
    punch.connected = true;
    CHECK_ENCODES(punch, test::kAcPunchResult);
}

TEST(every_server_message_decodes_to_what_the_server_put_in_it)
{
    ServerMsg msg;

    CHECK(decodes(test::kAcChallenge, sizeof(test::kAcChallenge), &msg));
    CHECK(msg.kind == ServerKind::Challenge);
    CHECK_EQ(int(msg.nonce[0]), 0x7e);
    CHECK_EQ(int(msg.nonce[31]), 0x7e);
    CHECK(msg.reflexive == v4(203, 0, 113, 7, 51000));

    CHECK(decodes(test::kAcAuthOkLinked, sizeof(test::kAcAuthOkLinked), &msg));
    CHECK(msg.kind == ServerKind::AuthOk);
    CHECK_EQ(int(msg.token[0]), 0x11);
    CHECK_EQ(msg.displayName, std::string("Grisu the Builder"));
    CHECK(msg.hasAccount);
    CHECK_EQ(msg.accountHandle, std::string("Grisu"));
    CHECK(!msg.firstClaim);
    CHECK_EQ(msg.keyFingerprint, std::string("abcd-ef01-2345-6789"));

    // The unlinked form is what the Profile screen decides "Link" against, so
    // the absent handle has to survive as absent rather than as empty.
    CHECK(decodes(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), &msg));
    CHECK(!msg.hasAccount);
    CHECK(msg.accountHandle.empty());
    CHECK(msg.firstClaim);
    CHECK_EQ(msg.displayName, std::string("Steve#A1B2"));

    CHECK(decodes(test::kAcAuthFail, sizeof(test::kAcAuthFail), &msg));
    CHECK(msg.kind == ServerKind::AuthFail);
    CHECK_EQ(msg.reason, std::string("already bound to a different key"));

    // IPv6 on the wire even though nothing punches over it yet: the field is
    // there and a console that gets one must not mis-parse the rest.
    CHECK(decodes(test::kAcKeepaliveAck, sizeof(test::kAcKeepaliveAck), &msg));
    CHECK(msg.kind == ServerKind::KeepaliveAck);
    CHECK(msg.reflexive.v6);
    CHECK_EQ(int(msg.reflexive.port), 9000);
    CHECK_EQ(endpointText(msg.reflexive), std::string("[2001:db8:0:0:0:0:0:5]:9000"));

    CHECK(decodes(test::kAcSessionOpened, sizeof(test::kAcSessionOpened), &msg));
    CHECK(msg.kind == ServerKind::SessionOpened);
    CHECK_EQ(int(msg.sessionId), 9);
    CHECK_EQ(msg.joinCode, std::string("PIGLIN"));

    CHECK(decodes(test::kAcSessionList, sizeof(test::kAcSessionList), &msg));
    CHECK(msg.kind == ServerKind::SessionList);
    CHECK_EQ(int(msg.sessions.size()), 2);
    CHECK_EQ(msg.sessions[0].worldName, std::string("New World"));
    CHECK_EQ(msg.sessions[0].hostName, std::string("Grisu"));
    CHECK_EQ(int(msg.sessions[0].players), 2);
    CHECK_EQ(int(msg.sessions[0].maxPlayers), 4);
    CHECK_EQ(msg.sessions[1].joinCode, std::string("XYZ789"));
    CHECK_EQ(int(msg.sessions[1].id), 2);

    CHECK(decodes(test::kAcPunchNow, sizeof(test::kAcPunchNow), &msg));
    CHECK(msg.kind == ServerKind::PunchNow);
    CHECK_EQ(msg.peerName, std::string("Steve#A1B2"));
    CHECK_EQ(int(msg.candidates.size()), 2);
    // The local candidate comes first, which is the whole point of sending one:
    // two consoles in a room should talk across it rather than out to the
    // internet and back, and many routers will not hairpin that at all.
    CHECK(msg.candidates[0].kind == CandidateKind::Local);
    CHECK(msg.candidates[0].endpoint == v4(192, 168, 1, 9, 7717));
    CHECK(msg.candidates[1].kind == CandidateKind::Reflexive);
    CHECK_EQ(int(msg.windowMs), 3000);
    CHECK_EQ(int(msg.punchToken[0]), 0x33);

    CHECK(decodes(test::kAcRelayAllocated, sizeof(test::kAcRelayAllocated), &msg));
    CHECK(msg.kind == ServerKind::RelayAllocated);
    CHECK(msg.relay == v4(203, 0, 113, 1, 7718));
    CHECK_EQ(int(msg.token[0]), 0x44);

    CHECK(decodes(test::kAcLinkCode, sizeof(test::kAcLinkCode), &msg));
    CHECK(msg.kind == ServerKind::LinkCode);
    CHECK_EQ(msg.code, std::string("K7P2M4QX"));
    CHECK_EQ(int(msg.expiresInS), 600);

    CHECK(decodes(test::kAcUnlinked, sizeof(test::kAcUnlinked), &msg));
    CHECK(msg.kind == ServerKind::Unlinked);
    CHECK_EQ(msg.displayName, std::string("Steve#A1B2"));

    CHECK(decodes(test::kAcError, sizeof(test::kAcError), &msg));
    CHECK(msg.kind == ServerKind::Error);
    CHECK_EQ(msg.reason, std::string("no such session"));
}

// Nothing without the magic is parsed at all. On a public UDP port that is the
// common case rather than the exceptional one: scanners, strays and the
// occasional reply to somebody else's spoofed packet.
TEST(a_datagram_without_the_magic_is_refused_before_anything_is_read)
{
    ServerMsg msg;
    const u8 junk[] = {'n', 'o', 'p', 'e', 0x02, 0x00};
    CHECK(!decodes(junk, sizeof(junk), &msg));
    CHECK(!decodes(junk, 2, &msg));
    CHECK(!decodes(nullptr, 0, &msg));

    // The magic alone, with no message id behind it.
    CHECK(!decodes(kMagic, sizeof(kMagic), &msg));
}

TEST(every_truncation_and_every_flipped_bit_is_refused_rather_than_read_past)
{
    const u8* vectors[] = {test::kAcChallenge, test::kAcAuthOkLinked, test::kAcSessionList,
                           test::kAcPunchNow,  test::kAcRelayAllocated};
    const usize sizes[] = {sizeof(test::kAcChallenge), sizeof(test::kAcAuthOkLinked),
                           sizeof(test::kAcSessionList), sizeof(test::kAcPunchNow),
                           sizeof(test::kAcRelayAllocated)};

    for (int v = 0; v < 5; ++v) {
        ServerMsg msg;
        // Every prefix short of the whole thing must be refused: a message is a
        // whole datagram or it is nothing.
        for (usize n = 0; n < sizes[v]; ++n) {
            CHECK(!decodes(vectors[v], n, &msg));
        }
        // The whole thing is fine, and one byte more is not -- trailing bytes
        // mean this build and the server disagree about the shape.
        CHECK(decodes(vectors[v], sizes[v], &msg));
        std::vector<u8> longer(vectors[v], vectors[v] + sizes[v]);
        longer.push_back(0);
        CHECK(!decodes(longer.data(), longer.size(), &msg));

        // And every single-byte corruption either parses or is refused, but
        // never crashes and never reads off the end. The sanitiser the host
        // suite builds with is what makes this assertion mean something.
        for (usize i = 0; i < sizes[v]; ++i) {
            std::vector<u8> bad(vectors[v], vectors[v] + sizes[v]);
            bad[i] = u8(bad[i] ^ 0xff);
            ServerMsg ignored;
            (void)decodes(bad.data(), bad.size(), &ignored);
        }
    }
}

// **Never reserve from a number a stranger sent.** A count is checked against
// its ceiling before anything is allocated, and against the bytes that are
// actually there before anything is read.
TEST(a_list_count_larger_than_the_datagram_is_refused_before_it_allocates)
{
    std::vector<u8> claim(kMagic, kMagic + sizeof(kMagic));
    claim.push_back(u8(ServerKind::SessionList));
    claim.push_back(0xff);  // 255 sessions, in a six-byte datagram.

    ServerMsg msg;
    CHECK(!decodeServer(claim.data(), claim.size(), &msg));
    CHECK(msg.sessions.empty());

    // Inside the ceiling but still a lie about what follows.
    claim[5] = u8(kMaxSessions);
    CHECK(!decodeServer(claim.data(), claim.size(), &msg));
}

TEST(a_string_longer_than_its_field_allows_is_refused_at_the_length)
{
    std::vector<u8> reason(kMagic, kMagic + sizeof(kMagic));
    reason.push_back(u8(ServerKind::Error));
    reason.push_back(u8((kMaxReason + 1) >> 8));
    reason.push_back(u8(kMaxReason + 1));
    reason.resize(reason.size() + kMaxReason + 1, 'x');

    ServerMsg msg;
    CHECK(!decodeServer(reason.data(), reason.size(), &msg));
}

// A message id this build has never heard of is refused rather than guessed at.
// A server one version ahead is a thing that will happen, and the answer to it
// is to stop, not to read its bytes with this version's layout.
TEST(an_unknown_message_id_is_refused)
{
    std::vector<u8> unknown(kMagic, kMagic + sizeof(kMagic));
    unknown.push_back(0x7f);
    unknown.push_back(0);

    ServerMsg msg;
    CHECK(!decodeServer(unknown.data(), unknown.size(), &msg));
}

// The payload the server rebuilds and verifies against. If this is wrong, every
// login fails with "signature does not verify" and nothing says why -- so it is
// asserted here byte for byte rather than left to a live test against a server.
TEST(the_signed_payload_is_the_domain_the_identity_and_the_nonce)
{
    u8 nonce[kNonceSize];
    for (usize i = 0; i < kNonceSize; ++i) {
        nonce[i] = u8(i);
    }

    std::vector<u8> payload;
    authPayload("3ds:000123456789", nonce, &payload);

    const std::string domain = "alphacomputer/auth/v1";
    CHECK_EQ(payload.size(), domain.size() + 1 + 2 + 16 + kNonceSize);
    CHECK(std::memcmp(payload.data(), domain.data(), domain.size()) == 0);
    // The NUL is part of the domain separator, not a terminator this file adds.
    CHECK_EQ(int(payload[domain.size()]), 0);
    CHECK_EQ(int(payload[domain.size() + 1]), 0);
    CHECK_EQ(int(payload[domain.size() + 2]), 16);
    CHECK(std::memcmp(payload.data() + domain.size() + 3, "3ds:000123456789", 16) == 0);
    CHECK(std::memcmp(payload.data() + payload.size() - kNonceSize, nonce, kNonceSize) == 0);
}

// The name a console reports comes off the friend list and can be anything.
// Cutting it at a byte would hand the server half a character, which it refuses
// as invalid UTF-8 -- the bug the UDS beacon already hit once.
TEST(a_name_too_long_for_its_field_is_cut_on_a_character_and_not_on_a_byte)
{
    ClientMsg msg;
    msg.kind = ClientKind::Auth;
    msg.hasPlatformName = true;
    // 30 bytes of ASCII, then a 4-byte character that cannot fit in 32.
    msg.platformName = std::string(30, 'a') + "\xf0\x9f\x98\x80";

    std::vector<u8> out;
    CHECK(encodeClient(msg, &out));

    // magic, id, the 64-byte signature, the presence byte, then the u16 length.
    const usize lengthAt = 4 + 1 + kSignatureSize + 1;
    const usize length = (usize(out[lengthAt]) << 8) | usize(out[lengthAt + 1]);
    CHECK_EQ(length, usize(30));
    CHECK_EQ(out.size(), lengthAt + 2 + 30);
}
