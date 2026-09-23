#pragma once

// The entities a server owns: the other players, and the items on the ground.
//
// **A multiplayer client is told about entities rather than running them.** The
// server spawns, moves and destroys them, and `gy` does nothing but write what
// arrives into the world -- there is no AI here and no pickup, because both of
// those are the server's and a client that ran them would be inventing a second
// world that disagrees. What the client *does* run is the small amount of motion
// between two packets: `EntityOtherPlayerMP.setPositionAndRotation2` is handed a
// place and moves a third of the way to it on each of three ticks, which is what
// makes another player walk instead of stuttering from packet to packet.
// **This port predicts instead** -- see core/entity/server_track.hpp: the body
// is carried on through a late packet and taken back when the guess was wrong,
// because the three-tick walk trails the server and stalls on every gap.
//
// **Items are the pool the single-player game already has.** An item on the
// ground is the same `ItemEntity` either way -- it falls, it bobs, it is drawn
// by the same pass -- so a remote one is spawned into `ItemEntitySystem` and
// carries the server's id. Only two things differ: nothing may pick it up
// locally (the server says who picked up what, with `0x16` and `0x11`), and a
// position update snaps it to where the server says it is.
//
// **Players have no pool of their own in single player**, so they have one here.
// It is small and fixed: protocol 2 has no player list, a1.1.2's own server caps
// at twenty, and a console drawing more than a handful of bipeds at once has
// worse problems than the cap.
//
// **Mobs are the same pool again**, with their local mind held off -- see
// `MobSystem::spawnFromServer`. What this still does not do is vehicles
// (`0x17`): boats and carts are two more pools, and a cart's tilt is read off
// the track it is on rather than off the packet.

#include "core/entity/arrow.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/server_track.hpp"
#include "core/net/packets.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::net {

// The wire's fixed point: positions are `floor(v * 32)` absolute, relative moves
// are eighths of that in a byte, and a rotation is a byte of a whole turn.
inline constexpr double kPositionUnitsPerBlock = 32.0;
inline constexpr float kRotationUnitsPerTurn = 256.0f;

// **Entity ids one upward belong to the players, and a player's is the
// session's own player id.** Nothing on the wire says what colour anybody is,
// and nothing needs to: a host that numbers its players this way lets every
// console work the same answer out of the id it already has, so the name over
// a head and the arrow on the map agree on two screens that never discussed
// it. See `WorldServer::addPlayer`, which is where the numbering is made, and
// `kFirstFreeEntityId`, which is where everything else starts.
inline constexpr i32 kFirstPlayerEntityId = 1;
inline constexpr i32 kMaxPlayerEntityIds = 8;
inline constexpr i32 kFirstFreeEntityId = kFirstPlayerEntityId + kMaxPlayerEntityIds;

// The colour that player wears -- on their nametag and on everyone's map.
// White for an id that is not a player's, which is what a session hosted by
// something other than this port gives every entity.
//
// Chosen to be told apart on a 240-line screen over whatever ground they are
// standing on, which rules out the dark end: each of these is bright enough to
// read against grass, stone and sand alike.
void playerColour(i32 entityId, u8* red, u8* green, u8* blue);

// One other player, as protocol 2 describes them -- a name, a place, a facing
// and what is in their hand, and nothing else. **There is no skin here**:
// `0x14` carries a name and a1.1.2 fetches that player's skin over HTTP from a
// service which has not existed for years, so everyone wears the skin this
// console is set to.
struct RemotePlayer {
    // A display name's 24 characters at three UTF-8 bytes each, the most a
    // font glyph takes, and the terminator: `usernameFrom` never has to be cut.
    static constexpr int kMaxNameBytes = 24 * 3 + 1;

    i32 id = 0;
    char name[kMaxNameBytes] = {};

    // **The feet.** `c(fc)` builds a Named Entity Spawn from `fc.m`, the
    // server's `posY`, and the server's `posY` for a player is the bottom of
    // the box: the same field it puts in the *third* slot of the Position &
    // Look that places this player, with the second slot being that plus 1.62.
    double x = 0.0, y = 0.0, z = 0.0;
    double prevX = 0.0, prevY = 0.0, prevZ = 0.0;

    // Where the server last said, and where the body is headed between packets.
    entity::ServerTrack track;
    // The look still turns over a1.1.2's three ticks: a head has no momentum
    // worth predicting, and a guessed turn that is wrong reads as a twitch.
    float targetYaw = 0.0f, targetPitch = 0.0f;
    int smoothTicks = 0;

