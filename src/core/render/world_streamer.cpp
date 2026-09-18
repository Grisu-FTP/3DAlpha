#include "core/render/world_streamer.hpp"

#include "core/item/block_breaking.hpp"
#include "core/settings/world_settings.hpp"
#include "core/util/worker.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace mc::render {

namespace {

int floorMod(i32 value, int modulus)
{
    const int r = int(value % modulus);
    return r < 0 ? r + modulus : r;
}

}  // namespace

bool WorldStreamer::open(const char* worldDir, int meshDistance, i64 nowMillis)
{
    // Before anything can fail: a tick left over from a previous world holds
    // that world's clock and a dirty set pointing at columns this one does not
    // have, and an open that fails half way would otherwise leave it in place.
    tick_.reset();
    light_.reset();
    tickDirtyCells_ = 0;
    player_ = {};
    entityPools_ = {};
    entitiesBound_ = false;

    remote_ = false;
    remoteColumns_.clear();
    remoteOps_.clear();
    edits_.clear();

    cache_.configure(cacheConfig_);
    if (cache_.open(worldDir, nowMillis) != world::OpenResult::Ok) {
        return false;
    }

    path_ = worldDir;
    level_ = cache_.level();
    lastSaveMillis_ = nowMillis;
    open_ = true;

    meshDistance_ = meshDistance;
    loadRadius_ = meshDistance + 1;
    // The budget starts wide at every distance change; an over-budget pass is
    // what narrows it again, and a radius chosen against the old distance says
    // nothing about the new one.
    memoryRadius_ = loadRadius_;

    // **No centre yet, whatever the last world left here.** The console keeps
    // one streamer for the whole process and builds a fresh renderer per world,
    // and the first `update()` is the only thing that tells that renderer where
    // its field is -- but only when the centre *moved*. Reopening a world in
    // the chunk the previous session ended in left `centreSet_` true and the
    // centre equal, so the new renderer stayed centred on (0, 0), refused every
    // column as out of range, and nothing was drawn until the player crossed a
    // chunk boundary. See `a_world_reopened_in_the_chunk_it_was_left_in_draws`.
    centreSet_ = false;
    buildGrid();

    if (generateMissing_) {
        mcver::WorldGenOptions options;
        options.snowCovered = level_.snowCovered;

        // **The world's own Extra Settings, read here rather than handed in.**
        // These change what a chunk generates, so they belong to the world and
        // not to whoever opened it -- the host harness, the console's menu and
        // a test all have to agree, and the only thing all three share is the
        // world directory. Absent is the ordinary case and means off.
        settings::WorldSettings worldSettings;
        settings::loadWorldSettings(fs_, worldDir, &worldSettings);
        options.fixOreVeinBounds = worldSettings.fixOreGeneration;
        options.fixBedrockHole = worldSettings.fixBedrockHole;

        mcver::ChunkGenerator::Store store;
        store.context = this;
        store.load = &WorldStreamer::generatorLoad;
        store.deliver = &WorldStreamer::generatorDeliver;

        // Through a trampoline rather than straight through: `Store::context`
        // is this streamer, and the terrain source has a context of its own
        // that outlives the world -- whoever is hosting the session.
        store.supplyTerrain =
            terrainSource_.supply != nullptr ? &WorldStreamer::generatorSupplyTerrain : nullptr;

        // Sized to the load radius, because what the generator has to hold at
        // once is two rings of unfinished frontier and that band grows with the
        // radius being filled. Undersizing it is not a slow path but a wrong
        // world -- see ChunkGenerator::cacheColumnsFor.
        generator_ = std::make_unique<mcver::ChunkGenerator>(
            level_.randomSeed, options, store,
            mcver::ChunkGenerator::cacheColumnsFor(loadRadius_));
        generated_ = std::make_unique<world::ChunkColumn>();

        if (generationThreaded_ && !startWorker()) {
            // Not fatal. Generating on the main thread is what this class did
            // before the worker existed: it stutters badly, and it is still
            // better than a world that stops at the edge of what is on the
            // card. `Stats::workerRunning` is how a caller finds out.
        }
    }

    // The tick's own random sources are seeded from the world seed. a1.1.2
    // seeds them from the wall clock, so its random ticks are not reproducible
    // between two runs of the same world and nothing depends on them being;
    // seeding from the seed costs no fidelity and makes a test able to assert.
    tick::TickAccess tickAccess;
    tickAccess.ctx = this;
    tickAccess.column = &WorldStreamer::tickColumn;
    tickAccess.changed = &WorldStreamer::tickBlockChanged;
    tick_ = std::make_unique<tick::TickWorld>(tickAccess, level_.randomSeed);
    tick_->setTime(level_.time);
    tick_->setSnowCovered(level_.snowCovered);
    {
        // Read here for the reason the generator's two fixes are: the world
        // directory is the one thing every caller shares. The console's pause
        // menu applies a change to the running tick itself.
        settings::WorldSettings worldSettings;
        settings::loadWorldSettings(fs_, worldDir, &worldSettings);
        tick_->setImprovedFencePlacement(worldSettings.improvedFencePlacement);
    }
    tickDirtyCells_ = 0;
    served_.clear();
    servedAreaCount_ = 0;
    servedOwed_ = 0;
    servedOwedScan_ = 0;
    servedCursor_ = 0;
    servedGenerating_ = -1;
    servedGeneratorGrown_ = 0;
    for (int i = 0; i < kMaxServedAreas; ++i) {
        servedAreaPending_[i] = 0;
        servedAreaPendingScan_[i] = 0;
    }

    // The relighter reaches columns through the same hook the tick does -- both
    // want "the resident column or nothing", and neither may generate.
    world::LightAccess lightAccess;
    lightAccess.ctx = this;
    lightAccess.column = &WorldStreamer::tickColumn;
    lightAccess.sectionLit = &WorldStreamer::lightSectionLit;
    light_ = std::make_unique<world::LightUpdater>(lightAccess);

    builder_.reserveQuads(4096);
    return true;
}

world::ChunkColumn* WorldStreamer::tickColumn(void* ctx, i32 chunkX, i32 chunkZ)
{
    auto* self = static_cast<WorldStreamer*>(ctx);
    Cell* cell = self->find(chunkX, chunkZ);
    if (cell != nullptr && cell->state == CellState::Loaded) {
        return cell->column.get();
    }

    // **And then the ground somebody else is standing on.** This one line is
    // what makes a served area a real part of the world rather than a buffer
    // of bytes to post: the tick, the lighting, the fluids, a guest's pick and
    // a guest's placement all reach columns through here, so a column held for
    // a guest behaves exactly as a resident one does. See `setServedAreas`.
    //
    // The grid is asked first and wins: when the camera walks into ground that
    // was being served, the cell is authoritative and the served entry is
    // dropped on the next `pumpServed`.
    if (self->servedAreaCount_ > 0) {
        auto it = self->served_.find(remoteKey(chunkX, chunkZ));
        if (it != self->served_.end()) {
            return it->second.column.get();
        }
    }
    return nullptr;
}

// A section's stored light moved, so its mesh -- which bakes light into every
// vertex -- is wrong. Same path a block change takes, because the renderer does
// not care *why* a section changed.
void WorldStreamer::lightSectionLit(void* ctx, i32 chunkX, int sectionY, i32 chunkZ)
{
    auto* self = static_cast<WorldStreamer*>(ctx);
    if (self->tickRenderer_ != nullptr) {
        self->tickRenderer_->invalidateSection(chunkX, sectionY, chunkZ);
    }
    // The column's stored light changed, so what is on the card is out of date
    // just as surely as if a block had changed in it.
    //
    // **A cell that holds a column, not a cell that exists.** Cells reach three
    // rings further than columns do, and served ground now lives in that band --
    // asking `find` here marked an empty cell dirty and left the served column
    // clean, which is an edit that never reaches the card. See
    // `gridColumnRadius`.
    if (Cell* cell = self->loadedCell(chunkX, chunkZ)) {
        if (!cell->tickDirty) {
            cell->tickDirty = true;
            ++self->tickDirtyCells_;
        }
        return;
    }
    self->markServedModified(chunkX, chunkZ);
}

void WorldStreamer::markColumnModified(i32 x, i32 z)
{
    // `loadedCell` and not `find`, for the reason `lightSectionLit` gives.
    if (Cell* cell = loadedCell(x >> 4, z >> 4)) {
        if (!cell->tickDirty) {
            cell->tickDirty = true;
            ++tickDirtyCells_;
        }
        return;
    }
    markServedModified(x >> 4, z >> 4);
}

// The served half of the two `tickDirty` marks above. A column held for a guest
// is edited by the same tick and has to reach the card the same way; what it
// does *not* need is a mesh, a map serial or a renderer invalidation, because
// nothing in it is on this console's screen.
void WorldStreamer::markServedModified(i32 chunkX, i32 chunkZ)
{
    if (served_.empty()) {
        return;
    }
    auto it = served_.find(remoteKey(chunkX, chunkZ));
    if (it != served_.end()) {
        it->second.dirty = true;
    }
}

void WorldStreamer::tickBlockChanged(void* ctx, i32 x, int y, i32 z)
{
    auto* self = static_cast<WorldStreamer*>(ctx);
    const i32 cx = x >> 4;
    const i32 cz = z >> 4;

    // **Light first, because it needs the height map the tick has just moved
    // and the block it has just written.** This only enqueues; the propagation
    // is budgeted and happens at the end of stepTicks.
    if (self->light_ != nullptr) {
        self->light_->blockChanged(x, y, z);
    }

    if (self->tickRenderer_ != nullptr) {
        const int sy = y / world::Section::kSize;
        self->tickRenderer_->invalidateSection(cx, sy, cz);

        // A face on the boundary of a section is culled against its
        // neighbour, so a block on the edge changes two meshes and a block in
        // a corner changes four. Invalidating unconditionally would quadruple
        // the mesh work for the common case, so it is asked per axis.
        const int lx = int(x & 15);
        const int lz = int(z & 15);
        const int ly = y % world::Section::kSize;
        if (lx == 0) self->tickRenderer_->invalidateSection(cx - 1, sy, cz);
        if (lx == 15) self->tickRenderer_->invalidateSection(cx + 1, sy, cz);
        if (lz == 0) self->tickRenderer_->invalidateSection(cx, sy, cz - 1);
        if (lz == 15) self->tickRenderer_->invalidateSection(cx, sy, cz + 1);
        // **And the fourth corner, which face culling never needed and the
        // chest does.** `BlockChest.getBlockTexture` asks about the two cells
        // beside its partner, so a block dropped diagonally across a column
        // corner decides which way a double chest faces. Only a block in a
        // corner cell pays for it, and it is one section rather than four.
        if ((lx == 0 || lx == 15) && (lz == 0 || lz == 15)) {
            self->tickRenderer_->invalidateSection(lx == 0 ? cx - 1 : cx + 1, sy,
                                                  lz == 0 ? cz - 1 : cz + 1);
        }
        if (ly == 0 && sy > 0) self->tickRenderer_->invalidateSection(cx, sy - 1, cz);
        if (ly == world::Section::kSize - 1 &&
            sy + 1 < world::ChunkColumn::kSectionCount) {
            self->tickRenderer_->invalidateSection(cx, sy + 1, cz);
        }
    }

    // **Ground held for somebody else, which the grid does not have.** One
    // lookup, and only when a served area exists at all -- see
    // `WorldStreamer::setServedAreas`.
    // `loadedCell` and not `find`: a classified cell with nothing in it is not
    // the grid answering for this ground. See `gridColumnRadius`.
    if (self->servedAreaCount_ > 0 && self->loadedCell(cx, cz) == nullptr) {
        self->markServedModified(cx, cz);
    }

    // O(1), on the cell itself. See Cell::tickDirty for what this replaced and
    // why the old linear scan got slower the longer a session ran.
    if (Cell* cell = self->find(cx, cz)) {
        if (!cell->tickDirty) {
            cell->tickDirty = true;
            ++self->tickDirtyCells_;
        }
        // **And the map, which is the other thing a block change invalidates.**
        // Unconditional, where the section invalidation above is bracketed by
        // `tickRenderer_`: a fluid spreading or a leaf decaying changes the
        // picture whether or not anyone is holding a renderer, and the map
        // reads this whenever it next looks rather than at the moment of the
        // write. See `blockChangeSerial`.
        cell->mapSerial = ++self->blockSerial_;
        // **...and the coordinate goes on the list, once.** A fluid rewrites
        // hundreds of blocks in this chunk in one tick and every one of them
        // arrives here; `ChunkQueue::push` answers the second and the
        // three-hundredth of them in a probe and a compare. See
        // `takeChangedColumn`.
        self->mapDirty_.push(cx, cz);
    }

    // **And whoever is serving this world to another console**, which needs
    // the block rather than the column. Last, because it is the only one of
    // these that can be absent. See setBlockWatcher.
    if (self->blockWatcher_ != nullptr) {
        self->blockWatcher_(self->blockWatcherCtx_, x, y, z);
    }
}

void WorldStreamer::stepTicks(ChunkRenderer& renderer, int ticks)
{
    if (!open_ || tick_ == nullptr || ticks <= 0 || !centreSet_) {
        return;
    }

    if (remote_) {
        // `gs.g()`: the clock and the revert list, and nothing else ticks.
        RenderBracket draws(*this, renderer);
        for (int i = 0; i < ticks; ++i) {
            tick_->setTime(tick_->time() + 1);
            edits_.tick(&WorldStreamer::revertEdit, this);
        }
        level_.time = tick_->time();
        return;
    }

    // a1.1.2 ticks a 19x19 square of chunks around the player regardless of
    // render distance. We cannot tick a column we do not hold, so the radius
    // is the smaller of the two -- and at the render distances this console
    // runs, ours is the smaller one. The consequence is stated rather than
    // hidden: at render distance 6 a world simulates 13x13 chunks where the
    // original simulates 19x19, so crops at the edge of view grow while the
    // original's grow a little further out still.
    int radius = loadRadius_;
    if (radius > tick::TickWorld::kChunkTickRadius) {
        radius = tick::TickWorld::kChunkTickRadius;
    }

    const tick::TickWorld::Centre centre{centreX_, centreZ_};

    {
        // **Settles what the ticks disturbed on the way out, with the renderer
        // still in hand.** The relighter reports the sections whose stored
        // light moved, and those need remeshing exactly as a block change does
        // -- light is baked into vertices. Budgeted, so a roof coming off costs
        // latency rather than a frame; what is left stays queued for the next.
        RenderBracket draws(*this, renderer);
        for (int i = 0; i < ticks; ++i) {
            tick_->tick(&centre, 1, radius);
        }
    }

    level_.time = tick_->time();
}

bool WorldStreamer::setBlock(ChunkRenderer& renderer, i32 x, int y, i32 z,
                             block::BlockId id, u8 metadata)
{
    if (tick_ == nullptr) {
        return false;
    }
    if (!tick_->chunkResident(x >> 4, z >> 4)) {
        return false;
    }

    // The same bracket stepTicks uses, and for the same reason: without it the
    // change callback below has no renderer to invalidate sections through.
    RenderBracket draws(*this, renderer);
    return tick_->setBlockAndDataWithNotify(x, y, z, id, metadata);
}

