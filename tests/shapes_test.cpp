// The ten render types that used to draw nothing.
//
// **There is no oracle for any of this.** The original's geometry comes out of
// `RenderBlocks` straight into a tessellator, and there is no method to ask
// "what shape is this block" the way `addCollisionBoxesToList` answers for
// collision -- which is exactly why four of these shapes borrow the collision
// table instead of inventing one. So what is checked here is what can be
// checked without one: that each type draws *something*, that it stays inside
// its own cell, that the shapes with a known box really do use that box, and
// that the two states which change a tile change it in the right direction.

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/mesher.hpp"
#include "core/mesh/shapes.hpp"
#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

using namespace mc;
using mesh::ColumnNeighbourhood;
using mesh::DetailVertex;
using mesh::MeshBuilder;
using mesh::MeshScratch;
using world::ChunkColumn;

namespace {

MeshScratch& scratch()
{
    static MeshScratch instance;
    return instance;
}

MeshBuilder meshOf(const ChunkColumn& column, int sectionY)
{
    scratch().fill(ColumnNeighbourhood::isolated(column), sectionY);
    MeshBuilder out;
    mesh::meshSection(scratch(), out);
    return out;
}

// `floorId` puts a block directly underneath, which only fire cares about:
// `bc.d` picks its whole shape from what is under and around the cell, and a
// flame with nothing solid below and nothing flammable beside draws no quads
// at all -- in the original as well as here. Every other shape ignores it.
MeshBuilder meshOne(u16 id, u8 metadata, int bx = 8, int by = 8, int bz = 8, u16 floorId = 0)
{
    ChunkColumn column;
    column.setBlock(bx, by, bz, id);
    column.setBlockData(bx, by, bz, metadata);
    if (floorId != 0) {
        column.setBlock(bx, by - 1, bz, floorId);
    }
    return meshOf(column, 0);
}

// What a shape has to be stood on to draw. Only fire answers with anything.
u16 floorFor(block::RenderType render)
{
    return render == block::RenderType::Fire ? u16(mcver::Block::Stone) : u16(0);
}

double local(i16 stored, int block)
{
    return double(stored - block * mesh::kDetailUnitsPerBlock)
           / double(mesh::kDetailUnitsPerBlock);
}

// The first block this version defines with the given render type.
int firstOfType(block::RenderType render)
{
    for (int id = 1; id < mcver::kBlockTableSize; ++id) {
        if (mcver::kBlocks[id].known && mcver::kBlocks[id].render == render) {
            return id;
        }
    }
    return -1;
}

constexpr block::RenderType kNewTypes[] = {
    block::RenderType::Stairs, block::RenderType::Door,         block::RenderType::Ladder,
    block::RenderType::Cactus, block::RenderType::Fence,        block::RenderType::Crops,
    block::RenderType::Rail,   block::RenderType::RedstoneWire, block::RenderType::Lever,
    block::RenderType::Fire,
};

}  // namespace

TEST(every_new_shape_draws_something)
{
    for (block::RenderType render : kNewTypes) {
        CHECK(mesh::shapeHasEmitter(render));
        CHECK(mesh::hasEmitter(render));

        const int id = firstOfType(render);
        CHECK(id > 0);

        // A ladder only exists at metadata 2..5, so the question is whether any
        // state draws -- the same widening mesher_test.cpp makes.
        usize best = 0;
        for (u8 metadata = 0; metadata < 16; ++metadata) {
            const MeshBuilder m = meshOne(u16(id), metadata, 8, 8, 8, floorFor(render));
            const usize quads = m.quadCount() + m.detailQuadCount() + m.translucentQuadCount();
            best = quads > best ? quads : best;
        }
        CHECK(best > 0);
    }
}

TEST(no_new_shape_leaves_its_own_cell)
{
    // The failure this catches is a shape whose geometry is written at the
    // section origin instead of at the block, which draws correctly for the one
    // block at (0,0,0) and nowhere else.
    for (block::RenderType render : kNewTypes) {
        const int id = firstOfType(render);
        CHECK(id > 0);
        // **Fire is a block and a half tall and that is the original's.**
        // `float f1 = 1.4F` in `bc.d`, plus the 0.0625 a wall flame is lifted
        // by, so a flame legitimately stands 1.4625 blocks above its own cell.
        // Horizontally it is still inside it, and that is where the bug this
        // test catches would show -- a shape written at the section origin is
        // wrong on every axis at once.
        const double ceiling = render == block::RenderType::Fire ? 1.4625 : 1.0;
        for (u8 metadata = 0; metadata < 16; ++metadata) {
            const MeshBuilder m = meshOne(u16(id), metadata, 5, 6, 7, floorFor(render));
            for (usize v = 0; v < m.detailQuadCount() * 4; ++v) {
                const DetailVertex& d = m.detailVertices()[v];
                // A fence's rails reach the cell edge and fire hugs the walls,
                // so the bound is the cell itself rather than anything tighter.
                CHECK(local(d.x, 5) >= -1e-9);
                CHECK(local(d.x, 5) <= 1.0 + 1e-9);
                CHECK(local(d.y, 6) >= -0.0626);
                CHECK(local(d.y, 6) <= ceiling + 1e-9);
                CHECK(local(d.z, 7) >= -1e-9);
                CHECK(local(d.z, 7) <= 1.0 + 1e-9);
            }
        }
    }
}

