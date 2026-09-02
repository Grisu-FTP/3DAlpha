#include "framework.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/mesher.hpp"

#include <cstring>
#include <vector>

// The geometry-shader cube format, checked against the one it is meant to
// replace.
//
// The claim the whole path rests on is that **the two formats describe the same
// geometry** -- the same corners in the same winding with the same texture
// coordinates, the same shade and the same light. Nothing on the console can
// check that: a wrong sign in kFaceBasis is a face turned inside out, which
// looks like a hole, and a hole is what an unmeshed section looks like too.
//
// So the expansion the geometry shader performs is written out once here, as
// plainly as quad.g.pica performs it, and every quad of a real-ish section is
// meshed both ways and compared vertex for vertex.

using namespace mc;
using mesh::CubeFormat;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using mesh::QuadVertex;
using mesh::WorldVertex;
using world::ChunkColumn;
using world::Section;

namespace {

constexpr world::BlockId kStone = 1;
constexpr world::BlockId kGrass = 2;
constexpr world::BlockId kGlass = 20;

MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

MeshBuilder meshOf(const ChunkColumn& column, int sectionY, CubeFormat format)
{
    scratch().fill(mesh::ColumnNeighbourhood::isolated(column), sectionY);
    MeshBuilder out;
    out.setCubeFormat(format);
    mesh::meshSection(scratch(), out);
    return out;
}

// What quad.v.pica and quad.g.pica between them produce for one corner.
//
// Deliberately written from the same three inputs the shaders get -- the quad,
// the face basis and the corner's (i, j) -- rather than from kFaceCorner, or
// this would be checking the table against itself.
WorldVertex expandCorner(const QuadVertex& q, int corner)
{
    const mesh::FaceBasis& b = mesh::kFaceBasis[q.face];
    const int i = mesh::kCornerIJ[corner][0];
    const int j = mesh::kCornerIJ[corner][1];

    WorldVertex v;
    v.x = u8(q.x + b.base[0] + i * b.e1[0] + j * b.e2[0]);
    v.y = u8(q.y + b.base[1] + i * b.e1[1] + j * b.e2[1]);
    v.z = u8(q.z + b.base[2] + i * b.e1[2] + j * b.e2[2]);
    v.face = q.face;

    // u = tileX + i tiles; v = tileY + (1 - uvSign)/2 + j*uvSign tiles -- and
    // both pulled kUvInset back off the tile boundary, which quad.v.pica does
    // with `insetP`/`insetN` for u and with uvSign * inset for v. The direction
    // has to follow which end of the tile the corner is on, or the two formats
    // would texture a block differently and only hardware would say so.
    const int vTile = (1 - b.uvSign) / 2 + j * b.uvSign;
    v.u = i16((int(q.tileX) + i) * mesh::kUvUnitsPerTile
              + (i == 0 ? mesh::kUvInset : -mesh::kUvInset));
    v.v = i16((int(q.tileY) + vTile) * mesh::kUvUnitsPerTile
              + (vTile == 0 ? mesh::kUvInset : -mesh::kUvInset));

    const u8 shade = mesh::kFaceShade[q.face];
    v.r = shade;
    v.g = shade;
    v.b = shade;
    v.light = q.light;
    return v;
}

// A section with all six face directions exposed, per-face textures in play
// (grass is grass on top and dirt underneath), and a non-opaque full cube that
// does not cull its neighbour. Meshing anything simpler would let a face table
// bug through.
ChunkColumn varied()
{
    ChunkColumn column;
    for (int x = 2; x < 8; ++x) {
        for (int z = 2; z < 8; ++z) {
            for (int y = 0; y < 4; ++y) {
                column.setBlock(x, y, z, kStone);
            }
            column.setBlock(x, 4, z, kGrass);
        }
    }
    column.setBlock(10, 6, 10, kGlass);
    column.setBlock(10, 7, 10, kGlass);
    column.setBlock(12, 3, 4, kStone);
    return column;
}

// Twice the area of the triangle, signed about the face's outward normal.
// Positive means counter-clockwise seen from outside, which is what the GPU's
// default front-face test keeps.
int signedArea(const WorldVertex& a, const WorldVertex& b, const WorldVertex& c, int face)
{
    const int e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
    const int e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
    const int cross[3] = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0],
    };
    const mesh::FaceOffset& n = mesh::kFaceOffset[face];
    return cross[0] * n.dx + cross[1] * n.dy + cross[2] * n.dz;
}

}  // namespace

