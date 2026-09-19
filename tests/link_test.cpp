// The local-session link: what it does when frames are lost, arrive twice, or
// arrive in the wrong order, and what a host and a guest say to each other.
//
// Every test here runs both ends in one process over a `Wire` that is as
// unhelpful as it is told to be. That is the point: the console's own local
// wireless cannot be made to drop the third frame on purpose, and these are
// the cases the link exists for.

#include "framework.hpp"

#include "core/net/link.hpp"
#include "core/net/session.hpp"

#include <deque>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net::link;

namespace {

// A frame in flight, with the node it is for.
struct Frame {
    u16 node = 0;
    std::vector<u8> bytes;
};

// Two ends of one link, each seeing what the other sent. `dropEvery` throws
// away every Nth frame put on the wire, and `reorder` hands the newest frame
// over before the ones waiting behind it.
class Wire {
public:
    class End : public Datagrams {
    public:
        End(Wire* wire, u16 self) : wire_(wire), self_(self) {}

        bool send(u16 node, const u8* data, usize size) override
        {
            return wire_->carry(self_, node, data, size);
        }

        bool receive(u8* buffer, usize capacity, usize* size, u16* node) override
        {
            return wire_->take(self_, buffer, capacity, size, node);
        }

    private:
        Wire* wire_;
        u16 self_;
    };

    Wire() : host(this, kHostNode), guest(this, kGuestNode) {}

    bool carry(u16 from, u16 to, const u8* data, usize size)
    {
        (void)to;
        ++carried;
        if (dropEvery > 0 && carried % dropEvery == 0) {
            ++dropped;
            return true;  // lost on the air, which is not a failure to send
        }
        std::deque<Frame>& queue = from == kHostNode ? toGuest_ : toHost_;
        queue.push_back(Frame{from, std::vector<u8>(data, data + size)});
        return true;
    }

    bool take(u16 self, u8* buffer, usize capacity, usize* size, u16* node)
    {
        std::deque<Frame>& queue = self == kHostNode ? toHost_ : toGuest_;
        if (queue.empty()) {
            return false;
        }
        Frame frame = reorder && queue.size() > 1 ? queue.back() : queue.front();
        if (reorder && queue.size() > 1) {
            queue.pop_back();
        } else {
            queue.pop_front();
        }
        if (frame.bytes.size() > capacity) {
            return false;
        }
        for (usize i = 0; i < frame.bytes.size(); ++i) {
            buffer[i] = frame.bytes[i];
        }
        *size = frame.bytes.size();
        *node = frame.node;
        return true;
    }

    static constexpr u16 kGuestNode = 2;

    End host;
    End guest;
    int dropEvery = 0;
    bool reorder = false;
    int carried = 0;
    int dropped = 0;

private:
    std::deque<Frame> toHost_;
    std::deque<Frame> toGuest_;
};

// Everything a session told the game, in order, as text -- so a test can say
// what it expects to have happened without a mock per callback.
class Log : public SessionListener {
public:
    void onPlayerJoined(u8 playerId, const std::string& name) override
    {
        lines.push_back("join " + std::to_string(playerId) + " " + name);
    }

    void onPlayerLeft(u8 playerId, const std::string& reason) override
    {
        (void)reason;
        lines.push_back("left " + std::to_string(playerId));
    }

    void onChat(u8 playerId, const std::string& text) override
    {
        lines.push_back("chat " + std::to_string(playerId) + " " + text);
    }

    void onPose(const Pose& pose) override
    {
        lines.push_back("pose " + std::to_string(pose.playerId) + " "
                        + std::to_string(pose.x));
    }

    void onGamePacket(u8 playerId, const u8* data, usize size) override
    {
        lines.push_back("packet " + std::to_string(playerId) + " "
                        + std::to_string(int(size)) + " " + std::to_string(int(data[0])));
    }