TEST(stairs_are_the_two_boxes_they_collide_as)
{
    const int id = firstOfType(block::RenderType::Stairs);
    CHECK(id > 0);

    AABB boxes[block::kMaxCollisionBoxes];
    const int count =
        block::collisionBoxes(block::BlockId(id), 0, boxes, block::kMaxCollisionBoxes);
    CHECK_EQ(count, 2);

    // Six faces each, and the geometry has to reach both boxes' extremes --
    // a stair drawn as one box would miss the step entirely.
    const MeshBuilder m = meshOne(u16(id), 0);
    CHECK_EQ(m.detailQuadCount(), usize(12));

    double lowestTop = 1.0;
    double highestTop = 0.0;
    for (usize v = 0; v < m.detailQuadCount() * 4; ++v) {
        const double y = local(m.detailVertices()[v].y, 8);
        lowestTop = y < lowestTop ? y : lowestTop;
        highestTop = y > highestTop ? y : highestTop;
    }
    CHECK(lowestTop <= 1e-9);
    CHECK(highestTop >= 1.0 - 1e-9);
}

TEST(a_fence_reaches_towards_a_neighbour_and_not_towards_air)
{
    const int id = firstOfType(block::RenderType::Fence);
    CHECK(id > 0);

    const MeshBuilder alone = meshOne(u16(id), 0);

    ChunkColumn joined;
    joined.setBlock(8, 8, 8, u16(id));
    joined.setBlock(9, 8, 8, u16(id));
    const MeshBuilder pair = meshOf(joined, 0);

    // A lone post is a post. Two fences side by side grow two rails each, so
    // the pair has strictly more geometry than twice a lone one would.
    CHECK(alone.detailQuadCount() > 0);
    CHECK(pair.detailQuadCount() > alone.detailQuadCount() * 2);
}

TEST(a_fence_reaches_towards_a_solid_block_too)
{
    const int id = firstOfType(block::RenderType::Fence);
    const MeshBuilder alone = meshOne(u16(id), 0);

    ChunkColumn againstStone;
    againstStone.setBlock(8, 8, 8, u16(id));
    againstStone.setBlock(9, 8, 8, u16(mcver::Block::Stone));
    const MeshBuilder joined = meshOf(againstStone, 0);

    // The stone's own six faces are in there too, so the comparison is on the
    // detail stream, which only the fence writes to.
    CHECK(joined.detailQuadCount() > alone.detailQuadCount());
}

namespace {

// Which atlas tile a detail vertex is sampling, from its u and v.
int tileOf(const mesh::DetailVertex& v)
{
    const int col = int(v.u) / mesh::kUvUnitsPerTile;
    const int row = int(v.v) / mesh::kUvUnitsPerTile;
    return row * mesh::kAtlasTilesPerEdge + col;
}

// The set of tiles a mesh's detail stream reads from.
std::set<int> tilesIn(const MeshBuilder& m)
{
    std::set<int> tiles;
    for (usize i = 0; i < m.detailQuadCount() * 4; ++i) {
        tiles.insert(tileOf(m.detailVertices()[i]));
    }
    return tiles;
}

}  // namespace

TEST(a_lone_wire_is_a_crossing_and_a_run_is_a_line)
{
    const int id = firstOfType(block::RenderType::RedstoneWire);
    CHECK(id > 0);
    const int cross = int(mcver::kBlocks[id].texture);

    // Nothing around it: the crossing tile, at full width, and that is the dot
    // a single dust makes.
    const MeshBuilder alone = meshOne(u16(id), 0);
    CHECK_EQ(int(tilesIn(alone).size()), 1);
    CHECK_EQ(*tilesIn(alone).begin(), cross);

    // Three in a row along x: the middle one is a straight run and draws the
    // *line* tile, which is the next column along.
    ChunkColumn run;
    for (int dx = -1; dx <= 1; ++dx) {
        run.setBlock(8 + dx, 8, 8, u16(id));
    }
    scratch().fill(ColumnNeighbourhood::isolated(run), 0);
    MeshBuilder line;
    mesh::addShape(scratch(), 8, 8, 8, block::BlockId(id), 0, line);
    CHECK_EQ(int(tilesIn(line).size()), 1);
    CHECK_EQ(*tilesIn(line).begin(), cross + 1);
}

