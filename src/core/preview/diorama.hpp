#pragma once

// **The World screen's diorama**: the nine later-version map tiles around the
// world's origin, as a miniature standing on a double slab.
//
// Nine tiles is the 3x3 of red squares the bottom map draws at `128` -- 384
// blocks a side, 576 chunks -- centred on the tile holding block 0, 0. Where
// it sits is fixed here on purpose; moving it is a later system's job.
//
// The table is the slab as **one block**: a cube as tall as it is wide, its
// top at **sea level**, the height most of a world's ground sits at, so
// ordinary terrain reads as the table's surface, mountains stand up out of it
// and a ravine or a quarry cuts down into it. Each side is the slab's side
// texture once, and its top, wherever there is no world to show, is the slab's
// top texture stretched over the whole table -- so a tile nobody has walked
// into yet is its ninth of that texture rather than a hole.
//
// **A cell is four blocks across and one block tall.** Across, the table is
// about two hundred pixels on a 320-pixel screen, so a finer cell would be
// under a pixel; up, every height counts. A cell is solid at a height when any
// of its sixteen columns has something there a map would draw, and shows the
// block most of those columns have -- so an overhang, an arch or a floating
// island keeps the air under it, and peaks stay peaks.
//
// **The light is the world's own.** A chunk file carries a sky and a block
// light level per block, so a cave with torches in it is lit like one and an
// overhang shades what is under it, for the cost of a nibble a cell and the
// lightmap the world renderer already builds. A cell keeps
// `max(sky, block)` of its brightest sample, which is what
// `world::effectiveLightLevel` reduces to at noon -- and the diorama is always
// at noon.
//
// **Only air the sky can reach is drawn against.** `openDioramaAir` floods in
// from above and from past the table's edges above its top; a cave sealed
// inside the ground is never reached, so it costs no geometry. It has two
// depths because the flood is the one part of this that a 268 MHz ARM11
// notices: `Sky` is the straight-down scan alone, which every tile read runs,
// and `Full` follows the air sideways as well -- under overhangs and into cave
// mouths -- and is run once, when the whole table has been read. `Sky` marks a
// subset of what `Full` does, so a tile drawn from it is missing faces rather
// than wrong.
//
// **A face the camera cannot be looking at is never drawn.** The camera orbits
// the table at a fixed angle above the horizon and never looks up, so at any
// moment two of the four wall directions face away from it and the undersides
// of overhangs face away always. The quads are therefore laid down grouped by
// which way they face -- `kDioramaFaceOrder`, counted in `DioramaMesh::run` --
// and the draw skips the groups `dioramaFaceVisible` turns down. That is about
// a third of the table's quads at any angle, for no extra memory and no work
// on the worker. It is a draw-time test and not a build-time one on purpose:
// **which faces are hidden by other terrain is not worth computing.** Measured
// over a generated 3x3 table, of 21,434 quads only 260 -- 1.2% -- are ones the
// camera faces but can never see past the terrain in front of them, because
// `openDioramaAir` has already thrown away everything sealed inside the ground
// and a four-block cell leaves little of a cave behind.
//
// Everything here is CPU and pure: the grid is filled from chunk columns, and
// a tile of it becomes coloured and textured `DetailVertex` quads the menu
// draws with the world's own detail shader. The reading -- which chunks, in
// what order, and when to stop -- is `readDioramaTile`, so the same order runs
// on the console's worker and in the host suite.

#include "core/block/block_def.hpp"
#include "core/map/map_sample.hpp"
#include "core/map/map_store.hpp"
#include "core/mesh/vertex.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/world/chunk.hpp"
#include "core/world/world_peek.hpp"

#include "version_config.hpp"

#include <vector>

