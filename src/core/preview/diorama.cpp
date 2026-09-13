#include "core/preview/diorama.hpp"

#include "core/block/registry.hpp"
#include "core/map/map_palette.hpp"

#include "version_slots.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace mc::preview {

// A cell is a byte, so every block id has to fit in one.
static_assert(mcver::kBlockTableSize <= 256);

namespace {

constexpr int kUnits = kDioramaUnitsPerBlock;
constexpr int kHalfBlocks = kDioramaBlocks / 2;
constexpr int kHeight = kDioramaHeight;
constexpr u8 kTable = u8(mcver::Block::DoubleSlab);

// The vertex's light byte is (sky << 4) | block, and the preview binds the
// world's own lightmap at noon -- where the texel for (sky, 0) is the
// brightness of `sky`. So a level 0..15 goes in as its sky nibble.
constexpr u8 kFullLight = 0xFF;

u8 lightByte(u8 level)
{
    return u8(level << 4);
}

int chunkIndex(int chunkX, int chunkZ)
{
    return chunkZ * kDioramaChunks + chunkX;
}

bool inGrid(int cx, int cz)
{
    return cx >= 0 && cz >= 0 && cx < kDioramaCells && cz < kDioramaCells;
}

void setOpen(std::vector<u32>* bits, usize index)
{
    (*bits)[index >> 5] |= u32(1) << (index & 31);
}

bool testOpen(const std::vector<u32>& bits, usize index)
{
    return ((bits[index >> 5] >> (index & 31)) & 1u) != 0;
}

i16 unitsX(int block)
{
    return i16((block - kHalfBlocks) * kUnits);
}

i16 unitsY(int y)
{
    return i16(y * kUnits);
}

u8 channel(u32 rgb, int shift, u8 shade)
{
    return u8((((rgb >> shift) & 0xFF) * u32(shade) + 127) / 255);
}

// One quad: four corners in order round its edge, one colour, one UV rectangle.
void quad(std::vector<mesh::DetailVertex>* out, const i16 corners[4][3], u32 rgb, u8 shade,
          int face, const i16 uv[4][2], u8 light)
{
    for (int c = 0; c < 4; ++c) {
        mesh::DetailVertex v;
        v.x = corners[c][0];
        v.y = corners[c][1];
        v.z = corners[c][2];
        v.face = i16(face);
        v.u = uv != nullptr ? uv[c][0] : 0;
        v.v = uv != nullptr ? uv[c][1] : 0;
        v.r = channel(rgb, 16, shade);
        v.g = channel(rgb, 8, shade);
        v.b = channel(rgb, 0, shade);
        v.light = light;
        out->push_back(v);
    }
}

// A horizontal rectangle facing `face` (up or down) at height `y` over blocks
// [x0, x1) x [z0, z1).
void flatQuad(std::vector<mesh::DetailVertex>* out, int face, int x0, int x1, int z0, int z1,
              int y, u32 rgb, const i16 uv[4][2], u8 light)
{
    const i16 corners[4][3] = {
        {unitsX(x0), unitsY(y), unitsX(z0)},
        {unitsX(x1), unitsY(y), unitsX(z0)},
        {unitsX(x1), unitsY(y), unitsX(z1)},
        {unitsX(x0), unitsY(y), unitsX(z1)},
    };
    quad(out, corners, rgb, mesh::kFaceShade[face], face, uv, light);
}

// A vertical rectangle facing `face`, from y0 up to y1, along the edge of the
// block span [a0, a1) at the fixed coordinate `at`.
void wallQuad(std::vector<mesh::DetailVertex>* out, int face, int at, int a0, int a1, int y0,
              int y1, u32 rgb, const i16 uv[4][2], u8 light)
{
    i16 corners[4][3];
    const bool alongX = face == mesh::kFaceNegZ || face == mesh::kFacePosZ;
    const int along[4] = {a0, a1, a1, a0};
    const int ys[4] = {y0, y0, y1, y1};
    for (int c = 0; c < 4; ++c) {
        corners[c][0] = alongX ? unitsX(along[c]) : unitsX(at);
        corners[c][1] = unitsY(ys[c]);
        corners[c][2] = alongX ? unitsX(at) : unitsX(along[c]);
    }
    quad(out, corners, rgb, mesh::kFaceShade[face], face, uv, light);
}

// The four neighbours of a cell, as the step to them and the face each shows
// back towards the cell.
struct Side {
    int face;
    int dx;
    int dz;
};
constexpr Side kSides[4] = {
    {mesh::kFacePosZ, 0, -1},
    {mesh::kFaceNegZ, 0, 1},
    {mesh::kFacePosX, -1, 0},
    {mesh::kFaceNegX, 1, 0},
};

bool cellPresent(const DioramaGrid& grid, int cx, int cz)
{
    return grid.chunks[usize(chunkIndex(cx / kDioramaCellsPerChunk, cz / kDioramaCellsPerChunk))]
           == DioramaChunk::Present;
}

// What stands above or below a cell: the grid inside the world's height, the
// table under the world, and sky over it.
u8 solidAbove(const DioramaGrid& grid, int cx, int y, int cz)
{
    return y + 1 >= kHeight ? 0 : grid.cell(cx, y + 1, cz);
}

u8 solidBelow(const DioramaGrid& grid, int cx, int y, int cz)
{
    return y <= 0 ? kTable : grid.cell(cx, y - 1, cz);
}

// The slab's top tile stretched across the whole table, sampled over blocks
// [x0, x1) x [z0, z1).
void slabTopUv(u16 tile, int x0, int x1, int z0, int z1, i16 uv[4][2])
{
    const int tileU = tile % mesh::kAtlasTilesPerEdge;
    const int tileV = tile / mesh::kAtlasTilesPerEdge;
    const auto at = [](int tileAxis, int block, bool high) {
        const long units = long(tileAxis) * mesh::kUvUnitsPerTile
                           + (long(block) * mesh::kUvUnitsPerTile + kDioramaBlocks / 2)
                                 / kDioramaBlocks;
        return i16(high ? units - mesh::kUvInset : units + mesh::kUvInset);
    };
    const i16 u0 = at(tileU, x0, false);
    const i16 u1 = at(tileU, x1, true);
    const i16 v0 = at(tileV, z0, false);
    const i16 v1 = at(tileV, z1, true);
    uv[0][0] = u0;
    uv[0][1] = v0;
    uv[1][0] = u1;
    uv[1][1] = v0;
    uv[2][0] = u1;
    uv[2][1] = v1;
    uv[3][0] = u0;
    uv[3][1] = v1;
}

}  // namespace

