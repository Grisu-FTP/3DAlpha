#include "core/texture/dev_art.hpp"

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"

#include "version_slots.hpp"

namespace mc::texture {

namespace {

// The six named below are the tiles the `--mesh` run over the real world
// reported as carrying 94 % of its geometry (docs/status.md), so giving those
// natural colours makes terrain read as terrain while everything else stays
// obviously synthetic.
struct NamedTile {
    int tile;
    u8 r, g, b;
};

constexpr NamedTile kNamedTiles[] = {
    {0, 124, 178, 86},    // grass, top -- reachable only through the per-face table
    {1, 125, 125, 125},   // stone
    {2, 140, 105, 74},    // dirt
    {3, 133, 108, 79},    // grass, side
    {17, 60, 60, 62},     // bedrock
    {52, 74, 122, 56},    // leaves
};

// Fluids need a group of tiles to agree, not just one.
//
// A flowing fluid's top face is textured by spinning a tile-sized square about
// the *corner* of its flowing tile, so it reads the 2x2 that starts there --
// see core/mesh/fluid.cpp for the transcription and why it is deliberate.
// a1.1.2's terrain.png fills that 2x2 with the same fluid; a placeholder that
// hashed each tile on its own would put four unrelated colours in one river and
// look like a bug in the mesher.
//
// Derived from the block table rather than written down, so it stays right for
// a version whose water is somewhere else in the atlas: every fluid claims its
// still tile, its flowing tile, and the three around that one.
struct TileGroups {
    // The tile whose noise each tile borrows. Identity except inside a fluid,
    // where the whole group shares the leader's so it reads as one surface.
    u8 leader[256];
    bool fluid[256];
    bool hot[256];         // that fluid emits light, so it is the lava-like one
    bool translucent[256]; // drawn in the blended pass, so it needs an alpha
    bool torch[256];       // a stick cut out of transparency, not a solid tile
};

TileGroups tileGroups()
{
    TileGroups groups{};
    for (int tile = 0; tile < 256; ++tile) {
        groups.leader[tile] = u8(tile);
    }

    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (!def.known) {
            continue;
        }

        // A torch is four full-block quads with the stick carved out of them by
        // the tile's own transparency -- see core/mesh/torch.cpp. A solid
        // placeholder tile therefore draws a torch as a *slab* filling its
        // whole block, which reads as a bug in the emitter and is not one. The
        // tile it claims is face 0, because that is the one the torch renderer
        // asks for; in a1.1.2 no other block shares any of the three.
        if (def.render == block::RenderType::Torch) {
            const int tile = def.faces[mesh::kFaceNegY];
            if (tile >= 0 && tile < 256) {
                groups.torch[tile] = true;
            }
            // Deliberately no colour distinction between the lit torch and the
            // unlit redstone one. Whether a torch glows in a dark cave is the
            // check that the full-bright override works, and baking the
            // difference into the tile would answer it in advance.
            continue;
        }

        if (def.render != block::RenderType::Fluid) {
            continue;
        }

        const int still = def.faces[mesh::kFacePosY];
        const int flowing = def.faces[mesh::kFaceNegZ];
        const int claimed[5] = {still, flowing, flowing + 1, flowing + 16, flowing + 17};

        for (int tile : claimed) {
            if (tile >= 0 && tile < 256) {
                groups.leader[tile] = u8(still);
                groups.fluid[tile] = true;
                // Lava is the fluid that emits light and water is the one that
                // does not, which is the only column in the table that
                // separates them without naming a block.
                groups.hot[tile] = def.light > 0;

                // The block table's own answer, straight from a1.1.2's
                // getRenderBlockPass. A tile drawn in the blended pass at
                // alpha 255 would look exactly like an opaque one, so the
                // placeholder has to carry it or the pass cannot be judged on
                // hardware at all.
                groups.translucent[tile] = def.translucent;
            }
        }
    }
    return groups;
}

// Ours, chosen to read as the material at a glance and deliberately not
// matched to any original texture -- see docs/assets.md.
constexpr NamedTile kWaterTile = {0, 54, 88, 168};
constexpr NamedTile kLavaTile = {0, 190, 88, 24};

// How see-through a translucent placeholder tile is. Ours, and picked so the
// pass is judgeable: opaque enough that a lake reads as a surface, clear enough
// that the sea floor is visible through it, which is the whole thing the
// blended pass exists to show. A real pack supplies its own per-texel alpha.
constexpr u8 kTranslucentAlpha = 150;

// Everything else: a stable hash into a muted palette. Two tiles never collide
// visibly next to each other, and nothing here pretends to be a real texture.
void hashedColour(int tile, u8* r, u8* g, u8* b)
{
    u32 h = u32(tile) * 2654435761u;
    h ^= h >> 15;
    *r = u8(96 + (h & 0x7F));
    *g = u8(96 + ((h >> 8) & 0x7F));
    *b = u8(96 + ((h >> 16) & 0x7F));
}

void tileBase(int tile, const TileGroups& groups, u8* r, u8* g, u8* b)
{
    if (groups.fluid[tile]) {
        const NamedTile& colour = groups.hot[tile] ? kLavaTile : kWaterTile;
        *r = colour.r;
        *g = colour.g;
        *b = colour.b;
        return;
    }

    for (const NamedTile& named : kNamedTiles) {
        if (named.tile == tile) {
            *r = named.r;
            *g = named.g;
            *b = named.b;
            return;
        }
    }
    hashedColour(tile, r, g, b);
}

// Where the stick sits inside a torch tile, in texels, and it is not a choice:
// it is what core/mesh/torch.cpp's geometry samples. The side quads map the
// whole tile across the whole block, so texel column t is at t/16 of a block
// and texel row t is at 1 - t/16 of its height. The stick spans 1/16 either
// side of the centre -- columns 7 and 8 -- and runs from its top at 0.625 of a
// block, which is row 6, down to the floor. The cap quad samples columns 7..9
// of rows 6..8 in the same tile, so the flame has to be opaque there or the
// top of every torch is a hole.
constexpr int kTorchStickU0 = 7;
constexpr int kTorchStickU1 = 9;
constexpr int kTorchFlameV0 = 6;
constexpr int kTorchFlameV1 = 8;

// Writes one texel as four bytes, R,G,B,A in memory order.
//
// **This used to build a u32 as (r<<24)|(g<<16)|(b<<8)|a**, which on a
// little-endian machine is A,B,G,R in memory -- the order GPU_RGBA8 wants. Now
// that a pack's terrain.png can be the source instead, the canonical order here
// is PNG's, and the reversal happens once at upload in
// platform/ctr/textures.cpp. Getting that backwards renders a perfectly
// correct world in the wrong colours.
void writeTexel(u8* dst, u8 r, u8 g, u8 b, u8 a)
{
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = a;
}

void tilePixel(u8* dst, int tile, const TileGroups& groups, int px, int py)
{
    if (groups.torch[tile]) {
        // Transparent everywhere but the stick. The alpha test in the opaque
        // detail pass is what turns four full-block quads into a torch, so
        // this also makes that test visible: if it is off, the torch appears
        // as a solid block-sized box.
        if (px < kTorchStickU0 || px >= kTorchStickU1 || py < kTorchFlameV0) {
            writeTexel(dst, 0, 0, 0, 0);
            return;
        }

        u8 r, g, b;
        tileBase(tile, groups, &r, &g, &b);

        // The flame end is drawn brighter than the handle so the cap quad and
        // the stick's orientation can be told apart at a glance -- an upside
        // down torch is otherwise a symmetric bar.
        if (py < kTorchFlameV1) {
            const auto lift = [](u8 v) { return u8(v > 155 ? 255 : v + 100); };
            writeTexel(dst, lift(r), lift(g), lift(b), 255);
            return;
        }
        writeTexel(dst, r, g, b, 255);
        return;
    }

    u8 r, g, b;
    tileBase(tile, groups, &r, &g, &b);
    const u8 a = groups.translucent[tile] ? kTranslucentAlpha : 255;

    // Jitter follows the group, not the tile, so a fluid's four tiles are one
    // continuous surface rather than four squares that happen to share a hue.
    //
    // **Multiplied in u32, and that is a fix rather than a style choice.** These
    // three products overflow `int` for almost every input, which is undefined
    // behaviour -- UBSan reports `26 * 83492791` on the first tile it reaches.
    // It went unseen for as long as this lived in platform/ctr/, where nothing
    // is sanitised; moving it into core is what found it. Unsigned multiply
    // wraps by definition and produces the same 32 bits the signed one happened
    // to produce, so the generated art is byte-identical to what the console
    // has been drawing.
    const u32 seed = groups.leader[tile];
    u32 n = (u32(px) * 73856093u) ^ (u32(py) * 19349663u) ^ (seed * 83492791u);
    n ^= n >> 13;
    const int jitter = int(n & 31) - 16;

    const auto clamp = [](int v) { return u8(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    writeTexel(dst, clamp(int(r) + jitter), clamp(int(g) + jitter), clamp(int(b) + jitter), a);
}

}  // namespace

// How far in from the tile's edge the blob starts, in texels. Two leaves a
// 12x12 shape inside a 16x16 tile, which reads as "smaller than a block" at the
// sixteen pixels a slot actually draws.
constexpr int kItemInset = 2;

void buildDevArt(std::vector<u8>* rgba)
{
    rgba->assign(kAtlasBytes, 0);

    const TileGroups groups = tileGroups();

    for (int y = 0; y < kAtlasEdge; ++y) {
        for (int x = 0; x < kAtlasEdge; ++x) {
            const int tile = (y / kAtlasTilePixels) * kAtlasTilesPerEdge + (x / kAtlasTilePixels);
            tilePixel(rgba->data() + (usize(y) * kAtlasEdge + usize(x)) * 4, tile, groups,
                      x % kAtlasTilePixels, y % kAtlasTilePixels);
        }
    }
}

void buildDevArtItems(std::vector<u8>* rgba)
{
    rgba->assign(kAtlasBytes, 0);

    const int radius = kAtlasTilePixels / 2 - kItemInset;
    const int centre = kAtlasTilePixels / 2;

    for (int y = 0; y < kAtlasEdge; ++y) {
        for (int x = 0; x < kAtlasEdge; ++x) {
            const int tile = (y / kAtlasTilePixels) * kAtlasTilesPerEdge + (x / kAtlasTilePixels);
            const int px = x % kAtlasTilePixels;
            const int py = y % kAtlasTilePixels;

            // A disc, measured from between the two centre texels so it comes
            // out symmetric on an even-sized tile.
            const int dx = px - centre;
            const int dy = py - centre;
            const int inside = dx * dx + dy * dy;
            u8* dst = rgba->data() + (usize(y) * kAtlasEdge + usize(x)) * 4;
            if (inside > radius * radius) {
                writeTexel(dst, 0, 0, 0, 0);
                continue;
            }

            u8 r, g, b;
            // The same hash the terrain placeholder uses, offset so an item and
            // the block of the same index are not the same colour -- a door
            // item that matched the door block would hide exactly the mistake
            // this sheet was added to make visible.
            hashedColour(tile + kAtlasTilesPerEdge * kAtlasTilesPerEdge, &r, &g, &b);
            // A highlight towards the top left, so a round blob reads as an
            // object rather than as a dot.
            const bool lit = dx + dy < -radius / 2;
            const auto lift = [](u8 v) { return u8(v > 195 ? 255 : v + 60); };
            if (lit) {
                writeTexel(dst, lift(r), lift(g), lift(b), 255);
            } else {
                writeTexel(dst, r, g, b, 255);
            }
        }
    }
}

}  // namespace mc::texture