bool WorldStreamer::rightClick(ChunkRenderer& renderer, item::ItemId held,
                              const entity::RayHit& hit, const AABB& playerBox,
                              float yawDegrees, const item::Effects& effects, bool* itemTook)
{
    if (itemTook != nullptr) {
        *itemTook = false;
    }
    if (tick_ == nullptr) {
        return false;
    }
    if (!tick_->chunkResident(hit.x >> 4, hit.z >> 4)) {
        return false;
    }

    RenderBracket draws(*this, renderer);
    return item::rightClick(*tick_, held, hit, playerBox, yawDegrees, effects, itemTook);
}

item::ItemUse WorldStreamer::useItem(ChunkRenderer& renderer, item::ItemId held, double eyeX,
                                    double eyeY, double eyeZ, double dirX, double dirY,
                                    double dirZ, const item::Effects& effects)
{
    if (tick_ == nullptr) {
        return item::ItemUse{false, held};
    }

    RenderBracket draws(*this, renderer);
    return item::useItem(*tick_, held, eyeX, eyeY, eyeZ, dirX, dirY, dirZ, effects);
}

bool WorldStreamer::breakBlock(ChunkRenderer& renderer, i32 x, int y, i32 z,
                              const item::Effects& effects)
{
    if (tick_ == nullptr) {
        return false;
    }
    if (!tick_->chunkResident(x >> 4, z >> 4)) {
        return false;
    }

    RenderBracket draws(*this, renderer);
    return item::destroyBlock(*tick_, x, y, z, effects);
}

bool WorldStreamer::harvestBlock(ChunkRenderer& renderer, i32 x, int y, i32 z,
                                item::ItemId held, const item::Effects& effects)
{
    if (tick_ == nullptr) {
        return false;
    }
    if (!tick_->chunkResident(x >> 4, z >> 4)) {
        return false;
    }

    RenderBracket draws(*this, renderer);
    return item::harvestBlockFor(*tick_, x, y, z, held, effects);
}

WorldStreamer::RenderBracket::RenderBracket(WorldStreamer& streamer, ChunkRenderer& renderer)
    : streamer_(streamer), previous_(streamer.tickRenderer_)
{
    streamer_.tickRenderer_ = &renderer;
}

WorldStreamer::RenderBracket::~RenderBracket()
{
    // The same drain `stepTicks` and `setBlock` do, and for the same reason:
    // what the writes inside queued has to reach the card while there is still
    // a renderer to invalidate through. Skipped when this is a nested bracket,
    // whose outer one will drain once on its own way out.
    if (previous_ == nullptr && streamer_.light_ != nullptr) {
        streamer_.light_->drain(kLightBudgetPerFrame);
    }
    streamer_.tickRenderer_ = previous_;
}

void WorldStreamer::flushTickDirty()
{
    if (remote_) {
        for (Cell& cell : cells_) {
            cell.tickDirty = false;
        }
        tickDirtyCells_ = 0;
        return;
    }
    // One clone and one queued write per column that changed, however many
    // blocks in it changed -- which is the whole reason this is deferred to
    // the save boundary instead of being done in the callback.
    //
    // A pass over the grid rather than over a list of coordinates: the grid is
    // at most 729 cells, this runs on the autosave timer rather than per frame,
    // and it cannot miss a column the way a coordinate list could once the
    // centre had moved past it.
    for (Cell& cell : cells_) {
        if (!cell.tickDirty) continue;
        if (cell.state == CellState::Loaded && cell.column != nullptr) {
            // Whatever the session holds outside the column -- sign text,
            // spawner state -- goes back into it first. See setColumnSinks.
            if (columnSaving_ != nullptr) {
                columnSaving_(columnSinkCtx_, *cell.column);
            }
            // Main thread: queue the write, never perform it here.
            cache_.save(*cell.column, world::ChunkCache::SavePressure::Defer);
        }
        cell.tickDirty = false;
    }
    tickDirtyCells_ = 0;
    flushServedDirty();
}

// ---- served areas ----------------------------------------------------------
//
// See `WorldStreamer::setServedAreas` for what these are for. The shape of the
// work is deliberately the grid's, one layer simpler: a column is read through
// the same cache, generated by the same generator on the same slate, and saved
// by the same autosave -- what it skips is everything to do with drawing,
// because nothing here is on this console's screen.

void WorldStreamer::setServedAreas(const ServedArea* areas, int count)
{
    if (areas == nullptr) {
        count = 0;
    }
    if (count < 0) {
        count = 0;
    }
    if (count > kMaxServedAreas) {
        count = kMaxServedAreas;
    }
    servedAreaCount_ = count;
    int widest = 0;
    for (int i = 0; i < count; ++i) {
        servedAreas_[i] = areas[i];
        if (servedAreas_[i].radius < 0) {
            servedAreas_[i].radius = 0;
        }
        widest = servedAreas_[i].radius > widest ? servedAreas_[i].radius : widest;
    }
    if (servedGenerating_ >= count) {
        servedGenerating_ = count > 0 ? 0 : -1;
    } else if (servedGenerating_ < 0 && count > 0) {
        servedGenerating_ = 0;
    }

    // **How large the generator's cache has to be**, which a second centre
    // changes; see `servedGeneratorSlack`.
    //
    // **Asked for, not done here.** `setMeshDistance` grows the cache on this
    // thread and pays `waitForWorkerIdle()` for the privilege, which is fair
    // for a settings change a player made. This runs *every frame* of a hosted
    // session, and `growCacheTo` resizes the very vectors the worker may be
    // inside `provide()` reading -- so the number is published under the queue
    // lock and the worker grows its own cache before its next sweep, where the
    // generator belongs. See `generateColumn`.
    servedGeneratorGrown_ = widest > 0 ? servedGeneratorSlack(widest) : 0;

    // **The generator is told where to keep at the same moment**, because a
    // centre it does not know about is a region it retires and re-derives on
    // the next sweep that reaches it. Published under the queue lock with the
    // camera's, which the worker reads together.
    publishRetireCentre();
}

int WorldStreamer::servedDistance(i32 chunkX, i32 chunkZ) const
{
    int best = -1;
    for (int i = 0; i < servedAreaCount_; ++i) {
        const ServedArea& area = servedAreas_[i];
        const int d = std::max(std::abs(chunkX - area.chunkX), std::abs(chunkZ - area.chunkZ));
        if (d > area.radius) {
            continue;
        }
        if (best < 0 || d < best) {
            best = d;
        }
    }
    return best;
}

bool WorldStreamer::inServedArea(i32 chunkX, i32 chunkZ) const
{
    return servedDistance(chunkX, chunkZ) >= 0;
}

void WorldStreamer::releaseServed(ServedColumn& entry, bool save)
{
    if (entry.column == nullptr) {
        return;
    }
    const i32 cx = entry.column->x;
    const i32 cz = entry.column->z;
    if (save && entry.dirty) {
        // Whatever the session holds outside the column goes back into it
        // first, exactly as `flushTickDirty` does for a cell. See
        // `setColumnSinks`.
        if (columnSaving_ != nullptr) {
            columnSaving_(columnSinkCtx_, *entry.column);
        }
        cache_.save(*entry.column, world::ChunkCache::SavePressure::Defer);
    }
    // `saving` and then `dropped`, in that order and for the reason `dropCell`
    // gives: a sink that erases its entries for a column must not erase them
    // out from under the one that writes them.
    if (columnDropped_ != nullptr) {
        columnDropped_(columnSinkCtx_, cx, cz);
    }
    // Given back rather than freed, for the reason `dropCell` gives: a guest
    // pacing over a chunk border would otherwise cost a read every step.
    cache_.give(std::move(entry.column));
    entry.column.reset();
    entry.dirty = false;
}

void WorldStreamer::flushServedDirty()
{
    for (auto& pair : served_) {
        ServedColumn& entry = pair.second;
        if (!entry.dirty || entry.column == nullptr) {
            continue;
        }
        if (columnSaving_ != nullptr) {
            columnSaving_(columnSinkCtx_, *entry.column);
        }
        cache_.save(*entry.column, world::ChunkCache::SavePressure::Defer);
        entry.dirty = false;
    }
}

// **How many of the wanted chunks are looked at in one frame.**
//
// Three guests at the host's view distance is 675 cells and every one of them
// is a hash lookup, which is a different thing from the grid's own unbudgeted
// classification pass -- that is an array index into a wrapping grid. Walking
// a slice and resuming next frame keeps the cost flat in the number of guests,
// and nothing is lost by it: a chunk missed this frame is asked about on the
// next, and the whole square is covered in at most two frames at the cap below.
constexpr int kServedScanPerFrame = 384;

void WorldStreamer::pumpServed(int budget)
{
    // **Nothing to serve: let the whole set go, once.** Single player and every
    // frame of a session with no guests takes this branch and one branch only.
    if (servedAreaCount_ == 0) {
        if (!served_.empty()) {
            for (auto& pair : served_) {
                releaseServed(pair.second, true);
            }
            served_.clear();
        }
        servedCursor_ = 0;
        servedOwed_ = 0;
        servedOwedScan_ = 0;
        servedGenerating_ = -1;
        for (int i = 0; i < kMaxServedAreas; ++i) {
            servedAreaPending_[i] = 0;
            servedAreaPendingScan_[i] = 0;
        }
        return;
    }

    // ---- what is no longer wanted -------------------------------------
    //
    // Two reasons to let a column go: nobody is near it any more, and **the
    // camera has come within reach of it**. The second is the one that has to
    // be exact. The grid is authoritative for every chunk it can reach, and
    // two copies of one column being edited independently is a world that
    // disagrees with itself -- so the test is the grid's *radius* and not
    // whether the grid has actually got the column yet. Anything else leaves a
    // window where a cell is still reading while the served copy is being
    // written into, and the read lands on top of the writes.
    //
    // This runs before `update()` classifies a single cell, which is what
    // makes "before" mean before: a chunk that came into reach this frame is
    // saved here and read back by the grid from the cache, with the edits in
    // it. See `ChunkCache::runJob`, whose completing read declines to replace
    // an entry that already exists.
    for (auto it = served_.begin(); it != served_.end();) {
        const i32 cx = it->second.column != nullptr ? it->second.column->x : 0;
        const i32 cz = it->second.column != nullptr ? it->second.column->z : 0;
        const bool inGrid = centreSet_ && std::abs(cx - centreX_) <= gridColumnRadius()
                            && std::abs(cz - centreZ_) <= gridColumnRadius();
        const bool gone =
            it->second.column == nullptr || !inServedArea(cx, cz) || inGrid;
        if (!gone) {
            ++it;
            continue;
        }
        releaseServed(it->second, true);
        it = served_.erase(it);
    }

    // ---- what is still owed --------------------------------------------
    //
    // A plain walk of every area's square, laid end to end and resumed where
    // the last frame stopped. Flat rather than per-area so one cap covers the
    // whole session however many guests there are and whatever radius each of
    // them wants, and so "a pass" means all of it and not one guest's share.
    //
    // **No spiral, and that is deliberate.** The grid walks one because the
    // *order* of generation is the world and nearest-first has to mean
    // nearest-first; here the order that matters is still the grid's, because
    // `refreshSlate` takes served columns only after the camera's own spiral is
    // clear. What is left for this walk to decide is which of a guest's own
    // columns is asked about first, and a1.1.2 has no opinion about that.
    int total = 0;
    for (int i = 0; i < servedAreaCount_; ++i) {
        const int side = servedAreas_[i].radius * 2 + 1;
        total += side * side;
    }
    if (servedCursor_ >= total) {
        servedCursor_ = 0;
        servedOwedScan_ = 0;
    }

    int served = int(served_.size());
    int scanned = 0;
    int base = 0;
    for (int i = 0; i < servedAreaCount_ && scanned < kServedScanPerFrame; ++i) {
        const ServedArea& area = servedAreas_[i];
        const int side = area.radius * 2 + 1;
        const int cells = side * side;
        const int first = servedCursor_ + scanned - base;
        base += cells;
        for (int n = first < 0 ? 0 : first; n < cells && scanned < kServedScanPerFrame;
             ++n, ++scanned) {
            const i32 cx = area.chunkX - area.radius + i32(n % side);
            const i32 cz = area.chunkZ - area.radius + i32(n / side);

            // **The grid's reach, not the grid's contents.** A chunk the
            // camera can reach belongs to the grid whether or not the grid has
            // got round to reading it; see the eviction above for why the
            // distinction matters.
            //
            // **Its reach and not its size** -- `gridColumnRadius`, which is
            // `loadRadius_`. Skipping everything inside `gridRadius_` skipped
            // three rings the grid never fills, and a guest standing in them
            // starved. See `gridColumnRadius`.
            if (centreSet_ && std::abs(cx - centreX_) <= gridColumnRadius()
                && std::abs(cz - centreZ_) <= gridColumnRadius()) {
                continue;
            }
            const i64 key = remoteKey(cx, cz);
            auto existing = served_.find(key);
            if (existing != served_.end()) {
                if (existing->second.owed) {
                    ++servedOwedScan_;
                    ++servedAreaPendingScan_[i];
                }
                continue;
            }
            // Wanted and not held: either still to be asked about, or asked
            // about and owed to the generator. Both count against this area's
            // turn -- see `servedAreaPending_`.
            ++servedOwedScan_;
            ++servedAreaPendingScan_[i];
            if (budget <= 0 || served >= kMaxServedColumns) {
                continue;
            }

            std::unique_ptr<world::ChunkColumn> column;
            switch (cache_.tryTake(cx, cz, &column)) {
            case world::ChunkCache::Take::Took: {
                ServedColumn& entry = served_[key];
                entry.column = std::move(column);
                entry.dirty = false;
                entry.owed = false;
                if (columnAdopted_ != nullptr && entry.column != nullptr) {
                    columnAdopted_(columnSinkCtx_, *entry.column);
                }
                --servedOwedScan_;
                --servedAreaPendingScan_[i];
                ++served;
                --budget;
                break;
            }
            case world::ChunkCache::Take::Pending:
                // Being read; ask again next frame, exactly as a cell does.
                --budget;
                break;
            case world::ChunkCache::Take::Missing:
                // **The world has never had this chunk.** It goes on the slate
                // as owed, which is the same road the camera's own missing
                // ground takes -- and it is taken only after the camera's, so
                // a guest exploring can never stall the ground under the
                // player holding the console. See `refreshSlate`.
                //
                // **Marked owed whichever area wants it**, and put on the
                // slate only when it is that area's turn -- the two are
                // different questions and answering them with one flag
                // deadlocked the rotation: an area that was never allowed to
                // record what it wanted could never be seen to want anything,
                // so its turn never came. `refreshSlate` is where the turn is
                // applied; see `servedGeneratorSlack` for why there is one.
                if (generateMissing_ && generator_ != nullptr) {
                    ServedColumn& entry = served_[key];
                    entry.owed = true;
                }
                --budget;
                break;
            }
        }
    }

    // **Published on a whole pass, not on a slice.** A counter that reported
    // what one frame's slice happened to see would read near zero on a session
    // owed hundreds of columns, which is the opposite of what it is for.
    servedCursor_ += scanned;
    if (servedCursor_ >= total) {
        servedOwed_ = servedOwedScan_;
        servedOwedScan_ = 0;
        servedCursor_ = 0;
        for (int i = 0; i < kMaxServedAreas; ++i) {
            servedAreaPending_[i] = servedAreaPendingScan_[i];
            servedAreaPendingScan_[i] = 0;
        }

        // **And the turn passes, at the end of a whole pass and not before.**
        //
        // The area on the slate keeps it until it wants **nothing** -- not
        // merely nothing the generator owes, but nothing this console has still
        // to ask storage about, which is what `servedAreaPending_` counts.
        // Passing on the narrower test made two guests swap every few frames
        // while both squares were still filling, and every swap moves the
        // generator's second retire centre, which retires and re-derives a
        // whole frontier.
        if (servedAreaCount_ > 0 && !servedAreaWants(servedGenerating_)) {
            const int was = servedGenerating_;
            for (int step = 1; step <= servedAreaCount_; ++step) {
                const int next = (was < 0 ? 0 : was + step) % servedAreaCount_;
                if (servedAreaWants(next)) {
                    servedGenerating_ = next;
                    break;
                }
            }
            if (servedGenerating_ != was) {
                publishRetireCentre();
            }
        }
    }
}