void DioramaGrid::reset(i32 blockX, i32 blockZ)
{
    originBlockX = blockX;
    originBlockZ = blockZ;
    const int tableTop = std::clamp(dioramaTableTop(), 0, kHeight);
    const usize columns = usize(kDioramaCells) * kDioramaCells;
    cells.assign(columns * kHeight, 0);
    open.assign((cells.size() + 31) / 32, 0);
    // Full sky until a chunk says otherwise: the table stands in daylight.
    light.assign((cells.size() + 1) / 2, 0xFF);
    // The table everywhere: slab below its top, and the air above it all open.
    for (usize column = 0; column < columns; ++column) {
        const usize base = column * kHeight;
        std::fill(cells.begin() + long(base), cells.begin() + long(base + usize(tableTop)),
                  kTable);
        for (int y = tableTop; y < kHeight; ++y) {
            setOpen(&open, base + usize(y));
        }
    }
    chunks.assign(usize(kDioramaChunks) * kDioramaChunks, DioramaChunk::Unread);
}

bool DioramaGrid::tileRead(int tileX, int tileZ) const
{
    if (empty()) {
        return false;
    }
    for (int dz = 0; dz < map::kTileChunks; ++dz) {
        for (int dx = 0; dx < map::kTileChunks; ++dx) {
            const int index = chunkIndex(tileX * map::kTileChunks + dx,
                                         tileZ * map::kTileChunks + dz);
            if (chunks[usize(index)] == DioramaChunk::Unread) {
                return false;
            }
        }
    }
    return true;
}

int dioramaTableTop()
{
    return mcver::WorldGen::kSeaLevel;
}

int dioramaTableBottom()
{
    return dioramaTableTop() - kDioramaBlocks;
}

