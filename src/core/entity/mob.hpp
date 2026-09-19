#pragma once

// **All nine mobs in a1.1.2** -- the four peaceful (`mv` pig, `bo` sheep, `am`
// cow, `mz` chicken) and the five hostile (`mb` zombie, `cw` skeleton, `dd`
// creeper, `ax` spider, `ma` slime) -- and the classes above them: `ag`
// (EntityAnimal), `dq` (EntityMob), `co` (IMob), `ek` (EntityCreature) and `ge`
// (EntityLiving).
//
// **One struct and a table, not nine subclasses.** The nine classes in the jar
// differ in a dozen numbers and a handful of short methods between them, and
// three of those methods are "which sound do I make". a1.1.2's own class
// hierarchy is a Java habit rather than nine behaviours; what actually varies is
// here in `MobDef` and what does not is the tick below, which is `ge.e_()` once.
// It is also the shape the rest of this project already uses -- render types,
// tick behaviours, block shapes -- and for the same reason: dispatching on a
// type byte keeps a pool flat and a tick branch-free where it matters.
//
// What genuinely branches is `MobAi`, which is the jar's five overrides of
// `ek.a(Lkh;F)V` -- and the slime, which is not an `ek` at all.
//
// What they have in common, in the order one tick runs it:
//
//   * **`kh.y()` / `ge.y()`** -- the counters. The idle sound, suffocation in a
//     block, drowning, fire, the void, and the twenty ticks a corpse spends
//     lying there before it disappears in a puff.
//   * **`ge.j()` -- onLivingUpdate.** Asks the AI what it wants
//     (`updatePlayerActionState`), then jumps, damps the three movement inputs
//     and hands them to `moveEntityWithHeading`. Finally every entity in a box
//     a fifth of a block wider than its own gets shoved.
//   * **`ek.b_()` -- the wander.** An animal has no target -- `findPlayerToAttack`
//     answers null for all four -- so what is left is: about once every 80
//     ticks, draw ten random cells within six blocks, score each with
//     `getBlockPathWeight`, and ask the pathfinder for a route to the best one.
//     Then follow that route a node at a time, turning at most 30 degrees a
//     tick and jumping when the next node is higher or when something is in the
//     way. See core/entity/path_finder.hpp.
//   * **`ge.b_()` -- the aimless fallback**, taken when there is no path and
//     once every hundred ticks regardless. It turns the head and nothing else:
//     `moveForward` is set to zero in it, which is why an animal with nowhere
//     to go stands still and looks around rather than milling about.
//   * **Despawning, and it applies to animals in this version.** Further than
//     128 blocks from the player is immediate; past 600 ticks of age there is a
//     1-in-800 chance per tick of vanishing, unless the player is within 32.
//     Beta 1.8 is where animals stopped despawning; here they do, and there is
//     no exception to it -- a herd left alone thins itself out.
//
// **`ek`'s attack half is live now.** It was left out when only the animals
// existed, with the reason written down: `ag` never overrides
// `findPlayerToAttack`, so no animal ever holds an `entityToAttack` and the
// branch would have been the monsters' behaviour with nothing to test it
// against. See `updateCreatureActionState`, and see docs/mobs-a1.1.2.md
// *The five hostiles* before changing any of it.
//
// **Memory, measured rather than estimated.** A `Mob` is **704 bytes** (680
// before the monsters): 388 of path, 176 of body and the rest its own state.
// a1.1.2 caps animals at 15 (`new az(15, ag.class, ...)`) and monsters at 200
// (`new k(200, co.class, ...)`), so a world at both caps is **148 KB**, and the
// pool holds 32 from construction (22.0 KB). Both caps are on *spawning* and
// not on existing, so either count may sit over its own. See docs/status.md 23
// and 25.

#include "core/entity/damage_source.hpp"
#include "core/entity/path_finder.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/rider.hpp"
#include "core/util/aabb.hpp"
#include "core/util/java_random.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"

namespace mc::audio {
class SoundEngine;
}

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `je`, from core/entity/explosion.hpp -- a creeper's blast and TNT's. Only
// `applyBlast` below names it, and only by reference.
class Explosion;

// The dropped stacks a blast reaches. See `MobSurroundings::items`.
class ItemEntitySystem;

// **The nine, in the order `ia`'s two spawners list them.** Each array is what
// a spawn draws its type from, and a reader comparing the two should not have
// to reorder one of them:
//
//     new az(15,  ag.class, {bo, mv, am, mz})        // the animals
//     new k(200,  co.class, {mb, cw, dd, ax, ma})    // the monsters
//
// **Appending is safe and prepending is not.** The save writes this index (see
// core/entity/persistence.cpp), so the four animals keep the values they had
// before the monsters existed and a world written by the animals-only build
// still loads.
enum class MobType : u8 {
    Sheep,
    Pig,
    Cow,
    Chicken,
    Zombie,
    Skeleton,
    Creeper,
    Spider,
    Slime,
};
inline constexpr int kMobTypeCount = 9;

// Where the monsters start, which is the only thing the two spawners need to
// know about the layout above.
inline constexpr int kAnimalTypeCount = 4;
inline constexpr int kMonsterTypeCount = kMobTypeCount - kAnimalTypeCount;

// **Which `updatePlayerActionState` and `attackEntity` a kind runs**, which is
// the whole of what the five monster classes add to `ek` -- and `Hop`, which
// leaves `ek` behind entirely.
//
// The jar spells this as five overrides of `ek.a(Lkh;F)V`. It is a column here
// for the reason `MobDef` is a table at all: four of the five are under twenty
// lines and the dispatch is once a tick per mob.
enum class MobAi : u8 {
    // `ag` -- **no target, ever.** `findPlayerToAttack` is not overridden, so
    // `entityToAttack` is never set and `ek`'s whole attack half is dead.
    Wander,
    // `dq` -- the zombie, and the base every monster but the slime starts from:
    // walk up and hit at 2.5 blocks when the boxes overlap vertically.
    Melee,
    // `cw` -- the skeleton: an arrow every 30 ticks from up to 10 blocks, and
    // it does not close.
    Bow,
    // `dd` -- the creeper: a fuse rather than a hit, and the entity is spent.
    Fuse,
    // `ax` -- the spider: melee, except that it leaps from between 2 and 6
    // blocks and loses interest in daylight.
    Leap,
    // `ma` -- the slime, which is not an `ek` at all: no path, no `moveSpeed`,
    // one hop every 10 to 30 ticks and damage by standing on you.
    Hop,
};

