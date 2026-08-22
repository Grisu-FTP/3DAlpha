#include "impl/worldgen/alpha_nobiome/population_view.hpp"

#include "core/block/registry.hpp"

namespace mc::worldgen {

namespace {

// Java's >> 4 on a negative int is an arithmetic shift, which is floor
// division -- not the truncation `/ 16` would give. Block -1 belongs to chunk
// -1, not chunk 0, and getting that wrong misplaces every vein west of the
// origin.
constexpr i32 chunkOf(i32 block)
{
    return block >> 4;
}

// Likewise: & 15 is a positive remainder for negatives, where % 16 is not.
constexpr int withinChunk(i32 block)
{
    return int(block & 15);
}

}  // namespace

void PopulationView::reset(i32 originChunkX, i32 originChunkZ, int countX, int countZ,
                           u8* const* columns)
{
    originChunkX_ = originChunkX;
    originChunkZ_ = originChunkZ;
    countX_ = countX < kMaxColumns ? countX : kMaxColumns;
    countZ_ = countZ < kMaxColumns ? countZ : kMaxColumns;
    refusedOutOfWindow_ = 0;
    writtenColumns_ = 0;

    for (int ix = 0; ix < countX_; ++ix) {
        for (int iz = 0; iz < countZ_; ++iz) {
            columns_[ix * kMaxColumns + iz] = columns[ix * countZ + iz];
        }
    }

    // `Chunk.generateSkylightMap` runs in the Chunk constructor, so every
    // column in the window already has a height map by the time population
    // starts. Building it here is that, and it is the only thing the light
    // model needs -- the fill below the height map is derived rather than
    // stored.
    for (int ix = 0; ix < countX_; ++ix) {
        for (int iz = 0; iz < countZ_; ++iz) {
            const u8* column = columns_[usize(ix * kMaxColumns + iz)];
            u8* heights = &heights_[usize((ix * kMaxColumns + iz) * 256)];
            for (int lx = 0; lx < 16; ++lx) {
                for (int lz = 0; lz < 16; ++lz) {
                    if (column == nullptr) {
                        heights[usize((lz << 4) | lx)] = 0;
                        continue;
                    }
                    const int base = localIndex(lx, 0, lz);
                    int y = 128;
                    while (y > 0 && block::def(column[usize(base + y - 1)]).opacity == 0) {
                        --y;
                    }
                    heights[usize((lz << 4) | lx)] = u8(y);
                }
            }
        }
    }
}

int PopulationView::heightIndex(i32 x, i32 z) const
{
    const i32 ix = chunkOf(x) - originChunkX_;
    const i32 iz = chunkOf(z) - originChunkZ_;
    if (ix < 0 || iz < 0 || ix >= countX_ || iz >= countZ_) {
        return -1;
    }
    return int((ix * kMaxColumns + iz) * 256) + ((withinChunk(z) << 4) | withinChunk(x));
}

int PopulationView::heightAt(i32 x, i32 z) const
{
    const int index = heightIndex(x, z);
    // `World.getHeightValue` returns 0 for a chunk that is not loaded, and it
    // does so *before* asking the chunk, so this is the original's answer and
    // not a fallback of ours.
    return index < 0 ? 0 : int(heights_[usize(index)]);
}

void PopulationView::relightColumn(i32 x, i32 z)
{
    const int index = heightIndex(x, z);
    if (index < 0) {
        return;
    }
    const u8* column = columnFor(x, z);
    if (column == nullptr) {
        return;
    }
    const int base = localIndex(withinChunk(x), 0, withinChunk(z));
    int y = 128;
    while (y > 0 && block::def(column[usize(base + y - 1)]).opacity == 0) {
        --y;
    }
    heights_[usize(index)] = u8(y);
}

int PopulationView::lightAt(i32 x, i32 y, i32 z) const
{
    // Outside the world horizontally the original answers 15 without looking
    // at anything, and below the floor it answers 0.
    if (x < -32000000 || z < -32000000 || x >= 32000000 || z >= 32000000) {
        return 15;
    }
    if (y < 0) {
        return 0;
    }
    if (y >= 128) {
        return 15;
    }

    const int height = heightAt(x, z);
    if (int(y) >= height) {
        return 15;
    }

    // The decay `Chunk.relightBlock` writes downward from the height map:
    // start at 15 and pay each block's opacity, clamped up to 1 so that even
    // clear air costs a level. Bounded at fifteen steps, because every step
    // costs at least one and the loop stops at zero -- which is also exactly
    // where the original stops writing.
    //
    // **Derived from the blocks rather than stored, and that is the one
    // simplification in this file.** The original keeps the fill in an array
    // and `relightBlock` returns early when the height has not moved, so a
    // block placed *below* the height map leaves the stored value behind,
    // describing a column that no longer exists. Recomputing here always sees
    // the current blocks, so we would disagree with it in that case.
    //
    // Storing it faithfully would cost the whole sky-light array for every
    // column of the window, and the case needs a light level above zero
    // somewhere under the height map -- so a canopy or water -- with a later
    // placement changing the opacity beneath it without moving the height.
    // Whether that is reachable at all is a question for the oracle rather
    // than for this comment: tests/flower_test.cpp compares against a real
    // World, and if the two ever part company this is the first place to
    // look.
    int light = 15;
    for (int step = height - 1; step >= int(y); --step) {
        const u16 opacity = block::def(blockAt(x, i32(step), z)).opacity;
        light -= opacity < 1 ? 1 : int(opacity);
        if (light <= 0) {
            return 0;
        }
    }
    return light;
}

u8* PopulationView::columnFor(i32 x, i32 z) const
{
    const i32 ix = chunkOf(x) - originChunkX_;
    const i32 iz = chunkOf(z) - originChunkZ_;
    if (ix < 0 || iz < 0 || ix >= countX_ || iz >= countZ_) {
        return nullptr;
    }
    return columns_[usize(ix * kMaxColumns + iz)];
}

u8 PopulationView::blockAt(i32 x, i32 y, i32 z) const
{
    if (y < 0 || y >= 128) {
        return 0;
    }
    const u8* column = columnFor(x, z);
    if (column == nullptr) {
        return 0;
    }
    return column[usize(localIndex(withinChunk(x), int(y), withinChunk(z)))];
}

bool PopulationView::setBlock(i32 x, i32 y, i32 z, u8 id)
{
    // World.setBlock's own bounds, in its own order: the horizontal limit
    // first, then y. Both return false rather than throwing.
    if (x < -32000000 || z < -32000000 || x >= 32000000 || z > 32000000) {
        return false;
    }
    if (y < 0 || y >= 128) {
        return false;
    }

    u8* column = columnFor(x, z);
    if (column == nullptr) {
        // The original would have generated the missing chunk here. We cannot,
        // so this is our limitation rather than the game's -- counted, so a
        // test can prove it never happens for the generators we ship.
        ++refusedOutOfWindow_;
        return false;
    }

    // The height map has to be read *before* the write, exactly as
    // `Chunk.setBlock` does, because the rule below compares against the old
    // value.
    const int oldHeight = heightAt(x, z);
    column[usize(localIndex(withinChunk(x), int(y), withinChunk(z)))] = id;

    writtenColumns_ |= 1u << u32((chunkOf(x) - originChunkX_) * kMaxColumns +
                                 (chunkOf(z) - originChunkZ_));

    // **The original does not relight on every placement**, and the condition
    // is copied rather than simplified to "always":
    //
    //   * something that absorbs light, at or above the old height -> relight
    //   * something that does not, exactly at the old height minus one, which
    //     is the block that *was* defining the height -> relight
    //   * anything else -> nothing at all
    //
    // Only the *height map* is maintained here, which is what this condition
    // governs. See the note on lightAt about the one place that is a
    // simplification of the original rather than a copy of it.
    const u16 opacity = block::def(id).opacity;
    if (opacity != 0) {
        if (int(y) >= oldHeight) {
            relightColumn(x, z);
        }
    } else if (int(y) == oldHeight - 1) {
        relightColumn(x, z);
    }
    return true;
}

}  // namespace mc::worldgen
