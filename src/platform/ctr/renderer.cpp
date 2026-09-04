#include "platform/ctr/renderer.hpp"

#include "core/mesh/vertex.hpp"
#include "core/texture/dev_art.hpp"

#include <3ds.h>

#include <cmath>
#include <cstdio>

#include <detail_shader_shbin.h>
#include <outline_shader_shbin.h>
#include <quad_shader_shbin.h>
#include <world_shader_shbin.h>

namespace mc::ctr {

// **A breadcrumb straight onto the bottom screen, for a console that may not
// live to draw another frame.**
//
// `consoleInit` turns double buffering off, so the bottom screen is a plain
// framebuffer the CPU writes and the LCD scans out -- no swap, no GPU, no
// completed frame required. A line printed here is on the screen before the
// next instruction runs, which makes it the one report channel that survives
// the thing being investigated. Row 30 is below the overlay's footer at 28-29.
//
// Only ever called on the quad path, and only at the few points worth naming:
// a printf a frame would cost more than it tells.
void geoTrace(const char* what)
{
    std::printf("\x1b[30;1H\x1b[2K\x1b[33mgeo: %s\x1b[0m", what);
    gfxFlushBuffers();
}


namespace {

// Alpha's sky, as the RGBA8 the render target is cleared to.
constexpr u32 kSkyColour = 0x90D9FFFF;

// The same colour as a texenv constant, which is 0xAABBGGRR. Terrain at the fog
// end therefore lands on exactly the colour behind it and the horizon has no
// seam.
constexpr u32 kFogColour = 0xFFFFD990;

constexpr u32 kDisplayTransferFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
    | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)
    | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

// Enough near plane to stand inside a block without the wall in front of your
// face being clipped away, and no more: depth precision is the cost, and a
// 16-bit depth buffer has little to spare.
constexpr float kNearPlane = 0.2f;

// The alpha test, and it is the original's own rather than a threshold of ours.
//
// `iq.class` does exactly two things about it, once at startup, and never
// touches either again:
//
//     glEnable(GL_ALPHA_TEST);          // 3008
//     glAlphaFunc(GL_GREATER, 0.1f);    // 516
//
// So it is on for every pass in the game, not just for the cutout shapes. 0.1
// of 255 is 25.5 and GL_GREATER passes what is strictly above the reference,
// so GL admits alpha >= 26; GPU_GREATER against 25 admits exactly the same
// set. Written as 25 rather than as a rounded 26 for that reason -- 26 would
// discard a texel the original keeps.
constexpr int kAlphaTestRef = 25;

// Stereo. The tunable is **on-screen disparity in pixels**, not an interocular
// distance in blocks, because pixels are the thing the eye actually judges and
// blocks are not: the same 0.25-block separation is dramatic at a focal
// distance of 1 and invisible at 100.
//
// The exact relation, from citro3d's own matrix (Mtx_PerspStereoTilt, read out
// of libcitro3d.a -- the tilt puts the parallax terms in row 1, which is the
// 400-pixel axis, so NDC maps to pixels at 200 per unit):
//
//     disparity_px(d) = 200 * (I/2) * [ 1/(F*t*a) - 1/d ]
//     t = tan(fovy/2),  a = 400/240,  F = focal distance,  I = eye separation
//
// This started at I = 0.25 blocks with F = 8, which sounds reasonable and is
// not. Putting real numbers through it: at full slider the whole scene spans
// 3 pixels of disparity, and **across 32 to 160 blocks -- which is most of
// what a player is looking at -- it varies by 0.62 of one pixel.** The far
// world was one flat card 2.5 px behind the screen, while everything nearer
// than 8 blocks still popped out properly. That is exactly the reported
// symptom, both halves of it.
//
// So the number below is a disparity, and the separation is derived from it.
// 10 px was the conservative starting point -- the 3DS convention is roughly
// 1/30 of screen width, about 13 px, and titles run to 20.
//
// **7 px is what was actually chosen on a New 3DS XL**, at the sixth launch,
// by holding Y and walking the d-pad until it read as comfortable. Below the
// convention rather than above it, which is worth knowing: the near field in
// first person is inherently harsh, because the 1/d term always wins close to
// the camera, and a lower infinity disparity is how that gets tolerable. Hold
// Y and the d-pad to try another; the overlay reads both numbers back.
constexpr float kInfinityDisparityPixels = 7.0f;

// The depth that appears to sit at the screen plane. Nearer than this pops out.
constexpr float kFocalBlocks = 8.0f;

// The 400-pixel axis spans NDC -1..1.
constexpr float kPixelsPerNdc = 200.0f;

// Eye separation in blocks that puts infinity at `pixels` of disparity, for
// the projection actually in use. Inverting the relation above at d = infinity:
//     pixels = 200 * (I/2) / (F*t*a)   ->   I = pixels * F * t * a / 100
float interocularForDisparity(float pixels, float focalBlocks, float fovRadians, float aspect)
{
    const float t = std::tan(fovRadians * 0.5f) * aspect;
    return 2.0f * (pixels / kPixelsPerNdc) * focalBlocks * t;
}

// C3D_Mtx stores each row reversed -- an FVec is (w, z, y, x) -- so element
// (row, col) lives at r[row].c[3 - col]. Reaching in expecting the obvious
// order is the classic mistake, which is why core keeps its own Mat4 and the
// conversion is here at the boundary.
Mat4 toCoreMat4(const C3D_Mtx& m)
{
    Mat4 out;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            out.m[row][col] = m.r[row].c[3 - col];
        }
    }
    return out;
}

// **The GPU drain, and the only one the public API can express.**
//
// `C3D_FrameSync` is not it, whatever the name suggests. Disassembled out of
// `renderqueue.o` it is `gspWaitForAnyEvent` spun until one of citro3d's two
// VBlank counters moves:
//
//     ldr r6, [r4]          @ frameCounter[0]
//     ldr r5, [r4, #4]      @ frameCounter[1]
//     bl  gspWaitForAnyEvent
//     ...                   @ loop while neither has changed
//
// It is the frame-rate limiter and it returns whether or not the GPU has
// finished anything at all. What actually waits on the GPU is the
// `gxCmdQueueWait` inside `C3D_FrameBegin` -- with no timeout under
// C3D_FRAME_SYNCDRAW, with a zero one under C3D_FRAME_NONBLOCK.
//
// So the drain is that poll, put on a deadline; the frame it opens on success
// is closed again immediately. Closing costs nothing: `C3Di_SplitFrame` finds
// an empty command list and adds none, and `C3D_FrameEnd` transfers only
// targets whose `used` flag a `C3D_FrameDrawOn` set, which nothing here does.
//
// **False means the deadline expired**, and the caller is about to hand back
// memory the GPU is still fetching from. There is nothing better available --
// the alternative is handing it back anyway, with no idea -- but the caller
// gets to say so on the way past.
// The poll every bounded wait in this file is built on: open a frame without
// blocking, and keep asking until the deadline. **On success a frame is open**
// and the caller owns it. There is no other way to wait on the GPU from outside
// citro3d -- C3D_FRAME_SYNCDRAW's wait takes no timeout, which is the whole
// reason this exists.
bool pollFrameBegin(float seconds)
{
    const u64 start = svcGetSystemTick();
    const u64 deadline = u64(double(seconds) * double(SYSCLOCK_ARM11));
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (svcGetSystemTick() - start > deadline) {
            return false;
        }
        // Not a spin: the GPU is the thing that has to make progress, and
        // burning the core it shares does not help it.
        svcSleepThread(1000000);  // 1 ms
    }
    return true;
}

bool drainGpu(float seconds)
{
    // No C3D_FrameSync here, unlike beginFrameBounded: a drain is not a pacing
    // point, and the callers are teardowns rather than frames.
    if (!pollFrameBegin(seconds)) {
        return false;
    }
    C3D_FrameEnd(0);
    return true;
}

// Long enough that no honest frame is still running -- the slowest this project
// has recorded is under 50 ms -- and short enough that a player who has just
// wedged the GPU is told rather than left holding a dead console.
constexpr float kGpuDrainSeconds = 0.5f;

}  // namespace

bool beginFrameBounded(float seconds)
{
    // The pacing half of C3D_FRAME_SYNCDRAW, kept: C3D_FrameSync waits on the
    // two VBlank counters rather than on the GPU, so it always returns and it
    // is what holds the caller to the refresh rate. Only the queue drain below
    // it can wedge, and only that is under the deadline.
    C3D_FrameSync();
    return pollFrameBegin(seconds);
}

void Camera::look(float* dx, float* dy, float* dz) const
{
    const float cp = std::cos(pitch);
    *dx = -std::sin(yaw) * cp;
    *dy = -std::sin(pitch);
    *dz = std::cos(yaw) * cp;
}

i32 Camera::chunkX() const
{
    return i32(std::floor(x / 16.0));
}

i32 Camera::chunkZ() const
{
    return i32(std::floor(z / 16.0));
}

int Camera::sectionY() const
{
    return int(std::floor(y / 16.0));
}

