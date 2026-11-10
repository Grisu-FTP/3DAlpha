// The serving half of a hosted world. See world_server.hpp.

#include "core/net/world_server.hpp"

#include "core/net/chunk_payload.hpp"
#include "core/net/entities.hpp"
#include "core/net/player_skin.hpp"
#include "core/net/player_sync.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/compress.hpp"
#include "core/util/span.hpp"
#include "core/util/worker.hpp"
#include "items.hpp"  // generated; see tools/configure.py

#include "items.hpp"  // generated; see tools/configure.py

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mc::net {

namespace {

// How much of a player's outbox goes to the link in one call. Well under
// `link::kMaxMessage` (1,388) on purpose: this file must not know the link's
// framing, and the slack costs a few per cent of a datagram.
constexpr usize kChunkBytes = 1024;

// Stop making work for a guest whose link is already this far behind. A column
// is a few kilobytes, so this is a handful of them -- enough to keep the radio
// busy, little enough that a guest who turns round is not shown ground they
// walked away from a second ago.
constexpr usize kOutboxHighWater = 24u << 10;

// **Past this, a guest is sent no more positions until it catches up** -- and
// below the second, it is sent them all again, current. Twenty-odd bytes a
// teleport, twenty times a second for every animal, item and player in view:
// on a link carrying less than that, a queue of them grew without bound and
// the guest watched the world further and further in the past, block changes
// and all. Eight kilobytes is under half a second of it at the worst.
constexpr usize kMoveHighWater = 8u << 10;
constexpr usize kMoveLowWater = 2u << 10;

// A guest's stream is a player, not a server: nothing it can legitimately send
// is large, and a buffer that grows past this is a stream that has lost its
// place.
constexpr usize kMaxInboundBytes = 64u << 10;

// Past this many changed blocks in one column the whole column is cheaper, and
// 0x34 carries a signed short count anyway.
constexpr int kMaxBlocksPerColumn = 64;

// How often the guests are told the time and where everybody is. The first is
// a1.1.2's own once-a-second; the second is one world tick, which is what
// `EntityOtherPlayerMP` interpolates against.
constexpr i64 kTimeIntervalMs = 1000;
constexpr i64 kEntityIntervalMs = 50;

// At most one column leaves the main thread per frame. The deflate behind it
// is several milliseconds and the radio behind that is slower still, so a
// larger budget would only move the queue from here to there.
constexpr int kColumnsPerPump = 1;

// **How long a guest waits for the ground they are supposed to be standing
// on.** The placement goes out once the column under it has, because a1.1.2's
// client closes its Downloading Terrain screen on the first Position & Look
// and sending it early drops the player through the world.
//
// Almost always that column arrives in a second or two. It never arrives when
// the position they are owed is somewhere this console has not loaded -- a
// spawn point a host has walked a thousand blocks from, say -- and the grid
// has one centre, so no amount of waiting will change it. Rather than leave
// them on a progress screen forever, they are put down beside the host and
// told so.
constexpr i64 kPlacementPatienceMs = 12000;

// How far down the owed list one pump looks for a column the host actually
// holds. The list is ordered nearest-first, so the ones that matter are at the
// front; this only stops a guest at the edge of the loaded area from walking
// all 169 of them every frame.
constexpr int kOwedScanLimit = 24;

// The two coordinates in one integer. Built out of unsigned halves because a
// negative chunk coordinate shifted left is undefined, and half the world has
// one.
i64 columnKey(i32 chunkX, i32 chunkZ)
{
    return i64((u64(u32(chunkX)) << 32) | u64(u32(chunkZ)));
}


const world::ChunkColumn* lookupColumn(void* ctx, i32 chunkX, i32 chunkZ)
{
    return static_cast<const ServerWorld*>(ctx)->column(chunkX, chunkZ);
}

// Whether there is anything to say: a spawn already starts bare.
bool wearsAnything(const WireStack* worn)
{
    for (int i = 0; i < kWireArmourSlots; ++i) {
        if (!worn[i].empty()) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ---- the oven --------------------------------------------------------------

ColumnOven::~ColumnOven()
{
    stop();
}

bool ColumnOven::start()
{
    stopRequested_ = false;
    // **`Net` rather than `Generation`, on a console that is doing both.** The
    // deflate is CPU-bound, so core 2 would be the faster home for it -- but on
    // a host that core is the generation worker's and the world being made
    // matters more than the world being sent. `Net`'s slot (core 0, one step
    // under the main thread) is preempted the instant a frame needs the CPU, so
    // an oven that falls behind costs a guest a slower download and costs the
    // player at the console nothing. See spawnWorker in platform/ctr/main.cpp.
    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformThread_ = workerSpawn()(&ColumnOven::threadEntry, this, WorkerRole::Net);
        running_ = platformThread_ != nullptr;
        return running_;
    }
    thread_ = std::thread([this] { run(); });
    running_ = true;
    return true;
}

void ColumnOven::stop()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopRequested_ = true;
    }
    wake_.notify_all();
    if (platformThread_ != nullptr) {
        workerJoin()(platformThread_);
        platformThread_ = nullptr;
    } else if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
    for (Job& job : jobs_) {
        job.slot = Slot::Free;
        job.raw.clear();
        job.encoded.clear();
    }
}

void ColumnOven::threadEntry(void* self)
{
    static_cast<ColumnOven*>(self)->run();
}

void ColumnOven::bake(Job& job)
{
    Packet packet;
    packet.reset(packet::MapChunk);
    packet.pushInt(job.chunkX * 16);
    packet.pushInt(0);
    packet.pushInt(job.chunkZ * 16);
    packet.pushInt(16 - 1);
    packet.pushInt(world::ChunkColumn::kHeight - 1);
    packet.pushInt(16 - 1);
    job.encoded.clear();
    // `new Deflater(1)`, the fastest level, is what the original server uses --
    // and what a console with a frame to hold wants even more than it does.
    if (zip::compress(ConstByteSpan(job.raw.data(), job.raw.size()), packet.bytes,
                      zip::Wrapper::Zlib, 1)) {
        encodePacket(packet, &job.encoded);
    }
    job.raw.clear();
}

void ColumnOven::run()
{
    for (;;) {
        int index = -1;
        {
            std::unique_lock<std::mutex> guard(lock_);
            wake_.wait(guard, [this, &index] {
                if (stopRequested_) {
                    return true;
                }
                for (int i = 0; i < kDepth; ++i) {
                    if (jobs_[i].slot == Slot::Waiting) {
                        index = i;
                        return true;
                    }
                }
                return false;
            });
            if (stopRequested_) {
                return;
            }
            if (index < 0) {
                continue;
            }
            jobs_[index].slot = Slot::Baking;
        }

        bake(jobs_[index]);

        {
            std::lock_guard<std::mutex> guard(lock_);
            jobs_[index].slot = Slot::Done;
        }
    }
}

bool ColumnOven::offer(u8 playerId, i32 chunkX, i32 chunkZ, std::vector<u8>* raw)
{
    int index = -1;
    {
        std::lock_guard<std::mutex> guard(lock_);
        for (int i = 0; i < kDepth; ++i) {
            if (jobs_[i].slot == Slot::Free) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            return false;
        }
        jobs_[index].playerId = playerId;
        jobs_[index].chunkX = chunkX;
        jobs_[index].chunkZ = chunkZ;
        jobs_[index].raw.swap(*raw);
        jobs_[index].slot = running_ ? Slot::Waiting : Slot::Baking;
    }

    if (running_) {
        wake_.notify_one();
        return true;
    }

    // No thread: the frame pays for it. Slow, but a session rather than none.
    bake(jobs_[index]);
    std::lock_guard<std::mutex> guard(lock_);
    jobs_[index].slot = Slot::Done;
    return true;
}

bool ColumnOven::take(u8* playerId, i32* chunkX, i32* chunkZ, std::vector<u8>* encoded)
{
    std::lock_guard<std::mutex> guard(lock_);
    for (Job& job : jobs_) {
        if (job.slot != Slot::Done) {
            continue;
        }
        *playerId = job.playerId;
        *chunkX = job.chunkX;
        *chunkZ = job.chunkZ;
        encoded->swap(job.encoded);
        job.encoded.clear();
        job.slot = Slot::Free;
        return true;
    }
    return false;
}

int ColumnOven::outstanding() const
{
    std::lock_guard<std::mutex> guard(lock_);
    int n = 0;
    for (const Job& job : jobs_) {
        if (job.slot != Slot::Free) {
            ++n;
        }
    }
    return n;
}

// ---- the server ------------------------------------------------------------

WorldServer::WorldServer() = default;

WorldServer::~WorldServer()
{
    close();
}

void WorldServer::open(ServerWorld* world, SendFn send, void* ctx, const std::string& hostName)
{
    world_ = world;
    send_ = send;
    ctx_ = ctx;
    // **Above the ids the players hold.** See `kFirstFreeEntityId`: a player's
    // entity id is their session player id so that every console can work out
    // their colour without being told it, which only works if nothing else is
    // ever handed one of those numbers.
    nextEntityId_ = kFirstFreeEntityId;
    columnsSent_ = 0;
    blockPacketsSent_ = 0;
    lastTimeMs_ = 0;
    lastEntityMs_ = 0;
    hostPosed_ = false;
    dirty_.clear();
    knownItems_.clear();
    knownMobs_.clear();
    knownPaintings_.clear();
    knownVehicles_.clear();
    knownFallers_.clear();
    knownArrows_.clear();
    lastItemMs_ = 0;
    lastMobMs_ = 0;
    lastVehicleMs_ = 0;
    lastFallerMs_ = 0;
    lastArrowMs_ = 0;

    for (Player& player : players_) {
        player = Player{};
    }

    // Slot zero is the host: it holds no columns and has no stream, but it is
    // an entity to everybody else and it is where `setHostPose` writes.
    Player& self = players_[0];
    self.used = true;
    self.id = 0;
    self.entityId = kFirstPlayerEntityId;
    self.name = hostName;
    self.loggedIn = true;
    self.placed = true;

    oven_.start();
}

void WorldServer::close()
{
    oven_.stop();
    world_ = nullptr;
    localSink_ = nullptr;
    localCtx_ = nullptr;
    send_ = nullptr;
    ctx_ = nullptr;
    for (Player& player : players_) {
        player = Player{};
    }
    dirty_.clear();
    knownItems_.clear();
    knownMobs_.clear();
    knownPaintings_.clear();
    knownVehicles_.clear();
    knownFallers_.clear();
    knownArrows_.clear();
}

WorldServer::Player* WorldServer::find(u8 playerId)
{
    for (int i = 1; i < kMaxPlayers; ++i) {
        if (players_[i].used && players_[i].id == playerId) {
            return &players_[i];
        }
    }
    return nullptr;
}

const WorldServer::Player* WorldServer::find(u8 playerId) const
{
    return const_cast<WorldServer*>(this)->find(playerId);
}

int WorldServer::playerCount() const
{
    int n = 0;
    for (int i = 1; i < kMaxPlayers; ++i) {
        if (players_[i].used) {
            ++n;
        }
    }
    return n;
}

WorldServer::SavedPlayer* WorldServer::savedFor(const std::string& name)
{
    for (SavedPlayer& saved : saved_) {
        if (saved.name == name) {
            return &saved;
        }
    }
    return nullptr;
}

void WorldServer::remember(const Player& player)
{
    if (player.name.empty()) {
        return;
    }
    SavedPlayer* saved = savedFor(player.name);
    if (saved == nullptr) {
        saved_.push_back(SavedPlayer{});
        saved = &saved_.back();
        saved->name = player.name;
    }
    // **Only a position they actually stood at.** Until the placement has gone
    // out and been echoed, the numbers here are the ones *we* chose for them,
    // and writing those back would pin somebody to the spawn point forever if
    // they joined and left again before the ground arrived.
    if (player.placed && !player.displaced) {
        saved->x = player.x;
        saved->feetY = player.feetY;
        saved->z = player.z;
        saved->yaw = player.yaw;
        saved->pitch = player.pitch;
        saved->placed = true;
    }
}

void WorldServer::addPlayer(u8 playerId, const std::string& name)
{
    if (world_ == nullptr || find(playerId) != nullptr) {
        return;
    }
    for (int i = 1; i < kMaxPlayers; ++i) {
        if (players_[i].used) {
            continue;
        }
        Player& player = players_[i];
        player = Player{};
        player.used = true;
        player.id = playerId;
        // The session's own player id, which is what makes the colour on the
        // nametag and the colour on the map agree across consoles that have
        // never exchanged one. Guests are numbered from `kHostPlayerId + 1`,
        // so this is inside the reserved range by construction.
        player.entityId = playerId < kMaxPlayerEntityIds ? i32(playerId)
                                                         : nextEntityId_++;
        player.name = name;

        // **Where they were, or the spawn point.** This is `ServerConfiguration
        // Manager`'s answer: a player with a position on this server comes back
        // to it, and a player without one starts where the world does. See
        // `SavedPlayer` for why the host is the one holding that.
        if (const SavedPlayer* saved = savedFor(name)) {
            player.x = saved->x;
            player.feetY = saved->feetY;
            player.z = saved->z;
            player.yaw = saved->yaw;
            player.pitch = saved->pitch;
        } else {
            // **`spawnY + 1` is the feet, not the eye and not the ground.**
            // `dm`'s constructor stands a fresh player at `spawnX + 0.5,
            // spawnY + 1, spawnZ + 0.5` and `setLocationAndAngles` adds a
            // `yOffset` of zero to it. Standing them at `spawnY` instead put
            // every joiner a block into the floor -- and the lift that would
            // have got them out of it is `settleFeet`, which cannot run until
            // the ground is loaded. See `streamColumns`.
            i32 sx = 0, sy = 0, sz = 0;
            world_->spawnPoint(&sx, &sy, &sz);
            player.x = double(sx) + 0.5;
            player.feetY = double(sy) + 1.0;
            player.z = double(sz) + 0.5;
        }
        player.eyeY = player.feetY + 1.62;
        return;
    }
}

void WorldServer::removePlayer(u8 playerId)
{
    Player* player = find(playerId);
    if (player == nullptr) {
        return;
    }
    remember(*player);
    // **Whatever they were steering is the host's again**, where they left it.
    releaseVehiclesOf(*player);
    if (player->spawned) {
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(player->entityId);
        announce(packet_, playerId);
    }
    *player = Player{};
}

// ---- writing ---------------------------------------------------------------

void WorldServer::write(Player& player, const Packet& packet)
{
    encodePacket(packet, &player.outbox);
}

void WorldServer::broadcast(const Packet& packet, u8 exceptPlayerId)
{
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (!player.used || !player.loggedIn || player.id == exceptPlayerId) {
            continue;
        }
        write(player, packet);
    }
}

