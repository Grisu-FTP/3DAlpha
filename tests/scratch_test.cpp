#include "framework.hpp"

#include "core/mesh/scratch.hpp"

#include <cstdlib>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::MeshScratch;
using world::BlockId;
using world::ChunkColumn;
using world::Section;

namespace {

// What the fill used to be: one ChunkColumn lookup per cell, no bulk reads, no
// assumptions about index order. The fast path exists because this cost 46 % of
// the time to mesh a section, and the only thing that makes it safe to replace
// is that the two agree cell for cell.
//
// Kept as a reference implementation rather than deleted with the old code,
// because "the totals over a real world did not move" is evidence and this is
// proof.
struct Reference {
    BlockId blocks[MeshScratch::kCount];
    u8 light[MeshScratch::kCount];
    u8 data[MeshScratch::kCount];

    void fill(const ColumnNeighbourhood& neighbourhood, int sectionY)
    {
        constexpr int kPad = MeshScratch::kPad;
        constexpr int kEdge = MeshScratch::kEdge;
        const int baseY = sectionY * kEdge;

        for (int x = -kPad; x < kEdge + kPad; ++x) {
            const int dx = x < 0 ? -1 : (x >= kEdge ? 1 : 0);
            const int lx = x & (kEdge - 1);
            for (int z = -kPad; z < kEdge + kPad; ++z) {
                const int dz = z < 0 ? -1 : (z >= kEdge ? 1 : 0);
                const int lz = z & (kEdge - 1);
                const ChunkColumn* column = neighbourhood.at(dx, dz);

                for (int y = -kPad; y < kEdge + kPad; ++y) {
                    const int worldY = baseY + y;
                    const int i = MeshScratch::index(x, y, z);

                    if (column == nullptr) {
                        blocks[i] = world::kAirBlock;
                        light[i] = 0;
                        data[i] = 0;
                        continue;
                    }
                    if (worldY >= ChunkColumn::kHeight) {
                        blocks[i] = world::kAirBlock;
                        light[i] = 15 << 4;  // sky above the world is full
                        data[i] = 0;
                        continue;
                    }
                    blocks[i] = column->block(lx, worldY, lz);
                    light[i] = u8((column->skyLight(lx, worldY, lz) << 4)
                                  | column->blockLight(lx, worldY, lz));
                    data[i] = column->blockData(lx, worldY, lz);
                }
            }
        }
    }
};

// A deterministic pseudo-random generator, so a failure is reproducible and the
// test does not depend on the host's <random>.
struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed) {}
    u32 next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    int upTo(int n) { return int(next() % u32(n)); }
};

// `distinct` drives which encoding the section lands in: 1 is Uniform, a
// handful is Palette4, tens is Palette8, hundreds is Direct16. All four have
// their own branch in the bulk read, so all four have to be exercised.
void fillColumn(ChunkColumn& column, Rng& rng, int distinct)
{
    for (int y = 0; y < ChunkColumn::kHeight; ++y) {
        for (int z = 0; z < ChunkColumn::kWidth; ++z) {
            for (int x = 0; x < ChunkColumn::kWidth; ++x) {
                column.setBlock(x, y, z, BlockId(1 + rng.upTo(distinct)));
            }
        }
    }
    for (int y = 0; y < ChunkColumn::kHeight; ++y) {
        for (int z = 0; z < ChunkColumn::kWidth; ++z) {
            for (int x = 0; x < ChunkColumn::kWidth; ++x) {
                column.setSkyLight(x, y, z, u8(rng.upTo(16)));
                column.setBlockLight(x, y, z, u8(rng.upTo(16)));
                column.setBlockData(x, y, z, u8(rng.upTo(16)));
            }
        }
    }
}