// Attribute layout is the vertex struct's memory layout: citro3d has no
// per-attribute offset, it accumulates component sizes in declaration order.
// Reordering either the loaders here or the fields in core/mesh/vertex.hpp
// without the other silently reinterprets the buffer.
bool Renderer::buildPipeline(Pipeline* pipeline, const void* shbin, u32 shbinSize, bool detail)
{
    pipeline->dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<void*>(shbin)), shbinSize);
    if (pipeline->dvlb == nullptr) {
        return false;
    }
    shaderProgramInit(&pipeline->program);
    shaderProgramSetVsh(&pipeline->program, &pipeline->dvlb->DVLE[0]);
    pipeline->uLocMvp = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "mvp");
    pipeline->uLocFog = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "fogparam");

    AttrInfo_Init(&pipeline->attrs);
    if (detail) {
        // mesh::DetailVertex, 16 bytes.
        AttrInfo_AddLoader(&pipeline->attrs, 0, GPU_SHORT, 4);          // x,y,z,face  offset 0
        AttrInfo_AddLoader(&pipeline->attrs, 1, GPU_SHORT, 2);          // u, v        offset 8
        AttrInfo_AddLoader(&pipeline->attrs, 2, GPU_UNSIGNED_BYTE, 4);  // r,g,b,light offset 12
    } else {
        // mesh::WorldVertex, 12 bytes.
        AttrInfo_AddLoader(&pipeline->attrs, 0, GPU_SHORT, 2);          // u, v        offset 0
        AttrInfo_AddLoader(&pipeline->attrs, 1, GPU_UNSIGNED_BYTE, 4);  // x,y,z,face  offset 4
        AttrInfo_AddLoader(&pipeline->attrs, 2, GPU_UNSIGNED_BYTE, 4);  // r,g,b,light offset 8
    }
    return true;
}

bool Renderer::buildOutlinePipeline(const void* shbin, u32 shbinSize)
{
    if (!buildPipeline(&outlinePipeline_, shbin, shbinSize, false)) {
        return false;
    }
    // buildPipeline installed the cube format's three loaders. The outline has
    // one attribute: three floats. Re-initialising is cheaper than teaching
    // buildPipeline a third layout it would only ever use here.
    AttrInfo_Init(&outlinePipeline_.attrs);
    AttrInfo_AddLoader(&outlinePipeline_.attrs, 0, GPU_FLOAT, 3);

    // Five kilobytes, once. Rebuilt when the crosshair moves off its block, and
    // never during a frame -- see setSelection.
    outlineVerts_ = linearAlloc(sizeof(render::OutlineVertex)
                                * usize(render::kOutlineVertexCount));
    return outlineVerts_ != nullptr;
}

void Renderer::setSelection(const AABB& worldBox)
{
    // Rebuilding 432 vertices is cheap, but doing it on a frame where nothing
    // moved is 432 vertices of pure waste at 60 Hz, and the crosshair sits on
    // one block for most of the frames it is on anything.
    if (hasSelection_ && worldBox.minX == selectionBox_.minX
        && worldBox.minY == selectionBox_.minY && worldBox.minZ == selectionBox_.minZ
        && worldBox.maxX == selectionBox_.maxX && worldBox.maxY == selectionBox_.maxY
        && worldBox.maxZ == selectionBox_.maxZ) {
        return;
    }
    selectionBox_ = worldBox;
    hasSelection_ = true;
    outlineDirty_ = true;
}

// **Last in the eye, and it writes no depth.** Drawn after the world so it sits
// on top of the face it outlines rather than fighting it, and with colour-only
// writes so a translucent block behind it is not re-ordered by an edge.
void Renderer::drawSelection(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ)
{
    if (!hasSelection_ || outlineVerts_ == nullptr) {
        return;
    }

    auto* verts = static_cast<render::OutlineVertex*>(outlineVerts_);
    // The origin the rest of the frame is built around. Y is **not** offset --
    // viewProjection only moves x and z -- and offsetting it here would put the
    // outline sixty-four blocks under the block it belongs to.
    const double originX = double(originChunkX) * 16.0;
    const double originZ = double(originChunkZ) * 16.0;
    if (outlineDirty_) {
        const int written = render::buildOutline(selectionBox_, originX, 0.0, originZ, verts,
                                                 render::kOutlineVertexCount);
        if (written != render::kOutlineVertexCount) {
            return;
        }
        outlineDirty_ = false;
        GSPGPU_FlushDataCache(verts,
                              sizeof(render::OutlineVertex) * u32(render::kOutlineVertexCount));
    }

    bindPipeline(outlinePipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, outlinePipeline_.uLocMvp, &viewProjection);

    // One combiner stage: hand the vertex colour straight to the framebuffer.
    // The world's three-stage texture combiner has nothing to say about a shape
    // with no texture on it.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }

    // The alpha the original uses is 0.4, so it has to blend; and the alpha
    // test the world pass leaves on would throw most of it away.
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);

    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, outlineVerts_, sizeof(render::OutlineVertex), 1, 0x0);
    C3D_DrawArrays(GPU_TRIANGLES, 0, render::kOutlineVertexCount);

    // Nothing is restored here on purpose: applyWorldState runs before every
    // eye and states all of the above rather than inheriting it, precisely so a
    // pass like this one can leave the state where it likes.
}

// The geometry-shader program. Three things differ from the two above.
//
// **A second DVLE.** picasso puts every source file it is given into one shbin,
// in the order they were passed, so DVLE[0] is quad.v.pica and DVLE[1] is
// quad.g.pica. Both run on the same shader units off one shared instruction
// blob; what `shaderProgramSetGsh` adds is the geometry stage and the routing
// of vertex-shader outputs into it.
//
// **The input stride, and it is in registers, not bytes.** 6 is the number of
// `.out` declarations in quad.v.pica: the hardware gathers that many output
// registers and runs the geometry shader once. One input vertex per invocation
// is what makes this a quad expander rather than a triangle one.
//
// **Two byte attributes and nothing else.** mesh::QuadVertex, 8 bytes.
bool Renderer::buildQuadPipeline(const void* shbin, u32 shbinSize)
{
    Pipeline* pipeline = &quadPipeline_;
    pipeline->dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<void*>(shbin)), shbinSize);
    if (pipeline->dvlb == nullptr || pipeline->dvlb->numDVLE < 2) {
        return false;
    }
    shaderProgramInit(&pipeline->program);
    shaderProgramSetVsh(&pipeline->program, &pipeline->dvlb->DVLE[0]);
    shaderProgramSetGsh(&pipeline->program, &pipeline->dvlb->DVLE[1], 6);

    // Every uniform this path has is in the vertex half -- the geometry shader
    // declares none on purpose, because work there is not divided across the
    // shader units and work here is.
    pipeline->uLocMvp = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "mvp");
    pipeline->uLocFog = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "fogparam");
    uLocFaceBasis_ =
        shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "faceBasis");
    if (pipeline->uLocMvp < 0 || pipeline->uLocFog < 0 || uLocFaceBasis_ < 0) {
        return false;
    }

    AttrInfo_Init(&pipeline->attrs);
    AttrInfo_AddLoader(&pipeline->attrs, 0, GPU_UNSIGNED_BYTE, 4);  // x,y,z,face       offset 0
    AttrInfo_AddLoader(&pipeline->attrs, 1, GPU_UNSIGNED_BYTE, 4);  // tileX,tileY,l,ao offset 4
    return true;
}

void Renderer::bindPipeline(const Pipeline& pipeline)
{
    C3D_BindProgram(const_cast<shaderProgram_s*>(&pipeline.program));
    C3D_SetAttrInfo(const_cast<C3D_AttrInfo*>(&pipeline.attrs));

    // The corner basis, straight out of the table core meshes against -- so the
    // geometry shader cannot disagree with the 12-byte path about winding or
    // texture orientation, and there is no second copy of these numbers to keep
    // in step. Eighteen writes twice a frame; see the note below on why this is
    // not hoisted to init.
    if (&pipeline == &quadPipeline_) {
        for (int face = 0; face < mesh::kFaceCount; ++face) {
            const mesh::FaceBasis& b = mesh::kFaceBasis[face];
            const int slot = uLocFaceBasis_ + face * 3;
            C3D_FVUnifSet(GPU_VERTEX_SHADER, slot + 0, float(b.base[0]), float(b.base[1]),
                          float(b.base[2]), mesh::kFaceShadeFloat[face]);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, slot + 1, float(b.e1[0]), float(b.e1[1]),
                          float(b.e1[2]), float(b.uvSign));
            C3D_FVUnifSet(GPU_VERTEX_SHADER, slot + 2, float(b.e2[0]), float(b.e2[1]),
                          float(b.e2[2]), 0.0f);
        }
    }

    // The fog line goes in here rather than once a frame, because C3D_FVUnifSet
    // addresses a *hardware* constant register and the two shaders are
    // assembled separately: picasso is free to put `fogparam` in a register the
    // other program uses for one of its .constf values, and binding that
    // program reloads its constant table over the top. Six writes a frame
    // sidesteps the whole question.
    if (pipeline.uLocFog >= 0) {
        const float start = fogStartBlocks();
        const float slope = 1.0f / (fogEndBlocks() - start);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, pipeline.uLocFog, slope, -start * slope, 0.0f, 0.0f);
    }
}

