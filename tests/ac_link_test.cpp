// Two consoles, a NAT each, and the three ways they might end up talking.
//
// **The NAT here is the point of the test.** `FakeNet` forwards an inbound
// datagram only from an address the receiving node has recently sent to, which
// is the rule most home routers actually keep and the whole reason a rendezvous
// server has to exist. A one-sided punch fails against it, exactly as it would
// in a living room; two consoles firing together get through. Without that rule
// the test would pass for a client that never punched at all.
//
// The third way is the relay, for the case where there is no hole to punch --
// symmetric NAT, which allocates a fresh external port per destination, so the
// address the server saw is not the address the peer will see.

#include "ac_wire_vectors.hpp"
#include "core/net/ac_link.hpp"
#include "framework.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net::ac;

namespace {

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

const Endpoint kServer = v4(203, 0, 113, 1, kDefaultPort);
const Endpoint kRelay = v4(203, 0, 113, 1, 7718);
const Endpoint kHostPublic = v4(198, 51, 100, 10, 50000);
const Endpoint kGuestPublic = v4(198, 51, 100, 20, 60000);

struct Packet {
    Endpoint from;
    std::vector<u8> bytes;
};

class FakeSocket;

// A network with opinions. Every node is behind a NAT that opens for an address
// only once this node has sent to it; the server is reachable from everywhere,
// because that is what makes it a rendezvous server.
class FakeNet {
public:
    void attach(const Endpoint& at, FakeSocket* socket);
    void send(const Endpoint& from, const Endpoint& to, const u8* data, usize size);

    // A node whose NAT never opens for anybody: carrier-grade, symmetric, the
    // case the relay exists for.
    void makeUnreachable(const Endpoint& at) { sealed_.push_back(at); }

    // Forwards `token || payload` to the other end of the allocation, stripping
    // the token -- exactly what AlphaComputer's relay does, and deliberately
    // blind to what is inside.
    //
    // **The addresses are learned, never told**, as `Relay::handle` learns
    // them: an end is known only once a datagram carrying its token has
    // arrived, and a datagram for an end not yet known is dropped. The real
    // relay is never given an address by the control plane, and a fake that was
    // given both let a deadlock through -- two consoles each waiting to hear
    // from the other before sending anything.
    void allocateRelay(const u8 tokenA[kTokenSize], const u8 tokenB[kTokenSize]);

    u32 relayed = 0;

private:
    struct Node {
        Endpoint addr;
        FakeSocket* socket = nullptr;
        std::vector<Endpoint> opened;
    };
    struct RelayEnd {
        Endpoint addr;
        bool known = false;
        u8 token[kTokenSize] = {};
    };

    Node* find(const Endpoint& at);
    bool sealed(const Endpoint& at) const;
    bool handleRelay(const Endpoint& from, const u8* data, usize size);

    std::vector<Node> nodes_;
    std::vector<Endpoint> sealed_;
    std::vector<RelayEnd> relayEnds_;
};

class FakeSocket : public UdpTransport {
public:
    FakeSocket(FakeNet* net, const Endpoint& self, const Endpoint& local)
        : net_(net), self_(self), local_(local)
    {
        net_->attach(self, this);
    }

    bool send(const Endpoint& to, const u8* data, usize size) override
    {
        net_->send(self_, to, data, size);
        return true;
    }

    bool receive(u8* buffer, usize capacity, usize* size, Endpoint* from) override
    {
        if (inbox.empty()) {
            return false;
        }
        const Packet& next = inbox.front();
        if (next.bytes.size() > capacity) {
            inbox.erase(inbox.begin());
            return false;
        }
        std::memcpy(buffer, next.bytes.data(), next.bytes.size());
        *size = next.bytes.size();
        *from = next.from;
        inbox.erase(inbox.begin());
        return true;
    }

    Endpoint localEndpoint() const override { return local_; }