bool WorldStreamer::servedAreaWants(int index) const
{
    return index >= 0 && index < servedAreaCount_ && servedAreaPending_[index] > 0;
}

bool WorldStreamer::generatorSupplyTerrain(void* context, i32 chunkX, i32 chunkZ, u8* blocks)
{
    WorldStreamer* self = static_cast<WorldStreamer*>(context);
    return self->terrainSource_.supply != nullptr
           && self->terrainSource_.supply(self->terrainSource_.context, chunkX, chunkZ, blocks);
}

// ChunkGenerator::Store. The generator asks the world what is already there and
// hands back what it makes; both go through the same storage slot the streamer
// reads from, so a generated chunk is on the SD card before anything else can
// ask for it.
const world::ChunkColumn* WorldStreamer::generatorLoad(void* context, i32 chunkX, i32 chunkZ,
                                                       world::ChunkColumn* scratch)
{
    WorldStreamer& self = *static_cast<WorldStreamer*>(context);

    // **The cache, and only the cache.** An earlier version answered out of the
    // resident grid, which is faster and became wrong the moment this started
    // running on another thread: the grid is the main thread's, and it changes
    // under the player's feet.
    //
    // ChunkCache is authoritative in the way storage was -- every column the
    // generator finishes is stored here before it is handed over, and a read
    // returns byte-identical content whether it comes from the table or the
    // card -- so nothing about what the generator sees has changed. What has
    // changed is the cost: a column the sweep wrote a moment ago used to be a
    // deflate, a file write and a read back, and is now a clone.
    return self.cache_.load(chunkX, chunkZ, scratch);
}

void WorldStreamer::generatorDeliver(void* context, world::ChunkColumn& column)
{
    WorldStreamer& self = *static_cast<WorldStreamer*>(context);

    // **Saving is not optional.** The generator evicts what it has handed over
    // and reaches it again through load(); a column that was never stored would
    // come back as bare terrain with every neighbour's population missing, and
    // the world would be quietly wrong rather than obviously so.
    //
    // "Stored" now means the cache rather than the card -- the write follows on
    // the I/O thread -- and that is the same guarantee, because the cache is
    // what load() reads and it answers with this column from here on.
    self.cache_.save(column);

    // Then the grid gets it, and **not from here**: this runs on the worker,
    // and the grid, the renderer and the visibility masks are the main
    // thread's. The column is queued and adopted in drainGenerated().
    auto owned = std::make_unique<world::ChunkColumn>(std::move(column));
    {
        std::lock_guard<std::mutex> guard(self.queueLock_);
        self.finished_.push_back(std::move(owned));
    }
}

void WorldStreamer::workerEntry(void* self)
{
    static_cast<WorldStreamer*>(self)->workerMain();
}

bool WorldStreamer::startWorker()
{
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        workerStop_ = false;
        workerRunning_ = true;
    }

    // The platform gets first refusal, because it is the only one that can say
    // "core 2". See core/util/worker.hpp.
    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformWorker_ =
            workerSpawn()(&WorldStreamer::workerEntry, this, WorkerRole::Generation);
        if (platformWorker_ == nullptr) {
            std::lock_guard<std::mutex> guard(queueLock_);
            workerRunning_ = false;
            return false;
        }
        return true;
    }

    worker_ = std::thread([this] { workerMain(); });
    return true;
}

// Blocks until the worker has nothing in hand. Only for the few main-thread
// operations that reach into the generator itself -- growing its cache while it
// is walking its own entry table would move the table under it.
void WorldStreamer::waitForWorkerIdle()
{
    if (!workerRunning_) {
        return;
    }
    // Whoever needs the worker idle needs it out of the columns too, and this
    // takes queueLock_ of its own so it goes before the lock below.
    reclaimColumnWork();
    std::unique_lock<std::mutex> guard(queueLock_);
    // **Pause first, then wait.** The worker takes its own next job the moment
    // it finishes one, so waiting for "not busy" without stopping it handing
    // itself more work is a wait that can never end on a full queue.
    queuePaused_ = true;
    idle_.wait(guard, [this] { return !jobActive_; });
}

void WorldStreamer::resumeGeneration()
{
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        queuePaused_ = false;
    }
    wake_.notify_one();
}

void WorldStreamer::stopWorker()
{
    if (platformWorker_ == nullptr && !worker_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        workerStop_ = true;
    }
    wake_.notify_all();
    if (platformWorker_ != nullptr) {
        workerJoin()(platformWorker_);
        platformWorker_ = nullptr;
    } else {
        worker_.join();
    }
    std::lock_guard<std::mutex> guard(queueLock_);
    workerRunning_ = false;
}

// The worker's whole life: take the nearest column that is owed, make it,
// repeat.
//
// **It takes its own next job**, and that is the whole reason this loop looks
// the way it does. The main thread used to hand one out per `update()`, which
// capped generation at one column per rendered frame -- thirty a second at
// thirty frames a second -- however fast the worker actually was. On a New 3DS
// the worker is on core 2 with nothing else on it and can beat that, and every
// column it could have made and did not is a frame the frontier stays empty.
//
// What does *not* change is the rule: one column at a time, nearest to the
// camera. The realised sequence does change, in the sense that finishing three
// columns between two frames means the camera moved less between the picks --
// which is the same speed-dependence a faster console already has and which
// nearest-first accepted deliberately. See the note on slate_.
//
// It holds no lock while generating, which is the point -- the main thread must
// never wait on anything longer than a pointer swap. What it does share is the
// storage slot, and that is taken and released once per chunk file inside
// generatorLoad and generatorDeliver.
void WorldStreamer::workerMain()
{
    for (;;) {
        std::pair<i32, i32> at{0, 0};
        {
            std::unique_lock<std::mutex> guard(queueLock_);
            wake_.wait(guard, [this] {
                return workerStop_ || (!queuePaused_ && !slate_.empty()) || columnWorkPosted_;
            });
            if (workerStop_) {
                jobActive_ = false;
                columnWorkBusy_ = false;
                idle_.notify_all();
                return;
            }

            // **Generation first, every time round.** The slate is looked at
            // before the offered work is, so a column that is owed is never
            // behind a map sample -- which is the whole of what "generation has
            // priority" means here, said in one `if` rather than in a policy the
            // caller has to honour.
            if (queuePaused_ || slate_.empty()) {
                if (!columnWorkPosted_) {
                    continue;
                }
                runColumnWorkLocked(guard);
                continue;
            }

            takeSlateLocked(&at);
            inFlight_ = at;
            jobActive_ = true;
        }

        generateColumn(at.first, at.second);

        // The generator's own counters are published here rather than read
        // from the main thread. They belong to the worker, which is the only
        // thread allowed inside the generator at all, and "it is only a
        // counter" is not a reason to leave a read unsynchronised -- the
        // thread sanitizer named this one before it could become a habit.
        std::lock_guard<std::mutex> guard(queueLock_);
        workerPeakLive_ = generator_->stats().peakLive;
        workerEvictedLive_ = generator_->stats().evictedLive;
        workerUnlightable_ = generator_->stats().sweepUnlightable;
        workerIncomplete_ = generator_->stats().sweepIncomplete;
        workerRetiredLive_ = generator_->stats().retiredLive;
        // The coordinate goes back to the main thread, which keeps it off the
        // slate until drainGenerated has put the column in the grid. A list
        // rather than a single slot, because more than one column can finish
        // between two frames -- which is the point of the worker taking its own
        // jobs.
        completed_.push_back(at);
        jobActive_ = false;
        idle_.notify_all();
    }
}

// One offered batch, with `guard` holding queueLock_ on the way in and on the
// way out. The lock is dropped for each column and taken again between them,
// which is what lets the two things that can interrupt a batch actually do so:
// a column coming onto the slate, and `reclaimColumnWork` withdrawing the
// offer. Neither can wait for a whole batch -- a batch is eight chunks of
// somebody else's work.
void WorldStreamer::runColumnWorkLocked(std::unique_lock<std::mutex>& guard)
{
    columnWorkBusy_ = true;
    const ColumnWork work = columnWork_;
    void* const ctx = columnWorkCtx_;
    const int count = columnWorkCount_;

    int done = 0;
    for (; done < count; ++done) {
        const world::ChunkColumn* column = columnWorkColumns_[done];
        if (column == nullptr) {
            break;
        }
        guard.unlock();
        work(ctx, done, *column);
        guard.lock();
        // **Both of the ways out, checked in the order they matter.** A
        // withdrawal means the main thread is waiting on this loop and the
        // column pointers are about to stop being anybody's; a column on the
        // slate means the world is owed something the player can walk into.
        if (!columnWorkPosted_ || workerStop_) {
            ++done;
            break;
        }
        if (!queuePaused_ && !slate_.empty()) {
            ++done;
            break;
        }
    }

    columnWorkDone_ = done;
    columnWorkPosted_ = false;
    columnWorkBusy_ = false;
    // **Set here and not only in `reclaimColumnWork`**, because a batch that
    // finished before the next frame asked for it back leaves nothing for that
    // call to wait on -- it returns early, and a result that announced itself
    // only there would never be collected at all.
    columnWorkReady_ = true;
    idle_.notify_all();
}

bool WorldStreamer::generationIdle() const
{
    // **A cell nobody has been able to ask about yet counts as outstanding**,
    // because it may be about to become a column that is owed. Without this a
    // caller can see an idle generator on the first frame of a world -- nothing
    // classified, so nothing owed, so nothing to do -- and conclude the world
    // has settled before a single question has been answered.
    //
    // That is not hypothetical. It is what made the three-arm world test
    // flaky the moment classification could be deferred: the harness settles at
    // each waypoint, and at the first one it moved on having generated nothing
    // at all. Every caller of this asks the same question -- the tests, the
    // `--fly` settle point, the console's loading screen -- so it belongs here
    // rather than in each of them.
    if (stats_.unclassified != 0) {
        return false;
    }
    std::lock_guard<std::mutex> guard(queueLock_);
    return slate_.empty() && !jobActive_ && finished_.empty() && completed_.empty();
}

// Takes what the worker finished into the grid. Main thread, once a frame, and
// **before the scan that rewrites the slate** -- which is what makes it safe to
// clear `completed_` here: a coordinate leaves that list and the column lands in
// the grid in the same call, so there is no moment where a finished column looks
// like one that is still owed.
//
// A result may be for a column that has since gone out of range -- the player
// kept walking while it was being made -- and dropping it costs nothing,
// because it was written to the card before it was handed back.
void WorldStreamer::drainGenerated(ChunkRenderer& renderer)
{
    std::vector<std::unique_ptr<world::ChunkColumn>> batch;
    std::vector<std::pair<i32, i32>> done;
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        batch.swap(finished_);
        done.swap(completed_);
        stats_.generatorPeakLive = workerPeakLive_;
        stats_.generatorEvictedLive = workerEvictedLive_;
        stats_.generationFailures = generationFailures_;
        stats_.generationUnlightable = workerUnlightable_;
        stats_.generationIncomplete = workerIncomplete_;
        stats_.generatorRetiredLive = workerRetiredLive_;
    }
    if (batch.empty()) {
        return;
    }
    stats_.generatedThisFrame = int(batch.size());

    for (auto& column : batch) {
        if (column == nullptr) {
            continue;
        }
        const i32 cx = column->x;
        const i32 cz = column->z;
        // **admitRadius(), not loadRadius_**: a column the budget has just
        // pushed out of range must not be adopted straight back in, or the
        // generator and the eviction chase each other for ever.
        if (!centreSet_ || std::abs(cx - centreX_) > admitRadius()
            || std::abs(cz - centreZ_) > admitRadius()) {
            // Out of the grid's reach, but it may be ground that was generated
            // *for* a guest -- in which case it goes where that guest's ground
            // goes rather than being dropped. It is already in the cache; this
            // holds it so the server can post it without a read.
            const bool inGrid = std::abs(cx - centreX_) <= gridColumnRadius()
                                && std::abs(cz - centreZ_) <= gridColumnRadius();
            if (!inGrid && inServedArea(cx, cz)) {
                ServedColumn& entry = served_[remoteKey(cx, cz)];
                entry.column = std::move(column);
                entry.owed = false;
                entry.dirty = false;
                ++stats_.adoptedThisFrame;
            }
            continue;
        }
        Cell& cell = cells_[cellIndex(cx, cz)];
        if (cell.state == CellState::Loaded && cell.chunkX == cx && cell.chunkZ == cz) {
            continue;  // already here; the card answered first
        }
        if (cell.state != CellState::Empty && (cell.chunkX != cx || cell.chunkZ != cz)) {
            dropCell(cell, renderer);
        }
        adoptColumn(cell, std::move(column));
        ++stats_.adoptedThisFrame;
    }

    // A generated column arrives with its neighbours, so the nine-cell
    // republish the load path does is not enough. Once over the grid is cheap
    // next to what was just generated, and only on a frame that adopted.
    if (stats_.adoptedThisFrame > 0) {
        for (const Offset& offset : spiral_) {
            publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
        }
    }
}

