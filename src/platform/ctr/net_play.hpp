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
// health to show and no container contents to edit. The other players, the mobs,
// the boats and the carts are drawn -- `entities()` is the pool, bound into the
// scene and into the click targets by `runGame` -- and from a host of this family
// so are the falling blocks and the lit TNT. A boat or a cart is ridden by asking
// the host (`ride`) and steered here once it says yes; see
// `RemoteEntities::driveReport`.

#include "core/entity/item_entity.hpp"
#include "core/entity/player_body.hpp"
#include "core/gui/chat_log.hpp"
#include "core/item/inventory.hpp"
#include "core/net/packet_channel.hpp"
#include "core/net/entities.hpp"
#include "core/net/player_sync.hpp"
#include "core/net/tile_sync.hpp"
#include "core/render/world_streamer.hpp"

#include <string>
#include <string_view>
#include <vector>

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

    // **A right click on the host's boat or cart: "let me in".** Nothing
    // happens here until the host answers; then this console steers it. Silent
    // unless the host is of this family. See `net::kNoVehicle`.
    //
    // **A furnace cart takes the same click**, with what is in the hand: the
    // host puts the coal in its cart and points it away from this player.
    void ride(i32 vehicleEntityId, int heldItem);

    // **The host's blast reached this player.** True once per blast. See
    // `net::IncomingBlast`.
    bool takeBlast(net::IncomingBlast* out) { return entities_.takeBlast(out); }

    // **A boat this console's player put down, for the host to put down
    // instead** -- the cell its ray found. See `net::kPlaceOnCell`.
    void placeBoat(int heldItem, i32 x, int y, i32 z);

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

    // **The skin this console is wearing**, a 64 x 32 RGBA page, sent to the host
    // once the Login arrives -- see core/net/player_skin.hpp. Copied; a page of
    // the wrong size is ignored. Only ever sent when extensions are allowed.
    void setOwnSkin(const std::vector<u8>& page);
    bool extensionsAllowed() const { return extensions_; }
    // The host shares its boats, carts, falling blocks and TNT: see
    // `RemoteEntities::sharesVehicles`.
    bool vehiclesShared() const { return extensions_ && entities_.sharesVehicles(); }

    // **Somebody hit this player.** True once per blow, and then the blow is
    // gone -- the caller applies it to the player's own vitals, which is the
    // only place health lives in this protocol. See `net::IncomingHit`.
    bool takeHit(net::IncomingHit* out);

    // ---- chests and furnaces ---------------------------------------------
    //
    // **The host owns the contents; this console mirrors them while the screen
    // is up.** See core/net/tile_sync.hpp. Nothing here is sent to a server that
    // is not this port: `extensionsAllowed` gates all of it.
    //
    // Asks the host for the container at x, y, z. `tileArrived` turns true when
    // its contents have been written into this console's own copy of the
    // column, which is when the screen can open on it without lying.
    void openTile(i32 x, int y, i32 z);
    bool tileArrived(i32 x, int y, i32 z) const;
    // The screen is shut.
    void closeTile();
    // **After a click on the open screen**: sends every part of the container
    // whose contents differ from what the host last said, so it learns what was
    // moved. `world` is this console's own.
    void sendTileEdits(const tick::TickWorld& world, i32 x, int y, i32 z);
    // **A chest cart's 27 slots**, the same way: the host's, mirrored into this
    // console's copy of the cart while its screen is up. `openTile` with the
    // cart's entity id and `net::kCartTileY` asks; this sends a click's worth
    // of difference back. `cartId` is the cart's local id, as the screen has it.
    void sendCartEdits(entity::MinecartSystem& carts, u32 cartId);
    // True once after the host said the container is gone.
    bool takeTileClosed()
    {
        const bool closed = tileClosed_;
        tileClosed_ = false;
        return closed;
    }

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

    // What the host last said about each part of the open container, encoded,
    // so a click is the only thing that reads as an edit. See `sendTileEdits`.
    struct TileMirror {
        bool used = false;
        i32 x = 0;
        int y = 0;
        i32 z = 0;
        std::vector<u8> last;
    };
    TileMirror tiles_[tick::kMaxChestParts];
    bool tileAsked_ = false;
    i32 tileAskX_ = 0;
    int tileAskY_ = 0;
    i32 tileAskZ_ = 0;
    bool tileArrived_ = false;
    bool tileClosed_ = false;

    i32 entityId_ = 0;
    int columns_ = 0;
    bool placed_ = false;
    bool closed_ = false;
    bool extensions_ = false;
    std::vector<u8> ownSkin_;
    std::string title_;
    std::string detail_;
};

}  // namespace mc::ctr