    bool has(const std::string& line) const
    {
        for (const std::string& seen : lines) {
            if (seen == line) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> lines;
};

// Messages a bare `Peer` delivered.
struct Sunk {
    std::vector<std::pair<Msg, std::string>> messages;
};

void collect(void* ctx, Msg kind, const u8* body, usize size)
{
    static_cast<Sunk*>(ctx)->messages.push_back(
        {kind, std::string(reinterpret_cast<const char*>(body), size)});
}

// The world both ends claim to be able to generate. Same seed, same options,
// same build, so a guest is offered terrain work.
GeneratorId testWorld()
{
    GeneratorId id;
    id.seed = 1234567890;
    id.options = 0;
    id.version = "test-gen";
    return id;
}

// Moves both ends forward together, the way one frame of the game would.
void step(Wire& wire, HostSession& host, GuestSession& guest, u32* nowMs, int frames = 1,
          u32 stepMs = 16)
{
    for (int i = 0; i < frames; ++i) {
        *nowMs += stepMs;
        host.pump(*nowMs, wire.host);
        guest.pump(*nowMs, wire.guest);
    }
}

}  // namespace

TEST(link_delivers_messages_in_the_order_they_were_queued)
{
    Peer sender;
    Peer receiver;
    sender.reset(0);
    receiver.reset(0);

    const u8 first[] = {1, 2, 3};
    const u8 second[] = {4};
    CHECK(sender.queue(Msg::Chat, first, sizeof(first), true));
    CHECK(sender.queue(Msg::GamePacket, second, sizeof(second), true));

    u8 datagram[kMaxDatagram];
    usize size = 0;
    Sunk sunk;
    while (sender.nextDatagram(0, datagram, sizeof(datagram), &size)) {
        CHECK(receiver.receive(datagram, size, 0, &collect, &sunk));
    }

    CHECK(sunk.messages.size() == 2);
    CHECK(sunk.messages[0].first == Msg::Chat);
    CHECK(sunk.messages[0].second == std::string("\x01\x02\x03", 3));
    CHECK(sunk.messages[1].first == Msg::GamePacket);
}

TEST(link_holds_an_early_datagram_until_the_gap_before_it_is_filled)
{
    Peer sender;
    Peer receiver;
    sender.reset(0);
    receiver.reset(0);

    // Three datagrams, each its own message: a full one, then two more.
    std::vector<u8> big(kMaxMessage, 0x7F);
    for (int i = 0; i < 3; ++i) {
        big[0] = u8(i);
        CHECK(sender.queue(Msg::GamePacket, big.data(), big.size(), true));
    }

    std::vector<std::vector<u8>> wire;
    u8 datagram[kMaxDatagram];
    usize size = 0;
    while (sender.nextDatagram(0, datagram, sizeof(datagram), &size)) {
        wire.push_back(std::vector<u8>(datagram, datagram + size));
    }
    CHECK(wire.size() == 3);

    // The middle one arrives last.
    Sunk sunk;
    CHECK(receiver.receive(wire[0].data(), wire[0].size(), 0, &collect, &sunk));
    CHECK(receiver.receive(wire[2].data(), wire[2].size(), 0, &collect, &sunk));
    CHECK(sunk.messages.size() == 1);  // the third is held, not delivered early
    CHECK(receiver.receive(wire[1].data(), wire[1].size(), 0, &collect, &sunk));
    CHECK(sunk.messages.size() == 3);
    CHECK(u8(sunk.messages[0].second[0]) == 0);
    CHECK(u8(sunk.messages[1].second[0]) == 1);
    CHECK(u8(sunk.messages[2].second[0]) == 2);
}

TEST(link_delivers_a_duplicated_datagram_once)
{
    Peer sender;
    Peer receiver;
    sender.reset(0);
    receiver.reset(0);

    const u8 body[] = {9};
    CHECK(sender.queue(Msg::Chat, body, sizeof(body), true));

    u8 datagram[kMaxDatagram];
    usize size = 0;
    CHECK(sender.nextDatagram(0, datagram, sizeof(datagram), &size));

    Sunk sunk;
    CHECK(receiver.receive(datagram, size, 0, &collect, &sunk));
    CHECK(receiver.receive(datagram, size, 0, &collect, &sunk));
    CHECK(sunk.messages.size() == 1);
}

TEST(link_resends_a_reliable_message_until_it_is_acknowledged)
{
    Peer sender;
    sender.reset(0);
    const u8 body[] = {1};
    CHECK(sender.queue(Msg::Chat, body, sizeof(body), true));

    u8 datagram[kMaxDatagram];
    usize size = 0;
    CHECK(sender.nextDatagram(0, datagram, sizeof(datagram), &size));
    // Nothing more until the retransmission timer is up.
    CHECK(!sender.nextDatagram(10, datagram, sizeof(datagram), &size));
    CHECK(sender.nextDatagram(kMaxRetryMs, datagram, sizeof(datagram), &size));
    CHECK(sender.retransmits() == 1);
    CHECK(sender.inFlight() == 1);

    // Delivered at last: the receiver's acknowledgement clears the window.
    Peer receiver;
    receiver.reset(0);
    Sunk sunk;
    CHECK(receiver.receive(datagram, size, 0, &collect, &sunk));
    usize back = 0;
    CHECK(receiver.nextDatagram(kKeepAliveMs, datagram, sizeof(datagram), &back));
    CHECK(sender.receive(datagram, back, kMaxRetryMs, nullptr, nullptr));
    CHECK(sender.inFlight() == 0);
}

TEST(link_never_resends_an_unreliable_message)
{
    Peer sender;
    sender.reset(0);
    const u8 body[] = {1};
    CHECK(sender.queue(Msg::Pose, body, sizeof(body), false));

    u8 datagram[kMaxDatagram];
    usize size = 0;
    CHECK(sender.nextDatagram(0, datagram, sizeof(datagram), &size));
    CHECK(sender.inFlight() == 0);
    // Only the keep-alive is left to send, and never the pose again.
    CHECK(!sender.nextDatagram(1, datagram, sizeof(datagram), &size));
    CHECK(sender.nextDatagram(kKeepAliveMs, datagram, sizeof(datagram), &size));
    CHECK(size == kHeaderSize);
}

TEST(link_calls_a_peer_gone_after_the_timeout_and_not_before)
{
    Peer peer;
    peer.reset(1000);
    CHECK(!peer.timedOut(1000 + kTimeoutMs - 1));
    CHECK(peer.timedOut(1000 + kTimeoutMs));
}

TEST(a_guest_joining_is_welcomed_with_the_world_and_the_player_list)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    CHECK(guest.state() == GuestSession::State::Playing);
    CHECK(guest.worldName() == "Hollow Hill");
    CHECK(guest.hostName() == "Grisu");
    CHECK(guest.playerId() == kHostPlayerId + 1);
    CHECK(host.guestCount() == 1);
    CHECK(hostLog.has("join 2 Ada"));
    // The guest knows about itself and about the host.
    CHECK(guest.players().size() == 2);
    CHECK(guestLog.has("join 1 Grisu"));
}

TEST(a_guest_keeps_all_24_characters_of_an_alphacomputer_display_name)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada the Very Long Builder", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    CHECK(guest.state() == GuestSession::State::Playing);
    CHECK(hostLog.has("join 2 Ada_the_Very_Long_Builde"));
}

