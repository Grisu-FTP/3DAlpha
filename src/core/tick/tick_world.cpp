#include "core/tick/tick_world.hpp"

#include "core/tick/behaviour.hpp"
#include "core/tick/furnace.hpp"
#include "core/tick/redstone.hpp"
#include "core/world/daylight.hpp"

namespace mc::tick {

namespace {

// The original's bound on how far a coordinate can be from the origin, from
// `cn.a(III)I`. Outside it every read is air and every write is refused, and
// that is a1.1.2's edge of the world -- long past the Far Lands, which are a
// generator artefact rather than a limit.
constexpr i32 kWorldLimit = 32000000;

// **The asymmetry is the original's and is not a typo here.** `cn.a(III)I`
// tests `x >= 32000000` and `z > 32000000`, so the world is one block wider on
// +z than on +x. Nothing can reach either edge, so it has never mattered; it is
// reproduced rather than tidied because tidying it would be the first place to
// look if a coordinate ever disagreed.
inline bool inWorld(i32 x, int y, i32 z)
{
    if (x < -kWorldLimit || z < -kWorldLimit) return false;
    if (x >= kWorldLimit || z > kWorldLimit) return false;
    return y >= 0 && y < TickWorld::kHeight;
}

// Arithmetic shift, which is what Java's `>> 4` on a block coordinate does and
// what C++ `/ 16` does not: -1 / 16 is 0 and -1 >> 4 is -1. Chunk -1 is where
// block -1 lives, so the difference is a whole chunk of wrong world.
inline i32 chunkOf(i32 block) { return block >> 4; }
inline int localOf(i32 block) { return int(block & 15); }

}  // namespace

TickWorld::TickWorld(TickAccess access, i64 seed, usize schedulerCapacity)
    : access_(access), scheduler_(schedulerCapacity), random_(seed)
{
    // Any value will do -- the original takes a wall-clock one -- but it must
    // not be zero, because 0 * 3 + addend walks a short, visibly striped path
    // through the first few chunks it ticks.
    JavaRandom lcgSeed(seed ^ 0x5DEECE66DLL);
    updateLcg_ = u32(lcgSeed.nextInt());
    skyDarken_ = world::skyLightSubtracted(time_);

    // Once, here, so the deferred queue never allocates while a tick is running.
    deferredNotify_.reserve(kDeferredNotifyCapacity);
}

world::ChunkColumn* TickWorld::columnFor(i32 chunkX, i32 chunkZ) const
{
    if (cachedValid_ && cachedX_ == chunkX && cachedZ_ == chunkZ) return cached_;
    if (access_.column == nullptr) return nullptr;
    cached_ = access_.column(access_.ctx, chunkX, chunkZ);
    cachedX_ = chunkX;
    cachedZ_ = chunkZ;
    cachedValid_ = true;
    return cached_;
}

world::ChunkColumn* TickWorld::columnAtBlock(i32 x, i32 z) const
{
    return columnFor(chunkOf(x), chunkOf(z));
}

bool TickWorld::chunkResident(i32 chunkX, i32 chunkZ) const
{
    return columnFor(chunkX, chunkZ) != nullptr;
}

block::BlockId TickWorld::blockAt(i32 x, int y, i32 z) const
{
    if (!inWorld(x, y, z)) return block::kAir;
    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return block::kAir;
    return c->block(localOf(x), y, localOf(z));
}

u8 TickWorld::dataAt(i32 x, int y, i32 z) const
{
    if (!inWorld(x, y, z)) return 0;
    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return 0;
    return c->blockData(localOf(x), y, localOf(z));
}

u8 TickWorld::skyLightAt(i32 x, int y, i32 z) const
{
    if (!inWorld(x, y, z)) return 15;
    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return 0;
    return c->skyLight(localOf(x), y, localOf(z));
}

u8 TickWorld::blockLightAt(i32 x, int y, i32 z) const
{
    if (!inWorld(x, y, z)) return 0;
    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return 0;
    return c->blockLight(localOf(x), y, localOf(z));
}

int TickWorld::lightValue(i32 x, int y, i32 z, bool checkNeighbours) const
{
    if (x < -kWorldLimit || z < -kWorldLimit || x >= kWorldLimit || z > kWorldLimit) {
        return 15;
    }

    // The original's one special case, and it is not decoration: a slab and a
    // farmland block are full-height in the light array but not in the world,
    // so a plant standing on one would read the darkness inside it. Both
    // answer with the brightest of the five cells around them instead.
    if (checkNeighbours) {
        const block::BlockId id = blockAt(x, y, z);
        if (id == block::BlockId(mcver::Block::Slab) ||
            id == block::BlockId(mcver::Block::Farmland)) {
            int best = lightValue(x, y + 1, z, false);
            const int candidates[4] = {
                lightValue(x + 1, y, z, false),
                lightValue(x - 1, y, z, false),
                lightValue(x, y, z + 1, false),
                lightValue(x, y, z - 1, false),
            };
            for (int v : candidates) {
                if (v > best) best = v;
            }
            return best;
        }
    }

    if (y < 0) return 0;
    if (y >= kHeight) {
        const int v = 15 - skyDarken_;
        return v < 0 ? 0 : v;
    }

    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return 0;
    const int lx = localOf(x);
    const int lz = localOf(z);
    return world::effectiveLightLevel(c->skyLight(lx, y, lz), c->blockLight(lx, y, lz),
                                      skyDarken_);
}

bool TickWorld::canSeeSky(i32 x, int y, i32 z) const
{
    if (y >= kHeight) return true;
    if (y < 0) return false;
    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return false;
    return y >= int(c->heightMap[usize(localOf(z) * 16 + localOf(x))]);
}

// The height map has to move when a block does, because `canSeeSky` is read by
// every plant and by the snow pass, and a stale one makes a cell that is now
// under a roof still count as open sky. This is the same rule
// `world::computeHeightMap` uses for a whole column, applied to one cell: walk
// down while the block below is fully transparent.
//
// **Light is repropagated, but not from here.** `world::LightUpdater` does it,
// driven off the same `access_.changed` callback this write already makes: the
// height map has to move first, because sky exposure is read from it, which is
// why that ordering is load-bearing rather than incidental.
//
// It used to not happen at all -- `LightEngine` solves a whole column against a
// 3x3 window and there was no incremental path -- so a block a tick placed left
// the stored sky and block light exactly as they were. Lava flowed through a
// world that stayed dark, and `lightValue` below kept answering with numbers
// that no longer described the world, which every plant and the snow pass read.
void TickWorld::refreshHeight(world::ChunkColumn& column, int lx, int y, int lz)
{
    const usize slot = usize(lz * 16 + lx);
    const int height = int(column.heightMap[slot]);
    const bool blocks = block::def(column.block(lx, y, lz)).opacity != 0;

    // The common cases are answered without touching the column at all, which
    // matters more than it looks: a spreading fluid writes thousands of blocks
    // a second and a full 128-deep rescan of a palette-compressed section for
    // each of them would cost more than the fluid does.
    if (blocks) {
        if (y + 1 > height) column.heightMap[slot] = u8(y + 1);
        return;
    }
    if (y + 1 != height) return;  // the cell was never the one holding the map up

    int next = y;
    while (next > 0 && block::def(column.block(lx, next - 1, lz)).opacity == 0) --next;
    column.heightMap[slot] = u8(next);
}

int TickWorld::precipitationHeight(i32 x, i32 z) const
{
    const world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return -1;
    const int lx = localOf(x);
    const int lz = localOf(z);
    for (int y = kHeight - 1; y > 0; --y) {
        const block::BlockId id = c->block(lx, y, lz);
        if (id == block::kAir) continue;
        const block::BlockDef& d = block::def(id);
        // `Material.isSolid() || Material.isLiquid()`. We carry solidity in
        // the table; "liquid" is the fluid render type, which in a1.1.2 is
        // exactly the four fluid blocks and nothing else.
        if (d.solid || d.render == block::RenderType::Fluid) return y + 1;
    }
    return -1;
}

bool TickWorld::chunksExist(i32 x1, int y1, i32 z1, i32 x2, int y2, i32 z2) const
{
    if (y2 < 0 || y1 >= kHeight) return false;
    const i32 cx1 = chunkOf(x1), cx2 = chunkOf(x2);
    const i32 cz1 = chunkOf(z1), cz2 = chunkOf(z2);
    for (i32 cx = cx1; cx <= cx2; ++cx) {
        for (i32 cz = cz1; cz <= cz2; ++cz) {
            if (columnFor(cx, cz) == nullptr) return false;
        }
    }
    return true;
}

bool TickWorld::setBlockRaw(i32 x, int y, i32 z, block::BlockId id)
{
    // `ga.a(IIII)Z` -- Chunk.setBlockID, which **clears the metadata**. That
    // is not incidental: a fluid block replaced by air and then by fluid again
    // would otherwise inherit the old flow level, and a torch would keep the
    // face it used to hang on.
    return writeBlock(x, y, z, id, 0, true);
}

bool TickWorld::setBlockAndDataRaw(i32 x, int y, i32 z, block::BlockId id, u8 data)
{
    // `ga.a(IIIII)Z` -- the same, keeping the metadata it is given. The
    // metadata is written *before* onBlockAdded runs, because a fluid that has
    // just appeared reads its own level.
    return writeBlock(x, y, z, id, data, true);
}

bool TickWorld::writeBlock(i32 x, int y, i32 z, block::BlockId id, u8 data, bool setData)
{
    if (!inWorld(x, y, z)) return false;
    world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return false;
    const int lx = localOf(x);
    const int lz = localOf(z);

    const block::BlockId old = c->block(lx, y, lz);
    if (old == id && (!setData || c->blockData(lx, y, lz) == data)) return true;

    if (access_.beforeWrite != nullptr) {
        access_.beforeWrite(access_.ctx, x, y, z, old, c->blockData(lx, y, lz));
    }
    c->setBlock(lx, y, lz, id);

    // The original runs onBlockRemoval here, before the metadata is touched,
    // so the departing block can still read its own.
    if (old != block::kAir) blockRemoved(*this, x, y, z, old);

    if (setData) c->setBlockData(lx, y, lz, data);
    refreshHeight(*c, lx, y, lz);
    ++stats_.blocksChanged;
    if (access_.changed != nullptr) access_.changed(access_.ctx, x, y, z);

    if (id != block::kAir) blockAdded(*this, x, y, z, id);
    return true;
}

bool TickWorld::setDataRaw(i32 x, int y, i32 z, u8 data)
{
    if (!inWorld(x, y, z)) return false;
    world::ChunkColumn* c = columnAtBlock(x, z);
    if (c == nullptr) return false;
    const int lx = localOf(x);
    const int lz = localOf(z);
    if (c->blockData(lx, y, lz) == data) return true;
    if (access_.beforeWrite != nullptr) {
        access_.beforeWrite(access_.ctx, x, y, z, c->block(lx, y, lz), c->blockData(lx, y, lz));
    }
    c->setBlockData(lx, y, lz, data);
    if (access_.changed != nullptr) access_.changed(access_.ctx, x, y, z);
    return true;
}

bool TickWorld::setBlockWithNotify(i32 x, int y, i32 z, block::BlockId id)
{
    if (!setBlockRaw(x, y, z, id)) return false;
    notifyNeighbours(x, y, z, id);
    return true;
}

bool TickWorld::setDataWithNotify(i32 x, int y, i32 z, u8 data)
{
    if (!setDataRaw(x, y, z, data)) return false;
    notifyNeighbours(x, y, z, blockAt(x, y, z));
    return true;
}

bool TickWorld::setBlockAndDataWithNotify(i32 x, int y, i32 z, block::BlockId id, u8 data)
{
    if (!setBlockAndDataRaw(x, y, z, id, data)) return false;
    notifyNeighbours(x, y, z, id);
    return true;
}

void TickWorld::notifyNeighboursNow(i32 x, int y, i32 z, block::BlockId fromId)
{
    if (!enterCascade(kNotifyLevelBytes)) {
        // Too deep to recurse. Remember the centre and come back to it once the
        // stack has unwound -- see kCascadeStackBudget for why there is a limit
        // at all, and drainDeferredNotifications for when this runs.
        if (deferredNotify_.size() < kDeferredNotifyCapacity) {
            deferredNotify_.push_back(PendingNotify{x, z, i16(y), fromId});
            ++stats_.notifyDeferred;
        } else {
            ++stats_.notifyDropped;
        }
        return;
    }

    neighbourChanged(*this, x - 1, y, z, fromId);
    neighbourChanged(*this, x + 1, y, z, fromId);
    neighbourChanged(*this, x, y - 1, z, fromId);
    neighbourChanged(*this, x, y + 1, z, fromId);
    neighbourChanged(*this, x, y, z - 1, fromId);
    neighbourChanged(*this, x, y, z + 1, fromId);

    leaveCascade(kNotifyLevelBytes);
}

void TickWorld::drainDeferredNotifications()
{
    // Only at the bottom of the stack, and never reentrantly: the calls below
    // can defer again, and appending to the queue while walking it is the whole
    // point -- it is what turns the recursion into iteration.
    if (cascadeBytes_ != 0 || drainingNotify_) return;

    drainingNotify_ = true;
    while (deferredNotifyHead_ < deferredNotify_.size()) {
        const PendingNotify pending = deferredNotify_[deferredNotifyHead_++];
        notifyNeighboursNow(pending.x, int(pending.y), pending.z, pending.fromId);
    }
    deferredNotify_.clear();
    deferredNotifyHead_ = 0;
    drainingNotify_ = false;
}

void TickWorld::notifyNeighbours(i32 x, int y, i32 z, block::BlockId fromId)
{
    notifyNeighboursNow(x, y, z, fromId);
    drainDeferredNotifications();
}

void TickWorld::scheduleBlockUpdate(i32 x, int y, i32 z, block::BlockId id)
{
    // The original refuses to schedule anything whose 17-cube neighbourhood is
    // not resident, which is what stops a fluid at the frontier from queueing
    // work against ground that does not exist yet.
    constexpr int r = 8;
    if (!chunksExist(x - r, y - r, z - r, x + r, y + r, z + r)) {
        ++stats_.scheduledDropped;
        return;
    }
    const i64 due = id > 0 ? time_ + i64(block::def(id).tickRate) : time_;
    if (!scheduler_.schedule(x, y, z, id, due)) {
        // Already queued is the common case and is not a drop; a full pool is.
        if (scheduler_.size() >= scheduler_.capacity()) ++stats_.scheduledDropped;
    }
}

void TickWorld::runScheduled()
{
    // `cn.a(Z)Z`. The count is taken once, before anything runs, so updates a
    // behaviour schedules during this tick wait for the next one -- which is
    // what stops flowing water from crossing a whole lake in a single tick.
    int budget = int(scheduler_.size());
    if (budget > TickScheduler::kMaxPerTick) budget = TickScheduler::kMaxPerTick;

    for (int i = 0; i < budget; ++i) {
        if (!scheduler_.dueAt(time_)) break;
        const ScheduledTick e = scheduler_.pop();

        constexpr int r = 8;
        if (!chunksExist(e.x - r, e.y - r, e.z - r, e.x + r, e.y + r, e.z + r)) continue;

        // The entry names the block it was scheduled for. If something else is
        // there now, the update belonged to a block that no longer exists.
        const block::BlockId id = blockAt(e.x, e.y, e.z);
        if (id != e.blockId || id == block::kAir) continue;

        ++stats_.scheduledRun;
        updateTick(*this, e.x, e.y, e.z, id, random_);
    }
}

void TickWorld::randomTickChunk(world::ChunkColumn& column)
{
    const i32 x0 = column.x * 16;
    const i32 z0 = column.z * 16;

    // **Which sections can produce a random tick at all, asked once.** See
    // Section::mayTickRandomly. The eighty samples below then reject against a
    // stack array instead of decoding a palette entry at a random offset in the
    // column, which is where this loop was spending its time.
    //
    // **The random sequence is untouched.** nextLcg() is still called exactly
    // eighty times per column whatever the answers are, so the positions
    // sampled are the same positions in the same order; only the work done at a
    // position that could never have ticked is skipped. `randomTicks` still
    // counts positions sampled, not lookups performed.
    bool live[world::ChunkColumn::kSectionCount];
    bool anyLive = false;
    for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
        live[sy] = column.section(sy).mayTickRandomly();
        anyLive = anyLive || live[sy];
    }

