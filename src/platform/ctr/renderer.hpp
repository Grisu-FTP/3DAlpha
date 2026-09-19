#pragma once

// The GPU half of the world renderer: citro3d state, the two eyes, and the draw
// loop that walks ChunkRenderer's list.
//
// Everything about *what* to draw was decided in core. What is left here is
// genuinely hardware: which matrix each section gets, which buffer the vertices
// come from, and skipping the second eye when the slider is at zero.

#include "core/mesh/vertex.hpp"
#include "core/net/entities.hpp"
#include "core/render/break_overlay.hpp"
#include "core/render/chat_mesh.hpp"
#include "core/render/hud_mesh.hpp"
#include "core/render/chunk_renderer.hpp"
#include "core/render/falling_block_mesh.hpp"
#include "core/render/primed_tnt_mesh.hpp"
#include "core/render/held_item.hpp"
#include "core/render/item_entity_mesh.hpp"
#include "core/render/arrow_mesh.hpp"
#include "core/render/boat_mesh.hpp"
#include "core/render/minecart_mesh.hpp"
#include "core/render/entity_fire_mesh.hpp"
#include "core/render/fire_overlay.hpp"
#include "core/render/mob_mesh.hpp"
#include "core/render/spawner_mesh.hpp"
#include "core/render/sign_mesh.hpp"
#include "core/render/painting_mesh.hpp"
#include "core/render/particle_mesh.hpp"
#include "core/render/outline.hpp"
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

// **A breadcrumb straight onto the bottom screen.** `consoleInit` leaves that
// screen single-buffered, so this lands before the next instruction runs -- no
// GPU, no completed frame. It is the only report channel that survives a GPU
// that has stopped finishing command lists, which is what the geometry-shader
// cube format did on hardware. Used only on that path, and only where the very
// next call might not return. See the definition in renderer.cpp.
void geoTrace(const char* what);

// **C3D_FrameBegin with a deadline, for a caller that owns no Renderer.**
//
// `C3D_FRAME_SYNCDRAW` is a VBlank wait followed by an unbounded
// `gxCmdQueueWait`, and the main thread it blocks is also `aptMainLoop` -- so a
// command list the GPU never finishes takes the whole application with it, HOME
// included. Every such wait on the world's frame path is behind
// `Renderer::beginFrameOrGiveUp`. **The menu's was not**, and the menu is
// precisely where a wedged GPU is handed over: `Renderer::shutdown` drains
// first, but that drain has a deadline of its own and the case where it expires
// is exactly the case where the GPU is already gone.
//
// Same shape as the member: `C3D_FrameSync` for pacing, which always returns,
// then the queue drain polled under a deadline. **False means no frame was
// opened and the caller must draw nothing** -- not even `C3D_FrameEnd`.
bool beginFrameBounded(float seconds);

// The deadline `beginFrameBounded` is called with everywhere in this project.
// Long enough that no honest frame reaches it -- the slowest recorded is under
// 50 ms -- and short enough that a player is told rather than left holding a
// console that has to be powered off.
constexpr float kFrameWatchdogSeconds = 2.0f;

class Renderer {
public:
    struct Config {
        int meshDistance = 8;
        float fovDegrees = 70.0f;

        // The block atlas, already assembled by core/texture/ -- the menu
        // builds it when the player picks a pack, so a broken pack is reported
        // on the screen that chose it rather than silently in here. Null falls
        // back to the generated Dev Art, which is what the harnesses and any
        // caller that has no menu in front of it get.
        //
        // Borrowed, not owned. It has to outlive init(), which is all it has to
        // outlive: the image is uploaded to a C3D_Tex and never read again.
        const texture::AtlasImage* atlas = nullptr;
    };

    struct FrameStats {
        int drawCalls = 0;
        usize quads = 0;
        bool stereo = false;
        // Command words this frame put in the GPU command buffer, against the
        // kCommandBufferBytes / 4 it has. **This is a hard per-frame budget and
        // nothing can extend it mid-frame** -- see commandWordsFree in
        // renderer.cpp for why a split does not -- so it is the number that
        // says how close a view is to running out of room to be drawn.
        u32 commandWords = 0;

        // Sections the frame refused to draw because that budget was gone.
        // Non-zero means holes in the world, far ones first; it also means
        // kCommandBufferBytes is too small for the render distance in use.
        // **Zero is not luck**: below the ceiling this cannot fire at all.
        int droppedSections = 0;

        // Splits taken on purpose, to drain the pipeline around the
        // geometry-shader cube pass. Two per eye while the quad format is
        // live, zero otherwise. They cost command words like anything else,
        // which is why commandWords above is the number that bounds a frame
        // and this one is only ever a description of it.
        int geoSplits = 0;

        // Sections dropped from a draw because the pool slot the visible set
        // recorded had since changed hands. Small and non-zero is the guard
        // doing its job while chunks are being edited; large means the pool is
        // churning hard enough that the draw list is stale by the time it runs.
        int staleSkipped = 0;

