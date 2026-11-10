// The server's entities on the client. See entities.hpp.

#include "core/net/entities.hpp"

#include "core/entity/explosion.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/player_body.hpp"
#include "core/net/player_sync.hpp"
#include "core/render/player_model.hpp"
#include "core/util/math_helper.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

#include <cmath>
#include <cstring>

namespace mc::net {

namespace {

double fromFixed(i64 units)
{
    return double(units) / kPositionUnitsPerBlock;
}

float fromByteAngle(i64 units)
{
    return float(units) * 360.0f / kRotationUnitsPerTurn;
}

}  // namespace

// See the note inside `mobTypeFor`.
constexpr int kOurSheep = 92;
constexpr int kOurCow = 93;

// **And three more of ours, for the slime's size.** `ez` carries a type byte
// and nothing else, and a slime's size is not a type: `ma`'s constructor draws
// `1 << nextInt(3)` and `gy` never overwrites it, so against a real a1.1.2
// server every slime you meet is a size the *client* chose and the two ends
// disagree about how big it is, how much it hurts to stand on and how far it
// hops. That is the jar's, and 55 keeps meaning it.
//
// Between two 3DAlpha consoles it is only a hole. A host knows the size it
// spawned and the byte is free, so it says which of the three a1.1.2 has --
// 1, 2 and 4, because `1 << nextInt(3)` never produces a 3.
constexpr int kOurSlime1 = 94;
constexpr int kOurSlime2 = 95;
constexpr int kOurSlime4 = 96;

void playerColour(i32 entityId, u8* red, u8* green, u8* blue)
{
    struct Rgb {
        u8 r, g, b;
    };
    // Eight, one per reserved id, so the table cannot run out before the ids
    // do and no two players in a session are ever handed the same one.
    //
    // **The first four are the ones that matter** -- a session is a host and
    // three guests -- so those four are chosen to be told apart from each
    // other *and* to be read against grass, stone and sand. Nothing dark and
    // nothing that sinks into the ground it is drawn over: the green that was
    // here first vanished on a hillside.
    static const Rgb kTable[kMaxPlayerEntityIds] = {
        {255, 255, 255},  // the host: plain white, as a single-player nametag is
        {110, 200, 255},  // sky
        {255, 170, 60},   // amber
        {255, 110, 110},  // coral
        {130, 235, 180},  // mint
        {200, 160, 255},  // violet
        {255, 235, 120},  // straw
        {130, 235, 225},  // teal
    };
    const i32 index = entityId - kFirstPlayerEntityId;
    const Rgb& pick =
        index >= 0 && index < kMaxPlayerEntityIds ? kTable[index] : kTable[0];
    *red = pick.r;
    *green = pick.g;
    *blue = pick.b;
}

bool RemoteEntities::mobTypeFor(int wireType, entity::MobType* out, int* slimeSize)
{
    if (slimeSize != nullptr) {
        *slimeSize = 0;
    }
    const auto slime = [&](int size) {
        *out = entity::MobType::Slime;
        if (slimeSize != nullptr) {
            *slimeSize = size;
        }
        return true;
    };

    switch (wireType) {
    case 50: *out = entity::MobType::Creeper; return true;
    case 51: *out = entity::MobType::Skeleton; return true;
    case 52: *out = entity::MobType::Spider; return true;
    case 54: *out = entity::MobType::Zombie; return true;
    // The jar's own slime id, which says nothing about size: 0 leaves the
    // client to draw one, exactly as `ma`'s constructor does.
    case 55: return slime(0);
    case 90: *out = entity::MobType::Pig; return true;
    case 91:
        // Sheep, cow and chicken all register as 91 and the last one wins, in
        // the jar and therefore here.
        *out = entity::MobType::Chicken;
        return true;
    // **92 and 93 are ours and no a1.1.2 server sends them.**
    //
    // The collision above is faithful and, against a real server, unavoidable:
    // the jar registers `bo`, `am` and `mz` all as 91 and the map keeps the
    // last, so a sheep really does arrive as a chicken. Between two 3DAlpha
    // consoles it is not faithfulness, it is a bug -- this port knows what it
    // spawned and has a byte to spare -- so a 3DAlpha host says 92 for a sheep
    // and 93 for a cow, and 91 keeps meaning what the jar says it means.
    case kOurSheep: *out = entity::MobType::Sheep; return true;
    case kOurCow:   *out = entity::MobType::Cow; return true;
    case kOurSlime1: return slime(1);
    case kOurSlime2: return slime(2);
    case kOurSlime4: return slime(entity::kSlimeMaxSize);
    default:
        // 53 is the giant, which this port has no model for, and anything else
        // is a type protocol 2 does not have.
        return false;
    }
}

int RemoteEntities::wireTypeForMob(const entity::Mob& mob)
{
    if (mob.type != entity::MobType::Slime) {
        return wireTypeFor(mob.type);
    }
    switch (mob.slimeSize) {
    case 1:  return kOurSlime1;
    case 2:  return kOurSlime2;
    case entity::kSlimeMaxSize: return kOurSlime4;
    // A size a1.1.2 cannot produce -- nothing here makes one, and a save that
    // held one has already been corrected by `entity::PersistentEntities` --
    // so 55 is the honest answer: "a slime, and you pick".
    default: return 55;
    }
}

int RemoteEntities::wireTypeFor(entity::MobType type)
{
    switch (type) {
    case entity::MobType::Creeper:  return 50;
    case entity::MobType::Skeleton: return 51;
    case entity::MobType::Spider:   return 52;
    case entity::MobType::Zombie:   return 54;
    case entity::MobType::Slime:    return 55;
    case entity::MobType::Pig:      return 90;
    case entity::MobType::Chicken:  return 91;
    case entity::MobType::Sheep:    return kOurSheep;
    case entity::MobType::Cow:      return kOurCow;
    }
    return 91;
}

RemotePlayer* RemoteEntities::findPlayer(i32 id)
{
    for (int i = 0; i < playerCount_; ++i) {
        if (players_[i].used && players_[i].id == id) {
            return &players_[i];
        }
    }
    return nullptr;
}

RemotePlayer* RemoteEntities::addPlayer(i32 id)
{
    if (RemotePlayer* existing = findPlayer(id)) {
        return existing;
    }
    if (playerCount_ >= kMaxPlayers) {
        return nullptr;
    }
    RemotePlayer& player = players_[playerCount_++];
    player = RemotePlayer{};
    player.id = id;
    player.used = true;
    return &player;
}

void RemoteEntities::removePlayer(i32 id)
{
    for (int i = 0; i < playerCount_; ++i) {
        if (!players_[i].used || players_[i].id != id) {
            continue;
        }
        // The last one takes its place, the way every other pool here removes.
        players_[i] = players_[playerCount_ - 1];
        players_[playerCount_ - 1] = RemotePlayer{};
        --playerCount_;
        return;
    }
}

void RemoteEntities::moveTo(i32 id, double x, double y, double z, bool hasLook, float yaw,
                            float pitch, bool relative)
{
    if (RemotePlayer* player = findPlayer(id)) {
        // **The server's position moves; the body follows it.** A relative move
        // is against where the server last said, not against where the body has
        // got to, or a body still catching up would fall further behind with
        // every packet.
        entity::ServerTrack& track = player->track;
        track.receive(relative ? track.x + x : x, relative ? track.y + y : y,
                      relative ? track.z + z : z);
        if (hasLook) {
            player->targetYaw = yaw;
            player->targetPitch = pitch;
            player->smoothTicks = kSmoothTicks;
        }
        return;
    }

    if (entity::Mob* mob = mobs_ != nullptr ? mobs_->findById(id) : nullptr) {
        // **A mob is smoothed exactly as a player is**, because in the jar it
        // is the same method: `gy` calls `setPositionAndRotation2` with three
        // increments on `kh`, and `ge` -- every animal and every monster --
        // overrides it to store the target and walk to it in `ge.j()`. See
        // `MobSystem::interpolateToServer`.
        //
        // The relative move is against the server's position and not against
        // where the body has got to, for the same reason it is for a player.
        // `body.y` is the feet -- a mob's `yOffset` is zero.
        const double nx = relative ? mob->track.x + x : x;
        const double ny = relative ? mob->track.y + y : y;
        const double nz = relative ? mob->track.z + z : z;
        mobs_->placeById(id, nx, ny, nz, hasLook, yaw, pitch);
        return;
    }

    // **An arrow walks nowhere**: it goes where it is told, one tick later,
    // and the renderer draws the step between. Relative moves are never sent
    // for one.
    if (arrows_ != nullptr && !relative && hasLook
        && arrows_->placeRemote(id, x, y, z, yaw, pitch)) {
        return;
    }

    if (items_ != nullptr) {
        if (entity::ItemEntity* item = items_->findById(id)) {
            // An item runs its own fall here, as `EntityItem` on a client does,
            // so it is corrected rather than walked -- see `placeById`. A
            // relative move is against the server's position, not the local one.
            const double nx = relative ? item->serverX + x : x;
            const double ny = relative ? item->serverY + y : y;
            const double nz = relative ? item->serverZ + z : z;
            items_->placeById(id, nx, ny, nz);
            return;
        }
    }

    // **The vehicles and the fallers walk to the server's word**, a relative
    // move being against the server's position -- `track`, which is the last
    // thing it said -- for the reason it is for a player. A vehicle with no
    // look in the packet keeps the heading it was given.
    if (boats_ != nullptr) {
        const int index = boats_->indexOfEntity(id);
        if (index >= 0) {
            const entity::ServerTrack& t = (*boats_)[index].track;
            boats_->placeRemote(id, relative ? t.x + x : x, relative ? t.y + y : y,
                                relative ? t.z + z : z, hasLook ? yaw : t.yaw);
            return;
        }
    }
    if (minecarts_ != nullptr) {
        const int index = minecarts_->indexOfEntity(id);
        if (index >= 0) {
            const entity::ServerTrack& t = (*minecarts_)[index].track;
            minecarts_->placeRemote(id, relative ? t.x + x : x, relative ? t.y + y : y,
                                    relative ? t.z + z : z, hasLook ? yaw : t.yaw);
            return;
        }
    }
    if (falling_ != nullptr) {
        const int index = falling_->indexOfEntity(id);
        if (index >= 0) {
            const entity::ServerTrack& t = (*falling_)[index].track;
            falling_->placeRemote(id, relative ? t.x + x : x, relative ? t.y + y : y,
                                  relative ? t.z + z : z);
            return;
        }
    }
    if (tnt_ != nullptr) {
        const int index = tnt_->indexOfEntity(id);
        if (index >= 0) {
            const entity::ServerTrack& t = (*tnt_)[index].track;
            tnt_->placeRemote(id, relative ? t.x + x : x, relative ? t.y + y : y,
                              relative ? t.z + z : z);
        }
    }
}

int RemoteEntities::objectTypeForFalling(block::BlockId id)
{
    switch (id) {
    case block::BlockId(mcver::Block::Sand):   return kObjectFallingSand;
    case block::BlockId(mcver::Block::Gravel): return kObjectFallingGravel;
    default:                                   return 0;
    }
}

bool RemoteEntities::spawnVehicle(const Packet& packet, tick::TickWorld* world)
{
    if (world == nullptr) {
        return false;
    }
    const i32 id = i32(packet.integer(0));
    const int type = int(packet.integer(1));
    const double x = fromFixed(packet.integer(2));
    const double y = fromFixed(packet.integer(3));
    const double z = fromFixed(packet.integer(4));
    switch (type) {
    case kVehicleBoat:
        return boats_ != nullptr && boats_->spawnRemote(*world, id, x, y, z);
    case kVehicleMinecart + int(entity::MinecartType::Rideable):
    case kVehicleMinecart + int(entity::MinecartType::Chest):
    case kVehicleMinecart + int(entity::MinecartType::Furnace):
        return minecarts_ != nullptr
               && minecarts_->spawnRemote(*world, id,
                                          entity::MinecartType(type - kVehicleMinecart), x, y,
                                          z);
    case kObjectPrimedTnt:
        return tnt_ != nullptr && tnt_->spawnRemote(*world, id, x, y, z);
    case kObjectFallingSand:
        return falling_ != nullptr
               && falling_->spawnRemote(*world, id, x, y, z,
                                        block::BlockId(mcver::Block::Sand));
    case kObjectFallingGravel:
        return falling_ != nullptr
               && falling_->spawnRemote(*world, id, x, y, z,
                                        block::BlockId(mcver::Block::Gravel));
    case kObjectArrow:
        // The heading follows in a teleport, as the host sends it.
        return arrows_ != nullptr && arrows_->spawnRemote(*world, id, x, y, z);
    default:
        return false;
    }
}

void RemoteEntities::seat(i32 rider, i32 vehicle)
{
    // **Out of whatever they were in first**, so a rider is never in two.
    for (int i = 0; boats_ != nullptr && i < boats_->count(); ++i) {
        const entity::Boat& b = (*boats_)[i];
        if (b.riderId == rider && b.entityId != vehicle) {
            boats_->setRider(b.entityId, 0);
        }
    }
    for (int i = 0; minecarts_ != nullptr && i < minecarts_->count(); ++i) {
        const entity::Minecart& c = (*minecarts_)[i];
        if (c.riderId == rider && c.entityId != vehicle) {
            minecarts_->setRider(c.entityId, 0);
        }
    }
    if (vehicle == kNoVehicle) {
        return;
    }
    if (boats_ != nullptr && boats_->setRider(vehicle, rider)) {
        return;
    }
    if (minecarts_ != nullptr) {
        minecarts_->setRider(vehicle, rider);
    }
}

void RemoteEntities::takeWheel(i32 vehicle)
{
    if (driving_.id == vehicle) {
        return;  // already steering it
    }
    letGo();
    if (boats_ != nullptr && boats_->driveHere(vehicle)) {
        driving_ = Driving{};
        driving_.kind = Driving::Kind::Boat;
        driving_.id = vehicle;
        return;
    }
    if (minecarts_ != nullptr && minecarts_->driveHere(vehicle)) {
        driving_ = Driving{};
        driving_.kind = Driving::Kind::Minecart;
        driving_.id = vehicle;
        return;
    }
    owesRelease_ = true;
}

void RemoteEntities::letGo()
{
    if (driving_.kind == Driving::Kind::Boat && boats_ != nullptr) {
        boats_->driveElsewhere(driving_.id, 0);
    } else if (driving_.kind == Driving::Kind::Minecart && minecarts_ != nullptr) {
        minecarts_->driveElsewhere(driving_.id, 0);
    }
    driving_ = Driving{};
}

void RemoteEntities::blastAt(tick::TickWorld& world, double x, double y, double z,
                             float strength, bool monster)
{
    // Phase one -- `random.explode` and the rays -- then phase two against this
    // player, while this console's blocks still stand (the host's Block
    // Changes come after the word that it went off), then phase three's
    // particles without its writes. See core/entity/explosion.hpp.
    entity::Explosion blast(x, y, z, strength);
    blast.cast(world);
    if (hasLocalFeet_) {
        const double half = double(entity::kPlayerWidth) / 2.0;
        const AABB box{localFeet_[0] - half, localFeet_[1], localFeet_[2] - half,
                       localFeet_[0] + half, localFeet_[1] + double(entity::kPlayerHeight),
                       localFeet_[2] + half};
        int damage = 0;
        double vx = 0.0, vy = 0.0, vz = 0.0;
        // `posY` is the eye, as `applyBlast` hands it over for the host's own
        // player.
        if (blast.effectOn(world, localFeet_[0], localFeet_[1] + double(entity::kEyeHeight),
                           localFeet_[2], box, &damage, &vx, &vy, &vz)) {
            if (!blast_.present || damage > blast_.damage) {
                blast_.damage = damage;
                blast_.monster = monster;
                blast_.fromX = x;
                blast_.fromZ = z;
            }
            blast_.present = true;
            blast_.vx += vx;
            blast_.vy += vy;
            blast_.vz += vz;
        }
    }
    blast.puff(world);
}

bool RemoteEntities::takeBlast(IncomingBlast* out)
{
    if (!blast_.present) {
        return false;
    }
    *out = blast_;
    blast_ = IncomingBlast{};
    return true;
}

bool RemoteEntities::hitFrom(i32 attackerId, IncomingHit* out)
{
    *out = IncomingHit{};
    for (int i = 0; i < playerCount_; ++i) {
        const RemotePlayer& from = players_[i];
        if (from.id != attackerId) {
            continue;
        }
        out->present = true;
        out->item = int(from.heldItem);
        out->fromX = from.x;
        out->fromZ = from.z;
        return true;
    }
    // Knocked back from where the arrow is, as the host's own player is.
    const entity::Arrow* arrow = arrows_ != nullptr ? arrows_->findById(attackerId) : nullptr;
    if (arrow != nullptr) {
        out->present = true;
        out->arrow = true;
        out->fromX = arrow->x;
        out->fromZ = arrow->z;
        return true;
    }
    // **A mob's blow is priced by the mob**, the same numbers the host's own
    // player is hit with: `attackStrength` through `dq` for a swing, the size
    // and no difficulty for a slime landing on them (mob.cpp's `ma.b(Ldm;)V`).
    const entity::Mob* mob = mobs_ != nullptr ? mobs_->findById(attackerId) : nullptr;
    if (mob != nullptr) {
        const entity::MobDef& def = entity::mobDef(mob->type);
        const bool slime = def.ai == entity::MobAi::Hop;
        out->mobDamage = slime ? int(mob->slimeSize) : def.attackStrength;
        out->monster = !slime;
        out->fromX = mob->body.x;
        out->fromZ = mob->body.z;
        out->present = out->mobDamage > 0;
        return out->present;
    }
    return false;
}

int RemoteEntities::driveReport(Packet* out)
{
    if (owesRelease_ && driving_.id == 0) {
        owesRelease_ = false;
        *out = makeUseEntity(self_, kNoVehicle, false);
        return 1;
    }
    owesRelease_ = false;
    if (driving_.id == 0) {
        return 0;
    }
    const bool boat = driving_.kind == Driving::Kind::Boat;
    int index = -1;
    bool aboard = false;
    double x = 0.0, y = 0.0, z = 0.0;
    float yaw = 0.0f;
    if (boat && boats_ != nullptr) {
        index = boats_->indexOfEntity(driving_.id);
        if (index >= 0) {
            const entity::Boat& b = (*boats_)[index];
            aboard = boats_->riddenIndex() == index;
            x = b.x;
            y = b.y;
            z = b.z;
            yaw = b.yaw;
        }
    } else if (!boat && minecarts_ != nullptr) {
        index = minecarts_->indexOfEntity(driving_.id);
        if (index >= 0) {
            const entity::Minecart& c = (*minecarts_)[index];
            aboard = minecarts_->riddenIndex() == index;
            x = c.x;
            y = c.y;
            z = c.z;
            yaw = c.yaw;
        }
    }

    if (index < 0) {
        // Broken here: on a wall, a cactus or in a fire.
        out->reset(packet::DestroyEntity);
        out->pushInt(driving_.id);
        driving_ = Driving{};
        return 1;
    }
    if (!aboard) {
        // The sneak button: the player is on the roof and the vehicle is the
        // host's to run again.
        *out = makeUseEntity(self_, kNoVehicle, false);
        letGo();
        return 1;
    }

    const i32 wx = toWirePosition(x);
    const i32 wy = toWirePosition(y);
    const i32 wz = toWirePosition(z);
    const i8 wyaw = toWireAngle(yaw);
    if (driving_.sent && wx == driving_.x && wy == driving_.y && wz == driving_.z
        && wyaw == driving_.yaw) {
        return 0;
    }
    driving_.sent = true;
    driving_.x = wx;
    driving_.y = wy;
    driving_.z = wz;
    driving_.yaw = wyaw;
    out->reset(packet::EntityTeleport);
    out->pushInt(driving_.id);
    out->pushInt(wx);
    out->pushInt(wy);
    out->pushInt(wz);
    out->pushInt(wyaw);
    out->pushInt(0);
    return 1;
}

bool RemoteEntities::apply(const Packet& packet, tick::TickWorld* world)
{
    switch (packet.id) {
    case packet::NamedEntitySpawn: {
        // `gp`: id, name, x, y, z, yaw, pitch, currentItem. **A spawn replaces**
        // whatever was here under that id rather than updating it, as both the
        // other pools do: a player back from the dead is sent one, and must not
        // come back still crouched or still falling.
        removePlayer(i32(packet.integer(0)));
        RemotePlayer* player = addPlayer(i32(packet.integer(0)));
        if (player == nullptr) {
            ++unhandledSpawns_;  // more players in view than this pool holds
            return true;
        }
        const std::string& name = packet.text(0);
        const usize copied = name.size() < RemotePlayer::kMaxNameBytes - 1
                                 ? name.size()
                                 : RemotePlayer::kMaxNameBytes - 1;
        std::memcpy(player->name, name.data(), copied);
        player->name[copied] = '\0';

        player->x = fromFixed(packet.integer(1));
        player->y = fromFixed(packet.integer(2));
        player->z = fromFixed(packet.integer(3));
        player->prevX = player->x;
        player->prevY = player->y;
        player->prevZ = player->z;
        player->track.place(player->x, player->y, player->z);
        player->yaw = fromByteAngle(packet.integer(4));
        player->pitch = fromByteAngle(packet.integer(5));
        player->prevYaw = player->yaw;
        player->renderYaw = player->prevRenderYaw = player->yaw;
        player->prevPitch = player->pitch;
        player->targetYaw = player->yaw;
        player->targetPitch = player->pitch;
        player->heldItem = i16(packet.integer(6));
        player->smoothTicks = 0;
        return true;
    }

    case packet::PickupSpawn: {
        // `ha`: id, item, count, x, y, z, and three bytes of velocity.
        if (items_ == nullptr || world == nullptr) {
            return true;
        }
        if (adoptDrop(i32(packet.integer(0)), item::ItemId(packet.integer(1)),
                      int(packet.integer(2)), fromFixed(packet.integer(3)),
                      fromFixed(packet.integer(4)), fromFixed(packet.integer(5)))) {
            return true;
        }
        items_->spawnFromServer(*world, i32(packet.integer(0)),
                                fromFixed(packet.integer(3)), fromFixed(packet.integer(4)),
                                fromFixed(packet.integer(5)),
                                item::ItemId(packet.integer(1)), int(packet.integer(2)),
                                0, double(packet.integer(6)) / 128.0,
                                double(packet.integer(7)) / 128.0,
                                double(packet.integer(8)) / 128.0);
        return true;
    }

    case packet::Collect:
        // `bm`: the collected entity is gone, and whoever collected it is told
        // separately with Add To Inventory. a1.1.2 animates the item flying to
        // the collector, so it does when it knows where the collector is and
        // simply goes when it does not.
        if (items_ != nullptr) {
            const RemotePlayer* collector = findPlayer(i32(packet.integer(1)));
            if (collector != nullptr) {
                items_->collectById(i32(packet.integer(0)), collector->x, collector->y + 0.9,
                                    collector->z);
            } else if (hasLocalFeet_) {
                items_->collectById(i32(packet.integer(0)), localFeet_[0], localFeet_[1] + 0.9,
                                    localFeet_[2]);
            } else {
                items_->removeById(i32(packet.integer(0)));
            }
        }
        // A stuck arrow its shooter walked over goes the same way.
        if (arrows_ != nullptr) {
            arrows_->removeById(i32(packet.integer(0)));
        }
        if (arrows_ != nullptr) {
            arrows_->removeById(i32(packet.integer(0)));
        }
        removePlayer(i32(packet.integer(0)));
        return true;

    case packet::PlayerSkin: {
        // Only for somebody this pool is tracking: a skin about an entity that
        // is not a player here has nothing to be worn by.
        // **On the heap**: a page is 8 KB and a frame here may not be.
        i32 id = 0;
        std::vector<u8> page(kSkinPageBytes);
        if (readPlayerSkin(packet, &id, page.data()) && findPlayer(id) != nullptr) {
            skins_.store(id, page.data());
        }
        return true;
    }

    case packet::DestroyEntity:
        skins_.release(i32(packet.integer(0)));
        removePlayer(i32(packet.integer(0)));
        if (paintings_ != nullptr) {
            paintings_->removeById(i32(packet.integer(0)));
        }
        if (items_ != nullptr) {
            items_->removeById(i32(packet.integer(0)));
        }
        if (mobs_ != nullptr) {
            mobs_->removeById(i32(packet.integer(0)));
        }
        if (paintings_ != nullptr) {
            paintings_->removeById(i32(packet.integer(0)));
        }
        // **A vehicle this console is steering can go too** -- the host broke
        // it -- and then the player is thrown clear the way a wall throws
        // them: the seat is gone from under them.
        if (i32(packet.integer(0)) == driving_.id) {
            driving_ = Driving{};
        }
        if (boats_ != nullptr) {
            boats_->removeById(i32(packet.integer(0)));
        }
        if (minecarts_ != nullptr) {
            minecarts_->removeById(i32(packet.integer(0)));
        }
        if (falling_ != nullptr) {
            falling_->removeById(i32(packet.integer(0)));
        }
        if (arrows_ != nullptr) {
            arrows_->removeById(i32(packet.integer(0)));
        }
        // **Lit TNT leaves the host's pool one way only**, so its Destroy
        // Entity is the bang. See `PrimedTntSystem::takeRemote`.
        if (tnt_ != nullptr) {
            double bx = 0.0, by = 0.0, bz = 0.0;
            if (tnt_->takeRemote(i32(packet.integer(0)), &bx, &by, &bz) && world != nullptr) {
                blastAt(*world, bx, by, bz, entity::kPrimedTntStrength, false);
            }
        }
        return true;

    case packet::Painting:
        // Ours; see `packet::Painting`. A title this build has no art for is
        // counted and dropped rather than drawn wrong.
        if (paintings_ != nullptr) {
            if (!paintings_->spawnRemote(world, i32(packet.integer(0)), packet.text(0).c_str(),
                                         i32(packet.integer(1)), int(packet.integer(2)),
                                         i32(packet.integer(3)), int(packet.integer(4)))) {
                ++unhandledSpawns_;
            }
        } else {
            ++unhandledSpawns_;
        }
        return true;

    case packet::EntityStatus:
        // Ours; see `packet::EntityStatus`. **A player answers only a death**:
        // `handleHealthUpdate(3)`'s death noise -- `random.hurt`, which `dm`
        // uses for both -- and the fall that `tick` counts. A second 3 for a
        // body already falling is not a second death.
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            if (int(packet.integer(1)) == entity::kStatusHurt) {
                player->hurtTime = kHurtTicks;
            }
            if (int(packet.integer(1)) == entity::kStatusDead && !player->dead) {
                player->dead = true;
                player->sneaking = false;
                const float pitch = (rand_.nextFloat() - rand_.nextFloat()) * 0.2f + 1.0f;
                if (world != nullptr) {
                    world->playSoundAt("random.hurt", player->x, player->y, player->z, 1.0f,
                                       pitch);
                }
            }
            return true;
        }
        if (mobs_ != nullptr && world != nullptr) {
            mobs_->statusFromServer(*world, i32(packet.integer(0)), int(packet.integer(1)));
        }
        // **A struck boat or cart rocks**, which a1.1.2 never shows anybody but
        // the one who hit it.
        if (int(packet.integer(1)) == entity::kStatusHurt) {
            if (boats_ != nullptr) {
                boats_->hurtRemote(i32(packet.integer(0)));
            }
            if (minecarts_ != nullptr) {
                minecarts_->hurtRemote(i32(packet.integer(0)));
            }
        }
        // **A furnace cart's chimney**, which the flag pair carries for a cart
        // as it carries a fleece for a sheep.
        if (minecarts_ != nullptr
            && (int(packet.integer(1)) == entity::kStatusFlagSet
                || int(packet.integer(1)) == entity::kStatusFlagClear)) {
            minecarts_->setRemoteBurning(i32(packet.integer(0)),
                                         int(packet.integer(1)) == entity::kStatusFlagSet);
        }
        // **A creeper went off**: the bang, the smoke, and the blow if this
        // player was close -- from where the host last put it.
        if (int(packet.integer(1)) == entity::kStatusExploded && mobs_ != nullptr
            && world != nullptr) {
            if (const entity::Mob* mob = mobs_->findById(i32(packet.integer(0)))) {
                blastAt(*world, mob->track.x, mob->track.y, mob->track.z, entity::kCreeperBlast,
                        true);
            }
        }
        return true;

    case packet::UseEntity: {
        // **A blow is `NetPlay`'s** -- the player's health is the player's.
        // A right click from the host is who rides what; see `kNoVehicle`.
        if (packet.integer(2) != 0) {
            return false;
        }
        const i32 rider = i32(packet.integer(0));
        const i32 vehicle = i32(packet.integer(1));
        hostRides_ = true;
        if (RemotePlayer* player = findPlayer(rider)) {
            player->riding = vehicle != kNoVehicle;
        }
        if (self_ != 0 && rider == self_) {
            if (vehicle == kNoVehicle) {
                // Only ever a host taking the wheel back; a player who got out
                // has let go already.
                letGo();
            } else {
                takeWheel(vehicle);
            }
        } else {
            seat(rider, vehicle);
        }
        return true;
    }

    case packet::Entity:
        // `lq`: a keep-alive for one entity, which says only that it still
        // exists. Nothing to do, but it is ours rather than unhandled.
        return true;

    case packet::RelEntityMove:
        moveTo(i32(packet.integer(0)), fromFixed(packet.integer(1)), fromFixed(packet.integer(2)),
               fromFixed(packet.integer(3)), false, 0.0f, 0.0f, true);
        return true;

    case packet::RelEntityMoveLook:
        moveTo(i32(packet.integer(0)), fromFixed(packet.integer(1)), fromFixed(packet.integer(2)),
               fromFixed(packet.integer(3)), true, fromByteAngle(packet.integer(4)),
               fromByteAngle(packet.integer(5)), true);
        return true;

    case packet::EntityTeleport:
        // Lit TNT has no heading, so the host's fuse rides in the yaw byte --
        // see `PrimedTnt::remote`.
        if (tnt_ != nullptr) {
            tnt_->setRemoteFuse(i32(packet.integer(0)), int(packet.integer(4)));
        }
        moveTo(i32(packet.integer(0)), fromFixed(packet.integer(1)), fromFixed(packet.integer(2)),
               fromFixed(packet.integer(3)), true, fromByteAngle(packet.integer(4)),
               fromByteAngle(packet.integer(5)), false);
        return true;

    case packet::EntityLook: {
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            player->targetYaw = fromByteAngle(packet.integer(1));
            player->targetPitch = fromByteAngle(packet.integer(2));
            player->smoothTicks = kSmoothTicks;
            return true;
        }
        if (mobs_ != nullptr) {
            mobs_->turnById(i32(packet.integer(0)), fromByteAngle(packet.integer(1)),
                            fromByteAngle(packet.integer(2)));
        }
        // A Java server turns a cart on the spot with this; it stays where the
        // server last put it.
        const i32 id = i32(packet.integer(0));
        const float yaw = fromByteAngle(packet.integer(1));
        if (boats_ != nullptr) {
            const int index = boats_->indexOfEntity(id);
            if (index >= 0) {
                const entity::ServerTrack& t = (*boats_)[index].track;
                boats_->placeRemote(id, t.x, t.y, t.z, yaw);
            }
        }
        if (minecarts_ != nullptr) {
            const int index = minecarts_->indexOfEntity(id);
            if (index >= 0) {
                const entity::ServerTrack& t = (*minecarts_)[index].track;
                minecarts_->placeRemote(id, t.x, t.y, t.z, yaw);
            }
        }
        return true;
    }