void WorldStreamer::buildGrid()
{
    // Every cell is about to be rebuilt, so anything the relighter still owes
    // names columns that are about to move. Dropping it costs a little stale
    // light where the queue was, which the next block change in that column
    // repairs; keeping it would be work against a grid that no longer exists.
    if (light_ != nullptr) {
        light_->reset();
    }

    // Three rings wider than the load radius, which is a sweep's reach -- but
    // only when there is something that sweeps. With generation off the extra
    // ring would be cells that are classified and never used, and the harnesses
    // that measure a fixed world would be paying for a question they never ask.
    gridRadius_ = loadRadius_ + (generateMissing_ && !remote_ ? 3 : 0);
    edge_ = gridRadius_ * 2 + 1;
    cells_.clear();
    cells_.resize(usize(edge_) * edge_);

    // **As long as the grid has cells**, which is the most distinct coordinates
    // that can be waiting on it at once: a coordinate only goes on when a cell
    // answers to it. Rebuilt with the grid rather than kept, because a change
    // list written against a different radius names ground this grid may not
    // have -- and the reader falls back to looking at everything when the list
    // says it lost some, which a grid rebuild has just made true anyway.
    mapDirty_.setCapacity(int(cells_.size()));

    // Nearest first, so the columns under the player's feet arrive before the
    // ones at the horizon. Ordering it once here is what keeps the per-frame
    // load step to a scan rather than a search.
    spiral_.clear();
    spiral_.reserve(usize(edge_) * edge_);
    for (int dz = -gridRadius_; dz <= gridRadius_; ++dz) {
        for (int dx = -gridRadius_; dx <= gridRadius_; ++dx) {
            spiral_.push_back({i16(dx), i16(dz)});
        }
    }
    std::sort(spiral_.begin(), spiral_.end(), [](const Offset& a, const Offset& b) {
        return a.dx * a.dx + a.dz * a.dz < b.dx * b.dx + b.dz * b.dz;
    });

    // **One ring wider, and in the same order**, for the group listings the
    // classification asks about. The order is what makes deferring a
    // classification cheap: listings arrive nearest-first, so the cells under
    // the player are the first to be answered and the horizon waits. Warming
    // row-major -- which is what this was -- had the far north-west corner
    // answered first and the ground being walked on last.
    warmSpiral_.clear();
    const int warm = gridRadius_ + 1;
    warmSpiral_.reserve(usize(warm * 2 + 1) * usize(warm * 2 + 1));
    for (int dz = -warm; dz <= warm; ++dz) {
        for (int dx = -warm; dx <= warm; ++dx) {
            warmSpiral_.push_back({i16(dx), i16(dz)});
        }
    }
    std::sort(warmSpiral_.begin(), warmSpiral_.end(), [](const Offset& a, const Offset& b) {
        return a.dx * a.dx + a.dz * a.dz < b.dx * b.dx + b.dz * b.dz;
    });
}

void WorldStreamer::setMeshDistance(int meshDistance, ChunkRenderer& renderer)
{
    if (!open_ || meshDistance < 1 || meshDistance == meshDistance_) {
        return;
    }

    // The grid is about to be rebuilt around a different radius, which moves
    // every column in it. Nothing may be reading one. See `offerColumnWork`.
    reclaimColumnWork();

    // The old grid is moved aside rather than indexed in place: the new one
    // wraps modulo a different edge, so a column's cell index changes even
    // though the column has not moved.
    std::vector<Cell> previous = std::move(cells_);

    meshDistance_ = meshDistance;
    loadRadius_ = meshDistance + 1;
    // The budget starts wide at every distance change; an over-budget pass is
    // what narrows it again, and a radius chosen against the old distance says
    // nothing about the new one.
    memoryRadius_ = loadRadius_;
    buildGrid();

    // The generator's working set scales with the radius being filled, so a
    // step up needs a bigger cache. It is only ever grown: shrinking would mean
    // discarding columns that neighbouring population passes are still writing
    // into, and those cannot be rebuilt -- see ChunkGenerator::growCacheTo.
    //
    // The wait is what makes it safe to reach into the generator at all from
    // here. It is a settings change, so a pause of one column's work is not
    // something a player will notice against the re-mesh that follows it.
    if (generator_ != nullptr) {
        waitForWorkerIdle();
        generator_->growCacheTo(mcver::ChunkGenerator::cacheColumnsFor(loadRadius_));
        {
            // The grid this was staged from has just been rebuilt around a
            // different radius. Nothing on it is wrong, but it is a frame out
            // of date and the next update() rewrites it from the new grid
            // anyway, so it starts empty rather than half-stale.
            std::lock_guard<std::mutex> guard(queueLock_);
            slate_.clear();
        }
        // The radius the generator retires against is derived from loadRadius_,
        // which has just moved. Republishing it here rather than waiting for the
        // centre to move keeps a step *up* from retiring ground the wider sweep
        // is about to want back.
        publishRetireCentre();
        // The wait paused the worker so it could not hand itself another job
        // while the generator's table was being moved under it. Let it go.
        resumeGeneration();
    }

    // Nothing is published yet -- the renderer's field was rebuilt with the
    // rest -- so every column that survives comes back through the same
    // neighbours-ready gate a freshly loaded one does.
    for (Cell& cell : previous) {
        if (cell.state == CellState::Empty || !centreSet_) {
            continue;
        }
        if (std::abs(cell.chunkX - centreX_) > gridRadius_
            || std::abs(cell.chunkZ - centreZ_) > gridRadius_) {
            // A multiplayer column is never sent twice, so it is held rather
            // than let go. See `openRemote`.
            if (remote_ && cell.column != nullptr) {
                remoteColumns_[remoteKey(cell.chunkX, cell.chunkZ)] = std::move(cell.column);
            }
            continue;  // outside the new radius: let it go
        }
        Cell& destination = cells_[cellIndex(cell.chunkX, cell.chunkZ)];
        destination = std::move(cell);
        destination.published = false;
    }

    // Residency changed the moment the grid did, and the caller may well read
    // it before the next update() -- a debug page reporting the old count would
    // look exactly like columns having been dropped and reloaded, which is the
    // thing this function exists not to do.
    countResidency();

    if (!centreSet_) {
        return;
    }

    // update() only republishes around a column that has just loaded, and after
    // this none has. Once over the grid is what puts the world back on screen
    // in the frame the setting changed rather than as columns trickle in.
    renderer.setCentre(centreX_, centreZ_);
    for (const Offset& offset : spiral_) {
        publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
    }
}

void WorldStreamer::setCubeFormat(mesh::CubeFormat format, ChunkRenderer& renderer)
{
    if (!open_ || format == builder_.cubeFormat()) {
        return;
    }
    builder_.setCubeFormat(format);
    republishAll(renderer);
}

void WorldStreamer::setGreedy(bool on, ChunkRenderer& renderer)
{
    if (on == builder_.greedy()) {
        return;
    }
    // Set even while closed, unlike the cube format: the next world opened
    // should mesh the way the setting says without anyone having to call this
    // again.
    builder_.setGreedy(on);
    if (open_) {
        republishAll(renderer);
    }
}

void WorldStreamer::republishAll(ChunkRenderer& renderer)
{
    // The grid keeps its columns -- this is a change of geometry, not of what
    // is loaded -- so all that has to happen is that every one of them goes
    // back through the publish gate and gets meshed again.
    for (Cell& cell : cells_) {
        cell.published = false;
    }
    if (!centreSet_) {
        return;
    }
    renderer.setCentre(centreX_, centreZ_);
    for (const Offset& offset : spiral_) {
        publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
    }
}

u32 WorldStreamer::dirtyColumns() const
{
    return cache_.stats().dirtyColumns;
}

void WorldStreamer::close(i64 nowMillis, void* progressContext, SaveProgressFn progress)
{
    if (!open_) {
        return;
    }

    // **Whoever was listening to this world is not listening to the next one.**
    // The streamer is a static that lives for the whole process and these two
    // point into a session that closes with the world -- a terrain pool and a
    // host, both gone by the time another world opens. Left set, the first
    // block the next world writes would call through a dangling pointer. Every
    // caller installs them after `open`, so clearing here is the pair to that.
    blockWatcher_ = nullptr;
    blockWatcherCtx_ = nullptr;
    terrainSource_ = TerrainSource{};
    serverViewRadius_ = kServerViewRadius;

    // **And the map, for exactly the same reason.** A survey answers on the I/O
    // thread into whatever registered it -- a screen that belongs to the session
    // now closing -- so the queue is dropped and the visitor forgotten before
    // anything else here runs. One already taken by the thread finishes against
    // an object that is still alive, because nothing is torn down until the
    // cache below is closed and its thread joined.
    cache_.cancelSurveys();

    if (remote_) {
        // Nothing is owed to anything: the server has the world. The tick goes
        // before the grid it reads, for the reason given further down.
        entityPools_ = {};
        entitiesBound_ = false;
        tick_.reset();
        light_.reset();
        cells_.clear();
        mapDirty_.clear();
        remoteColumns_.clear();
        remoteOps_.clear();
        edits_.clear();
        columnWork_ = nullptr;
        columnWorkCtx_ = nullptr;
        columnWorkCount_ = 0;
        columnWorkDone_ = 0;
        columnWorkPosted_ = false;
        columnWorkBusy_ = false;
        columnWorkReady_ = false;
        centreSet_ = false;
        remote_ = false;
        served_.clear();
        servedAreaCount_ = 0;
        servedOwed_ = 0;
        servedOwedScan_ = 0;
        servedCursor_ = 0;
        servedGenerating_ = -1;
        servedGeneratorGrown_ = 0;
        for (int i = 0; i < kMaxServedAreas; ++i) {
            servedAreaPending_[i] = 0;
            servedAreaPendingScan_[i] = 0;
        }
        open_ = false;
        return;
    }
    snapshotEntities();
    entityPools_ = {};
    entitiesBound_ = false;
    // The worker first, and before anything touches storage: it is the other
    // user of the slot, and joining it is what makes the rest of this function
    // single-threaded again.
    stopWorker();

    // Anything a tick changed and the autosave has not picked up yet. This has
    // to happen before the grid is torn down, because the columns it reads are
    // the grid's.
    flushTickDirty();

    // Anything the generator finished and has not handed over yet goes to the
    // card now. Dropping it would mean regenerating it next session, and
    // regenerating is not the same as reloading: the population order would be
    // the one this session's walk produced, not the one that made the columns
    // around it.
    if (generator_ != nullptr) {
        generator_->flush();
    }

    // The drain, watched rather than waited on, when the caller asked to be
    // told about it.
    //
    // `cache_.close` would do all of this in one blocking call, and did -- but
    // a call that returns when it is finished can say nothing while it runs,
    // and on a folder world with a few hundred dirty columns that is a still
    // screen for several seconds. So the writes are queued here, and the
    // counters are read as the I/O thread works through them.
    //
    // **`pump()` is what does the work.** Threaded, the I/O thread is what
    // drains the queue and pump() only keeps the harness honest; unthreaded,
    // where there is no I/O thread at all, pump() is what runs the writes and
    // the loop would spin for ever without it.
    //
    // It used to also be what made the *count* move -- the cache recounted its
    // columns there and nowhere else, so polling `stats()` without it reported
    // the same number for ever. `stats()` answers live now, so that is no
    // longer a reason to call it here; the writes are.
    if (progress != nullptr) {
        cache_.flush(false);
        cache_.pump();
        const u32 owed = cache_.stats().dirtyColumns;
        for (;;) {
            const u32 left = cache_.stats().dirtyColumns;
            progress(progressContext, owed - (left < owed ? left : owed), owed);
            if (left == 0) {
                break;
            }
            cache_.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    // **The tick world outlives the drain, and that is the fix for a hardware
    // crash rather than tidiness.** It used to be destroyed beside
    // `flushTickDirty` above, before the loop that has just run -- and that
    // loop calls back into the caller once per pumped write, which on the 3DS
    // draws a whole frame of the world behind the progress bar. One of those
    // passes borrows the `TickWorld` (a cart leans along the *track*, so the
    // minecart pass is the one entity draw that needs the world), so every
    // frame of the save screen was reading a freed object: the two words it
    // reads first are `TickAccess`'s context and function pointer, and in a
    // freed newlib chunk those two words are the bin's `fd` and `bk`. The
    // console jumped into `__malloc_av_`. See
    // crashlogs/009-save-with-a-minecart/.
    //
    // Nothing above needed it gone -- `flushTickDirty` has already handed the
    // tick's changes to the cache, and neither the generator flush nor the
    // drain asks the tick anything -- so it is released here, after the last
    // frame the progress callback can draw and before the grid it reads goes
    // away.
    tick_.reset();
    light_.reset();

    // **The ground held for the other consoles, before the cache shuts.** A
    // guest's last hour of mining is in these columns and nowhere else, so
    // they are saved here for the same reason the grid's are.
    for (auto& pair : served_) {
        releaseServed(pair.second, true);
    }
    served_.clear();
    servedAreaCount_ = 0;
    servedOwed_ = 0;
    servedOwedScan_ = 0;
    servedCursor_ = 0;
    servedGenerating_ = -1;
    servedGeneratorGrown_ = 0;
    for (int i = 0; i < kMaxServedAreas; ++i) {
        servedAreaPending_[i] = 0;
        servedAreaPendingScan_[i] = 0;
    }

    // close() blocks until the last column is on the card, which is what the
    // "Saving level.." message on the way out is for. Everything the generator
    // just flushed went into the cache, so this is where it becomes files --
    // level.dat, the lock and the format's own commit included, which is why it
    // still runs when the drain above has already emptied the queue.
    cache_.close(nowMillis, player_);
    cells_.clear();
    finished_.clear();
    completed_.clear();
    slate_.clear();
    mapDirty_.clear();
    // stopWorker() joined the worker above, so nothing is reading a column and
    // there is nobody left to hand a result to.
    columnWork_ = nullptr;
    columnWorkCtx_ = nullptr;
    columnWorkCount_ = 0;
    columnWorkDone_ = 0;
    columnWorkPosted_ = false;
    columnWorkBusy_ = false;
    columnWorkReady_ = false;
    queuePaused_ = false;
    centreSet_ = false;
    generator_.reset();
    open_ = false;
}

// ---- a multiplayer world ---------------------------------------------------

bool WorldStreamer::openRemote(int meshDistance)
{
    tick_.reset();
    light_.reset();
    tickDirtyCells_ = 0;
    player_ = {};
    entityPools_ = {};
    entitiesBound_ = false;

    remote_ = true;
    remoteColumns_.clear();
    remoteOps_.clear();
    edits_.clear();
    // A client serves nobody: the world it is looking at is not its own.
    served_.clear();
    servedAreaCount_ = 0;
    servedOwed_ = 0;
    servedOwedScan_ = 0;
    servedCursor_ = 0;
    servedGenerating_ = -1;
    servedGeneratorGrown_ = 0;
    for (int i = 0; i < kMaxServedAreas; ++i) {
        servedAreaPending_[i] = 0;
        servedAreaPendingScan_[i] = 0;
    }

    path_.clear();
    level_ = world::LevelData{};
    lastSaveMillis_ = 0;
    open_ = true;

    meshDistance_ = meshDistance;
    loadRadius_ = meshDistance + 1;
    memoryRadius_ = loadRadius_;
    // See the same line in `open`.
    centreSet_ = false;
    buildGrid();

    tick::TickAccess tickAccess;
    tickAccess.ctx = this;
    tickAccess.column = &WorldStreamer::tickColumn;
    tickAccess.changed = &WorldStreamer::tickBlockChanged;
    tickAccess.beforeWrite = &WorldStreamer::recordEdit;
    // No seed reaches a client in protocol 2, and nothing random ticks here.
    tick_ = std::make_unique<tick::TickWorld>(tickAccess, 0);

    world::LightAccess lightAccess;
    lightAccess.ctx = this;
    lightAccess.column = &WorldStreamer::tickColumn;
    lightAccess.sectionLit = &WorldStreamer::lightSectionLit;
    light_ = std::make_unique<world::LightUpdater>(lightAccess);

    builder_.reserveQuads(4096);
    return true;
}

void WorldStreamer::supplyColumn(std::unique_ptr<world::ChunkColumn> column)
{
    if (!remote_ || column == nullptr) {
        return;
    }
    const i32 cx = column->x;
    const i32 cz = column->z;
    remoteColumns_[remoteKey(cx, cz)] = std::move(column);
    remoteOps_.push_back(RemoteOp{cx, cz, false});
}

void WorldStreamer::unloadColumn(i32 chunkX, i32 chunkZ)
{
    if (!remote_) {
        return;
    }
    remoteColumns_.erase(remoteKey(chunkX, chunkZ));
    remoteOps_.push_back(RemoteOp{chunkX, chunkZ, true});
}

void WorldStreamer::drainRemote(ChunkRenderer& renderer)
{
    for (const RemoteOp& op : remoteOps_) {
        if (std::abs(op.x - centreX_) > gridRadius_ || std::abs(op.z - centreZ_) > gridRadius_) {
            continue;  // no cell: an arrival stays held, an unload has already let go
        }
        Cell& cell = cells_[cellIndex(op.x, op.z)];
        const bool here =
            cell.state != CellState::Empty && cell.chunkX == op.x && cell.chunkZ == op.z;

        if (op.unload) {
            if (here && cell.state == CellState::Loaded) {
                cell.column.reset();  // not kept: the server let it go
                dropCell(cell, renderer);
            }
            continue;
        }

        auto it = remoteColumns_.find(remoteKey(op.x, op.z));
        if (it == remoteColumns_.end() || !here) {
            // Unloaded again already, or a cell not classified yet --
            // classification takes it out of the map itself.
            continue;
        }
        if (cell.state == CellState::Loaded && cell.published) {
            renderer.dropColumn(op.x, op.z);
            cell.published = false;
        }
        std::unique_ptr<world::ChunkColumn> column = std::move(it->second);
        remoteColumns_.erase(it);
        adoptColumn(cell, std::move(column));
        ++stats_.adoptedThisFrame;

        // A neighbour already published was meshed with nothing here, which
        // only happens at the edge of the server's view; its faces toward this
        // column are re-made now that there is something to cull them against.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const Cell* neighbour = find(op.x + dx, op.z + dz);
                if ((dx != 0 || dz != 0) && neighbour != nullptr && neighbour->published) {
                    for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
                        renderer.invalidateSection(op.x + dx, sy, op.z + dz);
                    }
                }
            }
        }
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                publishIfReady(op.x + dx, op.z + dz, renderer);
            }
        }
    }
    remoteOps_.clear();
}

