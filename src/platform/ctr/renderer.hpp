#pragma once

// The GPU half of the world renderer: citro3d state, the two eyes, and the draw
// loop that walks ChunkRenderer's list.
//
// Everything about *what* to draw was decided in core. What is left here is
// genuinely hardware: which matrix each section gets, which buffer the vertices
// come from, and skipping the second eye when the slider is at zero.

#include "core/mesh/vertex.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/util/frustum.hpp"
#include "core/util/types.hpp"
#include "platform/ctr/gpu_memory.hpp"
#include "platform/ctr/textures.hpp"

#include <3ds.h>
#include <citro3d.h>

namespace mc::ctr {

// Minecraft's own camera convention, so a saved rotation goes straight in: yaw
// 0 looks along +Z, and positive pitch looks down.
//
// **Position is double and rotation is float, which is what the original does
// too** -- `Entity.posX` is a double and `Entity.rotationYaw` is a float, and
// level.dat stores them that way (see world::PlayerData). It used to be float
// here, and that was a real limit rather than a tidy simplification: a float
// holds whole numbers exactly only up to 16,777,216, so out at the Far Lands
// around 12,550,824 its spacing is a full block. The player's position would
// snap to the block grid and walking would become a series of one-block jumps
// that no amount of care in the renderer could smooth out.
//
// Doubles cost nothing here. There are three of them, they are touched once
// per frame, and every value the GPU ever sees is made relative and narrowed
// to float first -- see Renderer::renderOrigin.
struct Camera {
    double x = 0.0, y = 64.0, z = 0.0;
    float yaw = 0.0f;    // radians
    float pitch = 0.0f;  // radians, clamped to +-90 degrees

    void look(float* dx, float* dy, float* dz) const;

    i32 chunkX() const;
    i32 chunkZ() const;
    int sectionY() const;
};

// How much linear memory to give the GPU command buffer, for C3D_Init.
//
// Four times citro3d's C3D_DEFAULT_CMDBUF_SIZE, because a stereo frame over a
// long draw list fills the default one. This number is a performance knob and
// not a safety one -- what makes overrun impossible is the split guard in
// renderer.cpp, which is checked against whatever size is actually allocated.
// Raising it only makes splits rarer.
constexpr u32 kCommandBufferBytes = 4 * 0x40000;

// **Bind this before freeing any shader program, and it is never freed itself.**
//
// citro3d remembers the last program bound, in `C3D_Context::program`, and the
// next `C3D_BindProgram` **dereferences that old pointer before it looks at the
// new one** -- `oldProg->vertexShader->dvle`, to decide whether the geometry
// shader mode has to change. Disassembled from citro3d 1.7.1's `base.c:369`,
// which is where the crash landed:
//
//     ldr lr,  [r12]        @ oldProg->vertexShader
//     ldr r12, [lr]         @ ...->dvle
//     ldr r12, [r12, #8]    @ <-- data abort
//
// So freeing a shader program leaves citro3d holding a corpse, and the fault
// lands on whoever binds next -- never on the code that did the freeing.
// `crashlogs/004-loading-a-world-from-the-menu` is exactly that: the menu's
// `C2D_Fini` freed citro2d's shader, and the first `bindPipeline` of the first
// frame of the game read through it and aborted on `0x1008`. It is symmetric,
// too: `Renderer::shutdown` frees its own three pipelines, which would take the
// *menu* down on the way back.
//
// Parking the pointer on a program that is built once and never freed makes the
// next bind always dereference something alive. Nothing is ever drawn with it;
// every real path binds its own program before it draws. The one case that does
// not need it is a free immediately followed by `C3D_Fini`, which throws the
// context and its pointer away together -- the M0 probe's teardown.
void parkShaderProgram();

class Renderer {
public:
    struct Config {
        int meshDistance = 8;
        float fovDegrees = 70.0f;
    };

    struct FrameStats {
        int drawCalls = 0;
        usize quads = 0;
        bool stereo = false;
        // How many times the GPU command buffer filled up mid-frame. Any value
        // above zero is a frame that would have overrun the buffer and
        // corrupted linear memory before the split guard existed, so this is
        // the number that says whether that crash is what was happening.
        int commandSplits = 0;
    };

