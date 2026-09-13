// Greedy meshing and the cube atlas it samples.
//
// **The claim everything here rests on is that merging changes the quad count
// and nothing else.** A merged quad has to draw exactly the faces it replaces:
// the same cells, the same face direction, the same tile and the same light,
// with no face dropped, doubled or moved. Nothing on the console could show a
// violation reliably -- a missing face looks like a hole, a doubled one looks
// like nothing at all, and a tile off by a slot looks like a texture pack --
// so the property is checked here on the mesh itself: every greedy mesh is
// taken apart into unit faces and compared, as a multiset, with the one-quad-
// per-face mesh of the same section.

#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/mesher.hpp"

#include <algorithm>
#include <tuple>
#include <vector>

using namespace mc;
using mesh::CubeFormat;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using mesh::WorldVertex;
using world::ChunkColumn;
using world::Section;

namespace {

constexpr u16 kStone = u16(mcver::Block::Stone);
constexpr u16 kDirt = u16(mcver::Block::Dirt);
constexpr u16 kGrass = u16(mcver::Block::Grass);
constexpr u16 kCobble = u16(mcver::Block::Cobblestone);
constexpr u16 kGlass = u16(mcver::Block::Glass);
constexpr u16 kLeaves = u16(mcver::Block::Leaves);
constexpr u16 kFurnace = u16(mcver::Block::Furnace);

MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

MeshBuilder meshOf(const ChunkColumn& column, int sectionY, bool greedy,
                   CubeFormat format = CubeFormat::Vertices)
{
    scratch().fill(mesh::ColumnNeighbourhood::isolated(column), sectionY);
    MeshBuilder out;
    out.setCubeFormat(format);
    out.setGreedy(greedy);
    mesh::meshSection(scratch(), out);
    return out;
}

// One unit face as the renderer will draw it: which cell, which side, which
// cube-atlas slot and which light.
struct UnitFace {
    int x, y, z, face, slot, light;