TEST(an_unconnected_arm_of_a_crossing_is_cut_off)
{
    const int id = firstOfType(block::RenderType::RedstoneWire);

    // A corner -- wire to the west and to the north -- is still the crossing
    // tile, but the two arms that lead nowhere are pulled back 5/16 of a block.
    ChunkColumn corner;
    corner.setBlock(8, 8, 8, u16(id));
    corner.setBlock(7, 8, 8, u16(id));
    corner.setBlock(8, 8, 7, u16(id));
    scratch().fill(ColumnNeighbourhood::isolated(corner), 0);
    MeshBuilder m;
    mesh::addShape(scratch(), 8, 8, 8, block::BlockId(id), 0, m);

    CHECK_EQ(int(tilesIn(m).size()), 1);
    CHECK_EQ(*tilesIn(m).begin(), int(mcver::kBlocks[id].texture));

    double maxX = 0.0;
    double maxZ = 0.0;
    for (usize i = 0; i < m.detailQuadCount() * 4; ++i) {
        const double px = local(m.detailVertices()[i].x, 8);
        const double pz = local(m.detailVertices()[i].z, 8);
        maxX = px > maxX ? px : maxX;
        maxZ = pz > maxZ ? pz : maxZ;
    }
    // 1 - 0.3125, the trim in the class file.
    CHECK(maxX < 0.6876);
    CHECK(maxX > 0.6874);
    CHECK(maxZ < 0.6876);
    CHECK(maxZ > 0.6874);
}

TEST(a_powered_wire_is_drawn_from_the_row_below)
{
    const int id = firstOfType(block::RenderType::RedstoneWire);
    const int cross = int(mcver::kBlocks[id].texture);

    // `BlockRedstoneWire.getBlockTextureFromSideAndMetadata` is
    // `blockIndexInTexture + (metadata > 0 ? 16 : 0)`, which is one row down in
    // terrain.png -- the lit dust. This is the whole of the "active glow": the
    // colour a1.1.2 passes is brightness on all three channels and nothing
    // more.
    for (u8 power = 1; power <= 15; ++power) {
        const MeshBuilder m = meshOne(u16(id), power);
        CHECK_EQ(int(tilesIn(m).size()), 1);
        CHECK_EQ(*tilesIn(m).begin(), cross + mesh::kAtlasTilesPerEdge);
    }
}

TEST(a_wire_climbs_a_neighbour_that_has_wire_on_top_of_it)
{
    const int id = firstOfType(block::RenderType::RedstoneWire);

    ChunkColumn flat;
    flat.setBlock(8, 8, 8, u16(id));
    flat.setBlock(7, 8, 8, u16(mcver::Block::Stone));
    scratch().fill(ColumnNeighbourhood::isolated(flat), 0);
    MeshBuilder without;
    mesh::addShape(scratch(), 8, 8, 8, block::BlockId(id), 0, without);

    ChunkColumn stepped;
    stepped.setBlock(8, 8, 8, u16(id));
    stepped.setBlock(7, 8, 8, u16(mcver::Block::Stone));
    stepped.setBlock(7, 9, 8, u16(id));
    scratch().fill(ColumnNeighbourhood::isolated(stepped), 0);
    MeshBuilder with;
    mesh::addShape(scratch(), 8, 8, 8, block::BlockId(id), 0, with);

    // One more sheet -- two quads, because a sheet is drawn from both sides --
    // and it stands up rather than lying down.
    CHECK_EQ(with.detailQuadCount(), without.detailQuadCount() + 2);

    double top = 0.0;
    for (usize i = 0; i < with.detailQuadCount() * 4; ++i) {
        const double py = local(with.detailVertices()[i].y, 8);
        top = py > top ? py : top;
    }
    CHECK(top > 0.99);
}