bool agrees(const MeshScratch& fast, const Reference& slow)
{
    constexpr int kPad = MeshScratch::kPad;
    constexpr int kEdge = MeshScratch::kEdge;
    for (int x = -kPad; x < kEdge + kPad; ++x) {
        for (int y = -kPad; y < kEdge + kPad; ++y) {
            for (int z = -kPad; z < kEdge + kPad; ++z) {
                const int i = MeshScratch::index(x, y, z);
                if (fast.block(x, y, z) != slow.blocks[i]
                    || fast.light(x, y, z) != slow.light[i]
                    || fast.metadata(x, y, z) != slow.data[i]) {
                    return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

// Every section encoding, every section height, with neighbours present.
TEST(the_bulk_fill_agrees_with_a_per_cell_reference)
{
    static MeshScratch fast;
    static Reference slow;

    for (int distinct : {1, 4, 40, 400}) {
        Rng rng(0x9E3779B9u ^ u32(distinct));

        ChunkColumn centre(0, 0);
        ChunkColumn east(1, 0);
        ChunkColumn south(0, 1);
        fillColumn(centre, rng, distinct);
        fillColumn(east, rng, distinct);
        fillColumn(south, rng, distinct);

        ColumnNeighbourhood neighbourhood;
        neighbourhood.at(0, 0) = &centre;
        neighbourhood.at(1, 0) = &east;
        neighbourhood.at(0, 1) = &south;
        // The other six are deliberately left null: a partly loaded
        // neighbourhood is the normal state at the edge of the loaded area, and
        // both paths have to read those as air.

        for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
            fast.fill(neighbourhood, sy);
            slow.fill(neighbourhood, sy);
            if (!agrees(fast, slow)) {
                CHECK_EQ(distinct * 100 + sy, -1);  // reports which case failed
                return;
            }
        }
    }
    CHECK(true);
}

// The bottom and top of the world, where the shell leaves the column entirely.
TEST(the_shell_matches_at_the_world_floor_and_ceiling)
{
    static MeshScratch fast;
    static Reference slow;

    Rng rng(12345);
    ChunkColumn column(0, 0);
    fillColumn(column, rng, 8);

    const ColumnNeighbourhood neighbourhood = ColumnNeighbourhood::isolated(column);

    // Section 0's y = -1 is below bedrock; the top section's y = 16 is above the
    // build limit, where sky light is full rather than absent.
    for (int sy : {0, ChunkColumn::kSectionCount - 1}) {
        fast.fill(neighbourhood, sy);
        slow.fill(neighbourhood, sy);
        CHECK(agrees(fast, slow));
    }

    fast.fill(neighbourhood, ChunkColumn::kSectionCount - 1);
    CHECK_EQ(fast.block(0, MeshScratch::kEdge, 0), world::kAirBlock);
    CHECK_EQ(fast.light(0, MeshScratch::kEdge, 0), u8(15 << 4));

    fast.fill(neighbourhood, 0);
    CHECK_EQ(fast.block(0, -1, 0), world::kAirBlock);
    CHECK_EQ(fast.light(0, -1, 0), u8(0));
}

// An entirely absent neighbourhood still has to produce a well-defined scratch
// rather than whatever the previous section left behind.
TEST(a_fill_with_no_columns_at_all_is_air_everywhere)
{
    static MeshScratch fast;

    ChunkColumn column(0, 0);
    Rng rng(999);
    fillColumn(column, rng, 8);
    fast.fill(ColumnNeighbourhood::isolated(column), 3);

    ColumnNeighbourhood empty;
    fast.fill(empty, 3);

    constexpr int kPad = MeshScratch::kPad;
    constexpr int kEdge = MeshScratch::kEdge;
    for (int x = -kPad; x < kEdge + kPad; ++x) {
        for (int y = -kPad; y < kEdge + kPad; ++y) {
            for (int z = -kPad; z < kEdge + kPad; ++z) {
                CHECK_EQ(fast.block(x, y, z), world::kAirBlock);
                CHECK_EQ(fast.light(x, y, z), u8(0));
                CHECK_EQ(fast.metadata(x, y, z), u8(0));
            }
        }
    }
}
