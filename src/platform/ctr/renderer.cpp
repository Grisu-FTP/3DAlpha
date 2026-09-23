#include "platform/ctr/renderer.hpp"
#include "platform/ctr/bottom_screen.hpp"

#include "core/mesh/vertex.hpp"
#include "core/render/remote_player_mesh.hpp"
#include "core/render/sky.hpp"
#include "core/texture/dev_art.hpp"
#include "core/world/daylight.hpp"
#include "core/world/view_fog.hpp"
#include "core/texture/entity_skins.hpp"

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
// The console draws off-screen now (platform/ctr/bottom_screen.hpp), but
// `bottom::flush` copies it across synchronously -- no swap, no GPU, no
// completed frame required. A line printed here is on the screen before the
// next instruction runs, which makes it the one report channel that survives
// the thing being investigated. Row 30 is below the overlay's footer at 28-29.
//
// Only ever called on the quad path, and only at the few points worth naming:
// a printf a frame would cost more than it tells.
void geoTrace(const char* what)
{
    std::printf("\x1b[30;1H\x1b[2K\x1b[33mgeo: %s\x1b[0m", what);
    bottom::flush();
}


namespace {

// **The sky and the fog are the same colour and neither is a constant any more.**
//
// They used to be one daylit blue each, written here. a1.1.2 computes both from
// the time of day -- `World.getFogColor` pulled towards `World.getSkyColor` by
// the render distance, in `EntityRenderer.updateFogColor` -- and hands the same
// three floats to `glClearColor` and `glFogfv(GL_FOG_COLOR)`, which is what
// keeps the horizon seamless: terrain at the fog end lands on exactly the
// colour behind it. Both now live in `Renderer::clearColour_` and `fogColour_`,
// written once a frame by setWorldTime. See core/world/daylight.hpp.
//
// **Two packings, because the two consumers disagree**, and getting them the
// same way round tints the whole screen. A texenv constant is 0xAABBGGRR --
// alpha in the top byte, red in the bottom -- and `C3D_RenderTargetClear` takes
// the render target's own RGBA8, which is the exact reverse.
u32 colourByte(float value)
{
    const float scaled = value * 255.0f + 0.5f;
    return u32(scaled < 0.0f ? 0 : (scaled > 255.0f ? 255 : int(scaled)));
}

u32 packAbgr(float r, float g, float b)
{
    return 0xFF000000u | (colourByte(b) << 16) | (colourByte(g) << 8) | colourByte(r);
}

u32 packRgba(float r, float g, float b)
{
    return (colourByte(r) << 24) | (colourByte(g) << 16) | (colourByte(b) << 8) | 0xFFu;
}

constexpr u32 kDisplayTransferFlags =
    GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0)
    | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8)
    | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

// **a1.1.2's own**, and the whole of the reason it has to be is that the near
// plane is not a plane at all -- it is a rectangle, with corners, and on this
// console it does not even sit in front of the eye.
//
// This was 0.2, traded up from the original's 0.05 for depth precision a 16-bit
// buffer could not spare. Both eyes are `GPU_RB_DEPTH24_STENCIL8` now, which
// multiplies that precision by 256, so the trade has nothing left to buy: at
// 0.05 and a 176-block far plane one depth unit still spans 0.037 of a block at
// the far plane itself. The cost was being paid at the near end, where it was
// visible -- three separate reports of the view cutting into solid blocks, and
// all three are this number.
//
// **What the near rectangle actually reaches**, per eye, with fov 70 on a 5:3
// screen (t = tan(fov/2) = 0.700, A = 400/240) and the default 7 px of infinity
// disparity. `Mtx_PerspStereoTilt` builds an off-axis frustum, so from the row
// read out of libcitro3d.a in `cullFrustum` the eye is displaced sideways by
// `t*A*iod/2` -- **0.19 of a block**, which is most of the way to the player's
// own half-width -- and the rectangle's far edge sits `t*A*near` past that:
//
//     near   sideways   upward    corner
//     0.2    0.420      0.244     0.486
//     0.05   0.248      0.061     0.255
//
// Against those: the player box is 0.6 across, so **0.30** to a wall it is flush
// with, and 1.8 tall against a 1.62 eye, so **0.18** to a ceiling its head is
// against. At 0.2 both are lost -- jumping into a ceiling clips through it, and
// standing along a wall clips through that, worst where a pitch swings the
// rectangle's corner into it. At 0.05 both clear, and the sideways figure is
// the tight one because the stereo offset does not shrink with the near plane.
//
// The stereo offset is horizontal in *screen* space, and nothing here rolls the
// camera, so it never costs ceiling clearance -- only wall clearance.
//
// What this gives up is what the original gives up: stand inside a block and
// the face is drawn rather than clipped away, which is the face filling the
// screen. That is the behaviour being matched.
constexpr float kNearPlane = 0.05f;

// **The sky's far plane, which is not the world's.**
//
// The original's is 256 blocks -- `256 >> renderDistance` at the Far setting --
// and it clips the far corners of the sky plane, which cannot be seen because
// they are fully fogged by then and the screen behind them is the fog colour.
// What it never clips is the sun and the moon, at 100 blocks with a corner at
// 108. This port's world far plane is the render distance plus a ring, which at
// four chunks is 80 -- so a sky drawn through it would have no sun in it at
// all. 512 is past everything the sky contains, and costs nothing: the pass
// writes no depth.
constexpr float kSkyFarPlane = 512.0f;


// **The hand's own, which is the same number the world now uses.** `iq.a(FI)V`
// sets the world up with `gluPerspective(fov, aspect, 0.05F, far)` and
// `renderHand` reuses it -- one near plane for both, as here.
//
// **Kept as its own constant even so**, because the two are equal by agreement
// and not by construction: this one is pinned to the geometry (the nearest
// corner of a held sword reaches 0.193 of a block in front of the eye, so
// anything above that clips the tip off it) and `kNearPlane` is pinned to the
// player's box. The pass builds its own projection regardless -- see
// `drawHeldItem` for the two numbers that really do differ, the stereo scale
// and the focal distance.
constexpr float kHeldItemNearPlane = 0.05f;

// **How much of the depth range the hand is given, and why it needs any.**
//
// The original clears the depth buffer before drawing the hand
// (`glClear(GL_DEPTH_BUFFER_BIT)` at offset 704 of `iq.c(F)V`), so the item
// can never be clipped by a wall the player is standing against. citro3d has
// no mid-frame depth clear -- `C3D_RenderTargetClear` picks what
// `C3D_FrameDrawOn` clears and nothing more -- and a full-screen quad to clear
// it by hand would be 96,000 fragments of pure overdraw on a fill-bound
// device.
//
// So the hand is put where the world cannot reach instead. Depth here is
// reversed -- the buffer is cleared to 0 and the test is GPU_GREATER, so
// nearer is *larger* -- and `C3D_DepthMap` scales what the projection produces
// before it is written. Compressed into the top 5 %, the hand beats any world
// fragment whose own depth is below 0.95, and window depth is `near/d` to a
// far plane's worth of rounding -- so a world fragment reaches 0.95 only within
// `kNearPlane / 0.95` of the eye, which the near plane itself clips all but the
// last 5 % of. The shell that is left is five millimetres deep and no block
// face can be in it without filling the screen. **It shrinks with the near
// plane**, so this got safer when that came down to 0.05, not riskier.
//
// 5 % and not 1 %: the hand still has to sort against *itself*, and an
// extruded icon is only a sixteenth of a unit thick. A twentieth of a 16-bit
// buffer leaves about a dozen depth levels across that thickness, which is
// enough; a hundredth leaves two or three, which is not.
constexpr float kHeldItemDepthScale = -0.05f;
constexpr float kHeldItemDepthOffset = 0.95f;

// **The hand is nearer than the eyes are far apart, and that is a stereo
// problem the world never has.**
//
// The separation this renderer uses is derived from an on-screen disparity, not
// picked: `interocularForDisparity` inverts
//
//     disparity_px(d) = 200 * (I/2) * [ 1/(F*t*a) - 1/d ]
//
// at infinity, and 7 px of infinity disparity at F = 8 comes out as I/2 = 0.33
// of a block. The held item sits between 0.19 and 1.37 blocks from the eye, so
// the `1/d` term reaches 5.2 -- put those numbers in and the nearest corner of
// a held sword lands **86 pixels** out of the screen. The 3DS convention is
// about 13 px and titles run to 20; 86 cannot be fused at all, and no choice of
// focal distance fixes it, because the object is closer to the eye than the two
// eyes are to each other.
//
// So this pass gets its own two numbers. The focal distance is the item's own
// depth, which puts it *on* the screen plane rather than in front of it; and
// the separation is a sixteenth of the world's, which is what holds the whole
// item inside the same 7 px the world's infinity is allowed. The slider still
// works -- `iod` is what it scales -- so turning 3D down still flattens the
// hand along with everything else.
//
// **A sixteenth is arithmetic, not a measurement.** 200 * (I/2) * 1.67 = 7 at
// the nearest corner solves to I/2 = 0.021 against the world's 0.327, and 1/16
// is the round number next to it. What it feels like on hardware is the kind of
// thing only hardware can say; see CONTRIBUTING.md.
constexpr float kHeldItemStereoScale = 1.0f / 16.0f;
constexpr float kHeldItemFocalBlocks = 0.72f;

// citro3d's own default, restored on the way out -- see C3D_Init.
constexpr float kDepthMapScale = -1.0f;
constexpr float kDepthMapOffset = 0.0f;

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

// **The focus hint**: the strip along the bottom of the top screen that says
// the bottom screen has the buttons.
//
// It is here rather than on the bottom screen because that is where the player
// is looking. The mode it reports is invisible from the world view -- the d-pad
// quietly means something else -- and a line of text on the screen that has
// just taken the focus is read by nobody, because the reason to focus the
// bottom screen is to look at it and the reason to be told is that you are not.
// So: a translucent grey band across the bottom of the world, with an
// arrowhead pointing down at the screen that now owns the buttons. No text, in
// any language, and nothing the eye has to leave the world to read.
//
// Grey and translucent rather than opaque so it dims the bottom rows of the
// world instead of cutting them off, and 14 pixels so it clears the hearts,
// which `render::buildHud` puts higher up.
constexpr int kTopScreenWidth = 400;
constexpr int kTopScreenHeight = 240;
constexpr int kFocusHintHeight = 14;
constexpr int kFocusArrowHeight = 6;
constexpr float kFocusArrowHalfWidth = 6.0f;
constexpr u32 kFocusHintVertices = 9;  // six corners of the band, three of the head
constexpr float kFocusHintGrey = 0.62f;
constexpr float kFocusHintAlpha = 0.42f;
constexpr float kFocusArrowGrey = 0.95f;
constexpr float kFocusArrowAlpha = 0.85f;

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