// **What one kind of animal is**, which is the whole of what the four classes
// in the jar do not share.
struct MobDef {
    // `ew`'s string, which is what a save file holds. **The numeric ids in that
    // table are 90, 91, 91, 91** -- pig, sheep, cow and chicken, with three of
    // them sharing one id, which is a bug in a1.1.2's `EntityList` and is why
    // nothing anywhere should key an animal by its number.
    const char* saveId;

    // `setSize(width, height)`.
    float width;
    float height;

    // `ge`'s `E` -- health. Ten for everything except the chicken's four, which
    // is set in its own constructor.
    int health;

    // `getDropItemId` -- `ge.g()`. **Zero for the sheep**, which drops nothing
    // when it dies: its wool comes off `attackEntityFrom` instead, once, and
    // that is `MobDef::shearDrop` below.
    u16 dropItem;

    // `getLivingSound`, `getHurtSound`, `getDeathSound` and `getSoundVolume`.
    const char* livingSound;
    const char* hurtSound;
    const char* deathSound;
    float soundVolume;

    // The one boolean each of these classes carries, and the tag it is saved
    // under. Null for the two that have none.
    //
    // It is the *same field* in the jar -- all four classes declare `public
    // boolean a` -- and only two of them ever set it: the pig's saddle and the
    // sheep's fleece. See `Mob::flag`.
    const char* flagTag;

    // What a sheep leaves when it is first hit, and nothing else has one.
    u16 shearDrop;

    // ---- the monsters' columns -----------------------------------------
    //
    // Everything below this line is zero or false for the four animals, which
    // is not padding: each one is a field or a method `ag` genuinely does not
    // have, and the rows above stay readable because of it.

    // Which AI -- see `MobAi`.
    MobAi ai;

    // **Whether the class implements `co`** (IMob), which is the one thing that
    // divides the nine. It is what `new k(200, co.class, ...)` counts against
    // its cap, what `dq.e_()` removes on Peaceful, and what tells
    // `getBlockPathWeight` and `getCanSpawnHere` which of the two versions to
    // run. A marker interface in the jar and a boolean here.
    bool hostile;

    // `dq`'s `e` -- attackStrength. **Two by default and five for a zombie**,
    // which is the one number `mb`'s constructor changes about the fight.
    int attackStrength;

    // `ge`'s `aa` -- moveSpeed, which `ek.b_()` assigns straight to
    // `moveForward` on every tick it is following a path. 0.7 for everything
    // living in this version except the zombie's 0.5 and the spider's 0.8.
    float moveSpeed;

    // `mb.j()` and `cw.j()`, which are the same eight lines in both: in
    // daylight, under open sky, with a draw against the brightness, catch fire
    // for 300 ticks. Nothing else in this version burns in the sun.
    bool burnsInSunlight;

    // `ge.b()` -- getTalkInterval, and `ag` overrides it. 80 for a monster and
    // 120 for an animal: the number the idle clock is reset to, negated.
    int talkInterval;
};

const MobDef& mobDef(MobType type);

// **The animals' sound keys, decoded at boot from the table above.**
//
// `SoundEngine::preloadSound` is not an optimisation here: a key that was never
// preloaded is silence, deliberately, because there is no filesystem in the
// per-frame path -- see core/audio/sound_engine.hpp. So an animal whose three
// keys are not on the boot list is an animal that never makes a noise, which is
// exactly what the first pass of this shipped.
//
// Derived from `MobDef` rather than listed, for the reason
// `audio::preloadBlockSounds` gives: the table is the only thing that knows
// which keys exist, and a hand-written list next to it would be a second
// version of the truth that nothing checks. Returns how many entries decoded.
usize preloadMobSounds(audio::SoundEngine& engine);

// `ag.b()` -- getTalkInterval, 120 for an animal against `ge`'s 80. Both are
// in `MobDef::talkInterval`; these are what that column is filled from.
inline constexpr int kAnimalTalkInterval = 120;
inline constexpr int kMonsterTalkInterval = 80;

// `ge`'s `aa` -- moveSpeed. 0.7 for everything living in this version except
// the zombie's 0.5 and the spider's 0.8, both set in their own constructors;
// this is the default the column is filled from.
inline constexpr float kMobMoveSpeed = 0.7f;

// `ge`'s `j` -- maxHurtResistantTime, the ten ticks of invulnerability after a
// hit, and the `10` that `hurtTime` and `maxHurtTime` are set to.
inline constexpr int kHurtResistantTime = 20;
inline constexpr int kHurtTime = 10;

// `ge.y()`'s death: twenty ticks of lying there, then the puff of smoke and the
// entity is gone.
inline constexpr int kDeathTicks = 20;

// The two Entity Status values a mob answers to -- later versions' `ge.a(B)V`
// switch, and what `packet::EntityStatus` carries between two consoles.
inline constexpr int kStatusHurt = 2;
inline constexpr int kStatusDead = 3;

// `ge.z()` -- spawnExplosionParticle: twenty `explode` puffs, each drifting on
// a **Gaussian** fiftieth of a block, and each placed ten times that drift back
// along it. See `MobSystem::explosionPuff`.
inline constexpr int kExplosionPuffs = 20;
inline constexpr double kExplosionPuffDrift = 0.02;
inline constexpr double kExplosionPuffThrowBack = 10.0;

// `ge.C()` -- jump, and it is the player's number because it is the same method.
inline constexpr double kMobJumpVelocity = kJumpVelocity;

// `ek.b_()`: the ten cells it draws, the box it draws them in, the turn limit
// and the odds.
inline constexpr int kWanderSamples = 10;
inline constexpr int kWanderSpanXZ = 13;   // rand(13) - 6
inline constexpr int kWanderSpanY = 7;     // rand(7) - 3
inline constexpr int kWanderOdds = 80;     // rand(80) == 0, asked twice
inline constexpr int kRepathOdds = 100;    // rand(100) == 0 drops the path
inline constexpr float kPathSearchRange = 10.0f;
inline constexpr float kTurnLimit = 30.0f;

// `ge.b_()`: how far a mob looks for someone to watch, how long it watches, and
// the two odds that start it.
inline constexpr float kLookRange = 8.0f;
inline constexpr int kLookTicksBase = 10;
inline constexpr int kLookTicksSpread = 20;

// Despawning, all four numbers out of `ge.b_()`. The distances are squared in
// the jar and are kept squared here: 128 blocks and 32 blocks.
inline constexpr int kDespawnAge = 600;
inline constexpr int kDespawnOdds = 800;
inline constexpr double kDespawnFar = 16384.0;
inline constexpr double kDespawnNear = 1024.0;

// `ag.a(III)F` -- getBlockPathWeight: grass underfoot is worth ten, and
// everything else is worth the cell's brightness less a half. That one method
// is the whole reason animals are found standing in fields.
inline constexpr float kGrassPathWeight = 10.0f;