void Renderer::setStereo(float disparityPixels, float focalBlocks)
{
    // Clamped rather than trusted: this is driven by a d-pad, and a focal
    // distance at or below zero divides by it.
    disparityPixels_ = disparityPixels < 0.0f ? 0.0f : (disparityPixels > 40.0f ? 40.0f
                                                                                : disparityPixels);
    focalBlocks_ = focalBlocks < 1.0f ? 1.0f : (focalBlocks > 128.0f ? 128.0f : focalBlocks);
}

bool Renderer::init(const Config& config, bool isNew3DS)
{
    config_ = config;
    isNew3DS_ = isNew3DS;
    setStereo(kInfinityDisparityPixels, kFocalBlocks);

    // 24-bit depth, not 16.
    //
    // 16 bits cannot describe this world. Window depth is 1/d, so with a
    // 0.2-block near plane and a 176-block far plane one depth unit spans
    //
    //     dd = d^2 * (far - near) / (near * far * 65536)
    //
    // which is 0.19 blocks at 50 away, 0.76 at 100, and 1.95 at 160. Past
    // roughly 110 blocks two surfaces a whole block apart land on the *same*
    // depth value, so which one wins is decided by rounding -- and it changes
    // as the camera moves, and differs between the two eyes, which is what
    // makes it read as shimmer rather than as a static artefact.
    //
    // 24 bits multiplies every one of those figures by 256: a block of
    // separation at 160 blocks is 178 depth units. The cost is one more byte
    // per pixel in each eye's depth buffer, 375 KB total, against the ~5 MB of
    // VRAM M0 measured free.
    eye_[0] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    eye_[1] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (eye_[0] == nullptr || eye_[1] == nullptr) {
        return false;
    }
    C3D_RenderTargetSetOutput(eye_[0], GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
    C3D_RenderTargetSetOutput(eye_[1], GFX_TOP, GFX_RIGHT, kDisplayTransferFlags);

    if (!buildPipeline(&cubePipeline_, world_shader_shbin, world_shader_shbin_size, false)
        || !buildOutlinePipeline(outline_shader_shbin, outline_shader_shbin_size)
        || !buildPipeline(&detailPipeline_, detail_shader_shbin, detail_shader_shbin_size,
                          true)
        || !buildQuadPipeline(quad_shader_shbin, quad_shader_shbin_size)) {
        return false;
    }

    // The index buffer covers the largest single draw there can ever be: a
    // checkerboard section, 12288 quads. 144 KB, once, for the whole process.
    const int maxIndices = mesh::kMaxQuadsPerSection * 6;
    indices_ = static_cast<u16*>(linearAlloc(usize(maxIndices) * sizeof(u16)));
    if (indices_ == nullptr) {
        return false;
    }
    for (int q = 0; q < mesh::kMaxQuadsPerSection; ++q) {
        const u16 base = u16(q * 4);
        u16* t = &indices_[q * 6];
        t[0] = base;
        t[1] = u16(base + 1);
        t[2] = u16(base + 2);
        t[3] = base;
        t[4] = u16(base + 2);
        t[5] = u16(base + 3);
    }
    GSPGPU_FlushDataCache(indices_, u32(usize(maxIndices) * sizeof(u16)));

    // Dev Art when nothing was handed in, so a caller with no menu in front of
    // it still gets a textured world. Building it here rather than holding a
    // static keeps the 256 KB off .bss for the ordinary case, where the menu
    // has already built one.
    texture::AtlasImage fallback;
    if (config_.atlas == nullptr) {
        texture::buildDevArt(&fallback.rgba);
    }
    const texture::AtlasImage& atlasImage =
        config_.atlas != nullptr ? *config_.atlas : fallback;

    if (!atlas_.init(atlasImage) || !lightmap_.init()) {
        return false;
    }

    // The pool is sized against what is left once the render targets and the
    // atlas have taken theirs, with a megabyte held back for the GUI and entity
    // textures that arrive later.
    render::ChunkRendererConfig chunkConfig;
    chunkConfig.meshDistance = config_.meshDistance;
    chunkConfig.budget = vboBudget(isNew3DS, 1024 * 1024);
    // Four sections a frame at the measured 30.4 us each is well under a
    // millisecond on the dev host; the console is slower, and this is the knob
    // to turn once there is a frame time to turn it against.
    chunkConfig.meshBudgetPerFrame = 4;
    chunks_.reset(&allocator_, chunkConfig);

    return true;
}

void Renderer::rebuildChunks()
{
    // **Wait for the GPU before giving any of it back.** C3D_FrameEnd only
    // enqueues; the frame recorded a moment ago is still fetching vertices out
    // of exactly the blocks shutdown() is about to linearFree. VboPool's
    // per-block retirement (see VboPool::kRetireFrames) handles the steady
    // state, but a wholesale teardown outruns it by definition -- it frees
    // everything, including what is in flight. This is a settings change that
    // happens once when the player moves a slider, so a full drain costs
    // nothing worth counting.
    //
    // **This used to be `C3D_FrameSync`, and that waited for the wrong thing.**
    // It is a VBlank wait, not a GPU one -- see drainGpu, which disassembles it
    // -- so it returned on the next refresh whether or not the GPU had finished
    // the list it was handed, and the pool below was freed and immediately
    // reallocated underneath an active fetch. On a frame that outruns the
    // refresh, which is the whole of the far-from-origin case, that is every
    // time.
    if (drainGpu(kGpuDrainSeconds)) {
        // **Only on success.** The queue is empty, so whatever geoshader draws
        // it held have retired and the next frame may take the unbounded wait.
        geoWorkInFlight_ = false;
    } else {
        // The GPU did not come back, so what follows frees memory it is still
        // reading. Nothing here can prevent that -- the alternative is to leak
        // the pool and carry on with a console that is already dead -- but the
        // line says which of the two failures this was.
        //
        // **And `geoWorkInFlight_` stays set**, which is the point. Clearing it
        // unconditionally -- which this did -- told the next `drawFrame` that
        // the queue was empty on the strength of a drain that had just said it
        // was not, and that frame then took the unbounded
        // `C3D_FRAME_SYNCDRAW` on a queue still holding the list that wedged.
        // That is the third hardware failure exactly, re-armed inside its own
        // fix. See docs/3ds-performance.md section 2.
        geoTrace("GPU did not drain before the pool was freed");
    }

    render::ChunkRendererConfig chunkConfig = chunks_.config();
    chunkConfig.meshDistance = config_.meshDistance;

    // Hand the pool's memory back *before* asking how much is free, or the new
    // budget is measured against a linear heap the old pool is still holding
    // and a step up in distance would be sized as though there were no room.
    chunks_.shutdown();
    chunkConfig.budget = vboBudget(isNew3DS_, 1024 * 1024);
    chunks_.reset(&allocator_, chunkConfig);
}

void Renderer::setMeshDistance(int distance)
{
    if (distance < 1 || distance == config_.meshDistance) {
        return;
    }
    config_.meshDistance = distance;
    rebuildChunks();
}

void Renderer::setCubeFormat(mesh::CubeFormat format)
{
    if (format == cubeFormat_) {
        return;
    }
    cubeFormat_ = format;

    // **The way back out of the watchdog's latch, and the only one.**
    // `quadDrawsStopped_` stops the cube pass recording geoshader draws, and it
    // has to survive a run of good frames -- a GPU that missed a deadline is
    // not one to hand a geoshader draw back to because the next frame happened
    // to come through. What it must not survive is a deliberate change of
    // format: the fallback in main.cpp arrives here to select 4-vertex, and a
    // player who selects geoshader again is asking for it on purpose. Either
    // way the decision has been made somewhere that can be reasoned about,
    // which is more than the latch was doing.
    //
    // `gpuStalls_` is deliberately not cleared: the count is the session's
    // history and the debug page reports it as such.
    quadDrawsStopped_ = false;

    if (format == mesh::CubeFormat::Quads) {
        // Printed *before* the work, not after: the point of a breadcrumb is to
        // be the last thing on the screen if the next thing never returns.
        // rebuildChunks starts with a GPU drain, which is reached long before
        // any quad has been drawn.
        geoTrace("pool teardown (GPU drain)");
    }

    // Everything resident is in the old encoding, and its bytes mean something
    // else in the new one. The pool goes, and the streamer re-meshes.
    rebuildChunks();
}

bool Renderer::setWireframe(bool on)
{
    if (on && !atlas_.ensureWireframe()) {
        wireframe_ = false;
        return false;
    }
    wireframe_ = on;
    return true;
}

void Renderer::reclaimScreen()
{
    // Both eyes, though only the left one can have been displaced -- the menu
    // draws in 2D and never creates a right-eye target. Re-linking the right
    // one costs a store and a queue drain outside a frame, and it means this
    // says "the eyes own the top screen" rather than "the left eye probably
    // lost it".
    C3D_RenderTargetSetOutput(eye_[0], GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
    C3D_RenderTargetSetOutput(eye_[1], GFX_TOP, GFX_RIGHT, kDisplayTransferFlags);

    stereo_ = false;
}

bool Renderer::setAtlas(const texture::AtlasImage& image)
{
    // The in-flight frame is sampling the texture this is about to free, for
    // the same reason rebuildChunks has to wait: FrameEnd enqueued the list and
    // did not wait for it. Once, on a texture-pack change. **The GPU, not the
    // refresh** -- see the note in rebuildChunks on what `C3D_FrameSync` waits
    // for, which is not this.
    // Cleared only if the drain actually happened; see rebuildChunks for what
    // clearing it on a failed drain re-arms.
    if (drainGpu(kGpuDrainSeconds)) {
        geoWorkInFlight_ = false;
    }

    // The old texture is handed back *first*. Atlas::init asks for VRAM before
    // it will settle for linear, and holding 256 KB of the old one while the
    // new one asks would quietly demote the new one to linear on a console
    // that is close to full -- a silent bandwidth loss with no symptom to
    // trace it by.
    atlas_.shutdown();

    if (atlas_.init(image)) {
        // shutdown() took the outline atlas with it. The setting survives the
        // swap or it does not, but it must not survive as a flag with nothing
        // behind it -- bind() would fall back to the real atlas and the
        // wireframe would be on and invisible.
        if (wireframe_ && !atlas_.ensureWireframe()) {
            wireframe_ = false;
        }
        return true;
    }

    // The upload failed with the old texture already given back, so there is
    // nothing bound at all. Dev Art is generated rather than read off a card,
    // which makes it the one atlas that cannot fail for want of a file; if even
    // this will not fit there is no memory left and the next frame has larger
    // problems than its colours.
    texture::AtlasImage devArt;
    texture::buildDevArt(&devArt.rgba);
    atlas_.init(devArt);
    wireframe_ = false;
    return false;
}

void parkShaderProgram()
{
    // Function-local statics rather than members: this outlives every Renderer
    // and every Menu on purpose, and `-fno-threadsafe-statics` costs nothing
    // here because only the main thread ever touches citro3d.
    static DVLB_s* dvlb = nullptr;
    static shaderProgram_s program;

    if (dvlb == nullptr) {
        // The world shader, parsed a second time. A DVLB is about a kilobyte
        // and this is once for the process; sharing `cubePipeline_`'s would
        // defeat the whole point, since that is one of the things being freed.
        const void* shbin = world_shader_shbin;
        dvlb = DVLB_ParseFile(reinterpret_cast<u32*>(const_cast<void*>(shbin)),
                              u32(world_shader_shbin_size));
        if (dvlb == nullptr) {
            // Nothing useful is left to do -- the caller is about to free the
            // program citro3d is still pointing at -- so say so rather than
            // crash silently two screens later.
            std::printf("\x1b[31mshader park failed\x1b[0m\n");
            return;
        }
        shaderProgramInit(&program);
        shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
    }

    C3D_BindProgram(&program);
}

void Renderer::shutdown()
{
    // **Everything below is freed out from under a list that may still be
    // running.** Leaving a world hands the top screen to the menu, so a GPU
    // still chewing on the last world frame here surfaces several function
    // calls away from anything that mentions the world. Drain first.
    //
    // The drain is the first of two guards and not the load-bearing one: it has
    // a deadline, and the case where it expires is exactly the case where the
    // GPU is already gone. The menu's own frame is bounded for that reason --
    // see ctr::beginFrameBounded -- because a drain that fails here used to
    // hand the menu an unbounded wait on a wedged queue, which is a console
    // that has to be powered off.
    // Cleared only if the drain actually happened; see rebuildChunks. The menu
    // this hands the screen to now bounds its own wait (ctr::beginFrameBounded),
    // so a drain that expires here costs it a frame rather than the console.
    if (drainGpu(kGpuDrainSeconds)) {
        geoWorkInFlight_ = false;
    }

    // Before the pipelines go, so citro3d is not left pointing at one of them
    // when the menu binds citro2d's program on the way back. See
    // parkShaderProgram.
    parkShaderProgram();

    chunks_.shutdown();
    lightmap_.shutdown();
    atlas_.shutdown();
    if (indices_ != nullptr) {
        linearFree(indices_);
        indices_ = nullptr;
    }
    for (Pipeline* pipeline : {&cubePipeline_, &detailPipeline_, &quadPipeline_}) {
        if (pipeline->dvlb != nullptr) {
            shaderProgramFree(&pipeline->program);
            DVLB_Free(pipeline->dvlb);
            pipeline->dvlb = nullptr;
        }
    }
    for (C3D_RenderTarget*& target : eye_) {
        if (target != nullptr) {
            C3D_RenderTargetDelete(target);
            target = nullptr;
        }
    }
}

C3D_Mtx Renderer::viewProjection(const Camera& camera, float iod, float fovRadians) const
{
    C3D_Mtx projection;
    Mtx_PerspStereoTilt(&projection, fovRadians, 400.0f / 240.0f, kNearPlane, farPlane(),
                        iod, focalBlocks_, false);

    float dx, dy, dz;
    camera.look(&dx, &dy, &dz);

    // **Relative to the camera's own chunk, not to the world origin.** The
    // subtraction is done in double and only its result becomes a float, so the
    // eye lands somewhere in [0, 16) however far out the player has walked.
    // Everything downstream -- the section matrices in drawEye, the culling
    // frustum -- is offset by the same amount, so this is a change of origin
    // and not an approximation.
    const double originX = double(camera.chunkX()) * 16.0;
    const double originZ = double(camera.chunkZ()) * 16.0;
    const float ex = float(camera.x - originX);
    const float ey = float(camera.y);
    const float ez = float(camera.z - originZ);

    C3D_FVec eye = FVec3_New(ex, ey, ez);
    C3D_FVec target = FVec3_New(ex + dx, ey + dy, ez + dz);
    C3D_FVec up = FVec3_New(0.0f, 1.0f, 0.0f);

    C3D_Mtx view;
    Mtx_LookAt(&view, eye, target, up, false);

    C3D_Mtx vp;
    Mtx_Multiply(&vp, &projection, &view);
    return vp;
}

Frustum Renderer::cullFrustum(const Camera& camera) const
{
    // One frustum for both eyes, widened by exactly what an eye's own frustum
    // reaches past the centre one -- far cheaper than culling twice, and a
    // section the outer eye can see but the centre cannot is a section that
    // appears in one eye and not the other, at the left or right edge of the
    // screen, which is the most uncomfortable thing a stereo renderer can do.
    //
    // How much is not a guess. Reading Mtx_PerspStereoTilt out of libcitro3d.a,
    // the row that becomes the 400-pixel axis is
    //
    //     clip.y = -x/(t*A) + z*iod/(2*S*t*A) + iod/2,    w = -z
    //
    // with t = tan(fov/2), A = 400/240, S the focal distance. Solving clip.y/w
    // = 1 for x at distance d gives an edge at
    //
    //     x = -t*A*d - d*iod/(2*S) + iod*t*A/2
    //
    // against the centre eye's -t*A*d. The term that grows with distance is
    // d*|iod|/(2*S), so widening the half-extent by |iod|/(2*S) covers it at
    // every distance; the constant term shrinks the frustum and is dropped.
    //
    // The old number here was a flat 1.08. At the default 10 px of disparity
    // the honest answer is 1.025, so 1.08 was generous -- but it is a constant
    // and |iod| is not: the strength is on a d-pad, and by 25 px of disparity
    // 1.08 is no longer enough. That failure looks exactly like the symptom
    // above and gets worse the harder you turn the 3D up, which is the wrong
    // way round for a knob a player is meant to explore.
    const float fovRadians = C3D_AngleFromDegrees(config_.fovDegrees);
    const float aspect = 400.0f / 240.0f;
    const float maxIod =
        0.5f * interocularForDisparity(disparityPixels_, focalBlocks_, fovRadians, aspect);

    const float halfExtent = std::tan(fovRadians * 0.5f) + maxIod / (2.0f * focalBlocks_ * aspect);
    // A little over, because the frustum is tested against section boxes and
    // being one section too generous costs a rejected box.
    const float fov = 2.0f * std::atan(halfExtent * 1.02f);
    const C3D_Mtx vp = viewProjection(camera, 0.0f, fov);

    Frustum frustum;
    // Not a guess: citro3d's Mtx_Persp family writes M[2][2] = near/(near-far),
    // M[2][3] = far*near/(near-far), M[3][2] = -1, which puts the near plane at
    // z/w = -1 and the far plane at z/w = 0. See core/util/frustum.hpp.
    frustum.setFromViewProjection(toCoreMat4(vp), ClipRange::NegativeOneToZero);
    // viewProjection built these planes around the camera's chunk, so the
    // frustum has to test section boxes in the same space. Without this the
    // culler and the draw loop disagree about where the world is, which past
    // a few hundred thousand blocks means visible sections being rejected.
    frustum.setOrigin(camera.chunkX(), camera.chunkZ());
    return frustum;
}

// One vertex format, every section that has any of it. Splitting the frame this
namespace {

// ---------------------------------------------------------------------------
// The geometry-shader bisect knobs
// ---------------------------------------------------------------------------
// The quad path's first hardware run hung the GPU hard -- both screens, no
// HOME, power off -- after one or two chunks had drawn. Everything static about
// it checks out (the shbin's two DVLEs, the outmaps, the gsh stride and mode,
// the setemit encodings, the uniform allocation), so the fault is in what the
// draw loop *accumulates*, and the only way to find that is to take pieces of
// the loop away until it stops.
//
// **Compile-time, and deliberately not on the debug page.** Every test costs a
// power cycle anyway, so a rebuild is not the expensive part, and a knob that
// cannot be reached by a stray d-pad press cannot make a hardware measurement
// mean something other than what it says.
//
// **Every one of these is inert unless the mesh being drawn is a quad mesh**,
// so the 12-byte path measures the same with them compiled in as without.
// Defaults are "no limit": this block ships neutral.
//
//   kGeoMaxSections    draw at most N quad sections in the cube pass, 0 = all
//   kGeoMaxQuads       clamp each quad draw to N input vertices, 0 = all
//   kGeoCubePassOnly   skip the detail and translucent passes entirely
//   kGeoForceMono      one eye, halving the command list and the draw count
//
// Start where the probe is -- mono, cube pass only, kGeoMaxSections = 1 -- and
// relax one at a time. Which knob stops the hang names the suspect; see the
// table in docs/3ds-performance.md section 2.
constexpr int kGeoMaxSections = 0;
constexpr int kGeoMaxQuads = 0;
constexpr bool kGeoCubePassOnly = false;
constexpr bool kGeoForceMono = false;

// **The fix, as a switch, so its absence stays measurable.** End the command
// list on both sides of the geoshader cube pass, so the shader-unit
// repartition that turning the geometry stage on or off performs never lands
// on a list with draws still in flight. See the note on kGeoRamp.
constexpr bool kGeoSplitPasses = true;

// ---------------------------------------------------------------------------
// The ramp: the same bisect, done automatically in one boot
// ---------------------------------------------------------------------------
// **A wedged GPU stays wedged for the rest of the session**, so a bisect cannot
// walk downwards from a hang -- there is only ever one hang to learn from, and
// it is the last thing that happens. The bisect therefore has to walk *upwards*
// and be read backwards: start below anything that could plausibly break,
// loosen one limit at a time, and the step being held when the GPU stops is the
// one that did it.
//
// The two axes are the only two things left between the probe -- one draw, six
// quads, no hang -- and the game, which wedged on four draws and 1,701 quads
// with no command-list split. So quads are ramped first with the draw count
// pinned at one, and only then the draw count with the quads unpinned. Whether
// the ramp dies in the first block or the second is the whole question:
//
//   dies in the first block   a single geoshader draw has a size it cannot pass
//   dies in the second block  a geoshader draw cannot follow another one
//
// Each step is announced on the bottom screen as it is entered, so the answer
// survives even if nothing else does.
struct GeoRampStep {
    bool splitPasses;  // finish the command list around the geoshader pass
    bool cubeOnly;     // skip the detail and translucent passes entirely
    int sections;      // 0 = no limit
    int quads;         // 0 = no limit
    const char* name;
};

// **What the ramp found.** Blocks 1 and 2 survived -- the whole render distance,
// every geoshader draw unlimited, one after another, no trouble at all -- and it
// wedged the instant the detail passes were allowed back in, with the cube pass
// pinned to a single sixteen-quad draw. So the fault is not the geometry shader,
// not the size of a draw, and not how many of them there are. **It is a
// non-geoshader draw following a geoshader one.**
//
// That has a mechanism. Turning the geometry stage off repartitions the shader
// units -- `GPUREG_VSH_COM_MODE`, and `GPUREG_GEOSTAGE_CONFIG` with it -- and
// `shaderProgramConfigure` writes those registers straight into the command
// list. Nothing drains the pipeline first, so the repartition lands while the
// previous draw's vertices are still in flight, and the machine stops. No
// further register write can fix that, because a register write is just another
// command behind the ones already queued.
//
// **Ending the command list does fix it.** `C3D_FrameSplit` hands what has been
// recorded to the GPU and starts a new list; the GPU finishes the first list
// before it begins the second, so the shader-unit change in list two happens
// after every draw in list one has retired. That is the drain, and it is the
// only one available from outside citro3d.
//
// So the ramp now tests the fix rather than the fault: the mitigation is on for
// the first two rungs and off for the third. Surviving the first two and dying
// on the third is the whole proof -- it says both that the split works and that
// its absence is what was killing the console.
constexpr GeoRampStep kGeoRamp[] = {
    {true, false, 1, 16, "split on, 1 draw, 16 quads"},
    {true, false, 0, 0, "split on, no limit"},
    {false, false, 0, 0, "split OFF, no limit"},
};
constexpr int kGeoRampSteps = int(sizeof(kGeoRamp) / sizeof(kGeoRamp[0]));

// Frames to hold a step before loosening again. A second at 30 fps: long enough
// that a step which only fails sometimes still gets several chances, short
// enough that the whole ramp is over in about ten seconds.
constexpr int kGeoRampFrames = 90;

// **Off, its job done.** It found the fault in four launches and then proved the
// fix in a fifth. Kept rather than deleted because the next path that programs
// the GPU differently will want exactly this, and rebuilding it from the
// description is more work than switching it back on.
constexpr bool kGeoAutoRamp = false;

// The watchdog's deadline, in seconds. **The same one the menu's frame uses**
// -- one number, in renderer.hpp, because two copies of a deadline is two
// deadlines that drift. See Renderer::beginFrameOrGiveUp.
constexpr float kGeoWatchdogSeconds = kFrameWatchdogSeconds;

// **After the first stall, retry cheaply.** Spending the full deadline every
// frame would leave the console technically alive at half a frame a second,
// which is not far enough from dead to be worth the distinction. One frame's
// worth is long enough to catch a GPU that comes back and short enough that the
// buttons and the bottom screen stay usable while it does not.
constexpr float kGeoRetrySeconds = 0.032f;

// **citro3d does not bounds-check its command buffer.** `GPUCMD_AddRawCommands`
// memcpys into `gpuCmdBuf + offset` and advances the offset; nothing anywhere
// compares that against `gpuCmdBufSize`. Overrun it and the writes land in
// whatever linear allocation follows -- and since what follows is the linear
// heap itself, the fault does not surface as bad geometry. It surfaces as the
// *next* `linearAlloc` or `linearFree` walking a smashed free list, which on
// hardware is an exception screen or a reboot, somewhere else entirely, with
// nothing on the GPU's side to show for it.
//
// The draw list is what fills it, and **stereo is what makes it reachable**:
// the same view costs twice the commands, which is why the symptom was "it
// crashes in 3D, sometimes".
//
// **The budget is per frame and nothing can extend it mid-frame.** This is the
// part that was got wrong, so it is written out: `GPUCMD_Split` -- read out of
// libctru's own disassembly, not assumed -- does
//
//     gpuCmdBuf += offset;  gpuCmdBufSize -= offset;  gpuCmdBufOffset = 0;
//
// Free space is `size - offset`. Before the split that is `size - offset`;
// after it, `(size - offset) - 0`. **The same number.** A split hands the
// recorded words to the GX queue and starts a new list *in the space that was
// left*, so it cannot buy a single word. The previous version of this guard
// split when a section would not fit and then drew the section anyway, which
// is exactly the overrun it was written to prevent, plus a wasted queue entry.
//
// So the guard stops drawing instead. When there is no room for another
// section the frame gives up on the rest of its geometry: holes in the world,
// far ones first, and a counter that says so. That is the only choice this
// side of the ceiling that is not memory corruption.
//
// The ceiling itself is `ctr::kCommandBufferBytes`. At the ~43 words a section
// draw costs -- a buffer-info bind, four dirty float uniforms in the PICA's
// 24-bit packing, and eleven register writes for the draw -- 1 MB is a little
// over six thousand draws a frame, which covers the render distances the
// performance gate is written against and does not cover the debug page's
// ceiling of 24. `FrameStats::commandWords` on the Info page is what says
// which of those a given view is.
constexpr u32 kCommandWordsPerSection = 1024;

// **The geometry-shader bind is the fattest thing this engine records**, and the
// reserve above was sized against the other two. `C3D_BindProgram` sets both
// `C3DiF_VshCode` and `C3DiF_GshCode` whenever the DVLP changes, and the quad
// program is two DVLEs sharing one code blob -- so every switch onto it inlines
// that blob into the command list *twice*, once for the vertex stage and once
// for the geometry stage, plus both operand-descriptor tables, plus 23 float
// uniform vectors where the 12-byte path has five.
//
// Counted rather than guessed: roughly 430 words worst case against a 1024-word
// reserve, so 1024 is not actually too small today. It is doubled anyway,
// because the margin is what the reserve is *for*.
constexpr u32 kCommandWordsPerQuadSection = 2048;

// Words left in the command buffer, across the current list and everything
// after it. `size` shrinks by what each split handed over, so this is the whole
// remaining budget and not just this list's share.
u32 commandWordsFree()
{
    u32 size = 0;
    u32 offset = 0;
    GPUCMD_GetBuffer(nullptr, &size, &offset);

    // Written as a comparison rather than a subtraction because both are u32:
    // an offset that has somehow already passed the end would otherwise wrap to
    // a headroom of four billion and wave every draw through.
    return offset >= size ? 0 : size - offset;
}

}  // namespace

// way rather than drawing each section's two ranges together is the whole
// reason the two streams are kept apart in the mesh: the program and the
// attribute layout change twice per eye instead of twice per section.
void Renderer::drawPass(const C3D_Mtx& vp, Pass pass, i32 originChunkX, i32 originChunkZ)
{
    // The budget went in an earlier pass of this frame. Nothing gets it back,
    // so there is nothing to do here but leave the geometry out.
    if (commandBudgetSpent_) {
        return;
    }

    const render::VboPool& pool = chunks_.pool();
    const bool detail = pass != Pass::Cube;

    // Blending is not associative with the frame buffer, so the translucent
    // pass has to arrive far to near. The draw list is breadth-first out of the
    // camera's own section, which is near to far, so walking it backwards is
    // the whole sort -- at section granularity, which is what the original
    // does too: it sorts chunks, not quads.
    const std::vector<render::VisibleSection>& list = chunks_.drawList();
    const int count = int(list.size());
    const bool reversed = pass == Pass::Translucent;

    // Which program is bound, tracked rather than assumed. The cube pass has
    // two of them, chosen by the mesh's own recorded format: a section built
    // before a format switch would otherwise be drawn through the wrong shader
    // and the wrong attribute layout, which is the one way this could go wrong
    // silently.
    const Pipeline* bound = nullptr;

    // Only counts quad meshes, and only in the cube pass; see kGeoMaxSections.
    // Published to the member below so drawEye knows whether the pass it just
    // ran actually put a geoshader draw in the list -- an empty one needs no
    // split, and splits are not free.
    int geoSectionsDrawn = 0;

    for (int i = 0; i < count; ++i) {
        const render::VisibleSection& section = list[reversed ? count - 1 - i : i];

        // **The list is a snapshot; the pool is not.** This frame's meshing ran
        // after buildVisibleSet, and an upload or an eviction can have handed
        // this slot to a different section since. Drawing it anyway renders
        // that section's vertices through this section's model matrix -- a
        // chunk of world standing somewhere it does not belong. The generation
        // was recorded when the list was built; a mismatch means the slot
        // changed hands and there is nothing here to draw.
        if (!pool.resident(section.slot)
            || pool.generation(section.slot) != section.slotGeneration) {
            ++frameStats_.staleSkipped;
            continue;
        }

        const mesh::MeshRanges& ranges = pool.ranges(section.slot);
        const bool geoQuads = pass == Pass::Cube && ranges.cubeFormat == mesh::CubeFormat::Quads;
        const usize stride = pass == Pass::Cube
                                 ? (geoQuads ? sizeof(mesh::QuadVertex) : sizeof(mesh::WorldVertex))
                                 : sizeof(mesh::DetailVertex);
        const usize bytes = pass == Pass::Cube          ? ranges.cubeBytes
                            : pass == Pass::Detail      ? ranges.detailBytes
                                                        : ranges.translucentBytes;
        int quads = pass == Pass::Cube ? int(ranges.cubeQuads()) : int(bytes / (4 * stride));
        if (quads == 0) {
            continue;
        }

        // The bisect knobs, in the one place a quad draw can be cut short.
        if (geoQuads) {
            // The watchdog has already caught this path wedging the GPU once.
            // The section is left undrawn -- a hole in the world, which is what
            // a player should see rather than a console that has to be held
            // down to turn off.
            if (quadDrawsStopped_) {
                continue;
            }
            const int sectionLimit = geoSectionLimit();
            const int quadLimit = geoQuadLimit();
            if (sectionLimit != 0 && geoSectionsDrawn >= sectionLimit) {
                continue;
            }
            if (quadLimit != 0 && quads > quadLimit) {
                quads = quadLimit;
            }
            ++geoSectionsDrawn;
        }

        // **The shared index buffer is a hard bound, and only the cube pass has
        // a proof it fits.** kMaxQuadsPerSection is derived from the
        // checkerboard -- half the cells solid, each showing all six faces --
        // which bounds a pass whose faces are culled against their neighbours.
        // The detail pass has no such derivation: a torch emits five quads and
        // a plant four whatever is beside them, so a dense enough section can
        // ask for more indices than exist. C3D_DrawElements would then read
        // past the 144 KB array into whatever linear memory follows it, and
        // garbage indices fetch vertices from outside the bound buffer --
        // polygons stretched to nothing, or a GPU fault.
        //
        // The mesher stops emitting at the bound (see mesh::kMaxQuadsPerSection)
        // so this should never fire; it is here because the mesher's guard was
        // an assert, asserts are compiled out in Release, and the console is
        // the only build that matters for this failure.
        if (quads > mesh::kMaxQuadsPerSection) {
            quads = mesh::kMaxQuadsPerSection;
            ++frameStats_.clampedDraws;
        }

        // **The last point at which stopping is still free.** Everything below
        // records into the command buffer, so the room for all of it has to be
        // there before any of it is written.
        const u32 reserve = geoQuads ? kCommandWordsPerQuadSection : kCommandWordsPerSection;
        if (commandWordsFree() < reserve) {
            // What is left of *this* pass's list. The passes after it stop at
            // the top of drawPass and add nothing, so this reads as "the frame
            // ran out here" rather than as an exact count of missing geometry
            // -- some of the tail would have been empty or stale anyway. It is
            // a warning light, and the number beside it, `cmd`, is the
            // measurement.
            frameStats_.droppedSections += count - i;
            commandBudgetSpent_ = true;
            break;
        }

        // Bound lazily, so a frame with no non-cube geometry anywhere in view
        // costs nothing at all for the second pass.
        const Pipeline* want = detail       ? &detailPipeline_
                               : geoQuads   ? &quadPipeline_
                                            : &cubePipeline_;
        if (bound != want) {
            bindPipeline(*want);
            bound = want;
        }

        // Vertex positions are section-local -- 0..16 in blocks for cubes,
        // 0..16384 in 1/1024 blocks for detail -- so the section's origin lives
        // in the matrix. That is what keeps the position three bytes for cubes
        // instead of three floats.
        //
        // **The origin is relative to the camera's chunk**, matching the eye
        // that viewProjection put at the same place. Writing the absolute
        // `chunkX * 16` here would still be exact as a float -- it is a
        // multiple of 16 -- but the matrix it goes into holds the camera's
        // position negated, so the product is a difference of two numbers
        // around 12,550,000 whose float spacing is a whole block. That
        // cancellation is the far-from-origin wobble: geometry shivers by a
        // block as the camera moves, and it gets worse the further out you go.
        // Subtracting first, in integers, means the GPU only ever sees the
        // small number.
        //
        // The model matrix is a pure translation, so the product is vp with one
        // column replaced: mvp[:,0..2] = vp[:,0..2] and mvp[:,3] = vp * (t,1).
        // 12 multiplies instead of 64, exactly equal, about 1,800 times a
        // stereo frame. C3D_Mtx stores rows reversed -- c[i] is column 3-i --
        // which is why the offsets below look backwards.
        const float tx = float(section.chunkX - originChunkX) * 16.0f;
        const float ty = float(section.sectionY) * 16.0f;
        const float tz = float(section.chunkZ - originChunkZ) * 16.0f;

        C3D_Mtx mvp = vp;
        for (int row = 0; row < 4; ++row) {
            const float* r = vp.r[row].c;
            mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
        }

        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, bound->uLocMvp, &mvp);

        const u8* base = static_cast<const u8*>(pool.data(section.slot));
        base += pass == Pass::Cube          ? 0
                : pass == Pass::Detail      ? ranges.detailOffset()
                                            : ranges.translucentOffset();

        C3D_BufInfo bufInfo;
        BufInfo_Init(&bufInfo);
        if (geoQuads) {
            BufInfo_Add(&bufInfo, base, stride, 2, 0x10);
        } else {
            BufInfo_Add(&bufInfo, base, stride, 3, 0x210);
        }
        C3D_SetBufInfo(&bufInfo);

        if (geoQuads) {
            // No index buffer at all: one input vertex per quad, and the
            // geometry shader emits the two triangles. GPU_GEOMETRY_PRIM is
            // what hands primitive assembly to it.
            C3D_DrawArrays(GPU_GEOMETRY_PRIM, 0, quads);

            // **The one place a geoshader draw enters the queue**, so the one
            // place the latch that guards every wait on that queue can be set
            // honestly. See beginFrameOrGiveUp.
            geoWorkInFlight_ = true;
        } else {
            C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        }

        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
    }

    if (pass == Pass::Cube) {
        geoDrawsLastCubePass_ = geoSectionsDrawn;
    }
}

void Renderer::drawEye(int eye, const Camera& camera, float iod)
{
    C3D_RenderTargetClear(eye_[eye], C3D_CLEAR_ALL, kSkyColour, 0);
    C3D_FrameDrawOn(eye_[eye]);

    const C3D_Mtx vp = viewProjection(camera, iod, C3D_AngleFromDegrees(config_.fovDegrees));

    // The same origin viewProjection used. Recomputed rather than passed
    // around, because the two must not be able to disagree -- a mismatch of
    // one chunk between the eye and the section matrices is a 16-block shift
    // of the entire world, and it would only show up while moving.
    const i32 originChunkX = camera.chunkX();
    const i32 originChunkZ = camera.chunkZ();

    // The alpha test is on for every pass and never changes within a frame --
    // drawFrame sets it once. The cube pass was the one place it was worth
    // questioning, since the PICA drops early-Z while the test is on and that
    // pass carries 97 % of the world; measured on hardware, turning it off
    // changed GPU draw by nothing measurable. See drawFrame.
    // **The geometry stage is turned on by the bind inside this pass**, and on
    // the second eye that bind follows the first eye's detail draws. Same
    // hazard as the one on the way out, so the same drain: end the list first,
    // and the repartition happens with nothing of the previous pass in flight.
    // `C3Di_SplitFrame` returns early when nothing has been recorded, so the
    // first pass of a frame pays nothing for this.
    const bool splitAroundGeo =
        cubeFormat_ == mesh::CubeFormat::Quads && geoSplitPasses();
    if (splitAroundGeo) {
        C3D_FrameSplit(0);
        ++frameStats_.geoSplits;
    }

    drawPass(vp, Pass::Cube, originChunkX, originChunkZ);

    // **And the drain on the way out, which is the one the hardware asked for.**
    // Without it the next pass's bind writes GPUREG_VSH_COM_MODE and
    // GPUREG_GEOSTAGE_CONFIG into the same list the geoshader draws are still
    // being consumed from, and the GPU stops for good. Skipped when the pass put
    // no geoshader draw in the list, because then there is nothing to drain.
    if (splitAroundGeo && geoDrawsLastCubePass_ > 0) {
        C3D_FrameSplit(0);
        ++frameStats_.geoSplits;
    }

    // The bisect knob: with the quad format live, stop here. The detail pass is
    // the first thing that follows a geoshader draw, and it is also the first
    // thing that reads a buffer base the 12-byte path can never produce -- so
    // "cube pass only" separates "the geoshader draw wedges the GPU" from "what
    // runs after one does".
    if (geoCubePassOnly() && cubeFormat_ == mesh::CubeFormat::Quads) {
        return;
    }

    // Opaque non-cube geometry is drawn without back-face culling: a flower is
    // two crossed planes and the original has no culling to satisfy, so both
    // sides of each plane are meant to be seen. The mesher emits both windings,
    // and culling them would halve every cross.
    C3D_CullFace(GPU_CULL_NONE);
    drawPass(vp, Pass::Detail, originChunkX, originChunkZ);
    C3D_CullFace(GPU_CULL_BACK_CCW);

    // Water and ice, the two things a1.1.2 puts in its second terrain pass.
    //
    // Three state changes, and each one is doing a job. **Blending** is the
    // point. **Depth writes off** is what lets two water surfaces behind one
    // another both contribute instead of the nearer one hiding the further --
    // the depth *test* stays on, so the sea floor still occludes the sea. And
    // **culling back on**, unlike the opaque detail pass above, because a fluid
    // face is single-sided and wound outward: drawing its back face too would
    // blend the same surface into the frame buffer twice and make every lake
    // look twice as deep as the one next to it.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    drawPass(vp, Pass::Translucent, originChunkX, originChunkZ);

    // **After everything, including the translucent pass.** The outline is a
    // UI element that happens to live in the world: drawn earlier it would be
    // sorted against water and glass, and the one thing it must always be is
    // visible on the block the crosshair is on.
    drawSelection(vp, originChunkX, originChunkZ);

    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
}

void Renderer::applyWorldState()
{
    C3D_CullFace(GPU_CULL_BACK_CCW);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);

    // Stated rather than inherited. drawEye leaves this exactly here on its way
    // out of the translucent pass, so within a run of frames it is already
    // right -- but the *first* frame after anything else has drawn inherits
    // whatever that left, and citro2d leaves src-alpha blending on. That used
    // to happen once, on the way out of the main menu; with a pause menu it
    // happens every time a player resumes.
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    atlas_.bind(0, wireframe_);
    lightmap_.bind(1);

    // **Always on.** This used to be enabled only for wireframe, on the
    // reasoning that nothing else cared what a texture's alpha meant. Every
    // cutout shape in the game cares: a torch is four *full-block* quads with
    // the stick carved out of them by transparency and nothing else, so with
    // the test off it drew as four black squares -- the transparent texels are
    // rgba(0,0,0,0) and an untested fragment writes that as opaque black.
    // Glass, leaves and the crossed squares are the same shape of bug waiting
    // for a texture pack with real alpha in it.
    //
    // Wireframe needs no threshold of its own: its atlas is strictly 255 or 0,
    // so the original's reference separates it just as well. What wireframe
    // still changes is the *combiner*, below -- discarding the interior is
    // what lets the mesh behind show through, because a discarded fragment
    // writes no depth.
    //
    // Set once here and not touched again for the rest of the frame. The cube
    // pass used to be drawn with it switchable, to size what early-Z would be
    // worth in the pass carrying 97 % of the world's geometry; the answer from
    // hardware was nothing measurable, because this renderer never reaches the
    // fragment stage in quantity. The test is correct, faithful and free.
    C3D_AlphaTest(true, GPU_GREATER, kAlphaTestRef);

    // Three combiner stages, and the alpha channel is deliberately routed round
    // all of them.
    //
    // The vertex colour's alpha is no longer a constant 1: the vertex shader
    // puts the fog amount there, because primary colour is the only per-vertex
    // value a texenv stage can read and the three colour bytes already carry
    // face shade. So every stage below replaces alpha with the texture's own
    // rather than modulating it -- otherwise water's alpha would be multiplied
    // by how foggy it is and lakes would turn opaque as they receded.

    // Stage 0: atlas x vertex colour. The colour is face shade (x AO later), all
    // of it static, which is why none of it has to be rebuilt when the sun moves.
    C3D_TexEnv* env0 = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env0);
    C3D_TexEnvSrc(env0, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env0, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env0, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env0, C3D_Alpha, GPU_REPLACE);

    // Stage 1: x lightmap, sampled at the (block, sky) coordinate the shader
    // unpacked from the vertex's light byte.
    //
    // Both this and the fog below are skipped in wireframe, which is the whole
    // reason they are separate stages: a wireframe that dims into a cave or
    // fades out at the render distance hides exactly what it was turned on to
    // show. Face shade from stage 0 stays, so the six face directions are still
    // tellable apart.
    C3D_TexEnv* env1 = C3D_GetTexEnv(1);
    C3D_TexEnvInit(env1);
    if (!wireframe_) {
        C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
        C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
        C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
    }

    // Stage 2: fade to the sky colour. INTERPOLATE is arg0*arg2 + arg1*(1-arg2),
    // so with the fog amount in arg2 this is lerp(lit texel, sky, fog) -- the
    // same thing GL_LINEAR fog does, and applied before blending, as GL does.
    C3D_TexEnv* env2 = C3D_GetTexEnv(2);
    C3D_TexEnvInit(env2);
    if (!wireframe_) {
        C3D_TexEnvSrc(env2, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env2, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(env2, kFogColour);
        C3D_TexEnvSrc(env2, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
        C3D_TexEnvFunc(env2, C3D_Alpha, GPU_REPLACE);
    }

}

// **C3D_FrameBegin with a deadline, and only while the quad format is live.**
//
// `C3D_FRAME_SYNCDRAW` is two waits: `C3D_FrameSync`, which blocks until the
// GPU has finished the previous list, and then the queue drain. Neither is
// bounded, so a command list the GPU never completes takes the main thread with
// it -- and the main thread is also `aptMainLoop`, which is why the symptom is
// both screens dead and HOME doing nothing. That is exactly what the geometry
// shader path did on its first hardware run.
//
// **Be honest about what this buys.** If the GPU is genuinely wedged, nothing
// in this process can un-wedge it; the loop below will spin out its deadline,
// latch the stall, and then still have no frame to open. What it does buy is
// the difference between a console that dies silently and one that says *which*
// path killed it, plus a real chance at the softer failure -- a queue that is
// merely backed up -- where stopping the quad draws lets the next list be small
// enough to get through.
//
// **On expiry it opens no frame and says so, and the caller must draw nothing.**
// The first version of this fell back to the blocking `C3D_FRAME_SYNCDRAW` after
// the deadline, on the theory that there was nothing else to do -- and that made
// the whole watchdog useless, because a wedged GPU never returns from it either.
// The console still died with nothing on screen. Giving up properly is what lets
// the loop keep running: input, ticks and saves carry on, the top screen holds
// whatever it last managed, and the bottom screen -- which needs no GPU at all,
// see geoTrace -- gets to say what happened.
int Renderer::geoSectionLimit() const
{
    return kGeoAutoRamp ? kGeoRamp[geoRampStep_].sections : kGeoMaxSections;
}

int Renderer::geoQuadLimit() const
{
    return kGeoAutoRamp ? kGeoRamp[geoRampStep_].quads : kGeoMaxQuads;
}

bool Renderer::geoCubePassOnly() const
{
    return kGeoAutoRamp ? kGeoRamp[geoRampStep_].cubeOnly : kGeoCubePassOnly;
}

bool Renderer::geoSplitPasses() const
{
    return kGeoAutoRamp ? kGeoRamp[geoRampStep_].splitPasses : kGeoSplitPasses;
}

bool Renderer::beginFrameOrGiveUp()
{
    // **The question is what is in the queue, not what the settings page says.**
    //
    // This wait is for the *previous* frame, and the previous frame is the one
    // that could wedge -- so gating it on `cubeFormat_` was gating it on the
    // wrong frame. Toggling the debug page back to 4-vertex flipped that field
    // instantly, and the very next `drawFrame` took the unbounded
    // `C3D_FRAME_SYNCDRAW` on a queue still holding a geoshader list. If that
    // list was the one that wedged, the main thread -- which is also
    // `aptMainLoop` -- never came back: top screen black, HOME dead, no
    // exception, and not a word from the watchdog, because the watchdog is the
    // branch that was skipped. Switching the format back and forth is exactly
    // how a player reaches it.
    //
    // `geoWorkInFlight_` is what was actually recorded, cleared only where the
    // queue is proven empty, so it stays true across the toggle for as long as
    // it has to. The other two are kept: the format because the frame about to
    // be recorded needs the same protection, and the stall latch because once
    // it has fired the format has already been dropped back and taking the
    // blocking wait on the strength of that would hang on the very next frame.
    if (!geoWorkInFlight_ && cubeFormat_ != mesh::CubeFormat::Quads && !quadDrawsStopped_) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        return true;
    }

    // **The pacing half of C3D_FRAME_SYNCDRAW, kept.** `C3D_FrameSync` waits on
    // the two VBlank counters, not on the GPU -- it is the frame-rate limiter,
    // and VBlank keeps firing whether or not the GPU is finishing anything. So
    // it always returns, and skipping it (which the first version of this did,
    // by using NONBLOCK alone) let the game free-run at whatever rate the CPU
    // managed. That is not a difference the quad format should carry into a
    // measurement of the quad format.
    //
    // What can wedge is the other half: the queue drain. That is the only part
    // below the deadline.
    C3D_FrameSync();

    // **The deadline follows the GPU's current state, not the session's
    // history**, and getting that backwards was a hang of its own. It used to
    // be `gpuStalls_ == 0 ? kGeoWatchdogSeconds : kGeoRetrySeconds`, so a
    // single stall anywhere in a session put *every* later frame on the 32 ms
    // retry deadline -- for good, since `gpuStalls_` is never cleared, and
    // including after the cube format had been dropped back to the one that is
    // known to draw. At the Far Lands a perfectly healthy frame's queued work
    // can outlast 32 ms, so the watchdog then fired on merely-slow frames and
    // `drawFrame` returned having drawn nothing, every frame, forever: a top
    // screen frozen on the last good frame with HOME still working, which from
    // the couch is indistinguishable from the console being dead.
    //
    // A frame that completes is proof the GPU came back, so the short deadline
    // applies only while it has not. `gpuWedged_` is that state; `gpuStalls_`
    // stays the history the debug page reports.
    if (!pollFrameBegin(gpuWedged_ ? kGeoRetrySeconds : kGeoWatchdogSeconds)) {
        // Once per episode rather than once per session, so a second wedge
        // after a recovery is reported too and carries its own numbers.
        if (!gpuWedged_) {
            // The frame that did not come back is the one recorded before this
            // one started, so this is what was in it.
            stalledStats_ = previousStats_;
            if (geoRampName_ != nullptr) {
                // The rung is the answer, so it goes in the line that survives
                // rather than only on a page nobody may reach.
                std::snprintf(stallLine_, sizeof(stallLine_), "WEDGED at [%s]", geoRampName_);
                geoTrace(stallLine_);
            } else {
                geoTrace("GPU never finished a list -- see the settings page");
            }
        }
        ++gpuStalls_;
        gpuWedged_ = true;
        quadDrawsStopped_ = true;
        return false;
    }

    // The queue is empty, which is the only proof there is that whatever
    // geoshader draws it held have retired -- and the only proof that the GPU
    // is finishing lists again.
    geoWorkInFlight_ = false;
    gpuWedged_ = false;
    return true;
}