// **The matrix for a pass written in `render::kEntityUnitsPerBlock`**: the eye's
// block folded in as a translation, as every entity pass folds it, and then the
// scale from 1/256 of a block back to the 1/1024 the detail shader divides by.
// Translation first, from the unscaled columns, because the scale belongs
// inside it. `drawSigns` does the same at 1/512. See
// core/render/entity_range.hpp.
C3D_Mtx entityMatrix(const C3D_Mtx& viewProjection, float tx, float ty, float tz)
{
    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
        mvp.r[row].c[1] = r[1] * render::kEntityUnitScale;
        mvp.r[row].c[2] = r[2] * render::kEntityUnitScale;
        mvp.r[row].c[3] = r[3] * render::kEntityUnitScale;
    }
    return mvp;
}

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
    pipeline->uLocTint = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "tint");
    pipeline->uLocSeam = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "seam");
    pipeline->uLocSeamDir =
        shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "seamDir");

    AttrInfo_Init(&pipeline->attrs);
    if (detail) {
        // mesh::DetailVertex, 16 bytes.
        AttrInfo_AddLoader(&pipeline->attrs, 0, GPU_SHORT, 4);          // x,y,z,face  offset 0
        AttrInfo_AddLoader(&pipeline->attrs, 1, GPU_SHORT, 2);          // u, v        offset 8
        AttrInfo_AddLoader(&pipeline->attrs, 2, GPU_UNSIGNED_BYTE, 4);  // r,g,b,light offset 12
    } else {
        // mesh::WorldVertex, 12 bytes.
        AttrInfo_AddLoader(&pipeline->attrs, 0, GPU_SHORT, 2);          // u, v        offset 0
        AttrInfo_AddLoader(&pipeline->attrs, 1, GPU_UNSIGNED_BYTE, 4);  // x,y,z,seam  offset 4
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
    crosshairVerts_ = linearAlloc(sizeof(render::OutlineVertex) * 12);

    // 32 KB, once: 512 particles of four 16-byte vertices. It shares the
    // detail pipeline and the shared index buffer, so this allocation is the
    // whole cost of the particle pass.
    particleVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxParticleVertices));
    // 24 KB more, once, on the same argument: dropped items are built into it
    // every frame the pool is not empty and never allocate on a frame.
    itemVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxItemVertices));
    // 24 KB more: sixty-four falling blocks of six faces each.
    fallingVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxFallingVertices));
    // 36 KB more: ninety-six primed blocks of six faces each, plus 6.8 KB of
    // position-only triangles for the white flash over at most sixteen of them.
    tntVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxPrimedTntVertices));
    tntFlashVerts_ =
        linearAlloc(sizeof(render::OutlineVertex) * usize(kMaxPrimedTntFlashVertices));
    // 24 KB more: eight full-size paintings at six faces per 16 x 16 cell. See
    // kMaxPaintingVertices for why it is eight and not the pool's thirty-two.
    paintingVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxPaintingVertices));
    // 48 KB more: 128 arrows at six quads each.
    arrowVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxArrowVertices));
    // 60 KB more: thirty-two boats of five boxes each.
    boatVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxBoatVertices));
    // 72 KB more: thirty-two minecarts of six boxes each.
    minecartVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxMinecartVertices));
    minecartBlockVerts_ =
        linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxMinecartBlockVertices));
    // 108 KB more: twenty-four mobs at up to twelve boxes each.
    mobVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxMobVertices));
    // 6 KB more, and a buffer of its own for the same reason the flames have
    // one: the slime's shell is a *pass* and not a box, so it cannot share a
    // draw with the models it is drawn over. See `drawMobs`.
    mobShellVerts_ =
        linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kMobShellMaxVertices));
    // 6 KB more, and its own buffer for the same reason again: the spider's eye
    // page is a *pass* with its own blend, its own alpha source and -- unlike
    // the shell -- depth writes left on, so it cannot share a draw with either
    // of the two above it.
    mobEyeVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kMobEyeMaxVertices));
    // 16 KB more, and a buffer of its own rather than room at the end of the
    // one above: a mob's flames come off the *block* atlas and its body off the
    // entity sheet, so the two are different draws whatever they are built
    // into -- and the flames on a burning boat, cart, stack, falling block or
    // block of primed TNT go in here with them. See
    // core/render/entity_fire_mesh.hpp.
    entityFireVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxEntityFireVertices));
    // 240 KB, and it is the text that costs it -- see kMaxSignVertices.
    signVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxSignVertices));
    // 4 KB, and the smallest of the lot: one item, 66 quads at the worst.
    heldVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(kMaxHeldVertices));
    // 128 bytes for the flames over a burning player's view, built here and
    // never again. A version with no fire block builds nothing, and
    // `drawFireOverlay` then has nothing to draw.
    fireOverlayVerts_ =
        linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kFireOverlayVertices));
    if (fireOverlayVerts_ != nullptr) {
        auto* verts = static_cast<mesh::DetailVertex*>(fireOverlayVerts_);
        fireOverlayCount_ = render::buildFireOverlay(verts, render::kFireOverlayVertices);
        GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex)
                                         * u32(render::kFireOverlayVertices));
    }
    // 64 bytes for the water over a submerged player's view, rebuilt in each
    // eye it is drawn in. See core/render/water_overlay.hpp.
    waterOverlayVerts_ =
        linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kWaterOverlayVertices));
    // 70 KB for the whole sky -- two 169-quad planes, the sun, the moon and 780
    // stars -- written once below and never again. See core/render/sky.hpp.
    skyVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kSkyVertexCount));
    if (skyVerts_ != nullptr) {
        auto* verts = static_cast<mesh::DetailVertex*>(skyVerts_);
        if (render::buildSky(verts, render::kSkyVertexCount) == render::kSkyVertexCount) {
            GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex)
                                             * u32(render::kSkyVertexCount));
        } else {
            // Cannot happen -- the size is a compile-time constant of the same
            // header -- but a half-built sky would be geometry pointing at
            // uninitialised memory, so it is a sky that is not drawn instead.
            linearFree(skyVerts_);
            skyVerts_ = nullptr;
        }
    }

    // **The focus hint**, 108 bytes: six corners of a bar and three of an
    // arrowhead, in top-screen pixels, written here and never again. Nothing
    // about it moves -- what changes is whether it is drawn at all.
    focusHintVerts_ = linearAlloc(sizeof(render::OutlineVertex) * kFocusHintVertices);
    if (focusHintVerts_ != nullptr) {
        auto* hint = static_cast<render::OutlineVertex*>(focusHintVerts_);
        const float top = float(kTopScreenHeight - kFocusHintHeight);
        const float bottom = float(kTopScreenHeight);
        const float right = float(kTopScreenWidth);
        hint[0] = {0.0f, top, 0.0f};
        hint[1] = {right, top, 0.0f};
        hint[2] = {right, bottom, 0.0f};
        hint[3] = {0.0f, top, 0.0f};
        hint[4] = {right, bottom, 0.0f};
        hint[5] = {0.0f, bottom, 0.0f};
        // The arrowhead, centred, pointing at the screen below it.
        const float middle = float(kTopScreenWidth) * 0.5f;
        const float headTop = top + float(kFocusHintHeight - kFocusArrowHeight) * 0.5f;
        hint[6] = {middle - kFocusArrowHalfWidth, headTop, 0.0f};
        hint[7] = {middle + kFocusArrowHalfWidth, headTop, 0.0f};
        hint[8] = {middle, headTop + float(kFocusArrowHeight), 0.0f};
        GSPGPU_FlushDataCache(hint, sizeof(render::OutlineVertex) * kFocusHintVertices);
    }

    // 64 KB of glyphs and under a kilobyte of strips, for the chat lines.
    chatVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kChatMaxVertices));
    chatStrips_ = linearAlloc(sizeof(render::OutlineVertex) * 6u
                              * usize(mc::gui::kChatShownLines));
    // 3 KB for the hearts. A console without it draws no HUD, which `drawHud`
    // checks, rather than refusing to start.
    hudVerts_ = linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kHudMaxVertices));
    // 3 KB for the crack over a block being broken; the same terms.
    breakVerts_ =
        linearAlloc(sizeof(mesh::DetailVertex) * usize(render::kBreakOverlayMaxVertices));

    return outlineVerts_ != nullptr && crosshairVerts_ != nullptr && particleVerts_ != nullptr
           && itemVerts_ != nullptr && fallingVerts_ != nullptr && tntVerts_ != nullptr
           && tntFlashVerts_ != nullptr
           && paintingVerts_ != nullptr && arrowVerts_ != nullptr
           && boatVerts_ != nullptr && minecartVerts_ != nullptr
           && minecartBlockVerts_ != nullptr && mobVerts_ != nullptr
           && entityFireVerts_ != nullptr
           && signVerts_ != nullptr && heldVerts_ != nullptr && chatVerts_ != nullptr
           && chatStrips_ != nullptr;
}

void Renderer::setParticles(const mc::entity::ParticleSystem* particles,
                            const mc::render::Billboard& camera,
                            double eyeX, double eyeY, double eyeZ, float partial)
{
    particles_ = particles;
    particleCamera_ = camera;
    particleEyeX_ = eyeX;
    particleEyeY_ = eyeY;
    particleEyeZ_ = eyeZ;
    particlePartial_ = partial;
}

// **Built every frame, and that is the point.** A particle moves on every tick
// and every quad faces the camera, so there is nothing here that could be
// cached between frames the way the outline is: turning on the spot changes all
// four corners of all of them. What the frame path is not allowed to do is
// allocate or read the card, and this does neither -- the buffer was taken once
// at init, and the build only reads the pool.
void Renderer::drawParticles(const C3D_Mtx& viewProjection, i32 originChunkX,
                             i32 originChunkZ)
{
    if (particles_ == nullptr || particles_->count() == 0 || particleVerts_ == nullptr) {
        return;
    }

    // The quads are relative to the eye's *block*, not to the chunk origin the
    // rest of the frame uses: a detail position is a signed short of 1/1024
    // blocks and reaches 32 blocks, where a chunk-relative one would have to
    // reach the whole render distance. Flooring keeps the translation exact in
    // a float.
    const double eyeBlockX = std::floor(particleEyeX_);
    const double eyeBlockY = std::floor(particleEyeY_);
    const double eyeBlockZ = std::floor(particleEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(particleVerts_);

    // **The three sheets, in the order that costs the fewest binds.** The block
    // atlas is already bound from the passes above, so the digging flecks go
    // first and pay nothing; `particles.png` and `gui/items.png` each cost one
    // bind, and the atlas is put back at the end.
    //
    // **All three spans are built before any of them is drawn**, into disjoint
    // parts of the one buffer, for the reason `drawItemEntities` gives at
    // length: `C3D_DrawElements` records an *address* the GPU does not read
    // until `C3D_FrameEnd`, so building over a span that has already been
    // "drawn" draws the new geometry with the old texture.
    //
    // The three share one `DrawCutoff`, settled by the first -- which is handed
    // the whole buffer, as `draw_budget.hpp` requires.
    struct Span {
        mc::entity::ParticleSheet sheet;
        int count;
    };
    Span spans[3] = {
        {mc::entity::ParticleSheet::Terrain, 0},
        {mc::entity::ParticleSheet::Particles, 0},
        {mc::entity::ParticleSheet::Items, 0},
    };

    render::DrawCutoff cutoff;
    int built = 0;
    for (Span& span : spans) {
        // A pack with no `gui/items.png` cannot draw the two breaking kinds,
        // and neither can a console that had no memory for `particles.png`.
        // Both are a missing sprite rather than a lie, which is the same call
        // `drawItemEntities` makes for a dropped item.
        const bool haveSheet =
            span.sheet == mc::entity::ParticleSheet::Items    ? atlas_.hasItems()
            : span.sheet == mc::entity::ParticleSheet::Particles ? atlas_.hasParticles()
                                                                : true;
        if (!haveSheet) {
            continue;
        }
        span.count = render::buildParticles(*particles_, particleCamera_, eyeBlockX,
                                            eyeBlockY, eyeBlockZ, particlePartial_,
                                            span.sheet, verts + built,
                                            kMaxParticleVertices - built, &cutoff);
        built += span.count;
    }
    if (built < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(built));

    bindPipeline(detailPipeline_);

    // The same translation trick drawPass argues at length: the model matrix is
    // a pure translation, so the product is `vp` with one column replaced.
    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    bool rebound = false;
    int base = 0;
    for (const Span& span : spans) {
        if (span.count < 4) {
            base += span.count;
            continue;
        }
        if (span.sheet == mc::entity::ParticleSheet::Particles) {
            atlas_.bindParticles(0);
            rebound = true;
        } else if (span.sheet == mc::entity::ParticleSheet::Items) {
            atlas_.bindItems(0);
            rebound = true;
        }

        // **The base pointer is what separates the draws**, so each still
        // indexes from zero through the shared quad index buffer.
        C3D_BufInfo bufInfo;
        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, verts + base, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        // Four vertices a quad through the shared index buffer, exactly as a
        // section's detail range is drawn -- which is why no new index buffer
        // and no new shader were needed for any of this.
        const int quads = span.count / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
        base += span.count;
    }

    // **Put the block atlas back**, for the reason `drawItemEntities` gives:
    // everything after this in the frame assumes unit 0 is the atlas.
    if (rebound) {
        atlas_.bind(0, wireframe_);
    }
}

void Renderer::setItemEntities(const mc::entity::ItemEntitySystem* items,
                              float viewYawDegrees, double eyeX, double eyeY, double eyeZ,
                              float partial)
{
    items_ = items;
    itemViewYaw_ = viewYawDegrees;
    itemEyeX_ = eyeX;
    itemEyeY_ = eyeY;
    itemEyeZ_ = eyeZ;
    itemPartial_ = partial;
}

void Renderer::drawItemEntities(const C3D_Mtx& viewProjection, i32 originChunkX,
                                i32 originChunkZ)
{
    if (items_ == nullptr || items_->count() == 0 || itemVerts_ == nullptr) {
        return;
    }

    // The same eye-block origin the particles use, and for the same reason: a
    // detail position is a signed short of 1/1024 blocks and reaches 32.
    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    bindPipeline(detailPipeline_);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    auto* verts = static_cast<mesh::DetailVertex*>(itemVerts_);

    // **Both sheets are built before either is drawn, into disjoint halves of
    // the one buffer.** This used to build a sheet, draw it, and then build the
    // other over the top of the same vertices -- which reads as correct and is
    // not, because `C3D_DrawElements` records a command that names an *address*
    // and the GPU does not execute it until `C3D_FrameEnd`. Both draws
    // therefore ran against whatever the second `buildItemEntities` had left in
    // the buffer, so the terrain-sheet draw rendered the item-sheet geometry
    // with the terrain atlas on it -- a handful of quads in the wrong place
    // reading as **a dropped cobblestone that simply is not there** whenever
    // anything off gui/items.png was on the ground beside it. That is what
    // "many dropped items are invisible" was: not a missing icon, a buffer
    // written twice.
    //
    // The two spans cannot overflow between them, because every entity belongs
    // to exactly one sheet and both passes charge the one `itemCutoff`, which
    // the first settles against the whole buffer.
    render::DrawCutoff itemCutoff;
    const int terrainCount = render::buildItemEntities(*items_, itemViewYaw_, eyeBlockX,
                                                       eyeBlockY, eyeBlockZ, itemPartial_,
                                                       item::IconSheet::Terrain, verts,
                                                       kMaxItemVertices, &itemCutoff);
    // A pack with no gui/items.png. The bottom screen falls back to a terrain
    // tile for these; here there is nothing to fall back to that would not be a
    // lie, so they are not drawn. See core/texture/atlas_image.hpp.
    const int itemCount =
        atlas_.hasItems() ? render::buildItemEntities(*items_, itemViewYaw_, eyeBlockX,
                                                      eyeBlockY, eyeBlockZ, itemPartial_,
                                                      item::IconSheet::Items,
                                                      verts + terrainCount,
                                                      kMaxItemVertices - terrainCount,
                                                      &itemCutoff)
                          : 0;
    if (terrainCount + itemCount < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(terrainCount + itemCount));

    // Terrain first, because the atlas is already bound from the passes above
    // and the sheet swap is then paid at most once.
    const int counts[2] = {terrainCount, itemCount};
    const bool reboundItems = itemCount >= 4;
    int base = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int written = counts[pass];
        if (written < 4) {
            base += written;
            continue;
        }
        if (pass == 1) {
            atlas_.bindItems(0);
        }

        // **The base pointer is what separates the two draws**, so each still
        // indexes from zero through the shared quad index buffer.
        C3D_BufInfo bufInfo;
        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, verts + base, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        const int quads = written / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
        base += written;
    }

    // **Put the block atlas back.** Everything after this in the frame -- the
    // translucent terrain pass, and the next eye -- assumes unit 0 is the
    // atlas, and a texture left bound is the kind of fault that shows up as
    // water textured with swords.
    if (reboundItems) {
        atlas_.bind(0, wireframe_);
    }
}