        // Draws whose quad count was clipped to the shared index buffer's
        // capacity. Above zero means a section asked to draw more quads than
        // there are indices, which before the clamp read past the array and
        // fetched vertices from outside the bound buffer. It should be zero;
        // if it is not, the mesher is emitting past its own bound.
        int clampedDraws = 0;
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

    // **What time it is, for everything that is not a block.** The lightmap
    // above takes the day as an integer 0..11; the sky takes it as three
    // colours and an angle, and they are not the same function of it -- the
    // ground steps down eleven times while the sky fades smoothly, which is
    // what a1.1.2 looks like.
    //
    // Called once a frame beside `setSkyDarken`, with the same day-relative
    // tick count and the frame's partial. Everything it computes lands in
    // members the eyes read: the clear colour, the fog constant the world's
    // third combiner stage fades to, the two sky plane colours, how bright the
    // stars are and where the sun is.
    void setWorldTime(i64 dayTicks, float partialTicks);

    // **Fire, moving.** One 16 x 16 tile replaced in the block atlas -- see
    // `Atlas::updateTile` for why that is two 512-byte copies and not a
    // re-upload. Called from the frame loop beside `setSkyDarken` and for the
    // same reason: both write a texture the draw is about to sample, so both
    // belong before it rather than inside it.
    bool setAtlasTile(int tile, const u8* texels, int across = 1)
    {
        return atlas_.updateTile(tile, texels, across);
    }

    // **The compass, moving**, on the other sheet. A compass lying on the
    // ground is a `gui/items.png` sprite drawn by the detail pass, so it wants
    // the same treatment the fire tiles get -- and the items sheet is in
    // ordinary linear memory, so the two runs are a memcpy rather than a GPU
    // copy. See core/texture/compass_fx.hpp.
    bool setItemsTile(int tile, const u8* texels) { return atlas_.updateItemsTile(tile, texels); }

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

    // Swap the block atlas without tearing the renderer down, which is what
    // lets the pause menu's Texture Pack row apply to the world the player is
    // standing in.
    //
    // Nothing else has to be rebuilt: a pack changes what the tiles look like
    // and not where they are, so every UV already in the VBO pool still points
    // at the right tile. The image is only read here -- it is uploaded to a
    // C3D_Tex and never looked at again -- so the caller may let it go the
    // moment this returns, unlike Config::atlas.
    //
    // False means the upload failed and Dev Art was put up in its place; the
    // world stays textured either way, which is the whole reason this does not
    // simply leave the atlas deleted.
    bool setAtlas(const texture::AtlasImage& image);

    // **The pack's bitmap font, for sign text.** Separate from `setAtlas`
    // because the font is a different file with a different failure: a pack
    // with no `default.png` is a pack, and its signs show a blank board.
    bool setFont(const texture::FontImage& font) { return atlas_.initFont(font); }

    // **The pack's `particles.png`.** Separate from `setAtlas` for the reason
    // the font is, and unlike the font it never has nothing to upload: the
    // builder falls back to a generated stand-in, so false here means the
    // console had no memory for a 64 KB texture.
    bool setParticleSheet(const std::vector<u8>& sheet)
    {
        return atlas_.initParticles(sheet);
    }

    // **Call this after anything else has *taken* the top screen.**
    //
    // **Nothing does today, and that is a deliberate state rather than an
    // oversight.** The pause menu used to: it created a screen target of its
    // own and called gfxSet3D(false), and this was what put both back. It now
    // draws into this renderer's own frames instead -- see the overlay hook on
    // drawFrame -- so it takes neither, and there is nothing to give back.
    // This is kept because the next thing that wants the top screen to itself
    // will need it, and because the two paragraphs below are the reason a
    // resumed world used to freeze on its last frame forever. They were
    // expensive to find.
    //
    // Two things are taken and neither is given back:
    //
    //   * **The screen output itself, which is the one that would go unnoticed
    //     in review and be obvious on hardware.** citro3d holds exactly *one*
    //     target per output -- `linkedTarget[3]`, indexed top-left, top-right,
    //     bottom -- so `C3D_CreateScreenTarget(GFX_TOP, GFX_LEFT)` does not
    //     join a list, it evicts whatever was there and clears that target's
    //     `linked` flag. Deleting it then leaves the slot **null** rather than
    //     restoring what it displaced (disassembled out of
    //     `renderqueue.o`: `C3D_RenderTargetDelete` stores 0 into
    //     `linkedTarget[id]`). So after a menu has come and gone, the left eye
    //     is still drawn, still complete, and never transferred -- the top
    //     screen simply holds the last frame from before the pause, forever.
    //     This never bit the main menu because the Renderer is built and torn
    //     down around it and `init` re-links both eyes.
    //   * **gfxSet3D.** drawFrame only calls it when the slider crosses zero,
    //     because the call is not free and the answer changes a few times a
    //     session. That cache is a lie the moment someone else sets it, and the
    //     menu does on its way in, since nothing it draws has any depth.
    //     Without this, resuming with the slider up leaves the second eye off
    //     until the player happens to move the slider.
    void reclaimScreen();

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

