#include "platform/ctr/host_play.hpp"

#include "core/entity/ray_trace.hpp"
#include "platform/ctr/renderer.hpp"

#include <3ds.h>

#include <vector>

#include <cmath>

namespace mc::ctr {

namespace {

// a1.1.2's own unit for an absolute position on the wire: sixteenths of a
// block in the protocol's integers, thirty-seconds in `MathHelper.floor_double
// (x * 32)`. The link uses the finer one -- it costs nothing and a player
// walking slowly should not arrive in steps.
i32 fixed(double value)
{
    return i32(std::floor(value * 32.0));
}

// A whole turn in a byte, which is how every angle on the wire travels. The
// camera keeps radians, so this is the only place the two meet.
i8 turn(float radians)
{
    constexpr float kTau = 6.28318530717958647692f;
    return i8(int(radians * 256.0f / kTau) & 0xFF);
}

}  // namespace

HostPlay::HostPlay() = default;

HostPlay::~HostPlay()
{
    close("the host left the world");
}

bool HostPlay::open(const std::string& worldName, const std::string& hostName,
                    const net::link::GeneratorId& world, std::string* error)
{
    if (!startLocalWireless(error)) {
        return false;
    }
    if (!link_.host(worldName, hostName, LocalKind::Session, error)) {
        return false;
    }
    session_.open(worldName, hostName, world, u32(osGetTime()), this);
    hostName_ = hostName;
    server_.open(this, &HostPlay::sendToGuest, this, hostName);
    // **Nothing in the session describes a guest to this console except this.**
    // See `WorldServer::setLocalSink`. Left unbound to any item or mob pool on
    // purpose: those are already this world's own, and a second copy of each
    // would be drawn on top of the first.
    entities_.clear();
    server_.setLocalSink(&HostPlay::applyLocally, this);
    // See `ctr::linkPausing`: every keyboard in the build tells the session
    // before it hands the console to an applet.
    setLinkPauseHook(&HostPlay::linkPaused, this);
    advertised_ = -1;
    everOpened_ = true;
    return true;
}

void HostPlay::blockWatcher(void* context, i32 x, int y, i32 z)
{
    static_cast<HostPlay*>(context)->server_.blockChanged(x, y, z);
}

bool HostPlay::sendToGuest(void* context, u8 playerId, const u8* data, usize size)
{
    auto* self = static_cast<HostPlay*>(context);
    return self->session_.sendGamePacket(playerId, data, size);
}

void HostPlay::linkPaused(void* context, u32 expectedMs)
{
    auto* self = static_cast<HostPlay*>(context);
    if (self->link_.active()) {
        self->session_.announceAway(expectedMs, u32(osGetTime()), self->link_);
    }
}

void HostPlay::applyLocally(void* context, const net::Packet& packet)
{
    auto* self = static_cast<HostPlay*>(context);
    // `streamer_` is set for the length of `pump`, which is the only thing
    // that can reach this. A packet outside one has nowhere to put a body.
    tick::TickWorld* world = self->streamer_ != nullptr ? self->streamer_->worldTick() : nullptr;
    self->entities_.apply(packet, world);
}

render::WorldStreamer::TerrainSource HostPlay::terrainSource()
{
    render::WorldStreamer::TerrainSource source;
    source.context = &pool_;
    source.supply = &net::link::TerrainPool::supply;
    return source;
}

void HostPlay::close(const std::string& reason)
{
    if (!link_.active()) {
        return;
    }
    session_.close(reason, u32(osGetTime()), link_);
    link_.leave();
    entities_.clear();
    setLinkPauseHook(nullptr, nullptr);
    // **After the link, not before.** `close` on the session puts a Bye on the
    // wire for every guest, and the server's own thread has nothing to do with
    // that -- but stopping it first would leave `pump` able to run against a
    // stopped oven if anything called it in between.
    server_.close();
}

void HostPlay::pump(render::WorldStreamer& world, render::ChunkRenderer& chunks,
                    const item::Effects& effects, entity::ItemEntitySystem& drops,
                    gui::ChatLog& chat, const u8* fontWidths)
{
    if (!link_.active()) {
        return;
    }
    chat_ = &chat;
    fontWidths_ = fontWidths;
    streamer_ = &world;
    chunks_ = &chunks;
    effects_ = &effects;
    drops_ = &drops;

    // **Reading the radio comes first**, because everything below is an answer
    // to it: a guest's move decides which columns it is owed, and a guest's
    // dig is a block this frame has to write before anything draws.
    session_.pump(u32(osGetTime()), link_);

    // **And then the world is told where everybody is.** Between this and
    // `server_.pump` below, the streamer is holding the ground under every
    // guest as well as the ground under this console's camera -- so the answer
    // `ServerWorld::column` gives a moment from now is about the world as a
    // whole rather than about what happens to be on screen. See
    // `WorldStreamer::setServedAreas`; before it, a guest who walked past the
    // edge of the host's render distance simply stopped receiving ground.
    //
    // Set every frame, cheaply: it is at most four small integers, and a guest
    // whose Position & Look has not come back yet is not in the list, so
    // nothing is loaded around a player who has not said where they are.
    {
        net::WorldServer::ViewCentre centres[render::WorldStreamer::kMaxServedAreas];
        const int count = server_.viewCentres(
            centres, render::WorldStreamer::kMaxServedAreas);
        render::WorldStreamer::ServedArea areas[render::WorldStreamer::kMaxServedAreas];
        for (int i = 0; i < count; ++i) {
            areas[i].chunkX = centres[i].chunkX;
            areas[i].chunkZ = centres[i].chunkZ;
            areas[i].radius = net::WorldServer::kServedRadius;
        }
        world.setServedAreas(areas, count);
    }

    // ...and then the world goes out. `pump` is where every packet to a guest
    // is decided, and it is bracketed by the same borrowed pointers, because
    // a Block Change has to read the block the edit above it just made.
    server_.pump(i64(osGetTime()));

    // **The one thing about hosting a player cannot work out for themselves.**
    // A guest standing where this console has no column yet gets nothing, which
    // on their screen is a wall of nothing and no reason for it. Said once on
    // each change, not once a frame.
    //
    // **It is a wait now and not a wall.** Before `setServedAreas` this meant a
    // guest had walked past the edge of the host's render distance and was
    // never getting ground again; now their ground is being read or generated
    // like anybody else's and the state clears itself, so the words say
    // "loading" rather than "gone".
    const bool starved = starvedGuest();
    if (starved != wasStarved_) {
        wasStarved_ = starved;
        if (chat_ != nullptr) {
            chat_->add(fontWidths_,
                       starved ? "loading the ground somebody else is standing on"
                               : "everybody has ground under them");
        }
    }

    // **And the other half of it**: somebody who should have arrived somewhere
    // this console has no world was put down here instead. Said once, because
    // the player it happened to cannot see why they are not where they left.
    bool displaced = false;
    for (int id = net::link::kHostPlayerId + 1;
         id <= net::link::kHostPlayerId + net::link::kMaxGuests; ++id) {
        if (server_.displaced(u8(id))) {
            displaced = true;
            break;
        }
    }
    if (displaced != wasDisplaced_) {
        wasDisplaced_ = displaced;
        if (displaced && chat_ != nullptr) {
            chat_->add(fontWidths_,
                       "somebody was put beside you: their own spot is not loaded here");
        }
    }

    streamer_ = nullptr;
    chunks_ = nullptr;
    effects_ = nullptr;
    drops_ = nullptr;
    chat_ = nullptr;

    // Whatever the pool wants, to whichever console can answer soonest. The
    // requests go out with the next datagram rather than immediately, which is
    // the right cost for something nothing is waiting on.
    if (session_.anyGenerator()) {
        i32 x = 0;
        i32 z = 0;
        std::vector<u8> body;
        while (pool_.nextRequest(&x, &z)) {
            body.clear();
            net::link::encodeTerrainRequest(x, z, &body);
            if (!session_.requestTerrain(body.data(), body.size())) {
                break;
            }
        }
    }

    // The beacon says how full the session is, so a console scanning from the
    // next room sees the truth rather than the moment the world was opened.
    // Only when it changed: rewriting it is a service call.
    const int guests = session_.guestCount();
    if (guests != advertised_) {
        advertised_ = guests;
        link_.advertise(guests);
    }
}

void HostPlay::reportPose(const entity::PlayerBody& body, const Camera& camera)
{
    if (!link_.active()) {
        return;
    }

    // **The server is told every frame, and unconditionally.** It is four
    // stores, and it decides where the *next* guest lands -- so it has to be
    // true before anybody has joined, which is exactly when the throttle and
    // the guest count below would have skipped it.
    constexpr float kDegreesPerRadian = 180.0f / 3.14159265f;
    server_.setHostPose(body.x, body.box.minY, body.z, camera.yaw * kDegreesPerRadian,
                        camera.pitch * kDegreesPerRadian);

    if (session_.guestCount() == 0) {
        return;
    }
    // **Twenty a second, not sixty.** This is called once a frame, but a1.1.2
    // reports a position once a *tick* -- `la.e_()` runs `J()` from the tick,
    // not the render -- and a console's radio has better things to do than
    // carry three copies of the same walk. Fifty milliseconds is that tick.
    constexpr u32 kPoseIntervalMs = 50;
    const u32 now = u32(osGetTime());
    if (posedAtMs_ != 0 && u32(now - posedAtMs_) < kPoseIntervalMs) {
        return;
    }
    posedAtMs_ = now;

    net::link::Pose pose;
    pose.tick = tick_++;
    pose.x = fixed(body.x);
    pose.y = fixed(body.y);
    pose.z = fixed(body.z);
    pose.yaw = turn(camera.yaw);
    pose.pitch = turn(camera.pitch);
    pose.flags = u8((body.onGround ? 0x01 : 0x00) | (body.sneaking ? 0x02 : 0x00));
    session_.reportPose(pose);
}

void HostPlay::say(const std::string& text)
{
    if (link_.active()) {
        // **Both roads, because the guests are on two different ones.** A
        // console still in the lobby only reads the session's own chat; one
        // that is playing reads the protocol-2 stream. Sending on both is two
        // dozen bytes and means a line is never lost to whichever screen the
        // other player happens to be on.
        session_.say(text);
        server_.say(text);
    }
}

void HostPlay::onPlayerJoined(u8 playerId, const std::string& name)
{
    server_.addPlayer(playerId, name);
    if (chat_ != nullptr) {
        chat_->add(fontWidths_, name + " joined the session");
    }
}

void HostPlay::onPose(const net::link::Pose& pose)
{
    wantAround(pose);
}

void HostPlay::onPlayerLeft(u8 playerId, const std::string& reason)
{
    server_.removePlayer(playerId);
    // **Anything still owed is given up, whoever left.** The pool does not
    // record which console a request went to, and a slot waiting on a console
    // that has gone would never complete; re-asking a few columns costs
    // nothing next to a pool that slowly fills with them.
    pool_.abandon();
    if (chat_ != nullptr) {
        chat_->add(fontWidths_, reason.empty() ? std::string("somebody left the session")
                                               : "somebody left: " + reason);
    }
}

void HostPlay::onTerrainPart(u8 playerId, const u8* body, usize size)
{
    (void)playerId;
    pool_.onPart(body, size);
}

void HostPlay::onGamePacket(u8 playerId, const u8* data, usize size)
{
    // The guest's own stream, in order. What it says is the server's business,
    // not the session's -- see the note at the top of core/net/session.hpp.
    server_.feed(playerId, data, size);
}

// ---- the world, as the server reads and writes it --------------------------

const world::ChunkColumn* HostPlay::column(i32 chunkX, i32 chunkZ) const
{
    if (streamer_ == nullptr) {
        return nullptr;
    }
    // **The grid first and then the ground held for the guests.** The two are
    // one world and the question here is only "does this process have it" --
    // whether it is also on this console's screen is nothing to do with the
    // column a guest is owed. See `WorldStreamer::setServedAreas`.
    if (const world::ChunkColumn* resident = streamer_->residentColumn(chunkX, chunkZ)) {
        return resident;
    }
    return streamer_->servedColumn(chunkX, chunkZ);
}

i64 HostPlay::timeTicks() const
{
    if (streamer_ == nullptr) {
        return 0;
    }
    const tick::TickWorld* world = streamer_->worldTick();
    return world != nullptr ? world->time() : streamer_->level().time;
}

void HostPlay::spawnPoint(i32* x, i32* y, i32* z) const
{
    if (streamer_ == nullptr) {
        *x = 0;
        *y = 64;
        *z = 0;
        return;
    }
    *x = streamer_->level().spawnX;
    *y = streamer_->level().spawnY;
    *z = streamer_->level().spawnZ;
}

void HostPlay::attackHost(const AttackRequest& request)
{
    hit_.present = true;
    hit_.item = request.item;
    hit_.fromX = request.playerX;
    hit_.fromZ = request.playerZ;
}

bool HostPlay::takeHit(net::IncomingHit* out)
{
    if (!hit_.present) {
        return false;
    }
    *out = hit_;
    hit_ = net::IncomingHit{};
    return true;
}

double HostPlay::settleFeet(double x, double feetY, double z) const
{
    tick::TickWorld* world = streamer_ != nullptr ? streamer_->worldTick() : nullptr;
    if (world == nullptr) {
        return feetY;
    }
    // **A borrowed body, only to be walked upward.** `liftOutOfGround` is a
    // method on the player's own body because that is the only thing in the
    // original that is ever lifted; what it actually needs is a box and a
    // world, and standing a default one at the guest's feet gives it both
    // without this having to know anything else about them.
    //
    // The whole height of the world rather than `liftOutOfGround`'s default
    // eight, for the reason `liftIntoTheWorld` in main.cpp gives: a spawn
    // point that predates the search in core/world/spawn_point.hpp sits at
    // y = 64 with whatever has grown over it since.
    entity::PlayerBody body;
    body.setFeet(x, feetY, z);
    body.liftOutOfGround(*world, 128);
    return body.box.minY;
}

void HostPlay::breakBlock(i32 x, int y, i32 z, int heldItem)
{
    if (streamer_ == nullptr || chunks_ == nullptr) {
        return;
    }
    // **The same break the host's own left click makes.** The drop, the
    // particles, the sound, the neighbour updates and the relighting are all
    // downstream of it, so a guest breaking a block is a block broken in this
    // world rather than a second implementation of breaking one.
    //
    // **With the drop**, which it did without until a guest was watched mining:
    // the guest's own side has no stack sink at all -- items on the ground
    // belong to the host and are streamed back -- so a break that dropped
    // nothing here dropped nothing anywhere. See
    // `item::harvestBlockFor`.
    streamer_->harvestBlock(*chunks_, x, y, z, item::ItemId(heldItem),
                            effects_ != nullptr ? *effects_ : item::Effects{});
}

void HostPlay::placeBlock(const PlaceRequest& request)
{
    if (streamer_ == nullptr || chunks_ == nullptr) {
        return;
    }
    if (request.face < 0 || request.face >= mesh::kFaceCount) {
        return;
    }

    entity::RayHit hit;
    hit.hit = true;
    hit.x = request.x;
    hit.y = request.y;
    hit.z = request.z;
    hit.face = mesh::Face(request.face);
    // The middle of the struck face. The wire carries no hit point -- `do/fe`
    // has nowhere to put one -- and the middle is the only answer that cannot
    // be wrong about which half of a block was clicked.
    const mesh::FaceOffset& normal = mesh::kFaceOffset[hit.face];
    hit.hitX = double(request.x) + 0.5 + double(normal.dx) * 0.5;
    hit.hitY = double(request.y) + 0.5 + double(normal.dy) * 0.5;
    hit.hitZ = double(request.z) + 0.5 + double(normal.dz) * 0.5;

    // Where the guest is standing, so `item::rightClick` refuses a block that
    // would be placed inside them -- the check their own client already made,
    // made again here because this side is the one that decides.
    AABB box;
    box.minX = request.playerX - 0.3;
    box.maxX = request.playerX + 0.3;
    box.minY = request.playerFeetY;
    box.maxY = request.playerFeetY + 1.8;
    box.minZ = request.playerZ - 0.3;
    box.maxZ = request.playerZ + 0.3;

    bool itemTook = false;
    streamer_->rightClick(*chunks_, item::ItemId(request.item), hit, box, request.yawDegrees,
                          effects_ != nullptr ? *effects_ : item::Effects{}, &itemTook);
}

void HostPlay::spawnItem(double x, double y, double z, item::ItemId id, int count, double mx,
                         double my, double mz)
{
    if (drops_ == nullptr || streamer_ == nullptr) {
        return;
    }
    tick::TickWorld* world = streamer_->worldTick();
    if (world == nullptr) {
        return;
    }
    // **The velocity the guest gave it**, rather than a fresh scatter: their
    // client has already animated the throw and the two should agree about
    // where it lands. The damage does not survive the wire -- `ha` has nowhere
    // to put it -- so a thrown tool comes back undamaged, exactly as it does
    // against a real a1.1.2 server.
    drops_->spawnMoving(*world, x, y, z, id, count, 0, mx, my, mz);
}

void HostPlay::setWorldRules(settings::Gamemode gamemode, settings::Difficulty difficulty)
{
    gamemode_ = gamemode;
    net::link::WorldRules rules;
    rules.gamemode = u8(gamemode);
    rules.difficulty = u8(difficulty);
    session_.setWorldRules(rules);
}

void HostPlay::attackEntity(const AttackRequest& request)
{
    if (streamer_ == nullptr || effects_ == nullptr) {
        return;
    }
    tick::TickWorld* world = streamer_->worldTick();
    entity::MobSystem* pool = effects_->entities.mobs;
    if (world == nullptr || pool == nullptr) {
        return;
    }
    // **Only what the guest can see, which is only the animals.** Boats, carts
    // and paintings do not cross the link yet, so nothing over there has an id
    // for one and nothing here has to look for one.
    int index = -1;
    for (int i = 0; i < pool->count(); ++i) {
        if ((*pool)[i].entityId == request.entityId) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return;
    }

    item::EntityTarget target;
    target.kind = item::EntityTarget::Kind::Mob;
    target.index = index;

    item::Attacker attacker;
    attacker.present = true;
    attacker.x = request.playerX;
    attacker.z = request.playerZ;
    // **A Creative punch does not start a fight**, exactly as it does not for
    // the player at this console. See `item::Attacker::provokes`.
    attacker.provokes = gamemode_ != settings::Gamemode::Creative;

    // The same call the host's own left click makes, so the drop, the shear,
    // the knockback and the death are all one implementation.
    item::attackEntity(*world, target, item::ItemId(request.item), *effects_, attacker);
}

void HostPlay::showChat(const std::string& line)
{
    if (chat_ != nullptr) {
        chat_->add(fontWidths_, line);
    }
}

bool HostPlay::starvedGuest() const
{
    for (int id = net::link::kHostPlayerId + 1;
         id <= net::link::kHostPlayerId + net::link::kMaxGuests; ++id) {
        if (server_.starved(u8(id))) {
            return true;
        }
    }
    return false;
}

// **Where the host asks for ground.** A guest's own surroundings: it is about
// to be there, the host has to simulate there, and it is the part of the world
// this console is least likely to have on its card already.
//
// One ring at a time rather than a disc, because the pool is small on purpose
// -- each column waiting costs 32 KB -- and because there is no point asking
// for more than the generator will reach before the guest has moved again.
void HostPlay::wantAround(const net::link::Pose& pose)
{
    // Nobody to ask: leaving the pool empty is what keeps it from filling with
    // requests that can never go out.
    if (!session_.anyGenerator()) {
        return;
    }
    constexpr int kRing = 3;
    const i32 chunkX = i32(pose.x >> 5) >> 4;
    const i32 chunkZ = i32(pose.z >> 5) >> 4;
    for (int dz = -kRing; dz <= kRing; ++dz) {
        for (int dx = -kRing; dx <= kRing; ++dx) {
            // The edge of the ring only: the middle is where the guest already
            // is, which the host will have reached first.
            if (dx > -kRing && dx < kRing && dz > -kRing && dz < kRing) {
                continue;
            }
            if (!pool_.want(chunkX + dx, chunkZ + dz)) {
                return;  // full; the rest can wait for a later pose
            }
        }
    }
}

void HostPlay::onChat(u8 playerId, const std::string& text)
{
    (void)playerId;
    if (chat_ != nullptr) {
        chat_->add(fontWidths_, text);
    }
}

}  // namespace mc::ctr
