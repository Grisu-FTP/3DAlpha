#pragma once

// **Where mobs come from** -- `az` (SpawnerAnimals) and `k` (its monster
// subclass), transcribed.
//
// There are exactly two sources of mobs in a1.1.2 and these are both of them.
// **World generation spawns none**: `az` and `k` are referenced by `ia`
// (PlayerControllerSP) and by nothing else in the jar -- no chunk populator, no
// `performWorldGenSpawning` -- so a freshly generated world is empty and fills
// up while somebody stands in it. That is why a new a1.1.2 world looks lifeless
// for the first minute and why this runs on the *tick* and not on the
// generator.
//
// `ia`'s constructor builds two of these, and the second is ours:
//
//     new k(this, 200, co.class, {mb, cw, dd, ax, ma})   // 200 monsters
//     new az(15, ag.class, {bo, mv, am, mz})             // 15 animals
//
// and `ia.c()` -- which `Minecraft.runTick` calls every tick while the game is
// not paused -- runs **both**, in that order. Both are here.
//
// **The two differ in five things and share everything else**, which is why
// `az` is the base class and `k` is eleven lines:
//
//   1. the cap and what it counts -- 15 `ag`s against 200 `co`s, and `co` is an
//      *interface*, so a slime counts towards the monsters without being an
//      `ek`;
//   2. the y draw -- `rand(128)` flat for animals, `rand(rand(120) + 8)` for
//      monsters, which is `k`'s entire override and is what puts them
//      underground: the inner draw picks a ceiling and the outer picks beneath
//      it, so the distribution piles up near y = 0;
//   3. the type array;
//   4. `getCanSpawnHere` -- grass and light above 8 for an animal, two light
//      draws and darkness for a monster, and the slime's own, which checks
//      neither;
//   5. **the spider jockey**, which is in the shared `az` and would apply to an
//      animal too if one were ever an `ax`: one spawned spider in a hundred
//      arrives with a skeleton riding it.
//
// **One pass in three, ten to one against, and then it gives up.** The shape is
// worth reading before the code, because two of its quirks look like mistakes:
//
//   1. `performSpawning` runs **three times** per tick, and each run walks every
//      chunk within four of a player (81 of them) but only *tries* one chunk in
//      ten.
//   2. A chunk whose first draw lands in solid ground or in anything but air
//      **ends the whole pass** -- `return 0`, not `continue`. For an animal the
//      draw is `rand(128)` in y, so most of them land underground, which means
//      most passes stop at the first chunk they look at. That single `return` is
//      most of why animals trickle in rather than arrive. **A monster's draw is
//      `rand(rand(120) + 8)`**, which piles up near the bedrock -- where the air
//      is -- so a monster pass reaches the group loop far more often, and that
//      is most of why the 200 cap is not the fiction it looks like.
//
// Neither is reproduced for fidelity's sake: both shape the *rate*, and a
// spawner that ignored them would fill a world with pigs in a minute. This is
// the difference the project's standing rule draws between a technical limit --
// a freeze, a fixed table -- and what the game actually does.
//
// **`rand(1) - rand(1)` is always zero**, so the three-by-two scatter that
// places a group moves in x and z but never in y. That is in the class file
// (`rand.nextInt(1)` twice) and is left alone.
//
// What an *animal* spawn needs, after the position is drawn: a block below that
// is **grass**, a light level **above 8**, a clear box, no liquid, no player
// within 24 blocks and at least 24 blocks from the world's spawn point. A
// *monster* keeps the last two and swaps the rest for two light draws and
// darkness; a slime keeps only the last two and its own four clauses. See
// `canAnimalSpawnAt` and `canMonsterSpawnAt`.

#include "core/entity/mob.hpp"
#include "core/util/java_random.hpp"
#include "core/util/types.hpp"

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `new az(15, ag.class, ...)` -- **the cap is on the count of animals in the
// world**, asked before every pass. It is a rule of the game and it stays; see
// docs/status.md §18.
inline constexpr int kAnimalSpawnCap = 15;

// `new k(this, 200, co.class, ...)` -- the monsters' cap, and it is thirteen
// times the animals'. Two hundred is not a number this build will ever reach
// with a 3DS's render distance, but it is the game's and it stays.
inline constexpr int kMonsterSpawnCap = 200;