    case packet::BlockItemSwitch:
        // `dz` towards a client is another player changing what they hold.
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            player->heldItem = i16(packet.integer(1));
        }
        return true;

    case packet::PlayerInventory:
        // Ours when the type names somebody -- see `isWornReport`. The real
        // types are this console's own pack, which is not an entity's business.
        if (!isWornReport(packet)) {
            return false;
        }
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            // The wire is boots first and `armour` is helmet first.
            for (int slot = 0; slot < kWireArmourSlots; ++slot) {
                const usize wire = usize(kWireArmourSlots - 1 - slot);
                const int id = wire < packet.stacks.size() ? int(packet.stacks[wire].id) : -1;
                player->armour[slot] = id < 0 ? -1 : render::armourMaterialOf(id);
            }
        }
        return true;

    case packet::EntityAction:
        // Ours; see `packet::EntityAction`. `cr.j` for whoever it names. A body
        // that is falling over does not crouch on the way down.
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            const int action = int(packet.integer(1));
            if (!player->dead && action == kActionCrouch) {
                player->sneaking = true;
            } else if (action == kActionUncrouch) {
                player->sneaking = false;
            }
        }
        return true;

    case packet::ArmAnimation:
        // `gy.a(hf)`: whoever it names swings, whatever the second field says
        // -- a1.1.2 does not look at it. `swingProgressInt = -1`, so the first
        // tick lands on zero.
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            player->swingTicks = -1;
            player->swinging = true;
        }
        return true;

    case packet::MobSpawn: {
        // `ez`: id, type, x, y, z, yaw, pitch.
        entity::MobType type = entity::MobType::Pig;
        int slimeSize = 0;
        if (mobs_ == nullptr || world == nullptr
            || !mobTypeFor(int(packet.integer(1)), &type, &slimeSize)) {
            ++unhandledSpawns_;
            return true;
        }
        mobs_->spawnFromServer(*world, i32(packet.integer(0)), type,
                               fromFixed(packet.integer(2)), fromFixed(packet.integer(3)),
                               fromFixed(packet.integer(4)), fromByteAngle(packet.integer(5)),
                               fromByteAngle(packet.integer(6)), slimeSize);
        return true;
    }

    case packet::VehicleSpawn:
        // `kj`: id, type, x, y, z -- and no angles: a cart's tilt is the rail
        // under it on whichever console draws it, and the heading follows in
        // the first move. See `kVehicleBoat`.
        if (!spawnVehicle(packet, world)) {
            ++unhandledSpawns_;
        }
        return true;

    default:
        return false;
    }
}

