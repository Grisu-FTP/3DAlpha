#include "platform/ctr/textures.hpp"

#include "core/texture/tiled.hpp"
#include "core/world/daylight.hpp"

#include <3ds.h>

#include <cmath>

namespace mc::ctr {

namespace {

using texture::tiledOffsetFlipped;

// GPU_RGBA8 wants A,B,G,R in memory order, which on a little-endian machine is
// this word. Established by the M0 probe on hardware.
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255)
{
    return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a;
}

// Copies an already-tiled, already-flipped staging buffer into a texture.
//
// **This used to be a display transfer that did the tiling and the flip
// itself** -- `GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1)` over a
// linear source, into VRAM -- and the texture came out with its **last row of
// memory wrong**. On hardware that is the row `v = 0` samples, which is
// terrain.png's *first* row, which is the top texel row of every tile in atlas
// row 0 and of no other tile: grass, dirt, stone and the flowers drew with a
// gray line across the top of every face and nothing else in the world was
// touched. Dev Art hid it for as long as it was the only pack, because one
// wrong row in a flat colour tile is invisible. See docs/status.md.
//
// **Which half of that step lost the row was never isolated**, and this does
// not claim to know: the transfer was doing a tiling, a flip and a format
// conversion at once, and the destination was VRAM. What the rewrite does is
// remove every one of those from the upload rather than pick between them.
//
// So the tiling and the flip happen on the CPU now, through the same
// `tiledOffsetFlipped` the lightmap has always used and the M0 probe's own
// atlas used before that, and what is left here moves bytes that are already
// final: no format conversion, no tiling, no flip, nothing with an orientation
// left to get wrong.
//
// `C3D_TexUpload` is the move rather than a memcpy because the destination may
// be VRAM, which the CPU cannot store into (platform/ctr/gpu_memory.cpp). Read
// out of `libcitro3d.a`: it range-checks the destination against
// [0x1F000000, +0x600000) and routes VRAM through `C3D_SyncTextureCopy`,
// falling back to memcpy for ordinary linear memory. It does **not** flush the
// source first, which is what the line above is for. `C3D_TexFlush` carries the
// same VRAM check and so is a no-op for the case memcpy did not run.
void uploadTiled(const u32* tiled, C3D_Tex* tex, usize bytes)
{
    GSPGPU_FlushDataCache(const_cast<u32*>(tiled), u32(bytes));
    C3D_TexUpload(tex, tiled);
    C3D_TexFlush(tex);
}

}  // namespace

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

bool Atlas::init(const texture::AtlasImage& image)
{
    if (image.empty()) {
        // core/texture/ builds Dev Art for an empty pack path and never hands
        // back a short buffer, so this is a caller that skipped that step
        // rather than a pack that failed. Refusing beats uploading whatever is
        // in the staging buffer.
        return false;
    }

    // VRAM first: the atlas is sampled by every fragment of every chunk, so it
    // is the one texture where the bandwidth difference is worth the space.
    inVram_ = C3D_TexInitVRAM(&tex_, kAtlasEdge, kAtlasEdge, GPU_RGBA8);
    if (!inVram_ && !C3D_TexInit(&tex_, kAtlasEdge, kAtlasEdge, GPU_RGBA8)) {
        return false;
    }

    // The staging buffer has to be linear memory: it is what the GPU reads
    // when the destination is VRAM.
    const usize size = bytes();
    u32* tiled = static_cast<u32*>(linearAlloc(size));
    if (tiled == nullptr) {
        C3D_TexDelete(&tex_);
        return false;
    }

    // **The byte order changes here, and only here.** core/texture/ hands over
    // R,G,B,A in memory -- PNG's own order, which is what makes a decoded pack
    // and the generated Dev Art interchangeable. GPU_RGBA8 reads A,B,G,R in
    // memory, which on this little-endian machine is the word below. Getting it
    // backwards renders a perfectly correct world in the wrong colours, and
    // nothing upstream can tell.
    //
    // **The tiling and the vertical flip happen here too**, in the same pass,
    // because the pass was already walking every texel and the display transfer
    // that used to do them lost a row. `tiledOffsetFlipped` is the whole of it:
    // source row 0 -- terrain.png's top row -- lands in the last row of memory,
    // which is the row `v = 0` samples.
    const u8* src = image.rgba.data();
    for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
        for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
            const usize i = usize(y) * kAtlasEdge + x;
            const u8 r = src[i * 4 + 0];
            const u8 g = src[i * 4 + 1];
            const u8 b = src[i * 4 + 2];
            const u8 a = src[i * 4 + 3];
            tiled[tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge)] = rgba(r, g, b, a);
        }
    }

    uploadTiled(tiled, &tex_, size);
    linearFree(tiled);

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
    u32* tiled = static_cast<u32*>(linearAlloc(size));
    if (tiled == nullptr) {
        C3D_TexDelete(&wire_);
        return false;
    }

    for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
        for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
            const u32 px = x % kAtlasTilePixels;
            const u32 py = y % kAtlasTilePixels;
            const bool edge = px == 0 || py == 0 || px == u32(kAtlasTilePixels) - 1
                              || py == u32(kAtlasTilePixels) - 1;
            // Alpha 0 inside, because the alpha test is what makes this see
            // through to the geometry behind rather than a solid model with
            // stripes on it. White outside, so face shade -- which stage 0
            // still multiplies in -- is the only thing tinting the lines and
            // the six face directions stay tellable apart.
            //
            // Tiled and flipped like the real atlas. This pattern is symmetric,
            // so it would look identical written straight -- which is exactly
            // why it has to go through the same map: a second copy of the old
            // upload left here is a bug waiting for the first asymmetric debug
            // texture anyone adds.
            tiled[tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge)] =
                edge ? rgba(255, 255, 255, 255) : rgba(0, 0, 0, 0);
        }
    }

    uploadTiled(tiled, &wire_, size);
    linearFree(tiled);

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