// The three passes a tick, the one-in-ten chunk, the 9 x 9 of chunks around
// each player, and the group's three-by-two scatter.
inline constexpr int kSpawnPassesPerTick = 3;
inline constexpr int kSpawnChunkOdds = 10;
inline constexpr int kSpawnChunkRadius = 4;
inline constexpr int kSpawnGroups = 3;
inline constexpr int kSpawnGroupTries = 2;
inline constexpr int kSpawnScatter = 6;

// How many chunks that square holds, which is how many `az` puts in its set.
inline constexpr int kSpawnChunkSpan = kSpawnChunkRadius * 2 + 1;
inline constexpr int kEligibleChunks = kSpawnChunkSpan * kSpawnChunkSpan;

// **The table `HashSet<ol>` ends up with**, which is a load-bearing number and
// not an implementation detail -- see `eligibleOrder` in mob_spawn.cpp. Eighty
// one entries at a load factor of 0.75 resize a HashMap to 128 buckets and stop
// there, and `clear()` keeps the table it grew, so every pass after the first
// iterates a table of this size.
inline constexpr int kEligibleBuckets = 128;

// How high the first draw may land, and the two distances a candidate is
// refused for. `kMonsterHeightCeil`/`kMonsterHeightFloor` are `k.a(Lcn;II)Lmt;`'s
// `rand.nextInt(rand.nextInt(120) + 8)`.
inline constexpr int kSpawnHeightSpan = 128;
inline constexpr int kMonsterHeightCeil = 120;
inline constexpr int kMonsterHeightFloor = 8;
inline constexpr double kSpawnPlayerClear = 24.0;
inline constexpr double kSpawnPointClearSq = 576.0;

// What the spawner needs to know that is not in the world: where the player is
// and where the world's spawn point is.
struct SpawnContext {
    bool playerPresent = false;
    double playerX = 0.0, playerY = 0.0, playerZ = 0.0;

    // `cn.o/p/q` -- the world spawn, which nothing may spawn within 24 blocks
    // of. Not the player's bed and not the last place they stood.
    i32 spawnX = 0;
    int spawnY = 0;
    i32 spawnZ = 0;

    // `cn.l` -- the difficulty, which only the slime's `getCanSpawnHere` reads
    // on this side: on Peaceful a slime bigger than 1 is refused outright.
    // Everything else is removed a tick later instead -- see `MobSurroundings`.
    int difficulty = 2;

    // **The world's seed**, which exactly one spawn check needs: `ma.a()Z`
    // reseeds a `Random` from the chunk and this, and asks it for one draw in
    // ten. That is what makes slime chunks a fixed property of a world rather
    // than something the tick decides -- `cu.a(J)`:
    //
    //     new Random(seed + (chunkX*chunkX*4987142) + (chunkX*5947611)
    //                     + ((long)(chunkZ*chunkZ))*4392871 + (chunkZ*389711)
    //                ^ 987234911L)
    //
    // and the int-versus-long boundaries in that expression are load-bearing;
    // see mob_spawn.cpp.
    i64 worldSeed = 0;
};

// **Where the attempts went**, which is the one thing a spawn report from a
// console can be. Nothing here changes behaviour: a null pointer is the default
// and costs one predictable branch per counted event.
//
// It exists because "no monsters are spawning" arrived twice as a bug report
// and neither time could be told apart from "monsters are spawning and you have
// not walked into one". The two look identical from the outside and completely
// different here: `passes` still climbing with `positions` at zero is a world
// whose chunks are not resident; `positions` climbing with `checkRejected`
// taking all of them is the light; `spawned` climbing is a player who needs to
// look harder.
struct SpawnCounters {
    // Passes run -- three per tick per spawner, unless the cap stopped them.
    i32 passes = 0;

    // Chunks that got past the one-in-ten gate **and** were resident. The gap
    // between this and `passes * 8` is what is not loaded.
    i32 chunksTried = 0;

    // Passes that ended early because the drawn point was inside something.
    // `k`'s y draw piles up near the bedrock, so in a solid world this is most
    // of them and that is the original's shape, not a fault.
    i32 abortedSolid = 0;

    // Group positions examined -- up to six per chunk that survived the draw.
    i32 positions = 0;