    for (int i = 0; i < kRandomTicksPerChunk; ++i) {
        const i32 r = nextLcg();
        ++stats_.randomTicks;
        if (!anyLive) continue;

        const int lx = r & 15;
        const int lz = (r >> 8) & 15;
        const int y = (r >> 16) & (kHeight - 1);
        if (!live[y / world::Section::kSize]) continue;

        const block::BlockId id = column.block(lx, y, lz);
        if (!block::ticksRandomly(id)) continue;

        ++stats_.randomTicksRun;
        updateTick(*this, x0 + lx, y, z0 + lz, id, random_);
    }
}

void TickWorld::snowAndIce(world::ChunkColumn& column)
{
    // `cn.h()`, the block between the cave sound and the random ticks. One
    // position per chunk per tick, on a one-in-four roll, and only in a world
    // that rolled SnowCovered when it was created.
    if (!snowCovered_) return;
    if (random_.nextInt(4) != 0) return;

    const i32 x0 = column.x * 16;
    const i32 z0 = column.z * 16;
    const i32 r = nextLcg();
    const int lx = r & 15;
    const int lz = (r >> 8) & 15;
    const i32 x = x0 + lx;
    const i32 z = z0 + lz;
    const int y = precipitationHeight(x, z);
    if (y < 0 || y >= kHeight) return;

    // Block light, not the day-adjusted value: snow forms in the dark of night
    // as well as in shade, and reading the wrong one covers the world.
    if (column.blockLight(lx, y, lz) >= 10) return;

    const block::BlockId below = y > 0 ? column.block(lx, y - 1, lz) : block::kAir;

    if (column.block(lx, y, lz) == block::kAir &&
        canPlaceSnowAt(*this, x, y, z)) {
        setBlockWithNotify(x, y, z, block::BlockId(mcver::Block::SnowLayer));
    }

    if (below == block::BlockId(mcver::Block::Water) &&
        column.blockData(lx, y - 1, lz) == 0) {
        setBlockWithNotify(x, y - 1, z, block::BlockId(mcver::Block::Ice));
    }
}

