#include "platform/ctr/textures.hpp"

#include "core/mesh/cube_atlas.hpp"
#include "core/texture/tiled.hpp"
#include "core/world/daylight.hpp"

#include <3ds.h>

#include <cmath>
#include <cstring>

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

// The cube atlas goes up one slot row at a time: 64 image rows, which in the
// PICA's layout is one contiguous run of 64 memory rows -- rows advance by whole
// 8-row strips of `edge * 8` words, and 64 is eight of them. 128 KB, so the
// ordinary atlas's 256 KB staging buffer carries it in eight moves and the
// upload never asks the linear heap for a megabyte at once. That matters on a
// texture-pack change, which runs with the chunk pool already full.
constexpr u32 kCubeEdge = u32(mesh::kCubeAtlasEdge);
constexpr u32 kCubeBandRows = u32(mesh::kCubeSlotPixels);
constexpr u32 kCubeBandWords = kCubeEdge * kCubeBandRows;
constexpr usize kCubeBandBytes = usize(kCubeBandWords) * sizeof(u32);
static_assert(kCubeBandRows % 8 == 0, "a band has to be whole strips of 8x8 tiles");
static_assert(kCubeBandBytes <= usize(kAtlasEdge) * kAtlasEdge * 4,
              "a cube-atlas band must fit the ordinary atlas's staging buffer");

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

    // **The cube atlas asks for VRAM first**, ahead of this one: since greedy
    // meshing it is the texture the cube pass samples, and the cube pass is
    // nearly every fragment of the world. The ordinary atlas is what the
    // detail passes, items and particles sample -- a few percent of it. Both
    // fit together on any console; the order only decides which one is
    // demoted if one day they do not.
    cubeInVram_ = C3D_TexInitVRAM(&cube_, u16(kCubeEdge), u16(kCubeEdge), GPU_RGBA8);
    if (!cubeInVram_ && !C3D_TexInit(&cube_, u16(kCubeEdge), u16(kCubeEdge), GPU_RGBA8)) {
        return false;
    }

    // VRAM next for this one: it is still sampled by every detail fragment.
    inVram_ = C3D_TexInitVRAM(&tex_, kAtlasEdge, kAtlasEdge, GPU_RGBA8);
    if (!inVram_ && !C3D_TexInit(&tex_, kAtlasEdge, kAtlasEdge, GPU_RGBA8)) {
        C3D_TexDelete(&cube_);
        return false;
    }

    // The staging buffer has to be linear memory: it is what the GPU reads
    // when the destination is VRAM.
    const usize size = bytes();
    u32* tiled = static_cast<u32*>(linearAlloc(size));
    if (tiled == nullptr) {
        C3D_TexDelete(&tex_);
        C3D_TexDelete(&cube_);
        return false;
    }

    // **A second staging buffer, and it is the whole of the rail fix.**
    //
    // This function used to hand `tiled` to the ordinary atlas's upload and
    // then, on the very next line, hand the same buffer to `initCube` to be
    // overwritten with cube-atlas bands. When the atlas is in VRAM that upload
    // is not a memcpy: `C3D_TexUpload` -> `C3D_TexLoadImage` range-checks the
    // destination against [0x1F000000, +0x600000) and routes VRAM through
    // `C3D_SyncTextureCopy`, which enqueues a GPU texture copy. So the CPU went
    // back to writing the buffer the copy engine was still reading out of.
    //
    // **The symptom was rails, and the arithmetic says it had to be.** Band 0
    // writes `staging[tiledOffsetFlipped(x, y, 512, 512) - base]` for the
    // 64 image rows at the top of the cube atlas. Worked back through the
    // *ordinary* atlas's own tiled-and-flipped map, the first words it touches
    // are x = 0..15 of image row 136 -- which is atlas row 8, column 0, which
    // is **tile 128, the rail**. The tiles after it, in order, are 129, 130,
    // 131 and up: the rail is not one of the casualties, it is the first one.
    //
    // What arrives in those words is cube-atlas slot row 0, and the layout
    // hands slots out in ascending tile order, so slot 0 is tile 0 (grass top,
    // 97,161,55) and slot 4 is tile 4 (planks, 188,152,98) -- bright green and
    // bright brown, which is exactly how this was reported. And because it is a
    // race between the CPU's stores and the copy engine's reads, *which* texels
    // lose changes from load to load; it moved when the world was rejoined.
    //
    // The fix is to never let `initCube` write a buffer that may still be in
    // flight. Band 0 -- the dangerous one, the one that lands on the rail --
    // goes into a buffer the ordinary upload never touched, and the bands
    // alternate from there so a band's copy always has a full band of CPU work
    // behind it before its buffer comes round again. One band, not a second
    // full atlas: 128 KB, which matters because `Renderer::setAtlas` re-enters
    // this on a texture-pack change with the chunk pool already full.
    u32* spare = static_cast<u32*>(linearAlloc(kCubeBandBytes));
    if (spare == nullptr) {
        linearFree(tiled);
        C3D_TexDelete(&tex_);
        C3D_TexDelete(&cube_);
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

    // The cube atlas next. `spare` first so band 0 cannot land on the upload
    // above while it is still draining; see the note on the allocation.
    const bool cubeOk = initCube(image, spare, tiled);
    linearFree(spare);
    linearFree(tiled);
    if (!cubeOk) {
        C3D_TexDelete(&tex_);
        C3D_TexDelete(&cube_);
        return false;
    }

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

bool Atlas::initCube(const texture::AtlasImage& image, u32* spare, u32* shared)
{
    const u8* src = image.rgba.data();
    u32* dst = static_cast<u32*>(cube_.data);

    for (u32 band = 0; band < kCubeEdge / kCubeBandRows; ++band) {
        // **Alternate, and start on `spare`.** Two separate races are closed by
        // the same line. The first is band 0 against the ordinary atlas's own
        // upload, which is still draining out of `shared` when this is reached
        // -- that is the one that ate the rail tile, and it is why band 0 must
        // not be `shared`. The second is band N's copy against band N+1's
        // stores, which the old single-buffer loop had just as squarely: the
        // copy below is enqueued, not finished, when the next iteration starts
        // refilling the buffer underneath it. See the note in `init`.
        u32* staging = (band & 1) == 0 ? spare : shared;

        // Image rows [top, top + 64) are memory rows [kCubeEdge - 64 - top,
        // kCubeEdge - top) once flipped -- which run the band's words start at.
        const u32 top = band * kCubeBandRows;
        const u32 base = (kCubeEdge - kCubeBandRows - top) * kCubeEdge;

        for (u32 y = top; y < top + kCubeBandRows; ++y) {
            for (u32 x = 0; x < kCubeEdge; ++x) {
                int ax = 0;
                int ay = 0;
                u32 texel = rgba(0, 0, 0, 0);
                if (mesh::cubeAtlasSource(int(x), int(y), &ax, &ay)) {
                    const usize i = (usize(ay) * kAtlasEdge + usize(ax)) * 4;
                    texel = rgba(src[i + 0], src[i + 1], src[i + 2], src[i + 3]);
                }
                staging[texture::tiledOffsetFlipped(x, y, kCubeEdge, kCubeEdge) - base] = texel;
            }
        }

        GSPGPU_FlushDataCache(staging, u32(kCubeBandBytes));
        if (cubeInVram_) {
            // The raw copy updateTileOf uses, on 128 KB rather than 512 bytes:
            // no tiling, no conversion, nothing with an orientation to get
            // wrong.
            //
            // **The name promises more than the function delivers, which is
            // why the buffers alternate above.** Read out of `libcitro3d.a`:
            // it branches on citro3d's in-frame flag, and with a frame open it
            // splits the frame and tail-calls `GX_TextureCopy` with no wait at
            // all. With no frame open it does reach `gspWaitForEvent`, but on
            // `GSPGPU_EVENT_PPF` with `nextEvent = false` -- which libctru
            // documents as returning immediately when an unconsumed event of
            // that kind is already pending. A PPF left over from any earlier
            // transfer therefore satisfies the wait without this copy having
            // moved a byte. Neither path is a guarantee the buffer is free, so
            // this does not lean on one.
            C3D_SyncTextureCopy(staging, 0, dst + base, 0, u32(kCubeBandBytes), 8);
        } else {
            std::memcpy(dst + base, staging, kCubeBandBytes);
        }
    }
    if (!cubeInVram_) {
        C3D_TexFlush(&cube_);
    }

    // Nearest and clamped, for exactly the reasons the ordinary atlas is. A
    // merged quad never leaves its slot, so repeat would buy nothing and would
    // turn a rounding error at the atlas's edge into a wrap to the far side.
    C3D_TexSetFilter(&cube_, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&cube_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    cubeReady_ = true;
    return true;
}

bool Atlas::initItems(const texture::AtlasImage& image)
{
    if (!image.hasItems()) {
        return false;
    }
    if (itemsReady_) {
        C3D_TexDelete(&items_);
        itemsReady_ = false;
    }
    // Linear memory rather than VRAM -- see the note on the declaration.
    if (!C3D_TexInit(&items_, kAtlasEdge, kAtlasEdge, GPU_RGBA8)) {
        return false;
    }

    const usize size = bytes();
    u32* tiled = static_cast<u32*>(linearAlloc(size));
    if (tiled == nullptr) {
        C3D_TexDelete(&items_);
        return false;
    }

    // The same byte reversal, the same tiling and the same vertical flip the
    // block atlas gets, for the same three reasons -- and it has to be the
    // same, because the mesher's UV convention does not know which sheet it is
    // sampling.
    const u8* src = image.itemsRgba.data();
    for (u32 y = 0; y < u32(kAtlasEdge); ++y) {
        for (u32 x = 0; x < u32(kAtlasEdge); ++x) {
            const usize i = usize(y) * kAtlasEdge + x;
            tiled[tiledOffsetFlipped(x, y, kAtlasEdge, kAtlasEdge)] =
                rgba(src[i * 4 + 0], src[i * 4 + 1], src[i * 4 + 2], src[i * 4 + 3]);
        }
    }

    uploadTiled(tiled, &items_, size);
    linearFree(tiled);

    C3D_TexSetFilter(&items_, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&items_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    itemsReady_ = true;
    return true;
}

// `pixels` rather than `rgba`, because `rgba` is the byte-reversing helper this
// body calls and shadowing it here would be a silent miscolour.
bool Atlas::uploadSheet(C3D_Tex* tex, const u8* pixels, int width, int height)
{
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (!C3D_TexInit(tex, u16(width), u16(height), GPU_RGBA8)) {
        return false;
    }

    const usize size = usize(width) * usize(height) * 4;
    u32* tiled = static_cast<u32*>(linearAlloc(size));
    if (tiled == nullptr) {
        C3D_TexDelete(tex);
        return false;
    }

    // The same byte reversal, the same Morton tiling and the same vertical
    // flip every other sheet gets. It has to be the same: the vertex format's
    // UV convention does not know which texture it is sampling, so a sheet
    // uploaded any other way would be upside down against the same numbers.
    for (u32 y = 0; y < u32(height); ++y) {
        for (u32 x = 0; x < u32(width); ++x) {
            const usize i = usize(y) * usize(width) + x;
            tiled[tiledOffsetFlipped(x, y, u32(width), u32(height))] =
                rgba(pixels[i * 4 + 0], pixels[i * 4 + 1], pixels[i * 4 + 2],
                     pixels[i * 4 + 3]);
        }
    }

    uploadTiled(tiled, tex, size);
    linearFree(tiled);

    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return true;
}

bool Atlas::initIcons(const texture::AtlasImage& image)
{
    if (iconsReady_) {
        C3D_TexDelete(&icons_);
        iconsReady_ = false;
    }
    if (!image.hasIcons()) {
        return false;
    }
    constexpr int kIconEdge = 256;
    iconsReady_ = uploadSheet(&icons_, image.iconsRgba.data(), kIconEdge, kIconEdge);
    return iconsReady_;
}

bool Atlas::initEntitySheets(const texture::AtlasImage& image)
{
    if (entityReady_) {
        C3D_TexDelete(&entity_);
        entityReady_ = false;
    }
    if (artReady_) {
        C3D_TexDelete(&art_);
        artReady_ = false;
    }

    if (image.entityRgba.size() == texture::kEntitySheetBytes) {
        entityReady_ = uploadSheet(&entity_, image.entityRgba.data(),
                                   texture::kEntitySheetWidth, texture::kEntitySheetHeight);
    }
    // The art sheet is square and 256, which `kAtlasEdge` also happens to be --
    // stated as its own number rather than borrowed, because the two are the
    // same size by coincidence and not by rule.
    constexpr int kArtEdge = 256;
    if (image.artRgba.size() == usize(kArtEdge) * kArtEdge * 4) {
        artReady_ = uploadSheet(&art_, image.artRgba.data(), kArtEdge, kArtEdge);
    }
    return entityReady_ && artReady_;
}

bool Atlas::initFont(const texture::FontImage& font)
{
    if (fontReady_) {
        C3D_TexDelete(&font_);
        fontReady_ = false;
    }
    if (font.empty()) {
        return false;
    }
    fontReady_ = uploadSheet(&font_, font.rgba.data(), texture::kFontEdge,
                             texture::kFontEdge);
    return fontReady_;
}

bool Atlas::initParticles(const std::vector<u8>& sheet)
{
    if (particlesReady_) {
        C3D_TexDelete(&particles_);
        particlesReady_ = false;
    }
    if (sheet.size() != texture::kParticleSheetBytes) {
        return false;
    }
    particlesReady_ = uploadSheet(&particles_, sheet.data(), texture::kParticleSheetEdge,
                                  texture::kParticleSheetEdge);
    return particlesReady_;
}

bool Atlas::initWaterOverlay(const std::vector<u8>& sheet)
{
    if (waterOverlayReady_) {
        C3D_TexDelete(&waterOverlay_);
        waterOverlayReady_ = false;
    }
    if (sheet.size() != texture::kWaterOverlayBytes) {
        return false;
    }
    waterOverlayReady_ = uploadSheet(&waterOverlay_, sheet.data(), texture::kWaterOverlayEdge,
                                     texture::kWaterOverlayEdge);
    if (waterOverlayReady_) {
        // The one sheet that repeats: `jh.c(F)` scrolls its UVs by the yaw.
        C3D_TexSetWrap(&waterOverlay_, GPU_REPEAT, GPU_REPEAT);
    }
    return waterOverlayReady_;
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

    // **The cube pass's outline, in the cube atlas's layout.** Two kinds of
    // line, because a merged quad is now more than one block and the old
    // pattern could not show that:
    //
    //   * grey, one texel round every copy of a tile: the block grid, which is
    //     what the wireframe has always drawn;
    //   * white, along the first column and first row of each slot's copies --
    //     the two edges every quad starts from, u = 0 and v = 0 -- so each quad
    //     marks two of its own sides. A merged run is the white rectangle with
    //     grey crossings inside it, and a single face is white on two sides.
    //
    // The gutters are left transparent: they are only ever sampled by accident.
    // A texture cannot mark a quad's far edges: it does not know where the run
    // ends. The neighbour's white edge usually does. 1 MB of ordinary linear
    // memory, written in place rather than staged -- the CPU can store into
    // linear memory, and a debug view has no business asking for a second
    // megabyte to do it.
    if (!C3D_TexInit(&cubeWire_, u16(kCubeEdge), u16(kCubeEdge), GPU_RGBA8)) {
        C3D_TexDelete(&wire_);
        return false;
    }
    u32* cubeTexels = static_cast<u32*>(cubeWire_.data);
    constexpr int kCopies = mesh::kCubeRepeat * mesh::kCubeTilePixels;
    for (u32 y = 0; y < kCubeEdge; ++y) {
        for (u32 x = 0; x < kCubeEdge; ++x) {
            const int lx = int(x % u32(mesh::kCubeSlotPixels)) - mesh::kCubeGutterPixels;
            const int ly = int(y % u32(mesh::kCubeSlotPixels)) - mesh::kCubeGutterPixels;
            u32 texel = rgba(0, 0, 0, 0);
            if (lx >= 0 && ly >= 0 && lx < kCopies && ly < kCopies) {
                const int px = lx % mesh::kCubeTilePixels;
                const int py = ly % mesh::kCubeTilePixels;
                const bool start = lx == 0 || ly == 0;
                const bool grid = px == 0 || py == 0 || px == mesh::kCubeTilePixels - 1
                                  || py == mesh::kCubeTilePixels - 1;
                texel = start ? rgba(255, 255, 255, 255)
                              : (grid ? rgba(96, 96, 96, 255) : rgba(0, 0, 0, 0));
            }
            cubeTexels[tiledOffsetFlipped(x, y, kCubeEdge, kCubeEdge)] = texel;
        }
    }
    C3D_TexFlush(&cubeWire_);
    C3D_TexSetFilter(&cubeWire_, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&cubeWire_, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

    wireReady_ = true;
    cubeWireReady_ = true;
    return true;
}

bool Atlas::updateTile(int tile, const u8* texels, int across)
{
    if (!updateTileOf(&tex_, ready_, inVram_, tile, texels, across)) {
        return false;
    }

    // **And its slot in the cube atlas**, if a cube face can show it. None of
    // a1.1.2's animated tiles can -- fire, water and lava are not cubes -- so
    // this is dormant today, and it is here so that a version with an animated
    // cube texture animates the whole slot rather than one copy, which would
    // show as a run whose first block moves and whose other two do not.
    //
    // The whole 64 x 64 slot, as sixteen 16 x 16 blocks: the copies start after
    // an 8-texel gutter, so they do not sit on the 16-texel grid writeTile
    // moves, and the gutters repeat the tile's edge texels and have to follow
    // it too. Each block is assembled from the tile through the same mapping
    // the full upload uses.
    if (!cubeReady_) {
        return true;
    }
    for (int n = 0; n < across; ++n) {
        const int one = tile + n;
        const int slot = one >= 0 && one < mesh::kAtlasTileCount
                             ? int(mesh::kCubeAtlas.slotOfTile[one])
                             : int(mesh::kNoCubeSlot);
        if (slot == int(mesh::kNoCubeSlot)) {
            continue;
        }
        constexpr int kBlocksPerSlot = mesh::kCubeSlotPixels / mesh::kCubeTilePixels;
        const u32 column0 = u32(slot % mesh::kCubeSlotsPerEdge) * u32(kBlocksPerSlot);
        const u32 row0 = u32(slot / mesh::kCubeSlotsPerEdge) * u32(kBlocksPerSlot);
        u8 block[kAtlasTilePixels * kAtlasTilePixels * 4];
        for (int by = 0; by < kBlocksPerSlot; ++by) {
            for (int bx = 0; bx < kBlocksPerSlot; ++bx) {
                for (int y = 0; y < kAtlasTilePixels; ++y) {
                    const int ty = mesh::cubeSlotTexel(by * kAtlasTilePixels + y);
                    for (int x = 0; x < kAtlasTilePixels; ++x) {
                        const int tx = mesh::cubeSlotTexel(bx * kAtlasTilePixels + x);
                        std::memcpy(block + (y * kAtlasTilePixels + x) * 4,
                                    texels + (ty * kAtlasTilePixels + tx) * 4, 4);
                    }
                }
                if (!writeTile(&cube_, cubeInVram_, column0 + u32(bx), row0 + u32(by), kCubeEdge,
                               block)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool Atlas::updateItemsTile(int tile, const u8* texels)
{
    // **Never in VRAM**, so this always takes the memcpy branch -- see the
    // declaration of `items_`. The flag is passed anyway rather than hard-coded
    // false, so that moving the sheet into VRAM one day is a one-line change
    // and not a silent corruption.
    return updateTileOf(&items_, itemsReady_, false, tile, texels, 1);
}

bool Atlas::updateTileOf(C3D_Tex* target, bool live, bool vram, int tile, const u8* texels,
                         int across)
{
    constexpr int kTilesTotal = kAtlasTilesPerEdge * kAtlasTilesPerEdge;
    if (!live || texels == nullptr || tile < 0 || tile >= kTilesTotal) {
        return false;
    }
    // A run has to stay inside one row of the atlas: the tiles of a run are
    // adjacent in memory only because they are adjacent columns, and a run that
    // wrapped past the last column would write the next row's first tile at an
    // offset that is not its own.
    const int column = tile % kAtlasTilesPerEdge;
    if (across < 1 || across > kMaxTileRun || column + across > kAtlasTilesPerEdge) {
        return false;
    }
    return writeTile(target, vram, u32(column), u32(tile / kAtlasTilesPerEdge), u32(kAtlasEdge),
                     texels, u32(across));
}

bool Atlas::writeTile(C3D_Tex* target, bool vram, u32 column, u32 row, u32 edge,
                      const u8* texels, u32 across)
{
    if (across < 1 || across > u32(kMaxTileRun)) {
        return false;
    }
    if (tileStaging_ == nullptr) {
        tileStaging_ = static_cast<u32*>(
            linearAlloc(texture::kTileWords * kTileStagingTiles * sizeof(u32)));
        if (tileStaging_ == nullptr) {
            return false;
        }
    }
    // **Fresh words for every push**, never the last push's: its copy may still
    // be reading them. See `tileStagingUsed_`.
    if (tileStagingUsed_ + int(across) > kTileStagingTiles) {
        return false;
    }
    u32* staging = tileStaging_ + u32(tileStagingUsed_) * texture::kTileWords;
    tileStagingUsed_ += int(across);

    const texture::TileRuns runs = texture::tileRunsFlipped(column, row, edge);

    // The layout of what is staged: the first half of every tile in the run,
    // then the second half of every tile in the run. That is the order the
    // two destination runs hold them in.
    constexpr u32 kHalf = texture::kTileRunWords;
    u32* const firstHalves = staging;
    u32* const secondHalves = staging + kHalf * across;

    // Same byte-order reversal and same flip as the whole-atlas upload, over
    // 256 texels instead of 65,536. `tileRunIndex` is what folds the flip and
    // the Morton order into an index into the two runs laid end to end -- and
    // it lands the first tile of the run straight in place.
    for (u32 y = 0; y < u32(kAtlasTilePixels); ++y) {
        for (u32 x = 0; x < u32(kAtlasTilePixels); ++x) {
            const u8* src = texels + (usize(y) * kAtlasTilePixels + usize(x)) * 4;
            const u32 at = texture::tileRunIndex(runs, column, row, x, y, edge);
            const u32 word = rgba(src[0], src[1], src[2], src[3]);
            if (at < kHalf) {
                firstHalves[at] = word;
            } else {
                secondHalves[at - kHalf] = word;
            }
        }
    }

    // **The layout inside a run does not depend on which column the tile is
    // in**: subtracting the run's own base cancels the column term out of
    // `tiledOffset` entirely, leaving the Morton index and the half of the tile
    // the texel is in. So the rest of the run is the first tile's halves
    // repeated -- and the destination is still two copies, because tile
    // n + 1's runs start exactly where tile n's end.
    for (u32 n = 1; n < across; ++n) {
        std::memcpy(firstHalves + n * kHalf, firstHalves, kHalf * sizeof(u32));
        std::memcpy(secondHalves + n * kHalf, secondHalves, kHalf * sizeof(u32));
    }

    const u32 runBytes = kHalf * across * sizeof(u32);
    GSPGPU_FlushDataCache(staging, runBytes * 2);

    u32* dst = static_cast<u32*>(target->data);
    if (vram) {
        // **The CPU cannot store into VRAM**, so the move is the same texture
        // copy `C3D_TexUpload` routes a VRAM destination through -- twice, on
        // 512 bytes per tile in the run each. Flag 8 is the raw copy: no
        // tiling, no format conversion, nothing with an orientation left to get
        // wrong, which is the lesson the whole-atlas upload above was rewritten
        // for.
        C3D_SyncTextureCopy(firstHalves, 0, dst + runs.first, 0, runBytes, 8);
        C3D_SyncTextureCopy(secondHalves, 0, dst + runs.second, 0, runBytes, 8);
    } else {
        std::memcpy(dst + runs.first, firstHalves, runBytes);
        std::memcpy(dst + runs.second, secondHalves, runBytes);
        C3D_TexFlush(target);
    }
    return true;
}

void Atlas::shutdown()
{
    if (tileStaging_ != nullptr) {
        linearFree(tileStaging_);
        tileStaging_ = nullptr;
    }
    if (ready_) {
        C3D_TexDelete(&tex_);
        ready_ = false;
    }
    if (itemsReady_) {
        C3D_TexDelete(&items_);
        itemsReady_ = false;
    }
    if (iconsReady_) {
        C3D_TexDelete(&icons_);
        iconsReady_ = false;
    }
    if (entityReady_) {
        C3D_TexDelete(&entity_);
        entityReady_ = false;
    }
    if (artReady_) {
        C3D_TexDelete(&art_);
        artReady_ = false;
    }
    if (fontReady_) {
        C3D_TexDelete(&font_);
        fontReady_ = false;
    }
    if (particlesReady_) {
        C3D_TexDelete(&particles_);
        particlesReady_ = false;
    }
    if (waterOverlayReady_) {
        C3D_TexDelete(&waterOverlay_);
        waterOverlayReady_ = false;
    }
    if (wireReady_) {
        C3D_TexDelete(&wire_);
        wireReady_ = false;
    }
    if (cubeReady_) {
        C3D_TexDelete(&cube_);
        cubeReady_ = false;
    }
    if (cubeWireReady_) {
        C3D_TexDelete(&cubeWire_);
        cubeWireReady_ = false;
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