// `ag.a()Z` -- getCanSpawnHere: grass below, and a light level **above** eight.
inline constexpr int kSpawnLightLevel = 8;

// `mz`'s egg clock: `600 + rand(6000)`... and it is not. The class file says
// `rand.nextInt(6000) + 6000`, so a chicken lays every five to ten minutes.
inline constexpr int kEggTimeBase = 6000;
inline constexpr int kEggTimeSpread = 6000;

// `mz.j()`'s flap, which is the one piece of per-type animation in this build:
// the wing angle is integrated rather than driven, so a falling chicken's wings
// beat and a standing one's do not.
inline constexpr float kChickenFlapGain = 2.0f;
inline constexpr double kChickenDestStep = 0.3;
inline constexpr double kChickenFlapDecay = 0.9;
inline constexpr double kChickenFallDamping = 0.6;

// ---------------------------------------------------------------------------
// The monsters
// ---------------------------------------------------------------------------
//
// Five classes and one interface. `dq` (EntityMob) is `ek` plus a target, a
// fist and a reason to spawn in the dark; `mb`, `cw`, `dd` and `ax` are `dq`
// plus one overridden method each; `ma` (the slime) skips `ek` entirely and is
// its own tick.

// `dq`'s constructor: `this.E = 20` -- twenty health for all four of them, and
// the slime's is its size squared instead.
inline constexpr int kMonsterHealth = 20;

// `dq`'s `e` -- attackStrength, and the zombie's own.
inline constexpr int kMonsterAttack = 2;
inline constexpr int kZombieAttack = 5;

// `dq.i()` -- findPlayerToAttack: the closest player within sixteen blocks,
// and only if `canEntityBeSeen` -- a straight `rayTraceBlocks` from eye to eye.
// **The range is not squared here** because `cn.a(DDDD)` squares it itself.
inline constexpr double kTargetRange = 16.0;

// `ek.b_()`'s target half: the path to the target is asked for at sixteen
// blocks, and re-asked once in twenty ticks so that a player who walks away is
// followed rather than paced.
inline constexpr float kTargetPathRange = 16.0f;
inline constexpr int kRetargetOdds = 20;

// `dq.a(Lkh;F)V` -- the fist. Under 2.5 blocks, and only when the two boxes
// overlap in y, which is what stops a zombie punching through a ceiling.
inline constexpr float kMeleeRange = 2.5f;
inline constexpr int kAttackCooldown = 20;

// `mb.j()` and `cw.j()`, identical in both: in daylight, under open sky, a
// draw from 30 against `(brightness - 0.4) * 2` sets 300 ticks of fire. The
// draw is what makes a zombie caught at dawn smoulder for a few seconds before
// it goes up rather than igniting on the exact tick the sun clears the hill.
inline constexpr int kSunBurnTicks = 300;
inline constexpr float kSunBurnBrightness = 0.5f;
inline constexpr float kSunBurnDraw = 30.0f;
inline constexpr float kSunBurnBias = 0.4f;

// `dq.j()`'s first two lines. A monster standing in light ages **twice** as
// fast, and `entityAge` is what despawns it -- so a monster that survives the
// morning in the open is gone in half the time one in a cave is.
inline constexpr int kMonsterDaylightAgeing = 2;

// `cw.a(Lkh;F)V` -- the skeleton's bow. Ten blocks, one arrow every thirty
// ticks, and the arrow starts 1.4 blocks above the skeleton's feet and is aimed
// at 0.2 below the target's own position with a lift proportional to the
// distance. `12.0f` is the arrow's inaccuracy -- see `ArrowSystem::shootFrom`.
inline constexpr float kBowRange = 10.0f;
inline constexpr int kBowCooldown = 30;
inline constexpr double kBowArrowLift = 1.399999976158142;
inline constexpr double kBowTargetDrop = 0.20000000298023224;
inline constexpr float kBowArc = 0.2f;
inline constexpr float kBowVelocity = 0.6f;
inline constexpr float kBowInaccuracy = 12.0f;

// `dd` -- the creeper. `c` is fuseTime, and the two ranges are the one that
// starts the fuse and the one a lit creeper may chase to before it gives up.
// The blast is `createExplosion(this, x, y, z, 3.0F)`.
inline constexpr int kFuseTicks = 30;

// `dd.b(F)F` -- getCreeperFlashIntensity, the fuse over **`fuseTime - 2`** and
// not over `fuseTime`. The renderer scales by it, so the swell passes 1 two
// ticks before the blast and keeps going: a creeper is at its biggest the
// instant it goes off, not a moment before.
inline constexpr int kFuseTicksForSwell = kFuseTicks - 2;
inline constexpr float kFuseStartRange = 3.0f;
inline constexpr float kFuseHoldRange = 7.0f;
inline constexpr float kCreeperBlast = 3.0f;

// `di.aQ` -- `Item.record13`, id 2256, and `dd.b(Lkh;)V` drops
// `record13.shiftedIndex + rand(2)`. a1.1.2 has exactly two records and they
// are numbered consecutively, so the base plus a draw from two is both of them.
inline constexpr int kRecordItemBase = 2256;

// `ax` -- the spider. It leaps from between two and six blocks, one tick in
// ten, and only from the ground; and in daylight it forgets its target one
// tick in a hundred, which is why a spider caught at dawn wanders off rather
// than stopping dead.
inline constexpr float kSpiderLeapNear = 2.0f;
inline constexpr float kSpiderLeapFar = 6.0f;
inline constexpr int kSpiderLeapOdds = 10;
inline constexpr int kSpiderForgetOdds = 100;
inline constexpr double kSpiderLeapSpeed = 0.5 * 0.800000011920929;
inline constexpr double kSpiderLeapKeep = 0.20000000298023224;
inline constexpr double kSpiderLeapRise = 0.4000000059604645;

// `ax`'s eye, and it is **not** `ge.s()`'s `height * 0.85`: `ax.h()` overrides
// `getMountedYOffset` to `height * 0.75 - 0.5`, which is where a skeleton sits
// when the spawner builds a jockey. The spider's own eye is still `ge.s()`.
inline constexpr double kSpiderSeatScale = 0.75;
inline constexpr double kSpiderSeatDrop = 0.5;

// `az.a(...)`'s last branch: **one spider in a hundred arrives with a skeleton
// on its back.** It is in the shared spawner rather than in `k`, so it would
// apply to an animal spawn too if an animal were ever an `ax`.
inline constexpr int kJockeyOdds = 100;