bool TickWorld::providesPowerTo(i32 x, int y, i32 z, int side) const
{
    const block::BlockId id = blockAt(x, y, z);
    if (id == block::kAir) return false;
    return tick::providesPowerTo(*this, x, y, z, side, id);
}

bool TickWorld::indirectlyProvidesPowerTo(i32 x, int y, i32 z, int side) const
{
    // **An opaque cube conducts.** This is the whole reason a torch under a
    // block powers what is on top of it, and it is why the query has to be
    // distinct from the direct one rather than a wrapper round it.
    if (opaqueAt(x, y, z)) return isPowered(x, y, z);

    const block::BlockId id = blockAt(x, y, z);
    if (id == block::kAir) return false;
    return tick::indirectlyProvidesPowerTo(*this, x, y, z, side, id);
}

bool TickWorld::isPowered(i32 x, int y, i32 z) const
{
    return providesPowerTo(x, y - 1, z, 0) || providesPowerTo(x, y + 1, z, 1) ||
           providesPowerTo(x, y, z - 1, 2) || providesPowerTo(x, y, z + 1, 3) ||
           providesPowerTo(x - 1, y, z, 4) || providesPowerTo(x + 1, y, z, 5);
}

bool TickWorld::isIndirectlyPowered(i32 x, int y, i32 z) const
{
    return indirectlyProvidesPowerTo(x, y - 1, z, 0) ||
           indirectlyProvidesPowerTo(x, y + 1, z, 1) ||
           indirectlyProvidesPowerTo(x, y, z - 1, 2) ||
           indirectlyProvidesPowerTo(x, y, z + 1, 3) ||
           indirectlyProvidesPowerTo(x - 1, y, z, 4) ||
           indirectlyProvidesPowerTo(x + 1, y, z, 5);
}