namespace mc::preview {

inline constexpr int kDioramaTiles = 3;
inline constexpr int kDioramaChunks = kDioramaTiles * map::kTileChunks;                // 24
inline constexpr int kDioramaBlocks = kDioramaChunks * world::ChunkColumn::kWidth;     // 384
inline constexpr int kDioramaCellBlocks = 4;
inline constexpr int kDioramaCells = kDioramaBlocks / kDioramaCellBlocks;             // 96
inline constexpr int kDioramaCellsPerChunk = world::ChunkColumn::kWidth / kDioramaCellBlocks;
inline constexpr int kDioramaCellsPerTile = kDioramaCells / kDioramaTiles;            // 32
inline constexpr int kDioramaHeight = world::ChunkColumn::kHeight;                    // 128

// **Positions are in 64ths of a block**, not the detail format's 1024ths: the
// table is 384 blocks across and a 16-bit position at 1024 to the block ends
// at 32. The draw folds the difference into its matrix. Centred on the table's
// middle in x and z, with y = 0 at the bottom of the world -- the table itself
// reaches further down, to `dioramaTableBottom()`.
inline constexpr int kDioramaUnitsPerBlock = 64;

// The order the coloured stream lays its facings down in: the tops first,
// which are always visible, then the four walls, then the undersides, which
// never are. `DioramaMesh::run` counts each one.
inline constexpr int kDioramaFaceOrder[mesh::kFaceCount] = {
    mesh::kFacePosY, mesh::kFaceNegZ, mesh::kFacePosZ,
    mesh::kFaceNegX, mesh::kFacePosX, mesh::kFaceNegY,
};

// What one grid costs on the heap, which is what decides how many of them the
// menu can keep. Derived rather than remembered: the cells dominate, at a byte
// a cell, with the open bit and the light nibble on top.
inline constexpr usize kDioramaGridBytes =
    usize(kDioramaCells) * usize(kDioramaCells) * usize(kDioramaHeight)          // cells
    + ((usize(kDioramaCells) * usize(kDioramaCells) * usize(kDioramaHeight) + 31) / 32) * 4  // open
    + (usize(kDioramaCells) * usize(kDioramaCells) * usize(kDioramaHeight) + 1) / 2          // light
    + usize(kDioramaChunks) * usize(kDioramaChunks);                             // chunks

// The most quads one tile's draw can index: 65,536 vertices, four to a quad.
// A tile that would need more keeps its ground and walls and loses what is
// past the limit, which is overhang undersides first.
inline constexpr int kDioramaMaxTileQuads = 0x10000 / 4;

enum class DioramaChunk : u8 {
    Unread,   // not looked at yet -- drawn as table, like Absent
    Absent,   // the world has no such chunk, or it would not decode
    Present,
};

struct DioramaGrid {
    // The block at the table's north-west corner.
    i32 originBlockX = 0;
    i32 originBlockZ = 0;

    // `[cellIndex(cx, y, cz)]`: the block a cell shows, 0 for air. A chunk that
    // is not Present holds the table -- double slab below its top, air above.
    std::vector<u8> cells;
    // A bit a cell, set for air the sky reaches; see `openDioramaAir`.
    std::vector<u32> open;
    // A nibble a cell, `max(sky, block)` light; only air cells are read.
    std::vector<u8> light;
    // `[chunkZ * kDioramaChunks + chunkX]`, relative to the origin.
    std::vector<DioramaChunk> chunks;

    // Sizes the arrays, marks every chunk Unread and lays the table in them.
    void reset(i32 blockX, i32 blockZ);

    bool empty() const { return chunks.empty(); }
    bool tileRead(int tileX, int tileZ) const;