    bool operator<(const UnitFace& o) const
    {
        return std::tie(x, y, z, face, slot, light)
               < std::tie(o.x, o.y, o.z, o.face, o.slot, o.light);
    }
    bool operator==(const UnitFace& o) const
    {
        return std::tie(x, y, z, face, slot, light)
               == std::tie(o.x, o.y, o.z, o.face, o.slot, o.light);
    }
};

int dot(const int (&a)[3], const i8 (&b)[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// A 12-byte quad's run, read back off its four corners alone -- not off
// anything the mesher was asked -- and checked for being a well-formed quad
// of the cube format on the way. Appends its unit faces to `out`.
//
// void, and the run through a pointer, because CHECK returns from the function
// it fails in.
struct Run {
    int face = -1;
    int width = 0;
    int height = 0;
};

void takeApart(const WorldVertex* v, std::vector<UnitFace>& out, Run* run = nullptr)
{
    const int face = mesh::faceOf(v[0]);
    const mesh::FaceBasis& b = mesh::kFaceBasis[face];

    const int c0[3] = {v[0].x, v[0].y, v[0].z};
    const int d1[3] = {v[1].x - c0[0], v[1].y - c0[1], v[1].z - c0[2]};
    const int d3[3] = {v[3].x - c0[0], v[3].y - c0[1], v[3].z - c0[2]};
    const int w = dot(d1, b.e1);
    const int h = dot(d3, b.e2);
    CHECK(w >= 1 && w <= mesh::kCubeRepeat);
    CHECK(h >= 1 && h <= mesh::kCubeRepeat);

    // The corners are exactly base + i*w*e1 + j*h*e2 off the first cell, which
    // is also what pins the winding to kFaceCorner's.
    const int first[3] = {c0[0] - b.base[0], c0[1] - b.base[1], c0[2] - b.base[2]};
    for (int c = 0; c < 4; ++c) {
        const int i = mesh::kCornerIJ[c][0];
        const int j = mesh::kCornerIJ[c][1];
        CHECK_EQ(int(v[c].x), first[0] + b.base[0] + i * w * b.e1[0] + j * h * b.e2[0]);
        CHECK_EQ(int(v[c].y), first[1] + b.base[1] + i * w * b.e1[1] + j * h * b.e2[1]);
        CHECK_EQ(int(v[c].z), first[2] + b.base[2] + i * w * b.e1[2] + j * h * b.e2[2]);
    }

    // Every corner is on the same face and says whether the quad is merged,
    // and a merged quad's corners each carry their own index.
    const bool merged = w * h > 1;
    for (int c = 0; c < 4; ++c) {
        CHECK_EQ(int(v[c].seam), int(mesh::seamOf(face, merged, c)));
    }

    // The UVs cover exactly w by h copies of one tile from its slot's start,
    // in by the inset at both ends -- a run that sampled one tile stretched,
    // or strayed into the next slot, fails here.
    int minU = v[0].u, maxU = v[0].u, minV = v[0].v, maxV = v[0].v;
    for (int c = 1; c < 4; ++c) {
        minU = std::min(minU, int(v[c].u));
        maxU = std::max(maxU, int(v[c].u));
        minV = std::min(minV, int(v[c].v));
        maxV = std::max(maxV, int(v[c].v));
    }
    CHECK_EQ(minU % mesh::kCubeUvPerSlot, mesh::kCubeUvGutter + mesh::kCubeUvInset);
    CHECK_EQ(minV % mesh::kCubeUvPerSlot, mesh::kCubeUvGutter + mesh::kCubeUvInset);
    CHECK_EQ(maxU - minU, w * mesh::kCubeUvPerTile - 2 * mesh::kCubeUvInset);
    CHECK_EQ(maxV - minV, h * mesh::kCubeUvPerTile - 2 * mesh::kCubeUvInset);
    const int slot = (minV / mesh::kCubeUvPerSlot) * mesh::kCubeSlotsPerEdge
                     + minU / mesh::kCubeUvPerSlot;
    CHECK(slot < mesh::kCubeAtlas.slotCount);

    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const int x = first[0] + i * b.e1[0] + j * b.e2[0];
            const int y = first[1] + i * b.e1[1] + j * b.e2[1];
            const int z = first[2] + i * b.e1[2] + j * b.e2[2];
            CHECK(x >= 0 && x < Section::kSize);
            CHECK(y >= 0 && y < Section::kSize);
            CHECK(z >= 0 && z < Section::kSize);
            out.push_back({x, y, z, face, slot, int(v[0].light)});
        }
    }
    if (run != nullptr) {
        *run = {face, w, h};
    }
}

std::vector<UnitFace> unitFaces(const MeshBuilder& m)
{
    std::vector<UnitFace> faces;
    for (usize q = 0; q < m.quadCount(); ++q) {
        takeApart(m.vertices() + q * 4, faces);
    }
    std::sort(faces.begin(), faces.end());
    return faces;
}

// A small deterministic generator, so a failure names a seed rather than a
// mood.
struct Lcg {
    u32 state;
    u32 next()
    {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    int below(int n) { return int(next() % u32(n)); }
};

// Terrain-shaped, because terrain is what the merge is for: layers of stone
// under dirt under grass at uneven heights, with the things that break a run
// scattered through it -- other tiles, see-through cubes that keep their
// neighbours' faces, a furnace facing its metadata, an unknown id, and light
// that changes from cell to cell.
ChunkColumn terrain(u32 seed)
{
    Lcg rng{seed};
    ChunkColumn column;
    for (int x = 0; x < Section::kSize; ++x) {
        for (int z = 0; z < Section::kSize; ++z) {
            const int height = 5 + rng.below(4) + (x / 5);
            for (int y = 0; y < height && y < Section::kSize; ++y) {
                u16 id = y + 1 == height ? kGrass : (y + 3 >= height ? kDirt : kStone);
                const int roll = rng.below(40);
                if (roll == 0) id = kCobble;
                if (roll == 1) id = kGlass;
                if (roll == 2) id = kLeaves;
                if (roll == 3) id = 200;
                column.setBlock(x, y, z, id);
                if (roll == 4) {
                    column.setBlock(x, y, z, kFurnace);
                    column.setBlockData(x, y, z, u8(2 + rng.below(4)));
                }
            }
            for (int y = height; y < Section::kSize; ++y) {
                column.setSkyLight(x, y, z, 15);
            }
            if (rng.below(6) == 0) {
                column.setBlockLight(x, height < 15 ? height : 15, z, u8(rng.below(16)));
            }
        }
    }
    return column;
}

// And the opposite: noise, where almost nothing merges and every rule gets
// exercised at a run of one.
ChunkColumn noise(u32 seed)
{
    Lcg rng{seed};
    const u16 palette[] = {0, 0, 0, kStone, kStone, kDirt, kGlass, kCobble};
    ChunkColumn column;
    for (int x = 0; x < Section::kSize; ++x) {
        for (int y = 0; y < Section::kSize; ++y) {
            for (int z = 0; z < Section::kSize; ++z) {
                column.setBlock(x, y, z, palette[rng.below(8)]);
                column.setSkyLight(x, y, z, u8(rng.below(2) * 15));
            }
        }
    }
    return column;
}

void checkSameFaces(const ChunkColumn& column)
{
    const MeshBuilder flat = meshOf(column, 0, false);
    const MeshBuilder greedy = meshOf(column, 0, true);

    const std::vector<UnitFace> want = unitFaces(flat);
    const std::vector<UnitFace> got = unitFaces(greedy);
    CHECK_EQ(got.size(), want.size());
    CHECK(got == want);
    CHECK(greedy.quadCount() <= flat.quadCount());

    // The two detail streams are not touched by any of this.
    CHECK_EQ(greedy.detailQuadCount(), flat.detailQuadCount());
    CHECK_EQ(greedy.translucentQuadCount(), flat.translucentQuadCount());
}

}  // namespace

TEST(greedy_meshing_draws_exactly_the_faces_the_flat_mesh_does)
{
    for (u32 seed = 1; seed <= 24; ++seed) {
        checkSameFaces(terrain(seed));
        checkSameFaces(noise(seed));
    }
}

TEST(greedy_meshing_actually_merges_terrain)
{
    // The property above holds for a mesher that merges nothing, so it needs a
    // floor under it. A plain -- grass over dirt over stone, one hill in it --
    // is mostly grass tops and stone walls; if greedy meshing does not cut it
    // to a quarter, it is not running. (The real 1119-column world measured
    // 2.14x fewer cube quads; see docs/status.md.)
    ChunkColumn column;
    for (int x = 0; x < 16; ++x) {
        for (int z = 0; z < 16; ++z) {
            const bool hill = x >= 5 && x < 9 && z >= 6 && z < 9;
            const int height = hill ? 9 : 6;
            for (int y = 0; y < height; ++y) {
                column.setBlock(x, y, z, y + 1 == height ? kGrass : (y + 3 >= height ? kDirt
                                                                                    : kStone));
            }
            for (int y = height; y < 16; ++y) {
                column.setSkyLight(x, y, z, 15);
            }
        }
    }
    const usize flat = meshOf(column, 0, false).quadCount();
    const usize greedy = meshOf(column, 0, true).quadCount();
    CHECK(greedy * 4 < flat);
    checkSameFaces(column);
}

TEST(a_solid_sections_walls_merge_into_runs_no_longer_than_the_repeat)
{
    // Each 16x16 wall is five runs of three and one of one along each edge --
    // not one quad, because the cube atlas holds three copies of a tile along
    // each edge and a longer run would sample off the end of its slot.
    ChunkColumn column;
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                column.setBlock(x, y, z, kStone);
            }
        }
    }

    const MeshBuilder m = meshOf(column, 0, true);
    constexpr int kRunsPerEdge = (16 + mesh::kCubeRepeat - 1) / mesh::kCubeRepeat;
    CHECK_EQ(m.quadCount(), usize(6 * kRunsPerEdge * kRunsPerEdge));

    std::vector<UnitFace> faces;
    int full = 0;
    for (usize q = 0; q < m.quadCount(); ++q) {
        Run run;
        takeApart(m.vertices() + q * 4, faces, &run);
        full += run.width == mesh::kCubeRepeat && run.height == mesh::kCubeRepeat ? 1 : 0;
    }
    CHECK_EQ(faces.size(), usize(6 * 16 * 16));
    CHECK_EQ(full, 6 * (16 / mesh::kCubeRepeat) * (16 / mesh::kCubeRepeat));
}