void WorldServer::broadcastMove(const Packet& packet, u8 exceptPlayerId)
{
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (!player.used || !player.loggedIn || player.id == exceptPlayerId) {
            continue;
        }
        if (player.movesOwed || player.outbox.size() > kMoveHighWater) {
            player.movesOwed = true;
            continue;
        }
        write(player, packet);
    }
}

void WorldServer::announceMove(const Packet& packet, u8 exceptPlayerId)
{
    broadcastMove(packet, exceptPlayerId);
    if (localSink_ != nullptr) {
        localSink_(localCtx_, packet);
    }
}

void WorldServer::resendPositions(Player& player)
{
    const auto teleport = [&](i32 entityId, i32 x, i32 y, i32 z, i32 yaw, i32 pitch) {
        packet_.reset(packet::EntityTeleport);
        packet_.pushInt(entityId);
        packet_.pushInt(x);
        packet_.pushInt(y);
        packet_.pushInt(z);
        packet_.pushInt(yaw);
        packet_.pushInt(pitch);
        write(player, packet_);
    };
    for (int i = 0; i < kMaxPlayers; ++i) {
        const Player& subject = players_[i];
        if (!subject.used || !subject.loggedIn || !subject.everSent || subject.id == player.id
            || !(i == 0 || subject.spawned)) {
            continue;
        }
        teleport(subject.entityId, subject.lastSentX, subject.lastSentY, subject.lastSentZ,
                 subject.lastSentYaw, subject.lastSentPitch);
    }
    for (const KnownItem& item : knownItems_) {
        teleport(item.entityId, item.x, item.y, item.z, 0, 0);
    }
    for (const KnownMob& mob : knownMobs_) {
        teleport(mob.entityId, mob.x, mob.y, mob.z, mob.yaw, mob.pitch);
    }
    for (const KnownVehicle& vehicle : knownVehicles_) {
        // Not the one this guest is steering: it is the one saying where that
        // is.
        if (vehicle.rider != player.entityId) {
            teleport(vehicle.entityId, vehicle.x, vehicle.y, vehicle.z, vehicle.yaw, 0);
        }
    }
    for (const KnownFaller& faller : knownFallers_) {
        teleport(faller.entityId, faller.x, faller.y, faller.z, 0, 0);
    }
    for (const KnownArrow& arrow : knownArrows_) {
        teleport(arrow.entityId, arrow.x, arrow.y, arrow.z, arrow.yaw, arrow.pitch);
    }
    player.movesOwed = false;
}

void WorldServer::announce(const Packet& packet, u8 exceptPlayerId)
{
    broadcast(packet, exceptPlayerId);
    if (localSink_ != nullptr) {
        localSink_(localCtx_, packet);
    }
}

void WorldServer::flush(Player& player)
{
    usize offset = 0;
    while (offset < player.outbox.size()) {
        const usize take = std::min(kChunkBytes, player.outbox.size() - offset);
        if (!send_(ctx_, player.id, player.outbox.data() + offset, take)) {
            break;  // the link is full; the rest waits for the next frame
        }
        offset += take;
    }
    if (offset == player.outbox.size()) {
        player.outbox.clear();
    } else if (offset > 0) {
        player.outbox.erase(player.outbox.begin(), player.outbox.begin() + std::ptrdiff_t(offset));
    }
}

void WorldServer::say(const std::string& text)
{
    if (world_ == nullptr) {
        return;
    }
    packet_.reset(packet::Chat);
    packet_.pushString(text);
    broadcast(packet_, 0);
}

// ---- login -----------------------------------------------------------------

void WorldServer::login(Player& player)
{
    player.loggedIn = true;
    player.loggedInMs = nowMs_;

    packet_.reset(packet::Login);
    packet_.pushInt(player.entityId);
    packet_.pushString("");
    packet_.pushString("");
    write(player, packet_);

    i32 sx = 0, sy = 0, sz = 0;
    world_->spawnPoint(&sx, &sy, &sz);
    packet_.reset(packet::SpawnPosition);
    packet_.pushInt(sx);
    packet_.pushInt(sy);
    packet_.pushInt(sz);
    write(player, packet_);

    packet_.reset(packet::TimeUpdate);
    packet_.pushInt(world_->timeTicks());
    write(player, packet_);

    // **What they were carrying when they last left.** The server's only push
    // of an inventory in this protocol is exactly this one, at login; after it
    // the client owns the pack again and pushes its own changes back.
    if (const SavedPlayer* saved = savedFor(player.name)) {
        if (saved->haveInventory) {
            write(player, makeInventory(kInventoryMain, saved->main.data(),
                                        int(saved->main.size())));
            write(player, makeInventory(kInventoryArmour, saved->armour.data(),
                                        int(saved->armour.size())));
            adoptWorn(player, saved->armour.data(), saved->armour.size());
        }
    }

    // **Everyone else, before any ground.** A Named Entity Spawn for a player
    // in a column this guest has not got yet is harmless -- `RemoteEntities`
    // keeps a place, not a block -- and doing it here means nobody has to
    // remember to do it later.
    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& other = players_[i];
        if (!other.used || other.id == player.id || !other.loggedIn) {
            continue;
        }
        fillNamedSpawn(other);
        write(player, packet_);
        // What they have on, which a spawn starts without.
        if (wearsAnything(other.worn)) {
            write(player, makeWorn(other.entityId, other.worn));
        }
        // **And how they stand**, which the spawn cannot say: somebody already
        // crouching is drawn crouching, and somebody lying dead falls over and
        // goes, as they did on everyone else's screen.
        if (other.dead) {
            write(player, makeEntityStatus(other.entityId, entity::kStatusDead));
        } else if (other.sneaking) {
            write(player, makeEntityAction(other.entityId, kActionCrouch));
        }
    }

    // **"The host is in nothing"**, which is also how a guest learns this host
    // shares its vehicles at all -- see `RemoteEntities::sharesVehicles`. If the
    // host is in one, the re-announcement below says so straight after. Every
    // console of this family parses a right-click Use Entity and ignores what
    // it does not understand, a 3DS guest included.
    write(player, makeUseEntity(host().entityId, kNoVehicle, false));

    // ...and everyone else hears about this one.
    fillNamedSpawn(player);
    announce(packet_, player.id);
    player.spawned = true;
    if (wearsAnything(player.worn)) {
        announce(makeWorn(player.entityId, player.worn), player.id);
    }

    // **Everything alive, again.** `knownItems_` and `knownMobs_` record what
    // the *session* has been told, not what each player has: an animal is
    // announced once, when it appears, and a console that joined afterwards
    // was never in the room for it. Every mob and every stack already in this
    // world was invisible to a late joiner for the rest of its life -- which
    // is most of them, because a guest joins a world that has been running.
    //
    // Forgetting both re-announces everything on the next `syncItems` and
    // `syncMobs`, to everybody. The guests who already had them are not
    // confused by it: `spawnFromServer` on both pools removes the id first, so
    // a second spawn replaces rather than twins. The cost is one burst per
    // join, which is the right price for a world that is actually there.
    // The arrows the same way; `ArrowSystem::spawnRemote` replaces too.
    knownItems_.clear();
    knownMobs_.clear();
    knownPaintings_.clear();
    // The boats, the carts and whoever is in them, and anything mid-fall or
    // mid-fuse. A guest steering a vehicle is left out of its re-announcement:
    // a spawn replaces, and replacing the boat under them would tip them out.
    knownVehicles_.clear();
    knownFallers_.clear();
    // The arrows the same way; `ArrowSystem::spawnRemote` replaces too.
    knownArrows_.clear();

    rebuildView(player);
}

// ---- the view --------------------------------------------------------------

void WorldServer::rebuildView(Player& player)
{
    const i32 centreX = i32(std::floor(player.x / 16.0));
    const i32 centreZ = i32(std::floor(player.z / 16.0));
    player.viewX = centreX;
    player.viewZ = centreZ;
    player.viewSet = true;

    // **Let go of what is behind them.** One ring of slack past the view, so a
    // player pacing over a chunk border does not make the host resend the
    // column they just left.
    constexpr i32 kKeep = kViewDistance + 1;
    for (usize i = 0; i < player.holds.size();) {
        const i64 key = player.holds[i];
        const i32 cx = i32(u32(u64(key) >> 32));
        const i32 cz = i32(u32(u64(key)));
        if (std::abs(cx - centreX) <= kKeep && std::abs(cz - centreZ) <= kKeep) {
            ++i;
            continue;
        }
        packet_.reset(packet::PreChunk);
        packet_.pushInt(cx);
        packet_.pushInt(cz);
        packet_.pushInt(0);
        write(player, packet_);
        player.holds[i] = player.holds.back();
        player.holds.pop_back();
    }

    // What is owed, nearest first. Rebuilt rather than patched: it is 169
    // entries and this runs when a player crosses a chunk border, not every
    // frame.
    player.owed.clear();
    for (int radius = 0; radius <= kViewDistance; ++radius) {
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != radius) {
                    continue;
                }
                const i32 cx = centreX + dx;
                const i32 cz = centreZ + dz;
                const i64 key = columnKey(cx, cz);
                if (std::find(player.holds.begin(), player.holds.end(), key)
                    != player.holds.end()) {
                    continue;
                }
                player.owed.push_back(Pending{cx, cz});
            }
        }
    }
}