// The same pass as the item entities and deliberately not folded into it: a
// falling block is always a terrain cube, so there is no sheet to choose, no
// spin to compute and no stack to draw more than once. Sharing the loop would
// have meant a branch in the middle of it for a case that has none of the same
// arithmetic.
void Renderer::drawFallingBlocks(const C3D_Mtx& viewProjection, i32 originChunkX,
                                 i32 originChunkZ)
{
    if (fallingBlocks_ == nullptr || fallingBlocks_->count() == 0
        || fallingVerts_ == nullptr) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(fallingVerts_);
    const int written = render::buildFallingBlocks(*fallingBlocks_, eyeBlockX, eyeBlockY,
                                                   eyeBlockZ, itemPartial_, verts,
                                                   kMaxFallingVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // Scaled back from render::kEntityUnitsPerBlock, as `drawMobs` is.
    const C3D_Mtx mvp = entityMatrix(viewProjection, tx, ty, tz);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, fallingVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);
}

// **Two passes, because `hw` is two draws.** The first is a falling block by
// another name -- one terrain cube per entity, through the detail pipeline --
// and it is not folded into `drawFallingBlocks` because the cube swells: the
// scale is a function of the fuse, and a shared loop would carry a branch for
// a case the falling blocks never take.
//
// The second is the white flash, and it cannot be a colour on the first. The
// detail pipeline modulates the atlas by the vertex colour and modulation only
// darkens, so a white TNT is not expressible there. It goes through the
// **outline pipeline** instead -- the texture-free, blended, uniform-tinted
// program the selection box already uses -- which is a faithful reading of
// what the jar does: `glDisable(GL_TEXTURE_2D)`, `glDisable(GL_LIGHTING)`,
// `glEnable(GL_BLEND)`, `glColor4f(1, 1, 1, alpha)`, draw the block again.
//
// **The tint is a uniform, so the flash is one draw per entity**, and that is
// what `kPrimedTntFlashBudget` bounds. It is also why the flash runs after the
// whole textured pass rather than interleaved: the pipeline is bound once.
//
// The one state the jar sets that this does not is the blend function.
// `hw` asks for `GL_SRC_ALPHA, GL_DST_ALPHA`, which on a framebuffer with no
// destination alpha is not what it reads as; the outline pass's
// `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` is the ordinary over-blend and is what the
// effect looks like on a real client.
void Renderer::drawPrimedTnt(const C3D_Mtx& viewProjection, i32 originChunkX,
                             i32 originChunkZ)
{
    if (primedTnt_ == nullptr || primedTnt_->count() == 0 || tntVerts_ == nullptr) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(tntVerts_);
    const int written = render::buildPrimedTnt(*primedTnt_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                               itemPartial_, verts, kMaxPrimedTntVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    // The same chunk-relative fold the falling blocks use: the eye's block is
    // folded into the matrix so the vertices stay inside a 16-bit range.
    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // The cube is in render::kEntityUnitsPerBlock and the flash in plain
    // blocks, so they take the same translation and different scales.
    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
    }
    const C3D_Mtx cubeMvp = entityMatrix(viewProjection, tx, ty, tz);

    bindPipeline(detailPipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &cubeMvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, tntVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    drawPrimedTntFlash(mvp);
}

// The white half of the pass above. Split out because everything in it is
// state the textured half must not inherit, and because the loop is one draw
// per entity rather than one for the lot.
void Renderer::drawPrimedTntFlash(const C3D_Mtx& mvp)
{
    if (tntFlashVerts_ == nullptr) {
        return;
    }

    // Count first: binding the outline pipeline and tearing the world state
    // down for a frame with nothing flashing would be a waste on every other
    // group of five ticks.
    int flashing = 0;
    for (int i = 0; i < primedTnt_->count() && flashing == 0; ++i) {
        if (render::primedTntFlashing((*primedTnt_)[i].fuse)) {
            ++flashing;
        }
    }
    if (flashing == 0) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    bindPipeline(outlinePipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, outlinePipeline_.uLocMvp, &mvp);

    // One combiner stage, the vertex colour straight through -- the same
    // configuration `drawSelection` sets, and for the same reason: there is no
    // texture on this shape.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }

    // **Depth test on, writes off, and `GEQUAL` rather than the world's
    // `GREATER`.** The comparison is the one thing here that is deliberately
    // not `applyWorldState`'s: this pass redraws the *same* vertices through
    // the same matrix, so its fragments land at exactly the depth the textured
    // pass just wrote, and a strict `GREATER` would reject every one of them --
    // the flash would never appear at all. `GEQUAL` lets the equal case
    // through, which is what makes this an overlay rather than a second
    // surface. Writes stay off so it does not leave one.
    //
    // This is the same problem `drawSelection` has and it is solved the other
    // way there, by expanding the box 0.002 -- a1.1.2's own number for the
    // outline. There is no such expansion in `hw`, so there is none here.
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
    C3D_CullFace(GPU_CULL_NONE);

    auto* verts = static_cast<render::OutlineVertex*>(tntFlashVerts_);
    int drawn = 0;
    for (int i = 0; i < primedTnt_->count() && drawn < render::kPrimedTntFlashBudget; ++i) {
        const mc::entity::PrimedTnt& e = (*primedTnt_)[i];
        const int written = render::buildPrimedTntFlash(
            e, eyeBlockX, eyeBlockY, eyeBlockZ, itemPartial_, verts,
            render::kPrimedTntFlashVerticesEach);
        if (written == 0) {
            continue;
        }
        GSPGPU_FlushDataCache(verts, sizeof(render::OutlineVertex) * u32(written));

        // `glColor4f(1.0F, 1.0F, 1.0F, f1)`.
        C3D_FVUnifSet(GPU_VERTEX_SHADER, outlinePipeline_.uLocTint, 1.0f, 1.0f, 1.0f,
                      render::primedTntFlashAlpha(e.fuse, itemPartial_));

        C3D_BufInfo* buf = C3D_GetBufInfo();
        BufInfo_Init(buf);
        BufInfo_Add(buf, verts, sizeof(render::OutlineVertex), 1, 0x0);
        C3D_DrawArrays(GPU_TRIANGLES, 0, written);
        ++frameStats_.drawCalls;
        ++drawn;
    }

    // **Put the world back, and put all of it back.** Unlike `drawSelection`,
    // this is not the last thing in the eye -- the paintings, the arrows, the
    // mobs, the signs and then the *translucent terrain pass* all follow it --
    // so anything left set here is inherited by the rest of the frame.
    //
    // **This is what tinted the ocean white.** An earlier version restored
    // three pieces of state by hand and left the texture combiner where the
    // outline pass had put it: one stage, primary colour, REPLACE. Every draw
    // after it in the eye then ran with no texture at all, and the biggest
    // surface in the frame is the sea -- which came out as a flat sheet of
    // vertex colour, with the fog amount the shader parks in primary alpha as
    // its opacity. The three hand-restored values were wrong as well
    // (`GPU_GEQUAL` for `GPU_GREATER`, an alpha reference of 0 for
    // `kAlphaTestRef`, and a src-alpha blend where the world wants `ONE/ZERO`).
    //
    // Restating the whole thing is both the correct fix and the cheaper one to
    // keep correct: `applyWorldState` is the single description of what a world
    // pass runs under, it already exists for exactly this reason, and a state
    // added to it in future is then added here too. It costs two texture binds
    // and a combiner setup, on the frames where TNT is actually flashing on
    // screen and only those.
    applyWorldState();
    // The one thing `applyWorldState` states that this point in the eye does
    // not want: it sets `GPU_CULL_BACK_CCW`, and the entity passes run with
    // culling off -- a crossed square is meant to be seen from both sides, and
    // the mesher emits both windings. `drawEye` turns it back on itself once
    // the entities are done.
    C3D_CullFace(GPU_CULL_NONE);
}

void Renderer::drawPaintings(const C3D_Mtx& viewProjection, i32 originChunkX,
                             i32 originChunkZ)
{
    if (paintings_ == nullptr || paintings_->count() == 0 || paintingVerts_ == nullptr
        || !atlas_.hasArt()) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(paintingVerts_);
    const int written = render::buildPaintings(*paintings_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                               verts, kMaxPaintingVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);

    // **The art sheet, which is nobody else's.** `art/kz.png` is its own
    // texture -- see core/texture/entity_skins.hpp -- so this pass costs one
    // bind, and the block atlas has to go back afterwards or the translucent
    // pass draws water out of a painting.
    atlas_.bindArt(0);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, paintingVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    atlas_.bind(0, wireframe_);
}

void Renderer::drawArrows(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ)
{
    if (arrows_ == nullptr || arrows_->count() == 0 || arrowVerts_ == nullptr
        || !atlas_.hasEntities()) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(arrowVerts_);
    const int written = render::buildArrows(*arrows_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                            itemPartial_, verts, kMaxArrowVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);

    // The shared entity sheet -- see core/texture/entity_skins.hpp. The arrow's
    // page is the one square page of the four.
    atlas_.bindEntity(0);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, arrowVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    atlas_.bind(0, wireframe_);
}

void Renderer::drawBoats(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ)
{
    if (boats_ == nullptr || boats_->count() == 0 || boatVerts_ == nullptr
        || !atlas_.hasEntities()) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(boatVerts_);
    const int written = render::buildBoats(*boats_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                           itemPartial_, verts, kMaxBoatVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);
    atlas_.bindEntity(0);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // Scaled back from render::kEntityUnitsPerBlock; see `entityMatrix`.
    const C3D_Mtx mvp = entityMatrix(viewProjection, tx, ty, tz);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, boatVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    atlas_.bind(0, wireframe_);
}

void Renderer::drawMobs(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ)
{
    const int liveMobs = mobs_ != nullptr ? mobs_->count() : 0;
    const int liveSpawners = spawners_ != nullptr ? spawners_->count() : 0;
    const int livePlayers = remotePlayers_ != nullptr ? remotePlayers_->playerCount() : 0;
    if ((liveMobs == 0 && liveSpawners == 0 && livePlayers == 0) || mobVerts_ == nullptr
        || !atlas_.hasEntities()) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(mobVerts_);
    int written = 0;
    if (liveMobs > 0) {
        written = render::buildMobs(*mobs_, eyeBlockX, eyeBlockY, eyeBlockZ, itemPartial_,
                                    verts, render::kMobMaxVertices);
    }
    // **The miniatures append to the same buffer**, so a spawner costs no bind
    // and no draw call of its own -- see the note on `kMaxMobVertices`. They
    // take the room the animals did not, never the room they need.
    if (liveSpawners > 0) {
        written += render::buildSpawnerMobs(*spawners_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                            itemPartial_, verts + written,
                                            kMaxMobVertices - written);
    }
    // ...and the other players on the same terms again, for the same reason.
    if (livePlayers > 0) {
        written += render::buildRemotePlayers(*remotePlayers_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                              itemPartial_, verts + written,
                                              kMaxMobVertices - written);
    }
    // **`gq`'s render pass 0, in its own buffer**: the slime's outer jelly,
    // which is the one thing in the game that is neither opaque nor cut out.
    // Built here so the pass below can draw it with blending after everything
    // solid has gone down -- see core/render/mob_mesh.hpp.
    int shellVertices = 0;
    if (liveMobs > 0 && mobShellVerts_ != nullptr) {
        shellVertices = render::buildMobShells(*mobs_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                               itemPartial_,
                                               static_cast<mesh::DetailVertex*>(mobShellVerts_),
                                               render::kMobShellMaxVertices);
    }

    // **`ok`'s render pass 0**: the spider's head from the eye page, in its own
    // buffer for the same reason the shell has one. See core/render/mob_mesh.hpp.
    int eyeVertices = 0;
    if (liveMobs > 0 && mobEyeVerts_ != nullptr) {
        eyeVertices = render::buildMobEyes(*mobs_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                           itemPartial_,
                                           static_cast<mesh::DetailVertex*>(mobEyeVerts_),
                                           render::kMobEyeMaxVertices);
    }

    if (written < 4 && shellVertices < 4 && eyeVertices < 4) {
        return;
    }
    if (written > 0) {
        GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));
    }
    if (shellVertices > 0) {
        GSPGPU_FlushDataCache(mobShellVerts_, sizeof(mesh::DetailVertex) * u32(shellVertices));
    }
    if (eyeVertices > 0) {
        GSPGPU_FlushDataCache(mobEyeVerts_, sizeof(mesh::DetailVertex) * u32(eyeVertices));
    }

    bindPipeline(detailPipeline_);
    atlas_.bindEntity(0);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // Scaled back from render::kEntityUnitsPerBlock; see `entityMatrix`.
    const C3D_Mtx mvp = entityMatrix(viewProjection, tx, ty, tz);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    if (written >= 4) {
        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, mobVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        const int quads = written / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
    }

    if (shellVertices >= 4) {
        // **Blended, and with depth writes off.** `gq` turns `GL_BLEND` on for
        // this pass and leaves the depth buffer alone, which is what lets the
        // eyes inside the jelly -- drawn a moment ago and nearer to nothing --
        // show through it, and what stops two slimes standing in front of one
        // another from hiding each other's faces. The depth *test* stays on so
        // the wall behind still occludes.
        //
        // The alpha test goes off with it: `mob/slime.png`'s shell is alpha 199
        // everywhere, and the world's 0.1 reference passes all of it at full
        // opacity, which was the whole bug.
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);

        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, mobShellVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        const int quads = shellVertices / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);

        // Put back what the entity passes after this one expect to find: the
        // alpha-tested, unblended, depth-writing state every other model is
        // drawn in. `drawEye` hands the next pass the state this one leaves.
        C3D_AlphaTest(true, GPU_GREATER, kAlphaTestRef);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    }

    if (eyeVertices >= 4) {
        // **`ok.a(ax, int)`, transcribed.** Out of the class file rather than
        // out of a description of it: `loadTexture("/mob/spider_eyes.png")`,
        // `f = (1 - getBrightness(1.0F)) * 0.5F`, `glEnable(GL_BLEND)`,
        // `glDisable(GL_ALPHA_TEST)`, `glBlendFunc(SRC_ALPHA,
        // ONE_MINUS_SRC_ALPHA)` and `glColor4f(1, 1, 1, f)`.
        //
        // **Depth writes stay on, unlike the shell's.** The jar touches the
        // depth mask in neither pass, and the shell turns it off here for a
        // reason of its own -- the slime's face is *inside* the jelly and has
        // to show through it. Nothing is inside a spider's head: the eye box is
        // the head grown by 0.01, so it lands a hair in front of geometry that
        // has already written depth and the two agree about everything behind
        // them.
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                       GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

        // **The alpha is the combiner's, and that is what makes it exact.**
        // `glColor4f(1, 1, 1, f)` is one value per entity; a draw call is one
        // value for every spider in it. Rather than split the draw, the term is
        // rebuilt per fragment out of the lightmap, which is monochrome
        // `lightBrightness(effectiveLightLevel(sky, block, subtracted))` -- the
        // same function `Entity.getBrightness` returns -- sampled at the light
        // coordinate the spider's own vertices already carry. So `1 -
        // lightmap.r` *is* `1 - getBrightness`, per spider, out of one draw.
        //
        //   stage 0: rgb = the eye page, replacing the model's vertex colour,
        //            because `glColor4f(1, 1, 1, f)` is white and overrides it
        //            -- a hurt spider's eyes do not go red;
        //            alpha = the page's own, which is what keeps everything but
        //            the eyes transparent now the alpha test is off;
        //   stage 1: alpha x= 1 - lightmap.r;
        //   stage 2: rgb = the fade to fog every other pass gets, so a distant
        //            spider's eyes recede with the rest of it;
        //            alpha x= 0.5, which rides in the same stage's constant as
        //            the fog colour, since one is rgb and the other is alpha.
        C3D_TexEnv* eye0 = C3D_GetTexEnv(0);
        C3D_TexEnvInit(eye0);
        C3D_TexEnvSrc(eye0, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
        C3D_TexEnvFunc(eye0, C3D_Both, GPU_REPLACE);

        // Both stages below are skipped in wireframe for the reason
        // `applyAtlasTexEnv` skips them there: a debug view that dims into a
        // cave and fades out at the render distance hides what it was turned on
        // to show. Without them the eyes draw at the page's own alpha, which is
        // what wireframe wants -- visible.
        C3D_TexEnv* eye1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(eye1);
        C3D_TexEnv* eye2 = C3D_GetTexEnv(2);
        C3D_TexEnvInit(eye2);
        if (!wireframe_) {
            C3D_TexEnvSrc(eye1, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(eye1, C3D_RGB, GPU_REPLACE);
            C3D_TexEnvSrc(eye1, C3D_Alpha, GPU_PREVIOUS, GPU_TEXTURE1, GPU_PREVIOUS);
            C3D_TexEnvOpAlpha(eye1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_ONE_MINUS_SRC_R,
                              GPU_TEVOP_A_SRC_ALPHA);
            C3D_TexEnvFunc(eye1, C3D_Alpha, GPU_MODULATE);

            C3D_TexEnvSrc(eye2, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(eye2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                            GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(eye2, C3D_RGB, GPU_INTERPOLATE);
            // The fog colour with `f`'s other half in its alpha. 128 rather
            // than 127 because 128/255 is 0.502 and the half it stands for is
            // 0.5; the four thousandths are below a texel of difference.
            C3D_TexEnvColor(eye2, (fogColour_ & 0x00FFFFFFu) | 0x80000000u);
            C3D_TexEnvSrc(eye2, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvFunc(eye2, C3D_Alpha, GPU_MODULATE);
        }

        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, mobEyeVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        const int quads = eyeVertices / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);

        // Everything after this reads what this pass leaves, and it borrowed
        // all three stages.
        C3D_AlphaTest(true, GPU_GREATER, kAlphaTestRef);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        applyAtlasTexEnv();
    }

    atlas_.bind(0, wireframe_);
}

// **The flames on everything that is burning** -- `ak.a(Lkh;DDDF)V`, which the
// original runs from `doRenderShadowAndFire` immediately after each model it
// belongs to. Here it is one pass at the end of the entity passes, for two
// reasons: the sheets are tiles of the *block* atlas where most of those models
// are the entity sheet, so they could never have shared a draw; and every
// burning entity in the world draws from the same tile, so all of them together
// are one draw call. `drawMobs` put the block atlas back on its way out, so
// this costs no bind either. See core/render/entity_fire_mesh.hpp.
void Renderer::drawEntityFire(const C3D_Mtx& viewProjection, i32 originChunkX,
                           i32 originChunkZ)
{
    if (entityFireVerts_ == nullptr) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    render::FireScene scene;
    scene.mobs = mobs_;
    scene.items = items_;
    scene.boats = boats_;
    scene.minecarts = minecarts_;
    scene.fallingBlocks = fallingBlocks_;
    scene.primedTnt = primedTnt_;

    auto* verts = static_cast<mesh::DetailVertex*>(entityFireVerts_);
    const int written =
        render::buildEntityFires(scene, itemViewYaw_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                 itemPartial_, verts, kMaxEntityFireVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // The flames are in render::kEntityUnitsPerBlock too.
    const C3D_Mtx mvp = entityMatrix(viewProjection, tx, ty, tz);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, entityFireVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);
}

void Renderer::drawMinecarts(const C3D_Mtx& viewProjection, i32 originChunkX,
                             i32 originChunkZ)
{
    if (minecarts_ == nullptr || minecartWorld_ == nullptr || minecarts_->count() == 0
        || minecartVerts_ == nullptr || !atlas_.hasEntities()) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(minecartVerts_);
    const int written =
        render::buildMinecarts(*minecarts_, *minecartWorld_, eyeBlockX, eyeBlockY,
                               eyeBlockZ, itemPartial_, verts, kMaxMinecartVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);
    atlas_.bindEntity(0);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // Scaled back from render::kEntityUnitsPerBlock; see `entityMatrix`.
    const C3D_Mtx mvp = entityMatrix(viewProjection, tx, ty, tz);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, minecartVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    atlas_.bind(0, wireframe_);

    // **The block a chest or furnace cart carries, and it is a second draw
    // because it is a second sheet**: `kt.a` binds `/terrain.png` for it and
    // `/item/cart.png` for the cart. The block atlas is already back on by the
    // line above, which is why this runs after rather than before.
    drawMinecartBlocks(viewProjection, originChunkX, originChunkZ);
}

void Renderer::drawMinecartBlocks(const C3D_Mtx& viewProjection, i32 originChunkX,
                                  i32 originChunkZ)
{
    if (minecarts_ == nullptr || minecartWorld_ == nullptr || minecarts_->count() == 0
        || minecartBlockVerts_ == nullptr) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(minecartBlockVerts_);
    const int written =
        render::buildMinecartBlocks(*minecarts_, *minecartWorld_, eyeBlockX, eyeBlockY,
                                    eyeBlockZ, itemPartial_, verts, kMaxMinecartBlockVertices);
    if (written < 4) {
        return;  // no chest or furnace cart in range, which is the usual answer
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);

    // Scaled back from render::kEntityUnitsPerBlock; see `entityMatrix`.
    const C3D_Mtx mvp = entityMatrix(viewProjection, tx, ty, tz);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, minecartBlockVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);
}

void Renderer::drawSigns(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ)
{
    // **The names over other players' heads ride this pass**, because it is the
    // one that already draws text in the world with the font bound -- so a
    // world with no signs in it still comes here when there is somebody to
    // name. See core/render/remote_player_mesh.hpp.
    const int liveSigns = signs_ != nullptr ? signs_->count() : 0;
    const int namedPlayers = remotePlayers_ != nullptr ? remotePlayers_->playerCount() : 0;
    if ((liveSigns == 0 && namedPlayers == 0) || signVerts_ == nullptr) {
        return;
    }

    const double eyeBlockX = std::floor(itemEyeX_);
    const double eyeBlockY = std::floor(itemEyeY_);
    const double eyeBlockZ = std::floor(itemEyeZ_);

    auto* verts = static_cast<mesh::DetailVertex*>(signVerts_);

    // **Both spans are built before either is drawn.** The boards go into the
    // front of the buffer and the text after them, and only then does anything
    // draw -- because `C3D_DrawElements` records a command naming an *address*
    // and the GPU does not execute it until `C3D_FrameEnd`. Building one,
    // drawing it, and then building the other over the top is exactly the bug
    // that made dropped items invisible; see drawItemEntities.
    render::DrawCutoff signCutoff;
    const int boardCount =
        atlas_.hasEntities() && liveSigns > 0
            ? render::buildSignBoards(*signs_, eyeBlockX, eyeBlockY, eyeBlockZ, verts,
                                      kMaxSignVertices, &signCutoff)
            : 0;
    const bool haveFont = atlas_.hasFont() && signFont_ != nullptr && !signFont_->empty();
    int textCount =
        haveFont && liveSigns > 0
            ? render::buildSignText(*signs_, *signFont_, eyeBlockX, eyeBlockY, eyeBlockZ,
                                    verts + boardCount, kMaxSignVertices - boardCount,
                                    &signCutoff)
            : 0;
    if (haveFont && namedPlayers > 0) {
        textCount += render::buildNameTags(*remotePlayers_, *signFont_, particleCamera_,
                                           eyeBlockX, eyeBlockY, eyeBlockZ, itemPartial_,
                                           verts + boardCount + textCount,
                                           kMaxSignVertices - boardCount - textCount);
    }
    if (boardCount + textCount < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(boardCount + textCount));

    bindPipeline(detailPipeline_);

    const float tx = float(eyeBlockX - double(originChunkX) * 16.0);
    const float ty = float(eyeBlockY);
    const float tz = float(eyeBlockZ - double(originChunkZ) * 16.0);
    // The same translation every entity pass makes, and then **a scale**: the
    // sign builders write 1/512 of a block where the shader divides by 1024, so
    // that a sign reaches the 64 blocks a1.1.2 draws one at. See
    // render::kSignUnitsPerBlock. Translation first, from the unscaled columns,
    // because the scale belongs inside it.
    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
        mvp.r[row].c[1] = r[1] * render::kSignUnitScale;
        mvp.r[row].c[2] = r[2] * render::kSignUnitScale;
        mvp.r[row].c[3] = r[3] * render::kSignUnitScale;
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    // Boards off the entity sheet, text off the font -- two textures, so two
    // draws with two base pointers into one buffer.
    const int counts[2] = {boardCount, textCount};
    int base = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int written = counts[pass];
        if (written < 4) {
            base += written;
            continue;
        }
        if (pass == 0) {
            atlas_.bindEntity(0);
        } else {
            atlas_.bindFont(0);
        }

        C3D_BufInfo bufInfo;
        BufInfo_Init(&bufInfo);
        BufInfo_Add(&bufInfo, verts + base, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&bufInfo);

        const int quads = written / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
        base += written;
    }

    atlas_.bind(0, wireframe_);
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
    // `glColor4f(0.0F, 0.0F, 0.0F, 0.4F)` -- `RenderGlobal.drawSelectionBox`,
    // unchanged. The crosshair pass below shares this program and sets its own.
    C3D_FVUnifSet(GPU_VERTEX_SHADER, outlinePipeline_.uLocTint, 0.0f, 0.0f, 0.0f, 0.4f);

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

// The world view needs its own crosshair; the one on the bottom-screen look
// pad is only a touch affordance.  Reuse the colour-only outline pipeline so
// this stays a tiny, texture-free draw and remains visible over water, leaves
// and every other world surface.
void Renderer::drawCrosshair(const C3D_Mtx& viewProjection, const Camera& camera,
                             i32 originChunkX, i32 originChunkZ)
{
    if (crosshairVerts_ == nullptr) {
        return;
    }

    float fx, fy, fz;
    camera.look(&fx, &fy, &fz);
    const float rx = std::cos(camera.yaw);
    const float rz = std::sin(camera.yaw);
    // forward cross right is the camera's screen-up vector.
    const float ux = fy * rz;
    const float uy = fz * rx - fx * rz;
    const float uz = -fy * rx;
    constexpr float kDistance = 2.0f;
    constexpr float kArm = 0.11f;
    constexpr float kHalfWidth = 0.008f;
    const float cx = float(camera.x - double(originChunkX) * 16.0) + fx * kDistance;
    const float cy = float(camera.y) + fy * kDistance;
    const float cz = float(camera.z - double(originChunkZ) * 16.0) + fz * kDistance;

    auto* verts = static_cast<render::OutlineVertex*>(crosshairVerts_);
    int written = 0;
    const auto point = [=](float right, float up) {
        return render::OutlineVertex{cx + rx * right + ux * up, cy + uy * up,
                                     cz + rz * right + uz * up};
    };
    const auto quad = [&verts, &written](render::OutlineVertex a, render::OutlineVertex b,
                                         render::OutlineVertex c, render::OutlineVertex d) {
        verts[written++] = a; verts[written++] = b; verts[written++] = c;
        verts[written++] = a; verts[written++] = c; verts[written++] = d;
    };
    quad(point(-kArm, -kHalfWidth), point(kArm, -kHalfWidth),
         point(kArm, kHalfWidth), point(-kArm, kHalfWidth));
    quad(point(-kHalfWidth, -kArm), point(kHalfWidth, -kArm),
         point(kHalfWidth, kArm), point(-kHalfWidth, kArm));
    GSPGPU_FlushDataCache(verts, sizeof(render::OutlineVertex) * u32(written));

    bindPipeline(outlinePipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, outlinePipeline_.uLocMvp, &viewProjection);
    // **Set here and not inherited.** The selection pass shares this program and
    // leaves the original's black at four tenths in the uniform -- and on a
    // frame where the crosshair is on nothing at all, that pass returns before
    // it writes anything, so there is no colour to inherit either. Opaque
    // white: this mark has to read against a night sky and a cave wall alike.
    C3D_FVUnifSet(GPU_VERTEX_SHADER, outlinePipeline_.uLocTint, 1.0f, 1.0f, 1.0f, 1.0f);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    // **Culling off, and this is what made the crosshair invisible.**
    //
    // The translucent pass above turns culling back on, and the convention the
    // whole renderer is wound for is that counter-clockwise in screen space is
    // the front: `kFaceCorner[kFaceNegZ]` is (1,0,0), (0,0,0), (0,1,0) and
    // through Mtx_LookAt's basis -- s = forward x up, so at yaw 0 screen right
    // is world -X -- those three come out counter-clockwise, which is why the
    // world is visible at all under GPU_CULL_BACK_CCW.
    //
    // `rx, rz` above is the *other* horizontal perpendicular: it is screen
    // **left**, not screen right. Everything the two quads are built from is
    // symmetric about both axes, so the mark looks identical either way -- but
    // the triangles come out clockwise, which is back-facing, and the whole
    // pass was being thrown away by the cull. A mark that always faces the
    // camera has no back to cull, so this states none rather than depending on
    // which of the two perpendiculars the basis picked.
    //
    // Safe to leave set: applyWorldState restates the cull mode before every
    // eye rather than inheriting it.
    C3D_CullFace(GPU_CULL_NONE);

    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, verts, sizeof(render::OutlineVertex), 1, 0x0);
    C3D_DrawArrays(GPU_TRIANGLES, 0, written);

}

// **The item in the player's hand, and the only geometry on the top screen
// that is not the world.** `jh.a(F)V` -- `ItemRenderer.renderItemInFirstPerson`
// -- with core/render/held_item.cpp holding all of the arithmetic and this
// holding the three things about it that are the PICA's business.
//
// **One: no view matrix.** The original's `renderHand` calls `glLoadIdentity()`
// on the modelview and draws in camera space, so `buildHeldItem` returns
// camera-space blocks and the uniform here is the *projection alone*. That is
// also why this takes `iod` rather than the eye's view-projection -- the
// stereo separation still applies, the camera's position and heading do not.
//
// **Two: its own near plane**, kHeldItemNearPlane -- equal to the world's since
// that came down to a1.1.2's 0.05, and still stated separately because the two
// are pinned to different things. See the constant.
//
// **Three: its own slice of the depth buffer**, which is this port's answer to
// the `glClear(GL_DEPTH_BUFFER_BIT)` the original does first. Both constants
// carry the argument.
void Renderer::drawHeldItem(float iod)
{
    if (!heldVisible_ || heldVerts_ == nullptr) {
        return;
    }

    auto* verts = static_cast<mesh::DetailVertex*>(heldVerts_);
    // Named `built` rather than `mesh`, because `mesh::DetailVertex` is three
    // lines below it and a local of that name reads as a shadow even though
    // qualified lookup ignores it.
    const render::HeldItemMesh built =
        render::buildHeldItem(heldItem_, heldEquipped_, heldSwing_, 400.0f / 240.0f,
                              heldLight_, verts, kMaxHeldVertices);
    if (built.vertices < 4) {
        return;
    }
    // **Which of the three sheets this one draw is bound to.** A pack with no
    // gui/items.png gets the same answer drawItemEntities gives -- nothing,
    // because there is no fallback here that would not be a lie about what the
    // player is holding. The player's skin cannot be missing in the same way:
    // `buildEntitySkins` lays down its stand-in before it reads a pack, so an
    // empty hand with no `char.png` is a black arm rather than no arm at all.
    const bool fromItems = built.sheet == render::HeldSheet::Items;
    const bool fromSkin = built.sheet == render::HeldSheet::PlayerSkin;
    if ((fromItems && !atlas_.hasItems()) || (fromSkin && !atlas_.hasEntities())) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(built.vertices));

    C3D_Mtx projection;
    Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(config_.fovDegrees), 400.0f / 240.0f,
                        kHeldItemNearPlane, farPlane(), iod * kHeldItemStereoScale,
                        kHeldItemFocalBlocks, false);

    // **The world's fragment state, restated -- and this is what made the hand
    // invisible.**
    //
    // `drawSelection` and `drawCrosshair` both say they restore nothing "on
    // purpose", because `applyWorldState` runs before every eye and states it
    // all rather than inheriting it. That argument holds for as long as they
    // are the *last* thing in the eye, and this pass is now after them. What
    // they leave behind is a one-stage combiner that replaces both colour and
    // alpha with the vertex's own, the alpha test switched off, and
    // src-alpha/one-minus-src-alpha blending -- and the detail shader puts the
    // **fog amount** in the vertex alpha, which for something 0.8 of a block
    // from the eye is zero. So the hand was drawn, correctly, at an alpha of
    // nothing.
    //
    // Calling the one function that states all of it is the fix, rather than
    // undoing the crosshair's three settings here: a fourth thing either of
    // those passes changes later would be the same bug again.
    applyWorldState();

    bindPipeline(detailPipeline_);
    clearFogParam(detailPipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &projection);

    if (fromItems) {
        atlas_.bindItems(0);
    } else if (fromSkin) {
        atlas_.bindEntity(0);
    }

    // **Culling off and the depth test on**, which is the arrangement the
    // detail passes already run under: the sprite's two faces and its sixty-four
    // edge strips are wound the class file's way rather than this renderer's,
    // and depth is what sorts them instead. Both after `applyWorldState`, which
    // states the opposite of each.
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_DepthMap(true, kHeldItemDepthScale, kHeldItemDepthOffset);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, verts, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = built.vertices / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    // **Restored here and not left to applyWorldState**, unlike the cull mode
    // and the depth test above: nothing else in this renderer touches the depth
    // map, so there is no other statement of it to fall back on. Leaving it
    // compressed would put the *whole world* in the top 5 % of the buffer on
    // the next eye, which is a depth buffer with 3,000 usable levels in it.
    C3D_DepthMap(true, kDepthMapScale, kDepthMapOffset);

    // And the block atlas goes back, for the reason drawItemEntities gives.
    if (fromItems || fromSkin) {
        atlas_.bind(0, wireframe_);
    }
}