void dioramaOrigin(i32* blockX, i32* blockZ, i32 tileX, i32 tileZ)
{
    // `- 1` centres the three tiles on the one holding block 0, 0; the offset
    // is in the same units, so one step of it slides the table by a third of
    // its own width and the tile grid the bottom map draws in red stays the
    // grid the table's edges land on.
    *blockX = map::tileOriginBlock(map::tileOfBlock(0) - 1 + tileX);
    *blockZ = map::tileOriginBlock(map::tileOfBlock(0) - 1 + tileZ);
}

void foldChunk(const world::ChunkColumn& column, int chunkX, int chunkZ, DioramaGrid* grid)
{
    if (grid->empty() || chunkX < 0 || chunkZ < 0 || chunkX >= kDioramaChunks
        || chunkZ >= kDioramaChunks) {
        return;
    }
    // What a map would draw -- so torches, rails and the like are air here too.
    bool shows[mcver::kBlockTableSize];
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        shows[id] = map::showsOnMap(block::BlockId(id));
    }

    constexpr int kColumns = kDioramaCellBlocks * kDioramaCellBlocks;
    for (int cz = 0; cz < kDioramaCellsPerChunk; ++cz) {
        for (int cx = 0; cx < kDioramaCellsPerChunk; ++cx) {
            const usize base = usize(DioramaGrid::cellIndex(chunkX * kDioramaCellsPerChunk + cx, 0,
                                                            chunkZ * kDioramaCellsPerChunk + cz));
            for (int y = 0; y < kHeight; ++y) {
                // Solid if any column is; the block most of those columns have.
                u8 ids[kColumns];
                u8 counts[kColumns];
                int kinds = 0;
                for (int bz = 0; bz < kDioramaCellBlocks; ++bz) {
                    for (int bx = 0; bx < kDioramaCellBlocks; ++bx) {
                        const block::BlockId id =
                            column.block(cx * kDioramaCellBlocks + bx, y, cz * kDioramaCellBlocks + bz);
                        if (id >= mcver::kBlockTableSize || !shows[id]) {
                            continue;
                        }
                        int k = 0;
                        while (k < kinds && ids[k] != id) {
                            ++k;
                        }
                        if (k == kinds) {
                            ids[k] = u8(id);
                            counts[k] = 0;
                            ++kinds;
                        }
                        ++counts[k];
                    }
                }
                u8 best = 0;
                int bestCount = 0;
                for (int k = 0; k < kinds; ++k) {
                    if (counts[k] > bestCount) {
                        best = ids[k];
                        bestCount = counts[k];
                    }
                }
                grid->cells[base + usize(y)] = best;
                if (best != 0) {
                    continue;  // only air is ever read for its light
                }
                // The brightest of the cell's four corners, so a cell half in a
                // doorway is lit by the doorway. Four and not all sixteen: at
                // four blocks a cell light is smooth across it, and the light
                // is read for most of the volume of every chunk -- sampling
                // all sixteen doubled the whole fold on the dev host.
                u8 level = 0;
                for (int bz = 0; bz < kDioramaCellBlocks; bz += kDioramaCellBlocks - 1) {
                    for (int bx = 0; bx < kDioramaCellBlocks; bx += kDioramaCellBlocks - 1) {
                        const int lx = cx * kDioramaCellBlocks + bx;
                        const int lz = cz * kDioramaCellBlocks + bz;
                        const u8 sky = column.skyLight(lx, y, lz);
                        const u8 block = column.blockLight(lx, y, lz);
                        const u8 here = sky > block ? sky : block;
                        level = here > level ? here : level;
                    }
                }
                const usize cell = base + usize(y);
                u8& packed = grid->light[cell >> 1];
                const int shift = int(cell & 1) * 4;
                packed = u8((packed & ~(0xF << shift)) | (level << shift));
            }
        }
    }
    grid->chunks[usize(chunkIndex(chunkX, chunkZ))] = DioramaChunk::Present;
}

