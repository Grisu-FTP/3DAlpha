// The one socket an online session lives on, and the bind that a console
// refused.
//
// **This exists because of a hardware failure.** The first console run of the
// Profile screen died on `bind(): Invalid argument (errno 22)`. libctru's own
// argument checks cannot produce that here -- disassembling `soc_bind.o` shows
// EINVAL only when `addrlen` is under 8 for `AF_INET`, and this passes 16 -- so
// it was `SOCU:Bind` itself refusing, and the two ways the call differed from
// libctru's own sockets example were `INADDR_ANY` instead of `gethostid()` and
// port 0 instead of a named port. Both are now asked for in order.
//
// A host cannot reproduce the console's refusal, so what is checked here is the
// part that is the same on both: that a bind which cannot succeed is reported
// as words rather than as an errno, and that a bind which can succeed yields a
// local address the server can be offered as a candidate.

#include "core/net/udp_socket.hpp"
#include "framework.hpp"

#include <string>

using namespace mc;
using namespace mc::net;

namespace {

ac::Endpoint loopback(u16 port)
{
    return ac::endpointV4(0x7f000001u, port);
}

}  // namespace

TEST(a_bound_socket_knows_its_own_address_to_offer_as_a_candidate)
{
    UdpSocket socket;
    std::string error;
    CHECK(socket.open(loopback(7717), 0x7f000001u, &error));
    CHECK(error.empty());
    CHECK(socket.isOpen());

    // The local candidate is what lets two consoles on one network talk across
    // it rather than out to the internet and back, so a bound socket that
    // cannot say where it is has lost something.
    const ac::Endpoint local = socket.localEndpoint();
    CHECK(local.valid());
    CHECK(!local.v6);
    CHECK_EQ(ac::endpointText(local).substr(0, 10), std::string("127.0.0.1:"));

    socket.close();
    CHECK(!socket.isOpen());
    CHECK(!socket.localEndpoint().valid());
}

// Two sockets at once: the fixed ports the console may fall back to must not be
// a single one that a second session would collide with.
TEST(two_sockets_get_two_ports)
{
    UdpSocket first;
    UdpSocket second;
    std::string error;
    CHECK(first.open(loopback(7717), 0x7f000001u, &error));
    CHECK(second.open(loopback(7717), 0x7f000001u, &error));
    CHECK(first.localEndpoint().port != second.localEndpoint().port);
}

// **An errno is not an answer.** `bind(): Invalid argument (errno 22)` is what
// a console said when it had no address, and it told the player nothing they
// could act on. The number is kept, because whoever reads a log wants it, but
// the sentence in front of it names the switch to check.
TEST(a_bind_that_cannot_work_is_reported_in_words_a_player_can_act_on)
{
    UdpSocket socket;
    std::string error;
    // An address this machine does not have. Every attempt fails, so this also
    // walks the whole fallback list rather than stopping at the first.
    CHECK(!socket.open(loopback(7717), 0xc0000201u, &error));
    CHECK(!socket.isOpen());
    CHECK(!error.empty());
    CHECK(error.find("wireless") != std::string::npos);
    CHECK(error.find("bind()") != std::string::npos);
}

// Datagrams cross a real socket, both ways, with the sender's address reported
// -- which is what `ac::Connection` sorts the server, the peers and the relay
// apart by.
TEST(a_datagram_crosses_and_says_where_it_came_from)
{
    UdpSocket a;
    UdpSocket b;
    std::string error;
    CHECK(a.open(loopback(7717), 0x7f000001u, &error));
    CHECK(b.open(loopback(7717), 0x7f000001u, &error));

    const u8 payload[] = {'A', 'C', 'M', 'P', 0x10};
    CHECK(a.send(b.localEndpoint(), payload, sizeof(payload)));

    // Non-blocking, so give the loopback a few goes rather than one.
    u8 buffer[64];
    usize size = 0;
    ac::Endpoint from;
    bool got = false;
    for (int attempt = 0; attempt < 200 && !got; ++attempt) {
        got = b.receive(buffer, sizeof(buffer), &size, &from);
    }
    CHECK(got);
    CHECK_EQ(size, sizeof(payload));
    CHECK(from == a.localEndpoint());

    // Nothing waiting is false rather than a block: the frame loop calls this
    // until it empties and must not stop on it.
    CHECK(!b.receive(buffer, sizeof(buffer), &size, &from));
}

// A v6 destination is refused outright rather than failing somewhere less
// obvious: the wire carries v6 addresses and this socket does not.
TEST(a_v6_destination_is_refused_by_a_v4_socket)
{
    UdpSocket socket;
    std::string error;
    CHECK(socket.open(loopback(7717), 0x7f000001u, &error));

    ac::Endpoint v6;
    v6.v6 = true;
    v6.addr[15] = 1;
    v6.port = 7717;
    const u8 payload[] = {0};
    CHECK(!socket.send(v6, payload, sizeof(payload)));
}
