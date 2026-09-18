// The login, the keep-alive and the four errands, against the server's own
// bytes in both directions.
//
// **The vectors are load-bearing here, not decoration.** Every datagram fed in
// is one AlphaComputer's encoder produced, and most of what the client sends
// back is asserted to equal the vector byte for byte -- which is possible
// because the token in `kAcAuthOk*` is the token the client then puts in every
// message after it. So a keep-alive that says the wrong thing, a host request
// with its fields in the wrong order, or an errand that used a stale token all
// fail here rather than on a console with nothing on the screen.

#include "ac_wire_vectors.hpp"
#include "core/net/ac_client.hpp"
#include "core/net/ac_identity.hpp"
#include "core/util/ed25519.hpp"
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

// A socket that goes nowhere: what was sent is kept, and what the test says
// arrived is handed back.
class FakeTransport : public UdpTransport {
public:
    struct Sent {
        Endpoint to;
        std::vector<u8> bytes;
    };

    bool send(const Endpoint& to, const u8* data, usize size) override
    {
        sent.push_back({to, std::vector<u8>(data, data + size)});
        return true;
    }

    bool receive(u8* buffer, usize capacity, usize* size, Endpoint* from) override
    {
        if (inbox.empty()) {
            return false;
        }
        const Sent& next = inbox.front();
        if (next.bytes.size() > capacity) {
            inbox.erase(inbox.begin());
            return false;
        }
        std::memcpy(buffer, next.bytes.data(), next.bytes.size());
        *size = next.bytes.size();
        *from = next.to;
        inbox.erase(inbox.begin());
        return true;
    }

    Endpoint localEndpoint() const override { return local; }

    void deliver(const u8* data, usize size, const Endpoint& from)
    {
        inbox.push_back({from, std::vector<u8>(data, data + size)});
    }

    const std::vector<u8>& last() const { return sent.back().bytes; }
    usize count() const { return sent.size(); }

    std::vector<Sent> sent;
    std::vector<Sent> inbox;
    Endpoint local;
};

// The identity the vectors were generated for, so what the client sends can be
// compared with them directly.
Login vectorLogin()
{
    Login login;
    login.identity = "3ds:000123456789";
    for (usize i = 0; i < sizeof(login.seed); ++i) {
        login.seed[i] = u8(0x31 + i);
    }
    std::memset(login.publicKey, 0x2a, sizeof(login.publicKey));
    login.hasPlatformName = true;
    login.platformName = "Grisu";
    return login;
}

bool same(const std::vector<u8>& got, const u8* expected, usize size)
{
    return got.size() == size && std::memcmp(got.data(), expected, size) == 0;
}

#define CHECK_SENT(transport, vector) CHECK(same((transport).last(), vector, sizeof(vector)))

const Endpoint kServer = v4(203, 0, 113, 1, kDefaultPort);

// Drives a client all the way to Ready using the server's own Challenge and
// AuthOk bytes, and leaves it holding the token those vectors carry.
void logIn(Client* client, FakeTransport* transport, const u8* authOk, usize authOkSize,
           u32 nowMs)
{
    client->begin(kServer, vectorLogin(), transport, nowMs);
    transport->deliver(test::kAcChallenge, sizeof(test::kAcChallenge), kServer);

    u8 datagram[kMaxDatagram];
    Endpoint from;
    usize size = 0;
    while (transport->receive(datagram, sizeof(datagram), &size, &from)) {
        client->onDatagram(datagram, size, nowMs);
    }
    transport->deliver(authOk, authOkSize, kServer);
    while (transport->receive(datagram, sizeof(datagram), &size, &from)) {
        client->onDatagram(datagram, size, nowMs);
    }
}

}  // namespace

// Nothing goes out until somebody asks for it. A player who never opens the
// Profile screen and never picks Internet never sends a packet -- which is the
// whole of what "single player does not wait for a server" means in code.
TEST(a_client_that_was_never_begun_sends_nothing_and_waits_for_nothing)
{
    FakeTransport transport;
    Client client;

    CHECK(client.state() == State::Idle);
    for (u32 t = 0; t < 120000; t += 100) {
        client.pump(t);
    }
    CHECK_EQ(int(transport.count()), 0);
    CHECK(client.state() == State::Idle);

    // Errands pressed on an idle client are dropped rather than queued for a
    // login that is never going to happen.
    client.requestLinkCode();
    client.unlink();
    client.listSessions();
    client.pump(1000);
    CHECK_EQ(int(transport.count()), 0);
}