void WorldServer::streamColumns()
{
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (!player.used || !player.loggedIn) {
            continue;
        }

        // **The ground they are standing on**, which is the only honest test of
        // whether this console has a world for them. Asking instead whether
        // anything in the owed list was loaded says yes far too often: the
        // front of that list is the far ring, and the far ring is past the edge
        // of the host's grid for a guest standing anywhere but its middle. See
        // the note at the top of this file.
        player.starved = world_->column(player.viewX, player.viewZ) == nullptr;

        if (player.owed.empty() || player.outbox.size() > kOutboxHighWater) {
            continue;
        }

        // **One column, and the first of the near ones this console actually
        // holds.** The list is nearest-first, so a gap in it is ground the host
        // has not loaded and the entry stays where it is to be asked about
        // again next frame.
        const int limit = std::min(int(player.owed.size()), kOwedScanLimit);
        for (int scan = 0; scan < limit; ++scan) {
            const Pending owed = player.owed[usize(scan)];
            if (world_->column(owed.chunkX, owed.chunkZ) == nullptr) {
                continue;
            }
            scratch_.clear();
            serializeRegion(&lookupColumn, world_, owed.chunkX * 16, 0, owed.chunkZ * 16, 16,
                            world::ChunkColumn::kHeight, 16, &scratch_);
            if (!oven_.offer(player.id, owed.chunkX, owed.chunkZ, &scratch_)) {
                break;  // the oven is full; everything is still owed
            }
            player.owed.erase(player.owed.begin() + std::ptrdiff_t(scan));
            player.inOven.push_back(columnKey(owed.chunkX, owed.chunkZ));
            break;
        }
    }
}

void WorldServer::takeOven()
{
    u8 playerId = 0;
    i32 chunkX = 0;
    i32 chunkZ = 0;
    std::vector<u8> encoded;
    while (oven_.take(&playerId, &chunkX, &chunkZ, &encoded)) {
        Player* player = find(playerId);
        if (player == nullptr || encoded.empty()) {
            continue;  // they left while it was in the oven
        }

        const i64 key = columnKey(chunkX, chunkZ);
        auto baking = std::find(player->inOven.begin(), player->inOven.end(), key);
        if (baking != player->inOven.end()) {
            *baking = player->inOven.back();
            player->inOven.pop_back();
        }

        // **Thrown away rather than sent**, because the world moved under it
        // while it was in the oven. See `Player::stale`.
        auto stale = std::find(player->stale.begin(), player->stale.end(), key);
        if (stale != player->stale.end()) {
            *stale = player->stale.back();
            player->stale.pop_back();
            player->owed.insert(player->owed.begin(), Pending{chunkX, chunkZ});
            continue;
        }

        // Mode 1: a1.1.2's client makes the empty column the Map Chunk fills.
        packet_.reset(packet::PreChunk);
        packet_.pushInt(chunkX);
        packet_.pushInt(chunkZ);
        packet_.pushInt(1);
        write(*player, packet_);
        player->outbox.insert(player->outbox.end(), encoded.begin(), encoded.end());
        player->holds.push_back(key);
        ++columnsSent_;

        // **The position goes out once the ground under it has.** a1.1.2's
        // client closes its Downloading Terrain screen on the first Position &
        // Look, so sending it before there is anything to stand on drops the
        // player through the world.
        if (!player->placed && chunkX == player->viewX && chunkZ == player->viewZ) {
            // **The lift, here and nowhere else.** This is the first and only
            // moment the ground under this player is known to be loaded, which
            // is what `kh.q()` needs to be able to answer at all.
            player->feetY = world_->settleFeet(player->x, player->feetY, player->z);
            player->eyeY = player->feetY + 1.62;
            packet_.reset(packet::PlayerPositionLook);
            packet_.pushReal(player->x);
            packet_.pushReal(player->eyeY);
            packet_.pushReal(player->feetY);
            packet_.pushReal(player->z);
            packet_.pushReal(double(player->yaw));
            packet_.pushReal(double(player->pitch));
            packet_.pushInt(0);
            write(*player, packet_);
            player->placed = true;
        }
    }
}

void WorldServer::placeStragglers()
{
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (!player.used || !player.loggedIn || player.placed) {
            continue;
        }
        if (nowMs_ - player.loggedInMs < kPlacementPatienceMs) {
            continue;
        }
        // **Nowhere else to put them.** Without a host position there is no
        // second guess to make, so they keep waiting -- which is the honest
        // answer when this console does not know where anything is either.
        if (!hostPosed_) {
            player.loggedInMs = nowMs_;
            continue;
        }
        const Player& host = players_[0];
        player.x = host.x;
        player.feetY = host.feetY;
        player.z = host.z;
        player.eyeY = player.feetY + 1.62;
        player.displaced = true;
        player.loggedInMs = nowMs_;
        rebuildView(player);
    }
}

int WorldServer::viewCentres(ViewCentre* out, int max) const
{
    if (out == nullptr || max <= 0) {
        return 0;
    }
    int count = 0;
    // From 1: `players_[0]` is the host, whose ground is the grid's own.
    for (int i = 1; i < kMaxPlayers && count < max; ++i) {
        const Player& player = players_[i];
        if (!player.used || !player.loggedIn || !player.viewSet) {
            continue;
        }
        out[count].chunkX = player.viewX;
        out[count].chunkZ = player.viewZ;
        ++count;
    }
    return count;
}

bool WorldServer::displaced(u8 playerId) const
{
    const Player* player = find(playerId);
    return player != nullptr && player->displaced;
}

// ---- block changes ---------------------------------------------------------

void WorldServer::blockChanged(i32 x, int y, i32 z)
{
    if (world_ == nullptr || y < 0 || y >= world::ChunkColumn::kHeight) {
        return;
    }
    // Nobody to tell. This is the fluid's path -- hundreds of calls a tick --
    // so the single-player cost of hosting nothing has to be one compare.
    bool anyone = false;
    for (int i = 1; i < kMaxPlayers; ++i) {
        if (players_[i].used && players_[i].loggedIn) {
            anyone = true;
            break;
        }
    }
    if (!anyone) {
        return;
    }

    const i32 chunkX = x >> 4;
    const i32 chunkZ = z >> 4;
    DirtyColumn* column = nullptr;
    for (DirtyColumn& candidate : dirty_) {
        if (candidate.chunkX == chunkX && candidate.chunkZ == chunkZ) {
            column = &candidate;
            break;
        }
    }
    if (column == nullptr) {
        dirty_.push_back(DirtyColumn{chunkX, chunkZ, {}, false});
        column = &dirty_.back();
        column->blocks.reserve(kMaxBlocksPerColumn);
    }
    if (column->overflowed) {
        return;
    }

    const u16 packed = u16((u16(x & 15) << 12) | (u16(z & 15) << 8) | u16(y & 0xFF));
    for (u16 already : column->blocks) {
        if (already == packed) {
            return;
        }
    }
    if (int(column->blocks.size()) >= kMaxBlocksPerColumn) {
        // Past this the column itself is the cheaper thing to send.
        column->overflowed = true;
        column->blocks.clear();
        return;
    }
    column->blocks.push_back(packed);
}

void WorldServer::sendBlockChanges()
{
    if (dirty_.empty()) {
        return;
    }
    for (const DirtyColumn& column : dirty_) {
        const world::ChunkColumn* held = world_->column(column.chunkX, column.chunkZ);
        const i64 key = columnKey(column.chunkX, column.chunkZ);

        // **A column somebody is waiting on is now out of date.** Marked here
        // and discarded when the oven hands it back; see `Player::stale`.
        for (int i = 1; i < kMaxPlayers; ++i) {
            Player& player = players_[i];
            if (!player.used
                || std::find(player.inOven.begin(), player.inOven.end(), key)
                       == player.inOven.end()) {
                continue;
            }
            if (std::find(player.stale.begin(), player.stale.end(), key)
                == player.stale.end()) {
                player.stale.push_back(key);
            }
        }

        if (column.overflowed || held == nullptr) {
            // Send it again from scratch, to whoever had it.
            for (int i = 1; i < kMaxPlayers; ++i) {
                Player& player = players_[i];
                if (!player.used || !player.loggedIn) {
                    continue;
                }
                auto it = std::find(player.holds.begin(), player.holds.end(), key);
                if (it == player.holds.end()) {
                    continue;
                }
                *it = player.holds.back();
                player.holds.pop_back();
                player.owed.insert(player.owed.begin(), Pending{column.chunkX, column.chunkZ});
            }
            continue;
        }

        if (column.blocks.empty()) {
            continue;
        }

        if (column.blocks.size() == 1) {
            const u16 packed = column.blocks[0];
            const int lx = (packed >> 12) & 15;
            const int lz = (packed >> 8) & 15;
            const int ly = packed & 0xFF;
            packet_.reset(packet::BlockChange);
            packet_.pushInt(column.chunkX * 16 + lx);
            packet_.pushInt(ly);
            packet_.pushInt(column.chunkZ * 16 + lz);
            packet_.pushInt(i64(held->block(lx, ly, lz)));
            packet_.pushInt(held->blockData(lx, ly, lz));
        } else {
            packet_.reset(packet::MultiBlockChange);
            packet_.pushInt(column.chunkX);
            packet_.pushInt(column.chunkZ);
            for (u16 packed : column.blocks) {
                const int lx = (packed >> 12) & 15;
                const int lz = (packed >> 8) & 15;
                const int ly = packed & 0xFF;
                packet_.changeCoords.push_back(i16(packed));
                packet_.changeIds.push_back(u8(held->block(lx, ly, lz)));
                packet_.changeData.push_back(held->blockData(lx, ly, lz));
            }
        }

        for (int i = 1; i < kMaxPlayers; ++i) {
            Player& player = players_[i];
            if (!player.used || !player.loggedIn) {
                continue;
            }
            if (std::find(player.holds.begin(), player.holds.end(), key) == player.holds.end()) {
                continue;
            }
            write(player, packet_);
            ++blockPacketsSent_;
        }
    }
    dirty_.clear();
}

// ---- entities --------------------------------------------------------------

void WorldServer::hostSwing()
{
    const Player& self = players_[0];
    if (!self.used || self.entityId == 0) {
        return;
    }
    broadcast(makeArmSwing(self.entityId), self.id);
}

void WorldServer::setHostStance(bool sneaking, bool alive)
{
    Player& self = players_[0];
    if (!self.used || self.entityId == 0) {
        return;
    }
    // The same three transitions `StanceReporter` makes for a guest, answered
    // the way `feed` answers them -- except that the host draws nothing of
    // itself, so the guests are told and the local sink is not.
    if (self.dead && alive) {
        self.dead = false;
        self.sneaking = false;
        fillNamedSpawn(self);
        broadcast(packet_, self.id);
        if (wearsAnything(self.worn)) {
            broadcast(makeWorn(self.entityId, self.worn), self.id);
        }
    }
    if (!self.dead && !alive) {
        self.dead = true;
        self.sneaking = false;
        broadcast(makeEntityStatus(self.entityId, entity::kStatusDead), self.id);
        return;
    }
    if (alive && sneaking != self.sneaking) {
        self.sneaking = sneaking;
        broadcast(makeEntityAction(self.entityId, sneaking ? kActionCrouch : kActionUncrouch),
                  self.id);
    }
}