void Renderer::drawFrame(const Camera& camera, void* overlayContext, Overlay2D overlay)
{
    // Kept for one frame, because the frame the GPU fails to finish is the one
    // recorded *before* the FrameBegin that never returns.
    previousStats_ = frameStats_;
    frameStats_ = FrameStats{};

    // The budget is refilled by C3D_FrameBegin and by nothing else, so the
    // latch is cleared here rather than at the end of the frame that set it.
    commandBudgetSpent_ = false;

    // Skip the second eye entirely at slider zero rather than rendering it and
    // throwing it away -- half the geometry cost, for free.
    const float slider = osGet3DSliderState();
    const bool wantStereo = slider > 0.0f
                            && !(kGeoForceMono && cubeFormat_ == mesh::CubeFormat::Quads);
    if (wantStereo != stereo_) {
        gfxSet3D(wantStereo);
        stereo_ = wantStereo;
    }
    frameStats_.stereo = stereo_;

    // Everything the eyes depend on, applied per eye rather than once for the
    // frame -- see applyWorldState. Between two eyes there may now be a 2D
    // pass, and citro2d leaves the depth test, the culling and two combiner
    // stages set to its own taste.
    const float fovRadians = C3D_AngleFromDegrees(config_.fovDegrees);
    const float interocular =
        interocularForDisparity(disparityPixels_, focalBlocks_, fovRadians, 400.0f / 240.0f);

    // Timed in three pieces because the answer to "why is a frame 17.5 ms when
    // the GPU says 0.8" is in which of them is big. C3D_FrameBegin under
    // C3D_FRAME_SYNCDRAW blocks until the next VBlank, so on a healthy frame it
    // is *supposed* to be the large one and the console is simply at 59.83 Hz.
    const u64 beforeBegin = svcGetSystemTick();
    const bool haveFrame = beginFrameOrGiveUp();
    const u64 afterBegin = svcGetSystemTick();

    // No frame was opened, so there is nothing to draw into and nothing to end.
    // The game loop carries on around this: the bottom screen still updates and
    // the buttons still work, which is the whole point of coming back at all.
    if (!haveFrame) {
        blockedMs_ = millisFromTicks(afterBegin - beforeBegin);
        submitMs_ = 0.0f;
        return;
    }

    // **The ramp advances here and nowhere else** -- after a FrameBegin that
    // came back, which is the only proof the previous frame's list completed.
    // Advancing before that wait would credit a rung with surviving a frame the
    // GPU had not finished, and the rung being held is the whole answer.
    if (kGeoAutoRamp && cubeFormat_ == mesh::CubeFormat::Quads && !quadDrawsStopped_) {
        if (geoRampName_ == nullptr) {
            geoRampName_ = kGeoRamp[geoRampStep_].name;
            geoTrace(geoRampName_);
        } else if (++geoRampHeld_ >= kGeoRampFrames && geoRampStep_ + 1 < kGeoRampSteps) {
            ++geoRampStep_;
            geoRampHeld_ = 0;
            geoRampName_ = kGeoRamp[geoRampStep_].name;
            geoTrace(geoRampName_);
        }
    }

    for (int i = 0; i < (stereo_ ? 2 : 1); ++i) {
        applyWorldState();
        const float offset = slider * interocular * 0.5f;
        const float iod = stereo_ ? (i == 0 ? -offset : offset) : 0.0f;
        drawEye(i, camera, iod);

        // The pause menu, drawn into this frame on top of the world it is
        // pausing. Both eyes get it at the same screen position, so it sits at
        // the screen plane whatever the slider is doing.
        if (overlay != nullptr) {
            overlay(overlayContext, eye_[i]);
        }
    }
    // **Read before C3D_FrameEnd**, which splits one last time and hands the
    // buffer back: after it, what is left says nothing about what the frame
    // spent. The overlay's own draws are inside this figure, which is right --
    // they come out of the same budget as the world does.
    frameStats_.commandWords = kCommandBufferBytes / 4 - commandWordsFree();

    C3D_FrameEnd(0);

    blockedMs_ = millisFromTicks(afterBegin - beforeBegin);
    submitMs_ = millisFromTicks(svcGetSystemTick() - afterBegin);
    drawMs_ = C3D_GetDrawingTime();
    processMs_ = C3D_GetProcessingTime();
}

usize Renderer::freeVramBytes() const
{
    return vramSpaceFree();
}

usize Renderer::freeLinearBytes() const
{
    return linearSpaceFree();
}

}  // namespace mc::ctr
