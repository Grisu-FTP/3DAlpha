#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/torch.hpp"

#include <cmath>
#include <set>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::DetailVertex;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using mesh::TorchMount;
using world::ChunkColumn;

namespace {

constexpr u16 kTorch = u16(mcver::Block::Torch);
constexpr u16 kRedstoneTorch = u16(mcver::Block::RedstoneTorch);
constexpr u16 kUnlitRedstoneTorch = u16(mcver::Block::UnlitRedstoneTorch);
constexpr u16 kStone = u16(mcver::Block::Stone);

// 23 KB, so the cases share one rather than putting it on the stack each time.
MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

// Well away from the section's own edges, so the shell is never the thing
// under test. The one case that *is* about the edge places its own block.
constexpr int kX = 8;
constexpr int kY = 8;
constexpr int kZ = 8;

MeshScratch& worldWith(u16 id, u8 metadata, u8 blockLight = 0, u8 skyLight = 0)
{
    static ChunkColumn column;
    column = ChunkColumn();
    column.setBlock(kX, kY, kZ, id);
    column.setBlockData(kX, kY, kZ, metadata);
    column.setBlockLight(kX, kY, kZ, blockLight);
    column.setSkyLight(kX, kY, kZ, skyLight);
    scratch().fill(ColumnNeighbourhood::isolated(column), 0);
    return scratch();
}

MeshBuilder meshOne(MeshScratch& s, int x = kX, int y = kY, int z = kZ)
{
    MeshBuilder out;
    mesh::addTorch(s, x, y, z, block::def(s.block(x, y, z)), out);
    return out;
}

const DetailVertex* quad(const MeshBuilder& m, usize q) { return m.detailVertices() + q * 4; }

// Detail units back to blocks, relative to the block the torch is in. Every
// expectation below is written in the original's own units.
float blocksX(const DetailVertex& v) { return float(v.x) / mesh::kDetailUnitsPerBlock - kX; }
float blocksY(const DetailVertex& v) { return float(v.y) / mesh::kDetailUnitsPerBlock - kY; }
float blocksZ(const DetailVertex& v) { return float(v.z) / mesh::kDetailUnitsPerBlock - kZ; }

// A whole detail unit is 1/1024 of a block and detailPos rounds, so an
// expectation that is exact in blocks can be half a unit out here.
bool near(float a, float b) { return std::fabs(a - b) < 1.0f / 1024.0f; }

struct Vec {
    float x, y, z;
};

// The quad's geometric normal, from the first two edges -- the same test the
// cube corner table was checked with. Only the dominant axis is asserted on,
// because a sheared side quad is genuinely tilted.
Vec normalOf(const DetailVertex* q)
{
    const Vec a{blocksX(q[1]) - blocksX(q[0]), blocksY(q[1]) - blocksY(q[0]),
               blocksZ(q[1]) - blocksZ(q[0])};
    const Vec b{blocksX(q[2]) - blocksX(q[1]), blocksY(q[2]) - blocksY(q[1]),
               blocksZ(q[2]) - blocksZ(q[1])};
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// The quad whose stored face index is `face`. addTorch emits exactly one of
// each, which is itself worth relying on.
const DetailVertex* byFace(const MeshBuilder& m, int face)
{
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        if (quad(m, q)->face == face) {
            return quad(m, q);
        }
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The mount, `bc.b(ly,III)`
// ---------------------------------------------------------------------------

TEST(torch_mount_puts_the_stick_base_on_the_wall)
{
    // The check that catches a sign error, and the reason the tilt reads
    // backwards at first glance: the base offset and the tilt point the *same*
    // way, and their sum is exactly half a block. So the bottom of the stick
    // lands on the block face it hangs from rather than floating in the middle
    // or sinking into the wall.
    for (int meta = 1; meta <= 4; ++meta) {
        const TorchMount m = mesh::torchMount(meta);
        const float baseX = 0.5f + m.ox + m.dx;
        const float baseZ = 0.5f + m.oz + m.dz;

        // One axis is pinned to a block face, the other stays centred.
        const bool alongX = meta == 1 || meta == 2;
        CHECK(near(alongX ? baseZ : baseX, 0.5f));
        CHECK(near(alongX ? baseX : baseZ, meta == 1 || meta == 3 ? 0.0f : 1.0f));

        // And every wall torch is lifted, which is what makes it sit above the
        // floor of the block it is in.
        CHECK(near(m.oy, 0.2f));
    }
}

TEST(torch_mount_stands_upright_for_every_other_metadata)
{
    // The original's switch has no case for 0 or 5 -- both fall through to the
    // untilted call, and so does anything else a corrupt world might hold.
    for (int meta : {0, 5, 6, 15}) {
        const TorchMount m = mesh::torchMount(meta);
        CHECK(near(m.ox, 0.0f));
        CHECK(near(m.oy, 0.0f));
        CHECK(near(m.oz, 0.0f));
        CHECK(near(m.dx, 0.0f));
        CHECK(near(m.dz, 0.0f));
    }
}

// ---------------------------------------------------------------------------
// The geometry, `bc.a(ly,DDDDD)`
// ---------------------------------------------------------------------------

TEST(torch_is_five_quads_in_the_opaque_detail_stream)
{
    MeshBuilder m = meshOne(worldWith(kTorch, 0));

    // Four sides and a cap. There is no bottom face: the original never wrote
    // one, and a torch is not seen from below in normal play.
    CHECK_EQ(m.detailQuadCount(), usize(5));

    // Nothing in the 12-byte cube stream -- a torch is not a cube -- and
    // nothing in the translucent one, since it is cut out by the alpha test in
    // the opaque pass exactly as the crossed squares are.
    CHECK_EQ(m.quadCount(), usize(0));
    CHECK_EQ(m.translucentQuadCount(), usize(0));

    // One quad per face, and the cap is the only upward one.
    std::set<int> faces;
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        faces.insert(quad(m, q)->face);
    }
    CHECK_EQ(faces.size(), usize(5));
    CHECK(faces.count(mesh::kFacePosY) == 1);
    CHECK(faces.count(mesh::kFaceNegY) == 0);
}

TEST(torch_side_quads_are_full_block_and_wound_outward)
{
    MeshBuilder m = meshOne(worldWith(kTorch, 0));

    // Each side spans the whole block in its horizontal axis and the whole
    // block in height. It looks like a stick only because the tile is
    // transparent around one -- which is why this render type needs the alpha
    // test, and why a solid placeholder tile draws a slab.
    const DetailVertex* negX = byFace(m, mesh::kFaceNegX);
    CHECK(negX != nullptr);
    CHECK(near(blocksZ(negX[0]), 0.0f));
    CHECK(near(blocksZ(negX[3]), 1.0f));
    CHECK(near(blocksY(negX[0]), 1.0f));
    CHECK(near(blocksY(negX[1]), 0.0f));

    // And the four sit at +-1/16 of the centre, which is the stick's width.
    CHECK(near(blocksX(negX[0]), 0.5f - 1.0f / 16.0f));
    CHECK(near(blocksX(byFace(m, mesh::kFacePosX)[0]), 0.5f + 1.0f / 16.0f));
    CHECK(near(blocksZ(byFace(m, mesh::kFacePosZ)[0]), 0.5f + 1.0f / 16.0f));
    CHECK(near(blocksZ(byFace(m, mesh::kFaceNegZ)[0]), 0.5f - 1.0f / 16.0f));

    // Every quad faces out of the stick, so two of the four are culled from
    // any viewpoint and the torch is never seen from inside. A single corner
    // in the wrong order makes one side vanish from one direction only.
    CHECK(normalOf(byFace(m, mesh::kFaceNegX)).x < 0.0f);
    CHECK(normalOf(byFace(m, mesh::kFacePosX)).x > 0.0f);
    CHECK(normalOf(byFace(m, mesh::kFacePosZ)).z > 0.0f);
    CHECK(normalOf(byFace(m, mesh::kFaceNegZ)).z < 0.0f);
    CHECK(normalOf(byFace(m, mesh::kFacePosY)).y > 0.0f);
}

TEST(torch_cap_sits_at_five_eighths_and_is_an_eighth_across)
{
    MeshBuilder m = meshOne(worldWith(kTorch, 0));
    const DetailVertex* cap = byFace(m, mesh::kFacePosY);

    for (int i = 0; i < 4; ++i) {
        CHECK(near(blocksY(cap[i]), 0.625f));
    }
    CHECK(near(blocksX(cap[0]), 0.5f - 1.0f / 16.0f));
    CHECK(near(blocksX(cap[2]), 0.5f + 1.0f / 16.0f));
    CHECK(near(blocksZ(cap[0]), 0.5f - 1.0f / 16.0f));
    CHECK(near(blocksZ(cap[1]), 0.5f + 1.0f / 16.0f));
}

TEST(torch_lean_is_a_shear_with_the_bottom_displaced)
{
    // Metadata 1 hangs on the -X wall. The bottom edge moves and the top edge
    // does not, which is the opposite of a rotation and the thing most likely
    // to be transcribed upside down.
    MeshBuilder m = meshOne(worldWith(kTorch, 1));
    const DetailVertex* negX = byFace(m, mesh::kFaceNegX);

    // Top edge: the centre is shifted by the mount alone, 0.5 - 0.1.
    CHECK(near(blocksX(negX[0]), 0.4f - 1.0f / 16.0f));
    CHECK(near(blocksX(negX[3]), 0.4f - 1.0f / 16.0f));

    // Bottom edge: a further -0.4, putting the stick's base on the wall.
    CHECK(near(blocksX(negX[1]), 0.0f - 1.0f / 16.0f));
    CHECK(near(blocksX(negX[2]), 0.0f - 1.0f / 16.0f));

    // Lifted by the 0.2 rise, so the quad spans 0.2 to 1.2 rather than 0 to 1.
    CHECK(near(blocksY(negX[0]), 1.2f));
    CHECK(near(blocksY(negX[1]), 0.2f));
}

TEST(torch_cap_lies_on_the_line_the_sides_are_sheared_along)
{
    // The cap is placed by its own expression, `dx * (1 - 0.625)`, and the
    // sides by theirs. They agree only if the shear was transcribed the right
    // way up, so this is the cross-check between the two halves of `bc.a`.
    MeshBuilder m = meshOne(worldWith(kTorch, 1));
    const DetailVertex* cap = byFace(m, mesh::kFacePosY);

    const float capCentreX = (blocksX(cap[0]) + blocksX(cap[2])) * 0.5f;

    // 0.4 base, leaning -0.4 * 0.375 by the time it reaches 0.625 up.
    CHECK(near(capCentreX, 0.4f - 0.4f * 0.375f));
    CHECK(near(blocksY(cap[0]), 0.2f + 0.625f));

    // Interpolating the side quad's own edges to the cap's height must land in
    // the same place.
    const DetailVertex* negX = byFace(m, mesh::kFaceNegX);
    const float t = (0.625f + 0.2f - blocksY(negX[1])) / (blocksY(negX[0]) - blocksY(negX[1]));
    const float sheared = blocksX(negX[1]) + t * (blocksX(negX[0]) - blocksX(negX[1]));
    CHECK(near(sheared + 1.0f / 16.0f, capCentreX));
}

TEST(torch_at_a_section_edge_leans_out_of_it)
{
    // A wall torch's geometry genuinely leaves the block, and at x = 0 it
    // leaves the section. The detail format stores position signed for exactly
    // this reason; the cube format could not express it at all.
    static ChunkColumn column;
    column = ChunkColumn();
    column.setBlock(0, kY, kZ, kTorch);
    column.setBlockData(0, kY, kZ, 1);
    scratch().fill(ColumnNeighbourhood::isolated(column), 0);

    MeshBuilder m = meshOne(scratch(), 0, kY, kZ);
    CHECK_EQ(m.detailQuadCount(), usize(5));

    bool sawNegative = false;
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        for (int i = 0; i < 4; ++i) {
            if (quad(m, q)[i].x < 0) {
                sawNegative = true;
            }
        }
    }
    CHECK(sawNegative);
}

// ---------------------------------------------------------------------------
// Texture and light
// ---------------------------------------------------------------------------

TEST(torch_cap_samples_the_two_texel_square_the_flame_sits_on)
{
    MeshBuilder m = meshOne(worldWith(kTorch, 0));
    const DetailVertex* cap = byFace(m, mesh::kFacePosY);

    // Texels 7..9 across and 6..8 down of the tile, which the original writes
    // as 0.02734375 and 0.03515625 over a 256-pixel atlas.
    constexpr int kTexel = mesh::kUvUnitsPerTile / 16;
    const int tile = block::def(kTorch).faces[0];
    const int originU = (tile & 15) << 4;
    const int originV = tile & 240;

    CHECK_EQ(int(cap[0].u), (originU + 7) * kTexel);
    CHECK_EQ(int(cap[2].u), (originU + 9) * kTexel);
    CHECK_EQ(int(cap[0].v), (originV + 6) * kTexel);
    CHECK_EQ(int(cap[1].v), (originV + 8) * kTexel);
}

TEST(torch_sides_take_the_whole_tile)
{
    MeshBuilder m = meshOne(worldWith(kTorch, 0));
    const DetailVertex* negX = byFace(m, mesh::kFaceNegX);

    const int tile = block::def(kTorch).faces[0];
    const int originU = (tile & 15) << 4;
    constexpr int kTexel = mesh::kUvUnitsPerTile / 16;

    CHECK_EQ(int(negX[0].u), originU * kTexel + mesh::kUvInset);
    CHECK_EQ(int(negX[2].u), (originU + 16) * kTexel - mesh::kUvInset);

    // v runs downward, so the tile's top row is the quad's top edge.
    CHECK(negX[0].v < negX[1].v);
}

TEST(torch_reads_face_zero_and_not_the_blocks_bare_texture)
{
    // The two redstone torches carry a different tile on their top face. The
    // torch renderer calls getBlockTextureFromSide(0), so picking the block's
    // `texture` or its face 1 would put the wrong tile on the whole model --
    // and for the plain torch, where every face is the same, the mistake would
    // be invisible.
    CHECK(block::def(kRedstoneTorch).faces[1] != block::def(kRedstoneTorch).faces[0]);

    MeshBuilder m = meshOne(worldWith(kRedstoneTorch, 0));
    const DetailVertex* negX = byFace(m, mesh::kFaceNegX);

    const int tile = block::def(kRedstoneTorch).faces[0];
    constexpr int kTexel = mesh::kUvUnitsPerTile / 16;
    CHECK_EQ(int(negX[0].u), ((tile & 15) << 4) * kTexel + mesh::kUvInset);
    CHECK_EQ(int(negX[0].v), (tile & 240) * kTexel + mesh::kUvInset);
}

TEST(a_light_emitting_torch_is_drawn_full_bright_regardless_of_its_cell)
{
    // `if (ly.t[blockID] > 0) brightness = 1.0f`. A lit torch stores light 14,
    // so reading the cell instead would draw every torch in the game at 0.93
    // of full -- close enough to look plausible and wrong everywhere.
    CHECK_EQ(int(block::def(kTorch).light), 14);

    MeshScratch& s = worldWith(kTorch, 0, /*blockLight=*/14, /*skyLight=*/0);
    CHECK_EQ(int(mesh::torchLight(s, kX, kY, kZ, block::def(kTorch))), (15 << 4) | 15);

    // Even in a cave with nothing else around it.
    MeshScratch& dark = worldWith(kTorch, 0, 0, 0);
    CHECK_EQ(int(mesh::torchLight(dark, kX, kY, kZ, block::def(kTorch))), (15 << 4) | 15);

    MeshBuilder m = meshOne(dark);
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(int(quad(m, q)[i].light), (15 << 4) | 15);
        }
    }
}