TEST(a_login_is_a_hello_then_the_challenge_signed_with_the_consoles_key)
{
    FakeTransport transport;
    Client client;
    client.begin(kServer, vectorLogin(), &transport, 0);

    CHECK_EQ(int(transport.count()), 1);
    CHECK(transport.sent[0].to == kServer);
    CHECK_SENT(transport, test::kAcHello);
    CHECK(client.state() == State::Connecting);

    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 0);
    CHECK_EQ(int(transport.count()), 2);

    // The address the server saw, free of charge and with no STUN server in it.
    CHECK(client.reflexive() == v4(203, 0, 113, 7, 51000));

    // The Auth carries the signature over the domain, the identity and the
    // nonce -- the payload the server rebuilds. Recomputed here rather than
    // taken from the vector, whose signature was a filler byte repeated.
    u8 nonce[kNonceSize];
    std::memset(nonce, 0x7e, sizeof(nonce));
    std::vector<u8> payload;
    authPayload("3ds:000123456789", nonce, &payload);

    const Login login = vectorLogin();
    u8 expected[util::kEd25519SignatureSize];
    util::ed25519Sign(login.seed, login.publicKey, payload.data(), payload.size(), expected);

    const std::vector<u8>& auth = transport.last();
    CHECK_EQ(int(auth[4]), int(ClientKind::Auth));
    CHECK(std::memcmp(auth.data() + 5, expected, sizeof(expected)) == 0);
    // The friend-list name rides along, which is what the server falls back to
    // when a console is not linked to an account.
    CHECK_EQ(int(auth[5 + 64]), 1);

    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    CHECK(client.ready());
    CHECK_EQ(client.displayName(), std::string("Steve#A1B2"));
    CHECK(client.accountHandle().empty());
    CHECK(client.firstClaim());
    CHECK(client.takeEvent() == Event::LoggedIn);
}

TEST(an_unanswered_hello_is_repeated_and_then_given_up_on)
{
    FakeTransport transport;
    Client client;
    client.begin(kServer, vectorLogin(), &transport, 0);
    CHECK_EQ(int(transport.count()), 1);

    // Nothing before the retry is due, and exactly one more after it.
    client.pump(kRetryMs - 1);
    CHECK_EQ(int(transport.count()), 1);
    client.pump(kRetryMs);
    CHECK_EQ(int(transport.count()), 2);
    CHECK_SENT(transport, test::kAcHello);

    // Pumped a frame at a time, as a running loop does: a jump this long in one
    // step would be a suspension, which is not the server's silence.
    for (u32 t = kRetryMs + 100; t <= kRequestTimeoutMs + 100; t += 100) {
        client.pump(t);
    }
    CHECK(client.state() == State::Failed);
    CHECK(!client.message().empty());

    // A failed client stays failed and stays quiet rather than hammering a
    // server that is not there.
    const usize settled = transport.count();
    for (u32 t = kRequestTimeoutMs + 2; t < 60000; t += 500) {
        client.pump(t);
    }
    CHECK_EQ(int(transport.count()), int(settled));
}

// The word the server used, not a word of this build's own. "Already bound to a
// different key" is the one refusal that will say the same thing forever, and a
// player is owed the real sentence.
TEST(a_refused_login_says_what_the_server_said)
{
    FakeTransport transport;
    Client client;
    client.begin(kServer, vectorLogin(), &transport, 0);
    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 0);
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 0);

    CHECK(client.state() == State::Failed);
    CHECK_EQ(client.message(), std::string("already bound to a different key"));
    CHECK(client.takeEvent() == Event::Refused);
}

