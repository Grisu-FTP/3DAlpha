// See mob_spawner.hpp.

#include "core/entity/mob_spawner.hpp"

#include "core/entity/particle.hpp"
#include "core/nbt/nbt.hpp"
#include "core/tick/tick_world.hpp"
#include "core/util/math_helper.hpp"

#include <cstring>

namespace mc::entity {
namespace {

// `World.getClosestPlayer(x, y, z, distance)` -- a **sphere**, and the distance
// is compared squared against `getDistanceSq`, which measures from the player's
// feet. Single player, so "the closest" is "the only".
bool playerWithinRange(const SpawnContext& context, double x, double y, double z)
{
    if (!context.playerPresent) {
        return false;
    }
    const double dx = context.playerX - x;
    const double dy = context.playerY - y;
    const double dz = context.playerZ - z;
    return dx * dx + dy * dy + dz * dz < kSpawnerPlayerRange * kSpawnerPlayerRange;
}

// One `smoke` and one `flame` at the same point, which is every particle this
// tile entity makes -- the idle pair and all twenty of the burst.
void smokeAndFlame(tick::TickWorld& world, double x, double y, double z)
{
    world.spawnParticle(int(ParticleKind::Smoke), x, y, z, 0.0, 0.0, 0.0);
    world.spawnParticle(int(ParticleKind::Flame), x, y, z, 0.0, 0.0, 0.0);
}

// `World.getEntitiesWithinAABB(entity.getClass(), box).size()` -- **the same
// class**, so a zombie spawner is not held back by the skeletons beside it.
int nearbyOfKind(const MobSystem& mobs, MobType type, const AABB& box)
{
    int found = 0;
    for (int i = 0; i < mobs.count(); ++i) {
        if (mobs[i].alive && mobs[i].type == type && mobs[i].body.box.intersects(box)) {
            ++found;
        }
    }
    return found;
}

}  // namespace

bool mobTypeForSaveId(std::string_view saveId, MobType* out)
{
    for (int i = 0; i < kMobTypeCount; ++i) {
        const MobType type = MobType(i);
        if (saveId == mobDef(type).saveId) {
            if (out != nullptr) {
                *out = type;
            }
            return true;
        }
    }
    return false;
}

int readMobSpawners(const std::vector<world::TileEntity>& tiles, MobSpawnerStore& store)
{
    int added = 0;
    for (const world::TileEntity& tile : tiles) {
        if (tile.kind != world::TileEntityKind::MobSpawner) {
            continue;
        }
        // An `EntityId` missing entirely is `bd`'s constructor value, which is
        // what a compound written before the field existed would mean.
        std::string_view entityId = tile.entityId;
        if (entityId.empty()) {
            entityId = mobDef(MobType::Pig).saveId;
        }
        // **A short, and `bd` widens it into an int.** A delay above 32767
        // cannot be written and a negative one is `-1`'s "seed me", which is
        // how a1.1.2 spells "fire as soon as you can".
        if (store.put(tile.x, tile.y, tile.z, entityId, int(tile.delay)) >= 0) {
            ++added;
        }
    }
    return added;
}

int writeMobSpawners(const MobSpawnerStore& store, world::ChunkColumn& column)
{
    const i32 minX = column.x * world::ChunkColumn::kWidth;
    const i32 minZ = column.z * world::ChunkColumn::kWidth;

    int written = 0;
    for (int i = 0; i < store.count(); ++i) {
        const MobSpawnerBlock& s = store[i];
        if (!s.used) {
            continue;
        }
        if (s.x < minX || s.x >= minX + world::ChunkColumn::kWidth || s.z < minZ ||
            s.z >= minZ + world::ChunkColumn::kWidth) {
            continue;
        }
        world::TileEntity* tile = world::findTileEntity(column.tileEntities, s.x, s.y, s.z);
        if (tile == nullptr || tile->kind != world::TileEntityKind::MobSpawner) {
            tile = &world::putTileEntity(column.tileEntities, s.x, s.y, s.z,
                                         world::TileEntityKind::MobSpawner);
        }
        // `s.entityId` is what the file said when the file said anything, and
        // `mobDef(s.mob).saveId` when this build placed the block -- either way
        // it is already the string `bd` would have held.
        tile->entityId = s.entityId;
        tile->delay = i16(s.delay);
        ++written;
    }
    return written;
}

int MobSpawnerStore::find(i32 x, int y, i32 z) const
{
    for (int i = 0; i < spawners_.size(); ++i) {
        const MobSpawnerBlock& s = spawners_[i];
        if (s.used && s.x == x && s.y == y && s.z == z) {
            return i;
        }
    }
    return -1;
}

int MobSpawnerStore::put(i32 x, int y, i32 z)
{
    return put(x, y, z, mobDef(MobType::Pig).saveId, kSpawnerStartDelay);
}

int MobSpawnerStore::put(i32 x, int y, i32 z, std::string_view entityId, int delay)
{
    // Replacing rather than adding, for the reason `SignStore::put` gives: a
    // block broken and rebuilt in the same hole is a different tile entity, and
    // this is the case where `erase` was not called. **The rotation is reset
    // too** -- the jar builds a new `bd`, and carrying the old angle over would
    // leave a freshly placed spawner mid-spin.
    int index = find(x, y, z);
    if (index < 0) {
        MobSpawnerBlock* slot = spawners_.push();
        if (slot == nullptr) {
            ++refused_;
            return -1;
        }
        index = spawners_.size() - 1;
    }

    MobSpawnerBlock& s = spawners_[index];
    s = MobSpawnerBlock{};
    s.x = x;
    s.y = y;
    s.z = z;
    s.delay = delay;
    s.used = true;

    usize n = entityId.size();
    if (n > usize(kSpawnerEntityIdBytes - 1)) {
        n = usize(kSpawnerEntityIdBytes - 1);
    }
    // `memcpy` with a null source is undefined even for zero bytes, and a
    // default-constructed `string_view` has one.
    if (n > 0) {
        std::memcpy(s.entityId, entityId.data(), n);
    }
    s.entityId[n] = '\0';

    // A name the truncation mangled cannot match, which is the right answer:
    // it is a name this build does not have.
    s.known = mobTypeForSaveId(std::string_view(s.entityId, n), &s.mob);
    return index;
}

void MobSpawnerStore::erase(i32 x, int y, i32 z)
{
    const int index = find(x, y, z);
    if (index < 0) {
        return;
    }
    spawners_.swapRemove(index);
    spawners_.trim();
}

void MobSpawnerStore::eraseColumn(i32 chunkX, i32 chunkZ)
{
    // Backwards, because `swapRemove` moves the last entry into the hole.
    for (int i = spawners_.size() - 1; i >= 0; --i) {
        const MobSpawnerBlock& s = spawners_[i];
        if (s.used && (s.x >> 4) == chunkX && (s.z >> 4) == chunkZ) {
            spawners_.swapRemove(i);
        }
    }
    spawners_.trim();
}

int tickMobSpawners(MobSpawnerStore& store, tick::TickWorld& world, MobSystem& mobs,
                    JavaRandom& rand, const SpawnContext& context,
                    MobSpawnerCounters* counters)
{
    int placed = 0;

    for (int index = 0; index < store.count(); ++index) {
        MobSpawnerBlock& s = store.at(index);
        if (!s.used) {
            continue;
        }
        if (counters != nullptr) {
            ++counters->visited;
        }

        // `d = c`, and it happens **before** the range test -- so a spawner
        // nobody is near still has a consistent pair of angles for the frame
        // that interpolates them.
        s.prevYaw = s.yaw;

        const double cx = double(s.x) + 0.5;
        const double cy = double(s.y) + 0.5;
        const double cz = double(s.z) + 0.5;
        if (!playerWithinRange(context, cx, cy, cz)) {
            continue;
        }
        if (counters != nullptr) {
            ++counters->inRange;
        }

        // The idle pair, at a uniformly random point in the cell. Named rather
        // than nested: three draws in one argument list have no guaranteed
        // order in C++ and a fixed one in the jar.
        const float px = rand.nextFloat();
        const float py = rand.nextFloat();
        const float pz = rand.nextFloat();
        smokeAndFlame(world, double(s.x) + double(px), double(s.y) + double(py),
                      double(s.z) + double(pz));

        // **Float arithmetic, widened afterwards.** `1000.0F / (a + 200.0F)`
        // is computed in single precision and only then added to a double, so
        // doing it in double here would drift a spinning mob off the jar's
        // angle within a minute.
        s.yaw += double(kSpawnerSpinNumerator / (float(s.delay) + kSpawnerSpinBias));
        while (s.yaw > kSpawnerSpinWrap) {
            s.yaw -= kSpawnerSpinWrap;
            s.prevYaw -= kSpawnerSpinWrap;
        }

        if (s.delay == -1) {
            s.delay = kSpawnerDelayBase + rand.nextInt(kSpawnerDelaySpread);
        }
        if (s.delay > 0) {
            --s.delay;
            continue;
        }
        if (counters != nullptr) {
            ++counters->fired;
        }

        // A name this build cannot build is `ew` returning null: the method
        // **returns**, so no delay is seeded and the spawner retries next tick.
        // That is the jar's, and it costs nothing -- the cell is already known
        // to be in range and the return is before every other draw.
        if (!s.known) {
            if (counters != nullptr) {
                ++counters->unknown;
            }
            continue;
        }

        const AABB crowd = AABB{double(s.x),     double(s.y),     double(s.z),
                                double(s.x) + 1, double(s.y) + 1, double(s.z) + 1}
                               .expand(kSpawnerCrowdGrowXZ, kSpawnerCrowdGrowY,
                                       kSpawnerCrowdGrowXZ);

        for (int attempt = 0; attempt < kSpawnerAttempts; ++attempt) {
            // **Counted every attempt**, not once -- the entity is rebuilt at
            // the top of each pass and the count is taken with it, so a
            // spawner that fills its own room stops on the attempt that does
            // it rather than at the start of the next firing.
            if (nearbyOfKind(mobs, s.mob, crowd) >= kSpawnerNearbyLimit) {
                if (counters != nullptr) {
                    ++counters->crowded;
                }
                s.delay = kSpawnerDelayBase + rand.nextInt(kSpawnerDelaySpread);
                break;
            }

            // `(nextDouble() - nextDouble()) * 4`, which is a triangular
            // distribution about the cell and not a uniform one: most
            // candidates land near the spawner and the corners of the eight-
            // block square are rare.
            const double a0 = rand.nextDouble();
            const double a1 = rand.nextDouble();
            const double sx = double(s.x) + (a0 - a1) * kSpawnerScatterXZ;
            const double sy = double(s.y + rand.nextInt(kSpawnerScatterY) - 1);
            const double b0 = rand.nextDouble();
            const double b1 = rand.nextDouble();
            const double sz = double(s.z) + (b0 - b1) * kSpawnerScatterXZ;
            const float yaw = rand.nextFloat() * 360.0f;

            // **The animal and the monster halves split here for the reason
            // core/entity/mob_spawn.hpp argues at length**: `ag.a()Z` reads
            // nothing the constructor drew and can be asked about a position,
            // while `ma.a()Z` reads the size its own constructor drew and
            // cannot. So an animal is asked first and a monster is built,
            // asked, and taken back out again.
            bool accepted = false;
            if (!mobDef(s.mob).hostile) {
                if (canAnimalSpawnAt(world, mobs, s.mob, sx, sy, sz)
                    && mobs.spawn(world, s.mob, sx, sy, sz, yaw)) {
                    accepted = true;
                }
            } else if (mobs.spawn(world, s.mob, sx, sy, sz, yaw)) {
                const int spawned = mobs.count() - 1;
                if (canMonsterSpawnAt(world, mobs, spawned, rand, context)) {
                    accepted = true;
                } else {
                    mobs.despawn(spawned);
                }
            }

            if (!accepted) {
                if (counters != nullptr) {
                    ++counters->refused;
                }
                continue;
            }

            ++placed;
            if (counters != nullptr) {
                ++counters->spawned;
            }

            // Twenty smoke/flame pairs about the *cell centre*, spread two
            // blocks -- a different point and a different spread from the idle
            // pair above, which is why this is not the same call.
            for (int n = 0; n < kSpawnerBurstPairs; ++n) {
                const float bx = rand.nextFloat();
                const float by = rand.nextFloat();
                const float bz = rand.nextFloat();
                smokeAndFlame(world,
                              cx + (double(bx) - 0.5) * kSpawnerBurstSpread,
                              cy + (double(by) - 0.5) * kSpawnerBurstSpread,
                              cz + (double(bz) - 0.5) * kSpawnerBurstSpread);
            }

            // `ge.z()` -- the mob's own puff, out of the mob's own random.
            mobs.explosionPuff(world, mobs.count() - 1);

            // **A fresh delay, and the loop keeps going.** `updateDelay()` is
            // called here rather than after the loop, so four attempts that all
            // succeed place four mobs and seed four delays, the last of which
            // is the one that counts.
            s.delay = kSpawnerDelayBase + rand.nextInt(kSpawnerDelaySpread);
        }
    }

    return placed;
}

}  // namespace mc::entity
