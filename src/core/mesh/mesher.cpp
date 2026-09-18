#include "core/mesh/mesher.hpp"

#include "core/block/side_rule.hpp"

#include "core/block/collision.hpp"
#include "core/block/registry.hpp"
#include "core/block/world_texture.hpp"
#include "core/mesh/box.hpp"
#include "core/mesh/cube_atlas.hpp"
#include "core/mesh/shapes.hpp"
#include "core/mesh/fluid.hpp"
#include "core/mesh/torch.hpp"

#include <cassert>
#include <cstring>

namespace mc::mesh {

using block::BlockDef;
using block::RenderType;
using world::BlockId;
using world::ChunkColumn;
using world::Section;

namespace {

// A face's (layer, i, j) grid axes, read off kFaceBasis: the layer runs along
// the face normal, i along e1 and j along e2. A negative edge counts its axis
// down from 15, so that cell (i, j) is always first + i*e1 + j*e2 -- the
// property that lets a run found in the grid go straight to addQuad.
struct FaceAxes {
    u8 normal;
    u8 axisI;
    u8 axisJ;
    bool flipI;
    bool flipJ;
};

constexpr int axisOf(const i8 (&v)[3])
{
    return v[0] != 0 ? 0 : (v[1] != 0 ? 1 : 2);
}

constexpr FaceAxes faceAxes(int face)
{
    const FaceBasis& b = kFaceBasis[face];
    const FaceOffset& n = kFaceOffset[face];
    const int normal = n.dx != 0 ? 0 : (n.dy != 0 ? 1 : 2);
    const int i = axisOf(b.e1);
    const int j = axisOf(b.e2);
    return {u8(normal), u8(i), u8(j), b.e1[i] < 0, b.e2[j] < 0};
}

inline constexpr FaceAxes kFaceAxes[kFaceCount] = {
    faceAxes(0), faceAxes(1), faceAxes(2), faceAxes(3), faceAxes(4), faceAxes(5),
};

// Each edge is one unit along one axis, and the three axes are all different --
// the grid above is a relabelling of the section, not a projection of it.
constexpr bool faceAxesArePermutations()
{
    for (int face = 0; face < kFaceCount; ++face) {
        const FaceBasis& b = kFaceBasis[face];
        const FaceAxes& a = kFaceAxes[face];
        if (a.normal == a.axisI || a.normal == a.axisJ || a.axisI == a.axisJ) return false;
        if (b.e1[a.axisI] * b.e1[a.axisI] != 1 || b.e2[a.axisJ] * b.e2[a.axisJ] != 1) return false;
    }
    return true;
}

static_assert(faceAxesArePermutations(), "each face edge must be one unit along its own axis");

constexpr int kGridEdge = Section::kSize;
constexpr int kGridCells = kGridEdge * kGridEdge;
static_assert(kGridEdge == 16, "a grid row is one u16 of face bits");
static_assert(kCubeSlotCount <= 256, "a face key holds the slot in its high byte");

}  // namespace

void MeshBuilder::addQuad(int x, int y, int z, int face, u16 texture, u8 light, int width,
                          int height)
{
    emitQuad(x, y, z, face, cubeSlotOf(texture), light, width, height);
}

void MeshBuilder::emitQuad(int x, int y, int z, int face, int slot, u8 light, int width,
                           int height)
{
    assert(face >= 0 && face < kFaceCount);
    assert(width >= 1 && width <= kCubeRepeat && height >= 1 && height <= kCubeRepeat);

    // Every pass draws through one shared index buffer sized for
    // kMaxQuadsPerSection, so a stream that runs past it would have the GPU
    // read indices that do not exist. Dropping the quad loses a face; not
    // dropping it fetches vertices from outside the bound buffer. See the
    // clamp in Renderer::drawPass for the other half of this.
    if (quadCount() >= usize(kMaxQuadsPerSection)) {
        ++dropped_;
        return;
    }

    // The cube pass samples the cube atlas, not terrain.png -- see
    // core/mesh/cube_atlas.hpp. A texture index outside the atlas has already
    // been sent to tile 0's slot by cubeSlotOf: a world can name a block we do
    // not know, and the unknown entry has to land somewhere real rather than
    // sample past the texture.
    const int slotX = slot % kCubeSlotsPerEdge;
    const int slotY = slot / kCubeSlotsPerEdge;

    // One quad, and everything else -- the four corners, their UVs, the face
    // shade, the seam -- is rebuilt on the GPU from kFaceBasis, which is
    // asserted against the corner tables the branch below reads.
    if (cubeFormat_ == CubeFormat::Quads) {
        QuadVertex q;
        q.x = static_cast<u8>(x);
        q.y = static_cast<u8>(y);
        q.z = static_cast<u8>(z);
        q.face = static_cast<u8>(face);
        q.slotX = static_cast<u8>(slotX);
        q.slotY = static_cast<u8>(slotY);
        q.light = light;
        q.extent = packExtent(width, height);
        quads_.push_back(q);
        return;
    }

    const FaceBasis& b = kFaceBasis[face];
    const bool merged = width > 1 || height > 1;
    const u8 shade = kFaceShade[face];

    for (int c = 0; c < 4; ++c) {
        const int i = kCornerIJ[c][0];
        const int j = kCornerIJ[c][1];

        WorldVertex v;
        // The corner is kFaceCorner's for a single face -- the static_assert
        // on kFaceBasis says so -- and the run's far corner for a merged one.
        v.x = static_cast<u8>(x + b.base[0] + i * width * b.e1[0] + j * height * b.e2[0]);
        v.y = static_cast<u8>(y + b.base[1] + i * width * b.e1[1] + j * height * b.e2[1]);
        v.z = static_cast<u8>(z + b.base[2] + i * width * b.e1[2] + j * height * b.e2[2]);

        // kFaceCornerUV in tiles, stretched over the run: u follows i and v
        // follows j the way the table already says, so a merged quad is its
        // faces' texture laid end to end and not one tile stretched across
        // them. Inset off the slot boundary at the near end and off the end of
        // the run at the far one; see kCubeUvInset.
        const int uTiles = kFaceCornerUV[face][c][0] * width;
        const int vTiles = kFaceCornerUV[face][c][1] * height;
        v.u = uTiles == 0 ? cubeUvStart(slotX) : cubeUvEnd(slotX, uTiles);
        v.v = vTiles == 0 ? cubeUvStart(slotY) : cubeUvEnd(slotY, vTiles);
        v.seam = seamOf(face, merged, c);

        // Face shade alone, and for a1.1.2 that is the whole answer: its
        // Block.colorMultiplier returns white for every block in the game and
        // nothing overrides it, so there is no tint to apply. Ambient occlusion
        // will darken this per corner when smooth lighting lands, which is why
        // the field stays per-vertex rather than becoming a per-quad constant.
        // See docs/status.md.
        v.r = shade;
        v.g = shade;
        v.b = shade;

        v.light = light;

        cubes_.push_back(v);
    }
}

void MeshBuilder::ensureFaceGrid()
{
    if (faceKeys_.empty()) {
        faceKeys_.assign(usize(kFaceCount) * kGridEdge * kGridCells, 0);
        faceRows_.assign(usize(kFaceCount) * kGridCells, 0);
    }
}

void MeshBuilder::discardFaces()
{
    if (!facesPending_) {
        return;
    }
    std::memset(faceRows_.data(), 0, faceRows_.size() * sizeof(u16));
    std::memset(faceLayers_, 0, sizeof(faceLayers_));
    facesPending_ = false;
}

void MeshBuilder::addFace(int x, int y, int z, int face, u16 texture, u8 light)
{
    if (!greedy_) {
        addQuad(x, y, z, face, texture, light);
        return;
    }
    ensureFaceGrid();

    const FaceAxes& a = kFaceAxes[face];
    const int cell[3] = {x, y, z};
    const int layer = cell[a.normal];
    const int i = a.flipI ? kGridEdge - 1 - cell[a.axisI] : cell[a.axisI];
    const int j = a.flipJ ? kGridEdge - 1 - cell[a.axisJ] : cell[a.axisJ];

    const usize row = (usize(face) * kGridEdge + usize(layer)) * kGridEdge + usize(j);
    faceKeys_[row * kGridEdge + usize(i)] = u16((cubeSlotOf(texture) << 8) | light);
    faceRows_[row] = u16(faceRows_[row] | (1u << i));
    faceLayers_[face] = u16(faceLayers_[face] | (1u << layer));
    facesPending_ = true;
}

// **The merge.** Row by row, the lowest face left in the row starts a run; the
// run grows along i while the next face is there and has the same key, then
// along j while the whole of the next row's span is there and matches. Each
// covered bit is cleared, so every face is drawn exactly once and the grid is
// empty afterwards.
//
// Both directions stop at kCubeRepeat, and that limit is the hardware's and
// not a tuning choice: past four copies the cube atlas's slot runs out of tile
// to sample. See core/mesh/cube_atlas.hpp.
//
// Plain greedy, widest first, with no search for a better cover. The runs
// this finds are within a small factor of the best rectangle cover on terrain,
// and the search that would close the gap costs main-thread time the mesh
// budget is measured in.
void MeshBuilder::flushFaces()
{
    if (!facesPending_) {
        return;
    }

    for (int face = 0; face < kFaceCount; ++face) {
        const FaceAxes& a = kFaceAxes[face];
        u32 layers = faceLayers_[face];
        while (layers != 0) {
            const int layer = __builtin_ctz(layers);
            layers &= layers - 1;

            const usize base = (usize(face) * kGridEdge + usize(layer)) * kGridEdge;
            u16* rows = &faceRows_[base];
            const u16* keys = &faceKeys_[base * kGridEdge];

            for (int j = 0; j < kGridEdge; ++j) {
                while (rows[j] != 0) {
                    const int i0 = __builtin_ctz(rows[j]);
                    const u16 key = keys[j * kGridEdge + i0];

                    int width = 1;
                    while (width < kCubeRepeat && i0 + width < kGridEdge
                           && (rows[j] >> (i0 + width) & 1u) != 0
                           && keys[j * kGridEdge + i0 + width] == key) {
                        ++width;
                    }
                    const u32 span = ((1u << width) - 1u) << i0;

                    int height = 1;
                    while (height < kCubeRepeat && j + height < kGridEdge
                           && (rows[j + height] & span) == span) {
                        const u16* next = keys + (j + height) * kGridEdge + i0;
                        bool same = true;
                        for (int k = 0; k < width; ++k) {
                            same = same && next[k] == key;
                        }
                        if (!same) {
                            break;
                        }
                        ++height;
                    }
                    for (int k = 0; k < height; ++k) {
                        rows[j + k] = u16(rows[j + k] & ~span);
                    }

                    // Back from the grid to the section: the run's first cell.
                    int cell[3];
                    cell[a.normal] = layer;
                    cell[a.axisI] = a.flipI ? kGridEdge - 1 - i0 : i0;
                    cell[a.axisJ] = a.flipJ ? kGridEdge - 1 - j : j;
                    emitQuad(cell[0], cell[1], cell[2], face, key >> 8, u8(key & 0xFF), width,
                             height);
                }
            }
        }
        faceLayers_[face] = 0;
    }
    facesPending_ = false;
}

void MeshBuilder::addDetailQuad(const i16 corner[4][3], const i16 uv[4][2], u8 shade, u8 light,
                                int face, DetailPass pass)
{
    std::vector<DetailVertex>& out = pass == DetailPass::Translucent ? translucent_ : details_;

    // The same bound, and this is the stream that can actually reach it: detail
    // faces are not culled against their neighbours, so a section packed with
    // torches (five quads each) or fire is not bounded by the checkerboard
    // argument that sizes kMaxQuadsPerSection.
    if (out.size() / 4 >= usize(kMaxQuadsPerSection)) {
        ++dropped_;
        return;
    }

    for (int c = 0; c < 4; ++c) {
        DetailVertex v;
        v.x = corner[c][0];
        v.y = corner[c][1];
        v.z = corner[c][2];
        v.face = static_cast<i16>(face);
        v.u = uv[c][0];
        v.v = uv[c][1];
        v.r = shade;
        v.g = shade;
        v.b = shade;
        v.light = light;
        out.push_back(v);
    }
}

void MeshBuilder::copyTo(void* destination) const
{
    const MeshRanges layout = ranges();
    u8* dst = static_cast<u8*>(destination);

    // **The zero checks are not redundant.** An empty std::vector's data() may
    // return null, and memcpy's second parameter is declared non-null
    // regardless of the length -- so copying nothing out of an empty vector is
    // undefined behaviour even though every implementation does the harmless
    // thing. Most sections reach here with at least one empty pass (a section
    // of plain stone has no detail geometry and no translucent geometry), so
    // this is the common path rather than an edge case. UBSan flags it by
    // name once the core library is instrumented.
    const auto copy = [](u8* to, const void* from, usize bytes) {
        if (bytes != 0) {
            std::memcpy(to, from, bytes);
        }
    };

    if (cubeFormat_ == CubeFormat::Quads) {
        copy(dst, quads_.data(), layout.cubeBytes);
    } else {
        copy(dst, cubes_.data(), layout.cubeBytes);
    }

    // The gap between the cube range's end and the detail range's aligned start
    // -- at most 8 bytes, and only in CubeFormat::Quads. It is zeroed rather
    // than left alone because the caller's staging buffer is reused between
    // sections and every byte of it is uploaded: skipping this would hand the
    // GPU the previous section's tail, which is not read by any draw but is
    // exactly the kind of thing that makes a memory checker's report useless.
    const bool anythingFollows = layout.detailBytes != 0 || layout.translucentBytes != 0;
    const usize pad = anythingFollows ? layout.detailOffset() - layout.cubeBytes : 0;
    if (pad != 0) {
        std::memset(dst + layout.cubeBytes, 0, pad);
    }

    copy(dst + layout.detailOffset(), details_.data(), layout.detailBytes);
    copy(dst + layout.translucentOffset(), translucent_.data(), layout.translucentBytes);
}

namespace {

// Crossed squares: flowers, saplings, mushrooms, sugar cane.
//
// a1.1.2's RenderBlocks draws two vertical quads on the block's diagonals,
// inset by 0.45 from the centre rather than reaching the corners -- the
// constant is literally 0.44999998807907104 in the bytecode, the double nearest
// 0.45. Each plane is emitted twice with opposite winding, because the original
// has no back-face culling to satisfy and a flower has to be visible from both
// sides.
//
// Unshaded, and that is not an omission: the cross renderer calls
// setColorOpaque_F once with the block's own brightness and never touches the
// per-face table, so a flower is equally bright on all four of its faces.
void addCross(const MeshScratch& scratch, int x, int y, int z, u16 texture, MeshBuilder& out)
{
    constexpr float kInset = 0.5f - 0.45f;  // 0.05 from each edge

    const i16 lo = detailPos(0, kInset);
    const i16 hi = detailPos(1, -kInset);
    const i16 x0 = i16(x * kDetailUnitsPerBlock + lo);
    const i16 x1 = i16(x * kDetailUnitsPerBlock + hi);
    const i16 z0 = i16(z * kDetailUnitsPerBlock + lo);
    const i16 z1 = i16(z * kDetailUnitsPerBlock + hi);
    const i16 y0 = i16(y * kDetailUnitsPerBlock);
    const i16 y1 = i16((y + 1) * kDetailUnitsPerBlock);

    const int tile = texture < kAtlasTileCount ? texture : 0;
    const i16 u0 = tileUvMin(tile % kAtlasTilesPerEdge);
    const i16 v0 = tileUvMin(tile / kAtlasTilesPerEdge);
    const i16 u1 = tileUvMax(tile % kAtlasTilesPerEdge);
    const i16 v1 = tileUvMax(tile / kAtlasTilesPerEdge);

    // v runs downward, as everywhere else: terrain.png's first row is its top.
    //
    // **The order here has to track `planes` below corner for corner, and it is
    // the one thing about a cross that no other test was checking.** A plane
    // runs top, bottom, bottom, top as it crosses the block, so the UVs must
    // run top-left, bottom-left, bottom-right, top-right. Writing the tile's
    // corners in rectangle order instead -- (u0,v0), (u1,v0), (u1,v1), (u0,v1)
    // -- swaps corners 1 and 3 against the geometry and mirrors the tile across
    // its own diagonal, which stands every flower on its side. torch.cpp's
    // `sideUv` is the same four corners in the correct order.
    const i16 uv[4][2] = {{u0, v0}, {u0, v1}, {u1, v1}, {u1, v0}};
    const i16 uvFlipped[4][2] = {{u1, v0}, {u1, v1}, {u0, v1}, {u0, v0}};

    // Light comes from the block's own cell. A cross is not an opaque cube, so
    // its cell carries real light rather than the darkness inside a solid block.
    const u8 light = scratch.light(x, y, z);
    constexpr u8 kUnshaded = 255;

    const i16 planes[2][4][3] = {
        {{x0, y1, z0}, {x0, y0, z0}, {x1, y0, z1}, {x1, y1, z1}},  // NW to SE
        {{x0, y1, z1}, {x0, y0, z1}, {x1, y0, z0}, {x1, y1, z0}},  // SW to NE
    };

    for (const auto& plane : planes) {
        out.addDetailQuad(plane, uv, kUnshaded, light, 0, DetailPass::Opaque);

        // The same plane reversed, so it survives back-face culling from the
        // other side. Reversing the corners flips the winding; the UVs have to
        // follow or the texture would be mirrored on the back.
        const i16 back[4][3] = {
            {plane[3][0], plane[3][1], plane[3][2]}, {plane[2][0], plane[2][1], plane[2][2]},
            {plane[1][0], plane[1][1], plane[1][2]}, {plane[0][0], plane[0][1], plane[0][2]},
        };
        out.addDetailQuad(back, uvFlipped, kUnshaded, light, 0, DetailPass::Opaque);
    }
}

// Whether a block's render bounds fill its cell, which is the test that keeps
// the fast path fast. Exact comparisons on purpose: these come out of a
// generated table of floats that were 0.0f and 1.0f in the jar, so anything
// that is not exactly the unit cube was written as something else on purpose.
bool isUnitCube(const AABB& b)
{
    return b.minX == 0.0 && b.minY == 0.0 && b.minZ == 0.0 && b.maxX == 1.0 && b.maxY == 1.0
           && b.maxZ == 1.0;
}

// A standard block that does not fill its cell. Same face-culling rule as the
// cube path with one addition: **a face is only a candidate for culling if the
// box actually reaches that side of the block.** A slab's top face is at y=0.5
// and has no neighbour to be hidden by, so it is always drawn; its bottom face
// is at y=0 and is culled by the block underneath exactly as a full cube's
// would be.
void addBoundedCube(const MeshScratch& scratch, int x, int y, int z, const u16 tiles[6],
                    const AABB& bounds, MeshBuilder& out)
{
    const double reach[kFaceCount] = {
        bounds.minY, 1.0 - bounds.maxY, bounds.minZ, 1.0 - bounds.maxZ,
        bounds.minX, 1.0 - bounds.maxX,
    };

    int mask = 0;
    u8 light = scratch.light(x, y, z);
    const BlockId self = scratch.block(x, y, z);
    const BlockDef& selfDef = block::def(self);
    for (int face = 0; face < kFaceCount; ++face) {
        const FaceOffset& offset = kFaceOffset[face];
        const int nx = x + offset.dx;
        const int ny = y + offset.dy;
        const int nz = z + offset.dz;
        const bool touchesEdge = reach[face] == 0.0;
        // **The block's own `shouldSideBeRendered`**, which for a slab is not
        // "is the neighbour opaque" -- see core/block/side_rule.hpp. The reach
        // test is still ours and still first: a face that does not touch the
        // side of the cell has no neighbour to be hidden by whatever rule the
        // block carries.
        if (touchesEdge
            && !block::sideVisible(self, selfDef, scratch.block(nx, ny, nz), face)) {
            continue;
        }
        mask |= 1 << face;
        // Light comes from the cell the face looks into for a face on the
        // block's own edge, and from the block's own cell for one that sits
        // inside it -- there is no neighbour to read for the top of a slab.
        if (touchesEdge) {
            light = scratch.light(nx, ny, nz);
        }
    }

    // **One light for the whole box, and that is a simplification.** The
    // original lights each face from the cell it faces; sampling six of them
    // and carrying six lights through addBox would be right and is not what
    // this does yet. In practice these blocks are thin and their faces see the
    // same cell, so the visible difference is the underside of a slab in a dark
    // room. Named here rather than discovered.
    addBox(x, y, z, bounds, tiles, light, true, mask, out);
}

// Everything the cube stream cannot draw: the nine render types with their own
// emitters, the ten shapes.cpp added, and the standard blocks whose bounds are
// smaller than their cell.
//
// Out of line and out of the inner loop on purpose -- it runs for a handful of
// blocks per section and the loop above runs for four thousand.
void emitNonCube(const MeshScratch& scratch, int x, int y, int z, BlockId id,
                 const BlockDef& def, MeshBuilder& out)
{
    if (def.render == RenderType::Cube) {
        const u8 metadata = scratch.metadata(x, y, z);
        // **The tiles, and for one block in a1.1.2 that is a question about
        // the neighbours**: a chest turns its front away from the blocks round
        // it and joins the chest beside it into one two-cell picture. The
        // scratch is padded by one cell, which is exactly the reach of the
        // rule -- including the two diagonals a pair reads. See
        // core/block/world_texture.hpp.
        u16 worldTiles[6];
        block::worldTextureFaces(id, metadata,
                                 [&scratch, x, y, z](int dx, int dz) {
                                     return scratch.block(x + dx, y, z + dz);
                                 },
                                 worldTiles);
        const u16* tiles = worldTiles;
        const AABB bounds = block::selectionBox(id, metadata);
        if (!isUnitCube(bounds)) {
            addBoundedCube(scratch, x, y, z, tiles, bounds, out);
        } else {
            // A full cube the fast path was told to leave alone: a furnace,
            // whose mouth is on the side its metadata names and which `faces`
            // cannot say, or an id past the end of the block table -- drawn
            // as a full cube, which is what the unknown block is for. Either
            // way it is the cube stream's own quad, so it lights and shades
            // exactly as the fast path would.
            for (int face = 0; face < kFaceCount; ++face) {
                const FaceOffset& offset = kFaceOffset[face];
                if (!block::sideVisible(id, def,
                                        scratch.block(x + offset.dx, y + offset.dy,
                                                      z + offset.dz),
                                        face)) {
                    continue;
                }
                out.addFace(x, y, z, face, tiles[face],
                            scratch.light(x + offset.dx, y + offset.dy, z + offset.dz));
            }
        }
        return;
    }

    // Each render type is its own emitter. The ones with no emitter are skipped
    // rather than drawn as cubes: a missing shape is obvious, a cubic one looks
    // deliberate. hasEmitter() below lists exactly the cases reached here, and
    // a test in mesher_test.cpp checks the two agree.
    switch (def.render) {
        case RenderType::Cross:
            addCross(scratch, x, y, z, def.texture, out);
            break;
        case RenderType::Fluid:
            addFluid(scratch, x, y, z, def, out);
            break;
        case RenderType::Torch:
            addTorch(scratch, x, y, z, def, out);
            break;
        default:
            // The other ten, all through one call. See core/mesh/shapes.hpp --
            // before it, every one of them fell through here and was drawn as
            // nothing, which stopped being defensible the moment Creative could
            // place them.
            addShape(scratch, x, y, z, id, scratch.metadata(x, y, z), out);
            break;
    }
}

}  // namespace

void meshSection(const MeshScratch& scratch, MeshBuilder& out)
{
    // X outer, Z middle, Y inner: the scratch is Y-fastest, so the inner loop
    // and its six neighbour probes stay inside a handful of cache lines.
    for (int x = 0; x < Section::kSize; ++x) {
        for (int z = 0; z < Section::kSize; ++z) {
            for (int y = 0; y < Section::kSize; ++y) {
                const BlockId id = scratch.block(x, y, z);
                if (id == world::kAirBlock) {
                    continue;
                }

                const BlockDef& def = block::def(id);

                // **One field decides the whole dispatch**, and that is a
                // measured shape rather than a tidy one. Two things disqualify
                // a block from the fast cube stream -- a render type that is
                // not a cube, and bounds that do not fill the cell -- and
                // asking those as two questions cost 3.3 us a section on the
                // dev host, because the second answer lives in a second table
                // and therefore a second cache line. `unitCube` is both, folded
                // by the generator into the row this loop has already loaded.
                if (!def.unitCube) {
                    emitNonCube(scratch, x, y, z, id, def, out);
                    continue;
                }

                for (int face = 0; face < kFaceCount; ++face) {
                    const FaceOffset& offset = kFaceOffset[face];
                    const int nx = x + offset.dx;
                    const int ny = y + offset.dy;
                    const int nz = z + offset.dz;

                    // **`shouldSideBeRendered`, which is the block's own and
                    // not one line.** The base class is "unless the neighbour
                    // is an opaque cube" and that covers sixty-five blocks;
                    // glass, ice and leaves also hide a face against their own
                    // kind, which is what makes a wall of glass a window
                    // rather than a stack of boxes. See
                    // core/block/side_rule.hpp.
                    if (!block::sideVisible(id, def, scratch.block(nx, ny, nz), face)) {
                        continue;
                    }

                    // Light comes from the cell the face looks into, not from
                    // the block itself, which is always dark inside.
                    // Per-face, so grass is grass on top and dirt underneath.
                    // Uniform blocks carry six copies, so there is nothing to
                    // branch on here.
                    out.addFace(x, y, z, face, def.faces[face],
                                scratch.light(nx, ny, nz));
                }
            }
        }
    }

    // The cube faces were only collected above; this is where they are merged
    // and written. See MeshBuilder::flushFaces.
    out.flushFaces();

    // Both streams draw through the same index buffer, one draw call each, so
    // each has to fit it -- not their sum. Enforced in addQuad and
    // addDetailQuad rather than here, because an assert is not enforcement:
    // NDEBUG is on for the console, which is the only build where reading past
    // the index array does any harm. These stay as a statement of the invariant
    // the emitters now uphold.
    assert(out.quadCount() <= static_cast<usize>(kMaxQuadsPerSection)
           && "the shared index buffer is sized for kMaxQuadsPerSection");
    assert(out.detailQuadCount() <= static_cast<usize>(kMaxQuadsPerSection)
           && "the shared index buffer is sized for kMaxQuadsPerSection");
    assert(out.translucentQuadCount() <= static_cast<usize>(kMaxQuadsPerSection)
           && "the shared index buffer is sized for kMaxQuadsPerSection");
}

// The same cases meshSection's switch names, plus the cube path it takes
// before reaching it. Kept immediately after that function so the two are read
// together, and cross-checked by a test rather than by discipline.
bool hasEmitter(RenderType type)
{
    switch (type) {
        case RenderType::Cube:
        case RenderType::Cross:
        case RenderType::Fluid:
        case RenderType::Torch:
            return true;
        default:
            // Everything else is shapes.cpp's, and it answers for itself so the
            // two lists cannot drift apart.
            return shapeHasEmitter(type);
    }
}

bool sectionIsEmpty(const ChunkColumn& column, int sectionY)
{
    return column.section(sectionY).isUniformAir();
}

}  // namespace mc::mesh
