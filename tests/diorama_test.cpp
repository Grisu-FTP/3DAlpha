// The World screen's diorama: where the table sits, how a chunk becomes cells,
// what a missing tile looks like and where walls go. See
// core/preview/diorama.hpp.

#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/preview/diorama.hpp"
#include "core/world/any_storage.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::preview;

namespace {

constexpr int kUnits = kDioramaUnitsPerBlock;

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_dio_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }

    std::string at(const char* name) const { return std::string(path) + "/" + name; }
};

// A cell column solid with `id` below `top` and air from there up.
void setColumn(DioramaGrid* grid, int cx, int cz, int top, block::BlockId id)
{
    for (int y = 0; y < kDioramaHeight; ++y) {
        grid->cells[usize(DioramaGrid::cellIndex(cx, y, cz))] = y < top ? u8(id) : u8(0);
    }
}

void markChunk(DioramaGrid* grid, int chunkX, int chunkZ, DioramaChunk state)
{
    grid->chunks[usize(chunkZ * kDioramaChunks + chunkX)] = state;
}

bool isTop(const mesh::DetailVertex* quad)
{
    return quad[0].face == mesh::kFacePosY;
}

}  // namespace

TEST(diorama_table_is_centred_on_the_origin_tile)
{
    i32 x = 0;
    i32 z = 0;
    // Block 0 is in tile 0, which starts at -64; one tile out is -192.
    dioramaOrigin(&x, &z);
    CHECK_EQ(x, -192);
    CHECK_EQ(z, -192);
    // A chunk corner, so every chunk falls wholly in one cell block.
    CHECK_EQ(((x % 16) + 16) % 16, 0);
    CHECK_EQ(((z % 16) + 16) % 16, 0);
    // And the table is a cube: as tall as it is wide, top at the table top.
    CHECK_EQ(dioramaTableTop() - dioramaTableBottom(), kDioramaBlocks);
}

// **Where World Settings' Move Panorama puts it.** The offset is in the same
// 128-block map tiles the bottom map draws its red grid at, so one step is a
// third of the table's own width -- and the corner has to stay on a chunk
// boundary, because a chunk that straddled two cell blocks would fold into
// neither.
TEST(a_moved_diorama_steps_by_whole_map_tiles)
{
    i32 baseX = 0;
    i32 baseZ = 0;
    dioramaOrigin(&baseX, &baseZ);

    // No offset is the position every world that has never been moved reads
    // as, and is what the argument-less call has always meant.
    i32 x = 0;
    i32 z = 0;
    dioramaOrigin(&x, &z, 0, 0);
    CHECK_EQ(x, baseX);
    CHECK_EQ(z, baseZ);

    const i32 steps[5] = {1, -1, 4, -9, 1000};
    for (i32 step : steps) {
        dioramaOrigin(&x, &z, step, 0);
        CHECK_EQ(x, baseX + step * map::kTileBlocks);
        CHECK_EQ(z, baseZ);

        dioramaOrigin(&x, &z, 0, step);
        CHECK_EQ(x, baseX);
        CHECK_EQ(z, baseZ + step * map::kTileBlocks);

        dioramaOrigin(&x, &z, step, -step);
        CHECK_EQ(x, baseX + step * map::kTileBlocks);
        CHECK_EQ(z, baseZ - step * map::kTileBlocks);

        // Still a chunk corner, whichever way it moved.
        CHECK_EQ(((x % 16) + 16) % 16, 0);
        CHECK_EQ(((z % 16) + 16) % 16, 0);
    }

    // Three tiles across: stepping the offset by the table's own width in
    // tiles moves it by exactly one table and leaves no gap or overlap.
    dioramaOrigin(&x, &z, kDioramaTiles, 0);
    CHECK_EQ(x, baseX + kDioramaBlocks);
}

