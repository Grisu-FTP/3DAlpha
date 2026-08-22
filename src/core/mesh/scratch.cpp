#include "core/mesh/scratch.hpp"

#include "core/world/section.hpp"

namespace mc::mesh {

using world::BlockId;
using world::ChunkColumn;
using world::kAirBlock;
using world::Section;

namespace {

// Sky light above the world is full, not zero.
//
// ChunkColumn reports out-of-range Y as air with no light, which is right for
// physics and wrong here: it would leave the top face of every block at the
// build limit unlit, and a world whose terrain reaches the ceiling would have a
// black lid. Below the world there genuinely is no sky.
constexpr u8 kAboveWorldLight = 15 << 4;

// The whole interior fast path rests on this: a run of consecutive Y values is
// contiguous in both the section and the scratch, so one bulk read moves a
// sixteen-block column. Both orders are load-bearing elsewhere too -- the Alpha
// column arrays are Y-fastest, which is why Section chose it -- but nothing
// else would *break* if one of them changed, so it is asserted here.
static_assert(MeshScratch::index(0, 1, 0) - MeshScratch::index(0, 0, 0) == 1,
              "the scratch must be Y-fastest for the bulk fill to be contiguous");
static_assert(Section::index(0, 1, 0) - Section::index(0, 0, 0) == 1,
              "the section must be Y-fastest for the bulk fill to be contiguous");

}  // namespace

void MeshScratch::fill(const ColumnNeighbourhood& neighbourhood, int sectionY)
{
    sectionY_ = sectionY;

    const int baseY = sectionY * kEdge;

    // One cell of the shell -- the y = -1 and y = 16 planes, which may live in
    // the section above or below, or outside the world entirely. 1,736 of the
    // 5,832 cells, and the only ones still paying for a ChunkColumn lookup.
    const auto shellCell = [&](int x, int y, int z, const ChunkColumn* column, int lx, int lz) {
        const int worldY = baseY + y;
        const int i = index(x, y, z);

        if (worldY >= ChunkColumn::kHeight) {
            blocks_[i] = kAirBlock;
            light_[i] = kAboveWorldLight;
            data_[i] = 0;
            return;
        }

        blocks_[i] = column->block(lx, worldY, lz);
        light_[i] = static_cast<u8>((column->skyLight(lx, worldY, lz) << 4)
                                    | column->blockLight(lx, worldY, lz));
        data_[i] = column->blockData(lx, worldY, lz);
    };

    for (int x = -kPad; x < kEdge + kPad; ++x) {
        // -1 and 16 are the only values that leave the centre column, and the
        // mask converts them to the neighbour's 15 and 0 in one operation.
        const int dx = x < 0 ? -1 : (x >= kEdge ? 1 : 0);
        const int lx = x & (kEdge - 1);

        for (int z = -kPad; z < kEdge + kPad; ++z) {
            const int dz = z < 0 ? -1 : (z >= kEdge ? 1 : 0);
            const int lz = z & (kEdge - 1);

            const ChunkColumn* column = neighbourhood.at(dx, dz);

            if (column == nullptr) {
                // Not loaded, read as air. See ColumnNeighbourhood for why this
                // is the honest answer rather than reading it as stone.
                const int first = index(x, -kPad, z);
                for (int i = 0; i < kDim; ++i) {
                    blocks_[first + i] = kAirBlock;
                    light_[first + i] = 0;
                    data_[first + i] = 0;
                }
                continue;
            }

            // The interior, 16 blocks of one section in one go. This is 4,096
            // of the 5,832 cells and it no longer touches ChunkColumn at all.
            const Section& section = column->section(sectionY);
            const int start = Section::index(lx, 0, lz);
            section.readBlocks(start, kEdge, &blocks_[index(x, 0, z)]);
            section.readPackedLight(start, kEdge, &light_[index(x, 0, z)]);
            section.readData(start, kEdge, &data_[index(x, 0, z)]);

            shellCell(x, -kPad, z, column, lx, lz);
            shellCell(x, kEdge, z, column, lx, lz);
        }
    }
}

}  // namespace mc::mesh
