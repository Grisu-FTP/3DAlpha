#pragma once

// **The block that makes monsters** -- `bd` (TileEntityMobSpawner), `r` (its
// renderer) and the half of `ic` they need.
//
// This is the *block* spawner and not `az`/`k`, which is the world's own
// SpawnerAnimals and lives in core/entity/mob_spawn.hpp. The two share nothing
// but the word: `az` sweeps a 9 x 9 of chunks looking for somewhere dark, and
// `bd` is one cell that counts down and throws four candidates at the ground
// around itself. A dungeon has one of these at its centre and a1.1.2 generates
// them nowhere else.
//
// **It lives beside the mobs and not beside the signs**, which is the one place
// this departs from the argument in core/world/sign_store.hpp. A sign is data
// that belongs to a block and never ticks, so it sits in core/world with no
// dependency on anything alive. A spawner ticks, reads the mob pool, and adds
// to it; putting it in core/world would point that layer at core/entity. **Only
// two of the four tile entities tick at all** -- `ic.b()` is a no-op and only
// `ke` (furnace, not ported) and `bd` override it -- which is the whole reason
// the tick list is worth keeping separate from the contents table. See
// docs/status.md, the tile-entity note.
//
// ## What `bd.b()` actually does, in order
//
//   1. `d = c` -- last frame's rotation becomes this frame's previous.
//   2. **`anyPlayerInRange`**: `World.getClosestPlayer(x+.5, y+.5, z+.5, 16)`.
//      Null and the method returns *before everything else*, so a spawner
//      nobody is near does not turn, does not smoke and does not count down.
//   3. One `smoke` and one `flame` at a uniformly random point in the cell.
//   4. `c += 1000f / (delay + 200f)`, computed in **float** and added as a
//      double, then wrapped past 360 with `d` carried down alongside it.
//   5. `delay == -1` seeds a delay; `delay > 0` spends one and returns.
//   6. Four attempts, and **all four run** unless one of them bails:
//      * build the mob named by `EntityId`; a name nothing knows returns,
//      * count that *same kind* within the cell grown by (8, 4, 8) and give up
//        at six -- with a fresh delay, so a full room keeps costing the tick,
//      * scatter to `(x + (nextDouble()-nextDouble())*4, y + nextInt(3)-1,
//        z + (nextDouble()-nextDouble())*4)` at a random heading,
//      * ask `getCanSpawnHere`, and on a yes add it, throw twenty smoke/flame
//        pairs around the cell, give the mob its own twenty `explode` puffs and
//        seed a fresh delay -- **without stopping**, so one firing can place
//        more than one mob.
//
// **The draws are the world's, not the tile entity's.** Every `nextFloat`,
// `nextDouble` and `nextInt` above is `World.rand` -- the same generator `az`
// uses and the same one block ticks use -- so a spawner's scatter is observable
// in everything else the tick does and the order here is the jar's.
//
// **Four things on the list this does not do**, each because a1.1.2 does not:
// there is no `requiredPlayerRange`, no `spawnCount`, no `maxNearbyEntities`
// and no `minSpawnDelay`/`maxSpawnDelay` -- those are the 1.x spawner's NBT and
// every number here is a literal in the bytecode. Nor is there any way in the
// game to change `EntityId`; a spawner placed by hand is the constructor's
// `"Pig"` for ever, which is exactly what a1.1.2 does with one.

#include "core/entity/mob.hpp"
#include "core/entity/mob_spawn.hpp"
#include "core/util/segmented_pool.hpp"
#include "core/util/types.hpp"
#include "core/world/chunk.hpp"
#include "core/world/tile_entity.hpp"

#include <string_view>
#include <vector>

namespace mc::tick {
class TickWorld;
}