    static int cellIndex(int cx, int y, int cz)
    {
        return (cz * kDioramaCells + cx) * kDioramaHeight + y;
    }
    u8 cell(int cx, int y, int cz) const { return cells[usize(cellIndex(cx, y, cz))]; }
    bool isOpen(int cx, int y, int cz) const
    {
        const usize i = usize(cellIndex(cx, y, cz));
        return ((open[i >> 5] >> (i & 31)) & 1u) != 0;
    }
    // 0..15. Past the top of the world it is full sky, which is what the air
    // over the table is.
    u8 lightAt(int cx, int y, int cz) const
    {
        if (y >= kDioramaHeight) {
            return 15;
        }
        const usize i = usize(cellIndex(cx, y, cz));
        return u8((light[i >> 1] >> ((i & 1) * 4)) & 0xF);
    }
};

// The height the table's top stands at: the version's sea level.
int dioramaTableTop();
// Its bottom, a whole table's width further down: the table is a cube.
int dioramaTableBottom();

// The table's north-west corner: the tile holding the anchor block, less one
// tile each way, and then `tileX`/`tileZ` map tiles further out.
//
// **The anchor is the world's spawn, not block 0, 0.** The table is 384 blocks
// a side and a1.1.2 puts spawn wherever its sand walk lands -- 341, 255 in one
// of the worlds this was measured on, 35, -511 in another -- so a table fixed
// at the origin stands next to the world rather than on it. Measured over four
// real a1.1.2 saves, the origin-centred table held 148, 188, 158 and 185 of its
// 576 chunks; anchored on spawn the same four hold 576, 576, 397 and 572. The
// ragged quarter-full tables that produced were the whole of "some worlds only
// load some chunks": every chunk the table asked for was read, and most of what
// it asked for had never been generated.
//
// **The offset is what World Settings' Move Panorama writes**, and it is
// relative to the anchor, so a world that has never been moved reads as zero.
// Both defaults are the old behaviour, which is what the tests that pass
// neither are pinning.
void dioramaOrigin(i32* blockX, i32* blockZ, i32 tileX = 0, i32 tileZ = 0,
                   i32 anchorBlockX = 0, i32 anchorBlockZ = 0);

// **The places one world offers to stand the table on**, read out of its level
// once so the menu and the worker can both answer `settings::PanoramaAnchor`
// without holding a whole `LevelData` between them.
struct DioramaAnchors {
    i32 spawnX = 0;
    i32 spawnZ = 0;
    // False for a level.dat with no Player compound -- a server-made world has
    // none -- and then `PanoramaAnchor::Player` is not an anchor this world has.
    bool hasPlayer = false;
    i32 playerX = 0;
    i32 playerZ = 0;
};

DioramaAnchors dioramaAnchorsOf(const world::LevelData& level);

// Whether this world can stand its table on `anchor`. Only Player is ever
// refused, and only for a world that has no player in it.
bool dioramaAnchorAvailable(settings::PanoramaAnchor anchor, const DioramaAnchors& anchors);

// Where `anchor` puts the table, in blocks. **An anchor the world does not have
// falls back to Spawn rather than to 0, 0, 0**: a world with no Player compound
// asked for the player's corner would otherwise put the table at the origin and
// call it "where you logged out". Returns false when it fell back, so a caller
// that is offering the choice can stop offering that one.
bool dioramaAnchorBlock(settings::PanoramaAnchor anchor, const DioramaAnchors& anchors,
                        i32* blockX, i32* blockZ);

// The next anchor in the menu's cycle that this world actually has, `step` of
// +1 or -1 from `anchor`. Spawn and Origin are always there, so this always
// terminates.
settings::PanoramaAnchor nextDioramaAnchor(settings::PanoramaAnchor anchor, int step,
                                           const DioramaAnchors& anchors);

// Folds one chunk into its cells and their light, every height of it.
// `chunkX`/`chunkZ` are relative to the origin, 0..kDioramaChunks-1. Leaves
// `open` stale.
void foldChunk(const world::ChunkColumn& column, int chunkX, int chunkZ, DioramaGrid* grid);

enum class DioramaOpen {
    Sky,   // straight down from the sky only: cheap, and always a subset
    Full,  // and sideways from there, which is what shows an overhang's underside
};

// Works out again which air the sky reaches, after cells have changed.
void openDioramaAir(DioramaGrid* grid, DioramaOpen depth);

// Reads every chunk of these tiles that is still Unread and folds what it
// finds. `keepGoing` is asked before the read and before each chunk; a false
// answer stops where it is, with what was read kept and the rest left Unread
// for the next pass. Returns false when it was stopped.
//
// **Ask for as many tiles as can be waited for at once.** The chunks go to
// `WorldPeek::loadChunks` as one batch, and what a batch saves is card
// operations, so eight tiles together cost far less than eight tiles apart.
// Measured over a real packed world's 576 chunks: 576 reads one at a time, 164
// a tile at a time, 86 as the centre tile and then the other eight.
//
// **`tileDone` is what keeps that from being a stall.** It is called the moment
// a tile's last chunk lands -- against a fresh `Sky` pass, because a mesh reads
// `open` -- rather than when the batch finishes, so the table fills in tile by
// tile while the rest of the batch is still arriving. Meshing is the caller's
// and belongs in there. Null asks for nothing and reads exactly the same.
bool readDioramaTiles(world::WorldPeek& peek, const int* tileXs, const int* tileZs, int tiles,
                      DioramaGrid* grid, bool (*keepGoing)(void* context),
                      void (*tileDone)(void* context, int tileX, int tileZ), void* context);

// One tile of the above.
bool readDioramaTile(world::WorldPeek& peek, int tileX, int tileZ, DioramaGrid* grid,
                     bool (*keepGoing)(void* context), void* context);

// Tiles in the order they are worth reading: the middle one, then its four
// neighbours, then the corners. Writes `kDioramaTiles^2` pairs.
void dioramaTileOrder(int tileXs[kDioramaTiles * kDioramaTiles],
                      int tileZs[kDioramaTiles * kDioramaTiles]);

// What the diorama is painted with, out of one pack: every block's top, side
// and bottom as an average colour, and the double slab's two tiles for the
// table.
struct DioramaColours {
    u32 top[mcver::kBlockTableSize] = {};
    u32 side[mcver::kBlockTableSize] = {};
    u32 bottom[mcver::kBlockTableSize] = {};
    u16 slabTopTile = 0;
    u16 slabSideTile = 0;
};
void buildDioramaColours(const texture::AtlasImage& atlas, DioramaColours* out);

// One tile's geometry, in two streams because it is drawn with two textures:
// `coloured` samples a white texel and carries its colour in the vertex,
// `textured` samples the pack's atlas.
struct DioramaMesh {
    std::vector<mesh::DetailVertex> coloured;
    std::vector<mesh::DetailVertex> textured;
    // Vertices in each of `coloured`'s facing groups, in `kDioramaFaceOrder`.
    // They describe what one `buildDioramaTile` laid down, so the mesh it is
    // given has to start empty -- which is how every caller uses it.
    u32 run[mesh::kFaceCount] = {};

