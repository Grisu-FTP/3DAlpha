#pragma once

// The read window a section is meshed through.
//
// Meshing a 16^3 section needs the shell one block outside it on all sides:
// every face is culled against its neighbour, and that neighbour may live in
// the section above, below, or in one of the eight surrounding columns.
//
// Reading those through ChunkColumn on demand costs a bounds check, a section
// index and a palette indirection per block, and the mesher asks 6 times per
// block -- 24576 lookups per section. Copying the 18^3 window out first turns
// that into 5832 lookups followed by flat array reads. The buffer is 23 KB,
// which is why it belongs to the worker thread and is reused across sections
// rather than living on the stack.
//
// Index order is Y-fastest, matching Section and the Alpha column arrays, so
// the mesher's inner loop walks memory forwards.

#include "core/util/types.hpp"
#include "core/world/chunk.hpp"

namespace mc::mesh {

// The 3x3 block of columns centred on the one being meshed.
//
// A null entry means "not loaded", and is read as air. That is the honest
// choice rather than reading it as stone: stone would hide the seam but leave
// a hole in the world if the neighbour never arrives, and either way the
// section must be re-meshed once it does. Keeping loaded chunks meshed only
// when their neighbours are present is the world layer's job, not this one's.
struct ColumnNeighbourhood {
    const world::ChunkColumn* columns[9] = {};

    static constexpr int slot(int dx, int dz) { return (dz + 1) * 3 + (dx + 1); }

    const world::ChunkColumn* at(int dx, int dz) const { return columns[slot(dx, dz)]; }
    const world::ChunkColumn*& at(int dx, int dz) { return columns[slot(dx, dz)]; }

    const world::ChunkColumn* centre() const { return columns[slot(0, 0)]; }

    // The common case: one column with nothing around it. Useful in tests and
    // for a single-column world, and it makes the intent explicit at call sites.
    static ColumnNeighbourhood isolated(const world::ChunkColumn& column)
    {
        ColumnNeighbourhood n;
        n.at(0, 0) = &column;
        return n;
    }
};

class MeshScratch {
public:
    static constexpr int kPad = 1;
    static constexpr int kEdge = world::Section::kSize;
    static constexpr int kDim = kEdge + 2 * kPad;  // 18
    static constexpr int kCount = kDim * kDim * kDim;

    // Coordinates run -1..16. Y fastest, then Z, then X -- the same order
    // Section uses, so a fill and a mesh both walk forwards.
    static constexpr int index(int x, int y, int z)
    {
        return (y + kPad) + (z + kPad) * kDim + (x + kPad) * kDim * kDim;
    }

    void fill(const ColumnNeighbourhood& neighbourhood, int sectionY);

    world::BlockId block(int x, int y, int z) const { return blocks_[index(x, y, z)]; }

    // (skyLevel << 4) | blockLevel, the byte the vertex carries verbatim.
    u8 light(int x, int y, int z) const { return light_[index(x, y, z)]; }

    // The block's metadata nibble: a fluid's level, which way a door swings,
    // how far wheat has grown. Carried across the whole padded window rather
    // than just the interior, because a fluid's surface height at one corner
    // samples the four cells around it -- three of which can be in a
    // neighbouring column.
    u8 metadata(int x, int y, int z) const { return data_[index(x, y, z)]; }

    int sectionY() const { return sectionY_; }

private:
    world::BlockId blocks_[kCount] = {};
    u8 light_[kCount] = {};
    u8 data_[kCount] = {};
    int sectionY_ = 0;
};

}  // namespace mc::mesh
