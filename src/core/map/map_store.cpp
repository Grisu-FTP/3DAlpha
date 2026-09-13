#include "core/map/map_store.hpp"

#include "core/util/math.hpp"

#include <cstring>

namespace mc::map {

namespace {

// Two chunk coordinates into one hash. The multiply-xor is the same shape the
// rest of the tree uses for coordinate keys; what matters here is only that
// neighbouring chunks do not land in neighbouring slots, because the access
// pattern is a solid rectangle of them.
u32 hashChunk(i32 x, i32 z)
{
    u32 h = u32(x) * 0x9E3779B1u;
    h ^= u32(z) * 0x85EBCA77u;
    h ^= h >> 15;
    return h;
}

}  // namespace

// `store` compares two samples with `memcmp`, which is only an answer about
// their contents if there is nothing between or after the three arrays.
static_assert(sizeof(MapChunkSample)
                  == sizeof(block::BlockId) * kChunkSamples + 2 * kChunkSamples,
              "MapChunkSample must be its three arrays and no padding");

i32 tileOfBlock(i32 blockX) { return floorDiv(blockX + kTileBlocks / 2, kTileBlocks); }

i32 tileOriginBlock(i32 tile) { return tile * kTileBlocks - kTileBlocks / 2; }

bool isTileEdgeBlock(i32 blockX) { return blockX == tileOriginBlock(tileOfBlock(blockX)); }

void MapStore::setCapacity(int chunks)
{
    entries_.clear();
    entries_.shrink_to_fit();
    if (chunks < 1) {
        chunks = 1;
    }
    entries_.reserve(usize(chunks));
    capacity_ = chunks;

    usize slots = 1;
    while (slots < usize(chunks) * 2) {
        slots <<= 1;
    }
    index_.assign(slots, -1);

    tick_ = 0;
    stats_ = Stats{};
}

int MapStore::slotOf(i32 chunkX, i32 chunkZ) const
{
    if (index_.empty()) {
        return -1;
    }
    const usize mask = index_.size() - 1;
    usize slot = usize(hashChunk(chunkX, chunkZ)) & mask;
    for (usize probe = 0; probe <= mask; ++probe) {
        const i32 at = index_[slot];
        if (at < 0) {
            return -1;
        }
        const Entry& entry = entries_[usize(at)];
        if (entry.x == chunkX && entry.z == chunkZ) {
            return int(slot);
        }
        slot = (slot + 1) & mask;
    }
    return -1;
}

const MapStore::Entry* MapStore::entryAt(i32 chunkX, i32 chunkZ) const
{
    const int slot = slotOf(chunkX, chunkZ);
    return slot < 0 ? nullptr : &entries_[usize(index_[usize(slot)])];
}

const MapChunkSample* MapStore::find(i32 chunkX, i32 chunkZ) const
{
    const Entry* entry = entryAt(chunkX, chunkZ);
    return entry == nullptr ? nullptr : &entry->sample;
}

const MapPixel* MapStore::patch(i32 chunkX, i32 chunkZ) const
{
    const Entry* entry = entryAt(chunkX, chunkZ);
    return entry == nullptr ? nullptr : entry->patch.pixels;
}

bool MapStore::patchStale(i32 chunkX, i32 chunkZ, u32 stamp) const
{
    const Entry* entry = entryAt(chunkX, chunkZ);
    return entry != nullptr && entry->stamp != stamp;
}

MapPixel* MapStore::claimPatch(i32 chunkX, i32 chunkZ, u32 stamp)
{
    const int slot = slotOf(chunkX, chunkZ);
    if (slot < 0) {
        return nullptr;
    }
    Entry& entry = entries_[usize(index_[usize(slot)])];
    entry.stamp = stamp;
    ++stats_.drawn;
    return entry.patch.pixels;
}

void MapStore::touch(i32 chunkX, i32 chunkZ)
{
    const int slot = slotOf(chunkX, chunkZ);
    if (slot >= 0) {
        entries_[usize(index_[usize(slot)])].used = ++tick_;
    }
}

void MapStore::rebuildIndex()
{
    std::memset(index_.data(), 0xFF, index_.size() * sizeof(i32));
    const usize mask = index_.size() - 1;
    for (usize i = 0; i < entries_.size(); ++i) {
        usize slot = usize(hashChunk(entries_[i].x, entries_[i].z)) & mask;
        while (index_[slot] >= 0) {
            slot = (slot + 1) & mask;
        }
        index_[slot] = i32(i);
    }
}

u32 MapStore::sampleSerial(i32 chunkX, i32 chunkZ) const
{
    const Entry* entry = entryAt(chunkX, chunkZ);
    return entry == nullptr ? 0 : entry->serial;
}

bool MapStore::store(i32 chunkX, i32 chunkZ, const MapChunkSample& sample, u32 serial)
{
    if (index_.empty()) {
        return false;
    }

    // Already here: overwrite in place. A chunk the player has changed is
    // sampled again, and the second sample is the true one.
    //
    // **Unless it is the same one**, which is the common answer for a chunk
    // that was re-sampled because a block was written into it: most blocks are
    // under the surface and the map draws the surface. Then all that has
    // happened is that this chunk is now known to be current as of `serial`,
    // and nothing about the picture -- this patch, the one to the south, or the
    // count the screen watches -- has any reason to move.
    const int slot = slotOf(chunkX, chunkZ);
    if (slot >= 0) {
        Entry& entry = entries_[usize(index_[usize(slot)])];
        entry.used = ++tick_;
        entry.serial = serial;
        if (std::memcmp(&entry.sample, &sample, sizeof(MapChunkSample)) == 0) {
            return false;
        }
        ++stats_.stored;
        invalidateSouthOf(chunkX, chunkZ);
        entry.sample = sample;
        entry.stamp = 0;
        return true;
    }

    ++stats_.stored;

    // **The chunk to the south is stale now too.** Its first row of pixels
    // shades against this one's last row of heights, so a chunk arriving
    // changes the picture of its neighbour as well as its own. Done before the
    // insert below, because that one can evict and move entries about.
    invalidateSouthOf(chunkX, chunkZ);

    if (entries_.size() < usize(capacity_)) {
        entries_.emplace_back();
        Entry& entry = entries_.back();
        entry.x = chunkX;
        entry.z = chunkZ;
        entry.used = ++tick_;
        entry.stamp = 0;
        entry.serial = serial;
        entry.sample = sample;

        const usize mask = index_.size() - 1;
        usize free = usize(hashChunk(chunkX, chunkZ)) & mask;
        while (index_[free] >= 0) {
            free = (free + 1) & mask;
        }
        index_[free] = i32(entries_.size() - 1);

        stats_.chunks = int(entries_.size());
        stats_.bytes = entries_.size() * sizeof(MapChunkSample);
        return true;
    }

    // Full: the oldest use goes. Found by a scan rather than by a linked list,
    // because the list would be maintained on every touch -- once per visible
    // chunk per redraw -- to save a scan that happens once per sampled chunk.
    usize oldest = 0;
    for (usize i = 1; i < entries_.size(); ++i) {
        if (entries_[i].used < entries_[oldest].used) {
            oldest = i;
        }
    }

    entries_[oldest].x = chunkX;
    entries_[oldest].z = chunkZ;
    entries_[oldest].used = ++tick_;
    entries_[oldest].stamp = 0;
    entries_[oldest].serial = serial;
    entries_[oldest].sample = sample;
    ++stats_.evicted;
    rebuildIndex();
    return true;
}

void MapStore::invalidateSouthOf(i32 chunkX, i32 chunkZ)
{
    const int slot = slotOf(chunkX, chunkZ + 1);
    if (slot >= 0) {
        entries_[usize(index_[usize(slot)])].stamp = 0;
    }
}

void MapStore::clear()
{
    entries_.clear();
    if (!index_.empty()) {
        std::memset(index_.data(), 0xFF, index_.size() * sizeof(i32));
    }
    tick_ = 0;
    const Stats keep = stats_;
    stats_ = Stats{};
    stats_.stored = keep.stored;
    stats_.evicted = keep.evicted;
}

}  // namespace mc::map