void WorldServer::setHostHeld(int heldItem)
{
    Player& self = players_[0];
    if (!self.used || self.entityId == 0 || self.heldItem == i16(heldItem)) {
        return;
    }
    // Kept for a guest who joins later, whose Named Entity Spawn carries it.
    self.heldItem = i16(heldItem);
    packet_.reset(packet::BlockItemSwitch);
    packet_.pushInt(self.entityId);
    packet_.pushInt(heldItem);
    broadcast(packet_, self.id);
}

bool WorldServer::adoptWorn(Player& player, const WireStack* worn, usize count)
{
    bool changed = false;
    for (usize i = 0; i < usize(kWireArmourSlots); ++i) {
        const WireStack piece = i < count ? worn[i] : WireStack{};
        // The id alone: a dent does not show, and is not worth a packet.
        changed = changed || piece.id != player.worn[i].id;
        player.worn[i] = piece;
    }
    return changed;
}

void WorldServer::setHostArmour(const item::Inventory& inventory)
{
    Player& self = players_[0];
    if (!self.used || self.entityId == 0) {
        return;
    }
    WireStack worn[kWireArmourSlots];
    wornToWire(inventory, worn);
    if (adoptWorn(self, worn, usize(kWireArmourSlots))) {
        broadcast(makeWorn(self.entityId, self.worn), self.id);
    }
}

void WorldServer::setHostPose(double x, double feetY, double z, float yaw, float pitch)
{
    hostPosed_ = true;
    Player& self = players_[0];
    self.x = x;
    self.feetY = feetY;
    self.eyeY = feetY + 1.62;
    self.z = z;
    self.yaw = yaw;
    self.pitch = pitch;
}

void WorldServer::setHostSkin(const u8* page, usize size)
{
    // On the heap: a page is 8 KB and these frames may not be.
    std::vector<u8> clean(kSkinPageBytes);
    if (world_ == nullptr || !sanitizeSkinPage(page, size, clean.data())) {
        return;
    }
    Player& self = players_[0];
    self.skin = clean;
    broadcastExtended(makePlayerSkin(self.entityId, clean.data()), self.id);
}

void WorldServer::acceptSkin(Player& player, const Packet& p)
{
    std::vector<u8> page(kSkinPageBytes);
    if (!sanitizeSkinPage(p.bytes.data(), p.bytes.size(), page.data())) {
        return;
    }
    player.skinSeen = true;
    player.skin = page;
    // **Now it has said it understands us**: it is sent everybody's skin, and
    // every painting again (a guest replaces a painting it already has).
    player.extended = true;
    for (const Player& other : players_) {
        if (other.used && other.id != player.id && other.skin.size() == kSkinPageBytes) {
            write(player, makePlayerSkin(other.entityId, other.skin.data()));
        }
    }
    knownPaintings_.clear();
    if (player.spawned) {
        const Packet mine = makePlayerSkin(player.entityId, page.data());
        broadcastExtended(mine, player.id);
        if (localSink_ != nullptr) {
            localSink_(localCtx_, mine);
        }
    }
}

void WorldServer::broadcastExtended(const Packet& packet, u8 exceptPlayerId)
{
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (!player.used || !player.loggedIn || !player.extended || player.id == exceptPlayerId) {
            continue;
        }
        write(player, packet);
    }
}

void WorldServer::fillNamedSpawn(const Player& player)
{
    packet_.reset(packet::NamedEntitySpawn);
    packet_.pushInt(player.entityId);
    packet_.pushString(player.name);
    packet_.pushInt(toWirePosition(player.x));
    packet_.pushInt(toWirePosition(player.feetY));
    packet_.pushInt(toWirePosition(player.z));
    packet_.pushInt(toWireAngle(player.yaw));
    packet_.pushInt(toWireAngle(player.pitch));
    packet_.pushInt(player.heldItem);
}

WorldServer::Player* WorldServer::playerByEntity(i32 entityId)
{
    for (Player& player : players_) {
        if (player.used && player.entityId == entityId) {
            return &player;
        }
    }
    return nullptr;
}

bool WorldServer::hostAttack(i32 targetEntityId, int heldItem)
{
    Player* victim = playerByEntity(targetEntityId);
    if (victim == nullptr || !victim->loggedIn || victim->id == players_[0].id) {
        return false;
    }
    // The host's own held item goes first, for the reason `NetPlay::attack
    // Entity` reports one: the blow lands as hard as whatever the other end
    // thinks is in the hand.
    players_[0].heldItem = i16(heldItem);
    packet_.reset(packet::BlockItemSwitch);
    packet_.pushInt(players_[0].entityId);
    packet_.pushInt(heldItem);
    write(*victim, packet_);
    write(*victim, makeUseEntity(players_[0].entityId, victim->entityId, true));
    // **Everyone else sees the flash**, this console included: the victim's own
    // console takes the damage, but nobody else would ever hear it happened.
    announce(makeEntityStatus(victim->entityId, entity::kStatusHurt), victim->id);
    return true;
}

void WorldServer::sendEntities(i64 nowMillis)
{
    if (nowMillis - lastEntityMs_ < kEntityIntervalMs) {
        return;
    }
    lastEntityMs_ = nowMillis;

    for (int i = 0; i < kMaxPlayers; ++i) {
        Player& subject = players_[i];
        if (!subject.used || !subject.loggedIn || !(i == 0 || subject.spawned)) {
            continue;
        }
        const i32 wx = toWirePosition(subject.x);
        const i32 wy = toWirePosition(subject.feetY);
        const i32 wz = toWirePosition(subject.z);
        const i8 wyaw = toWireAngle(subject.yaw);
        const i8 wpitch = toWireAngle(subject.pitch);
        if (subject.everSent && wx == subject.lastSentX && wy == subject.lastSentY
            && wz == subject.lastSentZ && wyaw == subject.lastSentYaw
            && wpitch == subject.lastSentPitch) {
            continue;
        }
        subject.lastSentX = wx;
        subject.lastSentY = wy;
        subject.lastSentZ = wz;
        subject.lastSentYaw = wyaw;
        subject.lastSentPitch = wpitch;
        subject.everSent = true;

        // **Teleport rather than a relative move.** 0x1F carries eighths of a
        // block in a byte and needs the two ends to agree on where the entity
        // was; this carries where it *is*, costs eleven more bytes twenty
        // times a second, and cannot drift. On a link this short that is the
        // right trade.
        packet_.reset(packet::EntityTeleport);
        packet_.pushInt(subject.entityId);
        packet_.pushInt(wx);
        packet_.pushInt(wy);
        packet_.pushInt(wz);
        packet_.pushInt(wyaw);
        packet_.pushInt(wpitch);
        // **Slot zero is this console walking**, and it is drawn from the
        // player's own body rather than from a packet about itself.
        if (i == 0) {
            broadcastMove(packet_, subject.id);
        } else {
            announceMove(packet_, subject.id);
        }
    }
}

// ---- the paintings ---------------------------------------------------------

void WorldServer::syncPaintings()
{
    entity::PaintingSystem* pool = world_->paintings();
    if (pool == nullptr) {
        return;
    }
    for (KnownPainting& known : knownPaintings_) {
        known.seen = false;
    }
    for (int i = 0; i < pool->count(); ++i) {
        entity::Painting& painting = pool->at(i);
        // A guest-side prediction or a server's painting is not this world's to
        // announce; on a host neither exists, but the pool is shared code.
        if (!painting.alive || painting.remote || painting.predicted) {
            continue;
        }
        if (painting.entityId == 0) {
            painting.entityId = nextEntityId_++;
        }
        KnownPainting* known = nullptr;
        for (KnownPainting& candidate : knownPaintings_) {
            if (candidate.entityId == painting.entityId) {
                known = &candidate;
                break;
            }
        }
        if (known != nullptr) {
            known->seen = true;
            continue;
        }
        knownPaintings_.push_back(KnownPainting{painting.entityId, true});
        broadcastExtended(makePainting(painting.entityId, painting.artwork().name, painting.tileX,
                               painting.tileY, painting.tileZ, painting.direction),
                  0);
    }
    for (usize i = 0; i < knownPaintings_.size();) {
        if (knownPaintings_[i].seen) {
            ++i;
            continue;
        }
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(knownPaintings_[i].entityId);
        broadcastExtended(packet_, 0);
        knownPaintings_[i] = knownPaintings_.back();
        knownPaintings_.pop_back();
    }
}

// ---- the arrows ------------------------------------------------------------

void WorldServer::announceArrow(entity::Arrow& arrow)
{
    if (arrow.entityId == 0) {
        arrow.entityId = nextEntityId_++;
    }
    for (const KnownArrow& known : knownArrows_) {
        if (known.entityId == arrow.entityId) {
            return;
        }
    }
    const i32 wx = toWirePosition(arrow.x);
    const i32 wy = toWirePosition(arrow.y);
    const i32 wz = toWirePosition(arrow.z);
    const i8 wyaw = toWireAngle(arrow.yaw);
    const i8 wpitch = toWireAngle(arrow.pitch);
    knownArrows_.push_back(KnownArrow{arrow.entityId, wx, wy, wz, wyaw, wpitch, true});

    // **The spawn and then the heading**, because a Vehicle Spawn has no
    // angles: without the teleport a guest would draw the first tick of the
    // arrow pointing north.
    packet_.reset(packet::VehicleSpawn);
    packet_.pushInt(arrow.entityId);
    packet_.pushInt(kObjectArrow);
    packet_.pushInt(wx);
    packet_.pushInt(wy);
    packet_.pushInt(wz);
    broadcast(packet_, 0);

    packet_.reset(packet::EntityTeleport);
    packet_.pushInt(arrow.entityId);
    packet_.pushInt(wx);
    packet_.pushInt(wy);
    packet_.pushInt(wz);
    packet_.pushInt(wyaw);
    packet_.pushInt(wpitch);
    broadcast(packet_, 0);
    ++arrowsSpawned_;
}

void WorldServer::collectArrowsFor(Player& player, entity::ArrowSystem& arrows)
{
    // The reach `collectFor` uses, because `kg.b(dm)` is called from the same
    // loop in `EntityPlayer.onLivingUpdate` as `dx.b(dm)`.
    AABB reach;
    reach.minX = player.x - 0.3 - 1.0;
    reach.maxX = player.x + 0.3 + 1.0;
    reach.minY = player.feetY;
    reach.maxY = player.feetY + 1.8;
    reach.minZ = player.z - 0.3 - 1.0;
    reach.maxZ = player.z + 0.3 + 1.0;

    for (int i = arrows.count() - 1; i >= 0; --i) {
        entity::Arrow& arrow = *arrows.at(i);
        if (!entity::arrowCollectableBy(arrow, player.entityId)
            || !arrow.box.intersects(reach)) {
            continue;
        }
        // Everybody sees it go; the one who fired it gets it back. The host
        // cannot see into the guest's pack, so "does it fit" is theirs --
        // as it is for an item, see `collectFor`. An arrow is announced within
        // a frame of being loosed and cannot be collected for seven ticks after
        // it lands, so it always has its id by now; named here regardless.
        announceArrow(arrow);
        const i32 arrowId = arrow.entityId;
        packet_.reset(packet::Collect);
        packet_.pushInt(arrowId);
        packet_.pushInt(player.entityId);
        broadcast(packet_, 0);
        packet_.reset(packet::AddToInventory);
        packet_.pushInt(i64(mcver::Item::Arrow));
        packet_.pushInt(1);
        packet_.pushInt(0);
        write(player, packet_);

        arrows.removeById(arrowId);
        for (usize k = 0; k < knownArrows_.size(); ++k) {
            if (knownArrows_[k].entityId == arrowId) {
                knownArrows_[k] = knownArrows_.back();
                knownArrows_.pop_back();
                break;
            }
        }
    }
}

