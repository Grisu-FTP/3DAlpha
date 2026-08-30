#include "core/world/light_update.hpp"

#include "core/world/lighting.hpp"

namespace mc::world {
namespace {

// The six faces, as offsets. Order does not matter here the way it does for
// neighbour notification -- light is a fixed point and reaching it in a
// different order reaches the same numbers.
constexpr int kDx[6] = {-1, 1, 0, 0, 0, 0};
constexpr int kDy[6] = {0, 0, -1, 1, 0, 0};
constexpr int kDz[6] = {0, 0, 0, 0, -1, 1};

int localOf(i32 v) { return int(v & 15); }

}  // namespace

LightUpdater::LightUpdater(LightAccess access) : access_(access)
{
    for (int k = 0; k < 2; ++k) {
        remove_[k].reserve();
        add_[k].reserve();
    }
}

ChunkColumn* LightUpdater::columnAtBlock(i32 x, i32 z)
{
    const i32 chunkX = x >> 4;
    const i32 chunkZ = z >> 4;
    if (cachedValid_ && cachedX_ == chunkX && cachedZ_ == chunkZ) return cached_;
    if (access_.column == nullptr) return nullptr;
    cached_ = access_.column(access_.ctx, chunkX, chunkZ);
    cachedX_ = chunkX;
    cachedZ_ = chunkZ;
    cachedValid_ = true;
    return cached_;
}

u8 LightUpdater::lightAt(Kind kind, i32 x, int y, i32 z)
{
    // **Outside the world vertically, sky reads 15 and block reads 0**, and
    // that is `getSavedLightValue` answering with the light type's default
    // rather than a clamp of ours. LightEngine transcribes the same quirk when
    // it seeds the top and bottom of every column, so the two agree at the
    // floor and the ceiling as well as in between.
    if (y < 0 || y >= kHeight) return kind == Kind::Sky ? u8(15) : u8(0);
    ChunkColumn* column = columnAtBlock(x, z);
    if (column == nullptr) return 0;
    const int lx = localOf(x);
    const int lz = localOf(z);
    return kind == Kind::Sky ? column->skyLight(lx, y, lz) : column->blockLight(lx, y, lz);
}

void LightUpdater::setLightAt(Kind kind, i32 x, int y, i32 z, u8 value)
{
    if (y < 0 || y >= kHeight) return;
    ChunkColumn* column = columnAtBlock(x, z);
    if (column == nullptr) return;
    const int lx = localOf(x);
    const int lz = localOf(z);
    const u8 before = kind == Kind::Sky ? column->skyLight(lx, y, lz)
                                        : column->blockLight(lx, y, lz);
    if (before == value) return;

    if (kind == Kind::Sky) {
        column->setSkyLight(lx, y, lz, value);
    } else {
        column->setBlockLight(lx, y, lz, value);
    }
    ++stats_.cellsSettled;
    noteSectionLit(x, y, z);
}

u8 LightUpdater::sourceAt(Kind kind, i32 x, int y, i32 z)
{
    if (y < 0 || y >= kHeight) return 0;
    ChunkColumn* column = columnAtBlock(x, z);
    if (column == nullptr) return 0;
    const int lx = localOf(x);
    const int lz = localOf(z);

    if (kind == Kind::Block) {
        return emittedLight(column->block(lx, y, lz));
    }

    // Sky: everything at or above the height map sees the sky and is 15. The
    // height map is the one piece of derived state a tick already maintains --
    // `TickWorld::refreshHeight` moves it on every write -- so this needs no
    // separate notion of what changed.
    if (y >= int(column->heightMap[usize(lz * 16 + lx)])) return 15;

    // The floor and the ceiling are sky sources too, for the same reason
    // lightAt answers 15 outside the world: the cell at y = 127 sees the
    // default from above and the cell at y = 0 sees it from below, whether or
    // not either can see the actual sky. Transcribed from LightEngine::seedSky.
    if (y == kHeight - 1 || y == 0) {
        const u8 opacity = clampedOpacity(column->block(lx, y, lz));
        return opacity >= 15 ? u8(0) : u8(15 - opacity);
    }
    return 0;
}

u8 LightUpdater::opacityAt(i32 x, int y, i32 z)
{
    if (y < 0 || y >= kHeight) return 15;
    ChunkColumn* column = columnAtBlock(x, z);
    if (column == nullptr) return 15;
    return clampedOpacity(column->block(localOf(x), y, localOf(z)));
}

void LightUpdater::push(Queue* queue, i32 x, int y, i32 z, u8 level)
{
    if (y < 0 || y >= kHeight) return;
    if (!queue->push(Cell{x, z, i16(y), level})) {
        ++stats_.dropped;
    }
}

void LightUpdater::noteSectionLit(i32 x, int y, i32 z)
{
    const i32 chunkX = x >> 4;
    const i32 chunkZ = z >> 4;
    const i16 sectionY = i16(y / Section::kSize);

    for (usize i = 0; i < litSectionCount_; ++i) {
        const LitSection& s = litSections_[i];
        if (s.chunkX == chunkX && s.chunkZ == chunkZ && s.sectionY == sectionY) return;
    }
    if (litSectionCount_ == kLitSectionCapacity) {
        // Full: report what is held and start again. Reporting twice is a
        // wasted remesh, which is far cheaper than not reporting at all.
        flushLitSections();
    }
    litSections_[litSectionCount_++] = LitSection{chunkX, chunkZ, sectionY};
}

void LightUpdater::flushLitSections()
{
    if (access_.sectionLit != nullptr) {
        for (usize i = 0; i < litSectionCount_; ++i) {
            const LitSection& s = litSections_[i];
            access_.sectionLit(access_.ctx, s.chunkX, int(s.sectionY), s.chunkZ);
        }
    }
    litSectionCount_ = 0;
}

void LightUpdater::blockChanged(i32 x, int y, i32 z)
{
    if (y < 0 || y >= kHeight) return;
    ++stats_.edits;

    // The grid may have moved since the last call; nothing here holds a column
    // pointer across a frame, but the cache does across a call.
    invalidateColumnCache();
    if (columnAtBlock(x, z) == nullptr) return;

    for (int k = 0; k < 2; ++k) {
        const Kind kind = k == 0 ? Kind::Sky : Kind::Block;

        const u8 held = lightAt(kind, x, y, z);
        // What the cell holds up by itself: the block's own emission, or 15
        // where the height map says it sees the sky.
        const u8 source = sourceAt(kind, x, y, z);

        if (source >= held) {
            // **Nothing to take back**, and checking this first is not an
            // optimisation so much as the difference between working and not.
            // Placing any block in open air leaves it a sky source at 15, which
            // is what it already held -- and seeding a removal at 15 anyway
            // would zero the entire sky-lit region around it and then refill it
            // from the same value. The commonest edit in the game would have
            // been the most expensive one.
            if (source > held) {
                setLightAt(kind, x, y, z, source);
            }
            // Still worth propagating: the block's opacity may have changed
            // even where its own light did not, so what passes *through* here
            // is different.
            if (source > 0) {
                push(&add_[k], x, y, z, source);
            }
        } else {
            // **Take the cell's light away and let it be rebuilt.** Seeding the
            // removal with what it held is what makes the addition afterwards
            // correct: the removal walk finds exactly the cells that were lit
            // *through* here, and every cell it meets that is bright enough to
            // have been lit from somewhere else becomes an addition source.
            push(&remove_[k], x, y, z, held);
            setLightAt(kind, x, y, z, 0);

            // The block that replaced it may still be a source in its own right,
            // just a dimmer one.
            if (source > 0) {
                setLightAt(kind, x, y, z, source);
                push(&add_[k], x, y, z, source);
            }
        }

        // And its neighbours may light it: the block that was here may have
        // been opaque and may not be now.
        for (int d = 0; d < 6; ++d) {
            const i32 nx = x + kDx[d];
            const int ny = y + kDy[d];
            const i32 nz = z + kDz[d];
            const u8 neighbour = lightAt(kind, nx, ny, nz);
            if (neighbour > 0) {
                push(&add_[k], nx, ny, nz, neighbour);
            }
        }
    }

    // **Sky exposure is a column, not a cell.** Placing a block under open sky
    // darkens everything below it and removing one lets the sky all the way
    // down, and either can move the height map by many cells at once. Comparing
    // the stored value against what the height map now says, over the whole
    // column, catches every one of those in 128 cheap reads and needs no record
    // of what the height map used to be.
    ChunkColumn* column = columnAtBlock(x, z);
    if (column == nullptr) return;
    const int lx = localOf(x);
    const int lz = localOf(z);
    const int height = int(column->heightMap[usize(lz * 16 + lx)]);

    for (int cy = 0; cy < kHeight; ++cy) {
        const u8 stored = column->skyLight(lx, cy, lz);
        if (cy >= height) {
            // Sees the sky and is not 15: newly exposed.
            if (stored != 15) {
                column->setSkyLight(lx, cy, lz, 15);
                ++stats_.cellsSettled;
                noteSectionLit(x, cy, z);
                push(&add_[int(Kind::Sky)], x, cy, z, 15);
            }
        } else if (stored == 15) {
            // Was exposed and is not any more. Whatever it was feeding has to
            // be taken back before it can be refilled from a real source.
            column->setSkyLight(lx, cy, lz, 0);
            ++stats_.cellsSettled;
            noteSectionLit(x, cy, z);
            push(&remove_[int(Kind::Sky)], x, cy, z, 15);
        }
    }
}

bool LightUpdater::stepRemoval(Kind kind)
{
    Queue& queue = remove_[int(kind)];
    if (queue.empty()) return false;

    const Cell cell = queue.pop();
    ++stats_.cellsVisited;

    for (int d = 0; d < 6; ++d) {
        const i32 nx = cell.x + kDx[d];
        const int ny = int(cell.y) + kDy[d];
        const i32 nz = cell.z + kDz[d];
        if (ny < 0 || ny >= kHeight) continue;

        const u8 neighbour = lightAt(kind, nx, ny, nz);
        if (neighbour == 0) continue;

        if (neighbour < cell.level) {
            // Dimmer than what was here, so it was lit through this cell -- but
            // only if it is not holding that value of its own accord. A lava
            // block beside a lava block is not lit by its neighbour.
            const u8 source = sourceAt(kind, nx, ny, nz);
            if (source >= neighbour) {
                push(&add_[int(kind)], nx, ny, nz, neighbour);
                continue;
            }
            setLightAt(kind, nx, ny, nz, 0);
            push(&remove_[int(kind)], nx, ny, nz, neighbour);
        } else {
            // As bright or brighter, so it has another supply and will refill
            // whatever the removal empties.
            push(&add_[int(kind)], nx, ny, nz, neighbour);
        }
    }
    return true;
}

bool LightUpdater::stepAddition(Kind kind)
{
    Queue& queue = add_[int(kind)];
    if (queue.empty()) return false;

    const Cell cell = queue.pop();
    ++stats_.cellsVisited;

    // The queued level is what the cell held when it was queued; it may have
    // been raised or emptied since, so the current value is what propagates.
    const u8 level = lightAt(kind, cell.x, int(cell.y), cell.z);
    if (level <= 1) return true;

    for (int d = 0; d < 6; ++d) {
        const i32 nx = cell.x + kDx[d];
        const int ny = int(cell.y) + kDy[d];
        const i32 nz = cell.z + kDz[d];
        if (ny < 0 || ny >= kHeight) continue;

        const int value = int(level) - int(opacityAt(nx, ny, nz));
        if (value <= 0) continue;
        if (value <= int(lightAt(kind, nx, ny, nz))) continue;

        setLightAt(kind, nx, ny, nz, u8(value));
        push(&add_[int(kind)], nx, ny, nz, u8(value));
    }
    return true;
}

u32 LightUpdater::drain(u32 budget)
{
    if (budget == 0) return 0;

    // The grid can have moved between frames, so no column pointer survives
    // into a drain.
    invalidateColumnCache();

    u32 done = 0;
    while (done < budget) {
        // **Removals before additions, and sky before block, but only within a
        // type.** A cell zeroed by a removal must not be refilled from a
        // neighbour that is itself about to be zeroed, so a type's removal
        // queue has to empty before its addition queue is touched. The two
        // types are independent and their order between each other does not
        // matter.
        bool worked = false;
        for (int k = 0; k < 2 && done < budget; ++k) {
            const Kind kind = k == 0 ? Kind::Sky : Kind::Block;
            if (!remove_[k].empty()) {
                while (done < budget && stepRemoval(kind)) {
                    ++done;
                }
                worked = true;
                continue;
            }
            if (!add_[k].empty()) {
                while (done < budget && stepAddition(kind)) {
                    ++done;
                }
                worked = true;
            }
        }
        if (!worked) break;
    }

    flushLitSections();
    return done;
}

bool LightUpdater::idle() const
{
    for (int k = 0; k < 2; ++k) {
        if (!remove_[k].empty() || !add_[k].empty()) return false;
    }
    return true;
}

usize LightUpdater::pending() const
{
    usize total = 0;
    for (int k = 0; k < 2; ++k) {
        total += remove_[k].size() + add_[k].size();
    }
    return total;
}

void LightUpdater::reset()
{
    for (int k = 0; k < 2; ++k) {
        remove_[k].clear();
        add_[k].clear();
    }
    litSectionCount_ = 0;
    invalidateColumnCache();
}

}  // namespace mc::world