// **The server answers every Auth, and only the first gets the login.** An
// AuthOk slower than `kRetryMs` means a second Auth went out; the server has
// spent the challenge on the first, so it answers the copy with "say hello
// first". That refusal is on its way when the login has already landed, and it
// used to take the login down with it -- seen against the real server as a
// connection that went straight from connecting to failed.
TEST(a_refusal_of_a_repeated_auth_does_not_end_the_login_it_arrives_after)
{
    FakeTransport transport;
    Client client;
    client.begin(kServer, vectorLogin(), &transport, 0);
    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 0);
    client.pump(kRetryMs);
    CHECK_EQ(int(transport.last()[4]), int(ClientKind::Auth));  // the copy
    CHECK_EQ(int(transport.count()), 3);                        // Hello, Auth, Auth

    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), kRetryMs + 20);
    CHECK(client.ready());
    CHECK(client.takeEvent() == Event::LoggedIn);
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), kRetryMs + 40);
    CHECK(client.ready());
    CHECK(client.takeEvent() == Event::None);

    // One copy, one allowance. A second refusal is not one the copies explain:
    // a logged-in console that is refused has been forgotten by the server, and
    // logs in again.
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), kRetryMs + 60);
    CHECK(client.state() == State::Connecting);
    CHECK_SENT(transport, test::kAcHello);
}

// **The server forgot this console while it was not listening.** A keyboard
// applet held open for over a minute leaves the server with a login it has not
// heard from, and it drops it; the join typed in that keyboard is then refused
// as "unknown or expired session". The client logs in again and sends the join
// it was asked for, rather than handing the player a failure.
TEST(a_join_refused_because_the_login_expired_logs_in_again_and_is_sent_again)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    CHECK(client.takeEvent() == Event::LoggedIn);

    client.joinByCode("ABC234");
    CHECK_EQ(int(transport.last()[4]), int(ClientKind::JoinRequest));
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 10);

    CHECK(client.state() == State::Connecting);
    CHECK_SENT(transport, test::kAcHello);

    // The keep-alive sent on the same dead token is refused too, and lands
    // before the Challenge does. It must not end the re-login it started.
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 15);
    CHECK(client.state() == State::Connecting);
    CHECK(client.takeEvent() == Event::None);  // nothing for the screen to show

    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 20);
    CHECK_EQ(int(transport.last()[4]), int(ClientKind::Auth));
    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 30);
    CHECK(client.ready());
    CHECK(client.takeEvent() == Event::LoggedIn);

    client.pump(40);
    CHECK_EQ(int(transport.last()[4]), int(ClientKind::JoinRequest));
    client.onDatagram(test::kAcPunchNow, sizeof(test::kAcPunchNow), 60);
    CHECK(client.takeEvent() == Event::PunchNow);
}

// A keep-alive is refused the same way, with nothing asked for: the console
// logs in again and asks for nothing afterwards.
TEST(a_refused_keepalive_logs_in_again_with_nothing_to_resend)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    (void)client.takeEvent();
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 10);
    CHECK(client.state() == State::Connecting);

    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 20);
    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 30);
    CHECK(client.ready());
    const usize settled = transport.count();
    client.pump(40);
    CHECK_EQ(int(transport.count()), int(settled));
}

// **Once, not in a loop.** A server that refuses the console again straight
// after letting it back in is saying something, and the second refusal is
// believed -- with the server's own words on the screen.
TEST(a_second_expiry_straight_after_a_re_login_is_believed)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 10);
    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 20);
    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 30);
    CHECK(client.ready());

    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 40);
    CHECK(client.state() == State::Failed);
    CHECK_EQ(client.message(), std::string("already bound to a different key"));
}

// Past the guard it is a new expiry, and gets a new re-login.
TEST(an_expiry_long_after_the_last_re_login_logs_in_again_too)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 10);
    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 20);
    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 30);

    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), 10 + kReloginGuardMs);
    CHECK(client.state() == State::Connecting);
}

// **A repeated Hello replaces the challenge** the first was answered with, so
// the Auth signed over the first nonce is refused. That is started over once
// with a fresh Hello; a refusal that comes back the second time is real.
TEST(a_refusal_after_a_repeated_hello_starts_the_login_over_once)
{
    FakeTransport transport;
    Client client;
    client.begin(kServer, vectorLogin(), &transport, 0);
    client.pump(kRetryMs);
    CHECK_EQ(int(transport.count()), 2);  // Hello twice
    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), kRetryMs + 10);
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), kRetryMs + 20);

    CHECK(client.state() == State::Connecting);
    CHECK_SENT(transport, test::kAcHello);

    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), kRetryMs + 30);
    client.onDatagram(test::kAcAuthFail, sizeof(test::kAcAuthFail), kRetryMs + 40);
    CHECK(client.state() == State::Failed);
    CHECK_EQ(client.message(), std::string("already bound to a different key"));
}