void WorldServer::syncArrows(i64 nowMillis)
{
    entity::ArrowSystem* arrows = world_->arrows();
    if (arrows == nullptr) {
        return;
    }

    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (player.used && player.loggedIn && player.placed && !player.dead) {
            collectArrowsFor(player, *arrows);
        }
    }

    const bool moveDue = nowMillis - lastArrowMs_ >= kEntityIntervalMs;
    if (moveDue) {
        lastArrowMs_ = nowMillis;
    }
    for (KnownArrow& known : knownArrows_) {
        known.seen = false;
    }

    for (int i = 0; i < arrows->count(); ++i) {
        entity::Arrow& arrow = *arrows->at(i);
        if (!arrow.alive || arrow.remote) {
            continue;
        }
        KnownArrow* known = nullptr;
        if (arrow.entityId != 0) {
            for (KnownArrow& candidate : knownArrows_) {
                if (candidate.entityId == arrow.entityId) {
                    known = &candidate;
                    break;
                }
            }
        }
        if (known == nullptr) {
            announceArrow(arrow);
            continue;
        }
        known->seen = true;

        const i32 wx = toWirePosition(arrow.x);
        const i32 wy = toWirePosition(arrow.y);
        const i32 wz = toWirePosition(arrow.z);
        const i8 wyaw = toWireAngle(arrow.yaw);
        const i8 wpitch = toWireAngle(arrow.pitch);
        if (!moveDue
            || (wx == known->x && wy == known->y && wz == known->z && wyaw == known->yaw
                && wpitch == known->pitch)) {
            continue;
        }
        known->x = wx;
        known->y = wy;
        known->z = wz;
        known->yaw = wyaw;
        known->pitch = wpitch;
        packet_.reset(packet::EntityTeleport);
        packet_.pushInt(arrow.entityId);
        packet_.pushInt(wx);
        packet_.pushInt(wy);
        packet_.pushInt(wz);
        packet_.pushInt(wyaw);
        packet_.pushInt(wpitch);
        broadcastMove(packet_, 0);
    }

    // Spent on a target, aged out, fallen out of the world, or taken back by
    // the host's own player.
    for (usize i = 0; i < knownArrows_.size();) {
        if (knownArrows_[i].seen) {
            ++i;
            continue;
        }
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(knownArrows_[i].entityId);
        broadcast(packet_, 0);
        knownArrows_[i] = knownArrows_.back();
        knownArrows_.pop_back();
    }
}

int WorldServer::arrowTargets(entity::RemoteTarget* out, int max) const
{
    int n = 0;
    for (int i = 1; i < kMaxPlayers && n < max; ++i) {
        const Player& player = players_[i];
        if (!player.used || !player.loggedIn || !player.placed || player.dead) {
            continue;
        }
        entity::RemoteTarget& target = out[n++];
        target.entityId = player.entityId;
        target.box = AABB{player.x - 0.3, player.feetY, player.z - 0.3,
                          player.x + 0.3, player.feetY + 1.8, player.z + 0.3};
    }
    return n;
}

void WorldServer::arrowStruck(i32 victimEntityId, entity::Arrow& arrow)
{
    Player* victim = playerByEntity(victimEntityId);
    if (victim == nullptr || victim->id == players_[0].id || !victim->loggedIn) {
        return;
    }
    // Named before it is spent, so the guest can find it: an arrow loosed at
    // point blank strikes on its first tick, before `syncArrows` has run.
    announceArrow(arrow);
    write(*victim, makeUseEntity(arrow.entityId, victim->entityId, true));
}

void WorldServer::mobStruck(i32 victimEntityId, i32 mobEntityId)
{
    Player* victim = playerByEntity(victimEntityId);
    if (victim == nullptr || victim->id == players_[0].id || !victim->loggedIn
        || mobEntityId == 0) {
        return;
    }
    write(*victim, makeUseEntity(mobEntityId, victim->entityId, true));
}

// ---- the vehicles ----------------------------------------------------------

void WorldServer::syncVehicles(i64 nowMillis)
{
    entity::BoatSystem* boats = world_->boats();
    entity::MinecartSystem* carts = world_->minecarts();
    if (boats == nullptr && carts == nullptr) {
        return;
    }

    const bool moveDue = nowMillis - lastVehicleMs_ >= kEntityIntervalMs;
    if (moveDue) {
        lastVehicleMs_ = nowMillis;
    }
    for (KnownVehicle& known : knownVehicles_) {
        known.seen = false;
    }

    // **Who got out and who got in, told after everything else and in that
    // order**, so a rider who moved from one vehicle to another in the same
    // frame is never last heard of as getting out.
    struct Seat {
        i32 rider;
        i32 vehicle;
    };
    std::vector<Seat> outs;
    std::vector<Seat> ins;

    const auto visit = [&](bool boat, i32 entityId, int type, double x, double y, double z,
                           float yaw, int hit, i32 rider, bool burning) {
        const i32 wx = toWirePosition(x);
        const i32 wy = toWirePosition(y);
        const i32 wz = toWirePosition(z);
        const i8 wyaw = toWireAngle(yaw);
        // The guest steering it, who is never told where it is: they are the
        // one saying.
        const Player* driver = rider != 0 && rider != host().entityId ? playerByEntity(rider)
                                                                     : nullptr;
        const u8 except = driver != nullptr ? driver->id : 0;

        KnownVehicle* known = nullptr;
        for (KnownVehicle& candidate : knownVehicles_) {
            if (candidate.entityId == entityId) {
                known = &candidate;
                break;
            }
        }
        if (known == nullptr) {
            KnownVehicle fresh;
            fresh.entityId = entityId;
            fresh.boat = boat;
            fresh.x = wx;
            fresh.y = wy;
            fresh.z = wz;
            fresh.yaw = wyaw;
            fresh.hit = hit;
            fresh.rider = rider;
            fresh.burning = burning;
            fresh.seen = true;
            knownVehicles_.push_back(fresh);
            // **The spawn and then the heading**, because a Vehicle Spawn has
            // no angles: without the teleport a guest would draw the first
            // ticks of every boat pointing the same way.
            packet_.reset(packet::VehicleSpawn);
            packet_.pushInt(entityId);
            packet_.pushInt(type);
            packet_.pushInt(wx);
            packet_.pushInt(wy);
            packet_.pushInt(wz);
            broadcast(packet_, except);
            packet_.reset(packet::EntityTeleport);
            packet_.pushInt(entityId);
            packet_.pushInt(wx);
            packet_.pushInt(wy);
            packet_.pushInt(wz);
            packet_.pushInt(wyaw);
            packet_.pushInt(0);
            broadcast(packet_, except);
            if (burning) {
                broadcast(makeEntityStatus(entityId, entity::kStatusFlagSet), 0);
            }
            if (rider != 0) {
                ins.push_back(Seat{rider, entityId});
            }
            return;
        }

        known->seen = true;
        // **A blow, as it lands**: `attack` puts `timeSinceHit` back up to ten
        // and nothing else ever raises it. Everybody sees the rock, the one
        // steering included.
        if (hit > known->hit) {
            broadcast(makeEntityStatus(entityId, entity::kStatusHurt), 0);
        }
        known->hit = hit;
        if (burning != known->burning) {
            known->burning = burning;
            broadcast(makeEntityStatus(entityId, burning ? entity::kStatusFlagSet
                                                         : entity::kStatusFlagClear),
                      0);
        }
        if (rider != known->rider) {
            if (known->rider != 0) {
                outs.push_back(Seat{known->rider, entityId});
            }
            if (rider != 0) {
                ins.push_back(Seat{rider, entityId});
            }
            known->rider = rider;
        }
        if (!moveDue
            || (wx == known->x && wy == known->y && wz == known->z && wyaw == known->yaw)) {
            return;
        }
        known->x = wx;
        known->y = wy;
        known->z = wz;
        known->yaw = wyaw;
        packet_.reset(packet::EntityTeleport);
        packet_.pushInt(entityId);
        packet_.pushInt(wx);
        packet_.pushInt(wy);
        packet_.pushInt(wz);
        packet_.pushInt(wyaw);
        packet_.pushInt(0);
        broadcastMove(packet_, except);
    };

    // **A guest who is no longer here steers nothing.** `removePlayer` hands
    // their vehicle back when it can reach the pools; this catches a guest who
    // went while it could not.
    const auto gone = [&](i32 rider) {
        const Player* driver = rider != 0 ? playerByEntity(rider) : nullptr;
        return rider != 0 && (driver == nullptr || !driver->loggedIn);
    };

    for (int i = 0; boats != nullptr && i < boats->count(); ++i) {
        entity::Boat& b = boats->at(i);
        if (!b.alive) {
            continue;
        }
        if (b.entityId == 0) {
            b.entityId = nextEntityId_++;
        }
        if (gone(b.riderId)) {
            boats->takeBack(b.entityId);
        }
        const i32 rider = boats->riddenIndex() == i ? host().entityId : b.riderId;
        // **A boat a guest is steering goes out where that guest last said**,
        // not where the host's copy has walked to: everybody else walks it the
        // same three ticks they walk its rider, and from the same report, so
        // the rider stays in the seat on a third console's screen.
        if (b.remote) {
            visit(true, b.entityId, kVehicleBoat, b.track.x, b.track.y, b.track.z, b.track.yaw,
                  b.timeSinceHit, rider, false);
        } else {
            visit(true, b.entityId, kVehicleBoat, b.x, b.y, b.z, b.yaw, b.timeSinceHit, rider,
                  false);
        }
    }
    for (int i = 0; carts != nullptr && i < carts->count(); ++i) {
        entity::Minecart& c = carts->at(i);
        if (!c.alive) {
            continue;
        }
        if (c.entityId == 0) {
            c.entityId = nextEntityId_++;
        }
        if (gone(c.riderId)) {
            carts->takeBack(c.entityId);
        }
        const i32 rider = carts->riddenIndex() == i ? host().entityId : c.riderId;
        const int type = RemoteEntities::vehicleTypeFor(c.type);
        const bool burning = entity::MinecartSystem::burning(c);
        if (c.remote) {
            visit(false, c.entityId, type, c.track.x, c.track.y, c.track.z, c.track.yaw,
                  c.timeSinceHit, rider, burning);
        } else {
            visit(false, c.entityId, type, c.x, c.y, c.z, c.yaw, c.timeSinceHit, rider, burning);
        }
    }

    // Broken, or gone with a column the host let go of. Whoever was in it is
    // out of it on every screen.
    for (usize i = 0; i < knownVehicles_.size();) {
        if (knownVehicles_[i].seen) {
            ++i;
            continue;
        }
        if (knownVehicles_[i].rider != 0) {
            outs.push_back(Seat{knownVehicles_[i].rider, knownVehicles_[i].entityId});
        }
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(knownVehicles_[i].entityId);
        broadcast(packet_, 0);
        knownVehicles_[i] = knownVehicles_.back();
        knownVehicles_.pop_back();
    }

    // `announce`, so the host's own screen sits its guests down as well.
    for (const Seat& seat : outs) {
        announce(makeUseEntity(seat.rider, kNoVehicle, false), 0);
    }
    for (const Seat& seat : ins) {
        announce(makeUseEntity(seat.rider, seat.vehicle, false), 0);
    }
}

void WorldServer::releaseVehiclesOf(const Player& player)
{
    if (world_ == nullptr || player.entityId == 0) {
        return;
    }
    if (entity::BoatSystem* boats = world_->boats()) {
        for (int i = 0; i < boats->count(); ++i) {
            if ((*boats)[i].riderId == player.entityId) {
                boats->takeBack((*boats)[i].entityId);
            }
        }
    }
    if (entity::MinecartSystem* carts = world_->minecarts()) {
        for (int i = 0; i < carts->count(); ++i) {
            if ((*carts)[i].riderId == player.entityId) {
                carts->takeBack((*carts)[i].entityId);
            }
        }
    }
}