    std::vector<Packet> inbox;

private:
    FakeNet* net_;
    Endpoint self_;
    Endpoint local_;
};

FakeNet::Node* FakeNet::find(const Endpoint& at)
{
    for (Node& node : nodes_) {
        if (node.addr == at) {
            return &node;
        }
    }
    return nullptr;
}

bool FakeNet::sealed(const Endpoint& at) const
{
    for (const Endpoint& one : sealed_) {
        if (one == at) {
            return true;
        }
    }
    return false;
}

void FakeNet::attach(const Endpoint& at, FakeSocket* socket)
{
    Node node;
    node.addr = at;
    node.socket = socket;
    nodes_.push_back(node);
}

void FakeNet::allocateRelay(const u8 tokenA[kTokenSize], const u8 tokenB[kTokenSize])
{
    RelayEnd first;
    std::memcpy(first.token, tokenA, kTokenSize);
    RelayEnd second;
    std::memcpy(second.token, tokenB, kTokenSize);
    relayEnds_.push_back(first);
    relayEnds_.push_back(second);
}

bool FakeNet::handleRelay(const Endpoint& from, const u8* data, usize size)
{
    if (size <= kTokenSize) {
        return true;  // Nothing to forward; dropped, as the real one does.
    }
    for (usize i = 0; i < relayEnds_.size(); ++i) {
        if (std::memcmp(relayEnds_[i].token, data, kTokenSize) != 0) {
            continue;
        }
        // Re-learned on every datagram, as the real one does.
        relayEnds_[i].addr = from;
        relayEnds_[i].known = true;

        const RelayEnd& other = relayEnds_[i ^ 1];
        if (!other.known) {
            return true;  // `PeerUnknown`: one side always arrives first.
        }
        Node* node = find(other.addr);
        if (node == nullptr) {
            return true;
        }
        // **The relay is one more address behind the other end's NAT.** It is
        // the server's machine but not the port the console logged in on, so
        // it gets through only once that console has sent to the relay itself.
        bool opened = false;
        for (const Endpoint& one : node->opened) {
            if (one == kRelay) {
                opened = true;
                break;
            }
        }
        if (!opened) {
            return true;
        }
        ++relayed;
        // From the relay's own address, with the token gone: the receiver
        // never sees the other end's credential.
        node->socket->inbox.push_back(
            {kRelay, std::vector<u8>(data + kTokenSize, data + size)});
        return true;
    }
    return true;
}

void FakeNet::send(const Endpoint& from, const Endpoint& to, const u8* data, usize size)
{
    Node* sender = find(from);
    if (sender != nullptr) {
        sender->opened.push_back(to);
    }
    if (to == kRelay) {
        handleRelay(from, data, size);
        return;
    }

    Node* target = find(to);
    if (target == nullptr) {
        return;
    }
    if (to != kServer) {
        if (sealed(to)) {
            return;
        }
        // The rule the whole design turns on: an inbound datagram gets through
        // only from an address this node has already sent to.
        bool opened = false;
        for (const Endpoint& one : target->opened) {
            if (one == from) {
                opened = true;
                break;
            }
        }
        if (!opened) {
            return;
        }
    }
    target->socket->inbox.push_back({from, std::vector<u8>(data, data + size)});
}

// ---------------------------------------------------------------------------
// The server's half, encoded here because only the console's half ships. The
// decoder these are fed to is the one already checked against Rust's bytes in
// tests/ac_wire_test.cpp, so an encoder that agreed with a wrong decoder would
// have been caught there.

void putU16(std::vector<u8>* out, u16 v)
{
    out->push_back(u8(v >> 8));
    out->push_back(u8(v));
}

void putU64(std::vector<u8>* out, u64 v)
{
    for (int i = 7; i >= 0; --i) {
        out->push_back(u8(v >> (8 * i)));
    }
}

void putString(std::vector<u8>* out, const std::string& text)
{
    putU16(out, u16(text.size()));
    out->insert(out->end(), text.begin(), text.end());
}

void putEndpoint(std::vector<u8>* out, const Endpoint& endpoint)
{
    out->push_back(endpoint.v6 ? 6 : 4);
    out->insert(out->end(), endpoint.addr, endpoint.addr + (endpoint.v6 ? 16 : 4));
    putU16(out, endpoint.port);
}

std::vector<u8> punchNow(u64 sessionId, const std::string& peerName, const Endpoint& reflexive,
                         const u8 token[kTokenSize], u16 windowMs)
{
    std::vector<u8> out(kMagic, kMagic + sizeof(kMagic));
    out.push_back(u8(ServerKind::PunchNow));
    putU64(&out, sessionId);
    putString(&out, peerName);
    out.push_back(1);
    out.push_back(u8(CandidateKind::Reflexive));
    putEndpoint(&out, reflexive);
    out.insert(out.end(), token, token + kTokenSize);
    putU16(&out, windowMs);
    return out;
}

std::vector<u8> relayAllocated(u64 sessionId, const u8 token[kTokenSize])
{
    std::vector<u8> out(kMagic, kMagic + sizeof(kMagic));
    out.push_back(u8(ServerKind::RelayAllocated));
    putU64(&out, sessionId);
    putEndpoint(&out, kRelay);
    out.insert(out.end(), token, token + kTokenSize);
    return out;
}

// ---------------------------------------------------------------------------

Login loginFor(const char* identity)
{
    Login login;
    login.identity = identity;
    for (usize i = 0; i < sizeof(login.seed); ++i) {
        login.seed[i] = u8(i);
    }
    std::memset(login.publicKey, 0x2a, sizeof(login.publicKey));
    return login;
}

// One console: its socket and its connection, logged in with the server's own
// Challenge and AuthOk bytes.
struct Console {
    Console(FakeNet* net, const Endpoint& publicAddr, const Endpoint& localAddr, bool hosting,
            const char* identity)
        : socket(net, publicAddr, localAddr)
    {
        connection.begin(kServer, loginFor(identity), &socket, hosting, 0);
        socket.inbox.push_back(
            {kServer, std::vector<u8>(test::kAcChallenge,
                                      test::kAcChallenge + sizeof(test::kAcChallenge))});
        connection.pump(0);
        socket.inbox.push_back(
            {kServer,
             std::vector<u8>(test::kAcAuthOkUnlinked,
                             test::kAcAuthOkUnlinked + sizeof(test::kAcAuthOkUnlinked))});
        connection.pump(1);
        while (connection.takeEvent() != Event::None) {
        }
    }