// **A request's six seconds start when it is sent**, not when the one before
// it was. The join is asked for after the player has typed a code, which is
// long after the login; timed from the login, the join had "run out" before it
// left, and the first pump that did not already hold the answer failed it with
// "The server did not answer". A wrong code survived -- the server refuses one
// on the spot -- and a right one did not, because the server writes to its
// database before it introduces two consoles.
TEST(a_request_made_long_after_the_login_is_timed_from_when_it_was_sent)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    (void)client.takeEvent();
    for (u32 t = 100; t <= 20000; t += 100) {
        client.pump(t);
        client.onDatagram(test::kAcKeepaliveAck, sizeof(test::kAcKeepaliveAck), t);
    }

    client.joinByCode("ABC234");
    client.pump(20033);
    client.pump(20066);
    CHECK(client.ready());
    CHECK(client.takeEvent() == Event::None);

    client.onDatagram(test::kAcPunchNow, sizeof(test::kAcPunchNow), 20100);
    CHECK(client.takeEvent() == Event::PunchNow);
    CHECK(client.ready());
}

// **A console that was suspended did not hear anything because it was not
// listening.** Typing a code is a keyboard applet, and HOME is another: nothing
// pumps while either is up. The time away is not held against the server --
// neither the login's silence allowance nor a request's six seconds -- and the
// first thing back is a keep-alive, which re-teaches a NAT that may have moved.
TEST(time_spent_suspended_is_not_counted_against_the_server)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    (void)client.takeEvent();
    client.pump(100);

    // A minute in the keyboard, then the code it produced.
    client.joinByCode("ABC234");
    const usize before = transport.count();
    client.pump(60100);
    CHECK(client.ready());
    CHECK(transport.count() > before);  // the keep-alive, straight away
    client.pump(60133);
    CHECK(client.ready());

    client.onDatagram(test::kAcPunchNow, sizeof(test::kAcPunchNow), 60200);
    CHECK(client.takeEvent() == Event::PunchNow);
}

TEST(a_logged_in_console_says_it_is_still_there_every_ten_seconds)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    const usize afterLogin = transport.count();

    for (u32 t = 100; t < kKeepAliveMs; t += 100) {
        client.pump(t);
    }
    client.pump(kKeepAliveMs - 1);
    CHECK_EQ(int(transport.count()), int(afterLogin));

    client.pump(kKeepAliveMs);
    CHECK_EQ(int(transport.count()), int(afterLogin) + 1);
    // Byte for byte the server's own Keepalive, token included -- which is the
    // token the AuthOk vector handed over.
    CHECK_SENT(transport, test::kAcKeepalive);

    // The answer carries the address again, so the server re-learns a mapping
    // that moved while a lid was shut.
    client.onDatagram(test::kAcKeepaliveAck, sizeof(test::kAcKeepaliveAck), kKeepAliveMs);
    CHECK(client.reflexive().v6);
}

TEST(silence_from_the_server_eventually_ends_the_session)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);

    for (u32 t = 0; t <= kServerTimeoutMs; t += 1000) {
        client.pump(t);
        CHECK(client.ready());
    }
    client.pump(kServerTimeoutMs + 1001);
    CHECK(client.state() == State::Failed);
}

TEST(link_and_unlink_are_the_two_errands_the_profile_screen_runs)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkLinked, sizeof(test::kAcAuthOkLinked), 0);

    // Linked: the screen's button says Unlink.
    CHECK_EQ(client.accountHandle(), std::string("Grisu"));
    CHECK_EQ(client.displayName(), std::string("Grisu the Builder"));
    CHECK(client.takeEvent() == Event::LoggedIn);

    client.unlink();
    client.pump(10);
    CHECK_SENT(transport, test::kAcUnlink);

    client.onDatagram(test::kAcUnlinked, sizeof(test::kAcUnlinked), 20);
    CHECK(client.takeEvent() == Event::Unlinked);
    CHECK(client.accountHandle().empty());
    // The name the server says this console has now, rather than a blank the
    // screen would have to invent something for.
    CHECK_EQ(client.displayName(), std::string("Steve#A1B2"));

    // Not linked: the button says Link, and pressing it asks for a code.
    client.requestLinkCode();
    client.pump(30);
    CHECK_SENT(transport, test::kAcRequestLinkCode);

    client.onDatagram(test::kAcLinkCode, sizeof(test::kAcLinkCode), 40);
    CHECK(client.takeEvent() == Event::LinkCodeIssued);
    CHECK_EQ(client.linkCode(), std::string("K7P2M4QX"));
    CHECK_EQ(int(client.linkCodeSeconds()), 600);
}

