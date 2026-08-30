#include "core/world/lighting.hpp"

#include <cstring>

namespace mc::world {

namespace {

constexpr int kLocal(int x, int y, int z)
{
    return (x << 11) | (z << 7) | y;
}

// Packs a light value into the chunk file's nibble layout: index i lives in
// the low half of byte i/2 when i is even and the high half when it is odd.
void writeNibble(u8* packed, int index, u8 value)
{
    u8& byte = packed[usize(index >> 1)];
    if ((index & 1) == 0) {
        byte = u8((byte & 0xF0) | value);
    } else {
        byte = u8((byte & 0x0F) | u8(value << 4));
    }
}

}  // namespace

void computeHeightMap(const u8* blocks, u8* heightMap)
{
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            const int base = (x << 11) | (z << 7);
            int y = kColumnHeight;
            // Walks down while the block *below* y is fully transparent, so it
            // stops on the first block that absorbs anything. Starting at 128
            // rather than 127 is what lets a column whose topmost block is
            // opaque report 128, meaning nothing in it sees the sky.
            while (y > 0 && block::def(blocks[usize(base + y - 1)]).opacity == 0) {
                --y;
            }
            heightMap[usize((z << 4) | x)] = u8(y);
        }
    }
}

LightEngine::LightEngine()
    : opacity_(usize(kWindowCells), 0), light_(usize(kWindowCells), 0)
{
    // The frontier is a surface, not a volume: for sky light it is roughly the
    // terrain surface plus cave mouths, for block light the reach of a handful
    // of lava pools. A few thousand per level is the realistic peak, and
    // reserving up front keeps a chunk's worth of generation allocation-free
    // once the engine has warmed up.
    for (std::vector<u32>& bucket : buckets_) {
        bucket.reserve(4096);
    }
}

void LightEngine::clearLight()
{
    std::memset(light_.data(), 0, usize(kWindowCells));
    for (std::vector<u32>& bucket : buckets_) {
        bucket.clear();
    }
}

void LightEngine::buildOpacity(const u8* const* columns)
{
    // One linear pass, writing the window in its own order rather than reading
    // it in the columns' order. Both are sequential; this way the random
    // access later in the search is against a single flat array instead of
    // nine, and the index arithmetic is done once per cell here rather than on
    // every neighbour probe.
    //
    // A 256-entry lookup is built alongside because `block::def` returns a
    // ~50-byte struct: touching it 295,000 times would stream the whole table
    // through a 32 KB L1 repeatedly, where a byte array of 256 stays resident.
    u8 opacityOf[256];
    u8 lightOf[256];
    bool clearOf[256];
    for (int id = 0; id < 256; ++id) {
        opacityOf[id] = clampedOpacity(u8(id));
        lightOf[id] = emittedLight(u8(id));
        // The height map's own test, and it is **not** the clamped opacity:
        // clamping turns a raw 0 into a 1, which would make air stop the
        // height map. It has to be read before the clamp, which is why this
        // third table exists rather than being derived from the first.
        clearOf[id] = block::def(u8(id)).opacity == 0;
    }

    emitters_.clear();

    for (int ix = 0; ix < kWindow; ++ix) {
        for (int iz = 0; iz < kWindow; ++iz) {
            const u8* column = columns[ix * kWindow + iz];
            for (int lx = 0; lx < 16; ++lx) {
                const int wx = ix * 16 + lx;
                for (int lz = 0; lz < 16; ++lz) {
                    const int wz = iz * 16 + lz;
                    const int base = index(wx, 0, wz);
                    u8* out = &opacity_[usize(base)];
                    if (column == nullptr) {
                        // Air, which is what the original sees when it lights
                        // against a chunk that is not loaded.
                        std::memset(out, opacityOf[0], usize(kColumnHeight));
                        heights_[wx * kWindowBlocks + wz] = 0;
                        continue;
                    }

                    // **One read of the block array per chunk, not three.**
                    // The opacity table, the height map and the list of light
                    // sources all come out of this loop. Each of them used to
                    // be its own pass over 294,912 cells, and on a console
                    // where the whole window is far larger than any cache, the
                    // passes were costing more than the search they feed.
                    const u8* in = &column[usize(kLocal(lx, 0, lz))];
                    for (int y = 0; y < kColumnHeight; ++y) {
                        const u8 id = in[y];
                        out[y] = opacityOf[id];
                        if (lightOf[id] != 0) {
                            emitters_.push_back(Emitter{u32(base + y), lightOf[id]});
                        }
                    }

                    int height = kColumnHeight;
                    while (height > 0 && clearOf[in[height - 1]]) {
                        --height;
                    }
                    heights_[wx * kWindowBlocks + wz] = u8(height);
                }
            }
        }
    }
}