TEST(a_run_stops_at_the_cube_atlas_repeat)
{
    // A single row of sixteen: its top is five runs of three and one of one,
    // and so are its bottom and its two long sides.
    ChunkColumn column;
    for (int x = 0; x < 16; ++x) {
        column.setBlock(x, 3, 7, kStone);
    }

    const MeshBuilder m = meshOf(column, 0, true);
    int tops = 0;
    std::vector<UnitFace> faces;
    for (usize q = 0; q < m.quadCount(); ++q) {
        Run run;
        takeApart(m.vertices() + q * 4, faces, &run);
        CHECK(run.width <= mesh::kCubeRepeat && run.height <= mesh::kCubeRepeat);
        if (run.face == mesh::kFacePosY) {
            ++tops;
            CHECK(run.width * run.height <= mesh::kCubeRepeat);
        }
    }
    constexpr int kRuns = (16 + mesh::kCubeRepeat - 1) / mesh::kCubeRepeat;
    CHECK_EQ(tops, kRuns);
    // Top, bottom and two long sides of that many quads each, plus the two ends.
    CHECK_EQ(m.quadCount(), usize(4 * kRuns + 2));
    CHECK_EQ(faces.size(), usize(4 * 16 + 2));
}

TEST(light_or_tile_that_differs_keeps_faces_apart)
{
    // The two things a cube quad carries per face, and so the two things a
    // merge must never average away.
    ChunkColumn same;
    same.setBlock(4, 4, 4, kStone);
    same.setBlock(5, 4, 4, kStone);

    ChunkColumn lit;
    lit.setBlock(4, 4, 4, kStone);
    lit.setBlock(5, 4, 4, kStone);
    lit.setBlockLight(5, 5, 4, 9);  // the air above the second block only

    ChunkColumn tiled;
    tiled.setBlock(4, 4, 4, kStone);
    tiled.setBlock(5, 4, 4, kCobble);

    const auto tops = [](const MeshBuilder& m) {
        int n = 0;
        for (usize q = 0; q < m.quadCount(); ++q) {
            n += mesh::faceOf(m.vertices()[q * 4]) == mesh::kFacePosY ? 1 : 0;
        }
        return n;
    };

    CHECK_EQ(tops(meshOf(same, 0, true)), 1);
    CHECK_EQ(tops(meshOf(lit, 0, true)), 2);
    CHECK_EQ(tops(meshOf(tiled, 0, true)), 2);
    checkSameFaces(lit);
    checkSameFaces(tiled);
}

