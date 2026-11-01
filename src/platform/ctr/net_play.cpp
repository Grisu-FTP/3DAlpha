// The game loop's side of a multiplayer session. See net_play.hpp.

#include "platform/ctr/net_play.hpp"

#include "platform/ctr/overlay.hpp"
#include "platform/ctr/renderer.hpp"

namespace mc::ctr {

namespace {

constexpr float kDegreesPerRadian = 180.0f / 3.14159265f;

}  // namespace

void NetPlay::setOwnSkin(const std::vector<u8>& page)
{
    ownSkin_.resize(net::kSkinPageBytes);
    if (!net::sanitizeSkinPage(page.data(), page.size(), ownSkin_.data())) {
        ownSkin_.clear();
    }
}

void NetPlay::pump(render::WorldStreamer& world, render::ChunkRenderer& chunks, Overlay& overlay,
                   gui::ChatLog& chat, const u8* fontWidths, entity::PlayerBody& body,
                   Camera& camera)
{
    while (!closed_ && session_.poll(&event_)) {
        switch (event_.kind) {
        case net::PacketChannel::Event::Kind::LoggedIn:
            entityId_ = event_.entityId;
            // So "they are in it" can tell this player from everybody else.
            entities_.setSelf(entityId_);
            // **Who this console is, said once**: the host attributes the page
            // to this connection whatever id is on it.
            if (extensions_ && ownSkin_.size() == net::kSkinPageBytes) {
                session_.send(net::makePlayerSkin(entityId_, ownSkin_.data()));
            }
            break;
        case net::PacketChannel::Event::Kind::Column:
            world.supplyColumn(std::move(event_.column));
            ++columns_;
            break;
        case net::PacketChannel::Event::Kind::Region:
            world.applyRegion(chunks, event_.region);
            break;
        case net::PacketChannel::Event::Kind::Closed:
            closed_ = true;
            title_ = event_.title;
            detail_ = event_.detail;
            break;
        case net::PacketChannel::Event::Kind::Packet:
            apply(event_.packet, world, chunks, overlay, chat, fontWidths, body, camera);
            break;
        }
    }
}

const char* NetPlay::stage() const
{
    switch (session_.state()) {
    case net::PacketChannel::State::Idle:
    case net::PacketChannel::State::Connecting:
        return "connecting";
    case net::PacketChannel::State::LoggingIn:
        return "logging in";
    case net::PacketChannel::State::Playing:
        return placed_ ? "playing" : "waiting for a position";
    case net::PacketChannel::State::Closed:
        return "closed";
    }
    return "";
}

void NetPlay::apply(const net::Packet& p, render::WorldStreamer& world,
                    render::ChunkRenderer& chunks, Overlay& overlay, gui::ChatLog& chat,
                    const u8* fontWidths, entity::PlayerBody& body, Camera& camera)
{
    net::Teleport teleport;
    if (net::readTeleport(p, &teleport)) {
        // `gy.a(eh)`: the player goes where the server says and stops dead, and
        // the answer is the same packet with the client's own x, feet, eye and
        // z written into it -- the server ignores every move until that echo
        // matches what it sent.
        if (teleport.hasPosition) {
            body.setFeet(teleport.x, teleport.feetY, teleport.z);
            body.motionX = 0.0;
            body.motionY = 0.0;
            body.motionZ = 0.0;
            body.snapRenderPosition();
            camera.x = body.x;
            camera.y = body.eyeY();
            camera.z = body.z;
        }
        float yaw = camera.yaw * kDegreesPerRadian;
        float pitch = camera.pitch * kDegreesPerRadian;
        if (teleport.hasLook) {
            yaw = teleport.yaw;
            pitch = teleport.pitch;
            camera.yaw = yaw / kDegreesPerRadian;
            camera.pitch = pitch / kDegreesPerRadian;
        }

        if (teleport.hasPosition && teleport.hasLook) {
            session_.send(net::makePositionLook(body.x, body.box.minY, body.posY, body.z, yaw,
                                                pitch, false));
        } else if (teleport.hasPosition) {
            session_.send(net::makePosition(body.x, body.box.minY, body.posY, body.z, false));
        } else if (teleport.hasLook) {
            session_.send(net::makeLook(yaw, pitch, false));
        } else {
            session_.send(net::makeFlying(false));
        }
        placed_ = placed_ || teleport.hasPosition;
        return;
    }

    // **The entities first**, because everything below is about the world or
    // this player and nothing below is about them. See core/net/entities.hpp.
    entities_.setLocalFeet(body.x, body.box.minY, body.z);
    if (entities_.apply(p, world.worldTick())) {
        return;
    }

    switch (p.id) {
    case net::packet::UseEntity: {
        // **The direction protocol 2 never had, and neither does protocol 4.**
        // Here it means "that entity hit you", and it is the one thing about a
        // fight that cannot be worked out locally: who swung, and from where.
        // Only a 3DAlpha host ever sends it -- see `packet::UseEntity` -- so a
        // Java server cannot reach this.
        //
        // **What it cost is worked out here**, from the attacker's held item,
        // because protocol 2 has no health packet and never will: this
        // console owns this player's health, exactly as it owns their fall
        // damage. See `net::IncomingHit`.
        if (i32(p.integer(1)) != entityId_ || p.integer(2) == 0) {
            break;
        }
        // Another player, one of the host's arrows or one of its mobs -- see
        // `RemoteEntities::hitFrom`.
        net::IncomingHit hit;
        if (entities_.hitFrom(i32(p.integer(0)), &hit)) {
            hit_ = hit;
        }
        break;
    }

    case net::packet::SpawnPosition:
        world.setRemoteSpawn(i32(p.integer(0)), i32(p.integer(1)), i32(p.integer(2)));
        break;

    case net::packet::TimeUpdate:
        world.setRemoteTime(p.integer(0));
        break;

    case net::packet::Chat:
        // `lu.a(String)`, the same log a single-player message goes to.
        chat.add(fontWidths, p.text(0));
        break;

    case net::packet::PreChunk:
        // Mode 1 makes an empty column in a1.1.2 for the Map Chunk to fill;
        // here the column simply is not until it arrives.
        if (p.integer(2) == 0) {
            world.unloadColumn(i32(p.integer(0)), i32(p.integer(1)));
        }
        break;

    case net::packet::BlockChange:
        world.applyServerBlock(chunks, i32(p.integer(0)), int(p.integer(1)), i32(p.integer(2)),
                               block::BlockId(p.integer(3)), u8(p.integer(4)));
        break;

    case net::packet::MultiBlockChange: {
        // `gy.a(na)`: x << 12 | z << 8 | y, relative to the chunk.
        const i32 baseX = i32(p.integer(0)) * 16;
        const i32 baseZ = i32(p.integer(1)) * 16;
        for (usize i = 0; i < p.changeCoords.size(); ++i) {
            const u16 packed = u16(p.changeCoords[i]);
            world.applyServerBlock(chunks, baseX + ((packed >> 12) & 15), packed & 255,
                                   baseZ + ((packed >> 8) & 15), block::BlockId(p.changeIds[i]),
                                   p.changeData[i]);
        }
        break;
    }

    case net::packet::TileSync: {
        // The host's word about a container this console asked for. Written into
        // the column's own tile entity, where the container screen already
        // reads and clicks, and remembered so the next click is the only thing
        // that reads as a change.
        net::TileSnapshot snapshot;
        tick::TickWorld* tick = world.worldTick();
        if (!net::readTileSync(p, &snapshot)) {
            break;
        }
        // **A chest cart's**, into this console's copy of the cart.
        const bool cart = snapshot.y == net::kCartTileY;
        entity::MinecartSystem* carts = entities_.minecarts();
        if (cart ? carts == nullptr || !net::writeCart(*carts, snapshot)
                 : tick == nullptr || !net::writeTile(*tick, snapshot, true)) {
            break;
        }
        net::TileSnapshot now;
        TileMirror* slot = nullptr;
        for (TileMirror& mirror : tiles_) {
            if (mirror.used && mirror.x == snapshot.x && mirror.y == snapshot.y
                && mirror.z == snapshot.z) {
                slot = &mirror;
                break;
            }
        }
        for (TileMirror& mirror : tiles_) {
            if (slot == nullptr && !mirror.used) {
                slot = &mirror;
            }
        }
        const bool readBack = cart ? net::readCart(*carts, snapshot.x, &now)
                                   : net::readTile(*tick, snapshot.x, snapshot.y, snapshot.z,
                                                   &now);
        if (slot != nullptr && readBack) {
            slot->used = true;
            slot->x = snapshot.x;
            slot->y = snapshot.y;
            slot->z = snapshot.z;
            net::encodeTile(now, &slot->last);
        }
        if (tileAsked_ && snapshot.x == tileAskX_ && snapshot.y == tileAskY_
            && snapshot.z == tileAskZ_) {
            tileArrived_ = true;
        }
        break;
    }

    case net::packet::TileClose:
        tileClosed_ = true;
        for (TileMirror& mirror : tiles_) {
            mirror = TileMirror{};
        }
        break;

    case net::packet::PlayerInventory:
        if (net::applyInventory(p, &overlay.editInventory())) {
            overlay.inventoryEdited();
        }
        break;

    case net::packet::AddToInventory:
        // `gy.a(ld)`: a pickup the server has already taken off the ground.
        if (p.integer(0) >= 0) {
            overlay.editInventory().addStack(item::ItemId(p.integer(0)), int(p.integer(1)),
                                             i16(p.integer(2)));
            overlay.inventoryEdited();
        }
        break;

    default:
        // Other players, mobs, items on the ground, vehicles and tile entity
        // payloads arrive and are parsed, and are not drawn yet.
        break;
    }
}

void NetPlay::openTile(i32 x, int y, i32 z)
{
    if (!extensions_) {
        return;
    }
    for (TileMirror& mirror : tiles_) {
        mirror = TileMirror{};
    }
    tileAsked_ = true;
    tileArrived_ = false;
    tileClosed_ = false;
    tileAskX_ = x;
    tileAskY_ = y;
    tileAskZ_ = z;
    session_.send(net::makeTileOpen(x, y, z));
}

bool NetPlay::tileArrived(i32 x, int y, i32 z) const
{
    return tileAsked_ && tileArrived_ && x == tileAskX_ && y == tileAskY_ && z == tileAskZ_;
}

void NetPlay::closeTile()
{
    if (!tileAsked_) {
        return;
    }
    if (extensions_) {
        session_.send(net::makeTileClose(tileAskX_, tileAskY_, tileAskZ_));
    }
    tileAsked_ = false;
    tileArrived_ = false;
    for (TileMirror& mirror : tiles_) {
        mirror = TileMirror{};
    }
}

void NetPlay::sendCartEdits(entity::MinecartSystem& carts, u32 cartId)
{
    const int index = carts.indexOfId(cartId);
    if (!extensions_ || !tileAsked_ || index < 0) {
        return;
    }
    net::TileSnapshot snapshot;
    if (!net::readCart(carts, carts[index].entityId, &snapshot)) {
        return;
    }
    std::vector<u8> encoded;
    net::encodeTile(snapshot, &encoded);
    for (TileMirror& mirror : tiles_) {
        if (!mirror.used || mirror.x != snapshot.x || mirror.y != snapshot.y) {
            continue;
        }
        if (mirror.last != encoded) {
            mirror.last = encoded;
            session_.send(net::makeTileSync(snapshot));
        }
        return;
    }
}

void NetPlay::sendTileEdits(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    if (!extensions_ || !tileAsked_) {
        return;
    }
    tick::ChestPart parts[tick::kMaxChestParts];
    const int count = net::tileParts(world, x, y, z, parts);
    std::vector<u8> encoded;
    for (int i = 0; i < count; ++i) {
        net::TileSnapshot snapshot;
        if (!net::readTile(world, parts[i].x, parts[i].y, parts[i].z, &snapshot)) {
            continue;
        }
        net::encodeTile(snapshot, &encoded);
        for (TileMirror& mirror : tiles_) {
            if (!mirror.used || mirror.x != snapshot.x || mirror.y != snapshot.y
                || mirror.z != snapshot.z) {
                continue;
            }
            // Slots only: the burn and the cook are the host's, so a furnace
            // that merely kept cooking here is not an edit.
            net::TileSnapshot before;
            if (net::decodeTile(mirror.last.data(), mirror.last.size(), &before)
                && net::sameTile(before, snapshot, false)) {
                break;
            }
            mirror.last = encoded;
            session_.send(net::makeTileSync(snapshot));
            break;
        }
    }
}

void NetPlay::tick(const entity::PlayerBody& body, const Camera& camera,
                   const item::Inventory& inventory, const tick::TickWorld* world, bool alive)
{
    entities_.tick(world);

    // `la.J()` checks the inventory before it reports the move.
    const int sent = inventory_.tick(inventory, scratch_);
    for (int i = 0; i < sent; ++i) {
        session_.send(scratch_[i]);
    }

    // **What is in the hand, the tick it changes.** a1.1.2's `nj.e()` reports
    // it only ahead of a dig or a place, so everybody else saw a player's hand
    // empty until they next used it; later versions' `updateController` checks
    // it every tick, and so does this. Changing slots reaches a Java server as
    // the packet it always parsed.
    {
        net::Packet held;
        if (held_.changed(int(inventory.selectedItem()), &held)) {
            session_.send(held);
        }
    }

    net::PlayerPose pose;
    pose.x = body.x;
    pose.feetY = body.box.minY;
    pose.eyeY = body.posY;
    pose.z = body.z;
    pose.yaw = camera.yaw * kDegreesPerRadian;
    pose.pitch = camera.pitch * kDegreesPerRadian;
    pose.onGround = body.onGround;
    session_.send(movement_.tick(pose));

    // **After the move**, so a Respawn reaches the host with the spawn point
    // already reported and the fresh spawn it answers with stands there.
    if (extensions_) {
        const int n = stance_.tick(entityId_, body.sneaking, alive, scratch_);
        for (int i = 0; i < n; ++i) {
            session_.send(scratch_[i]);
        }
        // **The boat or cart this player is steering**, after it moved this
        // tick: where it went, that they got out, or that it broke.
        if (entities_.driveReport(&scratch_[0]) > 0) {
            session_.send(scratch_[0]);
        }
    }
}

void NetPlay::digStart(i32 x, int y, i32 z, int face, bool broke, int heldItem)
{
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    const int n = dig_.start(x, y, z, face, broke, scratch_);
    for (int i = 0; i < n; ++i) {
        session_.send(scratch_[i]);
    }
}

void NetPlay::digProgress(i32 x, int y, i32 z, int face, bool broke, int heldItem)
{
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    const int n = dig_.progress(x, y, z, face, broke, scratch_);
    for (int i = 0; i < n; ++i) {
        session_.send(scratch_[i]);
    }
}

void NetPlay::digStop()
{
    if (dig_.stop(scratch_) > 0) {
        session_.send(scratch_[0]);
    }
}

void NetPlay::place(int heldItem, i32 x, int y, i32 z, int face)
{
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    session_.send(net::makePlace(heldItem == 0 ? -1 : heldItem, x, y, z, face));
}

void NetPlay::swing()
{
    session_.send(net::makeArmSwing(entityId_));
}

void NetPlay::attackEntity(i32 targetEntityId, int heldItem)
{
    if (!extensions_ || targetEntityId == 0) {
        return;
    }
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    session_.send(net::makeUseEntity(entityId_, targetEntityId, true));
}

void NetPlay::ride(i32 vehicleEntityId, int heldItem)
{
    if (!extensions_ || vehicleEntityId == 0 || entityId_ == 0) {
        return;
    }
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    session_.send(net::makeUseEntity(entityId_, vehicleEntityId, false));
}

void NetPlay::placeBoat(int heldItem, i32 x, int y, i32 z)
{
    if (!extensions_) {
        return;
    }
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    session_.send(net::makePlace(heldItem, x, y, z, net::kPlaceOnCell));
}

bool NetPlay::useItem(int heldItem, const entity::PlayerBody& body, const Camera& camera)
{
    if (!extensions_) {
        return false;
    }
    net::Packet held;
    if (held_.changed(heldItem, &held)) {
        session_.send(held);
    }
    net::PlayerPose pose;
    pose.x = body.x;
    pose.feetY = body.box.minY;
    pose.eyeY = body.posY;
    pose.z = body.z;
    pose.yaw = camera.yaw * kDegreesPerRadian;
    pose.pitch = camera.pitch * kDegreesPerRadian;
    pose.onGround = body.onGround;
    session_.send(movement_.tick(pose));
    session_.send(net::makeUseItem(heldItem));
    return true;
}

bool NetPlay::takeHit(net::IncomingHit* out)
{
    if (!hit_.present) {
        return false;
    }
    *out = hit_;
    hit_ = net::IncomingHit{};
    return true;
}

void NetPlay::chat(std::string_view text)
{
    session_.send(net::makeChat(text));
}

void NetPlay::forwardDrops(entity::ItemEntitySystem& drops)
{
    if (drops.count() == 0) {
        return;
    }
    // **Only what this console threw.** The same pool holds the server's own
    // items, spawned into it by `RemoteEntities` and carrying the server's id
    // -- handing those back would ask the server to make a second copy of
    // everything it had just sent, every tick, for ever. One stack dropped in
    // front of a guest became an endless fountain of them; that is what
    // "dropping an item drops it infinitely" was.
    //
    // **What was thrown stays on screen** under a provisional id until the
    // server's spawn takes it over, or is taken away if none comes -- see
    // `RemoteEntities::predictDrop`. Past the few that can wait at once, a
    // throw goes the original way: sent and let go.
    for (int i = 0; i < drops.count(); ++i) {
        if (drops[i].entityId == 0 && !drops[i].collecting()) {
            session_.send(net::pickupSpawnFor(drops[i]));
            entities_.predictDrop(drops.at(i));
        }
    }
    drops.removeUnowned();
}

}  // namespace mc::ctr