int TickWorld::torchToggleCount(i32 x, int y, i32 z) const
{
    int count = 0;
    for (usize i = 0; i < torchToggleCount_; ++i) {
        const TorchToggle& t = torchToggles_[i];
        if (t.x == x && t.z == z && t.y == i16(y)) ++count;
    }
    return count;
}

bool TickWorld::noteTorchToggle(i32 x, int y, i32 z)
{
    // The original prunes anything older than 100 ticks first, then records,
    // then counts. Same order here.
    usize kept = 0;
    for (usize i = 0; i < torchToggleCount_; ++i) {
        if (time_ - torchToggles_[i].time > 100) continue;
        torchToggles_[kept++] = torchToggles_[i];
    }
    torchToggleCount_ = kept;

    if (torchToggleCount_ == kTorchToggleCapacity) {
        // Drop the oldest. a1.1.2's list is unbounded; ours cannot be, and
        // losing the oldest entry only ever makes a torch less likely to be
        // called burnt out -- the safe direction, since the alternative is a
        // torch that goes out and stays out for no reason a player can see.
        for (usize i = 1; i < torchToggleCount_; ++i) torchToggles_[i - 1] = torchToggles_[i];
        --torchToggleCount_;
    }
    torchToggles_[torchToggleCount_++] = TorchToggle{x, z, i16(y), time_};

    return torchToggleCount(x, y, z) >= 8;
}