    // Throws every mesh away so the streamer builds them all again: what a
    // change of greedy meshing needs. Unlike setCubeFormat nothing resident is
    // unreadable -- a flat mesh and a merged one draw the same way -- but a
    // pool half of each would make the setting's A/B read half of each.
    void remesh() { rebuildChunks(); }
    mesh::CubeFormat cubeFormat() const { return cubeFormat_; }

    // Stereo strength, as the on-screen disparity in pixels that a point at
    // infinity gets at full slider, and the distance that sits at the screen
    // plane. Adjustable at runtime because these are the two numbers no amount
    // of host testing can choose -- see the derivation in renderer.cpp.
    void setStereo(float disparityPixels, float focalBlocks);
    float stereoDisparityPixels() const { return disparityPixels_; }
    float stereoFocalBlocks() const { return focalBlocks_; }

    // Clears both eyes, draws the world, ends the frame.
    //
    // `overlay` is called once per eye with the frame still open and that eye's
    // target already carrying the world, which is how the pause menu draws over
    // a world instead of over a backdrop of its own. **Per eye rather than
    // once**: at any slider setting above zero there are two targets, and 2D
    // drawn on one of them is 2D half the player can see. The frame-level GPU
    // state is re-applied at the top of every eye precisely so that a 2D pass
    // between them cannot leave the next one drawing through citro2d's depth
    // test and culling.
    //
    // The context pointer is the same idiom io::FileSystem::listDirectory uses,
    // and for the same reason: -fno-exceptions, -fno-rtti, and no appetite for
    // a std::function's allocation in a frame.
    // **What the crosshair is on, or nothing.** Set once a frame from the ray
    // trace; the box is in world coordinates and this makes it chunk-relative
    // at draw time, because the camera's chunk is the origin every other matrix
    // in the frame is built around.
    //
    // The outline is drawn as triangles -- the PICA200 has no line primitive,
    // which is a deviation from a1.1.2's one-pixel `GL_LINE_STRIP` and the only
    // option available. See core/render/outline.hpp.
    void setSelection(const AABB& worldBox);

    // **The crack over the block Survival is breaking** -- the block, where it
    // is and which of the ten stages to draw. Set every frame there is progress
    // and cleared every frame there is not. See core/render/break_overlay.hpp.
    void setBreakOverlay(mc::block::BlockId block, u8 metadata, i32 x, int y, i32 z, int stage)
    {
        breakBlock_ = block;
        breakMeta_ = metadata;
        breakX_ = x;
        breakY_ = y;
        breakZ_ = z;
        breakStage_ = stage;
        breakVisible_ = true;
    }
    void clearBreakOverlay() { breakVisible_ = false; }

    // **What the particles are this frame**, handed over once and drawn inside
    // the world passes where they belong.
    //
    // The pointer is borrowed for the frame and may be null, which is what a
    // world with nothing broken in it looks like. `camera` is the billboard
    // basis -- see core/render/particle_mesh.hpp -- and the eye is where the
    // quads are made relative to, because a particle is always within a few
    // blocks of it and a 16-bit position has no room for a world coordinate.
    void setParticles(const mc::entity::ParticleSystem* particles,
                      const mc::render::Billboard& camera,
                      double eyeX, double eyeY, double eyeZ, float partial);

    // **What is lying on the ground this frame**, on the same terms as the
    // particles: borrowed for the frame, null for a world with nothing dropped
    // in it, and made relative to the eye because a 16-bit detail position
    // reaches 32 blocks and no further.
    //
    // `viewYawDegrees` is `playerViewY` -- the sprites are turned to face it,
    // and the blocks are not, which is `RenderItem`'s own split.
    void setItemEntities(const mc::entity::ItemEntitySystem* items, float viewYawDegrees,
                         double eyeX, double eyeY, double eyeZ, float partial);

    // **What is on its way down this frame**, on the same terms again. It
    // shares the item entities' eye and partial -- both are set from the same
    // two lines of the frame loop -- so this takes only the pool.
    void setFallingBlocks(const mc::entity::FallingBlockSystem* blocks)
    {
        fallingBlocks_ = blocks;
    }

    // **What is counting down this frame**, on the same terms as the falling
    // blocks: it shares the item entities' eye and partial, so this takes only
    // the pool. Two passes come out of it -- see core/render/primed_tnt_mesh.hpp.
    void setPrimedTnt(const mc::entity::PrimedTntSystem* tnt)
    {
        primedTnt_ = tnt;
    }

    // **What is hanging on the walls**, on the same terms and sharing the same
    // eye. A painting does not move and does not interpolate, so unlike the two
    // above it needs neither a yaw nor a partial -- only the pool.
    void setPaintings(const mc::entity::PaintingSystem* paintings)
    {
        paintings_ = paintings;
    }