// **The flames over a burning player's view** -- `jh.d(F)V`, which
// `renderOverlays` runs straight after the hand with the modelview still at
// identity. So this is the hand's pass again with three differences, and
// core/render/fire_overlay.cpp holds the geometry.
//
// **One: the same projection and the same slice of depth.** The original draws
// it after the depth clear the hand gets, with the depth test still on, so the
// part of a held item nearer than a sheet covers it and the rest is behind the
// flames. Keeping the hand's `C3D_DepthMap` does exactly that, and writing no
// depth keeps it from mattering to anything after.
//
// **Two: blended, at 0.9.** `glColor4f(1, 1, 1, 0.9F)` with src-alpha blending.
// The vertex alpha is the fog amount here, so the 0.9 goes into the last
// combiner stage's alpha as a constant instead, and `applyAtlasTexEnv` takes it
// back out afterwards.
//
// **Three: nothing to build.** The eight vertices were written at init; the
// flames move because `FlameAnimation` rewrites the two tiles under them.
void Renderer::drawFireOverlay(float iod)
{
    if (!burning_ || fireOverlayVerts_ == nullptr || fireOverlayCount_ < 4) {
        return;
    }

    C3D_Mtx projection;
    Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(config_.fovDegrees), 400.0f / 240.0f,
                        kHeldItemNearPlane, farPlane(), iod * kHeldItemStereoScale,
                        kHeldItemFocalBlocks, false);

    // Everything the hand's pass states, for the reason it gives: whatever ran
    // last in this eye left its own settings behind.
    applyWorldState();

    bindPipeline(detailPipeline_);
    clearFogParam(detailPipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &projection);

    // The texture's alpha times 0.9, on the stage that already carries the fog
    // colour as its constant -- so the colour stays and only its alpha byte is
    // borrowed. In wireframe the stage is a pass-through and this is all it does.
    const u32 alpha = u32(render::kFireOverlayAlpha * 255.0f + 0.5f);
    C3D_TexEnv* env2 = C3D_GetTexEnv(2);
    C3D_TexEnvSrc(env2, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvFunc(env2, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvColor(env2, (fogColour_ & 0x00FFFFFFu) | (alpha << 24));

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
    C3D_DepthMap(true, kHeldItemDepthScale, kHeldItemDepthOffset);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, fireOverlayVerts_, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    const int quads = fireOverlayCount_ / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    // Put back what the world's passes assume, as `drawHeldItem` does with the
    // depth map.
    C3D_DepthMap(true, kDepthMapScale, kDepthMapOffset);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    applyAtlasTexEnv();
}

// **The water over a submerged player's view** -- `jh.c(F)V`, run by
// `renderOverlays` after the flames. The fire overlay's pass with three
// differences, and core/render/water_overlay.cpp holds the geometry.
//
// **Its own sheet.** `water.png`, uploaded 4 x 4 and set to wrap, since the
// UVs scroll with the camera past the sheet's edge.
//
// **Lit.** `glColor4f(b, b, b, 0.5F)` with b the player's brightness, which
// here is the vertex's light byte through the world's lightmap -- so the water
// over the view darkens at night and in a deep sea as the world behind it does.
//
// **Rebuilt per eye**, as the hand is, because it follows the camera. Both
// eyes build the same four vertices.
void Renderer::drawWaterOverlay(float iod)
{
    if (!underwater_ || waterOverlayVerts_ == nullptr || !atlas_.hasWaterOverlay()) {
        return;
    }

    auto* verts = static_cast<mesh::DetailVertex*>(waterOverlayVerts_);
    const int count = render::buildWaterOverlayQuad(waterYaw_, waterPitch_, waterLight_, verts,
                                                    render::kWaterOverlayVertices);
    if (count < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(count));

    C3D_Mtx projection;
    Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(config_.fovDegrees), 400.0f / 240.0f,
                        kHeldItemNearPlane, farPlane(), iod * kHeldItemStereoScale,
                        kHeldItemFocalBlocks, false);

    // Everything the hand's pass states, for the reason it gives.
    applyWorldState();

    bindPipeline(detailPipeline_);
    clearFogParam(detailPipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &projection);
    atlas_.bindWaterOverlay(0);

    // The texture's alpha times 0.5, on the fog stage's constant, as the
    // flames borrow it for their 0.9.
    const u32 alpha = u32(render::kWaterOverlayAlpha * 255.0f + 0.5f);
    C3D_TexEnv* env2 = C3D_GetTexEnv(2);
    C3D_TexEnvSrc(env2, C3D_Alpha, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvFunc(env2, C3D_Alpha, GPU_MODULATE);
    C3D_TexEnvColor(env2, (fogColour_ & 0x00FFFFFFu) | (alpha << 24));

    // **No depth test at all**: the original clears depth before the hand and
    // this sheet is in front of everything, the hand and the flames included.
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, verts, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);

    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    ++frameStats_.quads;

    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    applyAtlasTexEnv();
    atlas_.bind(0, wireframe_);
}