namespace mc::entity {

// `ic`'s registry name, which is what the `id` of a saved compound holds.
inline constexpr const char* kMobSpawnerTileId = "MobSpawner";

// `bd`'s constructor: `"Pig"`, and a delay of 20 -- **not** `-1`, so a spawner
// that has just been built fires one second after a player walks up to it
// rather than waiting out a fresh 200-to-800.
inline constexpr int kSpawnerStartDelay = 20;

// `World.getClosestPlayer(..., 16.0)`. A *sphere* about the cell centre, so a
// player level with the block gets sixteen blocks and one above it rather less.
inline constexpr double kSpawnerPlayerRange = 16.0;

// `updateDelay()` -- `200 + rand.nextInt(600)`, which is ten seconds to forty.
inline constexpr int kSpawnerDelayBase = 200;
inline constexpr int kSpawnerDelaySpread = 600;

// The literal `4` the attempt loop counts to, and the `6` the crowd check
// refuses at.
inline constexpr int kSpawnerAttempts = 4;
inline constexpr int kSpawnerNearbyLimit = 6;

// `AxisAlignedBB.getBoundingBox(x, y, z, x+1, y+1, z+1).expand(8, 4, 8)` -- the
// box the crowd check counts in, which is 17 x 9 x 17 blocks about the cell.
inline constexpr double kSpawnerCrowdGrowXZ = 8.0;
inline constexpr double kSpawnerCrowdGrowY = 4.0;

// The scatter: four blocks either way on the flat, and one of three heights.
inline constexpr double kSpawnerScatterXZ = 4.0;
inline constexpr int kSpawnerScatterY = 3;

// The burst on a successful spawn -- twenty smoke/flame pairs, each at
// `cell + 0.5 + (nextFloat() - 0.5) * 2`, so a two-block cube centred on the
// block rather than the one-block cell the idle pair uses.
inline constexpr int kSpawnerBurstPairs = 20;
inline constexpr double kSpawnerBurstSpread = 2.0;

// `c += 1000f / (delay + 200f)`. Both literals are floats in the bytecode and
// the quotient is widened afterwards, which is why they are floats here.
inline constexpr float kSpawnerSpinNumerator = 1000.0f;
inline constexpr float kSpawnerSpinBias = 200.0f;
inline constexpr double kSpawnerSpinWrap = 360.0;

// **How long an `EntityId` may be here.** The tag is an arbitrary Java string;
// the longest `ew` can name is `"FallingSand"` at eleven, and a name this does
// not recognise is inert anyway (see `MobSpawnerBlock::known`). Sixteen bytes
// keeps the record 48 and always leaves room for the terminator.
inline constexpr int kSpawnerEntityIdBytes = 16;

// One `bd`. The rotation is here rather than in the renderer because it is a
// *tick* quantity -- `c` advances in `b()` and the frame only interpolates it --
// and because its rate depends on the delay, so a spawner about to fire visibly
// speeds up.
struct MobSpawnerBlock {
    i32 x = 0;
    int y = 0;
    i32 z = 0;

    // What `EntityId` names, when it names one of the nine this build has.
    MobType mob = MobType::Pig;

    // **False when the name is not one we can build.** a1.1.2 would hand
    // `ew.createEntityInWorld` a `"Giant"` or a `"PrimedTnt"` quite happily and
    // then throw a ClassCastException out of the tick on the second one; an
    // unknown name here is simply inert -- it smokes and turns like any other
    // and then takes `ew`'s null return, which draws nothing and spawns
    // nothing. The alternative is a crash that matches the jar, which is not
    // worth matching.
    bool known = true;

    // The tag verbatim, so the store can say what the world file held even for
    // a name this build has never heard of -- which is what the `--spawns`
    // report prints. NUL-terminated.
    char entityId[kSpawnerEntityIdBytes] = {};

    // `a` -- ticks left. -1 asks for a fresh one on the next tick.
    int delay = kSpawnerStartDelay;

    // `c` and `d` -- the mob's heading and last tick's, in degrees, before the
    // renderer's factor of ten.
    double yaw = 0.0;
    double prevYaw = 0.0;

    // `(sky << 4) | block` at the cell, resampled by whoever owns the store --
    // the same bargain core/world/sign_store.hpp takes and for the same reason.
    u8 light = 0xF0;

    bool used = false;
};

// **No cap**, as with every other pool here: a1.1.2 keeps as many tile entities
// as the world holds. Held from construction so ordinary play never allocates.
class MobSpawnerStore {
public:
    // A dungeon per few hundred chunks, and a render distance holds a couple of
    // hundred: sixteen is comfortably more than a session ever needs resident,
    // and it is not a limit.
    static constexpr int kInitialCapacity = 16;

    // Creates or replaces the spawner at this block, with `bd`'s constructor
    // defaults. Returns the index, or -1 when the heap would not hold another.
    int put(i32 x, int y, i32 z);

    // The same, with a name out of a world file. An unrecognised name is stored
    // and made inert rather than refused -- see `MobSpawnerBlock::known`.
    int put(i32 x, int y, i32 z, std::string_view entityId, int delay);

    // The spawner at this block, or -1.
    int find(i32 x, int y, i32 z) const;