TEST(a_session_survives_losing_every_third_frame)
{
    Wire wire;
    wire.dropEvery = 3;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 200);

    CHECK(wire.dropped > 0);
    CHECK(guest.state() == GuestSession::State::Playing);

    guest.say("hello from over here");
    host.say("and from over here");
    step(wire, host, guest, &now, 200);

    CHECK(hostLog.has("chat 2 hello from over here"));
    CHECK(guestLog.has("chat 1 and from over here"));
}

TEST(a_session_survives_frames_arriving_in_the_wrong_order)
{
    Wire wire;
    wire.reorder = true;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 60);
    CHECK(guest.state() == GuestSession::State::Playing);

    for (int i = 0; i < 8; ++i) {
        guest.say("line " + std::to_string(i));
    }
    step(wire, host, guest, &now, 60);

    // In order, whatever order the air put them in.
    int seen = 0;
    for (const std::string& line : hostLog.lines) {
        const std::string want = "chat 2 line " + std::to_string(seen);
        if (line == want) {
            ++seen;
        }
    }
    CHECK(seen == 8);
}

TEST(a_pose_is_delivered_but_a_stale_one_is_not_resent)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    Pose pose;
    pose.x = 128;
    guest.reportPose(pose);
    step(wire, host, guest, &now, 2);
    CHECK(hostLog.has("pose 2 128"));

    host.reportPose(pose);
    step(wire, host, guest, &now, 2);
    CHECK(guestLog.has("pose 1 128"));
}

TEST(a_guest_on_a_different_protocol_is_turned_away_with_a_reason)
{
    Wire wire;
    HostSession host;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;
    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);

    // A guest from another build: the same link, a protocol that is not ours.
    Peer peer;
    peer.reset(now);
    std::vector<u8> body;
    encode(Hello{u16(kProtocol + 1), "Ada", "test-gen"}, &body);
    CHECK(peer.queue(Msg::Hello, body, true));
    CHECK(peer.flush(now, wire.guest, kHostNode));

    now += 16;
    host.pump(now, wire.host);

    Sunk sunk;
    u8 datagram[kMaxDatagram];
    usize size = 0;
    u16 node = 0;
    while (wire.guest.receive(datagram, sizeof(datagram), &size, &node)) {
        CHECK(peer.receive(datagram, size, now, &collect, &sunk));
    }
    CHECK(!sunk.messages.empty());
    CHECK(sunk.messages[0].first == Msg::Reject);
    Reject reject;
    CHECK(decode(reinterpret_cast<const u8*>(sunk.messages[0].second.data()),
                 sunk.messages[0].second.size(), &reject));
    CHECK(!reject.reason.empty());
    CHECK(host.guestCount() == 0);
}