TEST(diorama_cell_keeps_every_height_a_map_would_draw)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    const u8 stone = u8(mcver::Block::Stone);
    const u8 grass = u8(mcver::Block::Grass);
    std::unique_ptr<world::ChunkColumn> column(new world::ChunkColumn(0, 0));
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < 69; ++y) {
                column->setBlock(x, y, z, stone);
            }
            column->setBlock(x, 69, z, grass);
        }
    }
    // x = 5, z = 6 is a stone peak to 89 with a gap in it at 80..84 -- an
    // overhang -- and a torch on top, which no map draws.
    for (int y = 70; y < 90; ++y) {
        if (y < 80 || y > 84) {
            column->setBlock(5, y, 6, stone);
        }
    }
    column->setBlock(5, 90, 6, block::BlockId(mcver::Block::Torch));

    foldChunk(*column, 1, 2, &grid);
    CHECK(grid.chunks[usize(2 * kDioramaChunks + 1)] == DioramaChunk::Present);
    // The peak's cell: solid wherever its one column is, air in the gap.
    const int px = 1 * 4 + 1;
    const int pz = 2 * 4 + 1;
    CHECK_EQ(int(grid.cell(px, 89, pz)), int(stone));
    CHECK_EQ(int(grid.cell(px, 82, pz)), 0);
    CHECK_EQ(int(grid.cell(px, 79, pz)), int(stone));
    CHECK_EQ(int(grid.cell(px, 90, pz)), 0);
    // At the grass layer fifteen columns of grass outvote one of stone.
    CHECK_EQ(int(grid.cell(px, 69, pz)), int(grass));
    // A plain cell.
    CHECK_EQ(int(grid.cell(4, 69, 8)), int(grass));
    CHECK_EQ(int(grid.cell(4, 68, 8)), int(stone));
    CHECK_EQ(int(grid.cell(4, 70, 8)), 0);
}

TEST(diorama_missing_tile_is_its_ninth_of_the_slab_top)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    texture::AtlasImage atlas;  // colours are not what this is about
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);

    const int tileU = colours.slabTopTile % mesh::kAtlasTilesPerEdge;
    const int tileV = colours.slabTopTile / mesh::kAtlasTilesPerEdge;
    const int baseU = tileU * mesh::kUvUnitsPerTile;
    const int baseV = tileV * mesh::kUvUnitsPerTile;
    const int third = mesh::kUvUnitsPerTile / 3;

    for (int tz = 0; tz < kDioramaTiles; ++tz) {
        for (int tx = 0; tx < kDioramaTiles; ++tx) {
            DioramaMesh mesh;
            buildDioramaTile(grid, tx, tz, colours, &mesh);
            CHECK(mesh.coloured.empty());
            CHECK_EQ(mesh.textured.size(), usize(map::kTileChunks * map::kTileChunks * 4));

            int minU = 1 << 30, maxU = -(1 << 30), minV = 1 << 30, maxV = -(1 << 30);
            for (const mesh::DetailVertex& v : mesh.textured) {
                CHECK_EQ(int(v.y), dioramaTableTop() * kUnits);
                minU = v.u < minU ? v.u : minU;
                maxU = v.u > maxU ? v.u : maxU;
                minV = v.v < minV ? v.v : minV;
                maxV = v.v > maxV ? v.v : maxV;
            }
            // Within an inset and a unit of rounding of exactly one third.
            CHECK(std::abs(minU - (baseU + tx * third)) <= mesh::kUvInset + 1);
            CHECK(std::abs(maxU - (baseU + (tx + 1) * third)) <= mesh::kUvInset + 1);
            CHECK(std::abs(minV - (baseV + tz * third)) <= mesh::kUvInset + 1);
            CHECK(std::abs(maxV - (baseV + (tz + 1) * third)) <= mesh::kUvInset + 1);
        }
    }
}