bool RemoteEntities::predictDrop(entity::ItemEntity* item)
{
    if (item == nullptr || item->entityId != 0 || dropCount_ >= kMaxPredictedDrops) {
        return false;
    }
    PredictedDrop& drop = drops_[dropCount_++];
    drop.provisionalId = nextProvisionalId_;
    drop.item = item->item;
    drop.count = item->count;
    drop.x = item->x;
    drop.y = item->y;
    drop.z = item->z;
    drop.ticks = 0;
    // Wraps long before it could reach a real id, and sixteen outstanding
    // throws cannot collide across a wrap that long.
    nextProvisionalId_ = nextProvisionalId_ <= -0x40000000 ? -1 : nextProvisionalId_ - 1;

    item->entityId = drop.provisionalId;
    // The server decides who picks it up, this one included.
    item->pickupDelay = 0x7FFF;
    return true;
}

bool RemoteEntities::adoptDrop(i32 serverId, item::ItemId id, int count, double x, double y,
                               double z)
{
    if (items_ == nullptr || dropCount_ == 0 || items_->findById(serverId) != nullptr) {
        return false;
    }
    constexpr double kMatchSquared = kDropMatchDistance * kDropMatchDistance;
    for (int i = 0; i < dropCount_; ++i) {
        const PredictedDrop& drop = drops_[i];
        const double dx = x - drop.x;
        const double dy = y - drop.y;
        const double dz = z - drop.z;
        if (drop.item != id || drop.count != count
            || dx * dx + dy * dy + dz * dz > kMatchSquared) {
            continue;
        }
        entity::ItemEntity* item = items_->findById(drop.provisionalId);
        for (int j = i + 1; j < dropCount_; ++j) {
            drops_[j - 1] = drops_[j];
        }
        --dropCount_;
        if (item == nullptr) {
            // Gone here already -- burnt, or blown up. The server's copy is
            // the truth, so it is spawned the ordinary way.
            return false;
        }
        item->entityId = serverId;
        item->serverX = x;
        item->serverY = y;
        item->serverZ = z;
        // The packet has nowhere to put a damage, so the server's stack has none.
        item->damage = 0;
        return true;
    }
    return false;
}