// `ma` -- the slime. Its size is `1 << rand(3)`, so 1, 2 or 4; its box is
// `0.6 * size` square; its health is `size * size`; and it hops every 10 to 30
// ticks, or three times as often when it can see a player.
inline constexpr float kSlimeSizeUnit = 0.6f;
inline constexpr int kSlimeMaxSize = 4;
inline constexpr int kSlimeHopBase = 10;
inline constexpr int kSlimeHopSpread = 20;
inline constexpr int kSlimeHopHurry = 3;
inline constexpr double kSlimeSquishDecay = 0.6;
inline constexpr float kSlimeLandSquish = -0.5f;
inline constexpr float kSlimeHopSquish = 1.0f;
inline constexpr float kSlimeVolume = 0.6f;

// `ma.a()Z` -- where slimes may be: below y = 16, in one chunk in ten picked
// by a hash of the chunk coordinates, and then one draw in ten on top of that.
// **The chunk seed is a literal in the class file** and it is what makes slime
// chunks a property of the world rather than of the tick.
inline constexpr double kSlimeMaxY = 16.0;
inline constexpr i64 kSlimeChunkSeed = 987234911LL;
inline constexpr int kSlimeChunkOdds = 10;
inline constexpr int kSlimeSpawnOdds = 10;

// `ma.b(Ldm;)V` -- onCollideWithPlayer. A slime bigger than 1 that can see the
// player and is within `0.6 * size` of them does `size` damage.
inline constexpr double kSlimeTouchScale = 0.6;

// **How many ticks a server's position update is walked over.** `gy` passes 3
// to every `setPositionAndRotation2` it makes -- the mob spawn, the teleport
// and the relative move alike -- and `ge.j()` divides what is left by whatever
// is still on the clock, so the gap is closed in thirds, halves and then
// wholly. The same number `net::RemoteEntities::kSmoothTicks` is, and the same
// reason.
inline constexpr int kServerSmoothTicks = 3;

// `dq.a()Z` -- getCanSpawnHere for a monster, which is the whole of why they
// are found underground: the **stored** sky light must be no more than a draw
// from 32 and the block light no more than a draw from 8. Sky light is read
// before the day's subtraction, so a cave roofed over at noon qualifies and an
// open field at midnight does not.
inline constexpr int kMonsterSpawnSkyDraw = 32;
inline constexpr int kMonsterSpawnBlockDraw = 8;

// `dq.a(III)F` -- getBlockPathWeight for a monster: `0.5 - getLightBrightness`,
// against `ag`'s grass-or-brightness. It is the same method with the sign the
// other way round, which is the whole of "monsters keep to the dark".
inline constexpr float kDarkPathWeight = 0.5f;

// `kh.f(kh)` -- applyEntityCollision, the shove two entities standing in each
// other give one another.
inline constexpr double kPushMinDistance = 0.009999999776482582;
inline constexpr double kPushScale = 0.05000000074505806;

// **There is no breeding in this version, and none is added here.** Wheat makes
// bread and nothing else, `ew`'s table has no baby animal, `ag` has no
// `growingAge` and nothing anywhere counts an animal towards being grown. Beta
// 1.8.1 has none either -- what 1.8 gave animals was *not despawning*, which is
// a different feature. Breeding arrives in 1.0/1.1; porting it back would mean
// a wheat use, an age field, a half-size model and an exception to the despawn
// above, none of which this version has.

// One animal.
struct Mob {
    // **`EntityLiving`'s body**, which is the player's physics with different
    // dimensions -- see core/entity/player_body.hpp. `yOffset` is zero for a
    // mob, so `body.y` and `body.posY` are both its feet.
    LivingBody body;

    MobType type = MobType::Pig;
    bool alive = false;

    // **A stable name for this mob, for as long as the session lasts.** The
    // pool swap-removes, so an index is not an identity: the mob at slot 3 this
    // tick is a different animal from the one that was there before slot 3's
    // occupant died. `kg.e_()` compares `entity != shootingEntity` by
    // reference, and this is what makes that comparison available -- see
    // `Arrow::shooterMob`, which used to approximate it by asking which mob was
    // still standing on the spot the shot was recorded at.
    //
    // **Not saved, and that is the jar's behaviour rather than a shortcut.**
    // `shootingEntity` is a live reference and a1.1.2 writes nothing for it, so
    // an arrow read back off the card has no shooter and excludes nobody. A
    // fresh handle is issued to every mob on load (`reissueHandles`), so a
    // number is never reused inside one run of the game.
    //
    // Zero is "no mob", which is what an arrow with no shooter carries.
    u32 handle = 0;

    // **The server's id, and whether the server is the one driving this mob.**
    // Zero and false in single player. A remote mob is moved only by the
    // packets that name it: its AI, its physics and its despawn are all the
    // server's, and running them here as well would be a second mind arguing
    // with the first. See core/net/entities.hpp.
    i32 entityId = 0;
    bool remote = false;

    // **Where the server last said, and how many ticks are left to get
    // there** -- `ge`'s `c`, `d`, `e`, `f`, `g` and `b`, which the jar calls
    // `newPosX/Y/Z`, `newRotationYaw/Pitch` and `newPosRotationIncrements`.
    //
    // A client does **not** teleport a mob to each packet: `gy` hands every
    // one of them to `setPositionAndRotation2` with three increments, and
    // `ge.j()` walks a third of what is left on each of the next three ticks.
    // That is the whole difference between an animal that moves and one that
    // steps at 20 Hz, and it is the same three-tick walk another player gets
    // (`net::RemoteEntities::kSmoothTicks`) because it is the same code path in
    // the jar, one class up.
    //
    // **The target is also the server's position.** A relative move is added to
    // it and never to where the body has got to -- `gy` accumulates into
    // `kh.bd/be/bf`, the fixed-point server position, and divides that -- so a
    // body still catching up does not fall further behind with every packet.
    // Seeded by the spawn, as `gy.a(ez)` seeds it.
    double serverX = 0.0, serverY = 0.0, serverZ = 0.0;
    double serverYaw = 0.0, serverPitch = 0.0;
    i16 smoothTicks = 0;

    // `ge`'s counters.
    i16 health = 10;
    // `F` -- what the previous hit left. Only the invulnerability window reads
    // it, and it is the whole of why two quick punches do not do two damage.
    i16 prevHealth = 10;
    i16 hurtTime = 0;
    i16 maxHurtTime = 0;
    i16 deathTime = 0;
    i16 attackTime = 0;
    i16 hurtResistant = 0;
    i16 air = 300;
    i16 fire = 0;
    i32 entityAge = 0;       // `U`, and what despawning counts
    i32 livingSoundTime = 0; // `a`, the idle-noise clock