void WorldServer::rideRequest(Player& player, i32 vehicle)
{
    if (!player.loggedIn || !player.placed || player.dead) {
        return;
    }
    if (vehicle == kNoVehicle) {
        releaseVehiclesOf(player);
        return;
    }
    // **Within reach of where they are**, so a stale id or a guest half a world
    // away cannot take a boat out from under somebody's nose. Generous: the
    // guest's own click was within `kEntityReach` of its own copy, which walks
    // three ticks behind the host's.
    constexpr double kRideReach = 8.0;
    const auto inReach = [&](double x, double y, double z) {
        const double dx = x - player.x;
        const double dy = y - player.feetY;
        const double dz = z - player.z;
        return dx * dx + dy * dy + dz * dz <= kRideReach * kRideReach;
    };
    // **Nobody in it already** -- `ridden` is the host's own player or another
    // guest -- which is `EntityBoat.interact`'s one rule, and `oc`'s for a
    // plain cart. A chest or furnace cart is not ridden at all.
    if (entity::BoatSystem* boats = world_->boats()) {
        const int index = boats->indexOfEntity(vehicle);
        if (index >= 0) {
            const entity::Boat& b = (*boats)[index];
            if (!b.ridden && inReach(b.x, b.y, b.z)) {
                releaseVehiclesOf(player);
                boats->driveElsewhere(vehicle, player.entityId);
            }
            return;
        }
    }
    if (entity::MinecartSystem* carts = world_->minecarts()) {
        const int index = carts->indexOfEntity(vehicle);
        if (index >= 0) {
            const entity::Minecart& c = (*carts)[index];
            if (!inReach(c.x, c.y, c.z)) {
                return;
            }
            // **A furnace cart takes the guest's coal and is pointed away from
            // them**, `oc.a(Ldm;)Z`'s third body, run here on the host's cart
            // with what the guest last said it holds. The coal itself is the
            // guest's and their console spends it.
            if (c.type == entity::MinecartType::Furnace) {
                carts->interact(index, item::ItemId(player.heldItem), player.x, player.z);
                return;
            }
            if (!c.ridden && c.type == entity::MinecartType::Rideable) {
                releaseVehiclesOf(player);
                carts->driveElsewhere(vehicle, player.entityId);
            }
        }
    }
}

// ---- the falling blocks and lit TNT -----------------------------------------

void WorldServer::syncBlasts()
{
    entity::MobSystem* mobs = world_->mobs();
    if (mobs == nullptr) {
        return;
    }
    i32 exploded[8];
    const int n = mobs->takeExploded(exploded, 8);
    for (int i = 0; i < n; ++i) {
        broadcast(makeEntityStatus(exploded[i], entity::kStatusExploded), 0);
    }
}

void WorldServer::syncFallers(i64 nowMillis)
{
    entity::FallingBlockSystem* falling = world_->fallingBlocks();
    entity::PrimedTntSystem* tnt = world_->primedTnt();
    if (falling == nullptr && tnt == nullptr) {
        return;
    }

    const bool moveDue = nowMillis - lastFallerMs_ >= kEntityIntervalMs;
    if (moveDue) {
        lastFallerMs_ = nowMillis;
    }
    for (KnownFaller& known : knownFallers_) {
        known.seen = false;
    }

    // `heading` is the yaw byte: nothing for a falling block, and **the fuse
    // for lit TNT**, which has no heading of its own -- see `PrimedTnt::remote`.
    const auto visit = [&](i32 entityId, int type, double x, double y, double z, int heading) {
        const i32 wx = toWirePosition(x);
        const i32 wy = toWirePosition(y);
        const i32 wz = toWirePosition(z);
        const auto teleport = [&](u8 except) {
            packet_.reset(packet::EntityTeleport);
            packet_.pushInt(entityId);
            packet_.pushInt(wx);
            packet_.pushInt(wy);
            packet_.pushInt(wz);
            packet_.pushInt(heading);
            packet_.pushInt(0);
            broadcastMove(packet_, except);
        };
        for (KnownFaller& known : knownFallers_) {
            if (known.entityId != entityId) {
                continue;
            }
            known.seen = true;
            if (!moveDue || (wx == known.x && wy == known.y && wz == known.z)) {
                return;
            }
            known.x = wx;
            known.y = wy;
            known.z = wz;
            teleport(0);
            return;
        }
        knownFallers_.push_back(KnownFaller{entityId, wx, wy, wz, true});
        packet_.reset(packet::VehicleSpawn);
        packet_.pushInt(entityId);
        packet_.pushInt(type);
        packet_.pushInt(wx);
        packet_.pushInt(wy);
        packet_.pushInt(wz);
        broadcast(packet_, 0);
        // The spawn has no angles, so the fuse follows at once.
        teleport(0);
    };

    for (int i = 0; falling != nullptr && i < falling->count(); ++i) {
        entity::FallingBlock& e = falling->at(i);
        const int type = RemoteEntities::objectTypeForFalling(e.block);
        if (!e.alive() || e.remote || type == 0) {
            continue;
        }
        if (e.entityId == 0) {
            e.entityId = nextEntityId_++;
        }
        visit(e.entityId, type, e.x, e.y, e.z, 0);
    }
    for (int i = 0; tnt != nullptr && i < tnt->count(); ++i) {
        entity::PrimedTnt& e = tnt->at(i);
        if (!e.alive() || e.remote) {
            continue;
        }
        if (e.entityId == 0) {
            e.entityId = nextEntityId_++;
        }
        const int fuse = e.fuse < 0 ? 0 : (e.fuse > 127 ? 127 : e.fuse);
        visit(e.entityId, kObjectPrimedTnt, e.x, e.y, e.z, fuse);
    }

    // Landed, given up, or gone off -- which for TNT is the only way out of
    // the pool, so a guest reads this one as the bang.
    for (usize i = 0; i < knownFallers_.size();) {
        if (knownFallers_[i].seen) {
            ++i;
            continue;
        }
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(knownFallers_[i].entityId);
        broadcast(packet_, 0);
        knownFallers_[i] = knownFallers_.back();
        knownFallers_.pop_back();
    }
}

// ---- containers a guest has open -------------------------------------------

void WorldServer::openCart(Player& player, i32 entityId)
{
    entity::MinecartSystem* carts = world_->minecarts();
    const int index = carts != nullptr ? carts->indexOfEntity(entityId) : -1;
    TileSnapshot snapshot;
    bool open = index >= 0 && readCart(*carts, entityId, &snapshot);
    if (open) {
        // The reach a chest gets, measured to the cart's middle.
        const entity::Minecart& c = (*carts)[index];
        const double dx = c.x - player.x;
        const double dy = c.y - player.eyeY;
        const double dz = c.z - player.z;
        open = dx * dx + dy * dy + dz * dz <= 10.0 * 10.0;
    }
    if (!open) {
        write(player, makeTileClose(entityId, kCartTileY, 0));
        return;
    }
    Player::Watch& watch = player.watch;
    watch.x[0] = entityId;
    watch.y[0] = kCartTileY;
    watch.z[0] = 0;
    encodeTile(snapshot, &watch.last[0]);
    watch.parts = 1;
    write(player, makeTileSync(snapshot));
    player.lastTileMs = nowMs_;
}

void WorldServer::openTile(Player& player, i32 x, int y, i32 z)
{
    closeTile(player);
    if (!player.loggedIn || !player.placed || player.dead) {
        return;
    }
    if (y == kCartTileY) {
        openCart(player, x);
        return;
    }
    tick::TickWorld* world = world_->tickWorld();
    if (world == nullptr) {
        return;
    }
    // **Within reach of where they stand**, because a guest that names a chest
    // two hundred blocks away is not clicking one. a1.1.2's own reach is six
    // blocks; a little over, for the lag between the click and this.
    const double dx = double(x) + 0.5 - player.x;
    const double dy = double(y) + 0.5 - player.eyeY;
    const double dz = double(z) + 0.5 - player.z;
    if (dx * dx + dy * dy + dz * dz > 10.0 * 10.0) {
        return;
    }
    tick::ChestPart parts[tick::kMaxChestParts];
    const int count = tileParts(*world, x, y, z, parts);
    if (count == 0) {
        write(player, makeTileClose(x, y, z));
        return;
    }
    Player::Watch& watch = player.watch;
    for (int i = 0; i < count; ++i) {
        TileSnapshot snapshot;
        if (!readTile(*world, parts[i].x, parts[i].y, parts[i].z, &snapshot)) {
            continue;
        }
        watch.x[watch.parts] = parts[i].x;
        watch.y[watch.parts] = parts[i].y;
        watch.z[watch.parts] = parts[i].z;
        encodeTile(snapshot, &watch.last[watch.parts]);
        ++watch.parts;
        write(player, makeTileSync(snapshot));
    }
    player.lastTileMs = nowMs_;
}

void WorldServer::closeTile(Player& player)
{
    Player::Watch& watch = player.watch;
    for (int i = 0; i < watch.parts; ++i) {
        watch.last[i].clear();
    }
    watch.parts = 0;
}

void WorldServer::acceptTile(Player& player, const TileSnapshot& snapshot)
{
    Player::Watch& watch = player.watch;
    // **A chest cart the guest has open**, by its entity id.
    if (snapshot.y == kCartTileY) {
        entity::MinecartSystem* carts = world_->minecarts();
        for (int i = 0; carts != nullptr && i < watch.parts; ++i) {
            if (watch.y[i] != kCartTileY || watch.x[i] != snapshot.x) {
                continue;
            }
            if (writeCart(*carts, snapshot)) {
                TileSnapshot now;
                if (readCart(*carts, snapshot.x, &now)) {
                    encodeTile(now, &watch.last[i]);
                }
            }
            return;
        }
        return;
    }
    tick::TickWorld* world = world_->tickWorld();
    if (world == nullptr) {
        return;
    }
    for (int i = 0; i < watch.parts; ++i) {
        if (watch.x[i] != snapshot.x || watch.y[i] != snapshot.y || watch.z[i] != snapshot.z) {
            continue;
        }
        // **Only a tile the guest has open**, and only its slots: a furnace's
        // burn and cook belong to this world's own tick.
        if (!writeTile(*world, snapshot, false)) {
            return;
        }
        TileSnapshot now;
        if (readTile(*world, snapshot.x, snapshot.y, snapshot.z, &now)) {
            encodeTile(now, &watch.last[i]);
        }
        return;
    }
}

void WorldServer::syncTiles(i64 nowMillis)
{
    tick::TickWorld* world = world_->tickWorld();
    if (world == nullptr) {
        return;
    }
    for (int p = 1; p < kMaxPlayers; ++p) {
        Player& player = players_[p];
        Player::Watch& watch = player.watch;
        if (!player.used || !player.loggedIn || watch.parts == 0) {
            continue;
        }
        // A tenth of a second: a furnace's flame changes every tick and a guest
        // has no use for twenty copies of it.
        if (nowMillis - player.lastTileMs < 100) {
            continue;
        }
        player.lastTileMs = nowMillis;
        std::vector<u8> encoded;
        for (int i = 0; i < watch.parts; ++i) {
            TileSnapshot snapshot;
            entity::MinecartSystem* carts = world_->minecarts();
            const bool read = watch.y[i] == kCartTileY
                                  ? carts != nullptr && readCart(*carts, watch.x[i], &snapshot)
                                  : readTile(*world, watch.x[i], watch.y[i], watch.z[i],
                                             &snapshot);
            if (!read) {
                // The block went under the screen.
                write(player, makeTileClose(watch.x[i], watch.y[i], watch.z[i]));
                closeTile(player);
                break;
            }
            encodeTile(snapshot, &encoded);
            if (encoded == watch.last[i]) {
                continue;
            }
            watch.last[i] = encoded;
            write(player, makeTileSync(snapshot));
        }
    }
}

// ---- the items on the ground -----------------------------------------------