// **Once a frame, before the first eye.** Both eyes draw the same lines at the
// same place, and a buffer rewritten between them would be rewritten under a
// draw the GPU has not run yet -- harmless only while the two builds agree.
// Building once makes that a fact rather than a coincidence.
void Renderer::buildChat()
{
    chatSpanCount_ = 0;
    if (chat_ == nullptr || chat_->count() == 0 || chatFont_ == nullptr
        || chatFont_->empty() || !atlas_.hasFont() || chatVerts_ == nullptr
        || chatStrips_ == nullptr) {
        return;
    }
    auto* verts = static_cast<mesh::DetailVertex*>(chatVerts_);
    chatSpanCount_ = render::buildChatText(*chat_, *chatFont_, 240, verts,
                                           render::kChatMaxVertices, chatSpans_,
                                           mc::gui::kChatShownLines);
    if (chatSpanCount_ == 0) {
        return;
    }

    // `drawRect(2, y - 1, 322, y + 8, ...)`: two triangles a line, in screen
    // pixels, which the strip pass's matrix takes as they are.
    auto* strips = static_cast<render::OutlineVertex*>(chatStrips_);
    int glyphVertices = 0;
    for (int i = 0; i < chatSpanCount_; ++i) {
        const float x0 = float(mc::gui::kChatLeft);
        const float x1 = float(mc::gui::kChatLeft + mc::gui::kChatStripWidth);
        const float y0 = float(chatSpans_[i].y - 1);
        const float y1 = float(chatSpans_[i].y + 8);
        render::OutlineVertex* s = strips + i * 6;
        s[0] = {x0, y0, 0.0f};
        s[1] = {x1, y0, 0.0f};
        s[2] = {x1, y1, 0.0f};
        s[3] = {x0, y0, 0.0f};
        s[4] = {x1, y1, 0.0f};
        s[5] = {x0, y1, 0.0f};
        const int end = chatSpans_[i].firstVertex + chatSpans_[i].vertices;
        glyphVertices = end > glyphVertices ? end : glyphVertices;
    }
    GSPGPU_FlushDataCache(strips, sizeof(render::OutlineVertex) * 6u * u32(chatSpanCount_));
    if (glyphVertices > 0) {
        GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(glyphVertices));
    }
}