    bool init(const Config& config, bool isNew3DS);
    void shutdown();

    render::ChunkRenderer& chunks() { return chunks_; }
    const render::ChunkRenderer& chunks() const { return chunks_; }

    const Config& config() const { return config_; }

    // The frustum used for culling: one for both eyes, widened slightly so a
    // section that only the outer eye can see is not cut. Call before
    // ChunkRenderer::beginFrame.
    Frustum cullFrustum(const Camera& camera) const;

    void setSkyDarken(int subtracted) { lightmap_.setSkyDarken(subtracted); }

    // Render distance in chunks, changed live from the debug settings page.
    //
    // The field, the pool and its size-class table are all sized to it, so this
    // throws all three away and builds them again -- which means every mesh in
    // the pool goes with them. **The world streamer has to be told immediately
    // afterwards**, in that order: it republishes its columns into whatever
    // field it finds, and the whole point is that it finds the new one.
    void setMeshDistance(int distance);

    // The debug wireframe. False if there was no memory for the outline atlas,
    // in which case the setting stays off rather than silently doing nothing.
    bool setWireframe(bool on);
    bool wireframe() const { return wireframe_; }

    // The cube encoding: four 12-byte vertices per quad, or one 8-byte vertex
    // expanded by a geometry shader. **Also a measurement instrument** -- this
    // is the comparison docs/3ds-performance.md section 2 asks for, and the M2
    // gate is waiting on it.
    //
    // Throws the pool away, because every mesh in it is in the old format.
    // **The world streamer has to be told immediately afterwards**, in that
    // order, exactly as with setMeshDistance: it republishes its columns into
    // whatever field it finds.
    void setCubeFormat(mesh::CubeFormat format);
    mesh::CubeFormat cubeFormat() const { return cubeFormat_; }

    // Stereo strength, as the on-screen disparity in pixels that a point at
    // infinity gets at full slider, and the distance that sits at the screen
    // plane. Adjustable at runtime because these are the two numbers no amount
    // of host testing can choose -- see the derivation in renderer.cpp.
    void setStereo(float disparityPixels, float focalBlocks);
    float stereoDisparityPixels() const { return disparityPixels_; }
    float stereoFocalBlocks() const { return focalBlocks_; }

    // Clears both eyes, draws the world, ends the frame.
    void drawFrame(const Camera& camera);

    const FrameStats& frameStats() const { return frameStats_; }

    // Milliseconds the GPU spent drawing and processing, from its own timers
    // rather than from wall clock, so vsync does not hide them.
    float gpuDrawMs() const { return drawMs_; }
    float gpuProcessMs() const { return processMs_; }

    // The frame split into the part that is work and the part that is waiting.
    //
    // `blockedMs` is C3D_FrameBegin, and it is a **vsync wait**, not a GPU one.
    // C3D_FRAME_SYNCDRAW makes it spin on gspWaitForAnyEvent until citro3d's
    // VBlank callback bumps its frame counter (disassembled out of
    // renderqueue.o), after which it waits for the GPU queue to drain -- and
    // that second wait happens with or without the flag. So the top screen's
    // 59.83 Hz is a hard floor on the frame period no matter how fast the rest
    // of this is, and the only question the overlay has to answer is whether
    // anything else is close to filling 16.7 ms.
    //
    // `submitMs` is everything between FrameBegin and FrameEnd: building
    // matrices, writing uniforms and recording draws. FrameEnd itself only
    // enqueues, so this does not include the GPU.
    float blockedMs() const { return blockedMs_; }
    float submitMs() const { return submitMs_; }

    usize freeVramBytes() const;
    usize freeLinearBytes() const;

private:
    // One ring beyond the render distance, so the far plane never cuts through
    // a section the walk is willing to draw. Fog reaches full opacity a ring
    // earlier, at exactly the render distance, so nothing is ever seen being
    // clipped by the far plane.
    float farPlane() const { return float(config_.meshDistance + 1) * 16.0f; }