// The Profile screen opens and the player presses Link on the next frame, long
// before a 3DS on wireless has finished saying hello. Holding the errand until
// the login lands is what makes that press do something.
TEST(an_errand_pressed_before_the_login_lands_is_held_and_then_sent)
{
    FakeTransport transport;
    Client client;
    client.begin(kServer, vectorLogin(), &transport, 0);
    client.requestLinkCode();
    client.pump(5);
    // Nothing but the Hello: there is no token to put in a request yet.
    CHECK_EQ(int(transport.count()), 1);

    client.onDatagram(test::kAcChallenge, sizeof(test::kAcChallenge), 10);
    client.onDatagram(test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 20);
    CHECK(client.ready());

    client.pump(30);
    CHECK_SENT(transport, test::kAcRequestLinkCode);
}

TEST(hosting_listing_and_joining_are_the_servers_own_bytes)
{
    FakeTransport transport;
    transport.local = v4(192, 168, 1, 50, 7717);
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    (void)client.takeEvent();

    // Locked, which is this build's default: a world is joined with the six
    // characters its host reads out, not walked into off a public list.
    client.hostSession("New World", 4, true);
    client.pump(10);
    CHECK_SENT(transport, test::kAcHostSession);

    client.onDatagram(test::kAcSessionOpened, sizeof(test::kAcSessionOpened), 20);
    CHECK(client.takeEvent() == Event::SessionOpened);
    CHECK_EQ(client.joinCode(), std::string("PIGLIN"));
    CHECK_EQ(int(client.sessionId()), 9);

    client.listSessions();
    client.pump(30);
    CHECK_SENT(transport, test::kAcListSessions);
    client.onDatagram(test::kAcSessionList, sizeof(test::kAcSessionList), 40);
    CHECK(client.takeEvent() == Event::SessionsListed);
    CHECK_EQ(int(client.sessions().size()), 2);
    CHECK_EQ(client.sessions()[0].worldName, std::string("New World"));

    client.joinByCode("PIGLIN");
    client.pump(50);
    CHECK_SENT(transport, test::kAcJoinByCode);

    // The introduction arrives at both consoles at once; the joiner's own
    // request stops waiting on it.
    client.onDatagram(test::kAcPunchNow, sizeof(test::kAcPunchNow), 60);
    CHECK(client.takeEvent() == Event::PunchNow);
    CHECK_EQ(client.introduction().peerName, std::string("Steve#A1B2"));
    CHECK_EQ(int(client.introduction().candidates.size()), 2);
}

// A refusal ends the errand and leaves the login alone: "no such session" is
// not a reason to log in again.
TEST(a_refused_errand_does_not_end_the_session)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    (void)client.takeEvent();

    client.joinByCode("ZZZZZZ");
    client.pump(10);
    client.onDatagram(test::kAcError, sizeof(test::kAcError), 20);

    CHECK(client.takeEvent() == Event::Refused);
    CHECK_EQ(client.message(), std::string("no such session"));
    CHECK(client.ready());

    // And the next errand still works.
    client.requestLinkCode();
    client.pump(30);
    CHECK_SENT(transport, test::kAcRequestLinkCode);
}

// Junk on the port is the ordinary case, not the exceptional one.
TEST(a_malformed_datagram_from_the_server_is_counted_and_ignored)
{
    FakeTransport transport;
    Client client;
    logIn(&client, &transport, test::kAcAuthOkUnlinked, sizeof(test::kAcAuthOkUnlinked), 0);
    (void)client.takeEvent();

    const u8 junk[] = {'h', 'e', 'l', 'l', 'o'};
    CHECK(!client.onDatagram(junk, sizeof(junk), 100));
    CHECK(client.ready());
    CHECK(client.takeEvent() == Event::None);
}