    // **Ours: how many hits have landed**, counted where `hurtTime` is set. A
    // host compares it against what it last told its guests, because protocol
    // 2 has no packet for a hit and the timer alone cannot say that two hits
    // happened between two looks. Wraps, and only inequality is ever asked.
    u8 hurtSerial = 0;

    // `kh`'s `aR` -- **ticksExisted**, which is not `entityAge`: this one only
    // ever goes up, and `entityAge` is reset by a hit and by a nearby player.
    // Exactly one thing reads it, and it is a renderer: `cb`'s zombie arms sway
    // on `cos(ticksExisted * 0.09)`. Not saved, because the jar does not save
    // it -- a reloaded zombie's arms restart their sway and nothing else
    // changes.
    i32 ticksExisted = 0;

    // Angles, in degrees, and each with the previous tick's value beside it
    // because the screen is drawn between ticks. `renderYaw` is `ge`'s `n` --
    // the **body's** heading, which chases the direction of travel while the
    // head turns freely.
    float yaw = 0.0f, prevYaw = 0.0f;
    float pitch = 0.0f, prevPitch = 0.0f;
    float renderYaw = 0.0f, prevRenderYaw = 0.0f;

    // The legs. `limbYaw` is how hard they swing and `limbSwing` is the phase.
    float limbYaw = 0.0f, prevLimbYaw = 0.0f, limbSwing = 0.0f;

    // What the AI is asking for this tick -- `V`, `W` and `X`.
    float moveStrafing = 0.0f, moveForward = 0.0f, randomYaw = 0.0f;
    bool jumping = false;

    // Who it is looking at, which in this build can only be the player.
    i16 lookTicks = 0;
    bool watchingPlayer = false;

    // Where it is walking, and the node it is walking to.
    PathRoute path;

    // **The one type-specific boolean.** Saddle on a pig, fleece gone on a
    // sheep, unused on the other two -- and it is literally the same field in
    // the jar, declared four times.
    bool flag = false;

    // `mz`'s egg clock and its four wing floats.
    i32 eggTime = 0;
    float wingRotation = 0.0f, prevWingRotation = 0.0f;
    float destPos = 0.0f, prevDestPos = 0.0f;
    float wingSpeed = 1.0f;

    // Whether the player is on this pig's back. Ours to hold: a1.1.2 keeps it
    // on the entity as `riddenByEntity`, and a saddled pig is the one animal
    // that can carry anybody.
    bool ridden = false;

    // ---- the monsters ---------------------------------------------------

    // **`ek`'s `f` -- entityToAttack, as a flag.** The only entity a monster
    // can target in this build is the player: `dq.i()` asks
    // `World.getClosestPlayer`, and nothing else in a1.1.2 ever writes the
    // field except `dq.a(Lkh;I)Z`, which sets it to whoever hit this -- and
    // the only thing that hits a monster here is the player too. So a
    // reference would have exactly two states and this is the honest width.
    bool targetingPlayer = false;

    // `ek`'s `g` -- hasAttacked, cleared at the top of `updatePlayerActionState`
    // and set by the skeleton and the creeper. It does two things: it stops the
    // path being re-asked on the tick a shot went off, and it is what turns a
    // monster's body to face what it is attacking rather than where it walks.
    bool hasAttacked = false;

    // `dd`'s four fields. `fuse` is `a` (timeSinceIgnited) and `prevFuse` is
    // `b`, which the renderer interpolates for the swell; `creeperState` is
    // `d`, and it is a three-state rather than a boolean -- **-1 idle, 1 lit
    // this tick, 2 held** -- because `b_()` uses the 2 to carry "lit" across
    // the tick boundary and reset it when nothing re-lit it.
    i16 fuse = 0, prevFuse = 0;
    i8 creeperState = -1;

    // `ma`'s. `slimeSize` is `c` and is 1, 2 or 4; `hopDelay` is `d`;
    // `squish`/`prevSquish` are `a` and `b`, the flatten-and-rebound the
    // renderer scales by.
    //
    // **A slime's box is not in `MobDef`**: `c(int)` calls `setSize(0.6 * size,
    // 0.6 * size)` per entity, so `body.width`/`body.height` are the live
    // numbers and every reader in the tick goes through them rather than
    // through the table.
    u8 slimeSize = 1;
    i16 hopDelay = 0;
    float squish = 0.0f, prevSquish = 0.0f;

    // **Which mob this one is riding**, or -1. The one pair a1.1.2 makes is
    // the spider jockey -- `az` puts a skeleton on one spawned spider in a
    // hundred -- and it is the only mob-on-mob riding in the version.
    //
    // An index into a pool that swap-removes, so `removeAt` fixes it up; a
    // pointer would dangle and a handle would be machinery for one pair. It is
    // saved, and it survives because the pool is written and read in order.
    i16 mountIndex = -1;

    u8 light = 0;

    bool dying() const { return health <= 0; }
};

// **What a monster's fist reaches**, which in this build is only ever the
// player -- so the hit leaves through a seam rather than landing here.
//
// The seam is the same shape as `TickWorld`'s four sinks and is there for the
// same reason: `core/entity/` must not know what a player is. What the caller
// does with it is the caller's business -- `platform/ctr/main.cpp` hands it to
// `PlayerVitals::attack`, which is `dm.a(Lkh;I)Z`: difficulty, armour, the
// window, the knockback and the sound.
//
// `source` is the one question `dm.a` asks of its attacker -- see
// core/entity/damage_source.hpp -- and `fromX`/`fromZ` are where it stands,
// because knockback is measured from it.
struct PlayerHurt {
    void (*sink)(void* ctx, int amount, DamageSource source, double fromX,
                 double fromZ) = nullptr;
    void* ctx = nullptr;

    void operator()(int amount, DamageSource source, double fromX, double fromZ) const
    {
        if (sink != nullptr) {
            sink(ctx, amount, source, fromX, fromZ);
        }
    }
    bool reachable() const { return sink != nullptr; }
};

// **What the tick needs to know about the player.** A mob despawns by distance,
// watches a nearby player, shoves one it is standing in and is shoved back, so
// the seam carries a position, a box and somewhere to put the shove -- the same
// shape core/entity/rider.hpp takes for the same reason.
struct MobPlayer {
    bool present = false;
    double x = 0.0, y = 0.0, z = 0.0;
    AABB box{};