    void fromServer(const std::vector<u8>& bytes)
    {
        socket.inbox.push_back({kServer, bytes});
    }

    // One frame: read, drive, and hand the two introduction events to the link.
    void frame(u32 nowMs)
    {
        connection.pump(nowMs);
        while (connection.takeEvent() != Event::None) {
        }
    }

    FakeSocket socket;
    Connection connection;
};

// Runs both consoles forward together, a frame at a time.
void run(Console* a, Console* b, u32 fromMs, u32 toMs, u32 stepMs)
{
    for (u32 t = fromMs; t <= toMs; t += stepMs) {
        a->frame(t);
        b->frame(t);
    }
}

bool crosses(Console* from, u16 node, Console* to, const char* text)
{
    const usize size = std::strlen(text);
    if (!from->connection.send(node, reinterpret_cast<const u8*>(text), size)) {
        return false;
    }
    to->frame(9999);
    u8 buffer[mc::net::link::kMaxDatagram];
    usize got = 0;
    u16 gotNode = 0;
    if (!to->connection.receive(buffer, sizeof(buffer), &got, &gotNode)) {
        return false;
    }
    return got == size && std::memcmp(buffer, text, size) == 0;
}

}  // namespace

// Both ends fire at the same moment and each one's outbound probe opens the
// pinhole the other's inbound probe needs. This is the ordinary path and the
// one worth being sure of.
TEST(two_consoles_punch_through_a_nat_that_only_opens_for_who_it_has_written_to)
{
    FakeNet net;
    Console host(&net, kHostPublic, v4(192, 168, 1, 10, 7717), true, "3ds:378243453201");
    Console guest(&net, kGuestPublic, v4(192, 168, 2, 20, 7717), false, "3ds:180388627432");

    u8 token[kTokenSize];
    std::memset(token, 0x55, sizeof(token));

    // One JoinRequest, two PunchNow messages, the same token and the same
    // window. Simultaneity is the mechanism.
    host.fromServer(punchNow(9, "Guest", kGuestPublic, token, 3000));
    guest.fromServer(punchNow(9, "Host", kHostPublic, token, 3000));

    run(&host, &guest, 100, 1000, 50);

    CHECK(host.connection.connected());
    CHECK(guest.connection.connected());
    CHECK(!host.connection.punching());
    CHECK(!guest.connection.relayed(mc::net::link::kHostNode));
    CHECK_EQ(host.connection.peerName(), std::string("Guest"));

    // A host numbers its guests from 2 upwards, the way local wireless does, so
    // the session above knows nothing about where the frames came from.
    CHECK(crosses(&guest, mc::net::link::kHostNode, &host, "hello from the guest"));
    CHECK(crosses(&host, 2, &guest, "and the world comes back"));
    CHECK_EQ(int(net.relayed), 0);
}