void openDioramaAir(DioramaGrid* grid, DioramaOpen depth)
{
    if (grid->empty()) {
        return;
    }
    const int tableTop = std::clamp(dioramaTableTop(), 0, kHeight);
    const std::vector<u8>& cells = grid->cells;
    std::vector<u32>& open = grid->open;
    std::fill(open.begin(), open.end(), 0);

    // Straight down from the sky, which is nearly all the open air there is
    // and needs no search: a column is open from the top down to its first
    // solid cell. Past the table's edge the sky reaches the table's top.
    for (int cz = 0; cz < kDioramaCells; ++cz) {
        for (int cx = 0; cx < kDioramaCells; ++cx) {
            const usize base = usize(DioramaGrid::cellIndex(cx, 0, cz));
            int y = kHeight - 1;
            for (; y >= 0 && cells[base + usize(y)] == 0; --y) {
                setOpen(&open, base + usize(y));
            }
            if (cx == 0 || cz == 0 || cx == kDioramaCells - 1 || cz == kDioramaCells - 1) {
                for (int edge = tableTop; edge <= y; ++edge) {
                    if (cells[base + usize(edge)] == 0) {
                        setOpen(&open, base + usize(edge));
                    }
                }
            }
        }
    }
    if (depth == DioramaOpen::Sky) {
        return;
    }

    // And sideways from there, wherever the air leads: under an overhang, into
    // the mouth of a cave. Air with no way out is never reached and so is
    // never drawn against.
    std::vector<u32> stack;
    const auto reach = [&](usize index) {
        if (cells[index] == 0 && !testOpen(open, index)) {
            setOpen(&open, index);
            stack.push_back(u32(index));
        }
    };
    for (int cz = 0; cz < kDioramaCells; ++cz) {
        for (int cx = 0; cx < kDioramaCells; ++cx) {
            const usize base = usize(DioramaGrid::cellIndex(cx, 0, cz));
            for (int y = 0; y < kHeight; ++y) {
                if (!testOpen(open, base + usize(y))) {
                    continue;
                }
                for (const Side& side : kSides) {
                    const int nx = cx + side.dx;
                    const int nz = cz + side.dz;
                    if (inGrid(nx, nz)) {
                        reach(usize(DioramaGrid::cellIndex(nx, y, nz)));
                    }
                }
            }
        }
    }
    while (!stack.empty()) {
        const usize index = stack.back();
        stack.pop_back();
        const int y = int(index % usize(kHeight));
        const int columnIndex = int(index / usize(kHeight));
        const int cx = columnIndex % kDioramaCells;
        const int cz = columnIndex / kDioramaCells;
        if (y + 1 < kHeight) {
            reach(index + 1);
        }
        if (y > 0) {
            reach(index - 1);
        }
        for (const Side& side : kSides) {
            const int nx = cx + side.dx;
            const int nz = cz + side.dz;
            if (inGrid(nx, nz)) {
                reach(usize(DioramaGrid::cellIndex(nx, y, nz)));
            }
        }
    }
}

namespace {

// What a tile's batch carries through WorldPeek::loadChunks' void*.
struct TileFold {
    DioramaGrid* grid = nullptr;
    i32 originChunkX = 0;
    i32 originChunkZ = 0;
    bool (*keepGoing)(void* context) = nullptr;
    void (*tileDone)(void* context, int tileX, int tileZ) = nullptr;
    void* context = nullptr;
    // Chunks of each tile the batch still owes an answer for. A tile reaching
    // zero is a tile that is finished, whatever else the batch is still doing.
    int outstanding[kDioramaTiles * kDioramaTiles] = {};
    bool skyStale = false;
    bool changed = false;
    bool stopped = false;
};

// One chunk out of the batch, in whatever order the card handed it over -- and
// a null column for one the world does not hold. The fold is per chunk and
// writes only that chunk's cells, so the order it arrives in does not reach the
// picture.
//
// **A tile is handed back the moment its last chunk lands**, not when the batch
// finishes. Reading all eight outer tiles at once is what makes the reads
// cheap, but nothing would reach the screen for the length of it otherwise --
// and on a console that is seconds of a table sitting empty.
bool foldOne(void* context, i32 chunkX, i32 chunkZ, world::ChunkColumn* column)
{
    TileFold& fold = *static_cast<TileFold*>(context);
    if (fold.keepGoing != nullptr && !fold.keepGoing(fold.context)) {
        fold.stopped = true;
        return false;
    }

    // Checked before anything is indexed by it: only chunks that were asked for
    // are ever reported, and those are all in the grid, but the marking below
    // writes into it and a wrong answer must not be what discovers that.
    const int gridX = int(chunkX - fold.originChunkX);
    const int gridZ = int(chunkZ - fold.originChunkZ);
    if (gridX < 0 || gridZ < 0 || gridX >= kDioramaChunks || gridZ >= kDioramaChunks) {
        return true;
    }

    if (column != nullptr) {
        foldChunk(*column, gridX, gridZ, fold.grid);
        fold.changed = true;
        fold.skyStale = true;
    } else {
        fold.grid->chunks[usize(chunkIndex(gridX, gridZ))] = DioramaChunk::Absent;
    }

    int& owed = fold.outstanding[(gridZ / map::kTileChunks) * kDioramaTiles
                                 + gridX / map::kTileChunks];
    if (owed > 0 && --owed == 0 && fold.tileDone != nullptr) {
        // The mesh reads `open`, so the tile has to be handed over against a
        // fresh Sky pass or it would be built against the table that was there
        // before it. This is the cadence the per-tile reads had.
        if (fold.skyStale) {
            openDioramaAir(fold.grid, DioramaOpen::Sky);
            fold.skyStale = false;
        }
        fold.tileDone(fold.context, gridX / map::kTileChunks, gridZ / map::kTileChunks);
    }
    return true;
}

}  // namespace