    // **Whether a hostile mob is allowed to treat this player as prey**, and
    // the one thing here a1.1.2 cannot be asked about: the version has no
    // gamemode, so there was never a player a monster had to ignore.
    //
    // The rule a later version settled on is the one worth copying, because it
    // is derived rather than invented: targeting runs through
    // `TargetingConditions.forCombat`, which refuses anybody who
    // `isInvulnerable()`, and a Creative player is invulnerable. So a Creative
    // player is not hunted, not leapt at, not crept up on, and -- since the
    // same test gates `HurtByTargetGoal` -- not even retaliated against by a
    // zombie they punched. See `MobSystem::attack`'s `provokes` for that half.
    //
    // **Present is not the same question and is still true.** A mob that
    // cannot hunt you still despawns by its distance from you, still turns its
    // head to watch you go past, and is still shoved by you walking into it --
    // the jar's `LookAtPlayerGoal` equivalent asks nothing about combat, and
    // the despawn asks nothing about anything. "Invisible" here means invisible
    // to the AI's target search, not absent from the world.
    //
    // **Spectator is deliberately left targetable.** It has no body, so a mob
    // that walks over to it reaches nothing and hurts nobody (see
    // platform/ctr/main.cpp); that is a different bargain from Creative's and
    // is not changed here.
    bool targetable = true;

    // The player's motion, so `applyEntityCollision` can push both ways. Null
    // is a player who cannot be pushed, which is what every headless caller
    // has.
    //
    // **`motionY` is the explosion's**, and only the explosion's: a shove is
    // horizontal (`kh.f(Lkh;)V` never touches y) and a blast is not. It is
    // null-checked separately so that a caller offering a body it does not want
    // launched can leave it out.
    double* motionX = nullptr;
    double* motionY = nullptr;
    double* motionZ = nullptr;
};

// Everything else one tick may reach.
struct MobSurroundings {
    MobPlayer player;

    // `cn.l` -- **the world's difficulty**, which three separate things read:
    // `dq.e_()` removes every monster on Peaceful, `ma.a()Z` refuses to spawn
    // a big slime on it, and `dm.a(Lkh;I)Z` scales incoming damage by it.
    // 0 Peaceful, 1 Easy, 2 Normal, 3 Hard, and a1.1.2's own default is 2.
    //
    // **Peaceful is not "no monsters spawn"** in this version; it is "every
    // monster is removed on the tick it notices", which is a visible
    // difference: a1.1.2 spawns them and then kills them, so the 200-cap
    // spawner still runs and still costs what it costs.
    int difficulty = 2;

    // Where a fist lands. Unset is a player who cannot be hurt, which is what
    // every headless caller has -- the swing still happens, the cooldown still
    // runs and the sound is still made.
    PlayerHurt hurtPlayer{};

    // `cw.a(Lkh;F)V` -- where a skeleton's arrow goes. Unset is a skeleton
    // that still aims, still plays `random.bow` and still spends its thirty
    // ticks, and whose arrow is thrown away -- the same bargain
    // `TickWorld::spawnItem` takes, and for the same reason: the pools belong
    // to the frame loop.
    //
    // `shooterMob` is the firing mob's `Mob::handle`, carried through so the
    // arrow can decline to hit the skeleton that loosed it -- `kg.e_()`'s
    // `entity != shootingEntity`. See `Arrow::shooterMob`.
    void (*shootArrow)(void* ctx, double x, double y, double z, double dx, double dy,
                       double dz, float velocity, float inaccuracy,
                       u32 shooterMob) = nullptr;
    void* shootArrowCtx = nullptr;

    // **What is lying on the ground, for `applyBlast`.** `je` hurts every
    // entity in its box and a dropped stack has five points of health, so a
    // creeper or TNT going off beside one destroys it. Unset is a blast that
    // leaves items alone, which is what every headless caller gets.
    ItemEntitySystem* items = nullptr;
};

// **No cap, as with every other pool here** -- a1.1.2 caps mob *spawning* and
// not the list. See core/util/segmented_pool.hpp and status.md §18.
class MobSystem {
public:
    // Held from construction so ordinary play never allocates. a1.1.2's own
    // animal cap is 15, so this is that plus room for the chickens an egg
    // clock and a breeding pen add on top.
    static constexpr int kInitialCapacity = 32;

    explicit MobSystem(i64 seed) : rand_(seed) {}

    // `cn.a(Lkh;)Z` for one animal, given a place and a heading. Returns false
    // only when the heap would not hold another.
    bool spawn(const tick::TickWorld& world, MobType type, double x, double y, double z,
               float yaw);

    // ---- mobs a server owns --------------------------------------------
    //
    // The same animal, with `remote` set: it is drawn and interpolated here and
    // decided entirely over there. Spawning one twice under the same id is the
    // server re-sending it, and replaces rather than duplicates.
    // `slimeSize` is 1, 2 or `kSlimeMaxSize` when the server named one, and
    // **0 when it did not** -- which is what a real a1.1.2 server always
    // leaves, `ez` having nowhere to put it. Zero keeps the size the
    // constructor drew, which is the jar's behaviour; anything else overwrites
    // it through `setSlimeSize`, box and health and all. See
    // `net::RemoteEntities::mobTypeFor`.
    Mob* spawnFromServer(const tick::TickWorld& world, i32 entityId, MobType type, double x,
                         double y, double z, float yaw, float pitch, int slimeSize = 0);
    Mob* findById(i32 entityId);
    bool removeById(i32 entityId);

    // `gy` -> `kh.a(DDDFFI)V` -- where the server says it is, to be walked to
    // over the next `kServerSmoothTicks` ticks rather than jumped to. The legs
    // follow from how far the body actually moves, as they do for another
    // player, so they are driven in `tick` and not here.
    bool placeById(i32 entityId, double x, double y, double z, bool hasLook, float yaw,
                   float pitch);

    // `gy.a(ju)` -- a look with no move. Same three ticks, position untouched.
    bool turnById(i32 entityId, float yaw, float pitch);

    // **Entity Status, which protocol 2 does not have** -- `packet::EntityStatus`
    // -- and later versions' `ge.a(B)V`, `handleHealthUpdate`, as its model. 2
    // is a hit: the legs flail, the red tint runs its ten ticks and the hurt
    // noise plays. 3 is the death: the death noise, health to zero, and the
    // twenty ticks of falling over that `tick` then counts. Anything else is
    // ignored. False when no such mob is here.
    bool statusFromServer(tick::TickWorld& world, i32 entityId, int status);

    // One 20 Hz tick of `ge.e_()` for every live animal.
    void tick(tick::TickWorld& world, const MobSurroundings& around);