TEST(a_quad_is_eight_bytes_and_a_vertex_quad_is_forty_eight)
{
    CHECK_EQ(sizeof(QuadVertex), usize(8));
    CHECK_EQ(mesh::cubeBytesPerQuad(CubeFormat::Quads), usize(8));
    CHECK_EQ(mesh::cubeBytesPerQuad(CubeFormat::Vertices), usize(48));

    // The claim in docs/3ds-performance.md section 2 is 7.5x, and it is 7.5x
    // only once the six two-byte indices each quad also costs are counted.
    const usize oldBytes = 4 * sizeof(WorldVertex) + 6 * sizeof(u16);
    CHECK_EQ(oldBytes, usize(60));
    CHECK_EQ(oldBytes / sizeof(QuadVertex), usize(7));
}

TEST(the_quad_format_expands_to_exactly_the_vertex_format)
{
    const ChunkColumn column = varied();

    const MeshBuilder vertices = meshOf(column, 0, CubeFormat::Vertices);
    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);

    CHECK(quads.quadCount() > usize(0));
    CHECK_EQ(quads.quadCount(), vertices.quadCount());

    for (usize q = 0; q < quads.quadCount(); ++q) {
        for (int c = 0; c < 4; ++c) {
            const WorldVertex got = expandCorner(quads.quads()[q], c);
            const WorldVertex& want = vertices.vertices()[q * 4 + c];

            CHECK_EQ(int(got.x), int(want.x));
            CHECK_EQ(int(got.y), int(want.y));
            CHECK_EQ(int(got.z), int(want.z));
            CHECK_EQ(int(got.face), int(want.face));
            CHECK_EQ(int(got.u), int(want.u));
            CHECK_EQ(int(got.v), int(want.v));
            CHECK_EQ(int(got.r), int(want.r));
            CHECK_EQ(int(got.g), int(want.g));
            CHECK_EQ(int(got.b), int(want.b));
            CHECK_EQ(int(got.light), int(want.light));
        }
    }
}

TEST(both_triangulations_of_a_quad_wind_the_same_way)
{
    // The 12-byte path draws a quad through the shared index buffer as
    // (0,1,2) and (0,2,3) -- split along the 0-2 diagonal. The geometry shader
    // emits a strip, 0, 1, 3, 2, which splits it along 1-3 instead. Two
    // different triangulations of the same four points, which is only the same
    // surface because a cube face is planar, and only the same *facing* if both
    // come out counter-clockwise.
    //
    // A face emitted backwards is invisible from the side it should be seen
    // from and visible from inside the block, which on hardware reads as the
    // world being inside out in patches. Worth a test of its own.
    const ChunkColumn column = varied();
    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);
    CHECK(quads.quadCount() > usize(0));

    for (usize q = 0; q < quads.quadCount(); ++q) {
        const int face = quads.quads()[q].face;
        WorldVertex c[4];
        for (int i = 0; i < 4; ++i) {
            c[i] = expandCorner(quads.quads()[q], i);
        }

        CHECK(signedArea(c[0], c[1], c[2], face) > 0);  // index buffer, first
        CHECK(signedArea(c[0], c[2], c[3], face) > 0);  // index buffer, second
        CHECK(signedArea(c[0], c[1], c[3], face) > 0);  // strip, first
        CHECK(signedArea(c[1], c[2], c[3], face) > 0);  // strip, second
    }
}

