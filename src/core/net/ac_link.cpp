#include "core/net/ac_link.hpp"

#include <cstring>
#include <string>

namespace mc::net::ac {

namespace {

constexpr u8 kProbePing = 0;
constexpr u8 kProbePong = 1;

}  // namespace

Connection::Connection() = default;
Connection::~Connection() = default;

void Connection::begin(const Endpoint& server, const Login& login, UdpTransport* transport,
                       bool hosting, u32 nowMs)
{
    transport_ = transport;
    server_ = server;
    hosting_ = hosting;
    nowMs_ = nowMs;
    peerName_.clear();
    message_.clear();
    overflows_ = 0;

    for (Peer& peer : peers_) {
        peer = Peer();
    }

    // Everything this object will ever need, taken here: nothing below
    // allocates, because the frame path runs inside a game loop.
    inbox_.assign(usize(kInboxSlots) * link::kMaxDatagram, 0);
    inboxHead_ = 0;
    inboxCount_ = 0;
    strays_ = 0;
    lastStray_ = Endpoint();
    outgoing_.reserve(kTokenSize + link::kMaxDatagram);

    client_.begin(server, login, transport, nowMs);
}

void Connection::end()
{
    client_.closeSession();
    client_.pump(nowMs_);
    client_.end();
    transport_ = nullptr;
    for (Peer& peer : peers_) {
        peer = Peer();
    }
    inbox_.clear();
    inbox_.shrink_to_fit();
    inboxCount_ = 0;
}

Connection::Peer* Connection::peerForNode(u16 node)
{
    for (Peer& peer : peers_) {
        if (peer.used && peer.node == node) {
            return &peer;
        }
    }
    return nullptr;
}

const Connection::Peer* Connection::peerForNode(u16 node) const
{
    for (const Peer& peer : peers_) {
        if (peer.used && peer.node == node) {
            return &peer;
        }
    }
    return nullptr;
}

Connection::Peer* Connection::peerForAddress(const Endpoint& from)
{
    for (Peer& peer : peers_) {
        if (!peer.used || !peer.connected) {
            continue;
        }
        if (peer.addr == from) {
            return &peer;
        }
        for (int i = 0; i < peer.provenCount; ++i) {
            if (peer.proven[i] == from) {
                return &peer;
            }
        }
    }
    return nullptr;
}

Connection::Peer* Connection::peerForToken(const u8 token[kTokenSize])
{
    for (Peer& peer : peers_) {
        if (peer.used && std::memcmp(peer.punchToken, token, kTokenSize) == 0) {
            return &peer;
        }
    }
    return nullptr;
}

Connection::Peer* Connection::freePeer()
{
    for (Peer& peer : peers_) {
        if (!peer.used) {
            return &peer;
        }
    }
    return nullptr;
}

bool Connection::connected() const
{
    for (const Peer& peer : peers_) {
        if (peer.used && peer.connected) {
            return true;
        }
    }
    return false;
}

bool Connection::punching() const
{
    for (const Peer& peer : peers_) {
        if (peer.used && !peer.connected) {
            return true;
        }
    }
    return false;
}

bool Connection::relayed(u16 node) const
{
    const Peer* peer = peerForNode(node);
    return peer != nullptr && peer->relayed;
}

void Connection::introduce(const ServerMsg& msg, u32 nowMs)
{
    peerName_ = msg.peerName;

    // A repeat for a punch already under way -- the server sends one PunchNow
    // per JoinRequest and a guest may retry -- updates the peer rather than
    // taking a second slot.
    Peer* peer = peerForToken(msg.punchToken);
    if (peer == nullptr) {
        peer = freePeer();
        if (peer == nullptr) {
            message_ = "This session is full.";
            return;
        }
        *peer = Peer();
        peer->used = true;
        // A guest only ever talks to the host, which is node 1 by the same
        // numbering local wireless uses. A host hands out 2 upwards, and the
        // session allocates a guest slot the first time it hears one.
        peer->node = hosting_ ? u16(link::kHostNode + 1 + (peer - peers_)) : link::kHostNode;
    }

    peer->sessionId = msg.sessionId;
    std::memcpy(peer->punchToken, msg.punchToken, kTokenSize);
    peer->candidateCount = 0;
    for (const Candidate& candidate : msg.candidates) {
        if (peer->candidateCount >= kMaxCandidates) {
            break;
        }
        peer->candidates[peer->candidateCount++] = candidate;
    }
    peer->deadlineMs = nowMs + u32(msg.windowMs);
    peer->lastProbeMs = nowMs - kProbeIntervalMs;
    peer->reported = false;
    message_.clear();

    // The first volley goes out on this frame rather than the next: the other
    // console started counting its window down when the server sent it, not
    // when this one got round to reading it.
    firePunches(nowMs);
}

void Connection::allocateRelay(const ServerMsg& msg, u32 nowMs)
{
    // **At most one relayed peer**, because the server keys its allocations by
    // session id: a second one would take the first's allocation and break a
    // guest that is already playing. Refused here, out loud, rather than
    // quietly.
    for (Peer& peer : peers_) {
        if (peer.used && peer.relayed && peer.connected) {
            message_ = "The server can relay only one player per session.";
            return;
        }
    }

    // The relay is the answer to a punch that failed, so the peer it belongs to
    // is the one still trying for this session.
    for (Peer& peer : peers_) {
        if (!peer.used || peer.connected || peer.sessionId != msg.sessionId) {
            continue;
        }
        peer.relayed = true;
        peer.relay = msg.relay;
        std::memcpy(peer.relayToken, msg.token, kTokenSize);
        peer.connected = true;
        peer.addr = msg.relay;
        peer.relayHeard = false;
        message_.clear();
        // Straight away, not on the next frame: the other end may already be
        // sending, and every datagram of its that arrives before this end is
        // known is one the relay throws away.
        sendRelayHello(peer, nowMs);
        return;
    }
}

void Connection::sendProbe(Peer& peer, const Endpoint& to, u8 kind)
{
    if (transport_ == nullptr) {
        return;
    }
    u8 probe[kProbeSize];
    std::memcpy(probe, kProbeMagic, sizeof(kProbeMagic));
    std::memcpy(probe + sizeof(kProbeMagic), peer.punchToken, kTokenSize);
    probe[kProbeSize - 1] = kind;
    transport_->send(to, probe, sizeof(probe));
}

// **The relay's registration, which is a probe sent through it.** AlphaComputer's
// relay is never told where the two ends are: it learns an end's address from
// the first datagram carrying that end's token, and one addressed to an end it
// has not learned yet is dropped. Sending this also opens this console's own
// NAT to the relay, which is the server's machine on a port the login never
// wrote to. The payload is a probe -- the magic, the punch token, "pong" -- so
// the other console swallows it in `handleProbe` rather than passing it to a
// session that would not know what it was.
void Connection::sendRelayHello(Peer& peer, u32 nowMs)
{
    peer.lastRelayHelloMs = nowMs;
    u8 probe[kProbeSize];
    std::memcpy(probe, kProbeMagic, sizeof(kProbeMagic));
    std::memcpy(probe + sizeof(kProbeMagic), peer.punchToken, kTokenSize);
    probe[kProbeSize - 1] = kProbePong;
    sendTo(peer, probe, sizeof(probe));
}

void Connection::firePunches(u32 nowMs)
{
    // Relayed peers first: until the other end has been heard through the
    // relay, it may not know this one yet, so the hello goes again at the
    // punch's own cadence. Once anything has arrived both ends are known and
    // the session's traffic keeps them so.
    for (Peer& peer : peers_) {
        if (peer.used && peer.relayed && !peer.relayHeard
            && nowMs - peer.lastRelayHelloMs >= kProbeIntervalMs) {
            sendRelayHello(peer, nowMs);
        }
    }

    for (Peer& peer : peers_) {
        if (!peer.used || peer.connected) {
            continue;
        }
        if (nowMs - peer.lastProbeMs >= kProbeIntervalMs && nowMs < peer.deadlineMs) {
            peer.lastProbeMs = nowMs;
            for (int i = 0; i < peer.candidateCount; ++i) {
                sendProbe(peer, peer.candidates[i].endpoint, kProbePing);
            }
        }
        // The window has closed with nothing coming back. The server has to be
        // told: it cannot see a link it is not part of, and a failure is what
        // allocates the relay.
        if (!peer.reported && nowMs >= peer.deadlineMs) {
            peer.reported = true;
            client_.reportPunch(peer.sessionId, false);
        }
    }
}

bool Connection::handleProbe(const u8* data, usize size, const Endpoint& from, u32 nowMs)
{
    (void)nowMs;
    if (size != kProbeSize || std::memcmp(data, kProbeMagic, sizeof(kProbeMagic)) != 0) {
        return false;
    }
    Peer* peer = peerForToken(data + sizeof(kProbeMagic));
    if (peer == nullptr) {
        return true;  // A probe for a punch that is over. Swallowed, not queued.
    }
    if (peer->relayed) {
        // The other end's relay hello, or a straggler from the punch. Neither
        // is answered: an answer would go to the relay without a relay token,
        // and it would drop it.
        if (from == peer->relay) {
            peer->relayHeard = true;
        }
        return true;
    }

    const bool firstTime = !peer->connected;
    if (!peer->relayed) {
        // The first path to prove itself is the one this end sends on; every
        // path that does is one its frames are heard on.
        if (!peer->connected) {
            peer->addr = from;
            peer->connected = true;
        }
        bool known = false;
        for (int i = 0; i < peer->provenCount; ++i) {
            known = known || peer->proven[i] == from;
        }
        if (!known && peer->provenCount < kMaxProven) {
            peer->proven[peer->provenCount++] = from;
        }
    }
    if (data[kProbeSize - 1] == kProbePing) {
        // Answering is what proves the path to the other end: its own probe may
        // have been the one that opened this side's pinhole and been lost doing
        // it.
        sendProbe(*peer, from, kProbePong);
    }
    if (firstTime && !peer->reported) {
        peer->reported = true;
        // **Either side reporting success is enough**, so this is worth saying
        // as soon as it is true rather than at the end of the window: a punch
        // is symmetric, and if this console has the other's packets the path
        // exists both ways.
        client_.reportPunch(peer->sessionId, true);
    }
    return true;
}

void Connection::enqueue(const u8* data, usize size, u16 node)
{
    if (inboxCount_ >= kInboxSlots || size > link::kMaxDatagram) {
        ++overflows_;
        return;
    }
    const int slot = (inboxHead_ + inboxCount_) % kInboxSlots;
    std::memcpy(inbox_.data() + usize(slot) * link::kMaxDatagram, data, size);
    inboxSize_[slot] = size;
    inboxNode_[slot] = node;
    ++inboxCount_;
}

void Connection::pump(u32 nowMs)
{
    nowMs_ = nowMs;
    if (transport_ == nullptr) {
        return;
    }

    u8 datagram[link::kMaxDatagram];
    Endpoint from;
    usize size = 0;
    while (inboxCount_ < kInboxSlots
           && transport_->receive(datagram, sizeof(datagram), &size, &from)) {
        if (from == server_) {
            client_.onDatagram(datagram, size, nowMs);
            continue;
        }
        if (handleProbe(datagram, size, from, nowMs)) {
            continue;
        }
        Peer* peer = peerForAddress(from);
        if (peer == nullptr) {
            // Nobody this console has been introduced to. On a public port this
            // is the ordinary case rather than the exceptional one.
            ++strays_;
            lastStray_ = from;
            continue;
        }
        if (peer->relayed) {
            peer->relayHeard = true;
        }
        ++peer->framesIn;
        enqueue(datagram, size, peer->node);
    }

    client_.pump(nowMs);
    firePunches(nowMs);
}

Event Connection::takeEvent()
{
    const Event event = client_.takeEvent();
    if (event == Event::PunchNow) {
        introduce(client_.introduction(), nowMs_);
    } else if (event == Event::RelayAllocated) {
        allocateRelay(client_.introduction(), nowMs_);
    }
    return event;
}

bool Connection::sendTo(Peer& peer, const u8* data, usize size)
{
    if (transport_ == nullptr) {
        return false;
    }
    if (!peer.relayed) {
        return transport_->send(peer.addr, data, size);
    }
    // The relay forwards `token || payload` and strips the token on the way
    // out, so what the other end receives is the frame alone.
    outgoing_.assign(peer.relayToken, peer.relayToken + kTokenSize);
    outgoing_.insert(outgoing_.end(), data, data + size);
    return transport_->send(peer.relay, outgoing_.data(), outgoing_.size());
}

bool Connection::send(u16 node, const u8* data, usize size)
{
    Peer* peer = peerForNode(node);
    if (peer == nullptr || !peer->connected) {
        // Not a broken link: a host queues for a guest whose punch has not
        // landed yet, and the link layer's own retransmission covers the gap.
        return true;
    }
    ++peer->framesOut;
    return sendTo(*peer, data, size);
}

std::string Connection::report(u16 node) const
{
    const Peer* peer = peerForNode(node);
    std::string out;
    if (peer == nullptr) {
        out = "no peer";
    } else {
        out = peer->relayed ? "relay " : "direct ";
        out += endpointText(peer->addr);
        out += ", sent " + std::to_string(peer->framesOut);
        out += ", heard " + std::to_string(peer->framesIn);
        out += ", paths " + std::to_string(peer->provenCount);
    }
    if (strays_ > 0) {
        out += ", " + std::to_string(strays_) + " from " + endpointText(lastStray_);
    }
    return out;
}

bool Connection::receive(u8* buffer, usize capacity, usize* size, u16* node)
{
    if (inboxCount_ == 0) {
        return false;
    }
    const int slot = inboxHead_;
    const usize have = inboxSize_[slot];
    if (have > capacity) {
        // Cannot happen -- the inbox refuses anything larger than a link
        // datagram -- but dropping it is better than writing past the caller.
        inboxHead_ = (inboxHead_ + 1) % kInboxSlots;
        --inboxCount_;
        ++overflows_;
        return false;
    }
    std::memcpy(buffer, inbox_.data() + usize(slot) * link::kMaxDatagram, have);
    *size = have;
    *node = inboxNode_[slot];
    inboxHead_ = (inboxHead_ + 1) % kInboxSlots;
    --inboxCount_;
    return true;
}

}  // namespace mc::net::ac