    float yaw = 0.0f, pitch = 0.0f;
    float prevYaw = 0.0f, prevPitch = 0.0f;

    // What the legs and arms are doing, from how far the body actually moved --
    // the wire says nothing about walking, and `EntityOtherPlayerMP` works it
    // out the same way.
    float limbSwing = 0.0f;
    float limbAmount = 0.0f;
    float prevLimbAmount = 0.0f;

    i16 heldItem = 0;
    int ticksExisted = 0;
    u8 light = 0;
    bool used = false;

    // **The arm swing**, which an Arm Animation starts -- `gy.a(hf)` calls
    // `dm.w()` on whoever it names -- and `dm.b_`'s counter then runs in eighths
    // of a swing, one a tick. The same counter `render::HeldItemState` runs for
    // this console's own hand.
    int swingTicks = 0;
    bool swinging = false;
    float swing = 0.0f, prevSwing = 0.0f;

    // `swingProgress(partial)`, with the wrap at the end of a swing that keeps
    // the arm from snapping back through it in one frame.
    float swingProgress(float partial) const
    {
        float f = swing - prevSwing;
        if (f < 0.0f) {
            f += 1.0f;
        }
        return prevSwing + f * partial;
    }

    // **How they stand**, which protocol 2 never says and a 3DAlpha host does
    // -- see `packet::EntityAction`. `sneaking` is `cr.j`, the model's crouch.
    // `dead` is a 3 in an Entity Status, and `deathTime` then counts the
    // twenty ticks `ge.y()` gives a body to fall over before it goes in a
    // puff: `dm` overrides none of it, so a player dies the way an animal does.
    bool sneaking = false;
    bool dead = false;
    int deathTime = 0;

    double renderX(float partial) const { return prevX + (x - prevX) * double(partial); }
    double renderY(float partial) const { return prevY + (y - prevY) * double(partial); }
    double renderZ(float partial) const { return prevZ + (z - prevZ) * double(partial); }
};

// **A blow that arrived from another console**, waiting for the frame loop to
// hand it to the player's own vitals.
//
// The wire says only *that* one entity hit another -- `packet::UseEntity`, in
// the direction protocol 2 never had -- so what it costs is worked out here,
// from the attacker's held item, by exactly the lookup the attacker's own
// console would have used. That is the same arrangement the rest of this file
// is built on: the console a thing is running on owns it, and the player's
// health is the player's own. Nothing about health crosses the link, which is
// what `kHasServerSideDamage` has always said about this protocol.
struct IncomingHit {
    bool present = false;
    // What the attacker was holding, for `ItemDef::damageVsEntity`.
    int item = 0;
    // **An arrow, not a hand**: `kArrowDamage` as `DamageSource::Arrow`,
    // which difficulty scales, and `item` means nothing. A 3DAlpha host says
    // so by naming the arrow as the attacker -- see `WorldServer::arrowStruck`.
    bool arrow = false;
    // Where they were standing, for the knockback.
    double fromX = 0.0;
    double fromZ = 0.0;
};

class RemoteEntities {
public:
    // `otherPlayerMPPosRotationIncrements` -- three ticks to cover the gap a
    // position update opens.
    static constexpr int kSmoothTicks = 3;
    static constexpr int kMaxPlayers = 8;

    // `ge.y()`'s death count: past this many ticks a dead player goes.
    static constexpr int kDeathTicks = entity::kDeathTicks;

    // The pool remote items are spawned into. Null leaves them untracked, which
    // is what a caller with no world wants.
    void bind(entity::ItemEntitySystem* items) { items_ = items; }
    void bindMobs(entity::MobSystem* mobs) { mobs_ = mobs; }
    // **Arrows a 3DAlpha host announces** (`kObjectArrow`), drawn where it
    // says and never simulated here. Null leaves them uncounted.
    void bindArrows(entity::ArrowSystem* arrows) { arrows_ = arrows; }
    entity::ArrowSystem* arrows() const { return arrows_; }

    // **What a Mob Spawn's type byte means.** `ew`'s table in the client jar,
    // read with javap: 50 creeper, 51 skeleton, 52 spider, 53 giant, 54 zombie,
    // 55 slime, 90 pig, 91 sheep *and* cow *and* chicken.
    //
    // Those three really do share one id in a1.1.2 -- `a(Class, String, int)`
    // is called with 91 for `bo`, `am` and `mz` -- so the packet cannot say
    // which of them it is, and since the map keeps the last registration a real
    // a1.1.2 client draws a **chicken** for every sheep and cow it meets. That
    // is reproduced rather than guessed around. False for a type with no model
    // here, which is the giant.
    //
    // `slimeSize` comes back as the size the server named, or **0 for "it did
    // not say"** -- which is what 55, the jar's own slime id, always means. See
    // `kOurSlime1` in the implementation.
    static bool mobTypeFor(int wireType, entity::MobType* out, int* slimeSize = nullptr);