TEST(an_unlit_redstone_torch_reads_its_cell_like_anything_else)
{
    // It emits nothing, so the override does not apply and it is lit by its
    // surroundings -- which is what makes it read as dead rather than as a
    // torch that has stopped glowing.
    CHECK_EQ(int(block::def(kUnlitRedstoneTorch).light), 0);

    MeshScratch& s = worldWith(kUnlitRedstoneTorch, 0, /*blockLight=*/3, /*skyLight=*/9);
    CHECK_EQ(int(mesh::torchLight(s, kX, kY, kZ, block::def(kUnlitRedstoneTorch))),
             (9 << 4) | 3);
}

TEST(torch_geometry_is_unshaded)
{
    // `bc.b` calls setColorOpaque_F once and never consults the per-face shade
    // table, so all five quads carry the same colour -- unlike a cube, whose
    // top and bottom differ by a factor of two.
    MeshBuilder m = meshOne(worldWith(kTorch, 0));
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        for (int i = 0; i < 4; ++i) {
            CHECK_EQ(int(quad(m, q)[i].r), 255);
            CHECK_EQ(int(quad(m, q)[i].g), 255);
            CHECK_EQ(int(quad(m, q)[i].b), 255);
        }
    }
}

// ---------------------------------------------------------------------------
// The dispatch
// ---------------------------------------------------------------------------

TEST(mesh_section_emits_torches_through_the_detail_stream)
{
    // The emitter existing is not the same as the mesher reaching it: before
    // this, every render type without one fell through to `continue` and
    // produced nothing at all.
    static ChunkColumn column;
    column = ChunkColumn();
    column.setBlock(kX, kY - 1, kZ, kStone);
    column.setBlock(kX, kY, kZ, kTorch);
    column.setBlockData(kX, kY, kZ, 0);
    scratch().fill(ColumnNeighbourhood::isolated(column), 0);

    MeshBuilder out;
    mesh::meshSection(scratch(), out);

    CHECK_EQ(out.detailQuadCount(), usize(5));

    // The stone underneath is still a cube, so the two streams coexist.
    CHECK(out.quadCount() > 0);
}
