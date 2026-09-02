#include "core/mesh/mesher.hpp"

#include "core/block/registry.hpp"
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

void MeshBuilder::addQuad(int x, int y, int z, int face, u16 texture, u8 light)
{
    assert(face >= 0 && face < kFaceCount);

    // Every pass draws through one shared index buffer sized for
    // kMaxQuadsPerSection, so a stream that runs past it would have the GPU
    // read indices that do not exist. Dropping the quad loses a face; not
    // dropping it fetches vertices from outside the bound buffer. See the
    // clamp in Renderer::drawPass for the other half of this.
    if (quadCount() >= usize(kMaxQuadsPerSection)) {
        ++dropped_;
        return;
    }

    // A texture index outside the atlas can only come from a table we generated
    // wrong, but a world can name a block we do not know and the unknown entry
    // has to land somewhere real rather than sample past the atlas.
    const int tile = texture < kAtlasTileCount ? texture : 0;
    // Inset by kUvInset at each edge rather than sitting on the tile boundary;
    // see vertex.hpp for the one texel row of every face that cost.
    const i16 uLo = tileUvMin(tile % kAtlasTilesPerEdge);
    const i16 uHi = tileUvMax(tile % kAtlasTilesPerEdge);
    const i16 vLo = tileUvMin(tile / kAtlasTilesPerEdge);
    const i16 vHi = tileUvMax(tile / kAtlasTilesPerEdge);

    // One quad, and everything else -- the four corners, their UVs, the face
    // shade -- is rebuilt on the GPU from kFaceBasis, which is asserted against
    // the corner tables the branch below reads.
    if (cubeFormat_ == CubeFormat::Quads) {
        QuadVertex q;
        q.x = static_cast<u8>(x);
        q.y = static_cast<u8>(y);
        q.z = static_cast<u8>(z);
        q.face = static_cast<u8>(face);
        q.tileX = static_cast<u8>(tile % kAtlasTilesPerEdge);
        q.tileY = static_cast<u8>(tile / kAtlasTilesPerEdge);
        q.light = light;
        q.ao = 0;
        quads_.push_back(q);
        return;
    }

    const u8 shade = kFaceShade[face];

    for (int c = 0; c < 4; ++c) {
        const Corner& corner = kFaceCorner[face][c];

        WorldVertex v;
        v.u = kFaceCornerUV[face][c][0] != 0 ? uHi : uLo;
        v.v = kFaceCornerUV[face][c][1] != 0 ? vHi : vLo;
        v.x = static_cast<u8>(x + corner.x);
        v.y = static_cast<u8>(y + corner.y);
        v.z = static_cast<u8>(z + corner.z);
        v.face = static_cast<u8>(face);

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

                // Each render type is its own emitter. The ones with no emitter
                // yet are skipped rather than drawn as cubes: a missing ladder
                // is obvious, a cubic one looks deliberate.
                //
                // hasEmitter() below lists exactly the cases named here. Adding
                // one without the other is what a test in mesher_test.cpp
                // catches.
                if (def.render != RenderType::Cube) {
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
                            break;
                    }
                    continue;
                }

                for (int face = 0; face < kFaceCount; ++face) {
                    const FaceOffset& offset = kFaceOffset[face];
                    const int nx = x + offset.dx;
                    const int ny = y + offset.dy;
                    const int nz = z + offset.dz;

                    // The original's rule, and the only one: a face is drawn
                    // unless the block against it is an opaque cube. Glass and
                    // leaves are full cubes that are not opaque, so their
                    // interior faces survive -- which is correct for alpha,
                    // where glass panes had not been invented.
                    if (block::def(scratch.block(nx, ny, nz)).opaque) {
                        continue;
                    }

                    // Light comes from the cell the face looks into, not from
                    // the block itself, which is always dark inside.
                    // Per-face, so grass is grass on top and dirt underneath.
                    // Uniform blocks carry six copies, so there is nothing to
                    // branch on here.
                    out.addQuad(x, y, z, face, def.faces[face],
                                scratch.light(nx, ny, nz));
                }
            }
        }
    }

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
            return false;
    }
}

bool sectionIsEmpty(const ChunkColumn& column, int sectionY)
{
    return column.section(sectionY).isUniformAir();
}

}  // namespace mc::mesh
