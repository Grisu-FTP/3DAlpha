#pragma once

// Hosting, as the game loop sees it: the world is the player's own, open the
// way it always is, with a session running beside it and a server inside it.
//
// **The host is not a server process.** `runGame` does not change shape for
// this the way it does for a Join -- there is no `NetPlay`, no waiting on a
// download, no remote authority over the player's own position. The world is
// this console's, and the session is something that happens next to it: the
// radio is read once a frame, guests are let in and answered, and what they
// say reaches the same chat log the player's own lines do.
//
// **What crosses the link, both ways.**
//
//   * *Out*: the world, in protocol-2 packets -- the columns, the block
//     changes, the time and where everybody is. See core/net/world_server.hpp
//     for why it is protocol 2 and not something of our own. This object is
//     the `ServerWorld` that server reads and writes through, because reading
//     a column needs the streamer and writing a block needs the renderer.
//   * *In*: terrain, which is the one thing that goes the other way. A guest
//     generates columns *for* the host -- the expensive half of making a
//     chunk, and the half that is a pure function of the seed, so it can be
//     made anywhere and arrive in any order. See core/net/terrain_share.hpp.
//
// **What a guest can play is what this console has loaded.** The grid has one
// centre (core/render/world_streamer.hpp), so a guest who walks past its edge
// runs out of ground until they come back. `starvedGuest` is how the screen
// says so.

#include "core/entity/item_entity.hpp"
#include "core/entity/player_body.hpp"
#include "core/gui/chat_log.hpp"
#include "core/item/use.hpp"
#include "core/net/entities.hpp"
#include "core/net/session.hpp"
#include "core/net/terrain_share.hpp"
#include "core/net/world_server.hpp"
#include "core/settings/world_settings.hpp"
#include "core/render/world_streamer.hpp"
#include "platform/ctr/local_link.hpp"
#include "platform/ctr/session_link.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mc::ctr {

struct Camera;

class HostPlay : public net::link::SessionListener, public net::ServerWorld {
public:
    HostPlay();
    ~HostPlay() override;

    HostPlay(const HostPlay&) = delete;
    HostPlay& operator=(const HostPlay&) = delete;

    // Puts the beacon on the air and opens the session. False with `*error` in
    // words a player can act on, which for this is almost always the wireless
    // switch.
    //
    // `world` is what a guest needs in order to generate terrain for this
    // world and what its own build is checked against, so this is called once
    // the world is open and its seed is known.
    bool open(const std::string& worldName, const std::string& hostName,
              const net::link::GeneratorId& world, std::string* error);

    // **The same session, served over the internet.** `link` is a connection
    // the rendezvous server has already introduced this console through, so
    // there is no radio to bring up and no beacon to put on the air -- the
    // directory on the server is what a joiner finds this world in. Everything
    // else is identical: the same handshake, the same world server, the same
    // terrain coming back from the guests. See platform/ctr/online.hpp.
    void openOnline(SessionLink& link, const std::string& worldName,
                    const std::string& hostName, const net::link::GeneratorId& world);

    // **The session before the world, which is what an internet host's lobby
    // is.** The handshake, the player list and the chat run over the link the
    // rendezvous server introduced; the world server does not exist yet,
    // because the world has not been opened and there is nothing to serve.
    //
    // A guest that arrives here is welcomed and listed, and sits on its own
    // lobby screen waiting for the Login the world server sends. `open` below
    // is what sends it: called once the world is up, it adopts this session
    // rather than replacing it, so nobody who joined while the code was on
    // screen has to join again. See `Menu::handleOnlineHost`.
    void openLobby(SessionLink& link, const std::string& worldName,
                   const std::string& hostName, const net::link::GeneratorId& world);

    // Once a frame while the lobby is up. **The link is not serviced here**:
    // the menu is pumping the login on the same socket, and a second service
    // in the same frame would take the events before it saw them. In game it
    // is the other way round -- there is no menu, and `pump` does both.
    void pumpLobby();

    // Whether `openLobby` ran and `open` has not yet: a session with guests in
    // it and no world behind it.
    bool inLobby() const { return lobby_; }

    // **Nobody is going to answer for about this long.** The same warning an
    // applet gets -- see `net::link::Msg::Away` -- for the other gap in which
    // this console stops pumping: opening the world, between the lobby's last
    // frame and the game loop's first. Without it a guest who joined the lobby
    // would be counting a card read against `kTimeoutMs`.
    void announceAway(u32 expectedMs);