world::ChunkColumn* WorldStreamer::regionColumn(void* ctx, i32 chunkX, i32 chunkZ)
{
    auto* self = static_cast<WorldStreamer*>(ctx);
    if (Cell* cell = self->find(chunkX, chunkZ)) {
        if (cell->state == CellState::Loaded) {
            return cell->column.get();
        }
    }
    auto it = self->remoteColumns_.find(remoteKey(chunkX, chunkZ));
    return it != self->remoteColumns_.end() ? it->second.get() : nullptr;
}

void WorldStreamer::applyRegion(ChunkRenderer& renderer, const net::MapChunkRegion& region)
{
    if (!remote_ || region.sizeX <= 0 || region.sizeY <= 0 || region.sizeZ <= 0) {
        return;
    }
    // `gy.a(bz)`: the revert list forgets the box first.
    edits_.confirm(region.x, region.y, region.z, region.x + region.sizeX - 1,
                   region.y + region.sizeY - 1, region.z + region.sizeZ - 1);
    net::applyMapChunk(region, &WorldStreamer::regionColumn, this, &regionScratch_);

    const i32 cx0 = region.x >> 4;
    const i32 cz0 = region.z >> 4;
    const i32 cx1 = (region.x + region.sizeX - 1) >> 4;
    const i32 cz1 = (region.z + region.sizeZ - 1) >> 4;
    for (i32 cx = cx0; cx <= cx1; ++cx) {
        for (i32 cz = cz0; cz <= cz1; ++cz) {
            Cell* cell = find(cx, cz);
            if (cell == nullptr || cell->state != CellState::Loaded) {
                continue;
            }
            for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
                cell->masks[sy] = mesh::computeVisibility(cell->column->section(sy), visScratch_);
            }
            if (cell->published) {
                renderer.dropColumn(cx, cz);
                cell->published = false;
            }
            cell->freshlyAdopted = true;
            cell->mapSerial = ++blockSerial_;
            mapDirty_.push(cx, cz);
            publishIfReady(cx, cz, renderer);
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dz == 0) {
                        continue;
                    }
                    for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
                        renderer.invalidateSection(cx + dx, sy, cz + dz);
                    }
                }
            }
        }
    }
}

bool WorldStreamer::applyServerBlock(ChunkRenderer& renderer, i32 x, int y, i32 z,
                                     block::BlockId id, u8 data)
{
    if (!remote_ || tick_ == nullptr) {
        return false;
    }
    edits_.confirm(x, y, z, x, y, z);
    if (y < 0 || y >= world::ChunkColumn::kHeight) {
        return false;
    }

    if (tick_->chunkResident(x >> 4, z >> 4)) {
        RenderBracket draws(*this, renderer);
        applyingServer_ = true;
        const bool wrote = tick_->setBlockAndDataRaw(x, y, z, id, data);
        applyingServer_ = false;
        return wrote;
    }

    auto it = remoteColumns_.find(remoteKey(x >> 4, z >> 4));
    if (it == remoteColumns_.end()) {
        return false;
    }
    it->second->setBlock(x & 15, y, z & 15, id);
    it->second->setBlockData(x & 15, y, z & 15, data);
    net::refreshHeightMap(*it->second, &regionScratch_);
    return true;
}

void WorldStreamer::setRemoteTime(i64 time)
{
    if (tick_ != nullptr) {
        tick_->setTime(time);
    }
    level_.time = time;
}

void WorldStreamer::setRemoteSpawn(i32 x, i32 y, i32 z)
{
    level_.spawnX = x;
    level_.spawnY = y;
    level_.spawnZ = z;
}

void WorldStreamer::recordEdit(void* ctx, i32 x, int y, i32 z, block::BlockId oldBlock,
                               u8 oldData)
{
    auto* self = static_cast<WorldStreamer*>(ctx);
    if (!self->applyingServer_) {
        self->edits_.record(x, y, z, oldBlock, oldData);
    }
}

void WorldStreamer::revertEdit(void* ctx, i32 x, int y, i32 z, u16 block, u8 data)
{
    auto* self = static_cast<WorldStreamer*>(ctx);
    if (self->tick_ == nullptr || !self->tick_->chunkResident(x >> 4, z >> 4)) {
        return;
    }
    self->applyingServer_ = true;
    self->tick_->setBlockAndDataRaw(x, y, z, block, data);
    self->applyingServer_ = false;
}

void WorldStreamer::spawnPosition(double* x, double* y, double* z) const
{
    // No narrowing on the way out: level.dat already holds these as doubles,
    // and rounding a far-out player position to float here would move them by
    // up to a block before the game had drawn a frame.
    if (level_.player.present) {
        *x = level_.player.pos[0];
        *y = level_.player.pos[1];
        *z = level_.player.pos[2];
        return;
    }
    *x = double(level_.spawnX);
    *y = double(level_.spawnY);
    *z = double(level_.spawnZ);
}

int WorldStreamer::cellIndex(i32 chunkX, i32 chunkZ) const
{
    return floorMod(chunkZ, edge_) * edge_ + floorMod(chunkX, edge_);
}

WorldStreamer::Cell* WorldStreamer::find(i32 chunkX, i32 chunkZ)
{
    return const_cast<Cell*>(static_cast<const WorldStreamer*>(this)->find(chunkX, chunkZ));
}

WorldStreamer::Cell* WorldStreamer::loadedCell(i32 chunkX, i32 chunkZ)
{
    Cell* cell = find(chunkX, chunkZ);
    return cell != nullptr && cell->state == CellState::Loaded ? cell : nullptr;
}

const WorldStreamer::Cell* WorldStreamer::find(i32 chunkX, i32 chunkZ) const
{
    if (cells_.empty()) {
        return nullptr;
    }
    if (std::abs(chunkX - centreX_) > gridRadius_ || std::abs(chunkZ - centreZ_) > gridRadius_) {
        return nullptr;
    }
    const Cell& cell = cells_[cellIndex(chunkX, chunkZ)];
    if (cell.state == CellState::Empty || cell.chunkX != chunkX || cell.chunkZ != chunkZ) {
        return nullptr;
    }
    return &cell;
}

// Takes a column the world does have. **Nothing here can touch a card.**
//
// A cache hit is a clone; a miss is a request posted to the I/O thread and a
// cell left exactly as it was, to be asked about again next frame. That is the
// whole change: this used to be an open, a read, a close and a gzip inflate,
// inside the frame, behind a lock the generation worker held while it wrote.
WorldStreamer::LoadResult WorldStreamer::loadColumn(i32 chunkX, i32 chunkZ)
{
    Cell& cell = cells_[cellIndex(chunkX, chunkZ)];

    std::unique_ptr<world::ChunkColumn> column;
    switch (cache_.tryTake(chunkX, chunkZ, &column)) {
    case world::ChunkCache::Take::Took:
        adoptColumn(cell, std::move(column));
        return LoadResult::Loaded;

    case world::ChunkCache::Take::Missing:
        // Classified as present and unreadable now: a damaged file, or one
        // removed under us. Treat it as the edge of the world rather than
        // asking again every frame.
        cell.state = CellState::Absent;
        return LoadResult::Failed;

    case world::ChunkCache::Take::Pending:
        break;
    }
    return LoadResult::Pending;
}

void WorldStreamer::classifyCell(Cell& cell, i32 chunkX, i32 chunkZ)
{
    // A multiplayer cell asks the columns the server has already sent, and
    // nothing else: there is no card to ask and nothing to generate.
    if (remote_) {
        cell.column.reset();
        cell.chunkX = chunkX;
        cell.chunkZ = chunkZ;
        cell.published = false;
        auto it = remoteColumns_.find(remoteKey(chunkX, chunkZ));
        if (it != remoteColumns_.end()) {
            std::unique_ptr<world::ChunkColumn> column = std::move(it->second);
            remoteColumns_.erase(it);
            adoptColumn(cell, std::move(column));
        } else {
            cell.state = CellState::Awaited;
        }
        return;
    }

    // Asked once, and the answer kept. Which "no" it is -- the edge of a finite
    // world, or a frontier waiting to be filled -- is the whole difference
    // between a column that may be meshed against and one that may not.
    // **With no generator there is nothing to classify.** Whether the world has
    // a chunk only matters here because the answer decides what gets generated,
    // and it has to be taken before a sweep can change it. Without one, asking
    // is a second trip to the SD card for every chunk that is about to be read
    // anyway: the cell goes straight to OnDisk and loadColumn finds out by
    // reading, exactly as it did before any of this existed.
    //
    // The question is answered from the cache's group index, and **the cell is
    // left alone when the index cannot answer yet**. That is what removes the
    // storm: crossing a chunk boundary used to re-classify a whole row of
    // cells, each one falling back to a `stat` on the render thread that first
    // had to wait for whatever file the I/O thread had open. The symptom was
    // the game stopping for a second or two while moving, with the storage
    // page's main-thread figure climbing to match. Now the group listing is
    // asked for urgently and this cell is asked about again next frame; the
    // sweep that must not run before the answer arrives is held back by
    // classifiedAround rather than by the frame.
    bool exists = true;
    if (generator_ != nullptr) {
        switch (cache_.chunkPresence(chunkX, chunkZ)) {
        case world::ChunkCache::Presence::Present:
            exists = true;
            break;
        case world::ChunkCache::Presence::Absent:
            exists = false;
            break;
        case world::ChunkCache::Presence::Unknown:
            return;  // the listing is on its way; the cell stays Empty
        }
    }

    cell.column.reset();
    cell.chunkX = chunkX;
    cell.chunkZ = chunkZ;
    cell.published = false;
    if (exists) {
        cell.state = CellState::OnDisk;
    } else {
        cell.state = generator_ != nullptr ? CellState::Ungenerated : CellState::Absent;
    }
}

// The 7x7 around a candidate column, which is the reach of the sweep that would
// make it. See the note on the declaration -- and note that every one of those
// cells exists: a candidate is inside the load radius, the grid is three rings
// wider, so three rings out from a candidate is still inside the grid.
bool WorldStreamer::classifiedAround(i32 chunkX, i32 chunkZ) const
{
    for (i32 dz = -3; dz <= 3; ++dz) {
        for (i32 dx = -3; dx <= 3; ++dx) {
            const Cell* cell = find(chunkX + dx, chunkZ + dz);
            if (cell == nullptr || cell->state == CellState::Empty) {
                return false;
            }
        }
    }
    return true;
}

void WorldStreamer::adoptColumn(Cell& cell, std::unique_ptr<world::ChunkColumn> column)
{
    const i32 chunkX = column->x;
    const i32 chunkZ = column->z;
    for (int sy = 0; sy < world::ChunkColumn::kSectionCount; ++sy) {
        cell.masks[sy] = mesh::computeVisibility(column->section(sy), visScratch_);
    }
    // **The tick's column cache holds a raw pointer at whatever was here.**
    // Replacing the cell's column leaves that pointer at a freed object, and
    // TickWorld's contract says whoever moves a column has to say so. It was
    // only ever safe because `update()` happens to run before `stepTicks()`
    // every frame and `tick()` invalidates on the way in -- an ordering nothing
    // enforced, and one that anything reading the tick outside a tick breaks.
    // Caught by AddressSanitizer as a heap-use-after-free.
    if (tick_ != nullptr) {
        tick_->invalidateColumnCache();
    }

    cell.column = std::move(column);
    cell.chunkX = chunkX;
    cell.chunkZ = chunkZ;
    cell.state = CellState::Loaded;
    cell.published = false;
    cell.freshlyAdopted = true;
    // A column the map may have a sample of from a previous visit, and one it
    // cannot tell from the cell's last tenant by coordinates alone. A fresh
    // serial says "this is not what you last drew" to anything holding an old
    // one; see `blockChangeSerial`.
    cell.mapSerial = ++blockSerial_;
    // On the same list a block change goes on, so a reader learns about ground
    // arriving and ground changing by one road rather than two. A column that
    // is adopted twice without being dropped is on it once.
    mapDirty_.push(chunkX, chunkZ);

    // `cn.b(Lcu;)V` -- the tile entities this column carries join the running
    // world. See the note on `setColumnSinks`.
    if (columnAdopted_ != nullptr) {
        columnAdopted_(columnSinkCtx_, *cell.column);
    }
}

// **How much world the generator is allowed to remember**, as a radius around
// the player, in chunks beyond the load radius.
//
// The floor is 3: provide() sweeps (cx-3..cx+2) and the streamer asks for
// columns out to loadRadius_, so anything nearer than loadRadius_ + 3 is
// something a sweep can still reach. The rest is slack, and it is not
// decoration -- retiring a column the next sweep wants back means re-deriving
// it, and a column re-derived beside a neighbour that has already been handed
// out is the one case where a pass does not re-run and its work is lost. So the
// radius is set well clear of the working set and retirement only ever fires on
// ground the player has genuinely left.
//
// Measured at render distance 7, whose cache is 224 columns, over a 4,500-frame
// `--fly ... gen` -- peak live against the slack:
//
//     4 -> 176      5 -> 178      6 -> 192      8 -> 208
//
// Six is the balance: three rings clear of anything a sweep can reach, and 14%
// of the cache still spare. Across render distances 3 to 11 the peak comes out
// at 128, 158, 192, 224 and 256 -- **`16 * loadRadius + 64` almost exactly,
// against `cacheColumnsFor`'s `16 * loadRadius + 96`**, so the headroom is a
// flat 32 columns at every distance rather than a fraction that thins out. And
// it is flat in *time* as well: 196 at 6,000 frames of walking and 188 at
// 9,000. `evictedLive` and the failed sweeps are zero throughout.
//
// Without retirement at all the same walk peaks at 468 against a cache of 240,
// and the world stops generating for the rest of the session.
constexpr int kRetireSlackChunks = 6;