TEST(a_guest_that_stops_answering_is_dropped_and_the_others_are_told)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);
    CHECK(host.guestCount() == 1);

    // The console walks out of range: the host goes on pumping, nothing comes.
    //
    // **Pumped a frame at a time, because that is the thing being tested.** A
    // host that jumped its own clock by ten seconds between two pumps was not
    // ignored for ten seconds -- it was *stopped* for ten seconds, and the
    // link forgives that rather than blaming the peer for it. See
    // `HostSession::forgiveStall`.
    const u32 until = now + kTimeoutMs + 100;
    while (now < until && host.guestCount() != 0) {
        now += 16;
        host.pump(now, wire.host);
    }
    CHECK(host.guestCount() == 0);
    CHECK(hostLog.has("left 2"));
}

TEST(a_console_that_said_it_was_going_quiet_is_not_dropped_for_going_quiet)
{
    // A library applet suspends the whole application: no thread runs and
    // nothing answers, for as long as somebody is typing. Ten seconds of that
    // is ordinary, and it used to end the session. See `Msg::Away`.
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);
    CHECK(host.guestCount() == 1);

    // The guest is about to open a keyboard, and says so.
    guest.announceAway(kAppletAwayMs, now, wire.guest);
    now += 16;
    host.pump(now, wire.host);

    // Now it stops answering for longer than the timeout.
    const u32 until = now + kTimeoutMs + 2000;
    while (now < until) {
        now += 16;
        host.pump(now, wire.host);
    }
    CHECK(host.guestCount() == 1);

    // But the grace is an interval and not a switch: a console that never
    // comes back is still dropped, later.
    const u32 end = now + kMaxAwayMs + 1000;
    while (now < end && host.guestCount() != 0) {
        now += 16;
        host.pump(now, wire.host);
    }
    CHECK(host.guestCount() == 0);
}

TEST(time_this_console_never_saw_is_not_counted_against_the_peer)
{
    // The other half of the same problem: the console that was suspended must
    // not come back and decide, on its first frame, that everybody else has
    // gone. See `Peer::forgive`.
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);
    CHECK(host.guestCount() == 1);

    // The host is suspended for half a minute and pumps once on the way back.
    now += 30000;
    host.pump(now, wire.host);
    CHECK(host.guestCount() == 1);

    // And the link still works afterwards.
    step(wire, host, guest, &now, 4);
    CHECK(host.guestCount() == 1);
    CHECK(guest.state() == GuestSession::State::Playing);
}

TEST(a_host_that_closes_tells_the_guest_why)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    host.close("the host left the world", now, wire.host);
    step(wire, host, guest, &now, 2);

    CHECK(guest.state() == GuestSession::State::Finished);
    CHECK(guest.reason() == "the host left the world");
}

TEST(a_game_packet_crosses_the_link_untouched)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    // A Block Change, as core/net/packets.hpp encodes one: the link is not
    // supposed to know or care what is inside.
    const u8 packet[] = {0x35, 0, 0, 0, 1, 64, 0, 0, 0, 2, 3, 0};
    CHECK(host.sendGamePacket(0, packet, sizeof(packet)));
    step(wire, host, guest, &now, 2);
    CHECK(guestLog.has("packet 1 12 53"));
}

TEST(a_guest_is_told_how_the_host_plays_its_world_and_hears_when_it_changes)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    // **Set before anybody joins**, which is where a host sets it: the world
    // has to be open for its settings to have been read.
    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    host.setWorldRules(WorldRules{2, 3});  // Creative, Hard
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    CHECK(guest.state() == GuestSession::State::Playing);
    CHECK(guest.worldRules().gamemode == 2);
    CHECK(guest.worldRules().difficulty == 3);

    // The pause menu can turn a world Survival without anybody leaving it.
    host.setWorldRules(WorldRules{1, 1});
    step(wire, host, guest, &now, 4);
    CHECK(guest.worldRules().gamemode == 1);
    CHECK(guest.worldRules().difficulty == 1);
}

TEST(a_session_that_never_says_how_it_plays_welcomes_into_the_first_gamemode)
{
    Wire wire;
    HostSession host;
    GuestSession guest;
    Log hostLog;
    Log guestLog;
    u32 now = 1000;

    host.open("Hollow Hill", "Grisu", testWorld(), now, &hostLog);
    guest.join("Ada", testWorld(), now, &guestLog);
    step(wire, host, guest, &now, 4);

    CHECK(guest.worldRules().gamemode == 0);
    CHECK(guest.worldRules().difficulty == 0);
}
