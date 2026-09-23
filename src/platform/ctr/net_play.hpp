#pragma once

// A multiplayer session as the game loop sees it: what the server's packets do
// to the world and the player, and what the player's ticks and clicks become.
//
// `runGame` is one loop for both kinds of world and takes one of these, or null
// for single player. Everything a session changes about that loop is a call on
// this object at the place in the loop a1.1.2 would have made it -- the event
// pump where `gs.g()` polls its handler, the movement report at the end of the
// player's tick where `la.e_()` runs `J()`, the dig statuses around the block
// breaker where `nj` sits -- so the loop reads the same with it and without it.
//
// What a protocol-2 server cannot tell a client is not guessed at: there is no
// health to show, no container contents to edit, and the other entities are
// sent but not drawn yet.

#include "core/entity/item_entity.hpp"
#include "core/entity/player_body.hpp"
#include "core/gui/chat_log.hpp"
#include "core/item/inventory.hpp"
#include "core/net/packet_channel.hpp"
#include "core/net/entities.hpp"
#include "core/net/player_sync.hpp"
#include "core/render/world_streamer.hpp"

#include <string>
#include <string_view>

namespace mc::ctr {

struct Camera;
class Overlay;

class NetPlay {
public:
    explicit NetPlay(net::PacketChannel& channel) : session_(channel) {}

    // Applies everything the session has delivered. Main thread, before the
    // streamer's update, with the renderer's frame begun.
    void pump(render::WorldStreamer& world, render::ChunkRenderer& chunks, Overlay& overlay,
              gui::ChatLog& chat, const u8* fontWidths, entity::PlayerBody& body, Camera& camera);

    // The end of one player tick: `la.J()`, and the other entities' own tick --
    // the walk toward wherever the server last put them. `alive` is the
    // player's own vitals; with the crouch it is what the others are told
    // about how this player stands, when the host is one that understands it.
    // See `net::StanceReporter`.
    void tick(const entity::PlayerBody& body, const Camera& camera,
              const item::Inventory& inventory, const tick::TickWorld* world, bool alive);

    // The other players and the items on the ground, for the renderer and the
    // debug page. See core/net/entities.hpp.
    const net::RemoteEntities& entities() const { return entities_; }
    net::RemoteEntities& entities() { return entities_; }

    // **The held item goes first**, as it does on `place` and on `digProgress`:
    // a click that finishes the block on the spot is the one dig the host never
    // sees a progress packet for, and the host decides the drop from what it
    // last heard the guest was holding. Without this, a slot changed and a
    // block one-shotted in the same breath dropped the previous tool's spoil.
    void digStart(i32 x, int y, i32 z, int face, bool broke, int heldItem);
    void digProgress(i32 x, int y, i32 z, int face, bool broke, int heldItem);
    void digStop();
    // `nj.a(dm, cn, ev, ...)`: every right click on a block, before the client
    // tries it itself. `heldItem` 0 is an empty hand.
    void place(int heldItem, i32 x, int y, i32 z, int face);
    void swing();

    // **A left click on an entity the server owns.** See `packet::UseEntity`:
    // protocol 2 has no such packet, so this is silent unless the session is
    // one this port hosts. `targetEntityId` is the server's id for the thing
    // that was hit, and `heldItem` is reported first when it has changed --
    // the hit lands as hard as whatever the *server* thinks is in the hand, so
    // a sword the server has not heard about would punch for one.
    void attackEntity(i32 targetEntityId, int heldItem);

    // **A right click in the air, for the host to act on** -- the bow, which
    // a1.1.2 never puts on the wire (see `net::makeUseItem`). The pose goes
    // first, so the arrow leaves along the look the player has *now* rather
    // than the one last tick reported. False when the other end is not this
    // port, and then the caller fires the arrow locally, as a1.1.2 does.
    bool useItem(int heldItem, const entity::PlayerBody& body, const Camera& camera);

    // **Whether the other end is this port**, and so understands the packets
    // protocol 2 does not have: `packet::UseEntity`, and the stance --
    // `EntityAction`, `EntityStatus` and `Respawn`. Off by default, because the
    // other end is usually a Java server that would end the connection over an
    // id its table does not have.
    void allowExtensions(bool allow) { extensions_ = allow; }
    bool extensionsAllowed() const { return extensions_; }

    // **Somebody hit this player.** True once per blow, and then the blow is
    // gone -- the caller applies it to the player's own vitals, which is the
    // only place health lives in this protocol. See `net::IncomingHit`.
    bool takeHit(net::IncomingHit* out);

    void chat(std::string_view text);

    // Sends every item the player threw this frame to the server, and keeps it
    // on screen until the server's own spawn takes it over. See
    // `net::pickupSpawnFor` and `net::RemoteEntities::predictDrop`.
    void forwardDrops(entity::ItemEntitySystem& drops);

    // **This client's own entity id**, as the Login packet gave it. Zero until
    // then. It is what every other console names this player with, and
    // therefore what picks their colour -- see `net::playerColour`.
    i32 selfEntityId() const { return entityId_; }

    // The server has put the player somewhere, which is when a1.1.2 closes
    // its Downloading Terrain screen.
    bool placed() const { return placed_; }

    // **What to put in front of a player who is still waiting.** A download
    // that does not finish is the one failure whose cause is invisible from
    // the outside, so the screen says which stage it reached and how much has
    // arrived -- "connecting" with nothing in is a network or an address,
    // "playing" with bytes but no columns is the server, and columns climbing
    // is simply a slow card and a slow link.
    const char* stage() const;
    u64 bytesIn() const { return session_.bytesIn(); }
    u32 packetsIn() const { return session_.packetsIn(); }
    int columns() const { return columns_; }

    bool closed() const { return closed_; }
    const std::string& closeTitle() const { return title_; }
    const std::string& closeDetail() const { return detail_; }

private:
    void apply(const net::Packet& packet, render::WorldStreamer& world,
               render::ChunkRenderer& chunks, Overlay& overlay, gui::ChatLog& chat,
               const u8* fontWidths, entity::PlayerBody& body, Camera& camera);

    net::PacketChannel& session_;
    net::MovementReporter movement_;
    net::InventoryReporter inventory_;
    net::DigReporter dig_;
    net::HeldItemReporter held_;
    net::StanceReporter stance_;
    net::Packet scratch_[3];
    static_assert(net::StanceReporter::kMaxPackets <= 3, "the stance reports into scratch_");
    net::PacketChannel::Event event_;
    net::RemoteEntities entities_;
    net::IncomingHit hit_;

    i32 entityId_ = 0;
    int columns_ = 0;
    bool placed_ = false;
    bool closed_ = false;
    bool extensions_ = false;
    std::string title_;
    std::string detail_;
};

}  // namespace mc::ctr