TEST(the_ranges_report_the_format_they_were_built_in)
{
    const ChunkColumn column = varied();

    const MeshBuilder vertices = meshOf(column, 0, CubeFormat::Vertices);
    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);

    // cubeQuads() is what the draw loop asks, and it has to answer the same
    // number in both formats from bytes that differ by 6x. Getting this wrong
    // is a draw call with the wrong vertex count, which is silent.
    CHECK_EQ(vertices.ranges().cubeQuads(), vertices.quadCount());
    CHECK_EQ(quads.ranges().cubeQuads(), quads.quadCount());
    CHECK(quads.ranges().cubeFormat == CubeFormat::Quads);
    CHECK(vertices.ranges().cubeFormat == CubeFormat::Vertices);

    CHECK_EQ(quads.ranges().cubeBytes * 6, vertices.ranges().cubeBytes);

    // The two detail ranges are untouched by any of this: the 16-byte format
    // carries sub-block geometry the quad format cannot express at all.
    CHECK_EQ(quads.ranges().detailBytes, vertices.ranges().detailBytes);
    CHECK_EQ(quads.ranges().translucentBytes, vertices.ranges().translucentBytes);
}

TEST(the_worst_case_section_still_fits_the_pools_largest_class)
{
    // The checkerboard, the arrangement that exposes the most faces. In the
    // 12-byte format it is 589,824 bytes and sizes the top of the size-class
    // table; in this one it is 98,304, and the point of the check is that the
    // table built for the larger still has somewhere to put the smaller.
    ChunkColumn column;
    for (int x = 0; x < Section::kSize; ++x) {
        for (int y = 0; y < Section::kSize; ++y) {
            for (int z = 0; z < Section::kSize; ++z) {
                if (((x + y + z) & 1) == 0) {
                    column.setBlock(x, y, z, kStone);
                }
            }
        }
    }

    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);
    CHECK_EQ(quads.quadCount(), usize(mesh::kMaxQuadsPerSection));
    CHECK_EQ(quads.ranges().cubeBytes, usize(mesh::kMaxQuadsPerSection) * 8);
}

TEST(copying_out_writes_the_quads_the_builder_holds)
{
    // copyTo is the only path the vertex data takes to the pool, and it has to
    // pick up the right one of the builder's two cube vectors.
    const ChunkColumn column = varied();
    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);

    std::vector<u8> buffer(quads.byteSize());
    quads.copyTo(buffer.data());

    const QuadVertex* copied = reinterpret_cast<const QuadVertex*>(buffer.data());
    for (usize q = 0; q < quads.quadCount(); ++q) {
        CHECK_EQ(int(copied[q].x), int(quads.quads()[q].x));
        CHECK_EQ(int(copied[q].y), int(quads.quads()[q].y));
        CHECK_EQ(int(copied[q].z), int(quads.quads()[q].z));
        CHECK_EQ(int(copied[q].face), int(quads.quads()[q].face));
        CHECK_EQ(int(copied[q].tileX), int(quads.quads()[q].tileX));
        CHECK_EQ(int(copied[q].tileY), int(quads.quads()[q].tileY));
        CHECK_EQ(int(copied[q].light), int(quads.quads()[q].light));
    }

    // The detail ranges still follow the cube range, at the offset the smaller
    // cube encoding puts them at rather than the one the larger did -- rounded
    // up to the range alignment, which for an even quad count is a no-op.
    CHECK_EQ(quads.ranges().detailOffset(), mesh::MeshRanges::alignUp(quads.ranges().cubeBytes));
    CHECK(quads.ranges().detailOffset() - quads.ranges().cubeBytes < 16);
}