    void clear()
    {
        coloured.clear();
        textured.clear();
        for (u32& count : run) {
            count = 0;
        }
    }
};

// Can a face pointing this way be seen by a camera orbiting at `yaw` radians,
// `pitch` radians above the horizon? The camera looks down, so a face pointing
// down is never visible whatever the yaw, and each wall direction is visible
// for half the turn.
bool dioramaFaceVisible(int face, float yaw, float pitch);

// Tile (`tileX`, `tileZ`) of the table: every face of solid ground that open
// air in the tile touches -- ground, walls and the undersides of overhangs --
// and the slab's top where chunks are not Present. Needs `open` to be current.
// Appends to `textured`; fills `coloured` grouped by facing and counts the
// groups in `run`, so the mesh must come in with an empty `coloured`.
void buildDioramaTile(const DioramaGrid& grid, int tileX, int tileZ,
                      const DioramaColours& colours, DioramaMesh* out);

// The order `buildDioramaSides` lays its four quads down in, four vertices
// each. They are the biggest quads the diorama has, so the two facing away
// are worth leaving out of the draw too.
inline constexpr int kDioramaSideOrder[4] = {
    mesh::kFaceNegZ,
    mesh::kFacePosZ,
    mesh::kFaceNegX,
    mesh::kFacePosX,
};

// The table's four sides, from its bottom to its top, each one the slab's side
// texture once, in `kDioramaSideOrder`. Appends to `textured`.
void buildDioramaSides(const DioramaColours& colours, DioramaMesh* out);

}  // namespace mc::preview