    // Forgets it. Called when the block goes, and when the column leaves the
    // resident grid -- a store that kept it would tick a spawner in ground that
    // is no longer there.
    void erase(i32 x, int y, i32 z);

    // Forgets every spawner inside one column. The release half of the pair
    // above; `erase` cannot do it because a column holds no list of its own.
    void eraseColumn(i32 chunkX, i32 chunkZ);

    // Resamples every spawner's light, from the tick loop rather than a draw.
    // Templated on the world for the reason `SignStore::refreshLight` is.
    template <class Access>
    void refreshLight(const Access& world)
    {
        for (int i = 0; i < spawners_.size(); ++i) {
            MobSpawnerBlock& s = spawners_[i];
            if (!s.used) {
                continue;
            }
            s.light = u8((world.skyLightAt(s.x, s.y, s.z) << 4)
                         | world.blockLightAt(s.x, s.y, s.z));
        }
    }

    void clear() { spawners_.clear(); }

    int count() const { return spawners_.size(); }
    const MobSpawnerBlock& operator[](int i) const { return spawners_[i]; }
    MobSpawnerBlock& at(int i) { return spawners_[i]; }
    u32 refused() const { return refused_; }

private:
    SegmentedPool<MobSpawnerBlock, kInitialCapacity> spawners_;
    u32 refused_ = 0;
};

// **Where the tick's answers went**, for the same reason `SpawnCounters`
// exists: "the spawner in my dungeon does nothing" and "the spawner in my
// dungeon is working and the zombies wandered off" look identical from a
// console and completely different from here.
struct MobSpawnerCounters {
    // Spawners visited, and the ones that had a player within sixteen blocks.
    i32 visited = 0;
    i32 inRange = 0;

    // Firings -- a spawner whose delay reached zero this tick.
    i32 fired = 0;

    // Attempts that gave up because six of that kind were already about.
    i32 crowded = 0;

    // Attempts `getCanSpawnHere` turned down, and mobs actually placed.
    i32 refused = 0;
    i32 spawned = 0;

    // Spawners holding an `EntityId` this build cannot build.
    i32 unknown = 0;
};

// **One 20 Hz tick of `bd.b()` for every spawner in the store.**
//
// `rand` is `World.rand` and every draw comes out of it; `context` is the same
// struct `az` takes, because `getCanSpawnHere` is the same method and wants the
// same difficulty and seed. Returns how many mobs were placed.
//
// `counters` may be null, which costs one predictable branch per counted event.
int tickMobSpawners(MobSpawnerStore& store, tick::TickWorld& world, MobSystem& mobs,
                    JavaRandom& rand, const SpawnContext& context,
                    MobSpawnerCounters* counters = nullptr);

// `ew`'s name table, backwards, over the nine this build has. False for a name
// that is in `ew` but is not an `ge` we model -- `"Giant"`, `"Item"`,
// `"Minecart"` -- and for anything not in it at all.
bool mobTypeForSaveId(std::string_view saveId, MobType* out);

// **Claims the spawners out of a column's decoded `TileEntities`.**
//
// The list is no longer an opaque tag: `world::ChunkColumn::tileEntities` holds
// every tenant modelled (core/world/tile_entity.hpp), so this is a walk over a
// vector rather than a second NBT parse. Compounds of any other kind -- a
// chest, a later version's -- are simply not `MobSpawner` and are stepped over.
//
// An `EntityId` naming a mob this build does not have is kept as a string and
// the spawner stays inert: it turns, it smokes, it never fires, and it goes
// back into the file with its name intact. Returns how many were added.
int readMobSpawners(const std::vector<world::TileEntity>& tiles, MobSpawnerStore& store);

// **...and puts them back**, which is the other half and the reason the list
// had to be modelled at all.
//
// Every spawner the store holds inside this column is written over the column's
// entry, creating one if the column has none -- so a spawner this build placed
// keeps its mob, and a dungeon this build generated reloads as what it
// generated rather than as `bd`'s default `"Pig"`. `Delay` is a Short on disk
// and the countdown is an int, so a delay outside a short's range is clamped
// the way `i2s` would fold it -- it cannot occur: the largest `bd` ever sets is
// 799.
//
// Spawners outside the column are left alone, which is what makes this safe to
// call once per column with one store holding every loaded chunk's.
//
// A spawner block the store never took -- the heap refused it -- keeps
// whatever the file already said rather than being reset to the default.
// Returns how many entries were written.
int writeMobSpawners(const MobSpawnerStore& store, world::ChunkColumn& column);

}  // namespace mc::entity