void WorldServer::collectFor(Player& player, entity::ItemEntitySystem& items)
{
    // `EntityPlayer.onUpdate` expands the player's box by one block
    // horizontally before it asks what is in it; the vertical reach is the
    // box's own.
    AABB reach;
    reach.minX = player.x - 0.3 - 1.0;
    reach.maxX = player.x + 0.3 + 1.0;
    reach.minY = player.feetY;
    reach.maxY = player.feetY + 1.8;
    reach.minZ = player.z - 0.3 - 1.0;
    reach.maxZ = player.z + 0.3 + 1.0;

    for (int i = items.count() - 1; i >= 0; --i) {
        const entity::ItemEntity& item = items[i];
        // A stack already flying to somebody (the host's own pickup) is taken.
        if (item.entityId == 0 || item.pickupDelay > 0 || item.count <= 0 || item.collecting()) {
            continue;
        }
        if (item.box.maxX < reach.minX || item.box.minX > reach.maxX
            || item.box.maxY < reach.minY || item.box.minY > reach.maxY
            || item.box.maxZ < reach.minZ || item.box.minZ > reach.maxZ) {
            continue;
        }

        // **Everyone is told it is gone; only the collector is told what it
        // was.** That is `bm` and `ld`, and it is why a client never decides a
        // pickup for itself -- two players reaching for the same stack would
        // both take it.
        const i32 itemId = item.entityId;
        const item::ItemId kind = item.item;
        const int count = item.count;
        const i16 damage = item.damage;

        packet_.reset(packet::Collect);
        packet_.pushInt(itemId);
        packet_.pushInt(player.entityId);
        broadcast(packet_, 0);

        packet_.reset(packet::AddToInventory);
        packet_.pushInt(i64(kind));
        packet_.pushInt(count);
        packet_.pushInt(damage);
        write(player, packet_);

        items.removeById(itemId);
        for (usize k = 0; k < knownItems_.size(); ++k) {
            if (knownItems_[k].entityId == itemId) {
                knownItems_[k] = knownItems_.back();
                knownItems_.pop_back();
                break;
            }
        }
    }
}

void WorldServer::syncItems(i64 nowMillis)
{
    entity::ItemEntitySystem* items = world_->items();
    if (items == nullptr) {
        return;
    }

    // Pick up for the guests first, so a stack taken this frame is never also
    // announced as having moved.
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (player.used && player.loggedIn && player.placed) {
            collectFor(player, *items);
        }
    }

    const bool moveDue = nowMillis - lastItemMs_ >= kEntityIntervalMs;
    if (moveDue) {
        lastItemMs_ = nowMillis;
    }

    for (KnownItem& known : knownItems_) {
        known.seen = false;
    }

    for (int i = 0; i < items->count(); ++i) {
        entity::ItemEntity& item = *items->at(i);
        // **A collected stack is not on the ground.** It is in an inventory and
        // only still drawn; announcing it would hand the guests a second copy.
        if (item.count <= 0 || item.collecting()) {
            continue;
        }

        // **A stack the single-player game spawned has no id**, because until
        // this moment nothing needed one. See `ItemEntity::entityId`.
        const bool fresh = item.entityId == 0;
        if (fresh) {
            item.entityId = nextEntityId_++;
        }

        const i32 wx = toWirePosition(item.x);
        const i32 wy = toWirePosition(item.y);
        const i32 wz = toWirePosition(item.z);

        KnownItem* known = nullptr;
        for (KnownItem& candidate : knownItems_) {
            if (candidate.entityId == item.entityId) {
                known = &candidate;
                break;
            }
        }

        if (known == nullptr) {
            knownItems_.push_back(KnownItem{item.entityId, wx, wy, wz, true});
            packet_ = pickupSpawnFor(item);
            packet_.ints[0] = item.entityId;
            broadcast(packet_, 0);
            ++itemsSpawned_;
            continue;
        }

        known->seen = true;
        if (!moveDue || (wx == known->x && wy == known->y && wz == known->z)) {
            continue;
        }
        known->x = wx;
        known->y = wy;
        known->z = wz;
        packet_.reset(packet::EntityTeleport);
        packet_.pushInt(item.entityId);
        packet_.pushInt(wx);
        packet_.pushInt(wy);
        packet_.pushInt(wz);
        packet_.pushInt(0);
        packet_.pushInt(0);
        broadcastMove(packet_, 0);
    }

    // What the pool no longer holds: aged out, blown up, or picked up by the
    // host's own player.
    for (usize i = 0; i < knownItems_.size();) {
        if (knownItems_[i].seen) {
            ++i;
            continue;
        }
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(knownItems_[i].entityId);
        broadcast(packet_, 0);
        knownItems_[i] = knownItems_.back();
        knownItems_.pop_back();
    }
}

// ---- the arrows ------------------------------------------------------------

void WorldServer::syncMobs(i64 nowMillis)
{
    entity::MobSystem* mobs = world_->mobs();
    if (mobs == nullptr) {
        return;
    }

    const bool moveDue = nowMillis - lastMobMs_ >= kEntityIntervalMs;
    if (moveDue) {
        lastMobMs_ = nowMillis;
    }

    for (KnownMob& known : knownMobs_) {
        known.seen = false;
    }

    for (int i = 0; i < mobs->count(); ++i) {
        entity::Mob& mob = mobs->at(i);
        if (!mob.alive || mob.remote) {
            continue;  // a remote mob on a host would be somebody else's
        }
        if (mob.entityId == 0) {
            mob.entityId = nextEntityId_++;
        }

        // **The feet**, which is what `MobSystem::placeById` writes back on the
        // other side: a mob's `yOffset` is zero in a1.1.2, so `body.y` is
        // already the bottom of the box.
        const i32 wx = toWirePosition(mob.body.x);
        const i32 wy = toWirePosition(mob.body.y);
        const i32 wz = toWirePosition(mob.body.z);
        const i8 wyaw = toWireAngle(mob.yaw);
        const i8 wpitch = toWireAngle(mob.pitch);

        KnownMob* known = nullptr;
        for (KnownMob& candidate : knownMobs_) {
            if (candidate.entityId == mob.entityId) {
                known = &candidate;
                break;
            }
        }

        if (known == nullptr) {
            KnownMob fresh{mob.entityId, wx, wy, wz, wyaw, wpitch, true};
            // Hits from before a guest ever saw it are not news.
            fresh.hurtSerial = mob.hurtSerial;
            fresh.deathSent = mob.health <= 0;
            knownMobs_.push_back(fresh);
            packet_.reset(packet::MobSpawn);
            packet_.pushInt(mob.entityId);
            packet_.pushInt(RemoteEntities::wireTypeForMob(mob));
            packet_.pushInt(wx);
            packet_.pushInt(wy);
            packet_.pushInt(wz);
            packet_.pushInt(wyaw);
            packet_.pushInt(wpitch);
            broadcast(packet_, 0);
            ++mobsSpawned_;
            // A shorn sheep or a saddled pig is spawned plain; say otherwise.
            if (mob.flag) {
                knownMobs_.back().flag = true;
                broadcast(makeEntityStatus(mob.entityId, entity::kStatusFlagSet), 0);
            }
            continue;
        }

        known->seen = true;

        // **A hit and a death, as they happen and not on the move clock.** The
        // tint is ten ticks long, so waiting for the next move would cost half
        // of it. A hit first, then the death, which is the order a1.2's server
        // sends them in when one blow does both.
        if (mob.hurtSerial != known->hurtSerial) {
            known->hurtSerial = mob.hurtSerial;
            broadcast(makeEntityStatus(mob.entityId, entity::kStatusHurt), 0);
        }
        if (mob.flag != known->flag) {
            known->flag = mob.flag;
            broadcast(makeEntityStatus(mob.entityId, mob.flag ? entity::kStatusFlagSet
                                                              : entity::kStatusFlagClear),
                      0);
        }
        if (mob.health <= 0 && !known->deathSent) {
            known->deathSent = true;
            broadcast(makeEntityStatus(mob.entityId, entity::kStatusDead), 0);
        }

        if (!moveDue
            || (wx == known->x && wy == known->y && wz == known->z && wyaw == known->yaw
                && wpitch == known->pitch)) {
            continue;
        }
        known->x = wx;
        known->y = wy;
        known->z = wz;
        known->yaw = wyaw;
        known->pitch = wpitch;
        packet_.reset(packet::EntityTeleport);
        packet_.pushInt(mob.entityId);
        packet_.pushInt(wx);
        packet_.pushInt(wy);
        packet_.pushInt(wz);
        packet_.pushInt(wyaw);
        packet_.pushInt(wpitch);
        broadcastMove(packet_, 0);
    }

    // Killed, despawned, or driven off the end of the pool.
    for (usize i = 0; i < knownMobs_.size();) {
        if (knownMobs_[i].seen) {
            ++i;
            continue;
        }
        packet_.reset(packet::DestroyEntity);
        packet_.pushInt(knownMobs_[i].entityId);
        broadcast(packet_, 0);
        knownMobs_[i] = knownMobs_.back();
        knownMobs_.pop_back();
    }
}

// ---- the paintings ---------------------------------------------------------

// ---- the guests' packets ---------------------------------------------------

void WorldServer::feed(u8 playerId, const u8* data, usize size)
{
    Player* player = find(playerId);
    if (player == nullptr || size == 0) {
        return;
    }
    if (player->inStart > 0) {
        player->in.erase(player->in.begin(), player->in.begin() + std::ptrdiff_t(player->inStart));
        player->inStart = 0;
    }
    if (player->in.size() + size > kMaxInboundBytes) {
        player->in.clear();
        return;
    }
    player->in.insert(player->in.end(), data, data + size);

    for (;;) {
        usize consumed = 0;
        const ParseResult result = parsePacket(player->in.data() + player->inStart,
                                               player->in.size() - player->inStart,
                                               &player->parsed, &consumed);
        if (result != ParseResult::Ok) {
            if (result != ParseResult::NeedMore) {
                // Nothing can be found in the stream after this, so it is
                // dropped rather than guessed at. The link is still up and the
                // guest's next packet starts a fresh buffer.
                player->in.clear();
                player->inStart = 0;
            }
            return;
        }
        player->inStart += consumed;
        onPacket(*player, player->parsed);
    }
}