// Fired one-sidedly, a punch works only on a full-cone NAT. This is what typing
// a code with no coordination would get you, and it is why the server sends
// `PunchNow` to both consoles from one request.
TEST(a_punch_only_one_console_knows_about_does_not_get_through)
{
    FakeNet net;
    Console host(&net, kHostPublic, v4(192, 168, 1, 10, 7717), true, "3ds:378243453201");
    Console guest(&net, kGuestPublic, v4(192, 168, 2, 20, 7717), false, "3ds:180388627432");

    u8 token[kTokenSize];
    std::memset(token, 0x55, sizeof(token));
    guest.fromServer(punchNow(9, "Host", kHostPublic, token, 3000));

    run(&host, &guest, 100, 4000, 50);

    CHECK(!guest.connection.connected());
    CHECK(!host.connection.connected());
}

// Symmetric or carrier-grade NAT: there is no hole to punch, so the punch is
// reported as failed and the server allocates a relay. It costs real bandwidth,
// which is why it is tried last rather than first.
TEST(a_punch_that_cannot_work_falls_back_to_the_relay_and_the_frames_still_cross)
{
    FakeNet net;
    Console host(&net, kHostPublic, v4(192, 168, 1, 10, 7717), true, "3ds:378243453201");
    Console guest(&net, kGuestPublic, v4(192, 168, 2, 20, 7717), false, "3ds:180388627432");
    net.makeUnreachable(kHostPublic);
    net.makeUnreachable(kGuestPublic);

    u8 token[kTokenSize];
    std::memset(token, 0x55, sizeof(token));
    host.fromServer(punchNow(9, "Guest", kGuestPublic, token, 3000));
    guest.fromServer(punchNow(9, "Host", kHostPublic, token, 3000));

    run(&host, &guest, 100, 3500, 50);
    CHECK(!host.connection.connected());
    CHECK(!guest.connection.connected());

    u8 hostToken[kTokenSize];
    u8 guestToken[kTokenSize];
    std::memset(hostToken, 0xa1, sizeof(hostToken));
    std::memset(guestToken, 0xb2, sizeof(guestToken));
    net.allocateRelay(hostToken, guestToken);

    host.fromServer(relayAllocated(9, hostToken));
    guest.fromServer(relayAllocated(9, guestToken));
    run(&host, &guest, 4000, 4100, 50);

    CHECK(host.connection.connected());
    CHECK(guest.connection.connected());
    CHECK(host.connection.relayed(2));
    CHECK(guest.connection.relayed(mc::net::link::kHostNode));

    // **The hellos that registered both ends went through and stopped there.**
    // The relay only learns an address from a datagram, so each console sends
    // one the moment it is told -- and the other console swallows it as a
    // probe rather than handing a session a frame it never sent.
    CHECK(net.relayed >= 1);
    u8 stray[mc::net::link::kMaxDatagram];
    usize strayBytes = 0;
    u16 strayNode = 0;
    CHECK(!host.connection.receive(stray, sizeof(stray), &strayBytes, &strayNode));
    CHECK(!guest.connection.receive(stray, sizeof(stray), &strayBytes, &strayNode));

    // The guest speaks first, as a GuestSession does: a host says nothing
    // until it has been asked to.
    CHECK(crosses(&guest, mc::net::link::kHostNode, &host, "through the relay"));
    CHECK(crosses(&host, 2, &guest, "and back again"));
    CHECK(net.relayed >= 2);
}