void RemoteEntities::tickDrops()
{
    int kept = 0;
    for (int i = 0; i < dropCount_; ++i) {
        PredictedDrop& drop = drops_[i];
        if (++drop.ticks < kDropConfirmTicks) {
            drops_[kept++] = drop;
            continue;
        }
        // **The prediction was wrong**: the server never made this stack.
        if (items_ != nullptr) {
            items_->removeById(drop.provisionalId);
        }
        ++revertedDrops_;
    }
    dropCount_ = kept;
}

void RemoteEntities::tick(const tick::TickWorld* world)
{
    tickDrops();

    for (int i = 0; i < playerCount_; ++i) {
        RemotePlayer& player = players_[i];
        if (!player.used) {
            continue;
        }

        player.prevX = player.x;
        player.prevY = player.y;
        player.prevZ = player.z;
        player.prevYaw = player.yaw;
        player.prevRenderYaw = player.renderYaw;
        player.prevPitch = player.pitch;
        player.prevLimbAmount = player.limbAmount;
        ++player.ticksExisted;
        if (player.hurtTime > 0) {
            --player.hurtTime;
        }

        // Where the body goes: the server's word, carried on through a late
        // packet and taken back when the silence says it stopped. A teleport
        // is not walked, so nothing drawn between two frames crosses it.
        if (player.track.step(&player.x, &player.y, &player.z)) {
            player.prevX = player.x;
            player.prevY = player.y;
            player.prevZ = player.z;
        }

        if (player.smoothTicks > 0) {
            // `onUpdate`'s turn: a third of what is left, then half of that,
            // then the rest -- over the three ticks a look is given.
            const double steps = double(player.smoothTicks);

            // The short way round, so a body turning past north does not spin
            // the long way -- `MathHelper.wrapAngleTo180`.
            float turn = player.targetYaw - player.yaw;
            while (turn < -180.0f) turn += 360.0f;
            while (turn >= 180.0f) turn -= 360.0f;
            player.yaw += turn / float(steps);
            player.pitch += (player.targetPitch - player.pitch) / float(steps);
            --player.smoothTicks;
        }

        // **The walk comes from the distance covered**, because nothing on the
        // wire says whether another player is walking. `EntityOtherPlayerMP`
        // measures the same thing between ticks.
        const double dx = player.x - player.prevX;
        const double dz = player.z - player.prevZ;
        float speed = float(std::sqrt(dx * dx + dz * dz)) * 4.0f;
        if (speed > 1.0f) {
            speed = 1.0f;
        }
        player.limbAmount += (speed - player.limbAmount) * 0.4f;
        player.limbSwing += player.limbAmount;
        player.turnBody(dx, dz);

        // `dm.b_`'s swing counter, exactly as the local hand runs it.
        player.prevSwing = player.swing;
        if (player.swinging) {
            ++player.swingTicks;
            if (player.swingTicks == 8) {
                player.swingTicks = 0;
                player.swinging = false;
            }
        } else {
            player.swingTicks = 0;
        }
        player.swing = float(player.swingTicks) / 8.0f;

        // **`ge.y()`'s death count**: past twenty ticks, the puff and
        // `setEntityDead`. The last player moves into this slot, so the slot is
        // looked at again.
        if (player.dead) {
            ++player.deathTime;
            if (player.deathTime > kDeathTicks) {
                if (world != nullptr) {
                    puff(player, *world);
                }
                removePlayer(player.id);
                --i;
                continue;
            }
        }

        if (world != nullptr) {
            const i32 bx = MathHelper::floorDouble(player.x);
            // **Mid-body, not the feet**: the feet are the wire's position floored
            // to 1/32, so a body landing or jumping can sit a hair under the
            // floor's top face and sample the solid block there -- light zero,
            // a black model for a few frames. `Entity.getBrightness` samples at
            // half the height for the same reason.
            const int by = int(MathHelper::floorDouble(
                player.y + double(entity::kPlayerHeight) * 0.5));
            const i32 bz = MathHelper::floorDouble(player.z);
            player.light = u8((world->skyLightAt(bx, by, bz) << 4)
                              | world->blockLightAt(bx, by, bz));
        }
    }
}