    // Positions with no floor, a blocked head or liquid underfoot.
    i32 noFloor = 0;

    // Positions inside 24 blocks of the player or of the world spawn.
    i32 tooNear = 0;

    // Positions `getCanSpawnHere` refused -- the light, the box, the entities.
    i32 checkRejected = 0;

    // Positions that became a mob. Matches the function's return value over
    // the same calls, jockeys included.
    i32 spawned = 0;

    void clear() { *this = SpawnCounters(); }
};

// `az.a(Lcn;)V` -- one tick's worth of animal spawning. Returns how many
// animals were added, which is what the debug page counts.
//
// **`rand` is the caller's**, because the original draws from the *world's*
// random rather than one of its own: `az` has no `Random` field and every draw
// in it is `world.rand`. Keeping it a parameter is what lets a test run a whole
// spawn pass with a known stream.
int spawnAnimals(tick::TickWorld& world, MobSystem& mobs, JavaRandom& rand,
                 const SpawnContext& context, SpawnCounters* counters = nullptr);

// `k`'s half of `ia.c()`, run on the same tick and from the same random. Two
// hundred rather than fifteen, underground rather than on grass, and with the
// jockey.
int spawnMonsters(tick::TickWorld& world, MobSystem& mobs, JavaRandom& rand,
                  const SpawnContext& context, SpawnCounters* counters = nullptr);

// **The order the spawner takes the 9 x 9 of chunks in**, written into
// `outDx`/`outDz` -- `kEligibleChunks` entries each. Exposed because it is the
// whole of where monsters may appear and because it is derived from a JVM's
// `HashSet`, which is a claim a test has to be able to check against one.
//
// See mob_spawn.cpp: `az` returns from the pass on the first drawn point that
// is not air, so the order is not a detail. Row-major put every monster north
// of the player.
void eligibleOrder(i32 centreX, i32 centreZ, i8* outDx, i8* outDz);

// `ma.a()Z`'s middle clause on its own -- **is this a slime chunk?** Exposed
// because it is a pure function of the world seed and a chunk coordinate, it is
// the one part of a spawn a player can plan around, and a test can check it
// against a known world without spawning anything.
bool isSlimeChunk(i64 worldSeed, i32 chunkX, i32 chunkZ);

// `ag.a()Z` -- **getCanSpawnHere**, which is asked after the animal has been
// placed and is the last thing that can refuse it: grass below, light above 8,
// nothing in the way and no liquid. Exposed because it is the interesting half
// and the one a test can drive directly.
bool canAnimalSpawnAt(const tick::TickWorld& world, const MobSystem& mobs, MobType type,
                      double x, double y, double z);

// `dq.a()Z` and `ma.a()Z` -- the monsters' getCanSpawnHere.
//
// **It takes the mob that is already in the pool**, which is the one place this
// file departs from the animal path above, and it departs because the jar does:
// `az` builds the entity, places it, and *then* asks. For a `dq` the difference
// is cosmetic; for a slime it is not, because `ma.a()Z` reads `this.c` -- the
// size its own constructor drew -- so there is nothing to ask until the entity
// exists.
//
// **It takes the pool by non-const reference because it excludes the candidate
// from the world for the length of the call**, and that exclusion is part of
// `ge.a()Z` rather than the caller's bookkeeping: the method is
// `getCollidingBoundingBoxes(this, box)` with `this` left out, and the
// candidate's own box is the first thing any box query finds. In the jar the
// entity is a Java object that is not in `World.loadedEntityList` until
// `spawnEntityInWorld`; here the pool *is* that list and `alive` is what every
// reader of it means by "in the world", so `alive` goes off and comes back.
//
// It cannot be an index test, because the **frame loop's own `anyEntityIn`**
// walks the same pool through `TickWorld` and has never heard of an index.
// Leaving this to the caller refused every monster on the console and passed
// every host test, because a bare test world sets no entity query at all.
//
// **It draws**, twice for a `dq` and once for a slime, and the draws happen
// whether the answer is yes or no.
bool canMonsterSpawnAt(const tick::TickWorld& world, MobSystem& mobs, int index,
                       JavaRandom& rand, const SpawnContext& context);

}  // namespace mc::entity