// A probe from somebody who was never introduced carries a token that matches
// nothing, and is dropped rather than becoming a peer. On a public port this is
// most of what arrives.
// **Two consoles on one network reach each other two ways**: across the room to
// the local candidate, and out to the router and back to the public one, which
// arrives from whatever source the router's loopback writes on it. Both prove
// the token, and the order they land in is the network's. The address a peer is
// sent to used to be whichever probe came *last*, and frames were accepted only
// from that one address -- so the two ends could settle on different paths, the
// guest's Hello arrived from an address the host no longer recognised, and the
// guest gave up with "the host did not answer".
TEST(a_peer_proven_on_two_paths_is_heard_on_both_and_sent_to_on_the_first)
{
    FakeNet net;
    Console host(&net, kHostPublic, v4(192, 168, 1, 10, 7717), true, "3ds:378243453201");
    const Endpoint guestLan = v4(192, 168, 1, 20, 7717);
    const Endpoint loopback = v4(192, 168, 1, 1, 40001);  // the router's rewrite

    // Something to observe what the host sends on the LAN. It writes to the
    // host first so its own NAT lets the answer in; that frame lands before any
    // probe, so it is from nobody and is dropped.
    FakeSocket lan(&net, guestLan, guestLan);
    const u8 nothing = 0;
    lan.send(kHostPublic, &nothing, 1);

    u8 token[kTokenSize];
    std::memset(token, 0x55, sizeof(token));
    host.fromServer(punchNow(9, "Guest", kGuestPublic, token, 3000));
    host.frame(100);

    std::vector<u8> ping(kProbeMagic, kProbeMagic + sizeof(kProbeMagic));
    ping.insert(ping.end(), token, token + kTokenSize);
    ping.push_back(0);
    host.socket.inbox.push_back({guestLan, ping});
    host.socket.inbox.push_back({loopback, ping});
    host.frame(150);
    CHECK(host.connection.connected());

    u8 buffer[mc::net::link::kMaxDatagram];
    usize got = 0;
    u16 node = 0;
    CHECK(!host.connection.receive(buffer, sizeof(buffer), &got, &node));

    // The guest's Hello comes in on the LAN, which proved the token first.
    const char hello[] = "hello over the room";
    host.socket.inbox.push_back(
        {guestLan, std::vector<u8>(hello, hello + sizeof(hello) - 1)});
    // ...and a later frame the long way round, which proved it too.
    const char again[] = "hello via the router";
    host.socket.inbox.push_back(
        {loopback, std::vector<u8>(again, again + sizeof(again) - 1)});
    // A stranger proved nothing and is still nobody.
    const char stray[] = "not a peer";
    host.socket.inbox.push_back(
        {v4(192, 168, 1, 99, 7717), std::vector<u8>(stray, stray + sizeof(stray) - 1)});
    host.frame(200);

    CHECK(host.connection.receive(buffer, sizeof(buffer), &got, &node));
    CHECK_EQ(int(node), 2);
    CHECK(got == sizeof(hello) - 1 && std::memcmp(buffer, hello, got) == 0);
    CHECK(host.connection.receive(buffer, sizeof(buffer), &got, &node));
    CHECK(got == sizeof(again) - 1 && std::memcmp(buffer, again, got) == 0);
    CHECK(!host.connection.receive(buffer, sizeof(buffer), &got, &node));

    // Answers go to the first path that proved itself, every time, rather
    // than following whichever probe happened to land last.
    lan.inbox.clear();
    const char welcome[] = "welcome";
    CHECK(host.connection.send(2, reinterpret_cast<const u8*>(welcome), sizeof(welcome) - 1));
    bool arrived = false;
    for (const auto& packet : lan.inbox) {
        arrived = arrived
                  || (packet.from == kHostPublic && packet.bytes.size() == sizeof(welcome) - 1
                      && std::memcmp(packet.bytes.data(), welcome, packet.bytes.size()) == 0);
    }
    CHECK(arrived);

    // What the session-ended screen reports: the path, frames each way, how
    // many paths proved themselves, and what came from nobody.
    const std::string report = host.connection.report(2);
    CHECK(report.find("direct 192.168.1.20:7717") != std::string::npos);
    CHECK(report.find("sent 1") != std::string::npos);
    CHECK(report.find("heard 2") != std::string::npos);
    CHECK(report.find("paths 2") != std::string::npos);
    CHECK(report.find("from 192.168.1.99:7717") != std::string::npos);
}