// **Last in the eye, flat, and at the screen plane.** An orthographic matrix
// over the top screen's 400 x 240, the same `Mtx_OrthoTilt` citro2d builds for
// the menu, and no interocular offset at all: the text sits on the glass in
// both eyes, which is where a HUD belongs whatever the slider says.
//
// Two passes, strips then text, rather than alternating per line: the strips
// of neighbouring lines meet but never overlap, so the order between lines
// cannot be seen, and alternating would rebind the pipeline twenty times.
// **Each line is its own draw in both passes**, because each fades on its own
// and neither program can take a per-vertex alpha -- the outline program's
// colour is a uniform, and the detail program spends vertex alpha on fog. So
// the strip's alpha is the tint and the text's is the combiner's constant.
//
// Blended, with no alpha test and no depth test: `GuiIngame` disables
// GL_ALPHA_TEST for the chat and draws it over everything. Nothing here is
// restored -- `applyWorldState` restates all of it before the next eye.
void Renderer::drawChat()
{
    if (chatSpanCount_ == 0) {
        return;
    }

    C3D_Mtx screen;
    Mtx_OrthoTilt(&screen, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    // The strips: the outline program, its colour straight through.
    bindPipeline(outlinePipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, outlinePipeline_.uLocMvp, &screen);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_BufInfo* strips = C3D_GetBufInfo();
    BufInfo_Init(strips);
    BufInfo_Add(strips, chatStrips_, sizeof(render::OutlineVertex), 1, 0x0);
    for (int i = 0; i < chatSpanCount_; ++i) {
        // `(alpha / 2) << 24` over black.
        const float alpha = float(chatSpans_[i].alpha / 2) / 255.0f;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, outlinePipeline_.uLocTint, 0.0f, 0.0f, 0.0f, alpha);
        C3D_DrawArrays(GPU_TRIANGLES, i * 6, 6);
        ++frameStats_.drawCalls;
    }

    // The text: the detail program off the font, glyph colour times vertex
    // colour, glyph alpha times the line's.
    bindPipeline(detailPipeline_);
    clearFogParam(detailPipeline_);
    C3D_Mtx glyphs = screen;
    // The detail shader divides positions by 1024 and the builder wrote
    // sixteen units a pixel, so a vertex arrives as pixels / 64.
    Mtx_Scale(&glyphs, float(mesh::kDetailUnitsPerBlock / render::kChatUnitsPerPixel),
              float(mesh::kDetailUnitsPerBlock / render::kChatUnitsPerPixel), 1.0f);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &glyphs);
    atlas_.bindFont(0);
    env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
    C3D_BufInfo text;
    auto* base = static_cast<mesh::DetailVertex*>(chatVerts_);
    for (int i = 0; i < chatSpanCount_; ++i) {
        const render::ChatSpan& span = chatSpans_[i];
        if (span.vertices < 4) {
            continue;
        }
        // **Fetched again for every line.** `C3D_TexEnvColor` only writes a
        // field; it is `C3D_GetTexEnv` that marks the stage dirty, and a stage
        // not marked is not re-sent -- so reusing the pointer from above would
        // draw every line at the first line's alpha.
        C3D_TexEnvColor(C3D_GetTexEnv(0), (u32(span.alpha) << 24) | 0x00FFFFFFu);
        // The shared index buffer counts from the buffer's base, so each line
        // gets its own base rather than an offset into the indices.
        BufInfo_Init(&text);
        BufInfo_Add(&text, base + span.firstVertex, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&text);
        const int quads = span.vertices / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
    }

    atlas_.bind(0, wireframe_);
}

// **The focus hint.** Two triangles and one more, at the screen plane in both
// eyes, on the same terms as the chat: an orthographic matrix over the top
// screen's 400 x 240 and no interocular offset, because a thing that is on the
// glass has no depth to have.
//
// The outline program, whose colour is a uniform -- which is exactly what this
// wants, since the band and the arrowhead are two flat colours and nothing
// here is textured. Blended, no alpha test, no depth test, last of everything.
// Nothing is restored: `applyWorldState` restates all of it before the next
// eye, the same contract `drawChat` works under.
void Renderer::drawFocusHint()
{
    if (!focusHint_ || focusHintVerts_ == nullptr) {
        return;
    }

    C3D_Mtx screen;
    Mtx_OrthoTilt(&screen, 0.0f, float(kTopScreenWidth), float(kTopScreenHeight), 0.0f, 1.0f,
                  -1.0f, true);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);

    bindPipeline(outlinePipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, outlinePipeline_.uLocMvp, &screen);
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }

    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, focusHintVerts_, sizeof(render::OutlineVertex), 1, 0x0);

    C3D_FVUnifSet(GPU_VERTEX_SHADER, outlinePipeline_.uLocTint, kFocusHintGrey, kFocusHintGrey,
                  kFocusHintGrey, kFocusHintAlpha);
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, outlinePipeline_.uLocTint, kFocusArrowGrey,
                  kFocusArrowGrey, kFocusArrowGrey, kFocusArrowAlpha);
    C3D_DrawArrays(GPU_TRIANGLES, 6, 3);
    frameStats_.drawCalls += 2;
}

void Renderer::drawBreakOverlay(const C3D_Mtx& viewProjection, i32 originChunkX,
                                i32 originChunkZ)
{
    if (!breakVisible_ || breakVerts_ == nullptr) {
        return;
    }
    auto* verts = static_cast<mesh::DetailVertex*>(breakVerts_);
    const int written = render::buildBreakOverlay(breakBlock_, breakMeta_, breakStage_, verts,
                                                  render::kBreakOverlayMaxVertices);
    if (written < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(written));

    bindPipeline(detailPipeline_);

    // Block-local vertices, moved to the block -- the falling blocks' own
    // translation, done in the matrix so the positions stay small.
    const float tx = float(double(breakX_) - double(originChunkX) * 16.0);
    const float ty = float(breakY_);
    const float tz = float(double(breakZ_) - double(originChunkZ) * 16.0);
    C3D_Mtx mvp = viewProjection;
    for (int row = 0; row < 4; ++row) {
        const float* r = viewProjection.r[row].c;
        mvp.r[row].c[0] = r[3] * tx + r[2] * ty + r[1] * tz + r[0];
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &mvp);

    // terrain.png, never the wireframe atlas: the crack tiles are the texture.
    atlas_.bind(0, false);

    // **`glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)` with the alpha test off**,
    // which is twice the product of the crack and what is already there: a
    // mid-grey texel leaves the block as it was and the dark lines darken it.
    // Unlit, for the reason the original draws it in plain white -- the block
    // under it is already lit.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_SRC_COLOR, GPU_DST_ALPHA,
                   GPU_SRC_ALPHA);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);

    C3D_BufInfo bufInfo;
    BufInfo_Init(&bufInfo);
    BufInfo_Add(&bufInfo, verts, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&bufInfo);
    const int quads = written / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    atlas_.bind(0, wireframe_);
}