TEST(diorama_walls_line_a_pit_and_a_peak)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    const int table = dioramaTableTop();
    const block::BlockId grass = block::BlockId(mcver::Block::Grass);
    // Chunk (12, 12) -- tile (1, 1)'s interior -- present and flat at the table.
    markChunk(&grid, 12, 12, DioramaChunk::Present);
    for (int cz = 48; cz < 52; ++cz) {
        for (int cx = 48; cx < 52; ++cx) {
            setColumn(&grid, cx, cz, table, grass);
        }
    }
    setColumn(&grid, 49, 49, table - 4, grass);   // a pit
    setColumn(&grid, 50, 50, table + 16, grass);  // a peak
    openDioramaAir(&grid, DioramaOpen::Full);

    texture::AtlasImage atlas;
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);
    DioramaMesh mesh;
    buildDioramaTile(grid, 1, 1, colours, &mesh);

    int pitWalls = 0;
    int peakWalls = 0;
    for (usize q = 0; q < mesh.coloured.size() / 4; ++q) {
        const mesh::DetailVertex* quad = &mesh.coloured[q * 4];
        if (isTop(quad)) {
            continue;
        }
        int low = quad[0].y;
        int high = quad[0].y;
        for (int c = 1; c < 4; ++c) {
            low = quad[c].y < low ? quad[c].y : low;
            high = quad[c].y > high ? quad[c].y : high;
        }
        if (low == (table - 4) * kUnits && high == table * kUnits) {
            ++pitWalls;
        } else if (low == table * kUnits && high == (table + 16) * kUnits) {
            ++peakWalls;
        }
    }
    CHECK_EQ(pitWalls, 4);
    CHECK_EQ(peakWalls, 4);
    // Nothing else stands out of the table in that chunk: eight merged tops
    // (one, three, three, one across its four rows) and the eight walls. The
    // rest of the tile is slab: 63 missing chunks, one quad each.
    CHECK_EQ(mesh.coloured.size() / 4, usize(8 + 8));
    CHECK_EQ(mesh.textured.size() / 4, usize(63));
}

TEST(diorama_overhang_keeps_its_air_and_a_sealed_cave_costs_nothing)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    const int table = dioramaTableTop();
    const u8 grass = u8(mcver::Block::Grass);
    markChunk(&grid, 12, 12, DioramaChunk::Present);
    for (int cz = 48; cz < 52; ++cz) {
        for (int cx = 48; cx < 52; ++cx) {
            setColumn(&grid, cx, cz, table, grass);
        }
    }
    // A slab of ground floating eight above the table over one cell, joined to
    // nothing -- the far end of an overhang.
    for (int y = table + 8; y < table + 10; ++y) {
        grid.cells[usize(DioramaGrid::cellIndex(49, y, 49))] = grass;
    }
    // And a pocket of air sealed deep in the ground.
    grid.cells[usize(DioramaGrid::cellIndex(50, table - 10, 50))] = 0;
    openDioramaAir(&grid, DioramaOpen::Full);

    CHECK(grid.isOpen(49, table, 49));        // under the overhang, reached sideways
    CHECK(!grid.isOpen(50, table - 10, 50));  // the pocket, reached by nothing

    texture::AtlasImage atlas;
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);
    DioramaMesh mesh;
    buildDioramaTile(grid, 1, 1, colours, &mesh);

    int groundAtTable = 0;
    int ceilings = 0;
    int topsAbove = 0;
    int walls = 0;
    for (usize q = 0; q < mesh.coloured.size() / 4; ++q) {
        const mesh::DetailVertex* quad = &mesh.coloured[q * 4];
        if (quad[0].face == mesh::kFacePosY && quad[0].y == table * kUnits) {
            ++groundAtTable;
        } else if (quad[0].face == mesh::kFacePosY && quad[0].y == (table + 10) * kUnits) {
            ++topsAbove;
        } else if (quad[0].face == mesh::kFaceNegY) {
            CHECK_EQ(int(quad[0].y), (table + 8) * kUnits);
            ++ceilings;
        } else {
            ++walls;
        }
    }
    // The ground under the overhang is still there: four whole rows at the
    // table, not three and a hole.
    CHECK_EQ(groundAtTable, 4);
    CHECK_EQ(topsAbove, 1);
    CHECK_EQ(ceilings, 1);
    CHECK_EQ(walls, 4);
    CHECK_EQ(mesh.coloured.size() / 4, usize(4 + 1 + 1 + 4));
}