    // The names in the session, host first, for a screen that lists them.
    void players(std::vector<net::link::Player>* out) const;

    // **Armed before the world opens, used when it does.** `open` is called
    // from inside `runGame`, once level.dat has been read and the seed is
    // known, and by then there is nowhere left to ask which kind of session
    // this is. So the caller says so beforehand and `open` takes the online
    // road instead of starting the radio. Null is the ordinary local case.
    void serveOnline(SessionLink* link) { pending_ = link; }

    // What the streamer installs so the generator can find terrain a guest
    // made. Valid for the life of this object.
    render::WorldStreamer::TerrainSource terrainSource();

    // The other direction: every block this world writes, so the guests can be
    // told. Installed on the streamer straight after it opens.
    static void blockWatcher(void* context, i32 x, int y, i32 z);

    // Tells whoever is still connected why, then takes the network down.
    void close(const std::string& reason);

    bool active() const { return link_ != nullptr && link_->active(); }

    // Whether the session ever came up. Distinguishes "the world was hosted
    // and is now closed" from "local wireless never started", which are the
    // same state afterwards and want different things said about them.
    bool everOpened() const { return everOpened_; }
    int guests() const { return session_.guestCount(); }

    // The worst round trip to any guest, for the debug page.
    u32 rttMs() const { return session_.worstRttMs(); }

    // Once a frame, before the streamer looks at the grid -- the same place
    // `NetPlay::pump` sits, and for the same reason: what the other consoles
    // said should be in before anything reads the world.
    //
    // **The streamer, the renderer and the effects are borrowed for the length
    // of the call**, which is the whole of the window in which a guest's dig
    // or place is applied. Nothing here holds them afterwards.
    void pump(render::WorldStreamer& world, render::ChunkRenderer& chunks,
              const item::Effects& effects, entity::ItemEntitySystem& drops, gui::ChatLog& chat,
              const u8* fontWidths);

    // Terrain columns guests have made for this world, and the ones that
    // arrived after the generator had already made them. For the debug page.
    u32 terrainUsed() const { return pool_.used(); }
    u32 terrainLate() const { return pool_.late(); }

    // Columns served to guests, how many they are still owed, and stacks on
    // the ground the guests have been told about.
    u32 columnsSent() const { return server_.columnsSent(); }
    u32 columnsOwed() const { return server_.columnsQueued(); }
    u32 itemsSpawned() const { return server_.itemsSpawned(); }
    u32 mobsSpawned() const { return server_.mobsSpawned(); }

    // True when somebody in the session is standing where this console has no
    // world loaded, so nothing more can be sent to them.
    bool starvedGuest() const;

    // How this world is played, for the guests. Called when the world opens and
    // again whenever the pause menu changes it; guests already in the session
    // are told, not just the ones who join later.
    void setWorldRules(settings::Gamemode gamemode, settings::Difficulty difficulty);

    // **The other players, as this console sees them.** A host is not a client
    // of its own server, so the bodies and names of the guests are put here by
    // `WorldServer`'s local sink rather than by a stream -- see
    // `WorldServer::setLocalSink`. The renderer, the map and the crosshair all
    // read it exactly as they read a guest's.
    const net::RemoteEntities& entities() const { return entities_; }
    net::RemoteEntities& entities() { return entities_; }

    // **This console swinging at a guest.** The host is not a client of its own
    // server, so a blow it lands goes out through the server rather than
    // through a stream. False when that entity id is not somebody here.
    bool attackPlayer(i32 targetEntityId, int heldItem)
    {
        return server_.hostAttack(targetEntityId, heldItem);
    }

    // **Somebody hit this console's player.** Once per blow. The host owns its
    // own health exactly as every player does at protocol 2, so what a blow
    // costs is worked out by the frame loop here rather than by the server.
    bool takeHit(net::IncomingHit* out);

    // One 20 Hz tick of those bodies: the walk toward wherever each guest last
    // said they were, and the light they are standing in.
    void tickEntities(const tick::TickWorld* world) { entities_.tick(world); }

    // The end of the player's own tick, where `la.J()` reports a position.
    void reportPose(const entity::PlayerBody& body, const Camera& camera);