void RemoteEntities::puff(const RemotePlayer& player, const tick::TickWorld& world)
{
    // `MobSystem::explosionPuff`, around a player's box: the three Gaussians
    // first, then the place, in the order the jar draws them.
    constexpr double kWidth = double(entity::kPlayerWidth);
    constexpr double kHeight = double(entity::kPlayerHeight);
    for (int n = 0; n < entity::kExplosionPuffs; ++n) {
        const double driftX = rand_.nextGaussian() * entity::kExplosionPuffDrift;
        const double driftY = rand_.nextGaussian() * entity::kExplosionPuffDrift;
        const double driftZ = rand_.nextGaussian() * entity::kExplosionPuffDrift;
        const double px = player.x + double(rand_.nextFloat()) * kWidth * 2.0 - kWidth
                          - driftX * entity::kExplosionPuffThrowBack;
        const double py = player.y + double(rand_.nextFloat()) * kHeight
                          - driftY * entity::kExplosionPuffThrowBack;
        const double pz = player.z + double(rand_.nextFloat()) * kWidth * 2.0 - kWidth
                          - driftZ * entity::kExplosionPuffThrowBack;
        world.spawnParticle(int(entity::ParticleKind::Explode), px, py, pz, driftX, driftY,
                            driftZ);
    }
}