// The two places a face has no air cell of its own to be drawn from, both of
// which left a hole to see in through.
TEST(diorama_closes_the_table_rim_and_the_top_of_the_world)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    const int table = dioramaTableTop();
    const u8 stone = u8(mcver::Block::Stone);
    markChunk(&grid, 0, 0, DioramaChunk::Present);
    for (int cz = 0; cz < 4; ++cz) {
        for (int cx = 0; cx < 4; ++cx) {
            setColumn(&grid, cx, cz, table, stone);
        }
    }
    // A hill on the table's north-west corner cell, standing four above it, and
    // a column that runs to the very top of the world beside it.
    for (int y = table; y < table + 4; ++y) {
        grid.cells[usize(DioramaGrid::cellIndex(0, y, 0))] = stone;
    }
    for (int y = table; y < kDioramaHeight; ++y) {
        grid.cells[usize(DioramaGrid::cellIndex(2, y, 0))] = stone;
    }
    openDioramaAir(&grid, DioramaOpen::Full);

    texture::AtlasImage atlas;
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);
    DioramaMesh mesh;
    buildDioramaTile(grid, 0, 0, colours, &mesh);

    int rimNegX = 0;
    int rimNegZ = 0;
    int topOfWorld = 0;
    for (usize q = 0; q < mesh.coloured.size() / 4; ++q) {
        const mesh::DetailVertex* quad = &mesh.coloured[q * 4];
        const int face = quad[0].face;
        int low = quad[0].y;
        int high = quad[0].y;
        for (int c = 1; c < 4; ++c) {
            low = quad[c].y < low ? quad[c].y : low;
            high = quad[c].y > high ? quad[c].y : high;
        }
        // The west and north walls of the corner hill, which face out of the
        // table and so have no cell of the grid in front of them.
        if (face == mesh::kFaceNegX && low == table * kUnits
            && high == (table + 4) * kUnits && quad[0].x == -kDioramaBlocks / 2 * kUnits) {
            ++rimNegX;
        }
        if (face == mesh::kFaceNegZ && low == table * kUnits
            && high == (table + 4) * kUnits && quad[0].z == -kDioramaBlocks / 2 * kUnits) {
            ++rimNegZ;
        }
        if (face == mesh::kFacePosY && quad[0].y == kDioramaHeight * kUnits) {
            ++topOfWorld;
        }
    }
    CHECK_EQ(rimNegX, 1);
    CHECK_EQ(rimNegZ, 1);
    // The column that reaches y = 127 is capped, rather than open to look down.
    CHECK_EQ(topOfWorld, 1);
}