    // A line the host typed, to everyone. The host's own chat log is written
    // by the caller, which is where a1.1.2 writes it too.
    void say(const std::string& text);

private:
    // The session over a link that is already up: the handshake, the player
    // list and the chat, and nothing that needs a world.
    void beginSession(SessionLink& link, const std::string& worldName,
                      const std::string& hostName, const net::link::GeneratorId& world);

    // The world server, once there is a world: opened, and handed everybody
    // the lobby already let in. Both roads through `open` end here.
    void startWorld(const net::link::GeneratorId& world);

    void onPlayerJoined(u8 playerId, const std::string& name) override;
    void onPlayerLeft(u8 playerId, const std::string& reason) override;
    void onPose(const net::link::Pose& pose) override;
    void onChat(u8 playerId, const std::string& text) override;
    void onTerrainPart(u8 playerId, const u8* body, usize size) override;
    void onGamePacket(u8 playerId, const u8* data, usize size) override;

    // ---- net::ServerWorld ------------------------------------------------
    const world::ChunkColumn* column(i32 chunkX, i32 chunkZ) const override;
    i64 timeTicks() const override;
    void spawnPoint(i32* x, i32* y, i32* z) const override;
    double settleFeet(double x, double feetY, double z) const override;
    void breakBlock(i32 x, int y, i32 z, int heldItem) override;
    void placeBlock(const PlaceRequest& request) override;
    void attackEntity(const AttackRequest& request) override;
    void attackHost(const AttackRequest& request) override;
    void showChat(const std::string& line) override;
    entity::ItemEntitySystem* items() override { return drops_; }
    entity::MobSystem* mobs() override
    {
        // The same pool a click reaches through, so the session never has to
        // be handed one of its own.
        return effects_ != nullptr ? effects_->entities.mobs : nullptr;
    }
    void spawnItem(double x, double y, double z, item::ItemId id, int count, double mx,
                   double my, double mz) override;

    // The server's way back onto the link. One player at a time; false is the
    // window being full, which is back-pressure rather than a failure.
    static bool sendToGuest(void* context, u8 playerId, const u8* data, usize size);

    // The other half of the split `sendToGuest` makes: a packet about a player
    // that this console has to apply to itself rather than send.
    static void applyLocally(void* context, const net::Packet& packet);

    // `ctr::setLinkPauseHook`'s callback. See `net::link::Msg::Away`.
    static void linkPaused(void* context, u32 expectedMs);

    // Asks for the ground around a guest that has just told us where it is.
    // **Nothing waits on the answer**: a column that does not arrive in time
    // is generated here as it always was, so a guest that walks out of range
    // costs the host nothing but a few unanswered requests.
    void wantAround(const net::link::Pose& pose);

    // Set for the length of one `pump`, so what the session reports can be
    // written straight into the log the player is looking at, and so a guest's
    // edit can reach the world it is an edit of.
    gui::ChatLog* chat_ = nullptr;
    const u8* fontWidths_ = nullptr;
    render::WorldStreamer* streamer_ = nullptr;
    render::ChunkRenderer* chunks_ = nullptr;
    const item::Effects* effects_ = nullptr;
    entity::ItemEntitySystem* drops_ = nullptr;

    net::RemoteEntities entities_;
    net::IncomingHit hit_;

    // The radio, when the guests are in the room, and the thing every line
    // below talks through, whichever it is.
    LocalLink local_;
    SessionLink* link_ = nullptr;
    SessionLink* pending_ = nullptr;
    net::link::HostSession session_;
    net::link::TerrainPool pool_;
    net::WorldServer server_;
    std::string hostName_;
    bool everOpened_ = false;
    // A session with a player list and no world behind it yet. See `openLobby`.
    bool lobby_ = false;
    int advertised_ = -1;
    u16 tick_ = 0;
    u32 posedAtMs_ = 0;
    // Whether somebody was standing outside the loaded world last frame, so
    // the chat line is written when it changes rather than every frame.
    bool wasStarved_ = false;
    bool wasDisplaced_ = false;
    // How this world is played, kept because a hit needs it: a Creative fist
    // lands in full and leaves the monster with nobody to chase.
    settings::Gamemode gamemode_ = settings::Gamemode::Survival;
};

}  // namespace mc::ctr