void WorldStreamer::publishRetireCentre()
{
    std::lock_guard<std::mutex> guard(queueLock_);
    retireCentreX_ = centreX_;
    retireCentreZ_ = centreZ_;
    retireRadius_ = loadRadius_ + kRetireSlackChunks;
    retireCentreSet_ = centreSet_;

    // **The camera's centre and the one served area that may generate.**
    //
    // Not every area, which is the version this started as: retirement is by
    // region, so a centre in the list is a region the generator keeps -- and
    // keeping three guests' frontiers at once needs three guests' worth of
    // cache, which is 11 MB of block pool on a console that has not got it.
    // Only the area on the slate holds a frontier; the others are finished,
    // delivered and evictable. See `servedGeneratorSlack`.
    //
    // A guest who is not the one generating therefore has their region retired,
    // which is safe -- a region always goes as a unit -- and costs a re-derive
    // when their turn comes round. It comes round only when the area before
    // them owes nothing, so a filled square is not swept twice.
    retireExtraCount_ = 0;
    if (servedGenerating_ >= 0 && servedGenerating_ < servedAreaCount_) {
        retireExtra_[0].chunkX = servedAreas_[servedGenerating_].chunkX;
        retireExtra_[0].chunkZ = servedAreas_[servedGenerating_].chunkZ;
        retireExtraCount_ = 1;
    }
    // And how much cache that second frontier needs, for the worker to grow to
    // before its next sweep. Never shrunk: `growCacheTo` only grows.
    wantedGeneratorColumns_ =
        mcver::ChunkGenerator::cacheColumnsFor(loadRadius_) + servedGeneratorGrown_;
    // The radius is the camera's, which is the wider of the two: a served area
    // is the server's view distance and the grid is the render distance plus
    // `kRetireSlackChunks`, so one number covers both.
}

bool WorldStreamer::generateColumn(i32 chunkX, i32 chunkZ)
{
    // **Touches nothing the main thread owns.** No cells, no renderer, no
    // visibility masks -- the finished column goes on the queue and
    // drainGenerated() puts it in the grid. That is the whole reason this is
    // safe to call from the worker.

    // -------------------------------------------------------------------
    // **A job for a chunk the world already has does nothing at all**, and
    // this early return is what keeps the worker from changing the world.
    //
    // Population order is the world in a1.1.2: two chunks whose passes reach
    // the same blocks come out differently depending on which ran first, so the
    // sequence of sweeps has to be a function of the game and not of the
    // scheduler. Everything else about that falls out of asking for columns in
    // spiral order -- the nearest missing column is the nearest missing column
    // however many frames have gone by, because the probe walks the same spiral
    // the requests do.
    //
    // The one thing that would not fall out is this: a column can be staged as
    // missing and then written by the sweep of a *neighbour* before the worker
    // reaches it. Sweeping for it anyway would reach three rings further out and
    // generate -- and populate -- ground that the synchronous path never would,
    // and how often that happened would depend on how many frames a generation
    // took. Taking the stored column instead makes such a job a no-op, which is
    // exactly what it is.
    // -------------------------------------------------------------------
    {
        auto stored = std::make_unique<world::ChunkColumn>(chunkX, chunkZ);
        const bool have = cache_.load(chunkX, chunkZ, stored.get()) != nullptr;
        if (have && stored->terrainPopulated) {
            std::lock_guard<std::mutex> guard(queueLock_);
            finished_.push_back(std::move(stored));
            return true;
        }
    }

    // **Let go of everything the player has walked away from, first.**
    //
    // A column the generator has not handed over cannot be evicted -- its
    // neighbours' populations live in it and nowhere else -- and nothing ever
    // finishes the ones a moving centre abandons at the sides of the corridor
    // it sweeps. Left alone that set grows with the distance walked until it
    // fills the cache, at which point acquire() takes a live column anyway and
    // the world starts coming back with half its trees. Retiring by distance
    // turns the walking case back into the standing-still case the cache is
    // sized for. See ChunkGenerator::retire.
    //
    // Here rather than on the main thread because the generator belongs to
    // whichever thread is inside it, and this is that thread.
    {
        mcver::ChunkGenerator::Centre centres[1 + kMaxServedAreas];
        int count = 0;
        int rr = 0;
        bool set = false;
        int wantedColumns = 0;
        {
            std::lock_guard<std::mutex> guard(queueLock_);
            set = retireCentreSet_;
            rr = retireRadius_;
            wantedColumns = wantedGeneratorColumns_;
            if (set) {
                centres[count++] = {retireCentreX_, retireCentreZ_};
            }
            for (int i = 0; i < retireExtraCount_; ++i) {
                centres[count++] = retireExtra_[i];
            }
        }
        // **The cache first, and on this thread.** A session serving ground to
        // a guest outside the grid needs room for a second frontier
        // (`servedGeneratorSlack`), and the main thread must not resize the
        // vectors this function is about to read -- so it publishes a number
        // and this grows to it, here, where the generator is this thread's.
        // `growCacheTo` ignores anything not larger than what it already has.
        if (wantedColumns > 0) {
            generator_->growCacheTo(wantedColumns);
        }
        if (set) {
            generator_->retire(centres, count, rr);
        }
    }

    if (!generator_->provide(chunkX, chunkZ, generated_.get())) {
        // **Counted, because a silent one stops the world.** The nearest owed
        // column is asked for again next frame and fails again, so a column
        // that cannot be made is a frontier that never advances -- and until
        // this counter existed, the only symptom was generation appearing to
        // stop with nothing on the debug page to say why. It means the
        // generator's own cache could not hold the sweep; see
        // ChunkGenerator::cacheColumnsFor and the `lost` figure beside it.
        std::lock_guard<std::mutex> guard(queueLock_);
        ++generationFailures_;
        return false;
    }

    // The column that was asked for is the one provide() writes out rather than
    // hands to deliver(), so it is saved and queued here. Everything else the
    // sweep finished has already gone through deliver().
    cache_.save(*generated_);
    auto owned = std::make_unique<world::ChunkColumn>(std::move(*generated_));
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        finished_.push_back(std::move(owned));
    }
    return true;
}

// **The slate is rewritten, not appended to: the nearest columns that are owed,
// as of this frame, straight off the grid.**
//
// The spiral is already sorted nearest-first, so the first `kSlateDepth`
// candidates it yields are the next few jobs in the order a1.1.2 asks for them
// -- see the note on slate_ for the jar evidence and for what the queue that
// used to be here was costing.
//
// A column the worker has in hand, or one it has finished and the main thread
// has not adopted yet, is passed over: both still read `Ungenerated` on the
// grid, and putting either back on the slate would run a second sweep for it.
// A column whose neighbourhood is not fully classified **stops the walk**
// rather than being passed over; the note where that happens says why.
//
// There is no cap and nothing to refuse: the slate is at most kSlateDepth long
// by construction, and it is thrown away and rebuilt next frame.
//
// **queueLock_ held**, because the worker reads the slate.
void WorldStreamer::refreshSlate()
{
    slate_.clear();
    if (generator_ == nullptr || !centreSet_) {
        return;
    }

    bool blocked = false;
    for (const Offset& offset : spiral_) {
        if (slate_.size() >= kSlateDepth) {
            break;
        }
        if (std::abs(offset.dx) > admitRadius() || std::abs(offset.dz) > admitRadius()) {
            continue;
        }
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        const Cell& cell = cells_[cellIndex(cx, cz)];

        // **A cell that has not been asked about stops the walk**, for the same
        // reason a blocked one does below: nobody knows yet whether this column
        // is owed, and if it turns out to be, it has to be swept before
        // anything further out. Passing over it would put the choice of the
        // next sweep in the hands of whichever directory listing landed first.
        if (cell.state == CellState::Empty || cell.chunkX != cx || cell.chunkZ != cz) {
            blocked = true;
            break;
        }
        if (cell.state != CellState::Ungenerated) {
            continue;  // the world has it, or never will
        }

        const std::pair<i32, i32> at{cx, cz};
        if (jobActive_ && inFlight_ == at) {
            continue;
        }
        if (std::find(completed_.begin(), completed_.end(), at) != completed_.end()) {
            continue;
        }
        if (!classifiedAround(cx, cz)) {
            // **Stop, rather than skip to the next one.** Taking a column
            // further out because a nearer one is still waiting on a directory
            // listing would make the order of the sweeps a function of when
            // listings happened to arrive -- and the order of the sweeps is the
            // world. Nearest-first has to mean nearest-first whatever the card
            // is doing, so a blocked column blocks the ones behind it. It is
            // waiting on one listing that has already been asked for urgently.
            blocked = true;
            break;
        }
        slate_.push_back(at);
    }

    // Gated means the nearest ground that is owed cannot be made *yet*, which
    // is a listing away rather than a fault. It reads on the debug page next to
    // the count of cells still waiting to be asked about, because the two are
    // the same sentence.
    stats_.generationGated = blocked;

    // **And then, and only then, the ground a guest is standing on.**
    //
    // After the camera's and never instead of it: the player holding this
    // console must not watch their own world stop arriving because somebody
    // else is exploring. A blocked walk above means the nearest owed column
    // cannot be judged yet, and taking a served one while that is true would
    // put the order of the sweeps -- which is the world -- in the hands of
    // whichever directory listing landed first, exactly as the note above
    // says. So a blocked frame serves nobody and tries again next frame.
    //
    // There is no nearest-first order *between* two guests and there is no
    // order to be faithful to: a1.1.2's server generates for whoever asked,
    // and two players asking at once is a case the single-player jar does not
    // have. What is preserved is the thing that matters -- every column is
    // swept once, by one sweep, with its neighbours -- and that is
    // `ChunkGenerator`'s to keep, not this list's.
    //
    // **And one served area at a time.** Only the area whose turn it is is
    // swept, because only one guest's frontier fits in the generator's cache
    // beside the camera's; see `WorldStreamer::servedGeneratorSlack`.
    if (blocked || servedGenerating_ < 0 || servedGenerating_ >= servedAreaCount_) {
        return;
    }
    const ServedArea& turn = servedAreas_[servedGenerating_];
    for (const auto& pair : served_) {
        if (slate_.size() >= kSlateDepth) {
            break;
        }
        if (!pair.second.owed || pair.second.column != nullptr) {
            continue;
        }
        const i32 cx = i32(u32(u64(pair.first) >> 32));
        const i32 cz = i32(u32(u64(pair.first)));
        if (std::abs(cx - turn.chunkX) > turn.radius
            || std::abs(cz - turn.chunkZ) > turn.radius) {
            continue;
        }
        const std::pair<i32, i32> at{cx, cz};
        if (jobActive_ && inFlight_ == at) {
            continue;
        }
        if (std::find(completed_.begin(), completed_.end(), at) != completed_.end()) {
            continue;
        }
        if (std::find(slate_.begin(), slate_.end(), at) != slate_.end()) {
            continue;
        }
        slate_.push_back(at);
    }
}

// **queueLock_ held.** The worker calls this as well as the main thread.
bool WorldStreamer::takeSlateLocked(std::pair<i32, i32>* out)
{
    if (slate_.empty()) {
        return false;
    }
    *out = slate_.front();
    slate_.erase(slate_.begin());
    return true;
}

// Makes the columns on the slate when there is no worker to make them.
//
// **a1.1.2 has no worker and no queue at all.** `ft.b` (provideChunk) loads the
// chunk and, failing that, calls the generator *inline* on the game thread, so
// the order is simply the order things ask for chunks -- and what asks is the
// renderer, which does `Arrays.sort(worldRenderers, new RenderSorter(player))`
// before it rebuilds them. Nearest-to-the-player, verified in the jar: class `e`
// at bytecode 686, comparator `fb`, ordering on `WorldRenderer.a(Entity)`
// ascending. This path is that, one column per frame; the worker is that with
// the stall taken off the frame.
void WorldStreamer::pumpGeneration(const Budget& budget)
{
    if (generator_ == nullptr) {
        return;
    }

    if (workerRunning_) {
        // **Nothing is handed out here.** The worker takes its own next job the
        // moment it finishes one; all this does is make sure it is awake.
        // Dispatching from here capped generation at one column per rendered
        // frame, which on a console holding thirty frames a second was thirty
        // columns a second however fast core 2 could actually go.
        std::lock_guard<std::mutex> guard(queueLock_);
        if (!slate_.empty()) {
            wake_.notify_one();
        }
        return;
    }

    // No worker. This is the path that stutters, and it is kept only so a
    // console whose thread would not start still fills its world in.
    for (int made = 0; made < budget.generatedPerFrame; ++made) {
        std::pair<i32, i32> at{0, 0};
        {
            std::lock_guard<std::mutex> guard(queueLock_);
            if (!takeSlateLocked(&at)) {
                break;
            }
        }
        const auto begin = std::chrono::steady_clock::now();
        generateColumn(at.first, at.second);
        stats_.generateMicros += std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count();
    }
}

void WorldStreamer::dropCell(Cell& cell, ChunkRenderer& renderer)
{
    if (cell.state == CellState::Empty) {
        return;
    }
    if (cell.published) {
        renderer.dropColumn(cell.chunkX, cell.chunkZ);
    }

    // **A column with tick edits is saved before it is let go.** `give()`
    // installs what it is handed as *clean*, and once the cell is gone
    // flushTickDirty cannot find it -- so a column edited by a tick and then
    // walked away from before the next autosave was never written to the card,
    // and with autosave set to Off that was every edit of the session. Worse,
    // `give()` keeps whatever the cache already holds under those coordinates
    // on the grounds that it is "at least as fresh", which stops being true the
    // moment the resident copy carries edits the cached one does not.
    //
    // Saving here costs one clone and one queued write, on a path that already
    // runs only when the render distance moves past a column.
    if (cell.tickDirty && cell.column != nullptr && !remote_) {
        if (columnSaving_ != nullptr) {
            columnSaving_(columnSinkCtx_, *cell.column);
        }
        cache_.save(*cell.column, world::ChunkCache::SavePressure::Defer);
    }
    if (cell.tickDirty) {
        cell.tickDirty = false;
        if (tickDirtyCells_ > 0) --tickDirtyCells_;
    }

    // `cn.c(Lcu;)V`, and it happens **after** the save above rather than
    // before it: the saving sink reads the very entries this one erases, so
    // dropping first would write a column with no sign text in it.
    if (columnDropped_ != nullptr) {
        columnDropped_(columnSinkCtx_, cell.chunkX, cell.chunkZ);
    }

    // **Given back rather than freed.** A column that leaves the grid is one
    // the player may walk straight back into, and re-reading it costs an open,
    // a read and an inflate. The cache keeps it until its byte cap says
    // otherwise, so turning round is free. It is also what makes the read-ahead
    // band worth having: the two are the same table.
    if (cell.column != nullptr) {
        if (remote_) {
            // Still loaded on the server, which will not send it again: kept,
            // so walking back into it costs nothing. See `openRemote`.
            remoteColumns_[remoteKey(cell.chunkX, cell.chunkZ)] = std::move(cell.column);
        } else {
            cache_.give(std::move(cell.column));
        }
    }
    cell.column.reset();
    cell.state = CellState::Empty;
    cell.published = false;

    // Same contract as adoptColumn: the column this cell held may have just
    // been freed, and the tick caches a raw pointer to it.
    if (tick_ != nullptr) {
        tick_->invalidateColumnCache();
    }
}