void RemoteEntities::clear()
{
    for (RemotePlayer& player : players_) {
        player = RemotePlayer{};
    }
    skins_.clear();
    playerCount_ = 0;
    unhandledSpawns_ = 0;
    dropCount_ = 0;
    revertedDrops_ = 0;
    driving_ = Driving{};
    hostRides_ = false;
    owesRelease_ = false;
    blast_ = IncomingBlast{};
}

void RemotePlayer::turnBody(double movedX, double movedZ)
{
    // `ge.b_`'s heading block, as `MobSystem::headingAndLight` runs it for an
    // animal, plus the line only a player reaches: a swinging arm squares the
    // body up to the look (`if (swingProgress > 0) f1 = rotationYaw`).
    const auto wrap = [](float degrees) {
        while (degrees < -180.0f) degrees += 360.0f;
        while (degrees >= 180.0f) degrees -= 360.0f;
        return degrees;
    };
    float heading = renderYaw;
    if (std::sqrt(movedX * movedX + movedZ * movedZ) > 0.05) {
        heading = float(std::atan2(movedZ, movedX) * 180.0 / 3.14159265358979323846) - 90.0f;
    }
    if (swing > 0.0f) {
        heading = yaw;
    }
    renderYaw += wrap(heading - renderYaw) * 0.3f;
    float headTurn = wrap(yaw - renderYaw);
    headTurn = headTurn < -75.0f ? -75.0f : (headTurn > 75.0f ? 75.0f : headTurn);
    renderYaw = yaw - headTurn;
    if (headTurn * headTurn > 2500.0f) {
        renderYaw += headTurn * 0.2f;
    }
}

}  // namespace mc::net