TEST(diorama_lights_a_face_from_the_air_in_front_of_it)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    std::unique_ptr<world::ChunkColumn> column(new world::ChunkColumn(0, 0));
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            for (int y = 0; y < 70; ++y) {
                column->setBlock(x, y, z, block::BlockId(mcver::Block::Stone));
            }
            for (int y = 70; y < kDioramaHeight; ++y) {
                column->setSkyLight(x, y, z, 15);
            }
        }
    }
    // One cell's worth of columns is in shadow with a torch beside it: sky 4,
    // block 9, so the level is 9 -- and the cell keeps its brightest sample.
    for (int x = 4; x < 8; ++x) {
        for (int z = 0; z < 4; ++z) {
            column->setSkyLight(x, 70, z, 4);
            column->setBlockLight(x, 70, z, u8(x == 4 ? 9 : 2));
        }
    }
    foldChunk(*column, 12, 12, &grid);
    openDioramaAir(&grid, DioramaOpen::Full);
    CHECK_EQ(int(grid.lightAt(48, 70, 48)), 15);
    CHECK_EQ(int(grid.lightAt(49, 70, 48)), 9);

    texture::AtlasImage atlas;
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);
    DioramaMesh mesh;
    buildDioramaTile(grid, 1, 1, colours, &mesh);

    // The ground under each of those two cells, lit by the air above it. The
    // byte is (sky << 4) | block, which at noon the lightmap reads as the level.
    bool sawLit = false;
    bool sawShaded = false;
    for (usize q = 0; q < mesh.coloured.size() / 4; ++q) {
        const mesh::DetailVertex* quad = &mesh.coloured[q * 4];
        if (quad[0].face != mesh::kFacePosY || quad[0].y != 70 * kUnits) {
            continue;
        }
        const int x0 = int(quad[0].x) / kUnits + kDioramaBlocks / 2;
        const int z0 = int(quad[0].z) / kUnits + kDioramaBlocks / 2;
        if (z0 != 48 * kDioramaCellBlocks) {
            continue;
        }
        if (x0 == 48 * kDioramaCellBlocks) {
            CHECK_EQ(int(quad[0].light), 15 << 4);
            sawLit = true;
        } else if (x0 == 49 * kDioramaCellBlocks) {
            CHECK_EQ(int(quad[0].light), 9 << 4);
            sawShaded = true;
        }
    }
    CHECK(sawLit);
    CHECK(sawShaded);
}

TEST(diorama_sides_are_one_slab_side_each_from_bottom_to_top)
{
    texture::AtlasImage atlas;
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);
    DioramaMesh mesh;
    buildDioramaSides(colours, &mesh);
    CHECK_EQ(mesh.textured.size(), usize(4 * 4));
    const int tileU = colours.slabSideTile % mesh::kAtlasTilesPerEdge;
    const int tileV = colours.slabSideTile / mesh::kAtlasTilesPerEdge;
    for (const mesh::DetailVertex& v : mesh.textured) {
        CHECK(v.y == dioramaTableBottom() * kUnits || v.y == dioramaTableTop() * kUnits);
        CHECK(v.u == mesh::tileUvMin(tileU) || v.u == mesh::tileUvMax(tileU));
        CHECK(v.v == mesh::tileUvMin(tileV) || v.v == mesh::tileUvMax(tileV));
        CHECK(std::abs(int(v.x)) <= kDioramaBlocks / 2 * kUnits);
        CHECK(std::abs(int(v.z)) <= kDioramaBlocks / 2 * kUnits);
    }
}

TEST(diorama_reads_a_tile_of_a_real_world_and_stops_when_asked)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");
    {
        world::AnyStorage storage(fs);
        CHECK(storage.create(dir, 7, 1000, world::WorldFormat::Folder)
              == world::OpenResult::Ok);
        world::ChunkColumn chunk(-1, -1);
        for (int x = 0; x < 16; ++x) {
            for (int z = 0; z < 16; ++z) {
                for (int y = 0; y < 24; ++y) {
                    chunk.setBlock(x, y, z, block::BlockId(mcver::Block::Stone));
                }
                chunk.heightMap[usize(z) * 16 + usize(x)] = 24;
            }
        }
        CHECK(storage.saveChunk(chunk));
        CHECK(storage.commit());
        CHECK(storage.close(2000));
    }

    world::WorldPeek peek(fs);
    CHECK(peek.open(dir));
    DioramaGrid grid;
    i32 originX = 0;
    i32 originZ = 0;
    dioramaOrigin(&originX, &originZ);
    grid.reset(originX, originZ);

    // Asked to stop before the first read: nothing changes.
    CHECK(!readDioramaTile(peek, 1, 1, &grid, [](void*) { return false; }, nullptr));
    CHECK(!grid.tileRead(1, 1));

    CHECK(readDioramaTile(peek, 1, 1, &grid, nullptr, nullptr));
    CHECK(grid.tileRead(1, 1));
    CHECK(!grid.tileRead(0, 0));

    // The table's corner is block -192, chunk -12: world chunk
    // (-1, -1) is grid chunk (11, 11), in the middle tile.
    CHECK(grid.chunks[usize(11 * kDioramaChunks + 11)] == DioramaChunk::Present);
    CHECK(grid.chunks[usize(12 * kDioramaChunks + 12)] == DioramaChunk::Absent);
    CHECK_EQ(int(grid.cell(44, 23, 44)), int(mcver::Block::Stone));
    CHECK_EQ(int(grid.cell(44, 24, 44)), 0);
    // And a chunk the world does not have is still the table.
    CHECK_EQ(int(grid.cell(48, 10, 48)), int(mcver::Block::DoubleSlab));
    CHECK(grid.isOpen(44, 24, 44));
}