    // **What is in flight**, on the item pass's eye and partial. Unlike a
    // painting an arrow moves, so it interpolates.
    void setArrows(const mc::entity::ArrowSystem* arrows) { arrows_ = arrows; }

    // **What is floating**, on the same eye and partial. A boat is the first
    // thing here drawn from a box model -- see core/render/box_model.hpp.
    void setBoats(const mc::entity::BoatSystem* boats) { boats_ = boats; }

    // **What is grazing.** Sixteen animals in range, posed and rebuilt every
    // frame like every other entity pass -- see core/render/mob_mesh.hpp, which
    // also explains why eleven texture pages are still one bind.
    void setMobs(const mc::entity::MobSystem* mobs) { mobs_ = mobs; }

    // The mob spawner blocks, whose contents ride the mob pass. Null is a build
    // with no spawner store, which draws the cages and nothing inside them.
    void setSpawners(const mc::entity::MobSpawnerStore* spawners) { spawners_ = spawners; }

    // **The other people on a server.** Bipeds off the same entity sheet the
    // mobs use, appended to the same buffer and the same draw call -- see
    // core/render/remote_player_mesh.hpp. Null in single player.
    void setRemotePlayers(const mc::net::RemoteEntities* entities)
    {
        remotePlayers_ = entities;
    }

    // **What is on the rails.** Unlike every other entity pass this one needs
    // the world, because a cart is tilted along the *track* rather than along
    // its own motion -- see core/render/minecart_mesh.hpp.
    void setMinecarts(const mc::entity::MinecartSystem* carts, const mc::tick::TickWorld* world)
    {
        minecarts_ = carts;
        minecartWorld_ = world;
    }

    // **What is written on the walls.** Two passes rather than one: the board
    // comes off the entity sheet and the text off the pack's font, and the
    // detail pipeline samples one texture at a time. `font` may be empty, which
    // is a blank board -- see core/render/sign_mesh.hpp.
    void setSigns(const mc::world::SignStore* signs, const mc::texture::FontImage* font)
    {
        signs_ = signs;
        signFont_ = font;
    }
    // **The lines in the bottom left of the top screen** -- a1.1.2's chat
    // overlay, which is where a spawn the heap refused is reported. Drawn last
    // in each eye, at the screen plane, off the pack's font; with no font
    // there is nothing to draw it with and it is skipped. See
    // core/gui/chat_log.hpp and core/render/chat_mesh.hpp.
    void setChat(const mc::gui::ChatLog* chat, const mc::texture::FontImage* font)
    {
        chat_ = chat;
        chatFont_ = font;
    }

    // **What is in the player's hand**, which is the one thing on the top
    // screen that is not the world. `equipped` and `swing` come off
    // `render::HeldItemState`; `light` is the `(sky << 4) | block` byte where
    // the player is standing. **Item 0 is an empty hand and draws the arm** --
    // see core/render/held_item.hpp.
    void setHeldItem(mc::item::ItemId item, float equipped, float swing, u8 light)
    {
        heldItem_ = item;
        heldEquipped_ = equipped;
        heldSwing_ = swing;
        heldLight_ = light;
        heldVisible_ = true;
    }

    // **No hand at all**, which is Spectator: there is no body to hold anything
    // and no arm to show either. Distinct from `setHeldItem(0, ...)`, which is
    // an empty hand and does draw one.
    void clearHeldItem() { heldVisible_ = false; }

    // **Whether the flames are over the screen**: the player's fire counter is
    // running, which is `jh.b(F)V`'s whole test. Set every frame alongside the
    // hand. See core/render/fire_overlay.hpp.
    void setBurning(bool burning) { burning_ = burning; }

    // **The hearts, the armour row and the bubbles** -- Survival's half of
    // GuiIngame, over the world on the top screen. Set every frame a Survival
    // body is being drawn; `clearHud` for every other mode, which `lu` hides the
    // rows in too (`PlayerController.shouldDrawHUD`). See core/render/hud_mesh.hpp.
    void setHud(const mc::render::HudInput& hud)
    {
        hudInput_ = hud;
        hudVisible_ = true;
    }
    void clearHud() { hudVisible_ = false; }

    // **The bottom screen has the buttons.** True while X has focused it, which
    // is a mode the world view cannot otherwise show: a translucent grey band
    // with an arrowhead pointing down is drawn along the bottom of the top
    // screen for as long as this is set. See `drawFocusHint`, and
    // `Overlay::focused`, which is where the flag comes from.
    void setFocusHint(bool on) { focusHint_ = on; }

    void clearSelection() { hasSelection_ = false; }

    using Overlay2D = void (*)(void* context, C3D_RenderTarget* target);
    void drawFrame(const Camera& camera, void* overlayContext = nullptr,
                   Overlay2D overlay = nullptr);

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

    // **How many times the GPU failed to finish a list inside the watchdog's
    // deadline.** Zero on every healthy frame this project has ever recorded;
    // non-zero means the quad path wedged and the renderer stopped feeding it.
    // Read by the debug overlay, and by main.cpp, which drops the cube format
    // back to the one that is known to draw. See beginFrameOrGiveUp.
    u32 gpuStalls() const { return gpuStalls_; }
    bool quadDrawsStopped() const { return quadDrawsStopped_; }