bool readDioramaTiles(world::WorldPeek& peek, const int* tileXs, const int* tileZs, int tiles,
                      DioramaGrid* grid, bool (*keepGoing)(void* context),
                      void (*tileDone)(void* context, int tileX, int tileZ), void* context)
{
    if (grid->empty() || tiles <= 0) {
        return true;
    }
    const i32 originChunkX = grid->originBlockX / world::ChunkColumn::kWidth
                             - (grid->originBlockX % world::ChunkColumn::kWidth < 0 ? 1 : 0);
    const i32 originChunkZ = grid->originBlockZ / world::ChunkColumn::kWidth
                             - (grid->originBlockZ % world::ChunkColumn::kWidth < 0 ? 1 : 0);

    // **Everything still owed, asked for at once**, because a chunk at a time
    // is a card operation at a time -- 576 of them for the table, against the
    // few dozen `WorldPeek::loadChunks` coalesces them into on a packed world.
    // Heap rather than stack: eight tiles is 512 chunks, and the worker this
    // runs on has 16 KB of stack on an Old 3DS.
    TileFold fold;
    fold.grid = grid;
    fold.originChunkX = originChunkX;
    fold.originChunkZ = originChunkZ;
    fold.keepGoing = keepGoing;
    fold.tileDone = tileDone;
    fold.context = context;

    std::vector<i32> xs;
    std::vector<i32> zs;
    xs.reserve(usize(tiles) * usize(map::kTileChunks * map::kTileChunks));
    zs.reserve(xs.capacity());
    for (int t = 0; t < tiles; ++t) {
        if (tileXs[t] < 0 || tileZs[t] < 0 || tileXs[t] >= kDioramaTiles
            || tileZs[t] >= kDioramaTiles
            // A tile listed twice would be counted twice and read once, so its
            // countdown would never reach zero and it would never be handed
            // over. Cheap to rule out; expensive to debug.
            || fold.outstanding[tileZs[t] * kDioramaTiles + tileXs[t]] != 0) {
            continue;
        }
        for (int dz = 0; dz < map::kTileChunks; ++dz) {
            for (int dx = 0; dx < map::kTileChunks; ++dx) {
                const int chunkX = tileXs[t] * map::kTileChunks + dx;
                const int chunkZ = tileZs[t] * map::kTileChunks + dz;
                if (grid->chunks[usize(chunkIndex(chunkX, chunkZ))] != DioramaChunk::Unread) {
                    continue;
                }
                xs.push_back(originChunkX + chunkX);
                zs.push_back(originChunkZ + chunkZ);
                ++fold.outstanding[tileZs[t] * kDioramaTiles + tileXs[t]];
            }
        }
    }
    if (xs.empty()) {
        return true;
    }
    if (keepGoing != nullptr && !keepGoing(context)) {
        return false;
    }

    peek.loadChunks(xs.data(), zs.data(), xs.size(), &fold, &foldOne);

    // **What is still Unread was never reached.** Every chunk asked for is
    // reported, so `foldOne` has already marked the absent ones; anything left
    // is what a stop -- or a read that failed outright -- did not get to, and it
    // stays Unread for the next pass rather than being written off as table.
    if (fold.skyStale) {
        openDioramaAir(grid, DioramaOpen::Sky);
    }
    return !fold.stopped;
}

