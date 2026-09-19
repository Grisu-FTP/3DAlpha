// The server's entities on the client. See entities.hpp.

#include "core/net/entities.hpp"

#include "core/entity/particle.hpp"
#include "core/util/math_helper.hpp"

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
        // **The target moves; the body walks to it.** A relative move is against
        // where the server last said, not against where the body has got to, or
        // a body still catching up would fall further behind with every packet.
        player->targetX = relative ? player->targetX + x : x;
        player->targetY = relative ? player->targetY + y : y;
        player->targetZ = relative ? player->targetZ + z : z;
        if (hasLook) {
            player->targetYaw = yaw;
            player->targetPitch = pitch;
        }
        player->smoothTicks = kSmoothTicks;
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
        const double nx = relative ? mob->serverX + x : x;
        const double ny = relative ? mob->serverY + y : y;
        const double nz = relative ? mob->serverZ + z : z;
        mobs_->placeById(id, nx, ny, nz, hasLook, yaw, pitch);
        return;
    }

    if (items_ == nullptr) {
        return;
    }
    if (entity::ItemEntity* item = items_->findById(id)) {
        // An item is not smoothed: it is small, it is usually already still,
        // and `EntityItem` on a client has no interpolation of its own either.
        const double nx = relative ? item->x + x : x;
        const double ny = relative ? item->y + y : y;
        const double nz = relative ? item->z + z : z;
        items_->placeById(id, nx, ny, nz);
    }
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
        player->targetX = player->x;
        player->targetY = player->y;
        player->targetZ = player->z;
        player->yaw = fromByteAngle(packet.integer(4));
        player->pitch = fromByteAngle(packet.integer(5));
        player->prevYaw = player->yaw;
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
        // the collector; this does not, so it simply goes.
        if (items_ != nullptr) {
            items_->removeById(i32(packet.integer(0)));
        }
        removePlayer(i32(packet.integer(0)));
        return true;

    case packet::DestroyEntity:
        removePlayer(i32(packet.integer(0)));
        if (items_ != nullptr) {
            items_->removeById(i32(packet.integer(0)));
        }
        if (mobs_ != nullptr) {
            mobs_->removeById(i32(packet.integer(0)));
        }
        return true;

    case packet::EntityStatus:
        // Ours; see `packet::EntityStatus`. **A player answers only a death**:
        // `handleHealthUpdate(3)`'s death noise -- `random.hurt`, which `dm`
        // uses for both -- and the fall that `tick` counts. A second 3 for a
        // body already falling is not a second death.
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
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
        return true;

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
        return true;
    }

    case packet::BlockItemSwitch:
        // `dz` towards a client is another player changing what they hold.
        if (RemotePlayer* player = findPlayer(i32(packet.integer(0)))) {
            player->heldItem = i16(packet.integer(1));
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
        // **Received, understood, and not drawn.** A boat and a cart are two
        // more pools, and a cart is tilted along the track it stands on rather
        // than by anything the packet carries. Counted so the debug page can
        // say the server is sending more than is on screen.
        ++unhandledSpawns_;
        return true;

    default:
        return false;
    }
}

void RemoteEntities::tick(const tick::TickWorld* world)
{
    for (int i = 0; i < playerCount_; ++i) {
        RemotePlayer& player = players_[i];
        if (!player.used) {
            continue;
        }

        player.prevX = player.x;
        player.prevY = player.y;
        player.prevZ = player.z;
        player.prevYaw = player.yaw;
        player.prevPitch = player.pitch;
        player.prevLimbAmount = player.limbAmount;
        ++player.ticksExisted;

        if (player.smoothTicks > 0) {
            // `onUpdate`: a third of what is left, then a quarter of that, and
            // so on -- the gap is closed over the three ticks a position update
            // is given, and a body that hears nothing more simply stops.
            const double steps = double(player.smoothTicks);
            player.x += (player.targetX - player.x) / steps;
            player.y += (player.targetY - player.y) / steps;
            player.z += (player.targetZ - player.z) / steps;

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
            const int by = int(MathHelper::floorDouble(player.y));
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
    playerCount_ = 0;
    unhandledSpawns_ = 0;
}

}  // namespace mc::net
