#include "platform/ctr/textures.hpp"

#include "core/block/registry.hpp"
#include "core/mesh/vertex.hpp"
#include "core/world/daylight.hpp"

#include <3ds.h>

#include <cmath>

namespace mc::ctr {

namespace {

// GPU_RGBA8 wants A,B,G,R in memory order, which on a little-endian machine is
// this word. Established by the M0 probe on hardware.
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255)
{
    return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a;
}

// PICA textures are stored in 8x8 tiles, Morton-ordered within each tile, and
// bottom-up. This is the CPU path, validated on hardware by the M0 probe; the
// atlas uses the GPU instead, but the lightmap is 16x16 and a display transfer
// will not go below 64x64.
u32 mortonInterleave(u32 x, u32 y)
{
    u32 i = (x & 7) | ((y & 7) << 8);
    i = (i ^ (i << 2)) & 0x1313;
    i = (i ^ (i << 1)) & 0x1515;
    i = (i | (i >> 7)) & 0x3F;
    return i;
}

u32 tiledOffset(u32 x, u32 y, u32 width)
{
    return mortonInterleave(x, y) + (x & ~7u) * 8 + (y & ~7u) * width;
}

// **v = 0 samples the LAST row in memory, not the first.** Everything the CPU
// writes into a texture has to invert its row index; nothing warns you if it
// does not, and the failure is a picture that is upside down rather than a
// picture that is missing.
//
// Derived rather than asserted, because the two halves of the evidence are in
// different files. `Atlas::init` puts tile 0 at source row 0 and hands the
// buffer to a display transfer with `GX_TRANSFER_FLIP_VERT(1)`, which sends
// source row 0 to the *last* row of the texture. `mesher.cpp` gives tile 0 a v
// of ~0. The atlas is right on hardware, so those two only agree if v = 0
// reads the last row.
//
// The lightmap did not do this and was inverted on hardware in the most
// legible way possible: caves and sea floors lit like open sky, open sky lit
// like a cave. The M0 probe validated this Morton path but not its
// orientation -- it drew one texture and never asked which way up it was.
u32 tiledOffsetFlipped(u32 x, u32 row, u32 width, u32 height)
{
    return tiledOffset(x, height - 1 - row, width);
}

// ---------------------------------------------------------------------------
// The placeholder pack
// ---------------------------------------------------------------------------
//
// **These colours are ours, not Mojang's.** No asset is extracted, decoded or
// shipped here; docs/assets.md is explicit that the bundled fallback is a
// CC BY-SA pack in RomFS, and that pipeline is not built yet. Until it is, the
// atlas is generated so the game is playable and -- more useful right now --
// so a wrong texture index is *visible*: every tile gets a stable distinct
// colour rather than a plausible one.
//
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

// Enough per-pixel variation that a surface is not a flat plane of colour --
// which matters for judging whether lighting and fog are working at all.
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

u32 tilePixel(int tile, const TileGroups& groups, int px, int py)
{
    if (groups.torch[tile]) {
        // Transparent everywhere but the stick. The alpha test in the opaque
        // detail pass is what turns four full-block quads into a torch, so
        // this also makes that test visible: if it is off, the torch appears
        // as a solid block-sized box.
        if (px < kTorchStickU0 || px >= kTorchStickU1 || py < kTorchFlameV0) {
            return rgba(0, 0, 0, 0);
        }

        u8 r, g, b;
        tileBase(tile, groups, &r, &g, &b);

        // The flame end is drawn brighter than the handle so the cap quad and
        // the stick's orientation can be told apart at a glance -- an upside
        // down torch is otherwise a symmetric bar.
        if (py < kTorchFlameV1) {
            const auto lift = [](u8 v) { return u8(v > 155 ? 255 : v + 100); };
            return rgba(lift(r), lift(g), lift(b), 255);
        }
        return rgba(r, g, b, 255);
    }

    u8 r, g, b;
    tileBase(tile, groups, &r, &g, &b);
    const u8 a = groups.translucent[tile] ? kTranslucentAlpha : 255;

    // Jitter follows the group, not the tile, so a fluid's four tiles are one
    // continuous surface rather than four squares that happen to share a hue.
    const int seed = groups.leader[tile];
    u32 n = u32(px * 73856093) ^ u32(py * 19349663) ^ u32(seed * 83492791);
    n ^= n >> 13;
    const int jitter = int(n & 31) - 16;

    const auto clamp = [](int v) { return u8(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    return rgba(clamp(int(r) + jitter), clamp(int(g) + jitter), clamp(int(b) + jitter), a);
}

// Fills a tiled texture from a linear image the caller has just written.
//
// The GPU does the Morton swizzle. This is the path the texture-pack importer
// will use for real images -- swizzling a 256 KB atlas on a 268 MHz ARM11 is
// milliseconds the loading screen does not need to spend, and the display
// transfer engine does it for free while the CPU moves on. FLIP_VERT is part of
// the idiom, not an accident: the transfer engine treats its source as
// bottom-up, and textures are sampled bottom-up too.
void uploadAtlasImage(const u32* linear, C3D_Tex* tex, usize bytes)
{
    GSPGPU_FlushDataCache(const_cast<u32*>(linear), u32(bytes));
    C3D_SyncDisplayTransfer(
        const_cast<u32*>(linear), GX_BUFFER_DIM(kAtlasEdge, kAtlasEdge),
        static_cast<u32*>(tex->data), GX_BUFFER_DIM(kAtlasEdge, kAtlasEdge),
        GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0)
            | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
            | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8)
            | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
}

}  // namespace

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

bool Atlas::init()
{
    // VRAM first: the atlas is sampled by every fragment of every chunk, so it
    // is the one texture where the bandwidth difference is worth the space.
    inVram_ = C3D_TexInitVRAM(&tex_, kAtlasEdge, kAtlasEdge, GPU_RGBA8);
    if (!inVram_ && !C3D_TexInit(&tex_, kAtlasEdge, kAtlasEdge, GPU_RGBA8)) {
        return false;
    }

    // The source has to be linear memory for the display transfer to read it.
    const usize size = bytes();
    u32* linear = static_cast<u32*>(linearAlloc(size));
    if (linear == nullptr) {
        C3D_TexDelete(&tex_);
        return false;
    }

    const TileGroups groups = tileGroups();

    for (int y = 0; y < kAtlasEdge; ++y) {
        for (int x = 0; x < kAtlasEdge; ++x) {
            const int tile = (y / kAtlasTilePixels) * kAtlasTilesPerEdge + (x / kAtlasTilePixels);
            linear[y * kAtlasEdge + x] =
                tilePixel(tile, groups, x % kAtlasTilePixels, y % kAtlasTilePixels);
        }
    }

    uploadAtlasImage(linear, &tex_, size);
    linearFree(linear);

    // Nearest, and no mip chain yet. Minecraft's look depends on nearest
    // filtering; mipmaps are a separate decision because a naive chain bleeds
    // neighbouring tiles into each other across the atlas.
    C3D_TexSetFilter(&tex_, GPU_NEAREST, GPU_NEAREST);
    // Clamp, not repeat: a quad's UVs never leave its tile, and repeat would
    // turn a rounding error at a tile edge into a wrap to the far side of the
    // atlas.
    C3D_TexSetWrap(&tex_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    ready_ = true;
    return true;
}

bool Atlas::ensureWireframe()
{
    if (wireReady_) {
        return true;
    }
    if (!C3D_TexInit(&wire_, kAtlasEdge, kAtlasEdge, GPU_RGBA8)) {
        return false;
    }

    const usize size = bytes();
    u32* linear = static_cast<u32*>(linearAlloc(size));
    if (linear == nullptr) {
        C3D_TexDelete(&wire_);
        return false;
    }

    for (int y = 0; y < kAtlasEdge; ++y) {
        for (int x = 0; x < kAtlasEdge; ++x) {
            const int px = x % kAtlasTilePixels;
            const int py = y % kAtlasTilePixels;
            const bool edge = px == 0 || py == 0 || px == kAtlasTilePixels - 1
                              || py == kAtlasTilePixels - 1;
            // Alpha 0 inside, because the alpha test is what makes this see
            // through to the geometry behind rather than a solid model with
            // stripes on it. White outside, so face shade -- which stage 0
            // still multiplies in -- is the only thing tinting the lines and
            // the six face directions stay tellable apart.
            linear[y * kAtlasEdge + x] = edge ? rgba(255, 255, 255, 255) : rgba(0, 0, 0, 0);
        }
    }

    uploadAtlasImage(linear, &wire_, size);
    linearFree(linear);

    // Nearest, so a line is a line at any distance rather than a grey smear.
    C3D_TexSetFilter(&wire_, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&wire_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    wireReady_ = true;
    return true;
}

void Atlas::shutdown()
{
    if (ready_) {
        C3D_TexDelete(&tex_);
        ready_ = false;
    }
    if (wireReady_) {
        C3D_TexDelete(&wire_);
        wireReady_ = false;
    }
}

// ---------------------------------------------------------------------------
// Lightmap
// ---------------------------------------------------------------------------

bool Lightmap::init()
{
    if (!C3D_TexInit(&tex_, 16, 16, GPU_RGBA8)) {
        return false;
    }
    // Linear filtering across light levels, and clamped: level 15 must not wrap
    // round to 0.
    C3D_TexSetFilter(&tex_, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&tex_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    ready_ = true;
    setSkyDarken(0);
    return true;
}

void Lightmap::shutdown()
{
    if (ready_) {
        C3D_TexDelete(&tex_);
        ready_ = false;
    }
}

void Lightmap::setSkyDarken(int subtracted)
{
    if (!ready_) {
        return;
    }
    // Alpha's subtraction is an integer 0..11, so there are twelve distinct
    // textures in a whole day and this rebuilds on twelve frames of the ~72,000
    // in one. The old float compare was guarding against a cost that the real
    // curve does not have.
    if (subtracted == lastSubtracted_) {
        return;
    }
    lastSubtracted_ = subtracted;

    u32* dst = static_cast<u32*>(tex_.data);
    for (int sky = 0; sky < 16; ++sky) {
        for (int block = 0; block < 16; ++block) {
            // Exactly what the original does per block: take the day's
            // subtraction off the stored sky light, keep whichever of that and
            // the block light is brighter, and look it up. Baking it into the
            // texture is the only difference, and it is why moving the sun
            // costs 256 texels instead of re-meshing the world.
            const int level = world::effectiveLightLevel(sky, block, subtracted);
            const float v = world::lightBrightness(level);

            // Monochrome, because a1.1.2's table is. Torchlight is not warmer
            // than sunlight until later versions give it its own curve.
            const u8 c = u8(v * 255.0f + 0.5f);
            // Row `sky` in texture space, which is not row `sky` in memory.
            dst[tiledOffsetFlipped(u32(block), u32(sky), 16, 16)] = rgba(c, c, c);
        }
    }
    C3D_TexFlush(&tex_);
}

}  // namespace mc::ctr
