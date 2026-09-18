#pragma once

// The other console's end of a local session: everything a guest owns, from
// the radio up to the stream `NetPlay` plays out of.
//
// **One object for two screens.** A guest's life has two halves -- the lobby,
// where it is connected and the host has not started sending a world, and the
// game, where it has -- and they used to be two places holding the same three
// objects. They are one object now because the link does not restart in
// between: the session that answered the Hello is the session that carries the
// columns, and dropping it to enter the game would mean joining twice.
//
// The line between them is `ready()`: the host's Login has arrived, which is
// the moment there is a world to be in. Until then `pumpLobby` reads the radio
// and makes terrain for the host; afterwards `pump` moves bytes between the
// radio and `core/net/local_channel.hpp` and the game does the rest.
//
// **What a guest still does for the host while playing.** It answers terrain
// requests, but far more slowly than in the lobby -- a console with a world to
// draw is a player, not a render farm. See `kPlayingTerrainMs`.

#include "core/net/local_channel.hpp"
#include "core/net/session.hpp"
#include "core/net/terrain_share.hpp"
#include "core/settings/world_settings.hpp"
#include "core/util/types.hpp"
#include "platform/ctr/local_link.hpp"
#include "platform/ctr/session_link.hpp"

#include <string>
#include <vector>

namespace mc::ctr {

class GuestPlay : public net::link::SessionListener {
public:
    GuestPlay();
    ~GuestPlay() override;

    GuestPlay(const GuestPlay&) = delete;
    GuestPlay& operator=(const GuestPlay&) = delete;

    // Connects to a host a scan found and sends the Hello. False with `*error`
    // in words a player can act on.
    bool join(const LocalSession& session, const std::string& name, std::string* error);

    // **The same session, over the internet.** The link is already up when this
    // is called -- two consoles the rendezvous server introduced, talking
    // directly or through a relay -- so there is no radio to start and nothing
    // that can fail here. Everything above this line is unchanged: the Hello,
    // the world, the terrain a guest makes for its host. See
    // platform/ctr/online.hpp.
    void joinOnline(SessionLink& link, const std::string& name);

    // The lobby's frame: read the radio, answer whatever terrain the host has
    // asked for, and note what happened. True when something changed that the
    // screen should be redrawn for.
    bool pumpLobby();

    // The game's frame: the radio in, the channel's outbox out. Called at the
    // top of the loop, before `NetPlay::pump` reads what arrived.
    void pump();

    // **The host has sent a Login**, so there is a world coming and the game
    // can start. Until this is true there is nothing to draw.
    bool ready() const;

    // Leaves on purpose, telling the host first. Safe to call twice.
    void leave(const std::string& reason);

    // The session ended without this end asking -- the host quit, or the link
    // went quiet. `reason` says which.
    bool finished() const;

    net::PacketChannel& channel() { return channel_; }

    net::link::GuestSession::State state() const { return session_.state(); }
    const std::string& reason() const { return session_.reason(); }
    const std::string& worldName() const { return session_.worldName(); }
    const std::string& hostName() const { return session_.hostName(); }
    int playerCount() const { return int(session_.players().size()); }
    u32 rttMs() const { return session_.rttMs(); }
    u32 terrainAnswered() const { return responder_.answered(); }
    bool active() const { return link_ != nullptr && link_->active(); }

    // **How the host's world is played**, which this console adopts rather than
    // choosing for itself: a guest in a Survival world must not be flying and a
    // guest in a Creative one must not be starving. Known from the Welcome, and
    // updated whenever the host changes it -- the pause menu can turn a world
    // Creative without anybody leaving it.
    settings::Gamemode gamemode() const { return gamemode_; }
    settings::Difficulty difficulty() const { return difficulty_; }

    // True once, each time the host has changed them. The game loop asks each
    // frame and applies what it finds.
    bool takeRulesChange();

    // What the session has reported, as lines for the lobby to draw.
    const std::vector<std::string>& lines() const { return lines_; }
    void clearLines() { lines_.clear(); }

private:
    // `ctr::setLinkPauseHook`'s callback: this console is about to be
    // suspended by a library applet, and the host has to be told before it is.
    static void linkPaused(void* context, u32 expectedMs);

    void onPlayerJoined(u8 playerId, const std::string& name) override;
    void onPlayerLeft(u8 playerId, const std::string& reason) override;
    void onChat(u8 playerId, const std::string& text) override;
    void onTerrainRequest(const u8* body, usize size) override;
    void onWorldRules(const net::link::WorldRules& rules) override;
    void onGamePacket(u8 playerId, const u8* data, usize size) override;

    // One column for the host, if it has asked for one and this end is willing
    // to spend the time now. True when one was made and sent.
    bool answerTerrain();

    // Whatever the channel has encoded, onto the link, in pieces the link can
    // carry. What it will not take stays for the next frame.
    void flushChannel();

    void add(std::string line);

    // The radio, when this is a session in a room, and the thing every line
    // below talks through, whichever it is.
    LocalLink local_;
    SessionLink* link_ = nullptr;
    net::link::GuestSession session_;
    net::link::TerrainResponder responder_;
    net::LocalChannel channel_;
    std::vector<std::string> lines_;
    std::vector<u8> outbound_;
    std::string name_;
    settings::Gamemode gamemode_ = settings::Gamemode::Survival;
    settings::Difficulty difficulty_ = settings::Difficulty::Normal;
    bool rulesChanged_ = false;
    bool channelStarted_ = false;
    bool left_ = false;
    u32 terrainAtMs_ = 0;
};

}  // namespace mc::ctr