bool WorldStreamer::neighboursReady(i32 chunkX, i32 chunkZ) const
{
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            // Absent counts as ready: the world genuinely ends there, and
            // waiting for a chunk that will never arrive would leave a ring of
            // permanently unmeshed columns at the edge of a finite world.
            //
            // **Ungenerated does not.** That chunk is on its way, and meshing
            // against it now would cull the faces of this column against air
            // and never come back to fix them -- a wall of holes along the
            // frontier, one ring behind wherever the player has walked.
            const Cell* cell = find(chunkX + dx, chunkZ + dz);
            if (cell == nullptr || cell->state == CellState::Empty
                || cell->state == CellState::OnDisk || cell->state == CellState::Ungenerated) {
                return false;
            }
            // A server column is on its way only inside the server's view.
            if (cell->state == CellState::Awaited
                && std::abs(chunkX + dx - centreX_) <= serverViewRadius_
                && std::abs(chunkZ + dz - centreZ_) <= serverViewRadius_) {
                return false;
            }
        }
    }
    return true;
}

void WorldStreamer::publishIfReady(i32 chunkX, i32 chunkZ, ChunkRenderer& renderer)
{
    Cell* cell = find(chunkX, chunkZ);
    if (cell == nullptr || cell->state != CellState::Loaded) {
        return;
    }
    if (!renderer.inRange(chunkX, chunkZ)) {
        return;
    }

    // ---------------------------------------------------------------------
    // **`published` is a cache of "the renderer has this column", and the
    // renderer can invalidate it without telling anyone.**
    //
    // The field wraps modulo the render distance, so a column one ring outside
    // it shares a cell with the column on the opposite side -- and when that
    // one is published, `publishColumn` releases this one's meshes and takes
    // the cell. Nothing tells this class. Walk out past the render distance and
    // back and the flag still says "published" while the field holds somebody
    // else, so the column is never handed over again: the walk finds its cell
    // occupied by a different column, treats this one as not loaded, and draws
    // nothing of it. Changing the render distance put it right, because that
    // rebuilds the field and republishes everything -- which is exactly the
    // shape of the bug report this comes from.
    //
    // The streamer's grid is three rings wider than what it loads, so the band
    // where this happens is real and a few chunks of walking wide. Asking the
    // renderer costs one lookup and makes the flag advisory instead of load
    // bearing.
    // ---------------------------------------------------------------------
    if (cell->published && renderer.hasColumn(chunkX, chunkZ)) {
        return;
    }
    if (!neighboursReady(chunkX, chunkZ)) {
        return;
    }

    // A column that has been read in since the renderer last saw it cannot
    // inherit what the renderer still holds under its coordinates -- see
    // Cell::freshlyAdopted. Dropping first is what makes publishColumn treat it
    // as an arrival rather than as the same column coming back, so its sections
    // are meshed again instead of drawing slots that belong to somebody else
    // now.
    if (cell->freshlyAdopted) {
        renderer.dropColumn(chunkX, chunkZ);
    }
    cell->published = renderer.publishColumn(chunkX, chunkZ, cell->masks);
    if (cell->published) {
        cell->freshlyAdopted = false;
    }
}

bool WorldStreamer::meshSection(const VisibleSection& section, ChunkRenderer& renderer)
{
    const Cell* cell = find(section.chunkX, section.chunkZ);
    if (cell == nullptr || cell->state != CellState::Loaded) {
        // Only published columns reach the queue and only loaded ones are
        // published, so this is the column having gone in between. There is
        // nothing to do and nothing wrong: true, so the caller moves on to the
        // next section rather than treating it as a full pool.
        return true;
    }

    const int sy = int(section.sectionY);

    // The cheap rejection: 36 % of a real world's sections are uniform air and
    // never reach the mesher at all.
    if (mesh::sectionIsEmpty(*cell->column, sy)) {
        renderer.noteEmptySection(section.chunkX, sy, section.chunkZ);
        ++stats_.emptyThisFrame;
        return true;
    }

    mesh::ColumnNeighbourhood neighbourhood;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const Cell* n = find(section.chunkX + dx, section.chunkZ + dz);
            neighbourhood.at(dx, dz) = n != nullptr ? n->column.get() : nullptr;
        }
    }

    builder_.clear();
    scratch_.fill(neighbourhood, sy);
    mesh::meshSection(scratch_, builder_);

    if (builder_.empty()) {
        renderer.noteEmptySection(section.chunkX, sy, section.chunkZ);
        ++stats_.emptyThisFrame;
        return true;
    }

    // One contiguous buffer per section: cubes, opaque detail, translucent
    // detail, in that order.
    const mesh::MeshRanges ranges = builder_.ranges();
    staging_.resize(ranges.total());
    builder_.copyTo(staging_.data());

    if (!renderer.uploadSection(section.chunkX, sy, section.chunkZ, staging_.data(), ranges)) {
        // The pool is full of geometry this frame is already drawing. The
        // section stays unmeshed and comes back round; nothing is lost but the
        // work, which is why the mesh budget is small.
        return false;
    }

    ++stats_.meshedThisFrame;
    return true;
}

void WorldStreamer::update(ChunkRenderer& renderer, i32 cameraChunkX, i32 cameraChunkZ,
                       const Budget& budget)
{
    stats_.columnsLoadedThisFrame = 0;
    stats_.meshedThisFrame = 0;
    stats_.emptyThisFrame = 0;
    stats_.generatedThisFrame = 0;
    stats_.adoptedThisFrame = 0;
    stats_.generateMicros = 0;
    stats_.pendingGeneration = 0;

    if (!open_) {
        return;
    }

    // **Before anything here touches a cell.** The worker may be reading a
    // column the next few hundred lines can drop, re-adopt or mesh, and this is
    // the line that says it is not. See `offerColumnWork`.
    reclaimColumnWork();

    // Moving the centre invalidates nothing by itself -- the grid wraps -- but
    // a cell now holding a column from outside the new radius has to go, or it
    // would answer for a column that is no longer there.
    //
    // The first frame always takes this path even if the camera happens to be
    // standing at chunk (0, 0), because the renderer's centre has to be told
    // once before the walk can find anything.
    const bool centreMoved =
        !centreSet_ || cameraChunkX != centreX_ || cameraChunkZ != centreZ_;
    if (centreMoved) {
        centreSet_ = true;
        centreX_ = cameraChunkX;
        centreZ_ = cameraChunkZ;
        renderer.setCentre(cameraChunkX, cameraChunkZ);
        publishRetireCentre();

        for (Cell& cell : cells_) {
            if (cell.state == CellState::Empty) {
                continue;
            }
            if (std::abs(cell.chunkX - centreX_) > gridRadius_
                || std::abs(cell.chunkZ - centreZ_) > gridRadius_) {
                dropCell(cell, renderer);
            }
        }

        // Only here, and deliberately: the set of groups worth listing and
        // columns worth reading ahead changes when the centre does and at no
        // other time, so doing this per frame would be several hundred lookups
        // a frame to reach the same conclusion.
        warmAndPrefetch();
    }

    // Whatever the worker finished since the last frame, and after the centre
    // has moved so a result is judged against where the player is now. A column
    // adopted here is one the scan below does not have to ask storage about.
    drainGenerated(renderer);
    if (remote_) {
        drainRemote(renderer);
    }

    // **The ground the other consoles are standing on**, after the grid's own
    // and on a budget of the same size. That is a second read posted per frame
    // while a guest is outside the grid, and it is deliberate rather than free:
    // the reads are the I/O thread's and never the frame's, and generation --
    // the expensive half -- is still strictly the camera's first, because
    // `refreshSlate` will not touch a served column until the camera's own
    // spiral is clear. One branch in single player. See `setServedAreas`.
    if (!remote_) {
        pumpServed(budget.columnsPerFrame);
    }

    // **Classification first, over the whole grid, and unbudgeted.** It is one
    // index lookup per cell and it never touches a card, so "unbudgeted" now
    // means what it says: the frame cost is a lookup, not an SD operation. A
    // cell whose group has not been listed yet is left Empty and asked about
    // again next frame -- see classifyCell.
    stats_.unclassified = 0;
    for (const Offset& offset : spiral_) {
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        Cell& cell = cells_[cellIndex(cx, cz)];

        const bool stale = cell.state != CellState::Empty
                           && (cell.chunkX != cx || cell.chunkZ != cz);
        if (stale) {
            dropCell(cell, renderer);
        }
        if (cell.state == CellState::Empty) {
            classifyCell(cell, cx, cz);
        }
        if (cell.state == CellState::Empty) {
            ++stats_.unclassified;
        }
    }

    // **Republish everything the camera has just brought back into range.**
    //
    // Publishing only ever happened where something arrived -- around a column
    // that had just been read, or over the whole grid when a generated one was
    // adopted. Walking back over ground that is already in memory and already
    // on the card does neither: nothing is loaded and nothing is generated, so
    // nothing published, and a column that had lost its cell to the field's
    // wrap never got it back. That is the other half of the bug the note in
    // publishIfReady describes, and it is why the symptom needed *revisiting*
    // rather than merely arriving.
    //
    // Once per chunk-boundary crossing, and after classification so that a
    // newly exposed neighbour has a state for neighboursReady to read. Most of
    // the sweep exits on the range check; what is left is a lookup each.
    if (centreMoved) {
        for (const Offset& offset : spiral_) {
            publishIfReady(centreX_ + offset.dx, centreZ_ + offset.dz, renderer);
        }
    }

    // Then read what the world already has, nearest first and within the
    // budget, and count what it does not.
    int loaded = 0;
    int pending = 0;
    int pendingReads = 0;
    int pendingGeneration = 0;
    for (const Offset& offset : spiral_) {
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        if (std::abs(offset.dx) > admitRadius() || std::abs(offset.dz) > admitRadius()) {
            continue;  // classification only out here, or squeezed out by the budget
        }
        Cell& cell = cells_[cellIndex(cx, cz)];

        if (cell.state == CellState::Ungenerated) {
            // Counted here; which of them the worker is given is decided in
            // refreshSlate, once the whole spiral has been walked.
            ++pendingGeneration;
            continue;
        }

        if (cell.state != CellState::OnDisk) {
            continue;
        }

        ++pending;
        if (loaded >= budget.columnsPerFrame) {
            continue;
        }

        // **A pending column costs nothing and must not spend the budget.** It
        // is a request posted to the I/O thread, not work done here, and
        // counting it would mean one column still being read held up every
        // other one in the spiral behind it -- which at a budget of one or two
        // a frame is most of the world.
        const LoadResult result = loadColumn(cx, cz);
        if (result == LoadResult::Pending) {
            ++pendingReads;
            continue;
        }
        if (result == LoadResult::Loaded) {
            ++stats_.columnsLoadedThisFrame;
        }
        ++loaded;

        // A column arriving completes its own neighbourhood and possibly its
        // eight neighbours', so all nine are re-examined rather than just this
        // one.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                publishIfReady(cx + dx, cz + dz, renderer);
            }
        }
    }
    stats_.pendingGeneration = pendingGeneration;

    // **After the whole spiral, because the slate is the head of it.** The scan
    // above is what put the grid in the state this reads.
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        refreshSlate();
    }

    pumpGeneration(budget);

    stats_.pendingColumns = pending;
    stats_.pendingReads = pendingReads;

    // With no worker the generator ran on this thread, so its counters can be
    // read directly. With one they arrive through drainGenerated, published by
    // the worker under the queue lock.
    if (generator_ != nullptr && !workerRunning_) {
        stats_.generatorPeakLive = generator_->stats().peakLive;
        stats_.generatorEvictedLive = generator_->stats().evictedLive;
        stats_.generationUnlightable = generator_->stats().sweepUnlightable;
        stats_.generationIncomplete = generator_->stats().sweepIncomplete;
        stats_.generatorRetiredLive = generator_->stats().retiredLive;
    }
    stats_.workerRunning = workerRunning_;

    // Anything the walk asked for, up to the budget. The queue is already cut
    // to it by the renderer and is ordered nearest first.
    for (const VisibleSection& section : renderer.meshQueue()) {
        if (!meshSection(section, renderer)) {
            break;
        }
    }

    // Last, so the counters describe the frame that just happened. Unthreaded
    // this also does one unit of queued I/O, which is how the host harnesses
    // make progress without a thread.
    // One lock and one struct copy, not two. `prefetchHits` is cumulative, and
    // `stats_.io` still holds last frame's, so the delta needs no second read.
    const u32 prefetchHitsBefore = stats_.io.prefetchHits;
    cache_.pump();
    stats_.io = cache_.stats();
    stats_.prefetched = int(stats_.io.prefetchHits - prefetchHitsBefore);

    countResidency();

    // **After the count, because it spends the number the count just made.**
    // Dropping columns here rather than before it also means the figure the
    // debug page reads is the one left standing, not the one that triggered
    // the eviction.
    enforceMemoryBudget(renderer);
}

// Lists the directory groups the classification is about to ask about, and
// reads ahead the columns just outside what is loaded.
//
// **One ring wider than the grid**, because a group has to be listed before the
// cell that needs it is exposed, and a cell at `gridRadius_` was one chunk
// outside the grid a crossing ago. Sprinting can still outrun it, which is what
// the `stat` fallback in ChunkCache::hasChunk is for -- it is then as slow as it
// always was, for one cell, instead of for a whole row.
void WorldStreamer::warmAndPrefetch()
{
    // The index only earns anything where classification asks a question, and
    // classification only asks one when there is a generator; see classifyCell.
    if (generator_ != nullptr) {
        for (const Offset& offset : warmSpiral_) {
            cache_.warmGroup(centreX_ + offset.dx, centreZ_ + offset.dz);
        }
    }

    if (prefetchRings_ <= 0) {
        return;
    }

    // Capped at the classification band: a cell further out than that has no
    // cell of its own, so nothing here knows whether the world even has it.
    const int reach = std::min(loadRadius_ + prefetchRings_, gridRadius_);
    for (const Offset& offset : spiral_) {
        if (std::abs(offset.dx) > reach || std::abs(offset.dz) > reach) {
            continue;
        }
        if (std::abs(offset.dx) <= loadRadius_ && std::abs(offset.dz) <= loadRadius_) {
            continue;  // inside the load radius: the ordinary path reads it
        }
        const i32 cx = centreX_ + offset.dx;
        const i32 cz = centreZ_ + offset.dz;
        const Cell& cell = cells_[cellIndex(cx, cz)];
        if (cell.state == CellState::OnDisk && cell.chunkX == cx && cell.chunkZ == cz) {
            cache_.prefetch(cx, cz);
        }
    }
}