bool readDioramaTile(world::WorldPeek& peek, int tileX, int tileZ, DioramaGrid* grid,
                     bool (*keepGoing)(void* context), void* context)
{
    return readDioramaTiles(peek, &tileX, &tileZ, 1, grid, keepGoing, nullptr, context);
}

void dioramaTileOrder(int tileXs[kDioramaTiles * kDioramaTiles],
                      int tileZs[kDioramaTiles * kDioramaTiles])
{
    constexpr int kOrder[kDioramaTiles * kDioramaTiles][2] = {
        {1, 1}, {1, 0}, {0, 1}, {2, 1}, {1, 2}, {0, 0}, {2, 0}, {0, 2}, {2, 2},
    };
    for (int i = 0; i < kDioramaTiles * kDioramaTiles; ++i) {
        tileXs[i] = kOrder[i][0];
        tileZs[i] = kOrder[i][1];
    }
}

void buildDioramaColours(const texture::AtlasImage& atlas, DioramaColours* out)
{
    *out = DioramaColours{};
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = block::def(block::BlockId(id));
        out->top[id] = map::averageTileColour(atlas, def.faces[mesh::kFacePosY]);
        out->side[id] = map::averageTileColour(atlas, def.faces[mesh::kFaceNegZ]);
        out->bottom[id] = map::averageTileColour(atlas, def.faces[mesh::kFaceNegY]);
    }
    const block::BlockDef& slab = block::def(block::BlockId(mcver::Block::DoubleSlab));
    out->slabTopTile = slab.faces[mesh::kFacePosY];
    out->slabSideTile = slab.faces[mesh::kFaceNegZ];
}

