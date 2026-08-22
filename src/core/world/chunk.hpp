#pragma once

// A chunk column: 16 x kWorldHeight x 16, stored as a stack of 16^3 sections.
//
// The column is the unit of storage and networking (Alpha writes one file and
// sends one Map Chunk packet per column); the section is the unit of meshing
// and memory. This type is the join between the two, and it is deliberately
// format-agnostic -- the Alpha chunk NBT layout lives in the storage slot, not
// here, so a later version's format is a new slot rather than an edit to this
// file.
//
// Column height comes from mcver::kWorldHeight. Nothing here assumes 128.

#include "core/nbt/preserved.hpp"
#include "core/util/types.hpp"
#include "core/world/section.hpp"
#include "version_config.hpp"

#include <cassert>

namespace mc::world {

class ChunkColumn {
public:
    static constexpr int kWidth = Section::kSize;
    static constexpr int kHeight = mcver::kWorldHeight;
    static constexpr int kSectionCount = kHeight / Section::kSize;
    static constexpr int kArea = kWidth * kWidth;

    static_assert(kHeight % Section::kSize == 0,
                  "world height must be a whole number of sections");

    ChunkColumn() = default;
    ChunkColumn(i32 chunkX, i32 chunkZ) : x(chunkX), z(chunkZ) {}

    ChunkColumn(const ChunkColumn&) = delete;
    ChunkColumn& operator=(const ChunkColumn&) = delete;
    ChunkColumn(ChunkColumn&&) = default;
    ChunkColumn& operator=(ChunkColumn&&) = default;

    i32 x = 0;
    i32 z = 0;
    bool terrainPopulated = false;
    i64 lastUpdate = 0;

    // One byte per XZ column, in ZX order: heightMap[z * 16 + x]. Derived data
    // that the original game recomputes; we round-trip it until the lighting
    // engine can regenerate it.
    u8 heightMap[kArea] = {};

    // Tags carried through unchanged. `preserved` holds the ones inside Level
    // -- which for now includes Entities and TileEntities -- and `preservedRoot`
    // the rare sibling of Level itself. See core/nbt/preserved.hpp.
    nbt::PreservedTags preserved;
    nbt::PreservedTags preservedRoot;

    Section& section(int sy)
    {
        assert(sy >= 0 && sy < kSectionCount);
        return sections_[sy];
    }
    const Section& section(int sy) const
    {
        assert(sy >= 0 && sy < kSectionCount);
        return sections_[sy];
    }

    // x and z are 0..15 within the column; y is 0..kHeight-1. Out-of-range y is
    // air rather than an error: the mesher and the physics both probe above and
    // below the world, and making every caller pre-check would be noise.
    BlockId block(int lx, int y, int lz) const
    {
        if (y < 0 || y >= kHeight) {
            return kAirBlock;
        }
        return sections_[y / Section::kSize].block(lx, y % Section::kSize, lz);
    }

    void setBlock(int lx, int y, int lz, BlockId id)
    {
        if (y < 0 || y >= kHeight) {
            return;
        }
        sections_[y / Section::kSize].setBlock(lx, y % Section::kSize, lz, id);
    }

    u8 blockData(int lx, int y, int lz) const { return nibble(&Section::data, lx, y, lz); }
    u8 blockLight(int lx, int y, int lz) const { return nibble(&Section::blockLight, lx, y, lz); }
    u8 skyLight(int lx, int y, int lz) const { return nibble(&Section::skyLight, lx, y, lz); }

    void setBlockData(int lx, int y, int lz, u8 v) { setNibble(&Section::data, lx, y, lz, v); }
    void setBlockLight(int lx, int y, int lz, u8 v) { setNibble(&Section::blockLight, lx, y, lz, v); }
    void setSkyLight(int lx, int y, int lz, u8 v) { setNibble(&Section::skyLight, lx, y, lz, v); }

    // Re-derives every section's palette. Worth doing after a bulk edit or
    // before a save; never on the block-placement path.
    void compact();

    usize memoryUsage() const;

private:
    using NibbleAccessor = const NibbleArray& (Section::*)() const;
    using NibbleMutator = NibbleArray& (Section::*)();

    u8 nibble(NibbleAccessor plane, int lx, int y, int lz) const
    {
        if (y < 0 || y >= kHeight) {
            return 0;
        }
        const Section& s = sections_[y / Section::kSize];
        return (s.*plane)().get(Section::index(lx, y % Section::kSize, lz));
    }

    void setNibble(NibbleMutator plane, int lx, int y, int lz, u8 value)
    {
        if (y < 0 || y >= kHeight) {
            return;
        }
        Section& s = sections_[y / Section::kSize];
        (s.*plane)().set(Section::index(lx, y % Section::kSize, lz), value);
    }

    Section sections_[kSectionCount];
};

}  // namespace mc::world