    // `ge.a(Lkh;I)Z` -- attackEntityFrom. `fromPlayer` is what the sheep
    // checks: its fleece comes off only when an `EntityLiving` hit it, which a
    // falling anvil of a cactus is not. Returns whether the hit landed.
    // `fromSkeleton` is the one thing a1.1.2 asks about the killer beyond
    // whether it was alive: `dd.b(Lkh;)V` drops a music disc when a **skeleton**
    // kills a creeper, which is the only source of a record in this version.
    // `provokes` is the other half of `MobPlayer::targetable`: false is a hit
    // that lands, shears, knocks back and kills exactly as any other does, and
    // leaves the monster with no reason to come after whoever threw it. It is
    // what a Creative fist is, and defaults to true so that everything that
    // was already a provocation -- an arrow, a Survival punch -- still is.
    bool attack(tick::TickWorld& world, int index, int amount, bool fromPlayer,
                double fromX = 0.0, double fromZ = 0.0, bool knockback = false,
                bool fromSkeleton = false, bool provokes = true);

    // **The right-click**, which is `kh.a(Ldm;)Z` -- interact -- plus the
    // `ItemStack.useItemOnEntity` that runs when interact refuses. Three
    // animals answer it:
    //
    //   * a **cow** holding out a bucket hands back a milk bucket (`am.a(Ldm;)`);
    //   * a **pig** with a saddle on takes the player aboard (`mv.a(Ldm;)`);
    //   * a **pig** without one takes the saddle being held (`jw.b`, ItemSaddle).
    //
    // `becomes` is what the held item turns into, which is how the bucket and
    // the saddle are spent.
    struct Interaction {
        bool taken = false;
        u16 becomes = 0;
        bool mounted = false;
    };
    Interaction interact(tick::TickWorld& world, int index, u16 held);

    // Which pig is being ridden, or -1, and where its rider goes.
    int riddenIndex() const { return ridden_; }
    RiderSeat seat() const;
    bool mount(int index);
    // Returns where the rider lands -- the animal's back, per
    // `mountEntity`. Invalid when nothing was aboard.
    RiderSeat dismount();

    // `kh.g(Lkh;)V` -- mountEntity, for the one pair that is mob on mob. The
    // rider keeps running its own AI and only its **position** is overwritten
    // afterwards, which is the same bargain `core/entity/rider.hpp` describes
    // for a player in a boat and is the jar's: `ge` never asks whether it is
    // riding. So a mounted skeleton still shoots.
    bool mountOn(int riderIndex, int mountIdx);

    // Where a mob riding another one sits, or an invalid seat. Same method as
    // `seat()` above -- `kh.v()`, with `ax`'s override.
    RiderSeat seatOf(int index) const;

    // How many of these are in the world -- `cn.b(Ljava/lang/Class;)I`, which
    // is what each spawner compares against its own cap. `ag.class` is the
    // fifteen animals; `co.class` is the two hundred monsters, and it is an
    // *interface*, so the slime counts even though it is not an `ek`.
    int animalCount() const;
    int monsterCount() const;

    // **`getCanSpawnHere` said no.** `az` constructs the entity, places it and
    // only then asks; a refusal drops it on the floor of the JVM. Here it is
    // put in the pool and taken straight back out, which is the same thing as
    // long as nothing ticks in between -- and nothing does. See
    // core/entity/mob_spawn.hpp.
    void despawn(int index) { removeAt(index); }

    // **Every mob in the pool gets a fresh handle.** Called once after a world
    // is read back off the card, because `Mob::handle` is not in the save and
    // every loaded mob therefore arrives holding zero -- which is the value an
    // arrow uses for "fired by nobody". Without this every loaded mob would
    // answer to that.
    void reissueHandles()
    {
        for (int i = 0; i < mobs_.size(); ++i) {
            mobs_[i].handle = nextHandle_++;
        }
    }

    // The handle of the mob at `index`, or zero when there is none. The arrow
    // sweep asks this rather than reaching into the pool, because "no mob at
    // that slot" and "a mob that is nobody" have to answer the same way.
    u32 handleAt(int index) const
    {
        return index >= 0 && index < mobs_.size() ? mobs_[index].handle : 0u;
    }

    // `ge.z()` -- **spawnExplosionParticle**, the twenty-puff cloud a mob makes
    // when it dies and when a mob spawner places it. Public because the block
    // spawner (core/entity/mob_spawner.hpp) calls it on a mob it has just added
    // and has no other way to reach this pool's generator.
    //
    // **Each puff is thrown back along its own drift by ten.** The three
    // Gaussians are drawn first and then subtracted, times 10, from the
    // position they are given -- so the cloud starts as a shell that collapses
    // inwards rather than a point that expands, which is why a death puff looks
    // like an implosion in the first few frames. Getting that backwards, or
    // drawing the positions before the drifts, gives the right *number* of
    // particles in the wrong places; both were wrong here until the bytecode
    // was read again for the spawner.
    void explosionPuff(const tick::TickWorld& world, int index);

    // `ma.c(I)V` -- **setSlimeSize**, which is not a field write: it resizes
    // the box, sets health to `size * size` and re-places the entity. Public
    // because the spawner draws the size and the save restores it.
    void setSlimeSize(Mob& mob, int size) const;

    void clear()
    {
        mobs_.clear();
        ridden_ = -1;
    }

    int count() const { return mobs_.size(); }
    const Mob& operator[](int i) const { return mobs_[i]; }
    Mob& at(int i) { return mobs_[i]; }

    u32 refused() const { return refused_; }
    PathFinder& pathFinder() { return paths_; }

    // **The most searches any one tick has run**, since the per-tick cap became
    // a count. This is the number that says what removing it costs: against a
    // 50 ms tick, a search that hits `PathFinder::kMaxNodes` is the expensive
    // case and this says how many of them landed together. The debug page
    // carries it beside `path`/`ex`.
    int peakSearchesPerTick() const { return peakSearchesPerTick_; }

private:
    friend struct PersistentEntities;

    void removeAt(int index);

    // `ge.j()`'s first block -- one tick of the walk towards where the server
    // last said this animal was. See the implementation.
    void interpolateToServer(Mob& mob) const;

    // `ge.e_()`'s tail: the legs, the body's heading, and the light byte the
    // frame is drawn with. Run for a remote animal as well as for ours, which
    // is the jar's arrangement -- `isMultiplayerEntity` holds off the AI and
    // nothing else.
    void headingAndLight(const tick::TickWorld& world, Mob& mob, double beforeX,
                         double beforeZ) const;

    // `kh.f(kh)` -- applyEntityCollision. Pushes `mob` away from whatever is at
    // `(otherX, otherZ)` and pushes that thing back, unless it has no motion to
    // be pushed with.
    void shove(Mob& mob, double otherX, double otherZ, double* otherMotionX,
               double* otherMotionZ);

    // `ge.b(Lkh;)V` -- onDeath: `rand(3)` of whatever this kind drops, plus
    // `dd.b(Lkh;)V`'s music disc.
    void dropOnDeath(const tick::TickWorld& world, const Mob& mob, bool fromSkeleton);