    // What the frame that never came back had in it. Meaningless until
    // gpuStalls() is non-zero, and the whole point of the watchdog: the numbers
    // in here are the difference between "the quad path hangs" and "the quad
    // path hangs at N draws with M splits".
    const FrameStats& stalledStats() const { return stalledStats_; }

    // Which rung of the geometry-shader ramp was being held when the GPU
    // stopped. Null when the ramp is not running. See kGeoRamp in renderer.cpp.
    const char* geoRampName() const { return geoRampName_; }

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

    // **The sky, first in the eye and the only pass with a matrix of its own.**
    //
    // Two matrices, in fact, and the pair of them is the whole of the day's
    // motion: one with the camera at the origin for the two flat planes, and
    // that one turned about X by the celestial angle for the sun, the moon and
    // the stars. Neither carries the camera's position, because a1.1.2's sky
    // does not -- see core/render/sky.hpp.
    //
    // Its own far plane, too. The sun hangs a hundred blocks away and the
    // planes reach four hundred; the world's far plane is the render distance
    // plus a ring, which at six chunks would clip the sun out of the sky
    // entirely. Depth writes are off and the pass runs before anything else,
    // so a different depth range costs nothing.
    void drawSky(const Camera& camera, float iod);
    C3D_Mtx skyViewProjection(const Camera& camera, float iod) const;

    // C3D_FrameBegin, with a deadline while the quad format is live. Returns
    // false when the deadline expired and no frame was opened -- the caller
    // must then draw nothing at all. See the comment on the definition.
    bool beginFrameOrGiveUp();

    // The two limits drawPass applies to a quad draw: either the ramp's current
    // rung or the fixed constants, never a mix. Zero means no limit.
    int geoSectionLimit() const;
    int geoQuadLimit() const;
    bool geoCubePassOnly() const;
    bool geoSplitPasses() const;

    // Everything a frame sets once and every eye depends on: the texture binds,
    // the alpha test, the three combiner stages, and the cull/depth/blend the
    // cube pass starts from. Hoisted out of drawFrame so it can run again
    // between eyes, after an overlay has drawn 2D over the first one.
    void applyWorldState();

    // The three-stage combiner every atlas pass inherits: texture x vertex
    // colour, x lightmap, then the fade to the fog colour, with alpha routed
    // round all three because the vertex shader spends primary alpha on fog.
    // `drawMobs`'s spider-eye pass replaces all three for one draw and calls
    // this to put them back, so it lives in one place rather than two.
    void applyAtlasTexEnv();

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

        // Only the outline program declares this. -1 everywhere else, which is
        // what `C3D_FVUnifSet` is never called with.
        int uLocTint = -1;

        // The seam that keeps a greedy-merged quad from cracking against its
        // neighbours -- see core/mesh/vertex.hpp. Both cube programs declare
        // `seam`; only the 12-byte one needs the per-corner `seamDir` table,
        // because the geometry-shader path builds its corners from the basis.
        int uLocSeam = -1;
        int uLocSeamDir = -1;
    };

    bool buildPipeline(Pipeline* pipeline, const void* shbin, u32 shbinSize, bool detail);

    // One float3 attribute and nothing else, so it does not fit buildPipeline's
    // two packed formats.
    bool buildOutlinePipeline(const void* shbin, u32 shbinSize);

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
    Pipeline outlinePipeline_;

    // One allocation for the life of the renderer: 432 vertices of twelve bytes
    // is five kilobytes, rebuilt only when the crosshair moves off the block it
    // was on, and never in the middle of a frame.
    void* outlineVerts_ = nullptr;
    // The crosshair is submitted after the selection outline, while both draws
    // remain queued until FrameEnd.  It cannot borrow outlineVerts_: rewriting
    // that memory would also rewrite the already-recorded selection draw.
    void* crosshairVerts_ = nullptr;
    AABB selectionBox_{};
    bool hasSelection_ = false;
    bool outlineDirty_ = false;

    // The crack: the block it covers and where, rebuilt in each eye into one
    // linear allocation of `render::kBreakOverlayMaxVertices`.
    void* breakVerts_ = nullptr;
    mc::block::BlockId breakBlock_ = 0;
    u8 breakMeta_ = 0;
    i32 breakX_ = 0;
    int breakY_ = 0;
    i32 breakZ_ = 0;
    int breakStage_ = 0;
    bool breakVisible_ = false;

    void drawSelection(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);

