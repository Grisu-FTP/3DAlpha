// A guest's end of a local session. See guest_play.hpp.

#include "platform/ctr/guest_play.hpp"

#include <3ds.h>

#include <algorithm>

namespace mc::ctr {

namespace {

// How much of the channel's outbox goes onto the link in one message. Under
// `net::link::kMaxMessage` (1,388) with room to spare; the link refuses
// anything longer outright.
constexpr usize kLinkChunk = 1024;

// How often a guest that is *playing* makes a column for the host. In the
// lobby it is one a frame -- there is nothing else for the console to do -- and
// in a world it is this, because the generation is a millisecond and a half
// and the player it belongs to would rather have the frame.
constexpr u32 kPlayingTerrainMs = 500;

// The most the lobby keeps on screen.
constexpr usize kMaxLines = 5;

}  // namespace

GuestPlay::GuestPlay() = default;

GuestPlay::~GuestPlay()
{
    leave("left the session");
}

bool GuestPlay::join(const LocalSession& session, const std::string& name, std::string* error)
{
    if (!startLocalWireless(error)) {
        return false;
    }
    if (!link_.join(session, error)) {
        return false;
    }
    name_ = name;
    left_ = false;
    lines_.clear();

    // What this console can generate: the seed and the switches are the host's
    // to state, so all that goes up is which build's arithmetic this is. See
    // core/net/terrain_share.hpp.
    net::link::GeneratorId own;
    own.version = net::link::generatorVersion();
    session_.join(name, own, u32(osGetTime()), this);
    // Every keyboard in the build warns the open session before it suspends
    // the console. See `ctr::linkPausing`.
    setLinkPauseHook(&GuestPlay::linkPaused, this);
    return true;
}

void GuestPlay::linkPaused(void* context, u32 expectedMs)
{
    auto* self = static_cast<GuestPlay*>(context);
    if (self->link_.active()) {
        self->session_.announceAway(expectedMs, u32(osGetTime()), self->link_);
    }
}

void GuestPlay::leave(const std::string& reason)
{
    if (left_) {
        return;
    }
    left_ = true;
    if (link_.active()) {
        session_.leave(reason, u32(osGetTime()), link_);
    }
    // **The channel before the link**, because stopping it joins a thread that
    // may be halfway through a column and the link's buffers are what it reads
    // out of.
    channel_.stop();
    link_.leave();
    setLinkPauseHook(nullptr, nullptr);
}

bool GuestPlay::ready() const
{
    return channel_.state() == net::PacketChannel::State::Playing;
}

bool GuestPlay::finished() const
{
    return session_.state() == net::link::GuestSession::State::Finished;
}

void GuestPlay::add(std::string line)
{
    lines_.push_back(std::move(line));
    if (lines_.size() > kMaxLines) {
        lines_.erase(lines_.begin());
    }
}

void GuestPlay::onPlayerJoined(u8 playerId, const std::string& name)
{
    (void)playerId;
    add(name + " joined");
}

void GuestPlay::onPlayerLeft(u8 playerId, const std::string& reason)
{
    (void)playerId;
    add(reason.empty() ? std::string("somebody left") : "somebody left: " + reason);
}

void GuestPlay::onChat(u8 playerId, const std::string& text)
{
    (void)playerId;
    add(text);
}

void GuestPlay::onWorldRules(const net::link::WorldRules& rules)
{
    // Numbers on the wire, because core/net/ has no opinion about settings.
    // Anything the two builds do not agree on falls back to the first value,
    // which is the same rule a settings file with a bad key gets.
    const auto mode = rules.gamemode <= u8(settings::Gamemode::Creative)
                          ? settings::Gamemode(rules.gamemode)
                          : settings::Gamemode::Spectator;
    const auto hard = rules.difficulty <= u8(settings::Difficulty::Hard)
                          ? settings::Difficulty(rules.difficulty)
                          : settings::Difficulty::Peaceful;
    if (mode == gamemode_ && hard == difficulty_) {
        return;
    }
    gamemode_ = mode;
    difficulty_ = hard;
    rulesChanged_ = true;
}

bool GuestPlay::takeRulesChange()
{
    const bool changed = rulesChanged_;
    rulesChanged_ = false;
    return changed;
}

void GuestPlay::onTerrainRequest(const u8* body, usize size)
{
    responder_.onRequest(body, size);
}

void GuestPlay::onGamePacket(u8 playerId, const u8* data, usize size)
{
    (void)playerId;
    // Straight through to the parse thread. Nothing on this thread looks
    // inside it -- that is the whole point of the channel.
    channel_.feed(data, size);
}

bool GuestPlay::answerTerrain()
{
    if (!responder_.ready()) {
        if (session_.world().version != net::link::generatorVersion()) {
            return false;
        }
        responder_.open(session_.world());
    }
    std::vector<std::vector<u8>> parts;
    if (!responder_.generateOne(&parts)) {
        return false;
    }
    for (const std::vector<u8>& part : parts) {
        session_.sendTerrainPart(part.data(), part.size());
    }
    return true;
}

void GuestPlay::flushChannel()
{
    channel_.takeOutbound(&outbound_);
    usize offset = 0;
    while (offset < outbound_.size()) {
        const usize take = std::min(kLinkChunk, outbound_.size() - offset);
        if (!session_.sendGamePacket(outbound_.data() + offset, take)) {
            break;  // the window is full; the rest goes next frame
        }
        offset += take;
    }
    if (offset == outbound_.size()) {
        outbound_.clear();
    } else if (offset > 0) {
        outbound_.erase(outbound_.begin(), outbound_.begin() + std::ptrdiff_t(offset));
    }
}

bool GuestPlay::pumpLobby()
{
    if (!link_.active()) {
        return false;
    }
    const usize before = lines_.size();
    session_.pump(u32(osGetTime()), link_);

    if (session_.state() != net::link::GuestSession::State::Playing) {
        return lines_.size() != before;
    }

    // **The channel starts as soon as the link is up**, not when the game does.
    // The host's Login can arrive at any moment after the Welcome, and a
    // channel that was not running yet would lose the bytes in front of it --
    // the stream has no length prefixes to resynchronise on.
    if (!channelStarted_) {
        channelStarted_ = channel_.start();
    }

    // **The lobby is where a guest earns its keep.** The host is asking for
    // terrain around this console, and this console is sitting on a menu with a
    // whole core doing nothing. One column a frame: the generation is the
    // expensive call, and the link carries the answer while the next one is
    // being made.
    const bool made = answerTerrain();
    flushChannel();
    return made || lines_.size() != before;
}

void GuestPlay::pump()
{
    if (!link_.active()) {
        return;
    }
    session_.pump(u32(osGetTime()), link_);

    const u32 now = u32(osGetTime());
    if (u32(now - terrainAtMs_) >= kPlayingTerrainMs) {
        terrainAtMs_ = now;
        answerTerrain();
    }

    flushChannel();

    if (session_.state() == net::link::GuestSession::State::Finished) {
        // The link is what knows why; the channel is what the game is watching.
        channel_.closeWith("Session ended",
                           session_.reason().empty() ? std::string("The host closed the world.")
                                                     : session_.reason());
    }
}

}  // namespace mc::ctr