TEST(every_range_starts_on_a_sixteen_byte_boundary_in_both_formats)
{
    // **The reason this test exists.** A vertex buffer's base is an address the
    // GPU is given, and the detail ranges start where the cube range ends. With
    // 48 bytes a quad that end is a multiple of 16 whatever the quad count; with
    // 8 bytes a quad it is not, and an odd quad count would put the detail
    // buffer's base at 8 mod 16 -- an alignment nothing in the 12-byte path can
    // produce, so nothing in the 12-byte path proves it is safe.
    //
    // Driven off a quad count made odd on purpose, because the natural meshes
    // in this file happen to be even and would pass without the padding.
    for (const CubeFormat format : {CubeFormat::Vertices, CubeFormat::Quads}) {
        for (usize quadCount = 0; quadCount < 4; ++quadCount) {
            mesh::MeshRanges ranges;
            ranges.cubeFormat = format;
            ranges.cubeBytes = quadCount * mesh::cubeBytesPerQuad(format);
            ranges.detailBytes = 3 * sizeof(mesh::DetailVertex);
            ranges.translucentBytes = 5 * sizeof(mesh::DetailVertex);

            CHECK_EQ(ranges.detailOffset() % 16, usize(0));
            CHECK_EQ(ranges.translucentOffset() % 16, usize(0));

            // The pad is layout, never geometry: the quad count still comes off
            // the unpadded length.
            CHECK_EQ(ranges.cubeQuads(), quadCount);

            // And total() covers the last range, pad included, so the staging
            // buffer meshSection sizes from it is big enough for copyTo.
            CHECK_EQ(ranges.total(), ranges.translucentOffset() + ranges.translucentBytes);
            CHECK(ranges.total() >= ranges.cubeBytes + ranges.detailBytes
                                        + ranges.translucentBytes);

            // A cube-only section -- most of a world -- is charged nothing for
            // a boundary nothing sits on, so the pool copies exactly what
            // copyTo wrote and not eight bytes more.
            mesh::MeshRanges cubesOnly = ranges;
            cubesOnly.detailBytes = 0;
            cubesOnly.translucentBytes = 0;
            CHECK_EQ(cubesOnly.total(), cubesOnly.cubeBytes);

            // Detail but no translucent still aligns the one range that exists.
            mesh::MeshRanges noTranslucent = ranges;
            noTranslucent.translucentBytes = 0;
            CHECK_EQ(noTranslucent.total(), noTranslucent.detailOffset() + ranges.detailBytes);
            CHECK_EQ(noTranslucent.detailOffset() % 16, usize(0));
        }
    }
}

TEST(copy_to_places_the_detail_ranges_at_the_padded_offsets)
{
    // The odd-quad case end to end: a single cube face is one quad, 8 bytes, so
    // the detail range is the one that has to move to 16.
    const ChunkColumn column = varied();
    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);
    const mesh::MeshRanges ranges = quads.ranges();

    std::vector<u8> buffer(quads.byteSize(), u8(0xCD));
    quads.copyTo(buffer.data());

    CHECK_EQ(buffer.size(), ranges.total());
    if (ranges.detailBytes != 0) {
        CHECK_EQ(std::memcmp(buffer.data() + ranges.detailOffset(), quads.detailVertices(),
                             ranges.detailBytes),
                 0);
    }
    if (ranges.translucentBytes != 0) {
        CHECK_EQ(std::memcmp(buffer.data() + ranges.translucentOffset(),
                             quads.translucentVertices(), ranges.translucentBytes),
                 0);
    }

    // The pad is written, not left as whatever the buffer held: the staging
    // buffer is reused between sections and all of it is uploaded.
    for (usize i = ranges.cubeBytes; i < ranges.detailOffset(); ++i) {
        CHECK_EQ(int(buffer[i]), 0);
    }
}

TEST(a_quad_never_addresses_a_cell_outside_the_section)
{
    // Positions are the block *cell*, 0..15, not the corner 0..16 the 12-byte
    // format stores -- the corner is the geometry shader's business. A cell of
    // 16 would be a quad a whole block outside its section.
    const ChunkColumn column = varied();
    const MeshBuilder quads = meshOf(column, 0, CubeFormat::Quads);

    for (usize q = 0; q < quads.quadCount(); ++q) {
        const QuadVertex& v = quads.quads()[q];
        CHECK(v.x < Section::kSize);
        CHECK(v.y < Section::kSize);
        CHECK(v.z < Section::kSize);
        CHECK(v.face < mesh::kFaceCount);
        CHECK(v.tileX < mesh::kAtlasTilesPerEdge);
        CHECK(v.tileY < mesh::kAtlasTilesPerEdge);
        CHECK_EQ(int(v.ao), 0);
    }
}
