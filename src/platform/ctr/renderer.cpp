#include "platform/ctr/renderer.hpp"

#include "core/mesh/vertex.hpp"
#include "core/texture/dev_art.hpp"

#include <3ds.h>

#include <cmath>
#include <cstdio>

#include <detail_shader_shbin.h>
#include <quad_shader_shbin.h>
#include <world_shader_shbin.h>

namespace mc::ctr {

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

}  // namespace

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

// **citro3d does not bounds-check its command buffer.** `GPUCMD_AddRawCommands`
// memcpys into `gpuCmdBuf + offset` and advances the offset; nothing anywhere
// compares that against `gpuCmdBufSize`. Overrun it and the writes land in
// whatever linear allocation follows -- so it presents as a crash somewhere
// else entirely, intermittently, depending on where the player is looking.
//
// The draw list is what fills it, and **stereo is what makes it reachable**:
// the same view costs twice the commands, which is why the symptom was "it
// crashes in 3D, sometimes". Sizing the buffer bigger only moves the view that
// breaks it, because the bound is the number of visible sections and that is a
// property of the world, not of us. So the buffer is checked instead: when
// there is not enough room left for another section, the frame is split, which
// hands what has been recorded to the GPU and starts recording again.
//
// A split is safe in the middle of a pass. `C3Di_SplitFrame` calls
// `GPUCMD_Split` and nothing else that matters; the GPU executes the lists in
// order and its registers -- render target, shader, textures, viewport --
// carry across, because a command list is a recording, not a context.
constexpr u32 kCommandWordsPerSection = 1024;

bool splitIfCommandBufferIsFull()
{
    u32 size = 0;
    u32 offset = 0;
    GPUCMD_GetBuffer(nullptr, &size, &offset);

    // Words, both of them. The reserve covers a pipeline bind plus a section's
    // draw with room to spare, which is far more than either costs: being wrong
    // in this direction buys an extra split, and being wrong in the other
    // corrupts whatever linear allocation sits after the command buffer.
    // Written as an addition so an offset that has somehow already passed the
    // end still splits, rather than wrapping to a huge headroom and sailing on.
    if (offset + kCommandWordsPerSection > size) {
        C3D_FrameSplit(0);
        return true;
    }
    return false;
}

}  // namespace

// way rather than drawing each section's two ranges together is the whole
// reason the two streams are kept apart in the mesh: the program and the
// attribute layout change twice per eye instead of twice per section.
void Renderer::drawPass(const C3D_Mtx& vp, Pass pass, i32 originChunkX, i32 originChunkZ)
{
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

    for (int i = 0; i < count; ++i) {
        const render::VisibleSection& section = list[reversed ? count - 1 - i : i];

        const mesh::MeshRanges& ranges = pool.ranges(section.slot);
        const bool geoQuads = pass == Pass::Cube && ranges.cubeFormat == mesh::CubeFormat::Quads;
        const usize stride = pass == Pass::Cube
                                 ? (geoQuads ? sizeof(mesh::QuadVertex) : sizeof(mesh::WorldVertex))
                                 : sizeof(mesh::DetailVertex);
        const usize bytes = pass == Pass::Cube          ? ranges.cubeBytes
                            : pass == Pass::Detail      ? ranges.detailBytes
                                                        : ranges.translucentBytes;
        const int quads = pass == Pass::Cube ? int(ranges.cubeQuads()) : int(bytes / (4 * stride));
        if (quads == 0) {
            continue;
        }

        if (splitIfCommandBufferIsFull()) {
            ++frameStats_.commandSplits;
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
        } else {
            C3D_DrawElements(GPU_TRIANGLES, quads * 6, C3D_UNSIGNED_SHORT, indices_);
        }

        ++frameStats_.drawCalls;
        frameStats_.quads += usize(quads);
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
    drawPass(vp, Pass::Cube, originChunkX, originChunkZ);

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

void Renderer::drawFrame(const Camera& camera, void* overlayContext, Overlay2D overlay)
{
    frameStats_ = FrameStats{};

    // Skip the second eye entirely at slider zero rather than rendering it and
    // throwing it away -- half the geometry cost, for free.
    const float slider = osGet3DSliderState();
    const bool wantStereo = slider > 0.0f;
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
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    const u64 afterBegin = svcGetSystemTick();

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