TEST(faces_in_different_planes_never_merge)
{
    // A step: two tops, one block apart in height. Same tile, same light,
    // neighbours in x -- and still two quads, because they are not coplanar.
    ChunkColumn column;
    column.setBlock(4, 4, 4, kStone);
    column.setBlock(5, 5, 4, kStone);
    column.setBlock(5, 4, 4, kStone);

    const MeshBuilder m = meshOf(column, 0, true);
    int tops = 0;
    for (usize q = 0; q < m.quadCount(); ++q) {
        tops += mesh::faceOf(m.vertices()[q * 4]) == mesh::kFacePosY ? 1 : 0;
    }
    CHECK_EQ(tops, 2);
    checkSameFaces(column);
}

TEST(merged_quads_wind_counter_clockwise_seen_from_outside)
{
    // The same test the single faces have, on runs -- where a sign error in
    // the grid's flipped axes would turn a whole merged wall inside out while
    // every single face stayed right.
    const MeshBuilder m = meshOf(terrain(3), 0, true);
    CHECK(m.quadCount() > 0);
    for (usize q = 0; q < m.quadCount(); ++q) {
        const WorldVertex* v = m.vertices() + q * 4;
        const int face = mesh::faceOf(v[0]);
        const int a[3] = {v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z};
        const int b[3] = {v[2].x - v[0].x, v[2].y - v[0].y, v[2].z - v[0].z};
        const int n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                          a[0] * b[1] - a[1] * b[0]};
        const mesh::FaceOffset& want = mesh::kFaceOffset[face];
        CHECK(n[0] * want.dx + n[1] * want.dy + n[2] * want.dz > 0);
    }
}

TEST(a_seam_grows_a_merged_quad_outward_at_every_corner)
{
    // The shader moves each corner of a merged quad by seamDirection times a
    // fraction of a pixel. Whatever the fraction, that has to make the quad
    // bigger on all four sides: a corner pulled inward would open exactly the
    // crack the seam is there to close.
    for (int face = 0; face < mesh::kFaceCount; ++face) {
        CHECK_EQ(int(mesh::seamDirection(mesh::seamOf(face, false, 0)).x), 0);
        CHECK_EQ(int(mesh::seamDirection(mesh::seamOf(face, false, 0)).y), 0);
        CHECK_EQ(int(mesh::seamDirection(mesh::seamOf(face, false, 0)).z), 0);

        const mesh::FaceBasis& b = mesh::kFaceBasis[face];
        for (int c = 0; c < 4; ++c) {
            const mesh::SeamDirection d = mesh::seamDirection(mesh::seamOf(face, true, c));
            const int dir[3] = {d.x, d.y, d.z};
            // Out along e1 at i = 1 and back along it at i = 0; the same for e2.
            const int along1 = dot(dir, b.e1);
            const int along2 = dot(dir, b.e2);
            CHECK_EQ(along1, mesh::kCornerIJ[c][0] != 0 ? 1 : -1);
            CHECK_EQ(along2, mesh::kCornerIJ[c][1] != 0 ? 1 : -1);
        }
    }
    CHECK_EQ(mesh::kSeamTableSize, 30);
    CHECK_EQ(int(mesh::seamOf(mesh::kFacePosX, true, 3)) % mesh::kFaceCount,
             int(mesh::kFacePosX));
}