    // ---- the monsters' halves of `ek.b_()` ------------------------------

    // `dq.i()` / `ax.i()` -- findPlayerToAttack, and the answer here can only
    // ever be the player. False also clears an existing target, which is what
    // `ek.b_()` does when `entityToAttack.isEntityAlive()` goes false.
    bool findTarget(const tick::TickWorld& world, Mob& mob, const MobSurroundings& around);

    // `ge.c(Lkh;)Z` -- canEntityBeSeen: a straight `rayTraceBlocks` from this
    // mob's eye to the target's.
    bool canSee(const tick::TickWorld& world, const Mob& mob, double x, double y,
                double z) const;

    // `dq.a(Lkh;F)V` and the four overrides of it. `distance` is
    // `getDistanceToEntity`, which is measured between *positions* and not
    // between boxes.
    void attackTarget(tick::TickWorld& world, int index, const MobSurroundings& around,
                      float distance);

    // `ma.b_()` -- the slime's whole action state, which is not `ek`'s at all:
    // no path, no `moveSpeed`, one hop every 10 to 30 ticks.
    void hopAbout(const tick::TickWorld& world, Mob& mob, const MobSurroundings& around);

    // `dd`'s explosion, and `ma.F()`'s four children. Both happen where the
    // entity is removed rather than where it is killed.
    void detonate(tick::TickWorld& world, int index, const MobSurroundings& around);
    void splitSlime(const tick::TickWorld& world, const Mob& parent);

    // `ek.b_()` and `ge.b_()`. Both answer **false when the mob despawned**,
    // which is `setEntityDead` and is not a death: no drops, no sound and none
    // of the twenty ticks a killed animal spends on the ground.
    // **Takes an index and a mutable world** because a creeper's own action
    // state can destroy the world around it and remove the mob: `dd.a(Lkh;F)V`
    // calls `createExplosion` and then `setEntityDead`. False means the slot
    // must be removed -- a despawn or a detonation, neither of which is a
    // death.
    bool updateActionState(tick::TickWorld& world, int index, const MobSurroundings& around);

    // `ek.b_()` itself, which `dd.b_()` wraps. Split out because the creeper's
    // override is three lines before `super` and one after, and inlining them
    // into the branches would put the "after" line at four return points.
    bool updateCreatureActionState(tick::TickWorld& world, int index,
                                   const MobSurroundings& around);
    bool wanderAimlessly(const tick::TickWorld& world, Mob& mob,
                         const MobSurroundings& around);

    // `ag.a(III)F` and `dq.a(III)F` -- getBlockPathWeight, and the two are
    // opposite: grass-or-brightness for an animal, `0.5 - brightness` for a
    // monster.
    float pathWeight(const tick::TickWorld& world, MobType type, i32 x, int y, i32 z) const;

    // `ge.b(Lkh;F)V` -- faceEntity, and the private `b(FFF)F` under it.
    void faceTowards(Mob& mob, double x, double y, double z, float limit) const;

    // `ge.y()` -- the counters, the idle noise and the twenty ticks of dying.
    // Returns false when the mob is gone and the slot must be removed.
    //
    // **It takes an index rather than a reference** because everything that
    // hurts an animal in here -- fire, lava, the void, drowning, a block it is
    // standing inside -- is `attackEntityFrom(null, n)` in the jar, and going
    // through `attack` is what makes those deaths drop what a punched one does
    // and make the same noise.
    bool updateCounters(tick::TickWorld& world, int index, const MobSurroundings& around);

    // The part of `updateCounters` a mob the server owns still runs here: the
    // idle noise, the hurt timers and the death count. False when the corpse
    // has lain its twenty ticks and goes. See `tick`'s remote branch.
    bool remoteCounters(tick::TickWorld& world, int index);

    SegmentedPool<Mob, kInitialCapacity> mobs_;

    // **Never reused inside a session**, which is the whole value of it: a
    // handle that came back round could let an arrow's five-tick grace land on
    // an animal that was not there when the shot was fired. It starts at one
    // because zero means "nobody".
    u32 nextHandle_ = 1;
    PathFinder paths_;
    int ridden_ = -1;
    u32 refused_ = 0;
    JavaRandom rand_;

    // **How many searches this tick has run, counted and no longer capped.**
    //
    // It used to be a budget of one a tick across every mob in the world, on
    // the argument that a1.1.2 runs the search inline whenever a mob asks and a
    // 268 MHz ARM11 might not afford that. With fifteen animals it never bound
    // -- they ask about 0.4 times a tick between them -- and it was invisible.
    // With the monsters it binds hard and in the wrong direction: two hundred
    // of them, a chasing one re-asking once in twenty ticks, is ten requests a
    // tick against a budget of one, so nine in ten mobs were handed no path and
    // fell back to `EntityLiving`'s aimless wander. That is not "the same state
    // the original reaches too"; that is most of a cave standing still.
    //
    // **And it moved the random stream**, which is the half that made it a
    // correctness bug rather than a performance trade. The wander branch draws
    // ten candidate cells *before* deciding anything, and the jar draws them
    // whether or not a path comes of it -- but the budget gate sat in front of
    // the draws, so the second mob to want a path in a tick skipped thirty
    // `nextInt` calls the original makes. Every mob ticked after it in that
    // world saw a different stream from the one a1.1.2 would have given it.
    //
    // The ceiling that remains is `PathFinder::kMaxNodes`, which is a fixed
    // arena rather than a refusal and is the one budget this port still keeps:
    // an unbounded search means allocating inside the 20 Hz tick, which the
    // platform does not allow. `PathFinder::exhausted()` says when it binds.
    int searchesThisTick_ = 0;
    int peakSearchesPerTick_ = 0;
};

// **`je`'s middle phase, for whoever set the blast off.** Between `cast` and
// `destroy`, every entity in reach takes damage and an impulse -- and it runs
// while the blocks are still standing, which is the whole reason the three
// phases are separate calls (see explosion.hpp).
//
// It lives here rather than on `Explosion` because the *targets* do: the
// player comes in through `MobSurroundings` and the animals are a pool this
// file owns. A creeper reaches it with itself excluded; primed TNT excludes
// nothing, because `cn.a(Lkh;DDDF)V`'s exploder argument is null there.
//
// Both `mobs` and `around` are optional. Neither set is a blast that moves
// blocks and nothing else, which is what every headless caller gets.
void applyBlast(tick::TickWorld& world, const Explosion& blast, MobSystem* mobs,
                const MobSurroundings* around, int exclude = -1);

}  // namespace mc::entity
