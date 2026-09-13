// See mob_spawn.hpp. `az.a(Lcn;)V`, `az.a(Lcn;ILnu;)I` and `az.a(Lcn;II)Lmt;`,
// transcribed, with `ag.a()Z` and `ek.a()Z` under them.

#include "core/entity/mob_spawn.hpp"

#include "core/block/collision.hpp"
#include "core/block/fluid_flow.hpp"
#include "core/block/registry.hpp"
#include "core/entity/sweep.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"
#include "core/world/daylight.hpp"

#include "blocks.hpp"  // generated; see tools/configure.py

namespace mc::entity {
namespace {

constexpr block::BlockId kGrass = block::BlockId(mcver::Block::Grass);
constexpr u8 kWaterMaterial = mcver::kBlocks[int(mcver::Block::Water)].material;
constexpr u8 kLavaMaterial = mcver::kBlocks[int(mcver::Block::Lava)].material;

// The two class arrays, each in `ia`'s order -- the draw is `rand(n)` into one
// of them, so the order is observable in the random stream and not just
// cosmetic.
constexpr MobType kAnimalTypes[kAnimalTypeCount] = {
    MobType::Sheep,
    MobType::Pig,
    MobType::Cow,
    MobType::Chicken,
};
constexpr MobType kMonsterTypes[kMonsterTypeCount] = {
    MobType::Zombie,
    MobType::Skeleton,
    MobType::Creeper,
    MobType::Spider,
    MobType::Slime,
};

// `cn.g(III)Z`, which is `Block.isOpaqueCube()` on the live object rather than
// the cached array -- see core/block/block_def.hpp on the difference, which is
// leaves.
bool opaqueCube(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    return world.opaqueAt(x, y, z);
}

bool liquidAt(const tick::TickWorld& world, i32 x, int y, i32 z)
{
    const u8 material = block::def(world.blockAt(x, y, z)).material;
    return material == kWaterMaterial || material == kLavaMaterial;
}

// `cn.a(Lcf;)Z` -- checkIfAABBIsClear, reduced to what it can answer here: no
// block collision box in the way. The entity half is the caller's, below.
bool boxIsClear(const tick::TickWorld& world, const AABB& box)
{
    const BlockRange range = sweepRange(box);
    AABB boxes[block::kMaxCollisionBoxes];
    for (i32 bx = range.x0; bx < range.x1; ++bx) {
        for (i32 bz = range.z0; bz < range.z1; ++bz) {
            if (!world.chunkResident(bx >> 4, bz >> 4)) {
                // An absent column is not a clear one: a mob spawned against
                // the edge of what is loaded would be standing in whatever
                // arrives next.
                return false;
            }
            for (int by = range.y0; by < range.y1; ++by) {
                const block::BlockId id = world.blockAt(bx, by, bz);
                if (id == block::kAir) {
                    continue;
                }
                const int count = block::collisionBoxes(id, world.dataAt(bx, by, bz), boxes,
                                                        block::kMaxCollisionBoxes);
                for (int i = 0; i < count; ++i) {
                    if (boxes[i].offset(double(bx), double(by), double(bz)).intersects(box)) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool anyLiquidInBox(const tick::TickWorld& world, const AABB& box)
{
    return block::isMaterialInBox(world, box, kWaterMaterial)
           || block::isMaterialInBox(world, box, kLavaMaterial);
}

}  // namespace

// **The order `az` takes the eligible chunks in, and it decides where monsters
// appear.**
//
// The set is a `HashSet<ol>` filled row by row and then iterated, and the
// obvious reading is that a set has no order worth reproducing. That reading is
// wrong here, and measurably so: the pass `return`s the moment a drawn point is
// not air, and `k`'s y draw -- `rand(rand(120) + 8)` -- lands inside rock
// better than nineteen times in twenty. Over 18,000 passes of a real world,
// 17,986 of them ended on their first or second chunk. **Whatever the set hands
// over first is the only part of the square that ever spawns anything.**
//
// Iterating row-major, which is what this did, pinned every monster to the
// first rows of the 9 x 9 -- 52 spawns in a measured run, every one of them
// north of the player, none at all in the southern third. That is not a subtle
// difference from the original; it is the difference between mobs around you
// and mobs only ever behind you.
//
// So the order is derived instead, from the two things that fix it:
//
//   * **`ol.hashCode()` is `(x << 8) | z`** -- and that is a *terrible* hash,
//     which is the point. For any negative z the sign bits fill everything
//     above bit 7, so the x half is swallowed whole and all nine columns of a
//     row share one hash. The scatter that results is the original's scatter.
//   * **`HashMap` puts a key in bucket `(h ^ (h >>> 16)) & (n - 1)`** and
//     iterates bucket by bucket, each bucket in insertion order. Eighty one
//     entries settle at `n` = 128 and `clear()` keeps the table, so that is the
//     table every pass after the first iterates.
//
// Insertion order is `az`'s own: x outer, z inner, each from -4 to 4.
//
// **This is Java 8's layout.** Java 7 spread its hashes differently and had no
// treeified bins, so a 2010 JVM would hand back a different order -- and the
// bins here run to nine entries, which is over Java 8's treeify threshold.
// Treeifying does not reorder: `TreeNode` keeps the `next` chain it was built
// from and `split` preserves it, so iteration is still insertion order within
// the bucket. A JVM-version-dependent order cannot be reproduced exactly by
// anything; what matters, and what this gets, is that the order is the jar's
// hash rather than a raster scan.
void eligibleOrder(i32 centreX, i32 centreZ, i8* outDx, i8* outDz)
{
    u8 bucket[kEligibleChunks];
    u8 count[kEligibleBuckets] = {0};

    int n = 0;
    for (int ix = 0; ix < kSpawnChunkSpan; ++ix) {
        for (int iz = 0; iz < kSpawnChunkSpan; ++iz) {
            const u32 x = u32(centreX + ix - kSpawnChunkRadius);
            const u32 z = u32(centreZ + iz - kSpawnChunkRadius);
            const u32 hash = (x << 8) | z;
            const u32 spread = hash ^ (hash >> 16);
            bucket[n] = u8(spread & u32(kEligibleBuckets - 1));
            ++count[bucket[n]];
            ++n;
        }
    }

    // Counting sort by bucket, stable, so each bucket keeps insertion order --
    // which is what a HashMap chain does.
    u8 start[kEligibleBuckets];
    u8 running = 0;
    for (int b = 0; b < kEligibleBuckets; ++b) {
        start[b] = running;
        running = u8(running + count[b]);
    }
    n = 0;
    for (int ix = 0; ix < kSpawnChunkSpan; ++ix) {
        for (int iz = 0; iz < kSpawnChunkSpan; ++iz) {
            const int slot = start[bucket[n]]++;
            outDx[slot] = i8(ix - kSpawnChunkRadius);
            outDz[slot] = i8(iz - kSpawnChunkRadius);
            ++n;
        }
    }
}


bool canAnimalSpawnAt(const tick::TickWorld& world, const MobSystem& mobs, MobType type,
                      double x, double y, double z)
{
    const i32 bx = MathHelper::floorDouble(x);
    const int by = int(MathHelper::floorDouble(y));
    const i32 bz = MathHelper::floorDouble(z);

    // `ag.a()Z`, and both halves of it: **grass underneath**, and a light level
    // strictly greater than eight. The light is `cn.j(III)` --
    // getFullBlockLightValue -- so it is the day's light after the sky
    // subtraction, which is why animals stop spawning at dusk.
    if (world.blockAt(bx, by - 1, bz) != kGrass) {
        return false;
    }
    if (world.lightValue(bx, by, bz) <= kSpawnLightLevel) {
        return false;
    }

    // `ek.a()Z` -> `ge.a()Z`: the box has to be clear of blocks, of entities and
    // of liquid, and then `getBlockPathWeight` at the feet must not be negative
    // -- which for an animal means "not pitch dark", since the weight is
    // brightness less a half everywhere that is not grass.
    const MobDef& def = mobDef(type);
    const double half = double(def.width / 2.0f);
    const AABB box{x - half, y, z - half, x + half, y + double(def.height), z + half};

    if (!boxIsClear(world, box) || anyLiquidInBox(world, box)) {
        return false;
    }
    if (world.anyEntityIn(box, tick::EntityFilter::Everything)) {
        return false;
    }
    for (int i = 0; i < mobs.count(); ++i) {
        if (mobs[i].alive && mobs[i].body.box.intersects(box)) {
            return false;
        }
    }
    return true;
}

bool isSlimeChunk(i64 worldSeed, i32 chunkX, i32 chunkZ)
{
    // `cu.a(J)Ljava/util/Random;` -- getChunkRandom, and **the int-versus-long
    // boundaries in it are load-bearing**: three of the four products are `int`
    // multiplies that wrap at 32 bits before they are widened, and only
    // `chunkZ * chunkZ` is widened first and then multiplied as a long. A
    // version that promoted everything to 64 bits picks different chunks.
    //
    //     seed + (long)(cx*cx*4987142) + (long)(cx*5947611)
    //          + (long)(cz*cz) * 4392871 + (long)(cz*389711)   ^ 987234911L
    //
    // The `^` binds looser than every `+`, so it applies to the whole sum.
    const i32 cx = chunkX;
    const i32 cz = chunkZ;
    const i64 mixed = worldSeed + i64(i32(i32(cx * cx) * 4987142)) + i64(i32(cx * 5947611))
                      + i64(i32(cz * cz)) * 4392871 + i64(i32(cz * 389711));
    JavaRandom chunkRand(mixed ^ kSlimeChunkSeed);
    return chunkRand.nextInt(kSlimeChunkOdds) == 0;
}

// Holds a mob out of the world for a scope. See `canMonsterSpawnAt`: the
// candidate must not be the entity that refuses its own spawn.
class NotYetInTheWorld {
public:
    NotYetInTheWorld(MobSystem& mobs, int index) : mobs_(mobs), index_(index)
    {
        was_ = mobs_.at(index_).alive;
        mobs_.at(index_).alive = false;
    }
    ~NotYetInTheWorld() { mobs_.at(index_).alive = was_; }

    NotYetInTheWorld(const NotYetInTheWorld&) = delete;
    NotYetInTheWorld& operator=(const NotYetInTheWorld&) = delete;

private:
    MobSystem& mobs_;
    int index_;
    bool was_ = false;
};

bool canMonsterSpawnAt(const tick::TickWorld& world, MobSystem& mobs, int index,
                       JavaRandom& rand, const SpawnContext& context)
{
    const NotYetInTheWorld hidden(mobs, index);
    const Mob& mob = mobs[index];
    const i32 bx = MathHelper::floorDouble(mob.body.x);
    const int by = int(MathHelper::floorDouble(mob.body.box.minY));
    const i32 bz = MathHelper::floorDouble(mob.body.z);

    // `ma.a()Z` -- **the slime's, which checks none of `dq`'s and none of
    // `ge`'s.** It does not call super at all, so a slime spawns in a box that
    // is not clear, in liquid, and inside another entity. Four clauses:
    if (mob.type == MobType::Slime) {
        if (mob.slimeSize != 1 && context.difficulty <= 0) {
            return false;
        }
        if (rand.nextInt(kSlimeSpawnOdds) != 0) {
            return false;
        }
        if (!isSlimeChunk(context.worldSeed, bx >> 4, bz >> 4)) {
            return false;
        }
        // **`posY`, not the feet** -- the two are the same for a mob, and the
        // 16 is the absolute height rather than a depth below the surface, so
        // a mountain does not push the layer up with it.
        return mob.body.y < kSlimeMaxY;
    }

    // `dq.a()Z`. **The sky light is the stored value**, before the day's
    // subtraction, so a cave roofed over at noon qualifies and an open field at
    // midnight does not -- the time of day changes nothing about where monsters
    // may spawn in this version, only about whether they burn afterwards.
    if (int(world.skyLightAt(bx, by, bz)) > rand.nextInt(kMonsterSpawnSkyDraw)) {
        return false;
    }
    // `cn.j(III)` -- the day-subtracted maximum of sky and block, which is the
    // same method `ag.a()Z` reads and compares the other way.
    if (world.lightValue(bx, by, bz) > rand.nextInt(kMonsterSpawnBlockDraw)) {
        return false;
    }

    // `ek.a()Z` -> `ge.a()Z`: a clear box, no entity in it, no liquid, and then
    // `getBlockPathWeight >= 0` -- which for a monster is `0.5 - brightness`,
    // so "not brighter than half".
    const AABB& box = mob.body.box;
    if (!boxIsClear(world, box) || anyLiquidInBox(world, box)) {
        return false;
    }
    // **The candidate must not find itself**, which `NotYetInTheWorld` above
    // is holding off for the length of this call: `ge.a()Z` is
    // `getCollidingBoundingBoxes(this, box)` and the `this` is an exclusion.
    // Both of these walk the pool -- the second directly, the first through the
    // frame loop's own entity query -- and neither can take an index.
    if (world.anyEntityIn(box, tick::EntityFilter::Everything)) {
        return false;
    }
    for (int i = 0; i < mobs.count(); ++i) {
        if (mobs[i].alive && mobs[i].body.box.intersects(box)) {
            return false;
        }
    }
    return kDarkPathWeight - world::lightBrightness(world.lightValue(bx, by, bz)) >= 0.0f;
}

// The two spawners' single body. Everything in `az.a(Lcn;ILnu;)I` is shared;
// what `k` overrides is the y draw, and what `ia` varies is the cap, the class
// list and which `getCanSpawnHere` runs.
int runSpawner(tick::TickWorld& world, MobSystem& mobs, JavaRandom& rand,
               const SpawnContext& context, bool monsters, SpawnCounters* counters)
{
    // One local, so every counted event below is a null-free increment and the
    // caller that wants none pays a single store.
    SpawnCounters ignored;
    SpawnCounters& count = counters != nullptr ? *counters : ignored;

    // `az.a(Lcn;)V`: the cap is checked **once**, before the three passes, so a
    // tick that starts under it may finish over it by a group or two -- which
    // is the original's and is how a herd of three ends up spawning at 14.
    const int living = monsters ? mobs.monsterCount() : mobs.animalCount();
    if (living >= (monsters ? kMonsterSpawnCap : kAnimalSpawnCap)) {
        return 0;
    }
    if (!context.playerPresent) {
        // No player, no eligible chunks. The original's set would be empty for
        // the same reason.
        return 0;
    }

    const MobType* types = monsters ? kMonsterTypes : kAnimalTypes;
    const int typeCount = monsters ? kMonsterTypeCount : kAnimalTypeCount;

    const i32 centreX = MathHelper::floorDouble(context.playerX / 16.0);
    const i32 centreZ = MathHelper::floorDouble(context.playerZ / 16.0);

    // The set is rebuilt per pass in the jar and is the same set each time, so
    // its order is computed once here. See `eligibleOrder`: it is the jar's
    // hash order, and with `az`'s early return it is the only thing that
    // decides which part of the square a monster can come out of.
    i8 orderDx[kEligibleChunks];
    i8 orderDz[kEligibleChunks];
    eligibleOrder(centreX, centreZ, orderDx, orderDz);

    int spawned = 0;
    for (int pass = 0; pass < kSpawnPassesPerTick; ++pass) {
        ++count.passes;

        bool aborted = false;
        for (int slot = 0; slot < kEligibleChunks && !aborted; ++slot) {
            const i32 dx = orderDx[slot];
            const i32 dz = orderDz[slot];
            if (rand.nextInt(kSpawnChunkOdds) != 0) {
                continue;
            }
            const i32 chunkX = centreX + dx;
            const i32 chunkZ = centreZ + dz;
            if (!world.chunkResident(chunkX, chunkZ)) {
                continue;
            }
            ++count.chunksTried;

            const MobType type = types[rand.nextInt(typeCount)];

            // `az.a(Lcn;II)Lmt;` -- getRandomSpawningPointInChunk, and
            // `k`'s override of it. The animals' y is a flat `rand(128)`,
            // which is why most draws land in rock; the monsters' is
            // **`rand(rand(120) + 8)`**, whose inner draw picks a ceiling
            // and whose outer picks under it, so the distribution piles up
            // near the bedrock and a monster pass reaches the group loop far
            // more often than an animal one does.
            const i32 x = chunkX * 16 + rand.nextInt(16);
            const int y = monsters
                              ? rand.nextInt(rand.nextInt(kMonsterHeightCeil)
                                             + kMonsterHeightFloor)
                              : rand.nextInt(kSpawnHeightSpan);
            const i32 z = chunkZ * 16 + rand.nextInt(16);

            // **These two `return`s end the pass**, not the chunk. See the
            // header: it is most of why animals arrive slowly.
            if (opaqueCube(world, x, y, z)) {
                ++count.abortedSolid;
                aborted = true;
                break;
            }
            if (world.blockAt(x, y, z) != block::kAir) {
                ++count.abortedSolid;
                aborted = true;
                break;
            }

            for (int group = 0; group < kSpawnGroups; ++group) {
                i32 gx = x;
                int gy = y;
                i32 gz = z;
                for (int attempt = 0; attempt < kSpawnGroupTries; ++attempt) {
                    gx += rand.nextInt(kSpawnScatter) - rand.nextInt(kSpawnScatter);
                    // `rand(1) - rand(1)`, which is zero every time. The
                    // draws are made because the original makes them.
                    gy += rand.nextInt(1) - rand.nextInt(1);
                    gz += rand.nextInt(kSpawnScatter) - rand.nextInt(kSpawnScatter);

                    ++count.positions;
                    if (!opaqueCube(world, gx, gy - 1, gz) || opaqueCube(world, gx, gy, gz)
                        || liquidAt(world, gx, gy, gz)
                        || opaqueCube(world, gx, gy + 1, gz)) {
                        ++count.noFloor;
                        continue;
                    }

                    const double fx = double(gx) + 0.5;
                    const double fy = double(gy);
                    const double fz = double(gz) + 0.5;

                    // No player within 24 blocks, and not within 24 of the
                    // world's spawn point -- so a player standing in a
                    // field never watches one appear.
                    const double px = context.playerX - fx;
                    const double py = context.playerY - fy;
                    const double pz = context.playerZ - fz;
                    if (px * px + py * py + pz * pz
                        < kSpawnPlayerClear * kSpawnPlayerClear) {
                        ++count.tooNear;
                        continue;
                    }
                    const double sx = fx - double(context.spawnX);
                    const double sy = fy - double(context.spawnY);
                    const double sz = fz - double(context.spawnZ);
                    if (sx * sx + sy * sy + sz * sz < kSpawnPointClearSq) {
                        ++count.tooNear;
                        continue;
                    }

                    const float yaw = rand.nextFloat() * 360.0f;

                    if (!monsters) {
                        if (!canAnimalSpawnAt(world, mobs, type, fx, fy, fz)) {
                            ++count.checkRejected;
                            continue;
                        }
                        if (mobs.spawn(world, type, fx, fy, fz, yaw)) {
                            ++spawned;
                            ++count.spawned;
                        }
                        continue;
                    }

                    // **Construct, place, then ask** -- see
                    // `canMonsterSpawnAt`. A refusal takes the entity back
                    // out again, which is `az` dropping it on the floor.
                    if (!mobs.spawn(world, type, fx, fy, fz, yaw)) {
                        continue;
                    }
                    const int index = mobs.count() - 1;

                    // `getCanSpawnHere` holds the candidate out of the
                    // world itself -- see `NotYetInTheWorld`. Getting that
                    // wrong refused **every** monster and nothing else, and
                    // passed every host test, because the candidate's own
                    // box is the first thing the world's entity query
                    // finds and a bare test world has no such query.
                    if (!canMonsterSpawnAt(world, mobs, index, rand, context)) {
                        mobs.despawn(index);
                        ++count.checkRejected;
                        continue;
                    }
                    ++spawned;
                    ++count.spawned;

                    // **The spider jockey**, and it is in `az` rather than
                    // in `k`: one spawned spider in a hundred gets a
                    // skeleton on its back, and the skeleton is added
                    // without any spawn check of its own.
                    if (type == MobType::Spider && rand.nextInt(kJockeyOdds) == 0) {
                        if (mobs.spawn(world, MobType::Skeleton, fx, fy, fz,
                                       mobs[index].yaw)) {
                            ++spawned;
                            ++count.spawned;
                            mobs.mountOn(mobs.count() - 1, index);
                        }
                    }
                }
            }
        }
    }
    return spawned;
}

int spawnAnimals(tick::TickWorld& world, MobSystem& mobs, JavaRandom& rand,
                 const SpawnContext& context, SpawnCounters* counters)
{
    return runSpawner(world, mobs, rand, context, false, counters);
}

int spawnMonsters(tick::TickWorld& world, MobSystem& mobs, JavaRandom& rand,
                  const SpawnContext& context, SpawnCounters* counters)
{
    return runSpawner(world, mobs, rand, context, true, counters);
}

}  // namespace mc::entity