void TickWorld::tick(const Centre* centres, int centreCount, int radius)
{
    ++stats_.ticks;
    invalidateColumnCache();

    // `cn.g()`: the sky-light subtraction is recomputed first, because every
    // light query for the rest of the tick reads it, then the clock advances.
    skyDarken_ = world::skyLightSubtracted(time_);
    ++time_;

    runScheduled();
    drainDeferredNotifications();

    if (radius > kChunkTickRadius) radius = kChunkTickRadius;
    if (radius < 0) return;

    // The original builds a `Set<ChunkCoordIntPair>` so that two players
    // standing together do not tick the overlap twice. Ours walks the same
    // square per centre and relies on the caller passing one centre, which is
    // every case a1.1.2 singleplayer has; a second centre would double-tick
    // the overlap and that is a bug to fix when there is a second player.
    for (int c = 0; c < centreCount; ++c) {
        for (i32 dz = -radius; dz <= radius; ++dz) {
            for (i32 dx = -radius; dx <= radius; ++dx) {
                world::ChunkColumn* column =
                    columnFor(centres[c].chunkX + dx, centres[c].chunkZ + dz);
                if (column == nullptr) continue;
                ++stats_.chunksTicked;
                snowAndIce(*column);
                randomTickChunk(*column);
                // **Over the same square the random ticks walk**, which is a
                // narrowing: a1.1.2 updates every loaded tile entity from
                // `updateEntities`, and a furnace out past the tick radius in
                // the original keeps cooking where this one waits.
                tickTileEntities(*column);
            }
        }
    }

    // The backstop. notifyNeighbours drains as it unwinds, which covers every
    // ordinary path, but a deferral can also be created from inside
    // wirePropagate with no notifyNeighbours below it on the stack. Nothing
    // deferred is allowed to outlive the tick that deferred it.
    drainDeferredNotifications();
}

void TickWorld::tickTileEntities(world::ChunkColumn& column)
{
    if (column.tileEntities.empty()) {
        return;
    }
    // **Positions first, then the ticks.** A furnace that lights or goes out
    // rewrites its own block, which drops and re-adds its entry and moves the
    // list's order under any index held across the call. A column holds a
    // handful of tile entities; past the array the rest wait a tick.
    constexpr int kMaxFurnacesPerColumn = 32;
    struct Where {
        i32 x;
        i32 z;
        int y;
    };
    Where furnaces[kMaxFurnacesPerColumn];
    int count = 0;
    for (const world::TileEntity& tile : column.tileEntities) {
        if (tile.kind == world::TileEntityKind::Furnace && count < kMaxFurnacesPerColumn) {
            furnaces[count++] = Where{tile.x, tile.z, tile.y};
        }
    }
    for (int i = 0; i < count; ++i) {
        if (furnaceTickAt(*this, furnaces[i].x, furnaces[i].y, furnaces[i].z)) {
            markTileEntityChanged(furnaces[i].x, furnaces[i].z);
        }
    }
}

}  // namespace mc::tick