    // `drawBlockBreaking`, right after the outline and before the crosshair.
    void drawBreakOverlay(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawCrosshair(const C3D_Mtx& viewProjection, const Camera& camera,
                       i32 originChunkX, i32 originChunkZ);

    // Drawn between the opaque and the translucent terrain passes, which is
    // where `EntityRenderer.renderWorld` puts `renderParticles`.
    //
    // **Three draws, because `EffectRenderer` keeps three lists and binds a
    // texture per list**: the sprites off `particles.png`, the digging flecks
    // off the block atlas, and the two breaking kinds off `gui/items.png`. The
    // three spans are built into disjoint parts of the one buffer *before* any
    // of them is drawn, for the reason `drawItemEntities` gives at length: a
    // draw call names an address the GPU does not read until the frame ends.
    void drawParticles(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);

    // **Two draws, because a1.1.2 loads two textures for this.** A block on the
    // ground samples terrain.png and everything else samples gui/items.png, and
    // the PICA takes one texture per draw -- so the pool is built once per
    // sheet and the atlas is rebound in between. Drawn beside the particles,
    // where `renderEntities` runs.
    void drawItemEntities(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawFallingBlocks(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawPrimedTnt(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    // The white overlay half of the pass above, which runs on the outline
    // pipeline and takes the matrix that pass has already folded.
    void drawPrimedTntFlash(const C3D_Mtx& mvp);
    void drawPaintings(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawArrows(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawBoats(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawMinecarts(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    // The chest or furnace a special cart carries, off the **block** atlas
    // rather than the entity sheet -- which is why it is a pass of its own.
    void drawMinecartBlocks(const C3D_Mtx& viewProjection, i32 originChunkX,
                            i32 originChunkZ);
    void drawMobs(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawEntityFire(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);
    void drawSigns(const C3D_Mtx& viewProjection, i32 originChunkX, i32 originChunkZ);

    // **The hand, last in the eye and in camera space.** It takes the eye's
    // interocular separation rather than a view-projection, because it builds
    // its own projection: the item is placed relative to the camera and never
    // sees where the camera is. See core/render/held_item.hpp.
    void drawHeldItem(float iod);

    // **The flames over the screen, straight after the hand**, in the same
    // camera space and under the same projection -- `renderOverlays` runs
    // inside `renderHand` in the original. See core/render/fire_overlay.hpp.
    void drawFireOverlay(float iod);

    // **The chat, after the hand**, where `GuiIngame` runs: the hand is drawn
    // before the whole GUI. Built once a frame by `buildChat`, since both eyes
    // draw the same lines at the same place, and drawn in each.
    void buildChat();
    void drawChat();

    // **The Survival rows, last of all**: at the screen plane, off
    // `gui/icons.png`, alpha-tested and unlit.
    void drawHud();
    void drawFocusHint();

    // Four vertices a quad, and the worst case is a stack of 21+ -- four
    // copies -- of a block, which is six faces. The pool has no cap, so no
    // buffer holds its worst case; this one is sixty-four items at one block
    // copy each, 24 KB, and past it `buildItemEntities` draws the **nearest**
    // (core/render/draw_budget.hpp). What a player sees at the ceiling is the
    // far edge of a carpet of drops not drawn.
    static constexpr int kMaxItemVertices = 64 * 24;
    // **Not every painting there could be.** The pool has no cap; the buffer
    // holds eight full-size pictures (24 KB) and `buildPaintings` then draws
    // the nearest -- the same rule every entity pass follows, and what a
    // player sees at the ceiling is the far end of a very long gallery not
    // drawn.
    static constexpr int kMaxPaintingVertices = 8 * mc::render::kPaintingMaxVertices;

    // 128 arrows in range at six quads each, 48 KB; past that, the nearest.
    static constexpr int kMaxArrowVertices = mc::render::kArrowMaxVertices;

    // Thirty-two boats in range at five boxes each, 60 KB; past that, the
    // nearest.
    static constexpr int kMaxBoatVertices = mc::render::kBoatMaxVertices;

    // Thirty-two carts in range at six boxes each, 72 KB; past that, the
    // nearest.
    static constexpr int kMaxMinecartVertices = mc::render::kMinecartMaxVertices;
    static constexpr int kMaxMinecartBlockVertices = mc::render::kMinecartBlockMaxVertices;

    // Twenty-four mobs in range at up to twelve boxes each -- a fleeced sheep is
    // two models -- which is 6,912 vertices and 108 KB; past that, the nearest.
    //
    // **The mob spawners' miniatures share it**, and share the bind and the
    // draw call with them: they are the same models out of the same sheet, and
    // the only thing that differs is the transform. Four more models is 1,152
    // vertices and 18 KB. See core/render/spawner_mesh.hpp.
    static constexpr int kMaxMobVertices =
        mc::render::kMobMaxVertices + mc::render::kSpawnerMaxVertices;

    // Thirty-two burning entities at once -- the frame's twenty-four mobs plus
    // the carts, boats and stacks a fire in a storeroom lights -- at four
    // sheets each, which is one more than anything in a1.1.2 can need. 16 KB,
    // one buffer and one draw for all of them. See
    // core/render/entity_fire_mesh.hpp.
    static constexpr int kMaxEntityFireVertices = mc::render::kEntityFireMaxVertices;

    // **Sized for the text, which is much the larger half.** Sixty-four signs
    // of two boxes is 6,144 vertices; sixty-four signs of sixty glyphs is
    // 15,360. The buffer holds the worst of the two and both passes build into
    // it in turn, because they are drawn one after the other and neither
    // outlives the draw.
    static constexpr int kMaxSignVertices = mc::render::kSignMaxVertices;

    // Sixty-four falling blocks in range at six faces each, 24 KB; past that,
    // the nearest.
    static constexpr int kMaxFallingVertices = mc::render::kFallingBlockMaxVertices;

    // Ninety-six primed blocks in range at six faces each, 36 KB -- a chain
    // lights every cell in a wall at once, so this is larger than the falling
    // blocks' budget. The flash pass is separate and tiny: position-only
    // triangles for at most sixteen cubes, 6.8 KB.
    static constexpr int kMaxPrimedTntVertices = mc::render::kPrimedTntMaxVertices;
    static constexpr int kMaxPrimedTntFlashVertices = mc::render::kPrimedTntFlashMaxVertices;

    // One item, 66 quads at worst. 4 KB.
    static constexpr int kMaxHeldVertices = mc::render::kMaxHeldItemVertices;

    void* itemVerts_ = nullptr;
    const mc::entity::ItemEntitySystem* items_ = nullptr;
    void* fallingVerts_ = nullptr;
    const mc::entity::FallingBlockSystem* fallingBlocks_ = nullptr;
    void* tntVerts_ = nullptr;
    void* tntFlashVerts_ = nullptr;
    const mc::entity::PrimedTntSystem* primedTnt_ = nullptr;
    void* paintingVerts_ = nullptr;
    const mc::entity::PaintingSystem* paintings_ = nullptr;
    void* arrowVerts_ = nullptr;
    const mc::entity::ArrowSystem* arrows_ = nullptr;
    void* boatVerts_ = nullptr;
    const mc::entity::BoatSystem* boats_ = nullptr;
    void* minecartVerts_ = nullptr;
    void* minecartBlockVerts_ = nullptr;
    const mc::entity::MinecartSystem* minecarts_ = nullptr;
    void* mobVerts_ = nullptr;
    void* entityFireVerts_ = nullptr;
    const mc::entity::MobSystem* mobs_ = nullptr;
    const mc::entity::MobSpawnerStore* spawners_ = nullptr;
    const mc::net::RemoteEntities* remotePlayers_ = nullptr;
    const mc::tick::TickWorld* minecartWorld_ = nullptr;
    // `gq`'s render pass 0 and nothing else: one box per slime on screen.
    void* mobShellVerts_ = nullptr;
    // `ok`'s render pass 0: the spider's head again from the eye page, blended
    // at the alpha the lightmap works out. One box per spider on screen.
    void* mobEyeVerts_ = nullptr;
    void* signVerts_ = nullptr;
    const mc::world::SignStore* signs_ = nullptr;
    const mc::texture::FontImage* signFont_ = nullptr;
    void* heldVerts_ = nullptr;
    // Eight vertices, 128 bytes, written once at init: the overlay has no
    // inputs, and the flames move because the tiles they sample do.
    void* fireOverlayVerts_ = nullptr;
    int fireOverlayCount_ = 0;
    bool burning_ = false;
    // The glyphs, 64 KB (render::kChatMaxVertices), and the strips behind the
    // lines, six corners each. Built before the first eye and read by both.
    void* chatVerts_ = nullptr;
    void* chatStrips_ = nullptr;
    // Nine corners, written once at init: the focus band and its arrowhead.
    void* focusHintVerts_ = nullptr;
    // The sky: 70 KB written once at init and never touched again, because
    // every part of it that changes with the time of day is a uniform or a
    // combiner constant. See core/render/sky.hpp.
    void* skyVerts_ = nullptr;
    const mc::gui::ChatLog* chat_ = nullptr;
    const mc::texture::FontImage* chatFont_ = nullptr;
    mc::render::ChatSpan chatSpans_[mc::gui::kChatShownLines];
    int chatSpanCount_ = 0;
    // The HUD's quads: `render::kHudMaxVertices` of them, one linear
    // allocation for the life of the renderer, rebuilt in each eye.
    void* hudVerts_ = nullptr;
    mc::render::HudInput hudInput_{};
    bool hudVisible_ = false;
    bool focusHint_ = false;
    bool heldVisible_ = false;
    mc::item::ItemId heldItem_ = 0;
    float heldEquipped_ = 0.0f;
    float heldSwing_ = 0.0f;
    u8 heldLight_ = 0;
    float itemViewYaw_ = 0.0f;
    double itemEyeX_ = 0.0;
    double itemEyeY_ = 0.0;
    double itemEyeZ_ = 0.0;
    float itemPartial_ = 0.0f;

    static constexpr int kMaxParticleVertices = 512 * 4;

    // **A quarter of the section quad budget**, which is 512 particles -- what
    // the pool holds before it grows -- at four vertices each; past that the
    // nearest are drawn. One linear allocation for
    // the life of the renderer; the build writes into it every frame the pool
    // is not empty, which is the one place in the frame path that is allowed to
    // because it neither allocates nor touches the card.
    void* particleVerts_ = nullptr;
    const mc::entity::ParticleSystem* particles_ = nullptr;
    mc::render::Billboard particleCamera_{};
    double particleEyeX_ = 0.0;
    double particleEyeY_ = 0.0;
    double particleEyeZ_ = 0.0;
    float particlePartial_ = 0.0f;

    // Where quad.v.pica's faceBasis[18] lives, and the values to put in it.
    // Written on every bind rather than once at init for the same reason the
    // fog line is -- binding a program reloads its own constant table, and the
    // two programs' registers were allocated independently.
    int uLocFaceBasis_ = -1;

    mesh::CubeFormat cubeFormat_ = mesh::CubeFormat::Vertices;

    Atlas atlas_;
    Lightmap lightmap_;

    // What `setWorldTime` worked out, in the form each user of it wants: the
    // clear colour in the render target's RGBA8 and the three combiner
    // constants in the texenv's reversed 0xAABBGGRR, with floats for the two
    // that are not colours. The defaults are full daylight at eight chunks, so
    // a renderer whose owner never sets a time draws the sky this port drew
    // before there was one to set.
    u32 clearColour_ = 0xB3D1FFFF;
    u32 fogColour_ = 0xFFFFD1B3;
    u32 skyPlaneColour_ = 0xFFFFBB88;
    u32 voidPlaneColour_ = 0xFFB33025;
    float starBrightness_ = 0.0f;
    float celestialAngle_ = 0.0f;

    // One immutable index buffer for the whole process: 4 vertices per quad,
    // repeating 0,1,2, 0,2,3. Chunk meshes therefore carry no index data at all.
    u16* indices_ = nullptr;

    GpuVboAllocator allocator_;
    render::ChunkRenderer chunks_;

    u32 frame_ = 0;
    bool stereo_ = false;

    // **A geoshader draw has been recorded and not yet proven finished.**
    //
    // Set where `C3D_DrawArrays(GPU_GEOMETRY_PRIM, ...)` is recorded and
    // cleared only where the GX queue is proven empty -- a `C3D_FrameBegin`
    // that returned, or a `drainGpu` that did. It therefore stays true across
    // a format switch, which is the whole point: `cubeFormat_` describes the
    // frame about to be drawn and this describes the one still in the queue,
    // and it is the one in the queue that a wait can hang on. See
    // beginFrameOrGiveUp.
    bool geoWorkInFlight_ = false;

    // **How many times the watchdog fired, latched for the session**, and the
    // number the debug page reports. Never cleared: it is a history, not a
    // state.
    u32 gpuStalls_ = 0;

    // **The state, as distinct from the history: the watchdog fired and no
    // frame has completed since.** It is what picks the retry deadline, and
    // separating it from `gpuStalls_` is the fix for a real hang. The deadline
    // used to be chosen by `gpuStalls_ == 0`, so one stall put *every*
    // subsequent frame on the 32 ms retry deadline for the rest of the session
    // -- including after the cube format had been dropped back to the one that
    // is known to draw. At the Far Lands a healthy frame's queued work can
    // outlast 32 ms, so the watchdog then fired on merely-slow frames and
    // `drawFrame` returned without drawing, permanently: a frozen top screen
    // with a live HOME, which is not distinguishable from a dead console
    // without pressing it. A frame that completes proves the GPU came back, so
    // the next one gets the full deadline again.
    bool gpuWedged_ = false;

    // Quad draws are off because the GPU stalled under them. Read by main.cpp,
    // which drops the cube format back. **Cleared only by `setCubeFormat`** --
    // a deliberate act by the player or by that fallback, never by a run of
    // good frames, because a GPU that missed a deadline is not one to hand a
    // geoshader draw back to on the strength of luck.
    bool quadDrawsStopped_ = false;
    FrameStats previousStats_;
    FrameStats stalledStats_;

    // The ramp's position, and how long it has been there.
    int geoRampStep_ = 0;
    int geoRampHeld_ = 0;
    const char* geoRampName_ = nullptr;

    // How many geoshader draws the last cube pass put in the list. Zero means
    // there is nothing for the drain after it to drain.
    int geoDrawsLastCubePass_ = 0;

    // **Latched for the rest of the frame the moment the command buffer runs
    // out**, and cleared by drawFrame. Every pass after it draws nothing: the
    // budget is per frame and nothing gets it back, so a pass that carried on
    // would only be recording past the end of the buffer.
    bool commandBudgetSpent_ = false;

    // Space held back from section draws for the entity, selection and
    // crosshair passes that follow them in each eye.  Citro3d has no command
    // buffer bounds check, so this reserve is a memory-safety boundary.
    u32 commandTailReserve_ = 0;

    // Formatted once, at the stall, and then pointed at by the trace. A local
    // would be gone by the time anything read it.
    char stallLine_[64] = {};
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
