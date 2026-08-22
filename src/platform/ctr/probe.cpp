// M0: prove the toolchain, the version pipeline, and the rendering primitives
// the whole mesher will sit on, then measure the hardware facts the design
// rests on instead of assuming them.
//
// This was the whole application at M0. It is kept, and reachable by holding
// SELECT at boot, because every number it produces is one the design still
// rests on -- the heap split, the fill rate, the SD cluster size, the cost of
// the second texture fetch. When a later change makes the game slower than it
// should be, the first useful question is whether the hardware still measures
// the way it did, and that has to stay answerable without reverting anything.
//
// Validated here:
//   - 12-byte vertex, three byte/short attributes, one interleaved buffer
//   - a single global immutable index buffer: 4 vertices per quad, not 6
//   - picasso shader build integration
//   - TEV modulate (texture x vertex colour x lightmap), the lighting path
//   - stereo 3D with the second eye skipped at slider zero
//
// M0b answers the M2 gate question: can day/night be a lightmap texture rather
// than light baked into vertex colour? Baked light means sunset invalidates
// every mesh in the world, and re-meshing at 268 MHz is not viable -- but the
// lightmap costs a second texture fetch per fragment on a device that is
// fill-rate bound. That trade has to be measured, not guessed, so X toggles the
// lightmap and Y drives a controlled-overdraw stress pass with the GPU's own
// timer reporting the cost per fragment.
//
// Top screen: a textured cube built exactly the way chunk meshes will be.
// Bottom screen: the probe report.

#include "platform/ctr/probe.hpp"

#include <3ds.h>
#include <citro3d.h>

#include <cinttypes>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "version_config.hpp"
#include "version_slots.hpp"

#include <world_shader_shbin.h>

extern "C" {
// libctru commits *all* remaining application memory at startup and splits it
// between the newlib heap and the linear (GPU-visible) heap, capping the latter
// at 32 MB. Both are weak symbols we can override; see docs/3ds-performance.md.
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;
}

namespace {

// Alpha's own sky colour, as RGBA8. The fog colour is 0xFFD990.
constexpr u32 kSkyColour = 0x90D9FFFF;

constexpr u32 kDisplayTransferFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

// ---------------------------------------------------------------------------
// Vertex format
// ---------------------------------------------------------------------------

// Field order must match the attribute loader order: citro3d has no per-attribute
// offset, it accumulates sizes in declaration order.
// 12 bytes, not 11. The GPU fetches the s16 pair from an address that must stay
// 2-byte aligned, and an 11-byte stride would put every odd vertex on an odd
// address. The spare byte pays for itself as the face index the geometry-shader
// path needs.
struct WorldVertex {
    s16 u, v;        // atlas coords, 1/16384 units
    u8 x, y, z;      // position within a 16^3 section, 0..16
    u8 face;         // face index 0..5; reserved for the geoshader path
    u8 r, g, b;      // static: face shade x AO -- never time-dependent
    u8 light;        // (skyLevel << 4) | blockLevel, unpacked in the shader
};
static_assert(sizeof(WorldVertex) == 12, "vertex must stay packed at 12 bytes");
// The stride is what moves the s16 pair between vertices, so an odd size would
// put every second vertex's UV on an odd address regardless of the field offset.
static_assert(sizeof(WorldVertex) % 2 == 0 && offsetof(WorldVertex, u) % 2 == 0,
              "the s16 pair must stay 2-byte aligned at every stride multiple");

constexpr u8 packLight(int sky, int block)
{
    return u8((sky << 4) | (block & 15));
}

constexpr int kUvScale = 16384;  // 1.0 in UV == 16384; s16 leaves room to tile

// A 16^3 section is at most 16*16*16/2 * 6 quads in the checkerboard worst case.
// The shared index buffer must cover the largest single draw we will ever issue.
constexpr int kMaxQuads = 12288;
constexpr int kMaxIndices = kMaxQuads * 6;
static_assert(kMaxQuads * 4 <= 65536, "quad vertices must remain u16-indexable");

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

constexpr int kAtlasSize = 64;  // 4x4 grid of 16px tiles
constexpr int kTilePx = 16;
constexpr int kTilesPerRow = kAtlasSize / kTilePx;

// GPU_RGBA8 wants A,B,G,R in memory order.
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 255)
{
    return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | a;
}