void Renderer::drawHud()
{
    if (!hudVisible_ || hudVerts_ == nullptr || !atlas_.hasIcons()) {
        return;
    }
    auto* verts = static_cast<mesh::DetailVertex*>(hudVerts_);
    const int count = render::buildHud(hudInput_, verts, render::kHudMaxVertices);
    if (count < 4) {
        return;
    }
    GSPGPU_FlushDataCache(verts, sizeof(mesh::DetailVertex) * u32(count));

    // The chat's space: screen pixels at the screen plane, no stereo offset,
    // with the detail shader's 1/1024 undone by the builder's sixteen units a
    // pixel.
    C3D_Mtx screen;
    Mtx_OrthoTilt(&screen, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true);
    Mtx_Scale(&screen, float(mesh::kDetailUnitsPerBlock / render::kHudUnitsPerPixel),
              float(mesh::kDetailUnitsPerBlock / render::kHudUnitsPerPixel), 1.0f);

    // **Cut out rather than blended.** Every texel `lu` draws is either solid or
    // clear, so the world's alpha test is the right answer and blending would
    // only cost fill on a fill-bound device.
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(true, GPU_GREATER, kAlphaTestRef);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    bindPipeline(detailPipeline_);
    clearFogParam(detailPipeline_);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &screen);
    atlas_.bindIcons(0);

    // **Unlit, and the alpha straight off the texture.** Stage 0 alone: the
    // lightmap and the fog the world's stages 1 and 2 apply would darken the
    // hearts at night and fade them with distance from a camera they are not
    // in. The vertex alpha is the shader's fog amount and must not reach the
    // test either, which is why alpha is the texture's and nothing else.
    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
    C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
    for (int i = 1; i < 3; ++i) {
        C3D_TexEnvInit(C3D_GetTexEnv(i));
    }

    C3D_BufInfo buf;
    BufInfo_Init(&buf);
    BufInfo_Add(&buf, verts, sizeof(mesh::DetailVertex), 3, 0x210);
    C3D_SetBufInfo(&buf);
    const int quads = count / 4;
    C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
    ++frameStats_.drawCalls;
    frameStats_.quads += usize(quads);

    atlas_.bind(0, wireframe_);
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
    pipeline->uLocSeam = shaderInstanceGetUniformLocation(pipeline->program.vertexShader, "seam");
    if (pipeline->uLocMvp < 0 || pipeline->uLocFog < 0 || uLocFaceBasis_ < 0
        || pipeline->uLocSeam < 0) {
        return false;
    }

    AttrInfo_Init(&pipeline->attrs);
    AttrInfo_AddLoader(&pipeline->attrs, 0, GPU_UNSIGNED_BYTE, 4);  // x,y,z,face         offset 0
    AttrInfo_AddLoader(&pipeline->attrs, 1, GPU_UNSIGNED_BYTE, 4);  // slotX,slotY,l,ext  offset 4
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

    // **The seam**, for whichever cube program this is. Growth of
    // `seam.x * w + seam.y` blocks at view distance w, and w is exactly the
    // distance at which a block is `focal / w` pixels across -- so seam.x is a
    // constant kSeamPixels on screen at any distance. The focal length comes
    // from the same fov viewProjection hands Mtx_PerspStereoTilt, whose scale on
    // both screen axes works out to 120 / tan(fov/2) pixels.
    //
    // **An eighth of a pixel.** The slivers it closes are the gap between a
    // corner snapped to the rasteriser's 1/16-pixel grid and the edge it is
    // meant to lie on, which is at most ~0.044 of a pixel; an eighth covers
    // that with room for a face seen at a slant. What it costs is the texture
    // of a merged quad sitting an eighth of a pixel out of line with its
    // neighbour's, which is not a thing a 3DS screen can show. The floor is a
    // thousandth of a block, for the far corners of a large quad close up,
    // whose own w is larger than the corner-0 w the geometry path sizes by.
    if (pipeline.uLocSeam >= 0) {
        constexpr float kSeamPixels = 0.125f;
        constexpr float kSeamFloorBlocks = 1.0f / 1024.0f;
        const float focalPixels =
            120.0f / std::tan(C3D_AngleFromDegrees(config_.fovDegrees) * 0.5f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, pipeline.uLocSeam, kSeamPixels / focalPixels,
                      kSeamFloorBlocks, 0.0f, 0.0f);
    }
    if (pipeline.uLocSeamDir >= 0) {
        for (int seam = 0; seam < mesh::kSeamTableSize; ++seam) {
            const mesh::SeamDirection d = mesh::seamDirection(seam);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, pipeline.uLocSeamDir + seam, float(d.x), float(d.y),
                          float(d.z), 0.0f);
        }
    }

    // The fog line goes in here rather than once a frame, because C3D_FVUnifSet
    // addresses a *hardware* constant register and the two shaders are
    // assembled separately: picasso is free to put `fogparam` in a register the
    // other program uses for one of its .constf values, and binding that
    // program reloads its constant table over the top. Six writes a frame
    // sidesteps the whole question.
    if (pipeline.uLocFog >= 0) {
        setFogParam(pipeline.uLocFog, fogStartBlocks(), fogEndBlocks());
    }
}

// **`iq.a(I)V`'s two modes, as one uniform.** `GL_LINEAR` is a line in the
// view distance, amount = d * x + y; `GL_EXP` -- the head in water or lava --
// replaces it with `1 - e^(-density * d)`, which the shaders compute as
// `1 - 2^(-d * z)` with z = density * log2(e). Both terms are always
// evaluated and the larger wins, so each mode zeroes the other's.
// **No fog, for what is drawn in camera or screen space.** `iq.c(F)` turns
// `GL_FOG` off before `renderHand`, so the hand, the flames and the water over
// the view are never fogged; a line that starts a quarter of the far plane out
// never reached them anyway, but the exponential curve does from the eye.
void Renderer::clearFogParam(const Pipeline& pipeline) const
{
    if (pipeline.uLocFog >= 0) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, pipeline.uLocFog, 0.0f, 0.0f, 0.0f, 0.0f);
    }
}