TEST(diorama_groups_its_quads_by_which_way_they_face)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    const int table = dioramaTableTop();
    const block::BlockId grass = block::BlockId(mcver::Block::Grass);
    markChunk(&grid, 12, 12, DioramaChunk::Present);
    for (int cz = 48; cz < 52; ++cz) {
        for (int cx = 48; cx < 52; ++cx) {
            setColumn(&grid, cx, cz, table, grass);
        }
    }
    setColumn(&grid, 50, 50, table + 16, grass);  // walls all four ways
    // An overhang, so there is an underside to group as well.
    for (int y = table + 4; y < table + 6; ++y) {
        grid.cells[usize(DioramaGrid::cellIndex(49, y, 50))] = u8(grass);
    }
    openDioramaAir(&grid, DioramaOpen::Full);

    texture::AtlasImage atlas;
    DioramaColours colours;
    buildDioramaColours(atlas, &colours);
    DioramaMesh mesh;
    buildDioramaTile(grid, 1, 1, colours, &mesh);

    // Every group is a run of one facing, and together they are the whole of
    // the coloured stream.
    usize at = 0;
    for (int i = 0; i < mesh::kFaceCount; ++i) {
        const int face = kDioramaFaceOrder[i];
        CHECK(mesh.run[i] % 4 == 0);
        for (usize v = 0; v < mesh.run[i]; ++v) {
            CHECK_EQ(int(mesh.coloured[at + v].face), face);
        }
        at += mesh.run[i];
        CHECK(mesh.run[i] > 0);  // this grid has faces every way
    }
    CHECK_EQ(at, mesh.coloured.size());
}

TEST(diorama_shows_a_facing_only_while_the_camera_is_on_that_side)
{
    const float pitch = 0.5236f;  // the menu's 30 degrees
    // The camera is above the table and never looks up.
    for (int i = 0; i < 64; ++i) {
        const float yaw = 6.2831853f * float(i) / 64.0f;
        CHECK(dioramaFaceVisible(mesh::kFacePosY, yaw, pitch));
        CHECK(!dioramaFaceVisible(mesh::kFaceNegY, yaw, pitch));
        int walls = 0;
        for (int face : {mesh::kFaceNegX, mesh::kFacePosX, mesh::kFaceNegZ, mesh::kFacePosZ}) {
            walls += dioramaFaceVisible(face, yaw, pitch) ? 1 : 0;
            // Opposite walls are never both shown.
            CHECK(!(dioramaFaceVisible(face, yaw, pitch)
                    && dioramaFaceVisible(face ^ 1, yaw, pitch)));
        }
        CHECK(walls <= 2);
    }
    // Looking from +Z: the faces pointing back at the camera, and not their
    // opposites. This is the axis the menu's own matrix makes deeper towards
    // the viewer, which is what keeps the two agreeing.
    CHECK(dioramaFaceVisible(mesh::kFacePosZ, 0.0f, pitch));
    CHECK(!dioramaFaceVisible(mesh::kFaceNegZ, 0.0f, pitch));
    // A quarter turn on, from -X.
    CHECK(dioramaFaceVisible(mesh::kFaceNegX, 1.5707963f, pitch));
    CHECK(!dioramaFaceVisible(mesh::kFacePosX, 1.5707963f, pitch));
}