void LightEngine::seedSky()
{
    // The height map for the whole window was built alongside the opacity
    // table; the seeding below needs a neighbour's height to know whether a
    // cell is on the boundary of the lit region.
    for (int wx = 0; wx < kWindowBlocks; ++wx) {
        for (int wz = 0; wz < kWindowBlocks; ++wz) {
            const int h = heights_[wx * kWindowBlocks + wz];

            // **This writes the whole column, dark half included, so the sky
            // pass needs no separate clear.** Zero below the height map and 15
            // at or above it is exactly what a memset plus a partial fill used
            // to produce, in one pass over the column instead of one and a
            // half.
            u8* out = &light_[usize(index(wx, 0, wz))];
            std::memset(out, 0, usize(h));
            std::memset(out + h, 15, usize(kColumnHeight - h));

            // **Only the boundary of that region goes in the queue.** A cell
            // whose six neighbours are all 15 as well can neither raise nor
            // lower anything, so enqueuing it is pure cost -- and in open
            // terrain that is the overwhelming majority of the lit volume,
            // tens of thousands of cells a chunk. What is left is the bottom
            // face of the lit region and the vertical walls where a
            // neighbouring column stands higher.
            //
            // Getting this wrong under-lights rather than crashing, which is
            // why the whole chunk is compared against the jar rather than
            // spot-checked.
            int tallest = h;
            if (wx > 0 && heights_[(wx - 1) * kWindowBlocks + wz] > tallest) {
                tallest = heights_[(wx - 1) * kWindowBlocks + wz];
            }
            if (wx + 1 < kWindowBlocks && heights_[(wx + 1) * kWindowBlocks + wz] > tallest) {
                tallest = heights_[(wx + 1) * kWindowBlocks + wz];
            }
            if (wz > 0 && heights_[wx * kWindowBlocks + (wz - 1)] > tallest) {
                tallest = heights_[wx * kWindowBlocks + (wz - 1)];
            }
            if (wz + 1 < kWindowBlocks && heights_[wx * kWindowBlocks + (wz + 1)] > tallest) {
                tallest = heights_[wx * kWindowBlocks + (wz + 1)];
            }

            for (int y = h; y < kColumnHeight && y < tallest + 1; ++y) {
                buckets_[15].push_back(u32(index(wx, y, wz)));
            }

            // **The world's floor and ceiling are sky sources too**, and this
            // is a real quirk rather than a safety clamp. `getSavedLightValue`
            // answers with the light type's *default* for any y outside
            // [0, 128), and Sky's default is 15 -- so the cell at y = 127 sees
            // 15 from above and the cell at y = 0 sees 15 from below, whether
            // or not either can see the actual sky.
            //
            // It shows up in a column capped by something that dims rather
            // than blocks: leaves at y = 127 have a height map of 128 and so
            // "cannot see the sky", yet come out at 14. Bedrock hides the
            // bottom case in every generated world, but the rule is the same.
            const int top = index(wx, kColumnHeight - 1, wz);
            const u8 fromAbove = u8(15 - opacity_[usize(top)]);
            if (fromAbove > light_[usize(top)]) {
                push(top, fromAbove);
            }
            const int bottom = index(wx, 0, wz);
            const u8 fromBelow = u8(15 - opacity_[usize(bottom)]);
            if (fromBelow > light_[usize(bottom)]) {
                push(bottom, fromBelow);
            }
        }
    }
}