TEST(the_builder_is_clean_after_a_flush)
{
    // The face grid is cleared by the merge consuming it, not by a memset per
    // section -- so a second section meshed with the same builder has to come
    // out exactly as it does from a fresh one.
    const ChunkColumn first = terrain(11);
    const ChunkColumn second = noise(12);

    MeshBuilder reused;
    scratch().fill(mesh::ColumnNeighbourhood::isolated(first), 0);
    mesh::meshSection(scratch(), reused);
    reused.clear();
    scratch().fill(mesh::ColumnNeighbourhood::isolated(second), 0);
    mesh::meshSection(scratch(), reused);

    const MeshBuilder fresh = meshOf(second, 0, true);
    CHECK_EQ(reused.quadCount(), fresh.quadCount());
    CHECK(unitFaces(reused) == unitFaces(fresh));

    // And faces collected but never flushed are thrown away by clear(), not
    // carried into the next section.
    MeshBuilder abandoned;
    abandoned.addFace(1, 2, 3, mesh::kFacePosY, 1, 0xF0);
    abandoned.clear();
    abandoned.flushFaces();
    CHECK_EQ(abandoned.quadCount(), usize(0));
}

TEST(the_checkerboard_worst_case_holds_with_greedy_meshing)
{
    // No two exposed faces of a checkerboard share a plane edge to edge, so
    // nothing merges and the index-buffer bound is still met exactly.
    ChunkColumn column;
    for (int x = 0; x < 16; ++x) {
        for (int y = 0; y < 16; ++y) {
            for (int z = 0; z < 16; ++z) {
                if (((x + y + z) & 1) == 0) {
                    column.setBlock(x, y, z, kStone);
                }
            }
        }
    }
    CHECK_EQ(meshOf(column, 0, true).quadCount(), usize(mesh::kMaxQuadsPerSection));
    CHECK_EQ(meshOf(column, 0, true, CubeFormat::Quads).quadCount(),
             usize(mesh::kMaxQuadsPerSection));
}

TEST(both_cube_formats_merge_the_same_runs)
{
    for (u32 seed = 30; seed < 36; ++seed) {
        const ChunkColumn column = terrain(seed);
        const MeshBuilder vertices = meshOf(column, 0, true, CubeFormat::Vertices);
        const MeshBuilder quads = meshOf(column, 0, true, CubeFormat::Quads);
        CHECK_EQ(quads.quadCount(), vertices.quadCount());
        for (usize q = 0; q < quads.quadCount(); ++q) {
            std::vector<UnitFace> ignored;
            Run run;
            takeApart(vertices.vertices() + q * 4, ignored, &run);
            CHECK_EQ(mesh::extentWidth(quads.quads()[q].extent), run.width);
            CHECK_EQ(mesh::extentHeight(quads.quads()[q].extent), run.height);
        }
    }
}

// ---------------------------------------------------------------------------
// The cube atlas
// ---------------------------------------------------------------------------