TEST(a_levers_handle_moves_when_it_is_thrown_and_stays_on_its_own_tile)
{
    const int id = firstOfType(block::RenderType::Lever);
    CHECK(id > 0);

    // A floor lever, off and then on. The handle swings through 40 degrees
    // either side of upright, so no corner of one can coincide with the other.
    const MeshBuilder off = meshOne(u16(id), 5);
    const MeshBuilder on = meshOne(u16(id), 5 | 8);
    CHECK_EQ(off.detailQuadCount(), on.detailQuadCount());
    CHECK(off.detailQuadCount() > 0);

    bool moved = false;
    for (usize i = 0; i < off.detailQuadCount() * 4; ++i) {
        moved = moved || off.detailVertices()[i].z != on.detailVertices()[i].z;
    }
    CHECK(moved);

    // **The handle reads a 2x10 strip of its own tile, not the whole of it.**
    // The whole tile is what `addBox` gave it, and the top two thirds of the
    // lever tile are transparent -- which is what "broken at the top" was.
    const int tile = int(mcver::kBlocks[id].texture);
    const i16 uLow = i16((tile % mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile
                         + 7 * mesh::kUvUnitsPerTexel);
    const i16 uHigh = i16((tile % mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile
                          + 9 * mesh::kUvUnitsPerTexel);
    const i16 vLow = i16((tile / mesh::kAtlasTilesPerEdge) * mesh::kUvUnitsPerTile
                         + 6 * mesh::kUvUnitsPerTexel);
    bool sawHandle = false;
    for (usize i = 0; i < off.detailQuadCount() * 4; ++i) {
        const mesh::DetailVertex& d = off.detailVertices()[i];
        if (tileOf(d) != tile) {
            continue;  // the cobblestone base plate
        }
        sawHandle = true;
        CHECK(d.u >= uLow);
        CHECK(d.u <= uHigh);
        CHECK(d.v >= vLow);
    }
    CHECK(sawHandle);
}

TEST(a_floor_lever_lies_the_other_way_round_at_orientation_six)
{
    const int id = firstOfType(block::RenderType::Lever);
    // 5 and 6 are the same lever a quarter turn apart -- the pair
    // `BlockLever.onBlockAdded` picks between -- so they must not mesh alike.
    const MeshBuilder five = meshOne(u16(id), 5);
    const MeshBuilder six = meshOne(u16(id), 6);
    CHECK_EQ(five.detailQuadCount(), six.detailQuadCount());

    bool differs = false;
    for (usize i = 0; i < five.detailQuadCount() * 4; ++i) {
        differs = differs || five.detailVertices()[i].x != six.detailVertices()[i].x;
    }
    CHECK(differs);
}

TEST(crops_show_a_later_tile_as_they_grow)
{
    const int id = firstOfType(block::RenderType::Crops);
    CHECK(id > 0);

    // `BlockCrops.getBlockTextureFromSideAndMetadata` is
    // `blockIndexInTexture + metadata`, so the eight stages are eight
    // consecutive tiles and the u coordinate has to march right.
    i16 previous = -1;
    for (u8 stage = 0; stage < 8; ++stage) {
        const MeshBuilder m = meshOne(u16(id), stage);
        CHECK(m.detailQuadCount() > 0);
        const i16 u = m.detailVertices()[0].u;
        if (previous >= 0) {
            CHECK(u > previous);
        }
        previous = u;
    }
}

// `BlockDoor.getBlockTexture(face, metadata)` for every metadata, one letter
// per face in face order, as the running a1.1.2 jar answers it for both the
// wooden door and the iron door (identical but for the base tile). `L` is the
// block's own tile and `U` the one a row up; lower case is a negative answer,
// which `renderBlockDoor` draws with u reversed.
constexpr const char* kDoorFaces[16] = {
    "LLLLLl", "LLLlLL", "LLLLlL", "LLlLLL", "LLlLLL", "LLLLLl", "LLLlLL", "LLLLlL",
    "LLLLUu", "LLUuLL", "LLLLuU", "LLuULL", "LLuULL", "LLLLUu", "LLUuLL", "LLLLuU",
};

// A door's one box emits its six faces in face order, four vertices each, and
// `kFaceCornerUV` gives corner 0 the low u end and corner 1 the high one on
// every face -- so a face is mirrored exactly when those two come out the other
// way round.
struct DrawnFace {
    int tile;
    bool mirrored;
};

DrawnFace drawnFace(const MeshBuilder& m, int face)
{
    const DetailVertex* v = m.detailVertices() + face * 4;
    i16 u = v[0].u;
    i16 vv = v[0].v;
    for (int c = 1; c < 4; ++c) {
        u = std::min(u, v[c].u);
        vv = std::min(vv, v[c].v);
    }
    const int column = u / mesh::kUvUnitsPerTile;
    const int row = vv / mesh::kUvUnitsPerTile;
    return DrawnFace{row * mesh::kAtlasTilesPerEdge + column, v[0].u > v[1].u};
}

TEST(a_door_draws_each_face_with_the_tile_and_mirror_the_jar_answers)
{
    int doors = 0;
    for (int id = 1; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (!def.known || def.render != block::RenderType::Door) {
            continue;
        }
        ++doors;
        for (u8 metadata = 0; metadata < 16; ++metadata) {
            const MeshBuilder m = meshOne(u16(id), metadata);
            CHECK_EQ(int(m.detailQuadCount()), 6);
            for (int face = 0; face < mesh::kFaceCount; ++face) {
                const char want = kDoorFaces[metadata][face];
                const bool upperTile = want == 'U' || want == 'u';
                const DrawnFace got = drawnFace(m, face);
                CHECK_EQ(got.tile, int(def.texture) - (upperTile ? mesh::kAtlasTilesPerEdge : 0));
                CHECK_EQ(got.mirrored, want == 'l' || want == 'u');
            }
        }
    }
    CHECK_EQ(doors, 2);
}

// The report this was fixed for, stated directly: the second door of a pair is
// placed with the facing a quarter back and bit 2 set, which is the *same box*
// as a plain closed door -- so the hinge side is carried by nothing but which
// broad face is mirrored. Facing 0 and its flipped placement, ((0-1)&3)+4 = 7.
TEST(the_second_door_of_a_pair_is_mirrored_the_other_way)
{
    const int id = firstOfType(block::RenderType::Door);
    CHECK(id > 0);

    AABB plainBox[block::kMaxCollisionBoxes];
    AABB pairedBox[block::kMaxCollisionBoxes];
    CHECK_EQ(block::collisionBoxes(block::BlockId(id), 0, plainBox, block::kMaxCollisionBoxes), 1);
    CHECK_EQ(block::collisionBoxes(block::BlockId(id), 7, pairedBox, block::kMaxCollisionBoxes), 1);
    CHECK_EQ(plainBox[0].minX, pairedBox[0].minX);
    CHECK_EQ(plainBox[0].maxX, pairedBox[0].maxX);
    CHECK_EQ(plainBox[0].minZ, pairedBox[0].minZ);
    CHECK_EQ(plainBox[0].maxZ, pairedBox[0].maxZ);

    const MeshBuilder plain = meshOne(u16(id), 0);
    const MeshBuilder paired = meshOne(u16(id), 7);
    CHECK(drawnFace(plain, mesh::kFacePosX).mirrored);
    CHECK(!drawnFace(plain, mesh::kFaceNegX).mirrored);
    CHECK(!drawnFace(paired, mesh::kFacePosX).mirrored);
    CHECK(drawnFace(paired, mesh::kFaceNegX).mirrored);
}

TEST(a_ladder_draws_only_where_the_game_puts_one)
{
    const int id = firstOfType(block::RenderType::Ladder);
    CHECK(id > 0);

    // 2..5 are the four walls; anything else is a state the game never writes,
    // and `renderBlockLadder`'s if-chain falls off the end for those and draws
    // nothing. Drawing the collision fallback instead would put a solid cube of
    // ladder texture in the world.
    for (u8 metadata = 0; metadata < 16; ++metadata) {
        const MeshBuilder m = meshOne(u16(id), metadata);
        const bool onAWall = metadata >= 2 && metadata <= 5;
        CHECK_EQ(m.detailQuadCount() > 0, onAWall);
    }
}

TEST(a_cactus_is_inset_at_the_sides_and_full_at_the_caps)
{
    const int id = firstOfType(block::RenderType::Cactus);
    CHECK(id > 0);

    const MeshBuilder m = meshOne(u16(id), 0);
    CHECK(m.detailQuadCount() > 0);

    bool sawInset = false;
    bool sawFullWidth = false;
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        const DetailVertex* quad = m.detailVertices() + q * 4;
        for (int c = 0; c < 4; ++c) {
            const double x = local(quad[c].x, 8);
            sawInset = sawInset || (x > 1e-9 && x < 1.0 - 1e-9);
            sawFullWidth = sawFullWidth || x <= 1e-9 || x >= 1.0 - 1e-9;
        }
    }
    // Both, which is the whole point: the caps fill the cell and the sides do
    // not, and that gap is what makes a column of cactus read as segments.
    CHECK(sawInset);
    CHECK(sawFullWidth);
}

// The spikes. `bc.b(ly,IIIFFF)` draws each side as the standard full-cell face
// under `setTranslation` of a sixteenth, so the tile's outer texel columns --
// where the spike pixels are -- hang past the neighbouring sides. A side
// narrowed to the inset box maps only the middle fourteen columns and draws a
// smooth post, which is what this used to do.
TEST(a_cactus_side_spans_the_cell_a_sixteenth_in)
{
    const int id = firstOfType(block::RenderType::Cactus);
    CHECK(id > 0);

    const MeshBuilder m = meshOne(u16(id), 0);
    int sides = 0;
    for (usize q = 0; q < m.detailQuadCount(); ++q) {
        const DetailVertex* quad = m.detailVertices() + q * 4;
        const int face = quad[0].face;
        if (face != mesh::kFaceNegZ && face != mesh::kFacePosZ && face != mesh::kFaceNegX
            && face != mesh::kFacePosX) {
            continue;
        }
        ++sides;

        const bool alongX = face == mesh::kFaceNegX || face == mesh::kFacePosX;
        const bool negative = face == mesh::kFaceNegX || face == mesh::kFaceNegZ;
        const double depth = negative ? 1.0 / 16.0 : 15.0 / 16.0;
        double lateralMin = 2.0, lateralMax = -1.0;
        int uMin = 1 << 20, uMax = -(1 << 20);
        for (int c = 0; c < 4; ++c) {
            const double normal = local(alongX ? quad[c].x : quad[c].z, 8);
            const double lateral = local(alongX ? quad[c].z : quad[c].x, 8);
            CHECK(std::abs(normal - depth) < 1e-9);
            lateralMin = std::min(lateralMin, lateral);
            lateralMax = std::max(lateralMax, lateral);
            uMin = std::min(uMin, int(quad[c].u));
            uMax = std::max(uMax, int(quad[c].u));
        }
        CHECK(std::abs(lateralMin) < 1e-9);
        CHECK(std::abs(lateralMax - 1.0) < 1e-9);
        // The whole tile, less the edge margin every emitter takes.
        CHECK_EQ(uMax - uMin, mesh::kUvUnitsPerTile - 2 * mesh::kUvInset);
    }
    CHECK_EQ(sides, 4);
}

// ---------------------------------------------------------------------------
// Fire, which is the one shape here with more than one shape
// ---------------------------------------------------------------------------
//
// `bc.d` branches on what is under and around the cell before it draws a
// single vertex, and the branch it takes is not cosmetic: floor fire stands in
// the middle of its cell and wall fire clings to a face. This used to draw an
// approximation of the first in every case, so a fire eating the side of a
// house floated in the air beside it.
//
// The quad counts below are `addSheet`'s doubling of the original's own sheet
// count -- see the note on `addFire`.

namespace {

// One fire block with whatever a test puts around it.
MeshBuilder meshFire(u16 below, u16 negX, u16 above)
{
    ChunkColumn column;
    column.setBlock(8, 8, 8, u16(mcver::Block::Fire));
    if (below != 0) column.setBlock(8, 7, 8, below);
    if (negX != 0) column.setBlock(7, 8, 8, negX);
    if (above != 0) column.setBlock(8, 9, 8, above);
    return meshOf(column, 0);
}

// The extremes of the drawn geometry along one axis, in cell-local units.
struct Extent {
    double lo = 2.0, hi = -2.0;
};

Extent spanOf(const MeshBuilder& m, int axis, int block)
{
    Extent out;
    for (usize v = 0; v < m.detailQuadCount() * 4; ++v) {
        const DetailVertex& d = m.detailVertices()[v];
        const i16 stored = axis == 0 ? d.x : (axis == 1 ? d.y : d.z);
        const double at = local(stored, block);
        out.lo = at < out.lo ? at : out.lo;
        out.hi = at > out.hi ? at : out.hi;
    }
    return out;
}

}  // namespace

TEST(fire_on_a_solid_floor_draws_the_leaning_sheets)
{
    // Eight sheets: four leaning from +-0.2 to +-0.3, four from +-0.4 to the
    // cell wall. `f1` puts their tops 1.4 blocks up.
    const MeshBuilder m = meshFire(u16(mcver::Block::Stone), 0, 0);
    CHECK_EQ(m.detailQuadCount(), usize(16));

    const Extent y = spanOf(m, 1, 8);
    CHECK(y.lo >= -1e-9 && y.lo <= 1e-9);
    CHECK(y.hi > 1.39 && y.hi < 1.41);

    // Horizontally it fills the cell -- the outermost sheets sit on the wall.
    const Extent x = spanOf(m, 0, 8);
    CHECK(x.lo >= -1e-9 && x.lo < 1e-9);
    CHECK(x.hi > 1.0 - 1e-9);
}

TEST(fire_over_a_burnable_block_draws_the_floor_shape_too)
{
    // The branch is `isBlockNormalCube(i, j-1, k) || canBlockCatchFire(...)`,
    // so a flame on planks and a flame on cobble are the same shape.
    const MeshBuilder solid = meshFire(u16(mcver::Block::Stone), 0, 0);
    const MeshBuilder planks = meshFire(u16(mcver::Block::Planks), 0, 0);
    CHECK_EQ(planks.detailQuadCount(), solid.detailQuadCount());
}

TEST(fire_with_nothing_under_it_and_nothing_beside_it_draws_nothing)
{
    // The original's answer too, and it is never seen: a flame in mid-air with
    // nothing to burn is put out on the next tick. See
    // `fire_in_the_air_with_nothing_to_burn_goes_out_at_once` in tick_test.
    const MeshBuilder m = meshFire(0, 0, 0);
    CHECK_EQ(m.detailQuadCount(), usize(0));
}

TEST(fire_beside_a_burnable_wall_clings_to_that_wall)
{
    // **This is the state that was missing.** One sheet, pinned to the -x
    // face, leaning out by 0.2 at the top and lifted 0.0625 off the floor.
    const MeshBuilder m = meshFire(0, u16(mcver::Block::Planks), 0);
    CHECK_EQ(m.detailQuadCount(), usize(2));

    const Extent x = spanOf(m, 0, 8);
    CHECK(x.lo >= -1e-9 && x.lo < 1e-9);
    CHECK(x.hi > 0.19 && x.hi < 0.21);

    const Extent y = spanOf(m, 1, 8);
    CHECK(y.lo > 0.06 && y.lo < 0.07);
    CHECK(y.hi > 1.45 && y.hi < 1.47);

    // ...and it spans the whole face across z, which is what makes it read as
    // a wall of flame rather than a sheet in the middle of the cell.
    const Extent z = spanOf(m, 2, 8);
    CHECK(z.lo >= -1e-9 && z.lo < 1e-9);
    CHECK(z.hi > 1.0 - 1e-9);
}

TEST(fire_beside_an_unburnable_wall_clings_to_nothing)
{
    // Stone is not in `chanceToEncourageFire`, so there is nothing for the
    // flame to hold on to and no quad for that face.
    const MeshBuilder m = meshFire(0, u16(mcver::Block::Stone), 0);
    CHECK_EQ(m.detailQuadCount(), usize(0));
}

TEST(fire_under_a_burnable_ceiling_hangs_from_it)
{
    // Two sheets, both in the top fifth of the cell: `f1` is reassigned to
    // -0.2F and `j` incremented, so the flame hangs down from the block above
    // rather than standing up from the one below.
    const MeshBuilder m = meshFire(0, 0, u16(mcver::Block::Planks));
    CHECK_EQ(m.detailQuadCount(), usize(4));

    const Extent y = spanOf(m, 1, 8);
    CHECK(y.lo > 0.79 && y.lo < 0.81);
    CHECK(y.hi > 1.0 - 1e-9 && y.hi < 1.0 + 1e-9);
}

TEST(a_wall_flame_and_a_ceiling_flame_are_drawn_together)
{
    // The wall tests are `if`s and not an `if/else` chain, so a cell burning
    // on two sides draws both.
    const MeshBuilder m = meshFire(0, u16(mcver::Block::Planks), u16(mcver::Block::Planks));
    CHECK_EQ(m.detailQuadCount(), usize(2 + 4));
}

TEST(neighbouring_wall_flames_do_not_all_use_the_same_tile)
{
    // `(i + j + k) & 1` picks which of the two flame tiles a wall face starts
    // on. It changes nothing about the geometry and everything about whether a
    // burning wall looks like wallpaper, so what is checked is that two
    // adjacent cells disagree.
    ChunkColumn column;
    column.setBlock(7, 8, 8, u16(mcver::Block::Planks));
    column.setBlock(8, 8, 8, u16(mcver::Block::Fire));
    column.setBlock(7, 8, 9, u16(mcver::Block::Planks));
    column.setBlock(8, 8, 9, u16(mcver::Block::Fire));
    const MeshBuilder m = meshOf(column, 0);
    CHECK_EQ(m.detailQuadCount(), usize(4));

    std::set<int> rows;
    for (usize v = 0; v < m.detailQuadCount() * 4; ++v) {
        rows.insert(m.detailVertices()[v].v / mesh::kUvUnitsPerTile);
    }
    CHECK_EQ(rows.size(), usize(2));
}


// ---------------------------------------------------------------------------
// The ladder is a sheet, and the rail turns
// ---------------------------------------------------------------------------

TEST(a_ladder_is_one_flat_sheet_against_its_wall)
{
    // `bc.g` writes four vertices and stops. This used to draw the collision
    // box -- six faces, two sixteenths thick -- whose back face sat *inside*
    // the wall the ladder hangs on and z-fought with it. Two quads here rather
    // than one because `addSheet` draws the mirror as well, which is what a
    // cutout texture on a glass wall needs; six would be the box again.
    const MeshBuilder mesh = meshOne(u16(mcver::Block::Ladder), 4);
    CHECK_EQ(int(mesh.detailQuadCount()) * 4, 8);

    // Every vertex on one plane, and that plane 0.05 off the +x wall.
    for (usize i = 0; i < mesh.detailQuadCount() * 4; ++i) {
        const double x = local(mesh.detailVertices()[i].x, 8);
        CHECK(x > 0.94 && x < 0.96);
    }
}

TEST(a_ladder_hangs_on_whichever_wall_its_metadata_names)
{
    // The four cases, each pinned by the axis and side its sheet lands on.
    struct Case {
        u8 metadata;
        int axis;      // 0 = x, 2 = z
        bool high;     // the +side of the cell
    };
    const Case cases[] = {
        {2, 2, true},    // +z wall
        {3, 2, false},   // -z wall
        {4, 0, true},    // +x wall
        {5, 0, false},   // -x wall
    };
    for (const Case& c : cases) {
        const MeshBuilder mesh = meshOne(u16(mcver::Block::Ladder), c.metadata);
        CHECK_EQ(int(mesh.detailQuadCount()) * 4, 8);
        for (usize i = 0; i < mesh.detailQuadCount() * 4; ++i) {
            const DetailVertex& v = mesh.detailVertices()[i];
            const double at = local(c.axis == 0 ? v.x : v.z, 8);
            if (c.high) {
                CHECK(at > 0.9);
            } else {
                CHECK(at < 0.1);
            }
        }
    }
}

TEST(a_ladder_at_a_metadata_the_game_never_writes_draws_nothing)
{
    // Metadata 0 is outside the 2..5 a placement can produce, and it is the one
    // `collisionBoxes` answers with a full cube for -- so drawing it would be a
    // solid block of ladder texture.
    const MeshBuilder mesh = meshOne(u16(mcver::Block::Ladder), 0);
    CHECK_EQ(int(mesh.detailQuadCount()), 0);
}

TEST(a_rail_turns_a_quarter_when_it_lies_along_x)
{
    // The bug this closes: the emitter kept one fixed set of cell corners for
    // every metadata, so a north-south rail and an east-west one were the same
    // picture and no curve pointed anywhere. The turn is in the geometry, not
    // the UVs, so the check is that a given texture corner lands on a different
    // cell corner.
    const MeshBuilder alongZ = meshOne(u16(mcver::Block::Rail), 0);
    const MeshBuilder alongX = meshOne(u16(mcver::Block::Rail), 1);
    CHECK_EQ(int(alongZ.detailQuadCount()) * 4, 8);
    CHECK_EQ(int(alongX.detailQuadCount()) * 4, 8);

    // Same four positions, in a different order -- so the picture is turned
    // rather than moved.
    bool anyDifferent = false;
    for (int i = 0; i < 4; ++i) {
        if (alongZ.detailVertices()[i].x != alongX.detailVertices()[i].x
            || alongZ.detailVertices()[i].z != alongX.detailVertices()[i].z) {
            anyDifferent = true;
        }
    }
    CHECK(anyDifferent);
}

TEST(an_ascending_rail_climbs_towards_the_side_its_metadata_names)
{
    // 2 towards +x, 3 towards -x, 4 towards -z, 5 towards +z. The emitter had
    // 2 and 3 the other way round, so every east-west rail staircase in a world
    // climbed backwards and met its neighbour in mid air.
    struct Case {
        u8 metadata;
        int axis;    // 0 = x, 2 = z
        bool high;   // the raised corners are on the + side
    };
    const Case cases[] = {{2, 0, true}, {3, 0, false}, {4, 2, false}, {5, 2, true}};

    for (const Case& c : cases) {
        const MeshBuilder mesh = meshOne(u16(mcver::Block::Rail), c.metadata);
        CHECK_EQ(int(mesh.detailQuadCount()) * 4, 8);
        for (usize i = 0; i < mesh.detailQuadCount() * 4; ++i) {
            const DetailVertex& v = mesh.detailVertices()[i];
            const double y = local(v.y, 8);
            const double at = local(c.axis == 0 ? v.x : v.z, 8);
            // A corner is either at the sixteenth or at the top of the cell,
            // and which one has to agree with which side of the block it is on.
            const bool raised = y > 0.5;
            CHECK_EQ(raised, c.high ? at > 0.5 : at < 0.5);
        }
    }
}

TEST(a_curved_rail_shows_the_tile_a_row_up)
{
    // `if.a(II)I`: metadata 6 and above answer `blockIndexInTexture - 16`, and
    // terrain.png puts the curved rail directly above the straight one.
    const MeshBuilder straight = meshOne(u16(mcver::Block::Rail), 0);
    const MeshBuilder curved = meshOne(u16(mcver::Block::Rail), 6);
    CHECK(tilesIn(straight) != tilesIn(curved));
}