    // The other direction, for a 3DAlpha console hosting a world. See
    // `kOurSheep` in the implementation for the places this is *not* the
    // inverse of the above, and why.
    static int wireTypeFor(entity::MobType type);

    // The same, for a mob whose type is not the whole of what the other end
    // has to draw: a slime's size is not in `ez` and cannot be guessed.
    static int wireTypeForMob(const entity::Mob& mob);

    // Applies one packet to the entities. False when the packet was not about
    // one, which leaves it for the caller to deal with.
    bool apply(const Packet& packet, tick::TickWorld* world);

    // One 20 Hz tick: the walk toward what the server last said, the limbs, the
    // light each body is standing in, and a dead one's fall -- after which it
    // goes in `ge.z()`'s puff, and is gone until the host spawns it again.
    void tick(const tick::TickWorld* world);

    void clear();

    // ---- this console's own throws -------------------------------------
    //
    // **A thrown stack is shown at once and confirmed later.** a1.1.2's
    // client sends the throw and lets go (`la.a(dx)`), so the stack appeared
    // only when the server's Pickup Spawn came back -- a round trip after the
    // button, and over the internet that is a visible pause. Here the stack
    // stays in the pool under a provisional id (negative, which no server
    // uses) and flies from the hand. The server's spawn for a stack of the same
    // item and count near where it left **takes it over**: the id becomes the
    // server's and the flight goes on uninterrupted, the server's corrections
    // pulling it onto the server's copy (`ItemEntitySystem::placeById`). No
    // spawn within `kDropConfirmTicks` is the throw refused, and the stack is
    // taken away -- which is what a1.1.2 would have shown all along.
    static constexpr int kMaxPredictedDrops = 16;
    static constexpr int kDropConfirmTicks = 60;
    static constexpr double kDropMatchDistance = 3.0;

    // Marks `item` -- in the bound pool, just thrown, id zero -- as waiting for
    // the server. False, leaving it untouched, when there is no room to wait.
    bool predictDrop(entity::ItemEntity* item);

    int predictedDrops() const { return dropCount_; }
    // Throws the server never answered. On the debug page with the rest.
    u32 revertedDrops() const { return revertedDrops_; }

    int playerCount() const { return playerCount_; }
    const RemotePlayer& player(int index) const { return players_[index]; }

    // Spawns this client has been sent and cannot draw yet: mob spawns and
    // vehicle spawns. Counted rather than dropped silently, so the debug page
    // can say "the server is sending you things you cannot see".
    u32 unhandledSpawns() const { return unhandledSpawns_; }

private:
    RemotePlayer* findPlayer(i32 id);
    RemotePlayer* addPlayer(i32 id);
    void removePlayer(i32 id);

    // A move or a teleport, whichever entity it names.
    void moveTo(i32 id, double x, double y, double z, bool hasLook, float yaw, float pitch,
                bool relative);

    // A server spawn that is one of this console's throws coming back: the
    // provisional stack takes the server's id. False when it is not one.
    bool adoptDrop(i32 serverId, item::ItemId id, int count, double x, double y, double z);
    void tickDrops();

    // `ge.z()` -- the puff a dead body goes in, around its feet.
    void puff(const RemotePlayer& player, const tick::TickWorld& world);

    RemotePlayer players_[kMaxPlayers];
    int playerCount_ = 0;
    // The death noise's pitch and the puff's scatter. Nothing else draws on
    // it, and two consoles never agree on either.
    JavaRandom rand_;
    entity::ItemEntitySystem* items_ = nullptr;
    entity::MobSystem* mobs_ = nullptr;
    entity::ArrowSystem* arrows_ = nullptr;
    u32 unhandledSpawns_ = 0;

    // Oldest first, so two identical throws are confirmed in the order made.
    struct PredictedDrop {
        i32 provisionalId;
        item::ItemId item;
        int count;
        double x, y, z;
        int ticks;
    };
    PredictedDrop drops_[kMaxPredictedDrops] = {};
    int dropCount_ = 0;
    i32 nextProvisionalId_ = -1;
    u32 revertedDrops_ = 0;
};

}  // namespace mc::net