TEST(every_tile_a_cube_block_can_show_has_a_cube_atlas_slot)
{
    // A tile with no slot would be drawn from tile 0's -- a stone-textured
    // furnace mouth, which looks like a texture pack's choice and not a bug.
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        if (mcver::kBlocks[id].render != block::RenderType::Cube) {
            continue;
        }
        for (int metadata = 0; metadata < 16; ++metadata) {
            const u16* faces = block::worldFaces(block::BlockId(id), u8(metadata));
            for (int face = 0; face < mesh::kFaceCount; ++face) {
                CHECK(faces[face] < mesh::kAtlasTileCount);
                CHECK(mesh::kCubeAtlas.slotOfTile[faces[face]] != mesh::kNoCubeSlot);
            }
        }
    }
    for (int face = 0; face < mesh::kFaceCount; ++face) {
        CHECK(mesh::kCubeAtlas.slotOfTile[mcver::kUnknownBlock.faces[face]] != mesh::kNoCubeSlot);
    }

    // a1.1.2 needs 58 of the 64: 54 named by the block table, measured when the
    // atlas was designed, plus the four halves of a double chest's picture,
    // which are a rule's and appear in no `faces` row (core/block/world_texture
    // .hpp -- without a slot they sampled tile 0 and wore grass). Pinned, so a
    // regenerated table that quietly grows toward the limit says so.
    CHECK_EQ(mesh::kCubeAtlas.slotCount, 58);

    // The two tables are each other's inverse, and slots go to tiles in
    // ascending order so the layout is stable from build to build.
    for (int slot = 0; slot < mesh::kCubeAtlas.slotCount; ++slot) {
        const int tile = mesh::kCubeAtlas.tileOfSlot[slot];
        CHECK_EQ(int(mesh::kCubeAtlas.slotOfTile[tile]), slot);
        if (slot > 0) {
            CHECK(tile > mesh::kCubeAtlas.tileOfSlot[slot - 1]);
        }
    }

    // A texture past the atlas lands where the mesher sends it: tile 0.
    CHECK_EQ(mesh::cubeSlotOf(9999), mesh::cubeSlotOf(0));
}

TEST(the_cube_atlas_holds_each_tile_three_times_over_inside_a_gutter)
{
    for (int slot = 0; slot < mesh::kCubeAtlas.slotCount; ++slot) {
        const int tile = mesh::kCubeAtlas.tileOfSlot[slot];
        const int x0 = (slot % mesh::kCubeSlotsPerEdge) * mesh::kCubeSlotPixels;
        const int y0 = (slot / mesh::kCubeSlotsPerEdge) * mesh::kCubeSlotPixels;
        for (int y = 0; y < mesh::kCubeSlotPixels; y += 3) {
            for (int x = 0; x < mesh::kCubeSlotPixels; x += 1) {
                int ax = -1;
                int ay = -1;
                CHECK(mesh::cubeAtlasSource(x0 + x, y0 + y, &ax, &ay));
                CHECK_EQ(ax, (tile % mesh::kAtlasTilesPerEdge) * 16 + mesh::cubeSlotTexel(x));
                CHECK_EQ(ay, (tile / mesh::kAtlasTilesPerEdge) * 16 + mesh::cubeSlotTexel(y));
            }
        }
    }

    // The mapping along one axis, spelled out: the near gutter is the tile's
    // first texel, the copies are the tile over and over, and the far gutter is
    // its last texel. So a sample that strays off either end of a slot's copies
    // reads the texel at that edge of the *same* tile -- never the next slot's,
    // which is what showed as red TNT texels on the corners of grass on
    // hardware.
    for (int offset = 0; offset < mesh::kCubeSlotPixels; ++offset) {
        const int inCopies = offset - mesh::kCubeGutterPixels;
        int want = inCopies % 16;
        if (inCopies < 0) want = 0;
        if (inCopies >= mesh::kCubeRepeat * 16) want = 15;
        CHECK_EQ(mesh::cubeSlotTexel(offset), want);
    }
    CHECK_EQ(mesh::kCubeGutterPixels * 2 + mesh::kCubeRepeat * 16, mesh::kCubeSlotPixels);

    // And every run's UVs stay inside the copies, a whole gutter clear of the
    // slot on both sides.
    for (int slotAxis = 0; slotAxis < mesh::kCubeSlotsPerEdge; ++slotAxis) {
        const int slotStart = slotAxis * mesh::kCubeUvPerSlot;
        CHECK_EQ(mesh::cubeUvStart(slotAxis) - slotStart, mesh::kCubeUvGutter + mesh::kCubeUvInset);
        CHECK_EQ(slotStart + mesh::kCubeUvPerSlot - mesh::cubeUvEnd(slotAxis, mesh::kCubeRepeat),
                 mesh::kCubeUvGutter + mesh::kCubeUvInset);
    }

    // The slots nobody claimed are left out of the upload altogether.
    int ax = 0;
    int ay = 0;
    CHECK(!mesh::cubeAtlasSource(mesh::kCubeAtlasEdge - 1, mesh::kCubeAtlasEdge - 1, &ax, &ay));
}