void buildDioramaTile(const DioramaGrid& grid, int tileX, int tileZ,
                      const DioramaColours& colours, DioramaMesh* out)
{
    if (grid.empty()) {
        return;
    }
    const int tableTop = dioramaTableTop();
    const int firstX = tileX * kDioramaCellsPerTile;
    const int firstZ = tileZ * kDioramaCellsPerTile;
    const int endX = firstX + kDioramaCellsPerTile;
    const int endZ = firstZ + kDioramaCellsPerTile;

    // The slab's top over each missing chunk, one quad a chunk.
    for (int dz = 0; dz < map::kTileChunks; ++dz) {
        for (int dx = 0; dx < map::kTileChunks; ++dx) {
            const int chunkX = tileX * map::kTileChunks + dx;
            const int chunkZ = tileZ * map::kTileChunks + dz;
            if (grid.chunks[usize(chunkIndex(chunkX, chunkZ))] == DioramaChunk::Present) {
                continue;
            }
            const int x0 = chunkX * world::ChunkColumn::kWidth;
            const int z0 = chunkZ * world::ChunkColumn::kWidth;
            const int x1 = x0 + world::ChunkColumn::kWidth;
            const int z1 = z0 + world::ChunkColumn::kWidth;
            i16 uv[4][2];
            slabTopUv(colours.slabTopTile, x0, x1, z0, z1, uv);
            flatQuad(&out->textured, mesh::kFacePosY, x0, x1, z0, z1, tableTop, 0xFFFFFF, uv,
                     kFullLight);
        }
    }

    std::vector<mesh::DetailVertex>* coloured = &out->coloured;
    const usize limit = coloured->size() + usize(kDioramaMaxTileQuads) * 4;
    const auto full = [&] { return coloured->size() >= limit; };

    // Every face below belongs to the open air in front of it, so a face is
    // written once, by the tile whose air it faces, and lit by that air's own
    // light. A run is merged only while the block **and** the light both hold,
    // which is what these pack together.
    const auto faceOf = [](u8 id, u8 light) { return u16(id) | u16(u16(light) << 8); };
    const auto idOf = [](u16 face) { return u8(face & 0xFF); };
    const auto lightOf = [](u16 face) { return u8(face >> 8); };

    // Ground: the top of whatever is under open air. The table's own top over
    // a missing chunk is the textured quad above instead. `kHeight` is the sky
    // over the top of the world, which would otherwise leave the topmost layer
    // of blocks with no top face at all -- a hole to look through.
    const auto groundAt = [&](int cx, int y, int cz) -> u16 {
        if (y >= kHeight) {
            return faceOf(grid.cell(cx, kHeight - 1, cz), 15);
        }
        if (!grid.isOpen(cx, y, cz) || (y == tableTop && !cellPresent(grid, cx, cz))) {
            return 0;
        }
        return faceOf(solidBelow(grid, cx, y, cz), grid.lightAt(cx, y, cz));
    };
    const auto tops = [&] {
        for (int y = 0; y <= kHeight; ++y) {
            for (int cz = firstZ; cz < endZ; ++cz) {
                int cx = firstX;
                while (cx < endX) {
                    const u16 face = groundAt(cx, y, cz);
                    if (idOf(face) == 0) {
                        ++cx;
                        continue;
                    }
                    int end = cx + 1;
                    while (end < endX && groundAt(end, y, cz) == face) {
                        ++end;
                    }
                    if (full()) {
                        return false;
                    }
                    flatQuad(coloured, mesh::kFacePosY, cx * kDioramaCellBlocks,
                             end * kDioramaCellBlocks, cz * kDioramaCellBlocks,
                             (cz + 1) * kDioramaCellBlocks, y, colours.top[idOf(face)], nullptr,
                             lightByte(lightOf(face)));
                    cx = end;
                }
            }
        }
        return true;
    };

    // One wall direction: the side of a neighbour facing open air, merged up
    // the column, and then the same direction along the table's outer rim.
    //
    // A face points away from the solid it belongs to, so its air cell is the
    // one the normal points into and its solid neighbour the one behind: the
    // quad sits on the boundary between them, which is the air cell's far side
    // from the normal. The rim is the other way round -- there is no air cell
    // out there to draw from, so a cell on the edge puts a face on its outward
    // boundary instead, or a hill running off the table would be open at the
    // side and show its inside. Only above the table's top: below that the
    // table's own side quad stands in front of it.
    const auto wallsFacing = [&](int face) {
        const int ndx = mesh::kFaceOffset[face].dx;
        const int ndz = mesh::kFaceOffset[face].dz;
        for (int cz = firstZ; cz < endZ; ++cz) {
            for (int cx = firstX; cx < endX; ++cx) {
                const int nx = cx - ndx;
                const int nz = cz - ndz;
                if (!inGrid(nx, nz)) {
                    continue;
                }
                const auto wallAt = [&](int y) -> u16 {
                    return grid.isOpen(cx, y, cz)
                               ? faceOf(grid.cell(nx, y, nz), grid.lightAt(cx, y, cz))
                               : u16(0);
                };
                // The boundary the face sits on, in blocks.
                const int at = (ndx != 0 ? cx : cz) * kDioramaCellBlocks
                               + ((ndx + ndz) < 0 ? kDioramaCellBlocks : 0);
                const int a0 = (ndx != 0 ? cz : cx) * kDioramaCellBlocks;
                int y = 0;
                while (y < kHeight) {
                    const u16 wall = wallAt(y);
                    if (idOf(wall) == 0) {
                        ++y;
                        continue;
                    }
                    int end = y + 1;
                    while (end < kHeight && wallAt(end) == wall) {
                        ++end;
                    }
                    if (full()) {
                        return false;
                    }
                    wallQuad(coloured, face, at, a0, a0 + kDioramaCellBlocks, y, end,
                             colours.side[idOf(wall)], nullptr, lightByte(lightOf(wall)));
                    y = end;
                }
            }
        }
        for (int cz = firstZ; cz < endZ; ++cz) {
            for (int cx = firstX; cx < endX; ++cx) {
                if (inGrid(cx + ndx, cz + ndz)) {
                    continue;  // not on the table's edge in this direction
                }
                const int at = (ndx != 0 ? cx : cz) * kDioramaCellBlocks
                               + ((ndx + ndz) > 0 ? kDioramaCellBlocks : 0);
                const int a0 = (ndx != 0 ? cz : cx) * kDioramaCellBlocks;
                int y = tableTop < 0 ? 0 : tableTop;
                while (y < kHeight) {
                    const u8 id = grid.cell(cx, y, cz);
                    if (id == 0) {
                        ++y;
                        continue;
                    }
                    int end = y + 1;
                    while (end < kHeight && grid.cell(cx, end, cz) == id) {
                        ++end;
                    }
                    if (full()) {
                        return false;
                    }
                    wallQuad(coloured, face, at, a0, a0 + kDioramaCellBlocks, y, end,
                             colours.side[id], nullptr, kFullLight);
                    y = end;
                }
            }
        }
        return true;
    };

    // Ceilings: the underside of whatever hangs over open air. Merged along x.
    // The camera never looks up at them, so they go last and the draw skips
    // them; they are built at all so that the order here is the only thing
    // that decides what is visible.
    const auto ceilingAt = [&](int cx, int y, int cz) -> u16 {
        return grid.isOpen(cx, y, cz)
                   ? faceOf(solidAbove(grid, cx, y, cz), grid.lightAt(cx, y, cz))
                   : u16(0);
    };
    const auto ceilings = [&] {
        for (int y = 0; y < kHeight; ++y) {
            for (int cz = firstZ; cz < endZ; ++cz) {
                int cx = firstX;
                while (cx < endX) {
                    const u16 face = ceilingAt(cx, y, cz);
                    if (idOf(face) == 0) {
                        ++cx;
                        continue;
                    }
                    int end = cx + 1;
                    while (end < endX && ceilingAt(end, y, cz) == face) {
                        ++end;
                    }
                    if (full()) {
                        return false;
                    }
                    flatQuad(coloured, mesh::kFaceNegY, cx * kDioramaCellBlocks,
                             end * kDioramaCellBlocks, cz * kDioramaCellBlocks,
                             (cz + 1) * kDioramaCellBlocks, y + 1, colours.bottom[idOf(face)],
                             nullptr, lightByte(lightOf(face)));
                    cx = end;
                }
            }
        }
        return true;
    };

    // Grouped by facing, in the order the draw expects, counting each group as
    // it is laid down -- including a group cut short by the vertex cap, whose
    // count is what did fit.
    for (int i = 0; i < mesh::kFaceCount; ++i) {
        const int face = kDioramaFaceOrder[i];
        const usize before = coloured->size();
        const bool whole = face == mesh::kFacePosY  ? tops()
                           : face == mesh::kFaceNegY ? ceilings()
                                                     : wallsFacing(face);
        out->run[i] = u32(coloured->size() - before);
        if (!whole) {
            return;
        }
    }
}