void WorldStreamer::setPlayerState(double x, double y, double z, float yaw, float pitch,
                                   i64 timeTicks)
{
    player_.valid = true;
    player_.pos[0] = x;
    player_.pos[1] = y;
    player_.pos[2] = z;
    player_.rotation[0] = yaw;
    player_.rotation[1] = pitch;
    player_.timeTicks = timeTicks;
}

void WorldStreamer::bindEntities(const entity::EntityPools& pools)
{
    entityPools_ = pools;
    entitiesBound_ = true;
    if (level_.entities) level_.entities->restore(pools);
}

void WorldStreamer::snapshotEntities()
{
    if (!entitiesBound_) return;
    auto state = std::make_shared<entity::PersistentEntities>();
    // A copy the heap would not hold keeps the last snapshot: an older set of
    // entities on disk is a save, a partial one is a loss.
    if (state->capture(entityPools_)) player_.entities = std::move(state);
}

void WorldStreamer::setPlayerVitals(i16 health, i16 hurtTime, i16 deathTime, i16 attackTime,
                                    i16 air, i16 fire)
{
    player_.valid = true;
    player_.hasVitals = true;
    player_.health = health;
    player_.hurtTime = hurtTime;
    player_.deathTime = deathTime;
    player_.attackTime = attackTime;
    player_.air = air;
    player_.fire = fire;
}

void WorldStreamer::setPlayerInventory(const std::vector<item::ItemStack>& stacks)
{
    player_.valid = true;
    player_.hasInventory = true;
    player_.inventory = stacks;
}

void WorldStreamer::tickSaves(i64 nowMillis)
{
    if (!open_ || remote_ || autosaveSeconds_ <= 0) {
        return;
    }
    if (nowMillis - lastSaveMillis_ < i64(autosaveSeconds_) * 1000) {
        return;
    }
    saveNow(nowMillis);
}

void WorldStreamer::saveNow(i64 nowMillis)
{
    if (!open_ || remote_) {
        return;
    }
    lastSaveMillis_ = nowMillis;
    snapshotEntities();

    // Whatever the tick changed since the last save becomes the cache's answer
    // for those columns now, so the flush below has something to write.
    flushTickDirty();

    // level.dat and session.lock have no other trigger during a session --
    // close() is the only thing that rewrites either today, so a console that
    // loses power mid-session comes back with a LastPlayed from whenever the
    // world was last left properly, and with a lock that has not been refreshed
    // since it was claimed. docs/world-format.md says to run the lock on a
    // timer; this is that timer.
    //
    // **Requested rather than done here.** Both are small files and it would be
    // easy to write them from this thread, which is the render thread -- and a
    // deflate plus an atomic write inside a frame every interval is precisely
    // the thing this whole arrangement exists to stop.
    //
    // **Writes are queued first, and the order is load-bearing.** The two calls
    // take `mutex_` separately, so the I/O thread can run between them. Ask for
    // housekeeping first and it can wake to an empty write queue, take the
    // housekeeping job, and reach `storage_.commit()` before this cycle's
    // chunks have been handed over -- which on the packed backend commits the
    // state *before* the autosave and leaves its own writes staged until the
    // next one. Queue the writes first and `takeJobLocked` settles it: writes
    // outrank housekeeping, so the commit necessarily follows them.
    flushSaves(false);
    cache_.requestHousekeeping(nowMillis, player_);
}

void WorldStreamer::flushSaves(bool blocking)
{
    if (!open_ || remote_) {
        return;
    }
    cache_.flush(blocking);
}

const world::ChunkColumn* WorldStreamer::residentColumn(i32 chunkX, i32 chunkZ) const
{
    const Cell* cell = find(chunkX, chunkZ);
    if (cell == nullptr || cell->state != CellState::Loaded) {
        return nullptr;
    }
    return cell->column.get();
}

const world::ChunkColumn* WorldStreamer::servedColumn(i32 chunkX, i32 chunkZ) const
{
    auto it = served_.find(remoteKey(chunkX, chunkZ));
    return it == served_.end() ? nullptr : it->second.column.get();
}

u32 WorldStreamer::columnBlockSerial(i32 chunkX, i32 chunkZ) const
{
    const Cell* cell = find(chunkX, chunkZ);
    // The same gate `residentColumn` uses, so a caller that reads the serial
    // and then asks for the column cannot be told "changed" by a cell whose
    // column it is not allowed to have.
    if (cell == nullptr || cell->state != CellState::Loaded) {
        return 0;
    }
    return cell->mapSerial;
}

bool WorldStreamer::takeChangedColumn(i32* chunkX, i32* chunkZ)
{
    return mapDirty_.pop(chunkX, chunkZ);
}

// ---------------------------------------------------------------------------
// Column work on the generation worker. See `offerColumnWork` in the header for
// what makes the borrow safe; these four are the mechanism and nothing more.
// ---------------------------------------------------------------------------

bool WorldStreamer::columnWorkAvailable() const
{
    std::lock_guard<std::mutex> guard(queueLock_);
    return workerRunning_ && !columnWorkPosted_ && !columnWorkBusy_ && !columnWorkReady_;
}

int WorldStreamer::offerColumnWork(i32* chunkX, i32* chunkZ, int count, ColumnWork work,
                                   void* ctx)
{
    if (work == nullptr || count <= 0) {
        return 0;
    }
    if (count > kColumnWorkMax) {
        count = kColumnWorkMax;
    }

    // **Resolved here, on the main thread**, because this is the one place the
    // grid is known to be settled: `update()` has run and nothing else will
    // move a cell until the next one. The worker never looks a coordinate up.
    const world::ChunkColumn* columns[kColumnWorkMax] = {};
    int taken = 0;
    for (int i = 0; i < count; ++i) {
        const Cell* cell = find(chunkX[i], chunkZ[i]);
        if (cell == nullptr || cell->state != CellState::Loaded || cell->column == nullptr) {
            continue;
        }
        columns[taken] = cell->column.get();
        // Compacted in place, so the caller's arrays and `index` in the
        // callback mean the same thing.
        chunkX[taken] = chunkX[i];
        chunkZ[taken] = chunkZ[i];
        ++taken;
    }
    if (taken == 0) {
        return 0;
    }

    {
        std::lock_guard<std::mutex> guard(queueLock_);
        if (!workerRunning_ || columnWorkPosted_ || columnWorkBusy_ || columnWorkReady_) {
            return 0;
        }
        for (int i = 0; i < taken; ++i) {
            columnWorkColumns_[i] = columns[i];
        }
        columnWork_ = work;
        columnWorkCtx_ = ctx;
        columnWorkCount_ = taken;
        columnWorkDone_ = 0;
        columnWorkPosted_ = true;
    }
    // The worker may be asleep with an empty slate, which is exactly the state
    // this exists for.
    wake_.notify_one();
    return taken;
}

bool WorldStreamer::takeColumnWork(int* done)
{
    std::lock_guard<std::mutex> guard(queueLock_);
    // **Only a reclaimed offer has an answer.** One still in flight says
    // nothing -- the worker may be halfway through it -- so this is false until
    // the next `update()` has withdrawn it, which is where the rule in the
    // header comes from rather than being a second one.
    if (!columnWorkReady_) {
        return false;
    }
    if (done != nullptr) {
        *done = columnWorkDone_;
    }
    columnWorkReady_ = false;
    columnWorkDone_ = 0;
    columnWorkCount_ = 0;
    columnWork_ = nullptr;
    columnWorkCtx_ = nullptr;
    return true;
}

void WorldStreamer::reclaimColumnWork()
{
    std::unique_lock<std::mutex> guard(queueLock_);
    if (!columnWorkPosted_ && !columnWorkBusy_) {
        return;
    }
    // **Withdrawn first, then waited on**, which is the same shape
    // waitForWorkerIdle uses and for the same reason: the worker checks this
    // between columns, so clearing it stops a batch that is running as well as
    // one that has not started. What is left to wait for is at most the one
    // column it is inside.
    columnWorkPosted_ = false;
    idle_.wait(guard, [this] { return !columnWorkBusy_; });
    columnWorkReady_ = true;
}

gui::ChunkState WorldStreamer::progressAt(i32 chunkX, i32 chunkZ,
                                          const std::pair<i32, i32>* job) const
{
    if (cells_.empty() || !centreSet_) {
        return gui::ChunkState::Unstarted;
    }
    if (std::abs(chunkX - centreX_) > gridRadius_ || std::abs(chunkZ - centreZ_) > gridRadius_) {
        return gui::ChunkState::Unstarted;
    }
    const Cell& cell = cells_[cellIndex(chunkX, chunkZ)];
    // The grid wraps modulo its own edge, so a cell reached by coordinate may
    // still be holding the column from the other side of the world. Same check
    // find() makes, and for the same reason.
    if (cell.chunkX != chunkX || cell.chunkZ != chunkZ) {
        return gui::ChunkState::Unstarted;
    }
    switch (cell.state) {
        case CellState::Empty:
            return gui::ChunkState::Unstarted;
        case CellState::Ungenerated:
            if (job != nullptr && job->first == chunkX && job->second == chunkZ) {
                return gui::ChunkState::Working;
            }
            return gui::ChunkState::Owed;
        case CellState::Awaited:
            return gui::ChunkState::Owed;
        case CellState::OnDisk:
            // The world already has it and it is on its way off the card.
            // Not a generation state, but it is the same thing to a player:
            // something is owed here and it is not here yet.
            return gui::ChunkState::Owed;
        case CellState::Loaded:
            // **Published is the honest end of the ramp.** A loaded column
            // with a missing neighbour has no geometry and is not on the
            // screen, so calling it done would fill the square a ring before
            // the world the player is looking at filled in.
            return cell.published ? gui::ChunkState::Done : gui::ChunkState::Ready;
        case CellState::Absent:
            // The edge of a finite world: nothing is owed here and nothing is
            // coming. See the note on ChunkState.
            return gui::ChunkState::Done;
    }
    return gui::ChunkState::Unstarted;
}

void WorldStreamer::progressGrid(i32 centreX, i32 centreZ, int radius, gui::ChunkState* out) const
{
    if (out == nullptr || radius < 0) {
        return;
    }
    const int edge = radius * 2 + 1;

    // One lock for the whole square. `inFlight_` is only meaningful while a job
    // is active -- it keeps its last value otherwise -- so both are read
    // together and the pair is passed down rather than the members.
    std::pair<i32, i32> job{0, 0};
    bool haveJob = false;
    {
        std::lock_guard<std::mutex> guard(queueLock_);
        haveJob = jobActive_;
        job = inFlight_;
    }

    for (int row = 0; row < edge; ++row) {
        const i32 chunkZ = centreZ - radius + row;
        gui::ChunkState* line = out + usize(row) * usize(edge);
        for (int column = 0; column < edge; ++column) {
            line[column] = progressAt(centreX - radius + column, chunkZ, haveJob ? &job : nullptr);
        }
    }
}

WorldStreamer::ProgressCount WorldStreamer::progressWithin(i32 centreX, i32 centreZ,
                                                           int radius) const
{
    ProgressCount count;
    if (radius < 0) {
        return count;
    }
    const int edge = radius * 2 + 1;
    count.total = edge * edge;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            // No job pointer: `Working` and `Owed` are both unfinished, so the
            // count does not need the lock the picture does.
            if (progressAt(centreX + dx, centreZ + dz, nullptr) == gui::ChunkState::Done) {
                ++count.done;
            }
        }
    }
    return count;
}

void WorldStreamer::setMemoryBudget(usize bytes)
{
    memoryBudget_ = bytes;
    // Starts wide open. The first over-budget pass is what narrows it, which
    // means a world that fits is never charged anything for this existing.
    memoryRadius_ = loadRadius_;
    // **Published here, not left to the next residency pass.** `admitRadius` is
    // a cached figure refreshed once every kResidencyStride frames, so a caller
    // that turned the budget off and then read the stat would be told the world
    // was still narrowed for another sixteen frames -- a debug page and the
    // thing it describes disagreeing, which is the one way this goes wrong
    // silently.
    stats_.admitRadius = admitRadius();
}

void WorldStreamer::enforceMemoryBudget(ChunkRenderer& renderer)
{
    // A multiplayer world's columns cost the same whether the grid holds them or
    // `remoteColumns_` does, so shrinking the grid would free nothing.
    if (memoryBudget_ == 0 || !centreSet_ || remote_) {
        return;
    }

    // Only on a pass that actually re-added the bytes; see blockBytesFresh_.
    if (!blockBytesFresh_) {
        return;
    }

    if (stats_.blockBytes > memoryBudget_) {
        // One ring per pass. The drop below is what frees the memory; shrinking
        // the radius is what stops it being read straight back in.
        if (memoryRadius_ > kMinAdmitRadius) {
            --memoryRadius_;
        }
    } else if (stats_.blockBytes < lowWater(memoryBudget_) && memoryRadius_ < loadRadius_) {
        ++memoryRadius_;
        stats_.admitRadius = admitRadius();
        return;  // nothing to drop when the radius just grew
    } else {
        return;
    }

    stats_.admitRadius = admitRadius();

    // Everything outside the new radius goes, and its bytes come off the total
    // so the page and the next pass both see what is actually resident rather
    // than a figure sixteen frames stale.
    const int keep = admitRadius();
    for (Cell& cell : cells_) {
        if (cell.state != CellState::Loaded) {
            continue;
        }
        if (std::abs(cell.chunkX - centreX_) <= keep && std::abs(cell.chunkZ - centreZ_) <= keep) {
            continue;
        }
        const usize freed = cell.column != nullptr ? cell.column->memoryUsage() : 0;
        dropCell(cell, renderer);
        stats_.blockBytes = stats_.blockBytes > freed ? stats_.blockBytes - freed : 0;
        ++stats_.evictedForMemory;
        --stats_.columnsResident;
    }
}

void WorldStreamer::countResidency()
{
    // **The counts every frame; the byte total occasionally.** The walk itself
    // is 729 integer comparisons and is not worth avoiding -- and the counts are
    // read by tests and by anything that wants to know whether the world around
    // the player is complete, so they must not lag. `memoryUsage()` is the
    // expensive part: it walks a column's eight sections *and* its preserved NBT
    // tags, once per resident column, and it feeds a number on the debug page
    // and nothing else. A byte total that updates twice a second is as useful as
    // one that updates thirty times a second.
    const bool withBytes = ++residencyStride_ >= kResidencyStride;
    blockBytesFresh_ = withBytes;
    if (withBytes) {
        residencyStride_ = 0;
        stats_.blockBytes = 0;
    }
    stats_.admitRadius = admitRadius();

    stats_.columnsResident = 0;
    stats_.columnsMissing = 0;
    for (const Cell& cell : cells_) {
        if (centreSet_
            && (std::abs(cell.chunkX - centreX_) > loadRadius_
                || std::abs(cell.chunkZ - centreZ_) > loadRadius_)) {
            continue;  // classification-only ring; never read in, never drawn
        }
        if (cell.state == CellState::Loaded) {
            ++stats_.columnsResident;
            if (withBytes) {
                stats_.blockBytes += cell.column->memoryUsage();
            }
        } else if (cell.state == CellState::Absent || cell.state == CellState::Ungenerated
                   || cell.state == CellState::Awaited) {
            // Both are "not in memory". Which one it is says whether anything
            // is going to change that, and pendingGeneration is the count that
            // separates them.
            ++stats_.columnsMissing;
        }
    }
}

}  // namespace mc::render