    // a1.1.2's own terrain fog, from iq.class: linear from renderDistance * 0.25
    // to renderDistance. Both ends scale with the render distance, which is what
    // stops distance 10 from being fogged as heavily as distance 6.
    float fogEndBlocks() const { return float(config_.meshDistance) * 16.0f; }
    float fogStartBlocks() const { return fogEndBlocks() * 0.25f; }

    void drawEye(int eye, const Camera& camera, float iod);

    // Which of a section's three ranges a pass draws. The order here is the
    // order they are drawn in and the order they sit in memory.
    enum class Pass {
        Cube,         // 12-byte vertices, back-face culled, depth writes on
        Detail,       // 16-byte vertices, no culling: a cross is two-sided
        Translucent,  // 16-byte vertices, blended, depth writes off, far to near
    };

    // One pass over the draw list. Reversed for Translucent, because the walk
    // produces the list breadth-first from the camera and blending needs the
    // opposite.
    // `originChunkX`/`originChunkZ` is the chunk the view-projection was built
    // around; section translations are measured from it. See the note in
    // drawPass on why this is not the world origin.
    void drawPass(const C3D_Mtx& viewProjection, Pass pass, i32 originChunkX, i32 originChunkZ);
    C3D_Mtx viewProjection(const Camera& camera, float iod, float fovRadians) const;

    Config config_;

    // Kept because the VBO budget is recomputed whenever the render distance
    // changes, and it is measured against the model.
    bool isNew3DS_ = false;
    bool wireframe_ = false;

    // Defaults live in renderer.cpp next to the arithmetic that explains them.
    float disparityPixels_;
    float focalBlocks_;

    C3D_RenderTarget* eye_[2] = {nullptr, nullptr};

    // Two vertex formats, so two programs and two attribute layouts. They are
    // bound once per pass rather than once per section: the draw loop runs
    // every section's cube range first, then switches and runs every section's
    // detail range.
    struct Pipeline {
        DVLB_s* dvlb = nullptr;
        shaderProgram_s program{};
        C3D_AttrInfo attrs{};
        int uLocMvp = -1;
        int uLocFog = -1;
    };

    bool buildPipeline(Pipeline* pipeline, const void* shbin, u32 shbinSize, bool detail);

    // The geometry-shader program, which differs in three ways and so does not
    // fit buildPipeline: a second DVLE to attach, a gsh input stride, and a
    // face table to upload.
    bool buildQuadPipeline(const void* shbin, u32 shbinSize);

    void bindPipeline(const Pipeline& pipeline);

    // Sizes the pool against what the linear heap has free and hands it to the
    // chunk renderer. Both setMeshDistance and setCubeFormat throw everything
    // away and call this.
    void rebuildChunks();

    Pipeline cubePipeline_;
    Pipeline detailPipeline_;
    Pipeline quadPipeline_;

    // Where quad.v.pica's faceBasis[18] lives, and the values to put in it.
    // Written on every bind rather than once at init for the same reason the
    // fog line is -- binding a program reloads its own constant table, and the
    // two programs' registers were allocated independently.
    int uLocFaceBasis_ = -1;

    mesh::CubeFormat cubeFormat_ = mesh::CubeFormat::Vertices;

    Atlas atlas_;
    Lightmap lightmap_;

    // One immutable index buffer for the whole process: 4 vertices per quad,
    // repeating 0,1,2, 0,2,3. Chunk meshes therefore carry no index data at all.
    u16* indices_ = nullptr;

    GpuVboAllocator allocator_;
    render::ChunkRenderer chunks_;

    u32 frame_ = 0;
    bool stereo_ = false;
    float drawMs_ = 0.0f;
    float processMs_ = 0.0f;
    float blockedMs_ = 0.0f;
    float submitMs_ = 0.0f;
    FrameStats frameStats_;
};

// svcGetSystemTick runs at the ARM11 reference clock on both models, so this is
// the one conversion the whole platform layer uses -- the overlay's phase
// numbers and the game loop's dt have to be in the same units to be comparable.
inline float millisFromTicks(u64 ticks)
{
    return float(double(ticks) * 1000.0 / double(SYSCLOCK_ARM11));
}

}  // namespace mc::ctr