TEST(diorama_grid_costs_what_the_cache_is_sized_against)
{
    DioramaGrid grid;
    grid.reset(0, 0);
    const usize actual = grid.cells.size() + grid.open.size() * sizeof(u32)
                         + grid.light.size() + grid.chunks.size();
    CHECK_EQ(actual, kDioramaGridBytes);
}

TEST(diorama_reads_a_packed_tile_as_one_batch_and_leaves_a_stopped_one_unread)
{
    // The batch is the whole tile at once, so what the tile looks like
    // afterwards has to be what a chunk at a time produced: read chunks
    // Present, never-generated ones Absent -- and, when the caller stops part
    // way, the rest still Unread rather than wrongly written off as Absent.
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");

    // The table's corner is chunk -12, so the middle tile is world chunks
    // -4..3. These three are in it; nothing else in the world is.
    const int present[3][2] = {{-1, -1}, {0, 0}, {2, 1}};
    {
        world::AnyStorage storage(fs);
        CHECK(storage.create(dir, 11, 1000, world::WorldFormat::Packed)
              == world::OpenResult::Ok);
        for (const auto& at : present) {
            world::ChunkColumn chunk(at[0], at[1]);
            for (int x = 0; x < 16; ++x) {
                for (int z = 0; z < 16; ++z) {
                    for (int y = 0; y < 24; ++y) {
                        chunk.setBlock(x, y, z, block::BlockId(mcver::Block::Stone));
                    }
                    chunk.heightMap[usize(z) * 16 + usize(x)] = 24;
                }
            }
            CHECK(storage.saveChunk(chunk));
        }
        CHECK(storage.commit());
        CHECK(storage.close(2000));
    }

    i32 originX = 0;
    i32 originZ = 0;
    dioramaOrigin(&originX, &originZ);
    const int originChunk = originX / world::ChunkColumn::kWidth;

    // **A stop leaves what it never reached Unread**, not written off as
    // Absent, so the next pass still reads it. Ten answers in, the tile is part
    // done and still owed.
    {
        world::WorldPeek peek(fs);
        CHECK(peek.open(dir));
        DioramaGrid grid;
        grid.reset(originX, originZ);

        int budget = 10;
        CHECK(!readDioramaTile(
            peek, 1, 1, &grid, [](void* c) { return (*static_cast<int*>(c))-- > 0; }, &budget));
        CHECK(!grid.tileRead(1, 1));

        int settled = 0;
        int unread = 0;
        for (int dz = 0; dz < map::kTileChunks; ++dz) {
            for (int dx = 0; dx < map::kTileChunks; ++dx) {
                const usize i = usize((map::kTileChunks + dz) * kDioramaChunks
                                      + map::kTileChunks + dx);
                settled += grid.chunks[i] != DioramaChunk::Unread ? 1 : 0;
                unread += grid.chunks[i] == DioramaChunk::Unread ? 1 : 0;
            }
        }
        // One answer went on the check before the read; the other nine settled
        // a chunk each, and the rest of the tile was never looked at.
        CHECK_EQ(settled, 9);
        CHECK_EQ(unread, map::kTileChunks * map::kTileChunks - 9);

        // Picking it up again finishes it, and nothing was lost to the stop.
        CHECK(readDioramaTile(peek, 1, 1, &grid, nullptr, nullptr));
        CHECK(grid.tileRead(1, 1));
        CHECK(grid.chunks[usize((0 - originChunk) * kDioramaChunks + (0 - originChunk))]
              == DioramaChunk::Present);
    }

    // Read through: the three chunks are Present and the other 61 Absent.
    world::WorldPeek peek(fs);
    CHECK(peek.open(dir));
    DioramaGrid grid;
    grid.reset(originX, originZ);
    CHECK(readDioramaTile(peek, 1, 1, &grid, nullptr, nullptr));
    CHECK(grid.tileRead(1, 1));

    for (const auto& at : present) {
        const int gx = at[0] - originChunk;
        const int gz = at[1] - originChunk;
        CHECK(grid.chunks[usize(gz * kDioramaChunks + gx)] == DioramaChunk::Present);
    }
    int absent = 0;
    for (int dz = 0; dz < map::kTileChunks; ++dz) {
        for (int dx = 0; dx < map::kTileChunks; ++dx) {
            const usize i =
                usize((map::kTileChunks + dz) * kDioramaChunks + map::kTileChunks + dx);
            absent += grid.chunks[i] == DioramaChunk::Absent ? 1 : 0;
        }
    }
    CHECK_EQ(absent, map::kTileChunks * map::kTileChunks - 3);

    // And the terrain landed where it should: chunk (0, 0) is grid chunk 12.
    CHECK_EQ(int(grid.cell(12 * kDioramaCellsPerChunk, 23, 12 * kDioramaCellsPerChunk)),
             int(mcver::Block::Stone));
}