// PICA textures are stored in 8x8 tiles, Morton-ordered within each tile, and
// bottom-up. Getting this wrong is the classic "why is my texture scrambled".
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

// Four flat-ish tiles with a darker border, so a UV mistake is obvious rather
// than subtle. Real terrain.png loading arrives with the asset pipeline.
u32 tilePixel(int tile, int px, int py)
{
    static const u32 base[4] = {
        rgba(126, 186, 90),   // 0 grass-ish
        rgba(150, 108, 74),   // 1 dirt-ish
        rgba(128, 128, 128),  // 2 stone-ish
        rgba(160, 130, 78),   // 3 planks-ish
    };
    const bool border = px == 0 || py == 0 || px == kTilePx - 1 || py == kTilePx - 1;
    const bool speck = ((px * 7 + py * 13 + tile * 31) & 7) == 0;

    u32 c = base[tile & 3];
    if (border || speck) {
        const u8 r = u8((c >> 24) * 3 / 4);
        const u8 g = u8(((c >> 16) & 0xFF) * 3 / 4);
        const u8 b = u8(((c >> 8) & 0xFF) * 3 / 4);
        c = rgba(r, g, b);
    }
    return c;
}

void buildAtlas(C3D_Tex* tex)
{
    C3D_TexInit(tex, kAtlasSize, kAtlasSize, GPU_RGBA8);

    u32* dst = static_cast<u32*>(tex->data);
    for (int y = 0; y < kAtlasSize; ++y) {
        for (int x = 0; x < kAtlasSize; ++x) {
            const int tile = (y / kTilePx) * kTilesPerRow + (x / kTilePx);
            const u32 c = tilePixel(tile, x % kTilePx, y % kTilePx);
            // Flip vertically: texture origin is bottom-left.
            dst[tiledOffset(u32(x), u32(kAtlasSize - 1 - y), kAtlasSize)] = c;
        }
    }

    C3D_TexSetFilter(tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(tex, GPU_REPEAT, GPU_REPEAT);
    C3D_TexFlush(tex);
}

// ---------------------------------------------------------------------------
// Lightmap
// ---------------------------------------------------------------------------
//
// 16x16 texels: u is block light 0..15, v is sky light 0..15, exactly the two
// nibbles the vertex carries. 1 KB in RGBA8, so it stays resident in the
// texture cache no matter what the rest of the frame is doing.
//
// The whole point: advancing time of day rewrites these 256 texels. Nothing
// about the world's geometry changes, so not one mesh is rebuilt.

constexpr int kLightmapSize = 16;

// A stand-in for Alpha's real curve, which comes out of the decompiled jar
// during M2. The shape that matters here is that sky light scales with daylight
// while block light does not, and that block light is warmer.
void buildLightmap(C3D_Tex* tex, float daylight)
{
    u32* dst = static_cast<u32*>(tex->data);
    for (int sky = 0; sky < kLightmapSize; ++sky) {
        for (int block = 0; block < kLightmapSize; ++block) {
            const float s = (sky / 15.0f) * daylight;
            const float b = block / 15.0f;

            // Ambient floor keeps caves readable instead of pure black.
            const float rf = 0.05f + 0.95f * (s > b ? s : b);
            const float gf = 0.05f + 0.95f * (s > b * 0.82f ? s : b * 0.82f);
            const float bf = 0.05f + 0.95f * (s > b * 0.62f ? s : b * 0.62f);

            const u32 c = rgba(u8(rf * 255.0f), u8(gf * 255.0f), u8(bf * 255.0f));
            // Flipped for the same reason buildAtlas above is, which this did
            // not do until the renderer inherited the omission and shipped a
            // world where caves lit like open sky. No M0 number moves -- fill
            // rate does not care what the texels say -- but a probe that draws
            // its own light upside down is not the thing to copy from.
            dst[tiledOffset(u32(block), u32(kLightmapSize - 1 - sky), kLightmapSize)] = c;
        }
    }
    C3D_TexFlush(tex);
}

// ---------------------------------------------------------------------------
// Geometry: one cube, built the way a chunk mesh will be -- quads only, four
// vertices each, consumed through the shared index buffer.
// ---------------------------------------------------------------------------

constexpr u8 S = 16;  // a section is 16 blocks; the cube spans one section

struct Face {
    u8 corner[4][3];  // counter-clockwise seen from outside
    u8 tile;
    u8 shade;  // Minecraft's fixed per-face brightness -- static, so it stays
               // in the vertex colour rather than in the lightmap
    u8 sky;    // sky light 0..15 reaching this face
    u8 block;  // block light 0..15, as if from a nearby torch
};

const Face kCubeFaces[6] = {
    // +X / -X get 0.6, +-Z get 0.8, top 1.0, bottom 0.5.
    // The sky levels differ per face so a wrong lightmap V is visible at once,
    // and the -Z face carries block light so the warm/cool split shows up.
    {{{S, 0, S}, {S, 0, 0}, {S, S, 0}, {S, S, S}}, 1, 153, 11, 0},
    {{{0, 0, 0}, {0, 0, S}, {0, S, S}, {0, S, 0}}, 1, 153, 11, 0},
    {{{0, S, S}, {S, S, S}, {S, S, 0}, {0, S, 0}}, 0, 255, 15, 0},
    {{{0, 0, 0}, {S, 0, 0}, {S, 0, S}, {0, 0, S}}, 2, 128, 4, 0},
    {{{0, 0, S}, {S, 0, S}, {S, S, S}, {0, S, S}}, 1, 204, 9, 0},
    {{{S, 0, 0}, {0, 0, 0}, {0, S, 0}, {S, S, 0}}, 1, 204, 2, 14},
};

// Tile k occupies [k*16, k*16+16) px of the atlas; in UV that is a 1/4 square.
void tileUv(int tile, int corner, s16& u, s16& v)
{
    const int tx = tile % kTilesPerRow;
    const int ty = tile / kTilesPerRow;
    const int cu = (corner == 1 || corner == 2) ? 1 : 0;
    const int cv = (corner >= 2) ? 1 : 0;
    u = s16((tx + cu) * kUvScale / kTilesPerRow);
    v = s16((ty + cv) * kUvScale / kTilesPerRow);
}

int buildCube(WorldVertex* out)
{
    int n = 0;
    u8 faceIndex = 0;
    for (const Face& f : kCubeFaces) {
        for (int c = 0; c < 4; ++c) {
            WorldVertex& vtx = out[n++];
            vtx.x = f.corner[c][0];
            vtx.y = f.corner[c][1];
            vtx.z = f.corner[c][2];
            vtx.face = faceIndex;
            vtx.r = vtx.g = vtx.b = f.shade;
            // Vary sky light across one face's corners: smooth lighting will
            // do exactly this, and it proves the lightmap UV interpolates
            // rather than snapping per face.
            const int skyBias = (faceIndex == 4 && (c == 1 || c == 2)) ? -5 : 0;
            const int sky = f.sky + skyBias < 0 ? 0 : f.sky + skyBias;
            vtx.light = packLight(sky, f.block);
            tileUv(f.tile, c, vtx.u, vtx.v);
        }
        faceIndex++;
    }
    return n;
}

// A screen-filling quad in the same vertex format, drawn repeatedly with the
// depth test off so every layer shades every pixel. That turns "what does the
// second texture unit cost per fragment" into a slope we can measure instead of
// a number we have to trust.
void buildStressQuad(WorldVertex* out)
{
    const u8 corners[4][2] = {{0, 0}, {S, 0}, {S, S}, {0, S}};
    for (int c = 0; c < 4; ++c) {
        WorldVertex& vtx = out[c];
        vtx.x = corners[c][0];
        vtx.y = corners[c][1];
        vtx.z = 0;
        vtx.face = 0;
        vtx.r = vtx.g = vtx.b = 255;
        // Spread the corners across the lightmap so the fetch addresses vary,
        // matching how a real chunk samples it.
        vtx.light = packLight(c * 5, (3 - c) * 5);
        tileUv(0, c, vtx.u, vtx.v);
    }
}

// One immutable index buffer, shared by every draw for the lifetime of the
// process. Chunk meshes therefore carry no index memory at all.
u16* buildSharedIndices()
{
    u16* idx = static_cast<u16*>(linearAlloc(kMaxIndices * sizeof(u16)));
    if (!idx) {
        return nullptr;
    }
    for (int q = 0; q < kMaxQuads; ++q) {
        const u16 base = u16(q * 4);
        u16* t = &idx[q * 6];
        t[0] = base;
        t[1] = u16(base + 1);
        t[2] = u16(base + 2);
        t[3] = base;
        t[4] = u16(base + 2);
        t[5] = u16(base + 3);
    }
    return idx;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

// osGetMemRegionFree(MEMREGION_APPLICATION) always reads 0 here and that is not
// a fault: libctru has already committed the whole region to this process. The
// numbers that bound the design are the two heap sizes, so report those.
void printMemory(bool isNew3DS)
{
    std::printf("Model      %s\n", isNew3DS ? "New 3DS" : "Old 3DS");
    std::printf("App region %lu MB (committed at startup)\n",
                static_cast<unsigned long>(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20));
    std::printf("Heap       %lu MB   Linear %lu MB\n",
                static_cast<unsigned long>(__ctru_heap_size >> 20),
                static_cast<unsigned long>(__ctru_linear_heap_size >> 20));
}

// VRAM must be sampled *after* citro3d has taken its render targets: the pools
// are lazily created by the first vramAlloc, so reading earlier reports 0.
void printGpuMemory()
{
    std::printf("Linear     %lu KB free (post-init)\n",
                static_cast<unsigned long>(linearSpaceFree() >> 10));
    std::printf("VRAM       %lu KB free of %d KB\n",
                static_cast<unsigned long>(vramSpaceFree() >> 10), OS_VRAM_SIZE >> 10);
}

// The storage numbers in docs/save-data.md depend entirely on the SD card's
// cluster size, so read it rather than trusting the 32 KB assumption.
void printStorage()
{
    FS_ArchiveResource sd{};
    if (R_FAILED(FSUSER_GetSdmcArchiveResource(&sd))) {
        std::printf("SD         unavailable\n");
        return;
    }

    const u64 freeBytes = static_cast<u64>(sd.freeClusters) * sd.clusterSize;
    const u64 worldCost = static_cast<u64>(4096) * sd.clusterSize;  // 1024^2 blocks

    std::printf("SD cluster %lu KB\n", static_cast<unsigned long>(sd.clusterSize >> 10));
    std::printf("SD free    %" PRIu64 " MB\n", freeBytes >> 20);
    std::printf("1024^2 world costs %" PRIu64 " MB unpacked\n", worldCost >> 20);
}

void printVersion()
{
    std::printf("\x1b[32m%s\x1b[0m  protocol %d\n", mcver::kDisplay, mcver::kProtocol);
    std::printf("Height %d, sections of %d, windows %s\n\n", mcver::kWorldHeight,
                mcver::kSectionSize, mcver::kHasWindows ? "yes" : "no");
}

// The measurement this build exists for. Both figures come from the GPU's own
// timer, so vsync does not hide the cost the way wall-clock frame time would.
// Everything here formats as integers on purpose. devkitARM's newlib links a
// printf without float support unless -u _printf_float is forced, so a "%f"
// here would silently print nothing and cost a round trip to hardware to
// discover. Microseconds and picoseconds carry all the precision needed anyway.
//
// The picosecond arithmetic must be 64-bit. `unsigned long` is 32 bits on ARM,
// and drawUs * 1000000 overflows it above ~4295 us -- which a 64-layer pass
// clears easily, silently reporting a small number instead of a large one.
u64 picosPerFragment(unsigned long drawUs, int layers)
{
    const u64 fragments = u64(layers) * 400ULL * 240ULL;
    if (fragments == 0) {
        return 0;
    }
    return (u64(drawUs) * 1000000ULL) / fragments;
}

// Two averages, held side by side: [0] is the lightmap off, [1] is it on. The
// probe alternates between them itself rather than asking for two readings
// taken minutes apart, because the difference being measured is small enough
// that drift between runs would swamp it.
struct AbResult {
    float sumMs = 0.0f;
    int rounds = 0;

    bool valid() const { return rounds > 0; }
    float meanMs() const { return rounds > 0 ? sumMs / float(rounds) : 0.0f; }
    void add(float ms)
    {
        sumMs += ms;
        rounds++;
    }
    void reset()
    {
        sumMs = 0.0f;
        rounds = 0;
    }
};

// The thresholds from docs/3ds-performance.md section 1b, fixed before any data
// was taken. Rendering the verdict on the console rather than the raw delta is
// deliberate: the decision rule should not get re-argued once a number is in
// front of us.
const char* lightmapVerdict(u64 deltaPicos)
{
    if (deltaPicos < 1500) {
        return "FREE - take the lightmap";
    }
    if (deltaPicos < 4000) {
        return "CHEAP - take it, tune fill";
    }
    return "COSTLY - use section uniform";
}

void printMeasurement(bool lightmap, bool stress, int layers, float drawMs, float procMs,
                      const AbResult ab[2], bool autoAb)
{
    const unsigned long drawUs = static_cast<unsigned long>(drawMs * 1000.0f);
    const unsigned long procUs = static_cast<unsigned long>(procMs * 1000.0f);

    std::printf("\x1b[16;1H\x1b[2K  lightmap %-3s  overdraw %s", lightmap ? "ON" : "off",
                stress ? "ON " : "off");
    if (stress) {
        std::printf(" x%d", layers);
    }
    std::printf("\n\x1b[2K  GPU draw %lu us   proc %lu us", drawUs, procUs);
    if (stress) {
        std::printf("\n\x1b[2K  %llu ps/fragment",
                    static_cast<unsigned long long>(picosPerFragment(drawUs, layers)));
    } else {
        std::printf("\n\x1b[2K");
    }

    if (!stress || !autoAb) {
        std::printf("\n\x1b[2K\n\x1b[2K\n\x1b[2K");
        return;
    }

    if (!ab[0].valid() || !ab[1].valid()) {
        std::printf("\n\x1b[2K  A/B sampling...\n\x1b[2K\n\x1b[2K");
        return;
    }

    const long onUs = static_cast<long>(ab[1].meanMs() * 1000.0f);
    const long offUs = static_cast<long>(ab[0].meanMs() * 1000.0f);
    const long deltaUs = onUs - offUs;
    const long absDelta = deltaUs < 0 ? -deltaUs : deltaUs;
    const u64 deltaPicos = picosPerFragment(static_cast<unsigned long>(absDelta), layers);

    std::printf("\n\x1b[2K  A/B x%d (%d rounds)  ON %ld  OFF %ld", layers, ab[1].rounds, onUs,
                offUs);
    std::printf("\n\x1b[2K  delta %s%ld us = %s%llu ps/frag", deltaUs < 0 ? "-" : "+", absDelta,
                deltaUs < 0 ? "-" : "+", static_cast<unsigned long long>(deltaPicos));
    std::printf("\n\x1b[2K  \x1b[33m%s\x1b[0m", lightmapVerdict(deltaPicos));
}

void printControls()
{
    std::printf("\x1b[23;1HA cull  B tex  X lightmap  Y overdraw\n");
    std::printf("L/R layers  UP/DOWN time  START exit");
}

}  // namespace

namespace mc::ctr {

// The graphics subsystem and the bottom-screen console belong to main(), which
// has already brought both up to read the button that got us here.
int runProbe(bool isNew3DS)
{
    fsInit();  // refcounted; safe even though the default app init already did it

    printVersion();
    printMemory(isNew3DS);
    printStorage();

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    C3D_RenderTarget* eye[2] = {
        C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16),
        C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH16),
    };
    C3D_RenderTargetSetOutput(eye[0], GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
    C3D_RenderTargetSetOutput(eye[1], GFX_TOP, GFX_RIGHT, kDisplayTransferFlags);

    // Shader
    DVLB_s* dvlb = DVLB_ParseFile((u32*)world_shader_shbin, world_shader_shbin_size);
    shaderProgram_s program;
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
    const int uLocMvp = shaderInstanceGetUniformLocation(program.vertexShader, "mvp");
    C3D_BindProgram(&program);

    // Attribute layout -- order defines the byte offsets inside WorldVertex.
    C3D_AttrInfo attrs;
    AttrInfo_Init(&attrs);
    AttrInfo_AddLoader(&attrs, 0, GPU_SHORT, 2);          // uv        offset 0
    AttrInfo_AddLoader(&attrs, 1, GPU_UNSIGNED_BYTE, 4);  // xyz+face  offset 4
    AttrInfo_AddLoader(&attrs, 2, GPU_UNSIGNED_BYTE, 4);  // rgb+light offset 8
    C3D_SetAttrInfo(&attrs);

    // Geometry
    WorldVertex* verts = static_cast<WorldVertex*>(linearAlloc(24 * sizeof(WorldVertex)));
    WorldVertex* stressVerts = static_cast<WorldVertex*>(linearAlloc(4 * sizeof(WorldVertex)));
    u16* indices = buildSharedIndices();
    if (!verts || !stressVerts || !indices) {
        std::printf("\x1b[31mout of linear memory\x1b[0m\n");
    }
    const int vertexCount = buildCube(verts);
    const int quadCount = vertexCount / 4;
    buildStressQuad(stressVerts);

    C3D_Tex atlas;
    buildAtlas(&atlas);
    C3D_TexBind(0, &atlas);

    C3D_Tex lightmap;
    C3D_TexInit(&lightmap, kLightmapSize, kLightmapSize, GPU_RGBA8);
    // Clamp, not repeat: light level 15 must not wrap round to 0.
    C3D_TexSetFilter(&lightmap, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&lightmap, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    buildLightmap(&lightmap, 1.0f);
    C3D_TexBind(1, &lightmap);

    std::printf("\nVertex %u B, %d quads, %d verts\n", unsigned(sizeof(WorldVertex)), quadCount,
                vertexCount);
    std::printf("Shared index buf %d KB (once, global)\n", (kMaxIndices * 2) >> 10);
    printGpuMemory();

    bool stereoOn = false;
    bool cull = true;
    bool textured = true;
    bool useLightmap = true;
    bool stress = false;
    int layers = 8;
    float angle = 0.0f;

    // Time of day drives the lightmap, and nothing else. If this animates
    // smoothly while the mesh is never rebuilt, the M2 question is answered.
    float timeOfDay = 0.25f;
    bool autoTime = true;
    float lastDaylight = -1.0f;

    // The GPU timers are per-frame and noisy; average before believing them.
    constexpr int kSampleFrames = 30;
    int sampleCount = 0;
    float drawAccum = 0.0f;
    float procAccum = 0.0f;
    float drawMs = 0.0f;
    float procMs = 0.0f;

    // During the overdraw pass the probe flips the lightmap itself once per
    // sample block, so both sides are measured under identical conditions
    // seconds apart rather than by hand minutes apart.
    AbResult ab[2];
    bool autoAb = true;

    printControls();

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        if (down & KEY_A) {
            cull = !cull;
        }
        if (down & KEY_B) {
            textured = !textured;
        }
        if (down & KEY_X) {
            useLightmap = !useLightmap;
            autoAb = false;  // manual control wins; stop flipping underneath them
        }
        if (down & KEY_Y) {
            stress = !stress;
            ab[0].reset();
            ab[1].reset();
            autoAb = true;
        }
        if (down & KEY_R) {
            layers = layers < 64 ? layers * 2 : 64;
            ab[0].reset();
            ab[1].reset();
        }
        if (down & KEY_L) {
            layers = layers > 1 ? layers / 2 : 1;
            ab[0].reset();
            ab[1].reset();
        }
        const u32 held = hidKeysHeld();
        if (held & (KEY_UP | KEY_DOWN)) {
            autoTime = false;
            timeOfDay += (held & KEY_UP) ? 0.004f : -0.004f;
        }
        if (autoTime) {
            timeOfDay += 0.0015f;
        }
        if (timeOfDay > 1.0f) {
            timeOfDay -= 1.0f;
        }
        if (timeOfDay < 0.0f) {
            timeOfDay += 1.0f;
        }

        // Day/night: rewrite 256 texels. No mesh is touched, which is the
        // entire argument for this approach over baking light into vertices.
        const float phase = timeOfDay * 6.28318530718f;
        float daylight = 0.5f + 0.5f * sinf(phase);
        daylight = daylight * daylight;  // dusk falls faster than noon lingers
        if (daylight < 0.02f) {
            daylight = 0.02f;
        }
        // Only 16 distinguishable steps are visible, so skip redundant uploads.
        if (int(daylight * 255.0f) != int(lastDaylight * 255.0f)) {
            buildLightmap(&lightmap, daylight);
            lastDaylight = daylight;
        }

        C3D_CullFace(cull && !stress ? GPU_CULL_BACK_CCW : GPU_CULL_NONE);

        // Stage 0: atlas x vertex colour (face shade, AO -- all static).
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        if (textured) {
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }

        // Stage 1: x lightmap. Turning it off leaves a pass-through stage, so
        // the only difference being measured is the texture fetch itself.
        C3D_TexEnv* env1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(env1);
        if (useLightmap) {
            C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
            C3D_TexEnvFunc(env1, C3D_Both, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(env1, C3D_Both, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(env1, C3D_Both, GPU_REPLACE);
        }

        // Skip the second eye entirely at slider zero, rather than rendering it
        // and throwing it away. See docs/3ds-performance.md.
        const float slider = osGet3DSliderState();
        const bool wantStereo = slider > 0.0f;
        if (wantStereo != stereoOn) {
            gfxSet3D(wantStereo);
            stereoOn = wantStereo;
        }

        angle += 0.01f;

        // The overdraw pass must not be depth-culled, or later layers would be
        // rejected before shading and there would be nothing to measure.
        C3D_DepthTest(!stress, GPU_GREATER, GPU_WRITE_ALL);

        C3D_BufInfo bufInfo;
        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, stress ? stressVerts : verts, sizeof(WorldVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        for (int i = 0; i < (stereoOn ? 2 : 1); ++i) {
            C3D_RenderTargetClear(eye[i], C3D_CLEAR_ALL, kSkyColour, 0);
            C3D_FrameDrawOn(eye[i]);

            const float iod = stereoOn ? (i == 0 ? -slider * 0.5f : slider * 0.5f) : 0.0f;

            C3D_Mtx mvp;
            if (stress) {
                // Orthographic over the quad's own 0..16 space, so one draw is
                // exactly one full screen of fragments.
                Mtx_OrthoTilt(&mvp, 0.0f, float(S), 0.0f, float(S), 0.0f, 1.0f, true);
                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocMvp, &mvp);
                for (int layer = 0; layer < layers; ++layer) {
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, indices);
                }
            } else {
                C3D_Mtx proj, model;
                Mtx_PerspStereoTilt(&proj, C3D_AngleFromDegrees(55.0f), 400.0f / 240.0f, 1.0f,
                                    200.0f, iod, 40.0f, false);
                Mtx_Identity(&model);
                Mtx_Translate(&model, 0.0f, 0.0f, -40.0f, true);
                Mtx_RotateY(&model, angle, true);
                Mtx_RotateX(&model, 0.4f, true);
                Mtx_Translate(&model, -8.0f, -8.0f, -8.0f, true);  // centre the 0..16 cube
                Mtx_Multiply(&mvp, &proj, &model);

                C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLocMvp, &mvp);
                C3D_DrawElements(GPU_TRIANGLES, quadCount * 6, C3D_UNSIGNED_SHORT, indices);
            }
        }
        C3D_FrameEnd(0);

        drawAccum += C3D_GetDrawingTime();
        procAccum += C3D_GetProcessingTime();
        if (++sampleCount >= kSampleFrames) {
            drawMs = drawAccum / float(sampleCount);
            procMs = procAccum / float(sampleCount);
            drawAccum = procAccum = 0.0f;
            sampleCount = 0;

            ab[useLightmap ? 1 : 0].add(drawMs);

            printMeasurement(useLightmap, stress, layers, drawMs, procMs, ab, autoAb);

            if (stress && autoAb) {
                useLightmap = !useLightmap;
            }
        }
    }

    C3D_TexDelete(&lightmap);
    C3D_TexDelete(&atlas);
    linearFree(indices);
    linearFree(stressVerts);
    linearFree(verts);
    shaderProgramFree(&program);
    DVLB_Free(dvlb);
    C3D_RenderTargetDelete(eye[1]);
    C3D_RenderTargetDelete(eye[0]);
    C3D_Fini();
    fsExit();
    return 0;
}

}  // namespace mc::ctr