bool dioramaFaceVisible(int face, float yaw, float pitch)
{
    if (face < 0 || face >= mesh::kFaceCount) {
        return true;
    }
    // Towards the camera. This is the depth axis the menu's own matrix is
    // built from -- x turns the other way there, because screen x does --
    // so the two cannot disagree about which side of the table is the front.
    const float flat = std::cos(pitch);
    const float ex = -flat * std::sin(yaw);
    const float ey = std::sin(pitch);
    const float ez = flat * std::cos(yaw);
    const mesh::FaceOffset& n = mesh::kFaceOffset[face];
    return float(n.dx) * ex + float(n.dy) * ey + float(n.dz) * ez > 0.0f;
}

void buildDioramaSides(const DioramaColours& colours, DioramaMesh* out)
{
    const int tableTop = dioramaTableTop();
    const int tableBottom = dioramaTableBottom();
    const int tileU = colours.slabSideTile % mesh::kAtlasTilesPerEdge;
    const int tileV = colours.slabSideTile / mesh::kAtlasTilesPerEdge;
    const i16 u0 = mesh::tileUvMin(tileU);
    const i16 u1 = mesh::tileUvMax(tileU);
    const i16 vTop = mesh::tileUvMin(tileV);
    const i16 vBottom = mesh::tileUvMax(tileV);
    // Corner order is along the edge at the bottom, then back at the top.
    const i16 uv[4][2] = {{u0, vBottom}, {u1, vBottom}, {u1, vTop}, {u0, vTop}};

    const int n = kDioramaBlocks;
    wallQuad(&out->textured, mesh::kFaceNegZ, 0, 0, n, tableBottom, tableTop, 0xFFFFFF, uv,
             kFullLight);
    wallQuad(&out->textured, mesh::kFacePosZ, n, 0, n, tableBottom, tableTop, 0xFFFFFF, uv,
             kFullLight);
    wallQuad(&out->textured, mesh::kFaceNegX, 0, 0, n, tableBottom, tableTop, 0xFFFFFF, uv,
             kFullLight);
    wallQuad(&out->textured, mesh::kFacePosX, n, 0, n, tableBottom, tableTop, 0xFFFFFF, uv,
             kFullLight);
}

}  // namespace mc::preview
