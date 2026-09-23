// The game loop's side of a multiplayer session. See net_play.hpp.

#include "platform/ctr/net_play.hpp"

#include "platform/ctr/overlay.hpp"
#include "platform/ctr/renderer.hpp"

namespace mc::ctr {

namespace {

constexpr float kDegreesPerRadian = 180.0f / 3.14159265f;

}  // namespace

void NetPlay::pump(render::WorldStreamer& world, render::ChunkRenderer& chunks, Overlay& overlay,
                   gui::ChatLog& chat, const u8* fontWidths, entity::PlayerBody& body,
                   Camera& camera)
{
    while (!closed_ && session_.poll(&event_)) {
        switch (event_.kind) {
        case net::PacketChannel::Event::Kind::LoggedIn:
            entityId_ = event_.entityId;
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
            body.setFeet(teleport.x, teleport.eyeY - double(entity::kEyeHeight), teleport.z);
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
        const net::RemotePlayer* from = nullptr;
        for (int i = 0; i < entities_.playerCount(); ++i) {
            if (entities_.player(i).id == i32(p.integer(0))) {
                from = &entities_.player(i);
                break;
            }
        }
        if (from == nullptr) {
            // **An arrow the host fired**, named as the attacker -- see
            // `WorldServer::arrowStruck`. Knocked back from where the arrow
            // is, as the host's own player is.
            entity::ArrowSystem* arrows = entities_.arrows();
            const entity::Arrow* arrow =
                arrows != nullptr ? arrows->findById(i32(p.integer(0))) : nullptr;
            if (arrow == nullptr) {
                break;
            }
            hit_.present = true;
            hit_.arrow = true;
            hit_.item = 0;
            hit_.fromX = arrow->x;
            hit_.fromZ = arrow->z;
            break;
        }
        hit_.present = true;
        hit_.arrow = false;
        hit_.item = int(from->heldItem);
        hit_.fromX = from->x;
        hit_.fromZ = from->z;
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

void NetPlay::tick(const entity::PlayerBody& body, const Camera& camera,
                   const item::Inventory& inventory, const tick::TickWorld* world, bool alive)
{
    entities_.tick(world);

    // `la.J()` checks the inventory before it reports the move.
    const int sent = inventory_.tick(inventory, scratch_);
    for (int i = 0; i < sent; ++i) {
        session_.send(scratch_[i]);
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
        if (drops[i].entityId == 0) {
            session_.send(net::pickupSpawnFor(drops[i]));
            entities_.predictDrop(drops.at(i));
        }
    }
    drops.removeUnowned();
}

}  // namespace mc::ctr