void LightEngine::seedBlock()
{
    // The sources were found during buildOpacity, so this does not touch the
    // block array again. In a generated chunk there are a few thousand of them
    // at most -- lava, and nothing else until torches exist.
    for (const Emitter& source : emitters_) {
        push(int(source.cell), source.value);
    }
}

u32 LightEngine::propagate()
{
    u32 visits = 0;

    // Descending, so a cell is popped at its final value: nothing later can
    // raise a cell above the level currently being drained, because every step
    // costs at least one. That is what makes this single-pass where the
    // original re-scans.
    for (int level = 15; level >= 1; --level) {
        std::vector<u32>& bucket = buckets_[level];
        // Indexed rather than iterated: lower levels append to *their* buckets,
        // never to this one, so the size is stable -- but the vector can
        // reallocate under an iterator if that assumption ever breaks, and an
        // index survives it.
        for (usize i = 0; i < bucket.size(); ++i) {
            const int cell = int(bucket[i]);
            // A cell can be queued twice: seeded low, then raised by a
            // brighter neighbour before its first entry comes up. The entry
            // for the stale, lower level is skipped here rather than being
            // prevented at push time, which would cost a search.
            if (light_[usize(cell)] != u8(level)) {
                continue;
            }
            ++visits;

            const int y = cell % kColumnHeight;
            const int wz = (cell / kColumnHeight) % kWindowBlocks;
            const int wx = cell / (kColumnHeight * kWindowBlocks);

            const int steps[6] = {y > 0 ? -1 : 0,
                                  y + 1 < kColumnHeight ? 1 : 0,
                                  wz > 0 ? -kStepZ : 0,
                                  wz + 1 < kWindowBlocks ? kStepZ : 0,
                                  wx > 0 ? -kStepX : 0,
                                  wx + 1 < kWindowBlocks ? kStepX : 0};

            for (const int step : steps) {
                if (step == 0) {
                    continue;
                }
                const int next = cell + step;
                const int value = level - opacity_[usize(next)];
                if (value > int(light_[usize(next)])) {
                    push(next, u8(value));
                }
            }
        }
        bucket.clear();
    }

    return visits;
}

void LightEngine::computeCentre(const u8* const* columns, u8* skyOut, u8* blockOut,
                                u8* heightMapOut)
{
    buildOpacity(columns);

    // Sky first: it clears the light buffer as it seeds, where the block pass
    // needs it already zero.
    seedSky();
    skyVisits_ = propagate();

    // The centre column is (1, 1) of the window, so blocks 16..31 on both axes.
    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            for (int y = 0; y < kColumnHeight; ++y) {
                writeNibble(skyOut, kLocal(lx, y, lz),
                            light_[usize(index(16 + lx, y, 16 + lz))]);
            }
        }
    }

    clearLight();
    seedBlock();
    blockVisits_ = propagate();

    for (int lx = 0; lx < 16; ++lx) {
        for (int lz = 0; lz < 16; ++lz) {
            for (int y = 0; y < kColumnHeight; ++y) {
                writeNibble(blockOut, kLocal(lx, y, lz),
                            light_[usize(index(16 + lx, y, 16 + lz))]);
            }
        }
    }

    if (heightMapOut != nullptr) {
        for (int lx = 0; lx < 16; ++lx) {
            for (int lz = 0; lz < 16; ++lz) {
                heightMapOut[usize((lz << 4) | lx)] =
                    heights_[(16 + lx) * kWindowBlocks + (16 + lz)];
            }
        }
    }
}

}  // namespace mc::world
