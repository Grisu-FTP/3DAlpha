#include "core/net/session.hpp"

#include "core/net/server_list.hpp"

#include <cstddef>

namespace mc::net::link {

namespace {

// A console that has said nothing but its first frame for this long never
// meant to join. Long enough for a handshake that had to be retransmitted
// twice, short enough that a slot is not held all game by a stray frame.
constexpr u32 kHandshakeMs = 5000;

// Frames read in one pump. A pump happens once a frame on the thread that
// draws, so this is a ceiling on what a flood -- or a guest that has queued a
// world's worth of edits -- can cost before the console draws again.
constexpr int kReadBudget = 64;

std::vector<u8> bodyOf(const Chat& chat)
{
    std::vector<u8> out;
    encode(chat, &out);
    return out;
}

}  // namespace

// ---- host ------------------------------------------------------------------

HostSession::HostSession()
{
    inbox_.resize(kMaxDatagram);
    body_.reserve(kMaxMessage);
}
HostSession::~HostSession() = default;

void HostSession::open(const std::string& worldName, const std::string& hostName,
                       const GeneratorId& world, u32 nowMs, SessionListener* listener)
{
    worldName_ = worldName;
    hostName_ = hostName;
    world_ = world;
    listener_ = listener;

    // **Every guest's buffers now, not when they arrive.** A `Peer` is 90 KB of
    // window and reorder space; taking that while a world is streaming is the
    // sort of allocation that shows up as a dropped frame in front of the
    // player who was already there.
    for (int i = 0; i < kMaxGuests; ++i) {
        if (!guests_[i]) {
            guests_[i] = std::make_unique<Guest>();
        }
        guests_[i]->node = 0;
        guests_[i]->playerId = u8(kHostPlayerId + 1 + i);
        guests_[i]->welcomed = false;
        guests_[i]->generates = false;
        guests_[i]->rulesOwed = false;
        guests_[i]->name.clear();
        guests_[i]->peer.reset(nowMs);
    }
    open_ = true;
}

void HostSession::close(const std::string& reason, u32 nowMs, Datagrams& datagrams)
{
    if (!open_) {
        return;
    }
    std::vector<u8> body;
    for (auto& slot : guests_) {
        Guest& guest = *slot;
        if (guest.node == 0) {
            continue;
        }
        body.clear();
        encode(Reject{reason}, &body);
        guest.peer.queue(Msg::Bye, body, true);
        guest.peer.flush(nowMs, datagrams, guest.node);
        guest.node = 0;
        guest.welcomed = false;
    }
    open_ = false;
}

int HostSession::guestCount() const
{
    int count = 0;
    for (const auto& slot : guests_) {
        if (slot && slot->welcomed) {
            ++count;
        }
    }
    return count;
}

void HostSession::players(std::vector<Player>* out) const
{
    out->clear();
    out->push_back(Player{kHostPlayerId, hostName_});
    for (const auto& slot : guests_) {
        if (slot && slot->welcomed) {
            out->push_back(Player{slot->playerId, slot->name});
        }
    }
}

u32 HostSession::worstRttMs() const
{
    u32 worst = 0;
    for (const auto& slot : guests_) {
        if (slot && slot->welcomed && slot->peer.rttMs() > worst) {
            worst = slot->peer.rttMs();
        }
    }
    return worst;
}

HostSession::Guest* HostSession::guestForNode(u16 node, u32 nowMs)
{
    for (auto& slot : guests_) {
        if (slot->node == node) {
            return slot.get();
        }
    }
    for (auto& slot : guests_) {
        if (slot->node != 0) {
            continue;
        }
        slot->node = node;
        slot->welcomed = false;
        slot->name.clear();
        slot->firstHeardMs = nowMs;
        slot->peer.reset(nowMs);
        return slot.get();
    }
    // The session is full. Nothing is sent back: local wireless is told the
    // same limit when the network is created, so a console that got this far
    // is one the service already let in and the next frame it hears is the
    // host's silence.
    return nullptr;
}

void HostSession::drop(Guest& guest, const std::string& reason, u32 nowMs, Datagrams& datagrams)
{
    if (guest.node == 0) {
        return;
    }
    std::vector<u8> body;
    encode(Reject{reason}, &body);
    guest.peer.queue(Msg::Bye, body, true);
    guest.peer.flush(nowMs, datagrams, guest.node);

    if (guest.welcomed) {
        body.clear();
        encode(Player{guest.playerId, guest.name}, &body);
        tellOthers(guest, Msg::Leave, body);
        if (listener_ != nullptr) {
            listener_->onPlayerLeft(guest.playerId, reason);
        }
    }
    guest.node = 0;
    guest.welcomed = false;
    guest.rulesOwed = false;
    guest.name.clear();
}

void HostSession::tellOthers(const Guest& about, Msg kind, const std::vector<u8>& body)
{
    for (auto& slot : guests_) {
        Guest& other = *slot;
        if (!other.welcomed || other.playerId == about.playerId) {
            continue;
        }
        other.peer.queue(kind, body, kind != Msg::Pose);
    }
}

void HostSession::sink(void* ctx, Msg kind, const u8* body, usize size)
{
    HostSession* self = static_cast<HostSession*>(ctx);
    if (self->current_ != nullptr && !self->currentBroken_) {
        self->onMessage(*self->current_, kind, body, size);
    }
}

void HostSession::onMessage(Guest& guest, Msg kind, const u8* body, usize size)
{
    std::vector<u8> out;
    switch (kind) {
    case Msg::Hello: {
        Hello hello;
        if (!decode(body, size, &hello)) {
            currentBroken_ = true;
            currentReason_ = "that console sent something this one could not read";
            return;
        }
        if (hello.protocol != kProtocol) {
            encode(Reject{"the two consoles are running different versions of 3DAlpha"},
                   &out);
            guest.peer.queue(Msg::Reject, out, true);
            currentBroken_ = true;
            currentReason_ = "a different version of 3DAlpha";
            return;
        }
        if (guest.welcomed) {
            return;  // a retransmitted Hello, already answered
        }
        // Through the same filter a server name goes through: what arrives is
        // another console's friend-list name, and it has to be something this
        // game can draw and a chat line can carry.
        guest.name = usernameFrom(hello.name);
        guest.welcomed = true;

        // **Whether this console may ever be asked to make terrain.** Same
        // build, same world: anything else plays normally and is never asked,
        // which is what stops a differently-built console writing terrain that
        // is not this world's into the host's save.
        guest.generates = !world_.version.empty() && hello.generator == world_.version;

        out.clear();
        encode(Welcome{kProtocol, guest.playerId, worldName_, hostName_, world_, rules_}, &out);
        guest.peer.queue(Msg::Welcome, out, true);

        // Who is already here, the host first. A guest builds its player list
        // from these and from the Join it is sent later.
        out.clear();
        encode(Player{kHostPlayerId, hostName_}, &out);
        guest.peer.queue(Msg::Join, out, true);
        for (auto& slot : guests_) {
            const Guest& other = *slot;
            if (!other.welcomed || other.playerId == guest.playerId) {
                continue;
            }
            out.clear();
            encode(Player{other.playerId, other.name}, &out);
            guest.peer.queue(Msg::Join, out, true);
        }

        out.clear();
        encode(Player{guest.playerId, guest.name}, &out);
        tellOthers(guest, Msg::Join, out);
        if (listener_ != nullptr) {
            listener_->onPlayerJoined(guest.playerId, guest.name);
        }
        return;
    }
    case Msg::Chat: {
        Chat chat;
        if (!decode(body, size, &chat) || !guest.welcomed) {
            currentBroken_ = true;
            currentReason_ = "that console sent something this one could not read";
            return;
        }
        // **The speaker is where the frame came from, not what it claims.**
        chat.playerId = guest.playerId;
        if (listener_ != nullptr) {
            listener_->onChat(chat.playerId, chat.text);
        }
        tellOthers(guest, Msg::Chat, bodyOf(chat));
        return;
    }
    case Msg::Pose: {
        Pose pose;
        if (!decode(body, size, &pose) || !guest.welcomed) {
            return;  // a pose is not worth ending a link over
        }
        pose.playerId = guest.playerId;
        if (listener_ != nullptr) {
            listener_->onPose(pose);
        }
        body_.clear();
        encode(pose, &body_);
        tellOthers(guest, Msg::Pose, body_);
        return;
    }
    case Msg::Away: {
        // **Two bytes that stop this console throwing somebody out for
        // opening a keyboard.** See `Msg::Away` and `kMaxAwayMs`.
        if (size < 2) {
            return;
        }
        guest.peer.grantAway(nowMs_, u32(u32(body[0]) << 8 | body[1]));
        return;
    }
    case Msg::GamePacket:
        if (guest.welcomed && listener_ != nullptr) {
            listener_->onGamePacket(guest.playerId, body, size);
        }
        return;
    case Msg::TerrainPart:
        // Trusted, as the file note in terrain_share.hpp says -- but only from
        // a console this session actually asked.
        if (guest.welcomed && guest.generates && listener_ != nullptr) {
            listener_->onTerrainPart(guest.playerId, body, size);
        }
        return;
    case Msg::Bye: {
        Reject bye;
        currentReason_ = decode(body, size, &bye) && !bye.reason.empty() ? bye.reason
                                                                        : "left the session";
        currentBroken_ = true;
        return;
    }
    default:
        // Welcome, Reject, Join, Leave and Rules are the host's to send. A
        // guest that sends one is not a guest.
        currentBroken_ = true;
        currentReason_ = "that console did not follow the session's rules";
        return;
    }
}

void HostSession::forgiveStall(u32 nowMs)
{
    const u32 gap = u32(nowMs - pumpedAtMs_);
    // The first pump has no previous one to measure against.
    if (pumpedAtMs_ != 0 && gap >= kStallMs) {
        for (auto& slot : guests_) {
            if (slot->node != 0) {
                slot->peer.forgive(gap);
            }
        }
    }
    pumpedAtMs_ = nowMs;
}

void HostSession::announceAway(u32 ms, u32 nowMs, Datagrams& datagrams)
{
    if (!open_) {
        return;
    }
    const u32 grace = ms > kMaxAwayMs ? kMaxAwayMs : ms;
    const u8 body[2] = {u8(grace >> 8), u8(grace)};
    for (auto& slot : guests_) {
        Guest& guest = *slot;
        if (guest.node == 0) {
            continue;
        }
        // **Not retried, and deliberately so.** This has to be on the air
        // before the applet starts, and the applet starts on the caller's next
        // line; a warning that could not be queued this instant is a warning
        // that would arrive after the thing it warns about. The peer still has
        // `kTimeoutMs` without it, which is the behaviour this replaces.
        guest.peer.queue(Msg::Away, body, sizeof(body), true);
        guest.peer.flush(nowMs, datagrams, guest.node);
    }
    // The console is about to stop; when it starts again the gap will be
    // forgiven from this moment rather than from the last frame.
    pumpedAtMs_ = nowMs;
}

void HostSession::pump(u32 nowMs, Datagrams& datagrams)
{
    if (!open_) {
        return;
    }
    forgiveStall(nowMs);
    nowMs_ = nowMs;

    usize size = 0;
    u16 node = 0;
    for (int i = 0; i < kReadBudget; ++i) {
        if (!datagrams.receive(inbox_.data(), inbox_.size(), &size, &node)) {
            break;
        }
        if (node == 0 || node == kHostNode) {
            continue;  // not a guest's frame
        }
        Guest* guest = guestForNode(node, nowMs);
        if (guest == nullptr) {
            continue;
        }
        current_ = guest;
        currentBroken_ = false;
        currentReason_.clear();
        const bool ok =
            guest->peer.receive(inbox_.data(), size, nowMs, &HostSession::sink, this);
        current_ = nullptr;
        if (!ok || currentBroken_) {
            drop(*guest,
                 currentReason_.empty() ? std::string("the link broke") : currentReason_, nowMs,
                 datagrams);
        }
    }

    for (auto& slot : guests_) {
        Guest& guest = *slot;
        if (guest.node == 0) {
            continue;
        }
        if (guest.peer.timedOut(nowMs)) {
            drop(guest, "stopped answering", nowMs, datagrams);
            continue;
        }
        if (!guest.welcomed && u32(nowMs - guest.firstHeardMs) >= kHandshakeMs) {
            drop(guest, "never finished joining", nowMs, datagrams);
            continue;
        }
        if (guest.rulesOwed) {
            body_.clear();
            encode(rules_, &body_);
            if (guest.peer.queue(Msg::Rules, body_, true)) {
                guest.rulesOwed = false;
            }
        }
        if (!guest.peer.flush(nowMs, datagrams, guest.node)) {
            drop(guest, "the link broke", nowMs, datagrams);
        }
    }
}

void HostSession::setWorldRules(const WorldRules& rules)
{
    rules_ = rules;
    if (!open_) {
        return;
    }
    // **Everybody who is already in.** A guest that has not been welcomed yet
    // will read them out of the Welcome instead, which is the same two bytes.
    // Marked rather than sent, because the window can be full at this exact
    // moment and a rules change is not something that may be dropped.
    for (auto& slot : guests_) {
        Guest& guest = *slot;
        if (guest.welcomed) {
            guest.rulesOwed = true;
        }
    }
}

void HostSession::say(const std::string& text)
{
    std::vector<u8> body;
    encode(Chat{kHostPlayerId, text}, &body);
    for (auto& slot : guests_) {
        if (slot->welcomed) {
            slot->peer.queue(Msg::Chat, body, true);
        }
    }
}

void HostSession::reportPose(const Pose& pose)
{
    if (!open_) {
        return;
    }
    Pose stamped = pose;
    stamped.playerId = kHostPlayerId;
    body_.clear();
    encode(stamped, &body_);
    for (auto& slot : guests_) {
        if (slot->welcomed) {
            slot->peer.queue(Msg::Pose, body_, false);
        }
    }
}

bool HostSession::anyGenerator() const
{
    for (const auto& slot : guests_) {
        if (slot && slot->welcomed && slot->generates) {
            return true;
        }
    }
    return false;
}

bool HostSession::requestTerrain(const u8* body, usize size)
{
    // The shortest round trip wins. `Peer::rttMs` is measured off the
    // acknowledgements every datagram already carries, so "quickest" costs
    // nothing to know and follows a link that has got worse.
    Guest* best = nullptr;
    for (auto& slot : guests_) {
        Guest& guest = *slot;
        if (!guest.welcomed || !guest.generates || guest.peer.stalled()) {
            continue;
        }
        if (best == nullptr || guest.peer.rttMs() < best->peer.rttMs()) {
            best = &guest;
        }
    }
    if (best == nullptr) {
        return false;
    }
    return best->peer.queue(Msg::TerrainRequest, body, size, true);
}

bool HostSession::sendGamePacket(u8 playerId, const u8* data, usize size)
{
    bool all = true;
    for (auto& slot : guests_) {
        Guest& guest = *slot;
        if (!guest.welcomed || (playerId != 0 && guest.playerId != playerId)) {
            continue;
        }
        all = guest.peer.queue(Msg::GamePacket, data, size, true) && all;
    }
    return all;
}

// ---- guest -----------------------------------------------------------------

GuestSession::GuestSession()
{
    inbox_.resize(kMaxDatagram);
    body_.reserve(kMaxMessage);
}
GuestSession::~GuestSession() = default;

void GuestSession::join(const std::string& name, const GeneratorId& world, u32 nowMs,
                        SessionListener* listener)
{
    listener_ = listener;
    name_ = name;
    own_ = world;
    world_ = GeneratorId{};
    rules_ = WorldRules{};
    state_ = State::Joining;
    reason_.clear();
    worldName_.clear();
    hostName_.clear();
    players_.clear();
    playerId_ = 0;
    broken_ = false;
    joinedAtMs_ = nowMs;
    peer_.reset(nowMs);

    std::vector<u8> body;
    encode(Hello{kProtocol, name_, own_.version}, &body);
    peer_.queue(Msg::Hello, body, true);
}

void GuestSession::leave(const std::string& reason, u32 nowMs, Datagrams& datagrams)
{
    if (state_ == State::Joining || state_ == State::Playing) {
        std::vector<u8> body;
        encode(Reject{reason}, &body);
        peer_.queue(Msg::Bye, body, true);
        peer_.flush(nowMs, datagrams, kHostNode);
    }
    finish(reason);
}

void GuestSession::finish(const std::string& reason)
{
    if (state_ != State::Finished) {
        state_ = State::Finished;
        reason_ = reason;
    }
}

void GuestSession::sink(void* ctx, Msg kind, const u8* body, usize size)
{
    GuestSession* self = static_cast<GuestSession*>(ctx);
    if (!self->broken_) {
        self->onMessage(kind, body, size);
    }
}

void GuestSession::onMessage(Msg kind, const u8* body, usize size)
{
    switch (kind) {
    case Msg::Welcome: {
        Welcome welcome;
        if (!decode(body, size, &welcome)) {
            broken_ = true;
            return;
        }
        if (welcome.protocol != kProtocol) {
            finish("the two consoles are running different versions of 3DAlpha");
            return;
        }
        playerId_ = welcome.playerId;
        worldName_ = welcome.worldName;
        hostName_ = welcome.hostName;
        world_ = welcome.world;
        rules_ = welcome.rules;
        state_ = State::Playing;
        if (listener_ != nullptr) {
            listener_->onWorldRules(rules_);
        }
        players_.push_back(Player{playerId_, name_});
        if (listener_ != nullptr) {
            listener_->onPlayerJoined(playerId_, name_);
        }
        return;
    }
    case Msg::Rules: {
        WorldRules rules;
        if (!decode(body, size, &rules)) {
            broken_ = true;
            return;
        }
        rules_ = rules;
        if (listener_ != nullptr) {
            listener_->onWorldRules(rules_);
        }
        return;
    }
    case Msg::Reject: {
        Reject reject;
        finish(decode(body, size, &reject) && !reject.reason.empty()
                   ? reject.reason
                   : std::string("the host turned this console away"));
        return;
    }
    case Msg::Join: {
        Player player;
        if (!decode(body, size, &player)) {
            broken_ = true;
            return;
        }
        for (const Player& known : players_) {
            if (known.playerId == player.playerId) {
                return;
            }
        }
        players_.push_back(player);
        if (listener_ != nullptr) {
            listener_->onPlayerJoined(player.playerId, player.name);
        }
        return;
    }
    case Msg::Leave: {
        Player player;
        if (!decode(body, size, &player)) {
            broken_ = true;
            return;
        }
        for (usize i = 0; i < players_.size(); ++i) {
            if (players_[i].playerId == player.playerId) {
                players_.erase(players_.begin() + std::ptrdiff_t(i));
                break;
            }
        }
        if (listener_ != nullptr) {
            listener_->onPlayerLeft(player.playerId, std::string());
        }
        return;
    }
    case Msg::Chat: {
        Chat chat;
        if (!decode(body, size, &chat)) {
            broken_ = true;
            return;
        }
        if (listener_ != nullptr) {
            listener_->onChat(chat.playerId, chat.text);
        }
        return;
    }
    case Msg::Pose: {
        Pose pose;
        if (decode(body, size, &pose) && listener_ != nullptr) {
            listener_->onPose(pose);
        }
        return;
    }
    case Msg::GamePacket:
        if (listener_ != nullptr) {
            listener_->onGamePacket(kHostPlayerId, body, size);
        }
        return;
    case Msg::Away:
        // The host is about to be suspended -- a keyboard, or the home menu.
        // See `Msg::Away`.
        if (size >= 2) {
            peer_.grantAway(nowMs_, u32(u32(body[0]) << 8 | body[1]));
        }
        return;
    case Msg::TerrainRequest:
        if (listener_ != nullptr) {
            listener_->onTerrainRequest(body, size);
        }
        return;
    case Msg::Bye: {
        Reject bye;
        finish(decode(body, size, &bye) && !bye.reason.empty()
                   ? bye.reason
                   : std::string("the host closed the session"));
        return;
    }
    default:
        broken_ = true;
        return;
    }
}

void GuestSession::forgiveStall(u32 nowMs)
{
    const u32 gap = u32(nowMs - pumpedAtMs_);
    if (pumpedAtMs_ != 0 && gap >= kStallMs) {
        peer_.forgive(gap);
    }
    pumpedAtMs_ = nowMs;
}

void GuestSession::announceAway(u32 ms, u32 nowMs, Datagrams& datagrams)
{
    if (state_ != State::Joining && state_ != State::Playing) {
        return;
    }
    const u32 grace = ms > kMaxAwayMs ? kMaxAwayMs : ms;
    const u8 body[2] = {u8(grace >> 8), u8(grace)};
    peer_.queue(Msg::Away, body, sizeof(body), true);
    peer_.flush(nowMs, datagrams, kHostNode);
    pumpedAtMs_ = nowMs;
}

void GuestSession::pump(u32 nowMs, Datagrams& datagrams)
{
    if (state_ != State::Joining && state_ != State::Playing) {
        return;
    }
    forgiveStall(nowMs);
    nowMs_ = nowMs;

    usize size = 0;
    u16 node = 0;
    for (int i = 0; i < kReadBudget; ++i) {
        if (!datagrams.receive(inbox_.data(), inbox_.size(), &size, &node)) {
            break;
        }
        if (node != kHostNode) {
            continue;  // in a star there is nothing else to listen to
        }
        if (!peer_.receive(inbox_.data(), size, nowMs, &GuestSession::sink, this) || broken_) {
            finish("the host sent something this console could not read");
            return;
        }
    }

    if (state_ == State::Joining && u32(nowMs - joinedAtMs_) >= kHandshakeMs) {
        finish("the host did not answer");
        return;
    }
    if (peer_.timedOut(nowMs)) {
        finish("lost contact with the host");
        return;
    }
    if (!peer_.flush(nowMs, datagrams, kHostNode)) {
        finish("the local wireless link failed");
    }
}

void GuestSession::say(const std::string& text)
{
    if (state_ != State::Playing) {
        return;
    }
    std::vector<u8> body;
    encode(Chat{playerId_, text}, &body);
    peer_.queue(Msg::Chat, body, true);
}

void GuestSession::reportPose(const Pose& pose)
{
    if (state_ != State::Playing) {
        return;
    }
    Pose stamped = pose;
    stamped.playerId = playerId_;
    body_.clear();
    encode(stamped, &body_);
    peer_.queue(Msg::Pose, body_, false);
}

bool GuestSession::sendGamePacket(const u8* data, usize size)
{
    return state_ == State::Playing && peer_.queue(Msg::GamePacket, data, size, true);
}

bool GuestSession::sendTerrainPart(const u8* body, usize size)
{
    return state_ == State::Playing && peer_.queue(Msg::TerrainPart, body, size, true);
}

}  // namespace mc::net::link