void Renderer::setFogParam(int location, float start, float end) const
{
    if (fogExp2_ > 0.0f) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, location, 0.0f, 0.0f, fogExp2_, 0.0f);
        return;
    }
    const float slope = 1.0f / (end - start);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, location, slope, -start * slope, 0.0f, 0.0f);
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
    // 0.05-block near plane and a 176-block far plane one depth unit spans
    //
    //     dd = d^2 * (far - near) / (near * far * 65536)
    //
    // which is 0.76 blocks at 50 away, 3.05 at 100, and 7.81 at 160. Well
    // inside 50 blocks two surfaces a whole block apart already land on the
    // *same* depth value, so which one wins is decided by rounding -- and it
    // changes as the camera moves, and differs between the two eyes, which is
    // what makes it read as shimmer rather than as a static artefact.
    //
    // (Those are the figures at the near plane this renderer settled on. The
    // shimmer was measured and fixed while `kNearPlane` was still 0.2, where
    // they are a quarter of the size -- 0.19, 0.76, 1.95 -- and 16 bits was not
    // enough even then. 24 is what made dropping the near plane free; see
    // `kNearPlane`.)
    //
    // 24 bits multiplies every one of those figures by 256: at 160 blocks a
    // depth unit is 0.03 of a block, so a block of separation is 33 of them.
    // The cost is one more byte per pixel in each eye's depth buffer, 375 KB
    // total, against the ~5 MB of VRAM M0 measured free.
    eye_[0] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    eye_[1] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (eye_[0] == nullptr || eye_[1] == nullptr) {
        return false;
    }
    C3D_RenderTargetSetOutput(eye_[0], GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
    C3D_RenderTargetSetOutput(eye_[1], GFX_TOP, GFX_RIGHT, kDisplayTransferFlags);

    if (!buildPipeline(&cubePipeline_, world_shader_shbin, world_shader_shbin_size, false)
        || cubePipeline_.uLocSeam < 0 || cubePipeline_.uLocSeamDir < 0
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
        // The entity sheets have no pack behind them here either, and unlike
        // the two above they are never optional: a caller with no menu still
        // hangs paintings and still wants a boat with a skin on it.
        texture::buildDevArtSkins(&fallback.entityRgba);
        texture::buildDevArtArt(&fallback.artRgba);
    }
    const texture::AtlasImage& atlasImage =
        config_.atlas != nullptr ? *config_.atlas : fallback;

    if (!atlas_.init(atlasImage) || !lightmap_.init()) {
        return false;
    }

    // **The item sheet, here as well as in setAtlas.** It used to be uploaded
    // only on a texture-pack change, so from launch `hasItems()` was false and
    // every gui/items.png sprite -- a sword in the hand, a dropped apple, the
    // compass -- drew nothing until the player happened to switch packs. Not
    // fatal either: a pack without the sheet, or no room for its 256 KB, is
    // what `hasItems()` answers for.
    atlas_.initItems(atlasImage);
    // The icon sheet the hearts come off, on the same terms -- and uploaded at
    // init for the reason the item sheet now is.
    atlas_.initIcons(atlasImage);

    // **Not fatal if it fails.** The entity and art sheets are 32 KB and
    // 256 KB of ordinary linear memory; a console that cannot spare them draws
    // no paintings, which `hasArt()` gates, rather than refusing to start.
    atlas_.initEntitySheets(atlasImage);

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
        // The item sheet rides along. A pack without one is a pack -- see
        // core/texture/atlas_image.hpp -- and `hasItems()` is what the item
        // pass asks before it draws anything from it.
        atlas_.initItems(image);
        atlas_.initIcons(image);
        // ...and so do the entity sheets, which unlike the item sheet are
        // always present: `buildEntitySkins` lays down stand-ins for whatever
        // the pack does not carry.
        atlas_.initEntitySheets(image);
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
    // These are per-renderer frame buffers, not process-lifetime resources.
    // Returning from a world to the menu and opening another must not exhaust
    // linear memory one renderer instance at a time.
    for (void** buffer : {&outlineVerts_, &crosshairVerts_, &focusHintVerts_,
                          &mobShellVerts_, &mobEyeVerts_, &particleVerts_, &itemVerts_,
                          &fallingVerts_, &tntVerts_, &tntFlashVerts_, &paintingVerts_,
                          &arrowVerts_, &boatVerts_,
                          &minecartVerts_, &minecartBlockVerts_, &mobVerts_,
                          &entityFireVerts_, &signVerts_,
                          &heldVerts_, &fireOverlayVerts_, &waterOverlayVerts_,
                          &chatVerts_, &chatStrips_, &skyVerts_}) {
        if (*buffer != nullptr) {
            linearFree(*buffer);
            *buffer = nullptr;
        }
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

// **Everything about the sky that changes, worked out once a frame.**
//
// `iq.h(F)V` -- updateFogColor -- and the three `World` colours it calls, plus
// the two things renderSky asks for directly: how bright the stars are and
// where the sun is. None of it is per-eye and none of it is per-vertex, which
// is why the sky's 4,480 vertices are written once at start-up and never again.
void Renderer::setWorldTime(i64 dayTicks, float partialTicks, world::FogMedium medium,
                            float fogBrightness)
{
    const world::SkyColour sky = world::skyColour(dayTicks, partialTicks);
    const world::ViewFog view =
        world::viewFog(dayTicks, partialTicks, config_.meshDistance, medium, fogBrightness);
    const world::SkyColour& fog = view.colour;
    fogExp2_ = view.density * 1.4426950f;  // log2(e)
    const world::SkyColour below = render::voidPlaneColour(sky);

    // **The clear and the fog are the same colour, and that is the point.**
    // Whatever the far plane cuts off -- and at a six-chunk render distance
    // that includes most of the sky plane -- is already fully fogged where it
    // is cut, so the seam between geometry and background cannot be seen.
    clearColour_ = packRgba(fog.r, fog.g, fog.b);
    fogColour_ = packAbgr(fog.r, fog.g, fog.b);
    skyPlaneColour_ = packAbgr(sky.r, sky.g, sky.b);
    voidPlaneColour_ = packAbgr(below.r, below.g, below.b);

    starBrightness_ = world::starBrightness(dayTicks, partialTicks);
    celestialAngle_ = world::celestialAngle(dayTicks, partialTicks);
}

// The sky's own view-projection: the camera's rotation with none of its
// position, and a far plane that does not depend on the render distance.
//
// **The eye is at the origin rather than at the camera**, which is the one
// thing that makes this not simply `viewProjection` with a different far plane.
// a1.1.2 draws the sky before it translates anything into world space, so the
// plane is sixteen blocks above *you* wherever you stand and the sun is a
// hundred blocks away whatever you are standing on. Stereo is kept: the
// interocular offset is in the projection, so the sky sits at the far end of
// the depth budget instead of being flattened onto the screen plane.
C3D_Mtx Renderer::skyViewProjection(const Camera& camera, float iod) const
{
    C3D_Mtx projection;
    Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(config_.fovDegrees), 400.0f / 240.0f,
                        kNearPlane, kSkyFarPlane, iod, focalBlocks_, false);

    float dx, dy, dz;
    camera.look(&dx, &dy, &dz);
    C3D_FVec eye = FVec3_New(0.0f, 0.0f, 0.0f);
    C3D_FVec target = FVec3_New(dx, dy, dz);
    C3D_FVec up = FVec3_New(0.0f, 1.0f, 0.0f);

    C3D_Mtx view;
    Mtx_LookAt(&view, eye, target, up, false);

    C3D_Mtx vp;
    Mtx_Multiply(&vp, &projection, &view);
    return vp;
}

// **`e.a(F)V` -- renderSky -- in the order the original draws it**: the sky
// plane, then the sun, the moon and the stars added over it, then the plane
// below. First in the eye, with depth writes off, so every one of them is
// behind the whole world whatever distance it claims to be at.
//
// Five draws off one buffer. Four states differ between the two halves and each
// one is the original's:
//
//   * the **planes are fogged**, which is where the horizon comes from -- a
//     flat ceiling with a linear fade across it reads as a dome;
//   * the **sun, moon and stars are not**, and are **added** rather than
//     blended (`glBlendFunc(GL_ONE, GL_ONE)`), so a star over the sky brightens
//     it and a star over nothing is the star;
//   * the **alpha test is off** for all five, because the fog amount lives in
//     the vertex alpha here as everywhere else and the world's test would throw
//     the foggy end of the sky away;
//   * **culling is off**, because half of this is seen from below and half from
//     above and neither has a back worth finding out about.
//
// **Named deviation: this draws at every render distance.** `iq.c(F)V` guards
// the whole call with `if (renderDistance < 2)`, so a1.1.2 at Short or Tiny has
// no sky at all -- no sun, no moon, no stars, just the clear colour. That is a
// 2010 frame-rate concession and not something a player chose to see, and this
// port's rule is faithful to the game rather than to its limits. The sky is
// 1,120 quads that never change and one full-screen plane's worth of fill; if a
// console ever says that is too much, the guard is one line and it goes here.
void Renderer::drawSky(const Camera& camera, float iod)
{
    if (skyVerts_ == nullptr) {
        return;
    }

    const C3D_Mtx flat = skyViewProjection(camera, iod);
    // The celestial half turns about X by the celestial angle, which is the
    // whole of the day's motion. `Mtx_RotateX(..., true)` multiplies from the
    // right, so this is `projection * view * rotate` -- the same order
    // `glRotatef` after the camera transform produces.
    C3D_Mtx turning = flat;
    Mtx_RotateX(&turning, C3D_AngleFromDegrees(celestialAngle_ * 360.0f), true);

    bindPipeline(detailPipeline_);
    // The sky's own fog line, and not the world's: `iq.a(-1)` sets the start to
    // zero and the end to eight tenths of the far plane, where the world's runs
    // from a quarter of it. Overhead is therefore already slightly fogged,
    // which is what the original looks like.
    // Same two numbers bindPipeline writes for the world, over the sky's own
    // start and end. The bound on `fogEnd` is a guard and not a setting: a
    // render distance of zero would divide by it, and the debug page can set
    // one.
    const float fogStart = fogEndBlocks() * render::kSkyFogStartScale;
    const float fogEnd = fogEndBlocks() * render::kSkyFogEndScale;
    // Under water the sky gets the world's curve and not a line of its own:
    // `iq.a(-1)` only moves the line, and `GL_EXP` has no start or end.
    if (detailPipeline_.uLocFog >= 0 && fogEnd > fogStart + 1.0f) {
        setFogParam(detailPipeline_.uLocFog, fogStart, fogEnd);
    }

    // The scale the sky's own units need: the vertices are written at 1/64 of a
    // block and the detail shader divides by 1024. See core/render/sky.hpp.
    constexpr float kSkyScale =
        float(mesh::kDetailUnitsPerBlock) / float(render::kSkyUnitsPerBlock);
    C3D_Mtx planes = flat;
    Mtx_Scale(&planes, kSkyScale, kSkyScale, kSkyScale);
    C3D_Mtx celestial = turning;
    Mtx_Scale(&celestial, kSkyScale, kSkyScale, kSkyScale);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);

    auto* base = static_cast<mesh::DetailVertex*>(skyVerts_);
    // **A local `C3D_BufInfo` and `C3D_SetBufInfo` for every range, which is
    // drawChat's pattern and not drawSelection's -- and the difference is the
    // whole of what was wrong with this pass.**
    //
    // `C3D_GetBufInfo()` hands back the context's buffer config *and marks it
    // dirty*; writing through that pointer afterwards does not. So fetching it
    // once and re-basing it between draws -- which is fine for the one-draw
    // passes that do it -- sent only the **first** range's base to the GPU, and
    // every range after it drew the first range's vertices. The void plane, the
    // sun, the moon and the stars were all drawing the sky plane: the void's
    // colour landed on the plane *above* the camera and painted over it,
    // nothing was drawn below at all, and the sun and moon became one 64-block
    // quad at sixteen blocks up, turning with the day and seen edge-on as a
    // pale stripe. Every reported symptom is that one missing dirty mark.
    //
    // It cannot be hoisted out of the lambda: each range has a different base,
    // and the base is exactly what has to reach the GPU.
    const auto drawRange = [&](int first, int vertices) {
        C3D_BufInfo range;
        BufInfo_Init(&range);
        BufInfo_Add(&range, base + first, sizeof(mesh::DetailVertex), 3, 0x210);
        C3D_SetBufInfo(&range);
        const int quads = vertices / 4;
        C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
    };

    // **A plane: a flat colour faded into the fog, and no texture at all.**
    // Two stages rather than the world's three, because there is nothing to
    // sample: stage 0 states the colour, stage 1 does the same INTERPOLATE the
    // world's third stage does, with the fog amount the vertex shader left in
    // primary alpha. The vertex colour bytes are unused here -- a constant can
    // change once a frame where a vertex cannot.
    //
    // Stated twice, once per plane, because the celestial draws in between take
    // both stages for their own.
    const auto planeState = [&](u32 colour) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_TexEnv* env0 = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env0);
        C3D_TexEnvSrc(env0, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env0, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(env0, colour);
        C3D_TexEnv* env1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(env1);
        C3D_TexEnvSrc(env1, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
        C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                        GPU_TEVOP_RGB_SRC_ALPHA);
        C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(env1, fogColour_);
        C3D_TexEnvSrc(env1, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvInit(C3D_GetTexEnv(2));
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &planes);
    };

    planeState(skyPlaneColour_);
    drawRange(render::kSkyPlaneFirst, render::kSkyPlaneVertices);

    // **The three that turn, added rather than blended and with no fog.**
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, detailPipeline_.uLocMvp, &celestial);

    // The sun and the moon are two quads off the entity sheet, at full
    // brightness: `glColor4f(1, 1, 1, 1)` with the texture straight through.
    if (atlas_.hasEntities()) {
        atlas_.bindEntity(0);
        C3D_TexEnv* sun = C3D_GetTexEnv(0);
        C3D_TexEnvInit(sun);
        C3D_TexEnvSrc(sun, C3D_Both, GPU_TEXTURE0, GPU_TEXTURE0, GPU_TEXTURE0);
        C3D_TexEnvFunc(sun, C3D_Both, GPU_REPLACE);
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        drawRange(render::kSunFirst, render::kSunVertices);
        drawRange(render::kMoonFirst, render::kMoonVertices);
        atlas_.bind(0, wireframe_);
    }

    // The stars, all 780 of them in one draw, at the brightness the time of day
    // says. Skipped entirely by day, which is what `if (f > 0)` does in the
    // original -- and by day it is exactly zero, not merely small.
    if (starBrightness_ > 0.0f) {
        C3D_TexEnv* stars = C3D_GetTexEnv(0);
        C3D_TexEnvInit(stars);
        C3D_TexEnvSrc(stars, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvFunc(stars, C3D_Both, GPU_REPLACE);
        C3D_TexEnvColor(stars, packAbgr(starBrightness_, starBrightness_, starBrightness_));
        C3D_TexEnvInit(C3D_GetTexEnv(1));
        drawRange(render::kStarsFirst, render::kStarVertices);
    }

    // **The plane below, last, which is where renderSky puts it and it matters.**
    // Nothing in this pass writes depth, so the order *is* the occlusion: the
    // original draws this opaque plane after the celestial half precisely so
    // that the half of the sky under your feet -- the moon by day, the stars
    // below the horizon -- is covered by it rather than shining through the
    // ground. Sixteen blocks down, so terrain hides it wherever there is any.
    planeState(voidPlaneColour_);
    drawRange(render::kVoidPlaneFirst, render::kVoidPlaneVertices);

    // **Everything this pass changed, stated back.** It runs before the world
    // rather than after it, so unlike the selection box and the crosshair it
    // cannot leave the state where it likes -- and `applyWorldState` is the one
    // description of what the world's passes need.
    applyWorldState();
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

// The draws after the terrain passes are not section draws, so they cannot use
// drawPass's per-section guard.  Keep enough room for every optional entity
// pass, their pipeline changes, the selection outline and the crosshair.  This
// is deliberately generous: omitting distant terrain is recoverable; writing
// one word past citro3d's command buffer corrupts application memory.
constexpr u32 kCommandWordsPerLatePasses = 65536;

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
        if (commandWordsFree() < reserve + commandTailReserve_) {
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
    // This applies to this eye only.  The next eye re-establishes the same
    // reserve before it records a section, so it too has space for its late
    // passes even when the first eye consumed most of the frame budget.
    commandTailReserve_ = kCommandWordsPerLatePasses;
    C3D_RenderTargetClear(eye_[eye], C3D_CLEAR_ALL, clearColour_, 0);
    C3D_FrameDrawOn(eye_[eye]);

    // **Before everything, which is where renderSky runs.** It writes no depth
    // and its own far plane is past the world's, so the order is the only thing
    // keeping it behind the terrain -- exactly as in the original, where
    // `glDepthMask(false)` is the whole of the argument.
    drawSky(camera, iod);

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
    // **On the first eye this used to cost nothing** -- `C3Di_SplitFrame`
    // returns early when nothing has been recorded -- and now it does, because
    // the sky pass above records five draws through the detail program before
    // this one. That is the same hazard rather than a new cost: something has
    // to drain between a detail draw and the bind that turns the geometry stage
    // on, and this is where it happens.
    const bool splitAroundGeo =
        cubeFormat_ == mesh::CubeFormat::Quads && geoSplitPasses();
    if (splitAroundGeo) {
        C3D_FrameSplit(0);
        ++frameStats_.geoSplits;
    }

    // **The cube pass samples the cube atlas**, where every tile is a 4x4
    // repeat so a merged quad can repeat it -- see core/mesh/cube_atlas.hpp.
    // Everything after the pass reads the ordinary atlas on unit 0, as every
    // draw function here assumes, so it goes back straight afterwards.
    atlas_.bindCube(0, wireframe_);
    drawPass(vp, Pass::Cube, originChunkX, originChunkZ);
    atlas_.bind(0, wireframe_);

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

    // **Opaque non-cube geometry is culled, and the sheets carry their own
    // second side.**
    //
    // This pass used to run with culling off, on the argument that a flower is
    // two crossed planes and the original has no culling to satisfy. The first
    // half of that is true and the conclusion was wrong: `mesh::addSheet` --
    // which *every* non-box shape here is built from -- already emits the plane
    // and its mirror, and so does the cross in `mesher.cpp`. Nothing in this
    // pass depends on the cull state to be two-sided; the shapes that need two
    // sides have two quads.
    //
    // What the missing cull cost was a **door showing its own inside**. The top
    // tile of a wooden door has a window in it -- 48 transparent texels of
    // terrain tile 81 -- and `renderBlockDoor` emits all six faces of the
    // door's three-sixteenths box, as this does. Looking through that window
    // with nothing culled, the face on the far side of the box is drawn from
    // behind, and a door reads as hollow. Reported from play. With the cull on
    // you see through the window, which is what the window is for.
    //
    // A torch is the other one that was already written for it: its four sides
    // are wound outward on purpose, with a comment saying the two facing away
    // are culled, and until now they were not.
    C3D_CullFace(GPU_CULL_BACK_CCW);
    drawPass(vp, Pass::Detail, originChunkX, originChunkZ);

    // **Between the two terrain passes**, which is where
    // `EntityRenderer.renderWorld` runs `renderParticles`: after the opaque
    // pass so a fleck is occluded by the ground it is bouncing on, before the
    // translucent one so it is visible *through* water rather than sorted
    // against it.
    //
    // **Culling off again for the entity passes**, which it used to inherit
    // from the detail pass above: a billboard faces the camera, and which way
    // it is wound depends on where the camera is. The entity models are
    // two-sided for their own reasons -- a mob's skin has cutouts and a
    // painting is seen from behind.
    C3D_CullFace(GPU_CULL_NONE);
    drawParticles(vp, originChunkX, originChunkZ);
    drawItemEntities(vp, originChunkX, originChunkZ);
    drawFallingBlocks(vp, originChunkX, originChunkZ);
    drawPrimedTnt(vp, originChunkX, originChunkZ);
    drawPaintings(vp, originChunkX, originChunkZ);
    drawArrows(vp, originChunkX, originChunkZ);
    drawBoats(vp, originChunkX, originChunkZ);
    drawMinecarts(vp, originChunkX, originChunkZ);
    drawMobs(vp, originChunkX, originChunkZ);
    drawEntityFire(vp, originChunkX, originChunkZ);
    drawSigns(vp, originChunkX, originChunkZ);

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
    drawBreakOverlay(vp, originChunkX, originChunkZ);
    drawCrosshair(vp, camera, originChunkX, originChunkZ);

    // **Last, which is where `renderHand` runs.** After the crosshair rather
    // than before it because the original draws the hand before the whole GUI,
    // and the crosshair is part of that GUI even though this port draws it as
    // geometry. The two never overlap -- one is the middle of the screen and
    // the other its bottom right corner -- so the order is a statement of
    // intent rather than something a player can see.
    drawHeldItem(iod);
    drawFireOverlay(iod);
    drawWaterOverlay(iod);
    drawChat();
    drawHud();
    // **After the hearts**, because it is over the world and over the HUD both:
    // the point of the band is that nothing on the top screen is taking the
    // buttons right now.
    drawFocusHint();

    C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    commandTailReserve_ = 0;
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

    // The three-stage combiner every pass that draws off the atlas inherits.
    // Factored out because `drawMobs`'s eye pass replaces it for one draw and
    // has to put it back exactly -- see `Renderer::applyAtlasTexEnv`.
    applyAtlasTexEnv();
}

// The combiner chain `applyWorldState` installs, and the one the spider's eye
// pass restores when it has finished borrowing the stages. It is a method
// rather than a copy in two places because the two must not drift: the eye pass
// is a single draw in the middle of the frame and everything after it reads
// whatever it left behind.
void Renderer::applyAtlasTexEnv()
{
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
        C3D_TexEnvColor(env2, fogColour_);
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

    // **The frame opened, so the GX queue is empty** -- `C3D_FrameBegin` does
    // not return true before it is -- and every animated-tile copy queued
    // since the last frame has finished reading its staging. That is the only
    // point at which the staging can be handed out again; see
    // `Atlas::tileStagingUsed_`.
    atlas_.tileCopiesRetired();

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

    buildChat();

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
    // After the frame is handed to the GPU, so the copy overlaps its work.
    bottom::presentIfChanged();

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