TEST(diorama_hands_each_tile_over_as_it_finishes_rather_than_at_the_end_of_the_batch)
{
    // Eight tiles are read as one batch because that is what makes the reads
    // cheap, but nothing may wait on the whole batch to reach the screen: each
    // tile is handed back the moment its last chunk lands.
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string dir = temp.at("World");
    {
        world::AnyStorage storage(fs);
        CHECK(storage.create(dir, 13, 1000, world::WorldFormat::Packed)
              == world::OpenResult::Ok);
        // One chunk in each of the nine tiles, so no tile is empty.
        for (int tz = 0; tz < kDioramaTiles; ++tz) {
            for (int tx = 0; tx < kDioramaTiles; ++tx) {
                world::ChunkColumn chunk(i32(-12 + tx * map::kTileChunks),
                                         i32(-12 + tz * map::kTileChunks));
                for (int x = 0; x < 16; ++x) {
                    for (int z = 0; z < 16; ++z) {
                        chunk.setBlock(x, 0, z, block::BlockId(mcver::Block::Stone));
                        chunk.heightMap[usize(z) * 16 + usize(x)] = 1;
                    }
                }
                CHECK(storage.saveChunk(chunk));
            }
        }
        CHECK(storage.commit());
        CHECK(storage.close(2000));
    }

    i32 originX = 0;
    i32 originZ = 0;
    dioramaOrigin(&originX, &originZ);

    world::WorldPeek peek(fs);
    CHECK(peek.open(dir));
    DioramaGrid grid;
    grid.reset(originX, originZ);

    struct Seen {
        std::vector<int> tiles;
        const DioramaGrid* grid;
        // A tile handed over must already be fully read, or the mesh built
        // from it would be missing ground it is about to be given.
        bool early = false;

        static void done(void* context, int tileX, int tileZ)
        {
            Seen& self = *static_cast<Seen*>(context);
            self.early = self.early || !self.grid->tileRead(tileX, tileZ);
            self.tiles.push_back(tileZ * kDioramaTiles + tileX);
        }
    };
    Seen seen;
    seen.grid = &grid;

    int xs[kDioramaTiles * kDioramaTiles];
    int zs[kDioramaTiles * kDioramaTiles];
    dioramaTileOrder(xs, zs);

    // All nine as one batch: every one of them still comes back separately.
    CHECK(readDioramaTiles(peek, xs, zs, kDioramaTiles * kDioramaTiles, &grid, nullptr,
                           &Seen::done, &seen));
    CHECK(!seen.early);
    CHECK_EQ(seen.tiles.size(), usize(kDioramaTiles * kDioramaTiles));
    std::sort(seen.tiles.begin(), seen.tiles.end());
    for (int t = 0; t < kDioramaTiles * kDioramaTiles; ++t) {
        CHECK_EQ(seen.tiles[usize(t)], t);
    }

    // Nothing is owed afterwards, and asking again hands nothing back.
    Seen again;
    again.grid = &grid;
    CHECK(readDioramaTiles(peek, xs, zs, kDioramaTiles * kDioramaTiles, &grid, nullptr,
                           &Seen::done, &again));
    CHECK(again.tiles.empty());
}