TEST(a_probe_with_the_wrong_token_does_not_become_a_peer)
{
    FakeNet net;
    Console host(&net, kHostPublic, v4(192, 168, 1, 10, 7717), true, "3ds:378243453201");
    Console stranger(&net, kGuestPublic, v4(192, 168, 2, 20, 7717), false, "3ds:180388627432");

    u8 mine[kTokenSize];
    u8 theirs[kTokenSize];
    std::memset(mine, 0x55, sizeof(mine));
    std::memset(theirs, 0x66, sizeof(theirs));

    host.fromServer(punchNow(9, "Guest", kGuestPublic, mine, 3000));
    stranger.fromServer(punchNow(9, "Host", kHostPublic, theirs, 3000));

    run(&host, &stranger, 100, 1000, 50);

    CHECK(!host.connection.connected());
    CHECK(!stranger.connection.connected());
}

// A host is told about a second relayed guest and says no, rather than taking
// an allocation that would break the guest already playing. The limit is
// AlphaComputer's -- it keys relay allocations by session id -- and the console
// keeps to it out loud.
TEST(a_second_relayed_guest_is_refused_rather_than_breaking_the_first)
{
    FakeNet net;
    Console host(&net, kHostPublic, v4(192, 168, 1, 10, 7717), true, "3ds:378243453201");
    Console first(&net, kGuestPublic, v4(192, 168, 2, 20, 7717), false, "3ds:180388627432");
    net.makeUnreachable(kHostPublic);
    net.makeUnreachable(kGuestPublic);

    u8 tokenA[kTokenSize];
    u8 tokenB[kTokenSize];
    std::memset(tokenA, 0x11, sizeof(tokenA));
    std::memset(tokenB, 0x22, sizeof(tokenB));

    host.fromServer(punchNow(9, "First", kGuestPublic, tokenA, 500));
    host.fromServer(punchNow(9, "Second", v4(198, 51, 100, 30, 60000), tokenB, 500));
    run(&host, &first, 100, 900, 50);

    u8 relayToken[kTokenSize];
    std::memset(relayToken, 0xa1, sizeof(relayToken));
    host.fromServer(relayAllocated(9, relayToken));
    host.frame(1000);
    CHECK(host.connection.connected());

    u8 second[kTokenSize];
    std::memset(second, 0xa2, sizeof(second));
    host.fromServer(relayAllocated(9, second));
    host.frame(1100);
    CHECK(!host.connection.message().empty());
}

// Nothing is read off the socket until somebody is playing online, and a
// connection that was never begun neither sends nor receives.
TEST(a_connection_that_was_never_begun_is_inert)
{
    Connection connection;
    for (u32 t = 0; t < 60000; t += 500) {
        connection.pump(t);
    }
    CHECK(!connection.connected());
    CHECK(!connection.punching());
    CHECK(connection.takeEvent() == Event::None);

    u8 buffer[64];
    usize size = 0;
    u16 node = 0;
    CHECK(!connection.receive(buffer, sizeof(buffer), &size, &node));
    CHECK(connection.send(1, buffer, 1));
}