void WorldServer::onPacket(Player& player, const Packet& p)
{
    switch (p.id) {
    case packet::KeepAlive:
        break;

    case packet::Flying:
        break;

    case packet::PlayerPosition:
    case packet::PlayerPositionLook: {
        // Towards the server the order is x, feet, eye, z.
        player.x = p.real(0);
        player.feetY = p.real(1);
        player.eyeY = p.real(2);
        player.z = p.real(3);
        if (p.id == packet::PlayerPositionLook) {
            player.yaw = float(p.real(4));
            player.pitch = float(p.real(5));
        }
        const i32 centreX = i32(std::floor(player.x / 16.0));
        const i32 centreZ = i32(std::floor(player.z / 16.0));
        if (!player.viewSet || centreX != player.viewX || centreZ != player.viewZ) {
            rebuildView(player);
        }
        break;
    }

    case packet::PlayerLook:
        player.yaw = float(p.real(0));
        player.pitch = float(p.real(1));
        break;

    case packet::PlayerSkin: {
        // **Theirs, whatever id it says**: the entity is the one this guest is,
        // so a guest cannot dress somebody else. Once per connection, and
        // sanitised before anything is kept or passed on.
        if (player.skinSeen) {
            break;
        }
        acceptSkin(player, p);
        break;
    }

    case packet::TileOpen:
        openTile(player, i32(p.integer(0)), int(p.integer(1)), i32(p.integer(2)));
        break;

    case packet::TileClose:
        closeTile(player);
        break;

    case packet::TileSync: {
        TileSnapshot snapshot;
        if (readTileSync(p, &snapshot)) {
            acceptTile(player, snapshot);
        }
        break;
    }

    case packet::BlockItemSwitch:
        player.heldItem = i16(p.integer(1));
        packet_.reset(packet::BlockItemSwitch);
        packet_.pushInt(player.entityId);
        packet_.pushInt(player.heldItem);
        announce(packet_, player.id);
        break;

    case packet::PlayerInventory: {
        // **a1.1.2's multiplayer inventory is the client's and the server
        // adopts it.** Every twentieth tick the client compares its inventory
        // with a snapshot and, if anything differs, sends all three arrays;
        // 0.2.1 writes them straight into the player. So does this -- into the
        // record that outlives their visit, which is the whole reason the host
        // holds one. See core/net/player_sync.hpp.
        //
        // **What they have on is the one part anybody else can see**, and this
        // is the only place the host hears it. See `isWornReport`.
        const i64 type = p.integer(0);
        if (type == kInventoryArmour && adoptWorn(player, p.stacks.data(), p.stacks.size())
            && player.spawned) {
            announce(makeWorn(player.entityId, player.worn), player.id);
        }

        SavedPlayer* saved = savedFor(player.name);
        if (saved == nullptr && !player.name.empty()) {
            saved_.push_back(SavedPlayer{});
            saved = &saved_.back();
            saved->name = player.name;
        }
        if (saved == nullptr) {
            break;
        }
        if (type == kInventoryMain) {
            saved->main = p.stacks;
            saved->haveInventory = true;
        } else if (type == kInventoryArmour) {
            saved->armour = p.stacks;
            saved->haveInventory = true;
        }
        // The crafting grid does not persist here, exactly as it does not
        // persist in the original.
        break;
    }

    case packet::BlockDig: {
        // Status 3 is "the block gave": a1.1.2's client breaks it locally and
        // waits for the server's echo to make that stick. Everything else is
        // progress, and the host does not count digs for anybody.
        if (p.integer(0) != 3) {
            break;
        }
        world_->breakBlock(i32(p.integer(1)), int(p.integer(2)), i32(p.integer(3)),
                           int(player.heldItem));
        break;
    }

    case packet::Place: {
        // **A right click in the air**, which only a guest of this family
        // sends. See `makeUseItem`: the arrow is fired here, from where they
        // stand and the way they face, and it is theirs.
        if (isUseItem(p)) {
            if (player.dead || !player.placed) {
                break;
            }
            ServerWorld::UseRequest request;
            request.item = int(p.integer(0));
            request.shooterEntityId = player.entityId;
            request.eyeX = player.x;
            request.eyeY = player.eyeY;
            request.eyeZ = player.z;
            request.yawDegrees = player.yaw;
            request.pitchDegrees = player.pitch;
            world_->useItem(request);
            break;
        }
        // **An empty hand is -1 and still reaches the block.** `activeBlockOrUseItem`
        // asks `Block.blockActivated` before it looks at the stack at all, which
        // is why a bare hand opens a chest, flips a lever and lights redstone
        // ore against a real server -- and why dropping the packet here left a
        // guest unable to do any of the three. `item::rightClick` takes 0 for
        // "nothing held" and already branches on it everywhere, so -1 becomes 0
        // and the request goes through.
        const int held = int(p.integer(0));
        const int item = held < 0 ? 0 : held;
        ServerWorld::PlaceRequest request;
        request.x = i32(p.integer(1));
        request.y = int(p.integer(2));
        request.z = i32(p.integer(3));
        request.face = int(p.integer(4));
        request.item = item;
        request.playerX = player.x;
        request.playerFeetY = player.feetY;
        request.playerZ = player.z;
        request.yawDegrees = player.yaw;
        world_->placeBlock(request);
        break;
    }

    case packet::UseEntity: {
        // Field 0 is the attacker's own id, which this side does not need --
        // the frame it came in on already said who it was. **A right click is
        // "let me in"**, or with `kNoVehicle` "I got out" -- see `kNoVehicle`.
        if (p.integer(2) == 0) {
            rideRequest(player, i32(p.integer(1)));
            break;
        }
        ServerWorld::AttackRequest request;
        request.entityId = i32(p.integer(1));
        request.item = int(player.heldItem);
        request.playerX = player.x;
        request.playerZ = player.z;

        // **A player is not an animal and is not resolved here.** Every other
        // entity in this world is the host's to run, so the host works out
        // what a blow costs it. A player's health belongs to the console that
        // player is sitting at -- there is no health on this wire and there
        // never was -- so the blow is forwarded and *they* work it out. See
        // `net::IncomingHit`.
        if (Player* victim = playerByEntity(request.entityId)) {
            if (victim->id == players_[0].id) {
                world_->attackHost(request);
                broadcast(makeEntityStatus(victim->entityId, entity::kStatusHurt), 0);
            } else if (victim->loggedIn) {
                write(*victim, makeUseEntity(player.entityId, victim->entityId, true));
                announce(makeEntityStatus(victim->entityId, entity::kStatusHurt), victim->id);
            }
            break;
        }
        world_->attackEntity(request);
        break;
    }

    case packet::EntityTeleport: {
        // **Where the boat or cart this guest is steering went**, which only
        // that guest knows: its console runs the vehicle its player is in.
        // From anybody else, or about anything else, it is nothing.
        const i32 id = i32(p.integer(0));
        const double x = double(p.integer(1)) / kPositionUnitsPerBlock;
        const double y = double(p.integer(2)) / kPositionUnitsPerBlock;
        const double z = double(p.integer(3)) / kPositionUnitsPerBlock;
        const float yaw = float(p.integer(4)) * 360.0f / kRotationUnitsPerTurn;
        if (entity::BoatSystem* boats = world_->boats()) {
            const int index = boats->indexOfEntity(id);
            if (index >= 0 && (*boats)[index].riderId == player.entityId) {
                boats->placeRemote(id, x, y, z, yaw);
                break;
            }
        }
        if (entity::MinecartSystem* carts = world_->minecarts()) {
            const int index = carts->indexOfEntity(id);
            if (index >= 0 && (*carts)[index].riderId == player.entityId) {
                carts->placeRemote(id, x, y, z, yaw);
            }
        }
        break;
    }

    case packet::DestroyEntity: {
        // **The vehicle this guest was steering broke on their side** -- a wall,
        // a cactus, a fire. A guest's world drops nothing (every drop is the
        // host's), so the host's copy breaks here, where it walked to, and
        // leaves the planks or the cart in this world.
        const i32 id = i32(p.integer(0));
        tick::TickWorld* ticks = world_->tickWorld();
        if (entity::BoatSystem* boats = world_->boats()) {
            const int index = boats->indexOfEntity(id);
            if (index >= 0 && (*boats)[index].riderId == player.entityId) {
                if (ticks == nullptr || !boats->breakById(*ticks, id)) {
                    boats->removeById(id);
                }
                break;
            }
        }
        if (entity::MinecartSystem* carts = world_->minecarts()) {
            const int index = carts->indexOfEntity(id);
            if (index >= 0 && (*carts)[index].riderId == player.entityId) {
                if (ticks == nullptr || !carts->breakById(*ticks, id)) {
                    carts->removeById(id);
                }
            }
        }
        break;
    }

    case packet::EntityAction: {
        // Ours; see `packet::EntityAction`. Field 0 is the guest's own id, and
        // the frame already said who it was. A corpse does not crouch.
        const int action = int(p.integer(1));
        if (player.dead || (action != kActionCrouch && action != kActionUncrouch)) {
            break;
        }
        const bool sneaking = action == kActionCrouch;
        if (sneaking != player.sneaking) {
            player.sneaking = sneaking;
            announce(makeEntityAction(player.entityId, action), player.id);
        }
        break;
    }

    case packet::EntityStatus:
        // **A guest saying it died**, which only it can know: its health is its
        // own. Anything but a death, or a death about somebody else, is not a
        // guest's to say.
        if (int(p.integer(1)) == entity::kStatusDead && i32(p.integer(0)) == player.entityId
            && !player.dead) {
            player.dead = true;
            player.sneaking = false;
            announce(makeEntityStatus(player.entityId, entity::kStatusDead), player.id);
        }
        break;

    case packet::Respawn:
        // **Back from the dead**, and the others are given the player afresh
        // -- where they are now, standing -- because the body they were drawing
        // fell over and went. The guest's position report goes ahead of this
        // one in the same tick, so "now" is the spawn point.
        if (player.dead) {
            player.dead = false;
            player.sneaking = false;
            fillNamedSpawn(player);
            announce(packet_, player.id);
            if (wearsAnything(player.worn)) {
                announce(makeWorn(player.entityId, player.worn), player.id);
            }
        }
        break;

    case packet::ArmAnimation: {
        packet_.reset(packet::ArmAnimation);
        packet_.pushInt(player.entityId);
        packet_.pushInt(1);
        announce(packet_, player.id);
        break;
    }

    case packet::Chat: {
        const std::string line = "<" + player.name + "> " + p.text(0);
        world_->showChat(line);
        packet_.reset(packet::Chat);
        packet_.pushString(line);
        broadcast(packet_, player.id);
        break;
    }

    case packet::PickupSpawn: {
        // `ha(dx)`: a stack the guest threw. It has already let go of it --
        // `la.a(dx)` sends and forgets -- so if this does not spawn it, it is
        // gone. The id in the packet is the guest's own and is not used: this
        // side gives it one when it announces it.
        if (p.integer(1) < 0 || p.integer(2) <= 0) {
            break;
        }
        world_->spawnItem(double(p.integer(3)) / 32.0, double(p.integer(4)) / 32.0,
                          double(p.integer(5)) / 32.0, item::ItemId(p.integer(1)),
                          int(p.integer(2)), double(p.integer(6)) / 128.0,
                          double(p.integer(7)) / 128.0, double(p.integer(8)) / 128.0);
        break;
    }

    case packet::KickDisconnect:
        // The link's own Bye is what actually ends a session; this is only the
        // client being polite on its way out.
        break;

    default:
        // Inventory, pickups and the rest of what a client may say: parsed,
        // and not acted on yet. See docs/current-work.md.
        break;
    }
}

// ---- the frame -------------------------------------------------------------

void WorldServer::pump(i64 nowMillis)
{
    if (world_ == nullptr) {
        return;
    }
    nowMs_ = nowMillis;

    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (player.used && !player.loggedIn) {
            login(player);
        }
    }

    syncBlasts();
    syncFallers(nowMillis);
    sendBlockChanges();
    streamColumns();
    takeOven();
    placeStragglers();
    // A guest that was sent no positions while it was behind, and has caught
    // up, is given every current one before anything newer is written.
    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (player.used && player.loggedIn && player.movesOwed
            && player.outbox.size() < kMoveLowWater) {
            resendPositions(player);
        }
    }
    sendEntities(nowMillis);
    syncItems(nowMillis);
    syncArrows(nowMillis);
    syncMobs(nowMillis);
    syncVehicles(nowMillis);
    syncPaintings();
    syncTiles(nowMillis);

    if (nowMillis - lastTimeMs_ >= kTimeIntervalMs) {
        lastTimeMs_ = nowMillis;
        packet_.reset(packet::TimeUpdate);
        packet_.pushInt(world_->timeTicks());
        broadcast(packet_, 0);
    }

    for (int i = 1; i < kMaxPlayers; ++i) {
        Player& player = players_[i];
        if (player.used && !player.outbox.empty()) {
            flush(player);
        }
    }
}

bool WorldServer::starved(u8 playerId) const
{
    const Player* player = find(playerId);
    return player != nullptr && player->starved;
}

u32 WorldServer::columnsQueued() const
{
    u32 n = 0;
    for (int i = 1; i < kMaxPlayers; ++i) {
        if (players_[i].used) {
            n += u32(players_[i].owed.size());
        }
    }
    return n;
}

usize WorldServer::outboundBytes() const
{
    usize n = 0;
    for (int i = 1; i < kMaxPlayers; ++i) {
        if (players_[i].used) {
            n += players_[i].outbox.size();
        }
    }
    return n;
}

}  // namespace mc::net
