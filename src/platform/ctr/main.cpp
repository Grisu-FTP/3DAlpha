// The 3DS entry point.
//
// M2's on-device renderer: open a world off the SD card, stream columns in
// around a free-flying camera, mesh what the visibility walk asks for, and draw
// it. There is no gameplay here yet -- no physics, no block placement, no
// inventory -- because M2 is about proving the render path against real terrain
// at a real frame rate. M3 is where the camera gets a body.
//
// **The shell around it is the main menu.** `main` brings up the GPU once and
// then alternates: the menu chooses or makes a world, `runGame` plays it, Exit
// World on the pause menu comes back out to the menu, and Quit on the title
// screen is the only way the process ends. So `runGame` is now "given a world
// path, play it" and knows nothing about which world that is or how it was
// picked -- see menu.hpp.
//
// **START pauses rather than exits.** It used to break the loop outright, which
// meant the only way out of a world was also the only thing the button could
// ever do. It now hands the frame loop to `Menu::runPause` -- the same Menu
// object the shell already owns, so the Options and Texture Pack screens are
// literally the ones the main menu uses, and a change made in a world is a
// change the main menu is holding when the player gets back to it.
//
// The frame is the one docs/architecture.md lays out, minus the worker thread:
//
//     poll input
//     move the camera
//     build the visible set   (frustum + the visibility walk, once for both eyes)
//     stream and mesh, both budgeted
//     draw both eyes, skipping the second at slider zero
//     redraw the overlay
//
// Meshing is still on the main thread. That is a deliberate stop: the budget
// that makes it survivable is measurable, and moving it to core1 without a
// frame time to measure against would be building on a guess.

#include "platform/ctr/audio.hpp"
#include "platform/ctr/heap.hpp"
#include "platform/ctr/menu.hpp"
#include "platform/ctr/overlay.hpp"
#include "platform/ctr/probe.hpp"
#include "platform/ctr/progress_screen.hpp"
#include "platform/ctr/renderer.hpp"
#include "core/audio/block_sound.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/entity/falling_block.hpp"
#include "core/gui/chat_log.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/world/sign_store.hpp"
#include "core/entity/painting.hpp"
#include "core/entity/particle.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/io/volume_info.hpp"
#include "core/block/collision.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/sprint_gesture.hpp"
#include "core/item/registry.hpp"
#include "core/tick/behaviour.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/texture/compass_fx.hpp"
#include "core/texture/texture_fx.hpp"
#include "core/tick/tick_timer.hpp"
#include "core/render/held_item.hpp"
#include "core/render/world_streamer.hpp"
#include "core/util/math_helper.hpp"
#include "core/util/memory.hpp"
#include "core/util/worker.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/daylight.hpp"

#include "version_config.hpp"

#include <3ds.h>
#include <citro3d.h>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {

using namespace mc;

constexpr float kPi = 3.14159265358979f;

// **The square the generation screen draws, in .bss rather than on the stack.**
//
// A 3DSX gets a 32 KB main-thread stack that nothing in the binary can enlarge,
// and this is one byte per column of a square that is two rings wider than the
// debug page's ceiling -- 2,601 of them. It is written once a frame while a
// world is being made and never read again, which is exactly what a static
// scratch buffer is for. See the same argument on `world` and `overlay` below.
constexpr int kMaxProgressRadius = ctr::kDebugMaxDistance + 1;
constexpr int kMaxProgressEdge = kMaxProgressRadius * 2 + 1;
gui::ChunkState gProgressCells[kMaxProgressEdge * kMaxProgressEdge];

// What the pause menu draws over, and the shape citro3d wants to be handed.
//
// The renderer owns the frame -- it opens it, draws the world on every eye and
// ends it -- and the menu is one callback inside that. Doing it the other way
// round is what the pause menu used to do, with a render target of its own, and
// it is why the world used to disappear behind a wall of dirt when a player
// paused. See ctr::PauseBackdrop.
struct PausedWorld {
    ctr::Renderer* renderer;
    const ctr::Camera* camera;
};

void drawPausedWorld(void* context, void* overlayContext,
                     void (*overlay)(void* overlayContext, C3D_RenderTarget* target))
{
    PausedWorld& paused = *static_cast<PausedWorld*>(context);
    paused.renderer->drawFrame(*paused.camera, overlayContext, overlay);
}

// The "Saving level.." screen, and what close() reports into it.
//
// **It used to be four lines of console text on the bottom screen while the top
// one held a frozen frame of the world.** The number was there, which was the
// hard part, but a still top screen for the length of a few hundred chunk
// writes is the thing a player reads as a crash. The bar is drawn over that
// same frozen world -- ProgressScreen borrows the renderer's frame the way the
// pause menu does -- so the screen is now visibly doing something.
//
// `owed` is 0 when there was nothing to write, and that is not a degenerate
// case: it is the answer to "did pressing START a moment ago already do this",
// and the answer is yes. The bar reads full and the line under it says so.
struct SaveScreen {
    ctr::ProgressScreen* screen;
    ctr::Renderer* renderer;
    const ctr::Camera* camera;
};

void drawSaveProgress(void* context, u32 written, u32 owed)
{
    SaveScreen& save = *static_cast<SaveScreen*>(context);
    save.screen->setCounts(written, owed);
    // One frame per call, and close() calls this about every 16 ms. The frame
    // blocks on VBlank inside drawFrame, so the drain is polled at the refresh
    // rate rather than at whatever the loop would otherwise spin at -- and the
    // writes themselves are on the I/O thread, so nothing here slows them.
    save.screen->present(*save.renderer, *save.camera);
}

// An analogue axis as -1..1, with the deadzone taken out.
//
// Both sticks rest a little off centre on most consoles, so anything under a
// tenth of full deflection is treated as no input at all. 156 is full
// deflection for the circle pad; the C-stick reports the same scale through
// ir:rst.
float axis(s16 raw)
{
    constexpr float kDeadzone = 0.1f;
    const float value = float(raw) / 156.0f;
    if (value > -kDeadzone && value < kDeadzone) {
        return 0.0f;
    }
    return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
}

// Free flight, which is now **Spectator's** movement rather than the only one.
// It stays exactly as honest as it was: no body, no collision, no gravity, and
// it is offered under a name that says so.
//
// Up and down are B and Y rather than R and L, because the shoulders are the
// two mouse buttons now. See the controls table in docs/status.md.
void flyCamera(ctr::Camera& camera, float dt, bool sprint)
{
    circlePosition pad;
    hidCircleRead(&pad);

    const float px = axis(pad.dx);
    const float pz = axis(pad.dy);

    const float speed = (sprint ? 40.0f : 12.0f) * dt;

    float dx, dy, dz;
    camera.look(&dx, &dy, &dz);

    // Walk on the horizontal plane, the way the original moves, rather than
    // along the look vector.
    const float len = std::sqrt(dx * dx + dz * dz);
    const float fx = len > 0.0001f ? dx / len : 0.0f;
    const float fz = len > 0.0001f ? dz / len : 1.0f;

    camera.x += (fx * pz - fz * px) * speed;
    camera.z += (fz * pz + fx * px) * speed;

    const u32 held = hidKeysHeld();
    if (held & KEY_B) {
        camera.y += speed;
    }
    if (held & KEY_Y) {
        camera.y -= speed;
    }

    camera.y = camera.y < 1.0 ? 1.0 : (camera.y > 254.0 ? 254.0 : camera.y);
}

// The circle pad and the two body buttons, as the original's heading inputs.
//
// **Strafe is negated.** `moveFlying` sends a positive strafe to +X at yaw 0,
// and yaw 0 faces +Z, so +X is the player's *left*; the circle pad's positive x
// is their right. One of the two has to flip and it is this one.
mc::entity::PlayerInput readBodyInput(const ctr::Camera& camera, u32 held)
{
    circlePosition pad;
    hidCircleRead(&pad);

    mc::entity::PlayerInput input;
    input.strafe = -axis(pad.dx);
    input.forward = axis(pad.dy);
    input.yawDegrees = camera.yaw * 180.0f / kPi;
    input.jump = (held & KEY_B) != 0;
    input.sneak = (held & KEY_Y) != 0;
    return input;
}

// The sprint gesture, fed the same stick the body is walking on.
//
// **Read once a frame and applied to every tick the frame owes**, which is the
// same treatment `readBodyInput` already gets: the gesture is frame-timed
// because a double tap is, and the acceleration it selects is per tick because
// the physics is. A frame owing three ticks sprints for all three.
//
// Sneaking cancels rather than merely suppressing, so letting go of Y does not
// drop the player straight back into a sprint they last asked for a minute ago.
bool readSprint(mc::entity::SprintGesture& gesture, const mc::entity::PlayerInput& input,
                float now, bool suppressed)
{
    if (suppressed || input.sneak) {
        gesture.cancel();
        return false;
    }
    return gesture.update(input.forward, now);
}

// Breaking and placing, on the two shoulder buttons: **L places and R breaks.**
//
// This is the established control mapping used by the game build.
//
// **Held repeats every five ticks, and that has nothing to do with hardness.**
// `Minecraft.runTick` gates both mouse buttons on
// `ticksRan - lastClickTick >= Timer.ticksPerSecond / 4`, and the timer is
// constructed with 20.0f -- so a quarter of a second, the same for breaking and
// for placing. Block hardness governs something else entirely: the *progress*
// of a break, accumulated per tick by the controller through
// `Block.getPlayerRelativeBlockHardness`. That is Survival's, and it is not
// what paces the repeat.
//
// Both paths go through `WorldStreamer::setBlock`, which is what makes the
// change redraw as well as save -- see the note on that method. Writing through
// `worldTick()` directly would be invisible until something else touched the
// section.
// **What the world can see of the entities in it**, which in a1.1.2 is one
// question asked by one block: `al.h` wants to know whether anything is
// standing in the quarter-block box above a pressure plate.
//
// The two pools live here rather than in core -- the body is a local and the
// dropped items are a `unique_ptr` beside it -- so this is where the answer
// has to come from. `TickWorld` takes it as a function pointer and a context,
// the same shape it reaches chunks through.
struct EntityScene {
    const mc::entity::PlayerBody* body = nullptr;
    const mc::entity::ItemEntitySystem* items = nullptr;

    // The same pool again, writable, and the world the drop lands in. Two
    // fields rather than casting the const away, because the two seams really
    // do ask different things: the plate wants to *read* what is standing on
    // it and a broken torch wants to *add* to the pool.
    mc::entity::ItemEntitySystem* mutableItems = nullptr;
    const mc::tick::TickWorld* world = nullptr;
    mc::entity::FallingBlockSystem* falling = nullptr;
};

bool anyEntityIn(void* ctx, const mc::AABB& box, mc::tick::EntityFilter filter)
{
    const EntityScene* scene = static_cast<const EntityScene*>(ctx);

    // **The player answers to all three.** `EntityPlayer` extends
    // `EntityLiving`, which is what `js.b` ("mobs") selects on, so a stone
    // plate is pressed by a player as well as by a mob -- and `js.c`
    // ("players") is the narrowest of the three and still includes them. There
    // is nothing in this build that is a mob and not the player, so the
    // filters differ only in what they *exclude*, below.
    if (scene->body != nullptr && scene->body->box.intersects(box)) {
        return true;
    }

    // **A dropped item is neither a mob nor a player**, which is the whole
    // observable difference between the two plates: a stack of dirt thrown on
    // to a wooden plate presses it and the same stack on a stone plate does
    // nothing.
    if (filter != mc::tick::EntityFilter::Everything || scene->items == nullptr) {
        return false;
    }
    for (int i = 0; i < scene->items->count(); ++i) {
        if ((*scene->items)[i].box.intersects(box)) {
            return true;
        }
    }
    return false;
}

// `cn.a(Lkh;)Z` -- spawnEntityInWorld, for the one entity a block behaviour
// spawns. The other half of the same seam: `TickWorld` knows how many of what
// to drop and where, and nothing in core owns the pool it goes into.
//
// **A full pool loses the drop and says nothing**, which is
// `ItemEntitySystem::spawn`'s own rule and is why nothing here checks. A
// refusal is counted there and shown on the debug page.
void spawnDroppedItem(void* ctx, double x, double y, double z, mc::u16 item, int count)
{
    EntityScene* scene = static_cast<EntityScene*>(ctx);
    if (scene->mutableItems == nullptr || scene->world == nullptr) {
        return;
    }
    scene->mutableItems->spawn(*scene->world, x, y, z, mc::item::ItemId(item), count, 0,
                               mc::entity::kBlockDropPickupDelay);
}

// `cn.a(DDDLjava/lang/String;FF)V` -- what a block behaviour asks the mixer
// for. The engine outlives the world, so the context is the engine itself
// rather than the scene.
void playTickSound(void* ctx, const char* key, double x, double y, double z, float volume,
                   float pitch)
{
    static_cast<mc::audio::SoundEngine*>(ctx)->playSoundAt(key, x, y, z, volume, pitch);
}

// `dh.h(Lcn;III)V`'s `new ff(...)`, refused when the pool is full -- and a
// refusal is not a lost block: `tick::fallingTick` falls through to the instant
// path, which puts the block where the entity would have put it. See
// core/entity/falling_block.hpp.
bool spawnFallingBlock(void* ctx, mc::i32 x, int y, mc::i32 z, mc::u16 block)
{
    EntityScene* scene = static_cast<EntityScene*>(ctx);
    if (scene->falling == nullptr || scene->world == nullptr) {
        return false;
    }
    return scene->falling->spawn(*scene->world, x, y, z, mc::block::BlockId(block));
}

// `cn.l(III)V` -- World.removeBlockTileEntity. Signs are the only tile entities
// with a store, so the context is the store itself.
void removeSign(void* ctx, mc::i32 x, int y, mc::i32 z)
{
    static_cast<mc::world::SignStore*>(ctx)->erase(x, y, z);
}

// **Where the player is told a spawn was refused**: the chat lines on the top
// screen, in Legacy Console Edition's words. `widths` is the pack font's
// advance table, which the wrap measures with -- null when there is no font,
// and then there is nothing to draw the line with either.
struct ChatSink {
    mc::gui::ChatLog* log = nullptr;
    const mc::u8* widths = nullptr;

    void limitReached(mc::item::LimitedEntity which) const
    {
        const char* message = mc::item::limitMessage(which);
        if (log != nullptr && message != nullptr) {
            log->post(widths, message);
        }
    }
};

void editBlocks(render::WorldStreamer& world, render::ChunkRenderer& chunks,
                const ctr::Camera& camera, const mc::entity::PlayerBody& body,
                ctr::Overlay& overlay, u32 down, u32 heldButtons, i64 nowTick,
                i64* lastEditTick, const mc::item::Effects& effects,
                mc::render::HeldItemState* hand, const ChatSink& chat)
{
    // An empty hotbar slot still breaks; it just has nothing to place. Checked
    // at the placement branch rather than here.
    // a1.1.2's own cadence: Timer.ticksPerSecond / 4, with ticksPerSecond 20.
    constexpr i64 kRepeatTicks = 5;

    const bool pressed = (down & (KEY_L | KEY_R)) != 0;
    const bool repeating = (heldButtons & (KEY_L | KEY_R)) != 0
                           && nowTick - *lastEditTick >= kRepeatTicks;
    if (!pressed && !repeating) {
        return;
    }
    *lastEditTick = nowTick;

    // A fresh press wins over a repeat, so tapping R while holding L breaks
    // rather than placing.
    const u32 acting = pressed ? down : heldButtons;
    mc::tick::TickWorld* tickWorld = world.worldTick();
    if (tickWorld == nullptr) {
        return;
    }

    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;
    camera.look(&dx, &dy, &dz);

    // The camera's y is the eye already -- it is the original's posY -- so this
    // is the ray the game casts, not one from the player's feet.
    const mc::entity::RayHit hit = mc::entity::rayTrace(
        *tickWorld, camera.x, camera.y, camera.z, double(dx), double(dy), double(dz));
    // `getMouseOver`'s second half: an entity nearer than that block, and
    // within three, is what the crosshair is on instead. See core/item/use.hpp.
    const mc::item::EntityTarget target = mc::item::pickEntity(
        effects.entities, camera.x, camera.y, camera.z, double(dx), double(dy), double(dz), hit);

    const mc::item::ItemId heldItem = overlay.inventory().selectedItem();

    if ((acting & KEY_R) != 0) {
        // **The arm swings whether or not the block gives**, which is
        // `Minecraft.clickMouse`'s own order: button 0 calls `swingItem`
        // before it looks at what is under the crosshair -- and before it
        // knows whether there is anything under it at all.
        hand->swing();
        // **An entity in the way is hit instead of the block behind it**, the
        // same precedence the right-click branch below already gives one. This
        // is the only way to break a boat, a cart or a painting, which is why
        // none of the three left anything on the ground before it existed. See
        // core/item/use.hpp.
        if (target.found()) {
            mc::item::attackEntity(*tickWorld, target, effects);
            return;
        }
        if (!hit.hit) {
            return;
        }
        // **Instant, and it always was.** Creative's "instant break" is not a
        // thing this had to add: block hardness governs the *progress* of a
        // break, accumulated per tick through
        // `Block.getPlayerRelativeBlockHardness`, and that mechanism is
        // Survival's and does not exist yet. So one press is one broken block
        // in every mode -- which is Creative's rule, and is a thing Survival
        // will have to take away rather than a thing Creative added.
        // The tile entity goes with the block inside the write, through
        // `TickWorld::removeTileEntity` -- see core/tick/behaviour.cpp.
        world.breakBlock(chunks, hit.x, hit.y, hit.z, effects);
        return;
    }
    if ((acting & KEY_L) == 0) {
        return;
    }

    // **An entity in the way is asked instead of the block**, which is
    // `Minecraft.clickMouse`'s own order: `objectMouseOver` is the entity, so
    // `interact` runs and the block behind it is never clicked -- even when
    // `interact` refuses, as a chest cart does here. A boat or a plain cart
    // takes the player aboard, and that ends the click.
    if (target.found() && mc::item::interactWithEntity(target, effects.entities)) {
        return;
    }

    // **Everything the right hand does, and it is not only placing.**
    // `PlayerController.onPlayerRightClick` asks the block first -- which is
    // what opens a door, flicks a lever and presses a button -- and only then
    // hands the click to the item. All of it is `item::rightClick`, in core and
    // under test; what is left here is the button and the renderer.
    //
    // **Nothing is spent.** There is no stack depletion in that path and no
    // code that would have done it; Survival is where it becomes a
    // subtraction. See docs/todo-m3.md step 4.
    // **Marked before either entry point runs**, so a click the heap turned
    // down can be told apart from one that simply had nowhere to go. See
    // item::RefusalMark.
    const mc::item::RefusalMark refusals = mc::item::markRefusals(effects.entities);

    if (hit.hit && !target.found()
        && world.rightClick(chunks, heldItem, hit, body.box, camera.yaw * 180.0f / kPi,
                            effects)) {
        // **Only when the click was taken**, which is the other half of
        // `clickMouse`'s split: `if (onPlayerRightClick(...)) swingItem()`.
        // Waving at a wall that refuses the block does not swing.
        hand->swing();
        // **The keyboard, which is the other half of "signs don't work".**
        // `ItemSign.onItemUse` ends with `displayGUIEditSign`, and a sign
        // placed with no way to type into it can never say anything. The store
        // hands the index back once and clears it, so this fires on the
        // placement and not on every click afterwards.
        //
        // **Outside the frame**, which the applet requires -- input is handled
        // at the top of the loop, well before C3D_FrameBegin.
        if (effects.entities.signs != nullptr) {
            const int placedSign = effects.entities.signs->takeJustPlaced();
            if (placedSign >= 0) {
                overlay.editSignViaKeyboard(effects.entities.signs, placedSign);
            }
        }
        return;
    }

    // **The second entry point, and it runs whether or not the crosshair found
    // anything.** `Minecraft.clickMouse` tries the block first and then hands
    // the same press to `Item.onItemRightClick`, which casts its own ray -- so
    // a bucket aimed at water works even though the crosshair's ray is not
    // allowed to see water at all, and that is the whole reason this is here
    // rather than folded into the call above. See core/item/use.hpp.
    const mc::item::ItemUse used = world.useItem(chunks, heldItem, camera.x, camera.y,
                                                 camera.z, double(dx), double(dy),
                                                 double(dz), effects);
    // Either entry point may have been the one refused -- a cart or a sign
    // goes through the first, a boat or an arrow through the second -- and
    // both have run by here whenever neither was taken.
    chat.limitReached(mc::item::refusedSince(effects.entities, refusals));
    // **`onItemRightClick` raises the item again and never swings the arm.**
    // Both are in `clickMouse`'s tail: it calls `ItemRenderer.resetEquippedProgress`
    // when the stack it got back is not the stack it handed in -- a bucket
    // becoming a water bucket -- and there is no `swingItem` on this path at
    // all.
    if (used.becomes != heldItem) {
        hand->reequip();
    }
    overlay.replaceHeldItem(used.becomes);
}

// Pitch is clamped just short of straight up and straight down, because
// Mtx_LookAt has no answer for a look vector parallel to its up vector.
void clampPitch(ctr::Camera& camera)
{
    constexpr float kLimit = kPi * 0.5f - 0.01f;
    camera.pitch = camera.pitch < -kLimit ? -kLimit
                                          : (camera.pitch > kLimit ? kLimit : camera.pitch);
}

// The bottom screen is the game's UI, and it is also the only pointing device
// an old 3DS has. Dragging on it looks around; a fresh touch does not, or every
// tap would snap the view.
//
// **`top` is which of the two owns this press**, and it is the overlay's answer
// rather than this function's: the Look page hands the pad below the tab strip
// to the camera, the map and the inventory keep their own presses, and the
// debug pages hand over the whole screen the way the bottom screen always did.
// -1 is "not yours". See Overlay::touchLookTop.
//
// Both axes follow the mouse convention: drag right, look right. Yaw increases
// clockwise -- Camera::look sends yaw 0 to +Z and positive yaw toward -X, which
// is south turning to west -- so dragging right *adds* to it. It used to
// subtract, which meant the view turned the opposite way from the finger.
void lookWithTouch(ctr::Camera& camera, bool* dragging, touchPosition* last, int top)
{
    const u32 held = hidKeysHeld();
    if (!(held & KEY_TOUCH) || top < 0) {
        *dragging = false;
        return;
    }

    touchPosition touch;
    hidTouchRead(&touch);

    // Checked when the drag starts and not afterwards, so a drag that began on
    // the pad may wander anywhere without stopping dead at the edge.
    if (!*dragging && int(touch.py) < top) {
        return;
    }

    if (*dragging) {
        constexpr float kSensitivity = 0.012f;
        camera.yaw += float(int(touch.px) - int(last->px)) * kSensitivity;
        camera.pitch += float(int(touch.py) - int(last->py)) * kSensitivity;
        clampPitch(camera);
    }

    *last = touch;
    *dragging = true;
}

// The New 3DS C-stick, which is the right way to look and the reason the touch
// screen can go back to being a screen.
//
// It arrives through ir:rst rather than hid -- the same service a Circle Pad
// Pro reports through, so an old 3DS with one attached gets this for free. Both
// axes are rate rather than position: the further it is pushed the faster the
// view turns, which is what a stick with a return spring wants.
void lookWithCstick(ctr::Camera& camera, float dt)
{
    circlePosition stick;
    hidCstickRead(&stick);

    // Radians a second at full deflection. About 100 degrees, which is a little
    // brisker than the original's default mouse sensitivity and reads as normal
    // on a stick this short.
    constexpr float kTurnRate = 1.8f;

    camera.yaw += axis(stick.dx) * kTurnRate * dt;
    // Push up, look up. Positive pitch looks down, so this subtracts.
    camera.pitch -= axis(stick.dy) * kTurnRate * dt;
    clampPitch(camera);
}

// Hold Y and use the d-pad to dial the stereo in. The strength and the focal
// distance are the two numbers docs/status.md has always said no amount of
// host testing can pick, and the first console run proved it: the defaults
// gave the whole far world 0.62 pixels of depth variation. Rather than guess
// again, they move here and the overlay reads them back, so the answer is a
// minute with the console rather than a rebuild per guess.
//
// Whatever settles, write it into renderer.cpp -- this is a way to find the
// numbers, not a settings menu.
void tuneStereo(ctr::Renderer& renderer)
{
    // **SELECT + d-pad**, not Y + d-pad: Y is sneak now. SELECT was already the
    // overlay's modifier and it claims only Y and X for the page cycle, so the
    // d-pad under it was free. The overlay's own d-pad handling returns early
    // while SELECT is held, so the two cannot both act on one press.
    const u32 held = hidKeysHeld();
    if (!(held & KEY_SELECT)) {
        return;
    }

    const u32 down = hidKeysDown();
    float disparity = renderer.stereoDisparityPixels();
    float focal = renderer.stereoFocalBlocks();

    // Repeat would make it impossible to land on a value, so this is one step
    // per press.
    if (down & KEY_DLEFT) {
        disparity -= 1.0f;
    }
    if (down & KEY_DRIGHT) {
        disparity += 1.0f;
    }
    // Focal distance matters most where the content is, so it steps
    // proportionally rather than by a fixed block.
    if (down & KEY_DDOWN) {
        focal *= 0.8f;
    }
    if (down & KEY_DUP) {
        focal *= 1.25f;
    }

    renderer.setStereo(disparity, focal);
}

// Set by spawnWorker so the frame after it can say which core generation
// actually got, rather than which one it asked for.
bool gWorkerOnCore2 = false;
bool gWorkerIsNew3DS = false;

// 64 KB, double devkitARM's pthread default, because worldgen recurses --
// WorldGenBigTree walks a branch at a time -- and the 3DS build's
// `-Werror=stack-usage=8192` bounds a single frame rather than the depth of a
// stack of them. It is a heap allocation against a 48 MB heap; the headroom is
// not worth being clever about.
constexpr size_t kWorkerStackBytes = 64 * 1024;

// 16 KB for the I/O thread. It inflates and deflates into heap buffers and
// recurses nowhere, so it needs a fraction of what worldgen does -- and there
// are two of these threads now rather than one.
constexpr size_t kIoStackBytes = 16 * 1024;

// 32 KB for the audio decoder. Twice the I/O thread's, because Tremor's inverse
// MDCT is not a shallow call and the 3DS build's `-Werror=stack-usage=8192`
// bounds a single frame rather than a stack of them -- and half the generation
// worker's, because it does not recurse.
constexpr size_t kAudioStackBytes = 32 * 1024;

void* spawnWorker(void (*entry)(void*), void* arg, mc::WorkerRole role)
{
    // Read on the main thread, which is where this runs. The kernel refuses a
    // thread priority numerically below the process's own, so the main
    // thread's value is the highest this may ask for.
    s32 mainPriority = 0x30;
    svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);

    if (role == mc::WorkerRole::Audio) {
        // **Deadline-bound, and the only thread here that is.** A wave buffer
        // emptying is a deadline measured in tens of milliseconds; missing it
        // is audible immediately, where a late chunk is merely a late chunk.
        //
        // On a New 3DS it goes to core 2 and sits one step *above* the
        // generation worker already there, because a 40 ms population pass
        // would otherwise starve it and core 2 has nobody else to be polite to.
        // The generation worker loses a few percent of a core it is not
        // frame-coupled to.
        if (gWorkerIsNew3DS) {
            // **Core 2 at the main thread's own priority, sharing with the
            // generation worker** -- deliberately *not* a step above it.
            //
            // Preempting generation was the first instinct, and it is the wrong
            // trade. The kernel refuses a priority numerically below what the
            // process was granted, and a refused `threadCreate` here is silent:
            // it would fall through to the Old 3DS path and put the decoder on
            // core 0 on a console that has a spare core, with nothing to say so.
            // Paying that risk buys almost nothing, because the decoder does not
            // need to win a race it is not in -- it needs ~15% of a core against
            // a third of a second of buffer, and at equal priority the scheduler
            // round-robins it against generation, which is far more than enough.
            //
            // This is also the one priority known to work on this hardware:
            // the generation worker has used exactly it on core 2 since M4.
            // **Depth is what covers the deadline here, not priority.**
            Thread thread =
                threadCreate(entry, arg, kAudioStackBytes, mainPriority, 2, false);
            if (thread != nullptr) {
                return thread;
            }
            // A New 3DS whose exheader did not grant core 2 falls through to
            // the Old 3DS policy, exactly as generation does.
        }

        // **Old 3DS: core 0, one step below the main thread**, which is the I/O
        // thread's slot and for a related reason -- it runs in the slack the
        // main thread leaves while blocked on VBlank, so it cannot cost a
        // frame. A 3DSX has no other core to move it to, so CONTRIBUTING's
        // "no decompression on core 0" is met in substance rather than
        // literally: never on the main thread, one buffer per wake, and a ring
        // deep enough that a missed frame is inaudible. See ctr/audio.hpp.
        const s32 priority = mainPriority + 1 > 0x3F ? 0x3F : mainPriority + 1;
        return threadCreate(entry, arg, kAudioStackBytes, priority, 0, false);
    }

    if (role == mc::WorkerRole::Io) {
        // **Core 0, one step below the main thread, and that is the right
        // answer on both consoles.**
        //
        // This thread spends nearly all its life blocked in an IPC round trip
        // to the FS sysmodule, so giving it a core of its own would waste one
        // -- and on a New 3DS core 2 is already the generation worker's, which
        // genuinely needs all of it. At a numerically larger priority than the
        // main thread it is preempted the instant the main thread is ready, so
        // it cannot cost a frame; and because the main thread blocks on VBlank
        // every frame, it runs in exactly that slack. SD reads therefore
        // overlap with the GPU, which is where they belong.
        //
        // One step rather than 0x3F so it is not sitting behind every other
        // low-priority thread in the process for the slack it is meant to use.
        const s32 priority = mainPriority + 1 > 0x3F ? 0x3F : mainPriority + 1;
        return threadCreate(entry, arg, kIoStackBytes, priority, 0, false);
    }

    if (gWorkerIsNew3DS) {
        // Core 2 has nothing else on it, so there is no one to be polite to:
        // the worker runs at the main thread's own priority and still costs the
        // render thread nothing.
        Thread thread = threadCreate(entry, arg, kWorkerStackBytes, mainPriority, 2, false);
        if (thread != nullptr) {
            gWorkerOnCore2 = true;
            return thread;
        }
        // Not fatal and worth falling through for: a New 3DS whose exheader did
        // not grant core 2 -- launched some way other than Luma's hb:ldr -- is
        // still a New 3DS, and core 0 still works.
    }

    // Core 0, sharing with the render thread, so priority is what keeps it out
    // of the way: on the 3DS a larger number is a *lower* priority and 0x3F is
    // the bottom of the range. That is also what devkitARM's pthread shim would
    // have chosen, and saying it here rather than inheriting it is the point --
    // the number is a decision, not a default.
    (void)mainPriority;
    return threadCreate(entry, arg, kWorkerStackBytes, 0x3F, 0, false);
}

void joinWorker(void* handle)
{
    Thread thread = static_cast<Thread>(handle);
    threadJoin(thread, U64_MAX);
    threadFree(thread);
}

// Plays one world, and returns when the player presses START. The GPU is
// already up and stays up: the menu the player comes back to needs it.
// `menu` is the shell's own, borrowed for the pause screen. It is not the
// thing that chose this world -- that was `choice`, which is a copy -- but it
// is where the render distance, the pack list and the live atlas live, and the
// pause menu edits all three.
int runGame(const ctr::MenuChoice& choice, ctr::Menu& menu, mc::audio::SoundEngine& sound,
            ctr::NdspBackend& audio, bool isNew3DS, bool haveCstick)
{
    ctr::Renderer::Config config;
    // What the options screen last settled on, which starts at the two
    // configurations the VBO pool was measured against -- distance 6 on an old
    // 3DS and 10 on a New one, both of which hold a full turn on the spot
    // without evicting anything.
    config.meshDistance = choice.renderDistance;
    // The menu already assembled and validated this, so a pack that will not
    // decode was refused on the screen that chose it. `choice` outlives the
    // game loop, which is what lets the renderer borrow the image rather than
    // copy 256 KB of it.
    config.atlas = &choice.atlas;

    ctr::Renderer renderer;
    if (!renderer.init(config, isNew3DS)) {
        std::printf("\x1b[31mrenderer init failed\x1b[0m\n");
        return 1;
    }

    // Static, not a local: WorldStreamer is 36 KB -- almost all of it the
    // mesher's 18^3 scratch -- and a 3DSX gets a 32 KB main-thread stack that
    // nothing in the binary can enlarge. As a local it overflowed the stack in
    // this function's prologue, before the first line of it ran. It lives for
    // the whole process either way, so .bss is where it belongs.
    static render::WorldStreamer world;

    // **The game makes world where there is none, and the harnesses do not.**
    // An Alpha world is unbounded -- walking west makes more of it -- so this
    // is the faithful setting and the reason M4 exists. `--mesh` and `--fly`
    // leave it off: they measure a fixed world, and a streamer that quietly
    // extended it would change the numbers and write chunk files into the copy
    // under measurement.
    world.setGenerateMissing(true);

    // **The worker gets core 2 on a New 3DS, and that is worth more than every
    // other generation tuning put together.**
    //
    // `std::thread` cannot say which core, and on this toolchain it does not
    // merely decline to: devkitARM's pthread shim calls
    // `threadCreate(func, arg, 32*1024, 0x3F, 0, false)` with the core
    // hardcoded. Every `std::thread` therefore lands on **core 0 at priority
    // 0x3F**, the bottom of the range, beside the render thread -- and the ARM11
    // scheduler is strictly priority-ordered, so the worker runs only in what
    // is left of a frame after the main thread blocks. On a console holding 30
    // fps that is a sliver, which is why generation could be outrun at walking
    // pace and never seen to catch up.
    //
    // A New 3DS has four cores: 0 for the application, 1 for the system, 2 free
    // to an application that asks, 3 reserved. Luma's synthesised 3DSX exheader
    // already grants core 2 -- `0xFF002109`, whose bit 13 is "Access core2" --
    // so nothing needs installing for this to work. `osSetSpeedupEnable(true)`
    // above has already put the whole MPCore at 804 MHz with L2 on, and core 2
    // has no one else on it, so the worker stops sharing and starts running.
    //
    // On an old 3DS there is no such core. The fallback is what the code did
    // before, said explicitly rather than inherited from the shim: core 0, the
    // bottom priority, living on the main thread's idle time.
    // **The model and the spawn hooks are installed in `main`, not here.** They
    // used to be set on this line, and that was a bug the moment something
    // other than a world wanted a thread: audio comes up in `runShell`, before
    // any world exists, so it found `workerSpawn()` still null and reported
    // itself unavailable on every console. Process-wide state belongs where the
    // process starts.
    //
    // `gWorkerOnCore2` stays: it is a diagnostic that `spawnWorker` fills in,
    // and it is per-world because the answer can differ between two worlds on
    // the same console -- a thread that failed to start on core 2 falls back.
    gWorkerOnCore2 = false;

    // How much newlib heap is left, for the one thing that can usefully spend
    // it: the cache's cap on world owed to the card. See heap.hpp.
    mc::setHeapFreeQuery(&ctr::heapFreeBytes);

    // **The chunk cache, which is what keeps the card off the render thread.**
    //
    // Reads, writes and existence checks all go through it and all of them
    // happen on its own thread; the retained ring and the read-ahead band are
    // the same table, so a column that leaves the grid is still there when the
    // player turns round. See core/world/chunk_cache.hpp.
    //
    // The cap is a fraction of the newlib heap rather than a fixed number: on a
    // New 3DS heap.cpp lands on 40 MB and block data at distance 12 is ~15 MB,
    // so 8 MB of retained columns is comfortable; on an Old 3DS the same split
    // gives ~21 MB and 2 MB is what is left over. A column is 18,013 bytes on
    // the real world, so 8 MB is ~465 of them -- more than the 264 that sit
    // between the load radius and the classification ring at distance 8.
    world::ChunkCache::Config cache;
    cache.cleanCapBytes = usize(choice.chunkCacheMB) << 20;

    // **What is owed to the card may use the heap that is going spare.** The
    // 4 MB floor is the number that has to hold at the longest render distance
    // with the generator's cache at full size; most of a session is nowhere
    // near that, and while it is not, a generation worker stopping to write a
    // chunk file itself is a stall bought for nothing. 16 MB is ~890 columns at
    // the measured 18,013-byte mean -- a ceiling rather than a target, and the
    // autosave timer still empties it long before it is reached.
    cache.dirtyCapMaxBytes = 16u << 20;
    cache.threaded = true;
    world.setCacheConfig(cache);
    world.setPrefetchRings(2);
    world.setAutosaveSeconds(choice.autosaveSeconds);

    if (!world.open(choice.worldPath.c_str(), config.meshDistance, ctr::nowMillis())) {
        // A world that will not open is not a reason to end the process: the
        // player picked it from a list and can pick another. The message goes
        // to the console under the menu that is about to come back up.
        std::printf("\x1b[31mcannot open %s\x1b[0m\n", choice.worldPath.c_str());
        renderer.shutdown();
        return 0;
    }

    // **The bound under the render distance**, sized from the heap that the
    // split in heap.cpp actually produced rather than from a constant beside
    // it -- the two consoles get very different figures and so does a launch
    // context that hands over less than either.
    //
    // Five eighths to the resident columns. The other three eighths are what
    // else lives in this heap and is not counted by `blockBytes`: the
    // generator's own column cache, which is `16 * loadRadius + 96` columns and
    // so about 11 MB at the debug page's ceiling; the chunk cache's clean and
    // dirty sides, the dirty one capped at 4 MB; decoded sound effects; the
    // map's patch cache; thread stacks and fragmentation.
    //
    // **Deliberately conservative, because the two failure modes are not
    // symmetric.** Too large is the crash this exists to stop; too small is a
    // shorter view at the Far Lands and nothing else. `blocks N.N MB in the
    // heap` and `free KB heap` on the Info page are the pair to tune it
    // against, with `admit` beside them saying whether it is biting at all.
    world.setMemoryBudget(mc::ctr::heapTotalBytes() / 8 * 5);

    // **The two things spawnPosition can mean, told apart.**
    //
    // With a player in level.dat it returns `Pos` verbatim, and a1.1.2's `Pos[1]`
    // is `posY`, which sits 1.62 above the feet because `yOffset` is 1.62 and
    // `setPosition` puts the box at `posY - yOffset`. The real world's
    // 70.62000000476837 is that 1.62 showing. With no player it returns
    // `spawnY`, which is a block coordinate and therefore the feet.
    //
    // Adding 1.62 to both -- which is what this did -- is right for a fresh
    // world and wrong for a saved one, and because the result was written
    // straight back out by setPlayerState it compounded: **a saved player rose
    // 1.62 blocks every time the world was opened and closed.** The body owns
    // the feet, and both the camera and Pos[1] are derived from it.
    double spawnX = 0.0;
    double spawnY = 0.0;
    double spawnZ = 0.0;
    world.spawnPosition(&spawnX, &spawnY, &spawnZ);
    const double feetY = world.level().player.present
                             ? spawnY - double(mc::entity::kEyeHeight)
                             : spawnY;

    mc::entity::PlayerBody body;
    body.setFeet(spawnX, feetY, spawnZ);

    // **The particle pool and the two things a click can set off.** Both are
    // stack objects with the lifetime of the world, which is what lets the core
    // side take borrowed pointers and a headless build pass neither. The seed
    // is a clock because the original's two particle generators are both
    // time-seeded and nothing about a spray of dirt is meant to repeat.
    //
    // **On the heap, and that is not a style choice.** The pool is 512
    // particles and a 3DSX main thread has 32 KB of stack that nothing in the
    // binary can enlarge -- `-Werror=stack-usage` caught it the moment it was a
    // local. One allocation when a world opens is what the streamer already
    // does; what the rule forbids is allocating on a *frame*, and this never
    // does.
    auto particles = std::make_unique<mc::entity::ParticleSystem>(i64(ctr::nowMillis()));

    // **What is hanging on the walls.** Thirty-two, on the heap for the same
    // stack reason the particles are, and time-seeded because the art a
    // painting comes out as is drawn at random from the ones that fit -- see
    // core/entity/painting.hpp. Declared before `effects` because the click
    // path reaches it through there.
    auto paintings = std::make_unique<mc::entity::PaintingSystem>(
        i64(ctr::nowMillis()) ^ 0x9a17);

    // **What is in flight.** Thirty-two arrows, time-seeded because the launch
    // scatter is `nextGaussian` in the original and nothing about a shot is
    // meant to repeat. See core/entity/arrow.hpp.
    auto arrows = std::make_unique<mc::entity::ArrowSystem>(i64(ctr::nowMillis()) ^ 0xa770);

    // **What is floating.** Eight boats, and the pool owns which one is being
    // ridden -- see core/entity/boat.hpp.
    auto boats = std::make_unique<mc::entity::BoatSystem>(i64(ctr::nowMillis()) ^ 0xb0a7);

    // **What is on the rails.** Eight carts; only the plain one can be ridden.
    auto minecarts =
        std::make_unique<mc::entity::MinecartSystem>(i64(ctr::nowMillis()) ^ 0xca27);

    // **What is written on the walls.** A tile entity rather than an entity --
    // see core/world/sign_store.hpp. Sign text still needs tile entity
    // persistence, separate from the entity snapshot below.
    auto signs = std::make_unique<mc::world::SignStore>();

    // **What the game says to the player**, bottom left of the top screen --
    // a1.1.2's chat lines, which is where a spawn the heap refused is
    // reported. Two kilobytes, on the heap for the stack reason below.
    auto chat = std::make_unique<mc::gui::ChatLog>();

    const mc::item::Effects effects{
        particles.get(), &sound,
        mc::item::EntityPools{paintings.get(), arrows.get(), boats.get(),
                              minecarts.get(), signs.get()}};

    // **What is lying on the ground.** Sixty-four entities, on the heap for the
    // same stack reason the particles are, and time-seeded for the same reason
    // too: `EntityItem`'s scatter comes off `Math.random()` in the original and
    // nothing about a thrown item is meant to repeat.
    auto droppedItems = std::make_unique<mc::entity::ItemEntitySystem>(
        i64(ctr::nowMillis()) ^ 0x5eed);
    // **What is on its way down.** Sixteen entities, no random of its own --
    // `EntityFallingSand`'s constructor draws nothing. See
    // core/entity/falling_block.hpp for why the pool is this small and what
    // happens when it is full.
    auto fallingBlocks = std::make_unique<mc::entity::FallingBlockSystem>();

    world.bindEntities(mc::entity::EntityPools{paintings.get(), arrows.get(), boats.get(),
        minecarts.get(), droppedItems.get(), fallingBlocks.get()});

    // `random.pop`'s pitch, which is `((r - r) * 0.7 + 1) * 2` per pickup. Its
    // own generator so that drawing a pitch cannot shift the scatter on the
    // next thing thrown.
    mc::JavaRandom pickupRand(i64(ctr::nowMillis()));

    // **The fire tiles, still running.** `applyAnimatedTiles` baked a settled
    // flame into the atlas when the pack loaded, which is what the bottom
    // screen and the map read; this is the same two simulations kept going so
    // the flame in the *world* moves. On the heap for the stack reason above --
    // two 16 x 20 float fields each is 7 KB, and a 3DSX main thread has 32.
    auto flames = std::make_unique<mc::texture::FlameAnimation>();

    // **The compass, which is a texture and not an item.** See
    // core/texture/compass_fx.hpp: a1.1.2's item 345 is a plain `di` with no
    // behaviour at all, and the whole of a working compass is `aa` rewriting
    // one 16 x 16 tile of gui/items.png every tick with a needle aimed at the
    // world's spawn. "The compass doesn't work" was that FX never being ported.
    //
    // Two consumers, because the same tile is read two ways: the bottom screen
    // rasterises it out of the pack's decoded pixels, and a compass lying on
    // the ground is a sprite the GPU samples out of the uploaded items sheet.
    // Neither reads the other's copy, so both are pushed.
    auto compass = std::make_unique<mc::texture::CompassTexture>();
    const int compassTile = mc::texture::compassTile();
    // The pack's own compass face, which the needle is drawn over each tick.
    // Re-seeded whenever the pack changes; see the atlasChanged branch below.
    auto seedCompass = [&compass, compassTile](const mc::texture::AtlasImage& atlas) {
        if (compassTile < 0 || !atlas.hasItems()) {
            return;
        }
        const int column = compassTile % mc::texture::kAtlasTilesPerEdge;
        const int row = compassTile / mc::texture::kAtlasTilesPerEdge;
        mc::u8 tile[16 * 16 * 4];
        for (int y = 0; y < 16; ++y) {
            const mc::usize src = (mc::usize(row * 16 + y) * mc::texture::kAtlasEdge
                                   + mc::usize(column * 16)) * 4;
            std::memcpy(tile + mc::usize(y) * 16 * 4, atlas.itemsRgba.data() + src, 16 * 4);
        }
        compass->setBase(tile);
    };
    seedCompass(choice.atlas);

    // The world can ask about both of them from here on. `tick_` is built when
    // the world opens and torn down when it closes, neither of which happens
    // inside this loop, so this is set once.
    EntityScene scene{&body,           droppedItems.get(), droppedItems.get(),
                      world.worldTick(), fallingBlocks.get()};
    if (mc::tick::TickWorld* entityWorld = world.worldTick()) {
        entityWorld->setEntityQuery(anyEntityIn, &scene);
        // **And what a block leaves behind when it falls off a wall.** Without
        // this every `dropBlockAsItem` in core is a draw from the world's random
        // and nothing else, which is what it was before there was a pool to put
        // the answer in. See core/tick/drop.hpp.
        entityWorld->setDropSink(spawnDroppedItem, &scene);
        // **And the sand that is falling rather than teleporting.** Setting
        // this is what turns `BlockSand.fallInstantly` off for the parts of the
        // world a player is watching; nothing else in the process sets it, so
        // world generation and the headless tools keep the instant path. See
        // core/entity/falling_block.hpp.
        entityWorld->setFallingBlockSink(spawnFallingBlock, &scene);
        // **And the click a plate makes.** A pressure plate is flush with the
        // floor and its whole state is one bit of metadata, so without this the
        // only way to know it had armed was to look at what it was wired to --
        // which is why it was reported as feeling unresponsive rather than as
        // being silent. The lever, the button and the door were in the same
        // position. See core/tick/tick_world.hpp.
        entityWorld->setSoundSink(playTickSound, &sound);
        // **And the text that goes with a sign.** Every removal of a sign
        // block reaches this -- the player's break, and a sign dropped because
        // the block it stood or hung on went. See core/tick/tick_world.hpp.
        entityWorld->setTileEntityRemovedSink(removeSign, signs.get());
    }

    // What the break/place repeat is paced against. Its own count rather than
    // the world clock, which the pause menu stops: holding R through a pause
    // should not bank a hundred placements.
    i64 editTick = 0;
    i64 lastEditTick = -1000;

    // **What the hand is showing, which is not always what the hotbar
    // selects.** `ItemRenderer`'s equip animation and the arm swing, both on
    // the 20 Hz clock -- see core/render/held_item.hpp. Per world rather than
    // per process: entering a world should raise the first item rather than
    // inherit the last one's progress.
    mc::render::HeldItemState hand;

    // **The renderer outlives the world and this state does not.** Its held
    // item is whatever the last world left, and the generation loop below draws
    // frames before anything sets it -- so a second world would open with one
    // frame of the first one's hand over the terrain being made.
    renderer.clearHeldItem();

    // **Creative flight, and its double tap.** Off at world entry, every time:
    // it is a state the player asked for with a gesture and there is nowhere to
    // save it that would not be inventing a `level.dat` key. The tap window is
    // frames rather than ticks because it is a gesture and not physics -- a
    // fifth of a second is what a double click is everywhere else on this
    // console.
    bool flying = false;
    float lastJumpTap = -1.0f;
    float sinceStart = 0.0f;
    // Off at world entry for the same reason flight is: it is a gesture, not a
    // saved state. See core/entity/sprint_gesture.hpp for why it is not
    // a1.1.2's at all.
    mc::entity::SprintGesture sprintGesture;
    if (world.level().player.present) {
        body.motionX = world.level().player.motion[0];
        body.motionY = world.level().player.motion[1];
        body.motionZ = world.level().player.motion[2];
        body.onGround = world.level().player.onGround;
        body.fallDistance = world.level().player.fallDistance;
    }

    ctr::Camera camera;
    camera.x = body.x;
    camera.y = body.eyeY();
    camera.z = body.z;
    if (world.level().player.present) {
        camera.yaw = world.level().player.rotation[0] * kPi / 180.0f;
        camera.pitch = world.level().player.rotation[1] * kPi / 180.0f;
    }

    // **The world's own clock, absolute rather than a time of day.**
    //
    // level.dat's `Time` is a running tick count and the day is `Time % 24000`,
    // so this used to normalise it to 0..1 on the way in and had nothing left
    // to write back -- which was fine while nothing wrote it back. Now that the
    // autosave does, the remainder is the wrong thing to keep: saving it would
    // reset the world to day zero every time.
    //
    // **The world clock is the tick system's now, not this loop's.** It used
    // to be a double advanced by `dt * 20` here, which was right for the sky
    // and wrong for everything else: a block update has to happen a whole
    // number of times or not at all, and at 30 fps `0.667` of a grass tick is
    // not a thing that can be run. `TickTimer` is a1.1.2's own accumulator and
    // hands out whole ticks plus the fraction the sky still wants.
    mc::tick::TickTimer tickTimer;

    // **Static for the same reason `world` above is**, and now with a second
    // reason: the overlay carries the spectator screen's map, whose colour
    // table alone is a few kilobytes, and a 3DSX gets a 32 KB main-thread stack
    // that nothing in the binary can enlarge. It lives for the whole process
    // either way. `begin()` below is what makes it forget the last world.
    static ctr::Overlay overlay;
    // The name, not the path: the header is 40 columns wide and the player knows
    // which card their worlds are on. `choice` outlives this call, which is why
    // the overlay may keep the pointer.
    overlay.begin(choice.worldName.c_str(), isNew3DS ? "New 3DS" : "Old 3DS");
    overlay.setAudio(&audio);
    // **The bottom screen is the world's gamemode's**, which is why this is read
    // off the choice rather than assumed: Spectator gets the map and no hotbar,
    // Creative gets the palette page as well, and Survival gets the inventory
    // frame without one.
    overlay.setGamemode(choice.gamemode);
    // **What the world says the player is carrying.** After setGamemode,
    // because a mode with no hotbar still loads it -- Spectator must not empty
    // a hand the real client filled -- and after begin(), which clears it.
    overlay.setInventory(world.level().player.inventory);
    overlay.configureMap(isNew3DS);
    // The same atlas the world is drawn with, for the map's colours and for the
    // block icons in the hotbar and the palette. `choice` outlives the game
    // loop, which is what lets the overlay borrow its pixels rather than hold a
    // second copy of a quarter of a megabyte.
    overlay.setAtlas(choice.atlas);
    // ...and the same pack's font, which is what sign text is drawn with.
    renderer.setFont(menu.fontImage());
    // ...and the same dirt the menu draws its own backdrop with, behind the
    // panels on the bottom screen. Copied out here too, in the format the
    // framebuffer wants.
    overlay.setBackdropTile(menu.backgroundTile());
    // After open(), because open() is where the worker is started and therefore
    // where the answer becomes known. Whether it started at all is a separate
    // question and the overlay reads that from the streamer's own stats.
    overlay.setWorkerCore(gWorkerOnCore2 ? "core2" : "core0");

    ctr::DebugSettings settings;
    settings.renderDistance = config.meshDistance;
    settings.minDistance = 2;
    // The debug page gets the hardware's ceiling, not the player's. The 8 and
    // 12 in ctr::kPlayMaxDistance* are what the M3 settings menu will offer;
    // they are a judgement about where a 3DS stops giving anything back, and
    // they are not the model's business to enforce on a maintainer who wants
    // to see what distance 20 does. Both are the same on an old 3DS and a New
    // one here, because what bounds this page is the heap rather than the pool
    // -- the pool evicts and carries on either way.
    settings.maxDistance = ctr::kDebugMaxDistance;

    // **The boot cube format, applied once, before the first frame.** The
    // renderer and the streamer both start in the 4-vertex encoding, and the
    // settings page only reaches this code when the player changes something --
    // so a default of "geoshader" that was never applied would show one thing
    // on the page and draw another. Cheap here and nowhere else: the pool is
    // empty and the streamer has published nothing, so the re-mesh both calls
    // would otherwise force has no work to do.
    {
        const mesh::CubeFormat startFormat = settings.geometryQuads
                                                 ? mesh::CubeFormat::Quads
                                                 : mesh::CubeFormat::Vertices;
        renderer.setCubeFormat(startFormat);
        world.setCubeFormat(startFormat, renderer.chunks());
        // Greedy meshing the same way, and for the same reason: it has to be
        // what the page says before the first section is meshed.
        world.setGreedy(settings.greedyMeshing, renderer.chunks());
    }

    render::WorldStreamer::Budget budget;
    budget.columnsPerFrame = isNew3DS ? 2 : 1;

    // Only used if the worker thread could not be started; with it running the
    // main thread posts a job and moves on. See WorldStreamer::Budget.
    budget.generatedPerFrame = 1;

    bool dragging = false;
    touchPosition lastTouch{};
    u64 lastTick = svcGetSystemTick();

    // The pool's eviction rule is "nothing drawn in this frame", so the counter
    // has to advance once per frame and never repeat.
    u32 frameCounter = 0;

    // **A world that was just created has nothing in it**, and the streamer
    // fills it in at the speed of one worker thread on an ARM11 -- so without
    // this the first thing a player sees after "Create New World" is empty sky,
    // for long enough to look like a bug rather than like work.
    //
    // The original shows a progress screen and generates the spawn area before
    // it hands over. This is the same idea with the same frame loop the game
    // uses, which is what keeps it honest: the world is streamed, not
    // pre-generated by a second code path.
    //
    // **It waits for the whole render distance now, not for the first quad.**
    // It used to stop at "geometry exists, or nine columns are resident", which
    // is the smallest thing that is not an empty screen -- and it is also
    // exactly what a player then walks straight off the edge of. The target is
    // every column inside the render distance *published*, which is the
    // streamer's own word for "the renderer has it and it can be drawn"; that
    // in turn requires the ring one chunk further out to be generated too,
    // because a section cannot be meshed until its eight neighbours are in
    // memory. So the world the player is handed reaches the horizon in every
    // direction, and one chunk past it.
    //
    // Three ways out, because "generating" must never become "hung":
    //
    //   * the square fills -- every column in range is drawable;
    //   * the player presses START, which is the reason the hint is on both
    //     screens; or
    //   * nothing new becomes drawable for 45 seconds, which is a generator
    //     that has stopped rather than one that is slow. The outer cap is six
    //     minutes and exists only so a console cannot sit here forever if even
    //     the stall detector is wrong.
    //
    // 45 seconds rather than something tighter because the first published
    // column is the slowest: nothing can be published until a 3x3 of columns
    // exists, and on an old 3DS the worker shares core 0 with this loop.
    if (choice.created) {
        constexpr int kMaxWaitFrames = 60 * 60 * 6;
        constexpr int kStallFrames = 60 * 45;

        // The square is drawn at the same radius the wait is counted over, so
        // it is full exactly when the bar is -- a ring that could never turn
        // green would read as a stall rather than as an outer ring.
        int radius = renderer.config().meshDistance;
        if (radius > kMaxProgressRadius) {
            radius = kMaxProgressRadius;
        }
        if (radius > ctr::ProgressScreen::maxGridRadius()) {
            radius = ctr::ProgressScreen::maxGridRadius();
        }

        ctr::ProgressScreen progress;
        const bool haveScreen = progress.init();
        if (haveScreen) {
            progress.begin(ctr::ProgressScreen::Kind::Generating, choice.worldName.c_str());
            progress.setNote("START to go in anyway");
        }

        int mostDone = -1;
        int sinceProgress = 0;
        for (int waited = 0; waited < kMaxWaitFrames && aptMainLoop(); ++waited) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                break;
            }

            const Frustum frustum = renderer.cullFrustum(camera);
            renderer.chunks().beginFrame(++frameCounter, frustum, camera.chunkX(),
                                         camera.sectionY(), camera.chunkZ());
            world.update(renderer.chunks(), camera.chunkX(), camera.chunkZ(), budget);

            const render::WorldStreamer::ProgressCount made =
                world.progressWithin(camera.chunkX(), camera.chunkZ(), radius);
            if (made.done > mostDone) {
                mostDone = made.done;
                sinceProgress = 0;
            } else if (++sinceProgress > kStallFrames) {
                break;
            }

            if (haveScreen) {
                world.progressGrid(camera.chunkX(), camera.chunkZ(), radius, gProgressCells);
                progress.setGrid(gProgressCells, radius * 2 + 1);
                progress.setCounts(u32(made.done), u32(made.total));
                progress.present(renderer, camera);
            } else {
                // No citro2d, so no bar -- but the world still gets made and
                // the frame still gets drawn, which is what this loop did
                // before the screen existed.
                renderer.drawFrame(camera);
            }

            if (made.finished()) {
                break;
            }
        }

        if (haveScreen) {
            progress.shutdown();
        }
        // The bottom screen is the progress screen's, and the clock has not
        // been read since before a minute of generation. Both are the same two
        // things the pause menu hands back -- see the note where runPause
        // returns.
        overlay.invalidate();
        lastTick = svcGetSystemTick();
    }

    while (aptMainLoop()) {
        hidScanInput();
        if (haveCstick) {
            // ir:rst has its own scan; hidScanInput knows nothing about the
            // C-stick.
            irrstScanInput();
        }
        const u32 down = hidKeysDown();
        const u32 held = hidKeysHeld();
        if (down & KEY_START) {
            // Entity changes do not dirty block columns. Snapshot them on
            // pause too, including removal of the last entity in the world.
            world.saveNow(ctr::nowMillis());

            // **No target, no screen taken, nothing read off the card.** The
            // menu draws into this loop's own frames, over the world, so all
            // this asks for is citro2d and a text buffer. It is also why there
            // is no listing here: the pause menu shows neither the world list
            // nor the pack list, and reading either was what made pressing
            // START a two-second stop.
            if (!menu.initOverlay(isNew3DS)) {
                std::printf("\x1b[31mno memory for the pause menu\x1b[0m\n");
                break;
            }

            // The world as it stood when START was pressed, redrawn every frame
            // the menu is up. Nothing is ticked, streamed or meshed while it is
            // -- the camera does not move and the sun does not either -- so
            // every one of those frames is the same picture with the menu over
            // it.
            PausedWorld backdropWorld{&renderer, &camera};
            ctr::PauseBackdrop backdrop;
            backdrop.context = &backdropWorld;
            backdrop.drawFrame = drawPausedWorld;

            const ctr::PauseChoice paused =
                menu.runPause(choice.worldName.c_str(), choice.worldPath.c_str(),
                              settings.renderDistance, backdrop);
            menu.shutdown();

            // Two things the menu took, where there used to be three. The top
            // screen is no longer one of them: the menu never created a target
            // of its own and never called gfxSet3D, so citro3d's output table
            // still holds both eyes and there is nothing to reclaim.
            //
            //   * The bottom-screen console, which the menu cleared and wrote
            //     its own help onto.
            //   * The clock. `dt` is measured from the last frame, and the last
            //     frame was however long ago the player pressed START.
            overlay.invalidate();
            lastTick = svcGetSystemTick();

            // Nothing below is worth doing for a world that is closing: the
            // pack and the distance are already saved in 3ds.ini and held by
            // the Menu, and applying either here would upload an atlas and
            // rebuild the whole VBO pool a few frames before both are thrown
            // away.
            if (paused.action == ctr::PauseChoice::Action::ExitWorld) {
                break;
            }

            // The world settings screen can change the gamemode without leaving
            // the world, and the gamemode is which bottom screen this is.
            //
            // **It is also which of the two movers owns the position**, and
            // that hand-over has to be made explicitly. Spectator flies a bare
            // camera and never touches the body; every other mode reads the
            // camera straight off the body. So a mode change that did not carry
            // the position across left the body wherever it was last ticked --
            // at the spawn point, usually -- and re-entering Creative teleported
            // the player back to it, frequently inside terrain. That is the
            // "changing gamemode puts you in the ground" bug.
            //
            // Only one direction needs work. Leaving Spectator puts the body
            // under the camera; entering it needs nothing, because the camera is
            // already at the eye the body was handing it.
            const settings::Gamemode previousMode = overlay.gamemode();
            overlay.setGamemode(paused.gamemode);
            if (previousMode == settings::Gamemode::Spectator
                && paused.gamemode != settings::Gamemode::Spectator) {
                // `camera.y` is the eye, which is what `eyeY()` hands out, so
                // the feet are that minus the offset -- the same conversion
                // world entry makes from a saved `Pos[1]`.
                body.setFeet(camera.x, camera.y - double(mc::entity::kEyeHeight), camera.z);
                // A camera that was flying has no velocity worth inheriting,
                // and a banked `fallDistance` would be cashed in the moment the
                // body touched down. `setFeet` has already snapped the
                // interpolation, so the first frame back draws where the camera
                // already was rather than sliding there.
                body.motionX = 0.0;
                body.motionY = 0.0;
                body.motionZ = 0.0;
                body.fallDistance = 0.0f;
                body.onGround = false;
                sprintGesture.cancel();
            }

            if (paused.atlasChanged) {
                if (!renderer.setAtlas(menu.atlas())) {
                    std::printf("\x1b[31mcould not upload that pack\x1b[0m\n");
                    overlay.invalidate();
                }
                // The font is its own file with its own absence, so it is
                // uploaded separately -- a pack with no `default.png` leaves
                // signs blank rather than leaving the world untextured.
                renderer.setFont(menu.fontImage());
                // The map is drawn from the same pack as the world, so ground
                // sampled under the old one is recoloured rather than redrawn:
                // the store holds block ids, not pixels.
                overlay.setAtlas(menu.atlas());
                overlay.setBackdropTile(menu.backgroundTile());
                // The needle is drawn over the pack's own compass face, so a
                // new pack means a new base. Without this the compass keeps the
                // old pack's dial for the rest of the session.
                seedCompass(menu.atlas());
                // The outline atlas went with the old one and may not have come
                // back, so the debug page is told what is actually on rather
                // than what was asked for -- the same read-back the page does
                // when it sets this itself.
                settings.wireframe = renderer.wireframe();
            }

            // The same order and the same reason as the debug page below: the
            // pool goes first, and the streamer republishes into whatever field
            // it finds. Routed through `settings` so the debug page and the
            // pause menu cannot end up disagreeing about what the distance is.
            if (paused.renderDistance != settings.renderDistance) {
                settings.renderDistance = paused.renderDistance;
                renderer.setMeshDistance(settings.renderDistance);
                world.setMeshDistance(settings.renderDistance, renderer.chunks());
            }

            world.setAutosaveSeconds(paused.autosaveSeconds);

            continue;
        }

        // The bottom screen owns SELECT, so nothing below fires while it is
        // held. Applying is the caller's job because a render distance means
        // rebuilding the field, the pool and the streamer's grid -- in that
        // order, since the streamer republishes into whatever field it finds.
        if (overlay.handleInput(down, held, &settings, &camera)) {
            if (settings.renderDistance != renderer.config().meshDistance) {
                renderer.setMeshDistance(settings.renderDistance);
                world.setMeshDistance(settings.renderDistance, renderer.chunks());
            }
            // Same order and the same reason: the pool goes first because every
            // mesh in it is in the old encoding, and the streamer republishes
            // into whatever field it finds.
            const mesh::CubeFormat wanted = settings.geometryQuads
                                                ? mesh::CubeFormat::Quads
                                                : mesh::CubeFormat::Vertices;
            if (wanted != renderer.cubeFormat()) {
                renderer.setCubeFormat(wanted);
                world.setCubeFormat(wanted, renderer.chunks());
                if (wanted == mesh::CubeFormat::Quads) {
                    // The republish walks the whole spiral synchronously, so if
                    // this line is the last one on the screen the fault is in
                    // there and not in any draw. See Renderer::setCubeFormat for
                    // the breadcrumb before it.
                    ctr::geoTrace("republished; first quad frame next");
                }
            }
            // Greedy meshing, pool first for the same reason: the streamer
            // republishes into whatever it finds, and a pool still holding the
            // other kind of mesh would make the A/B read half of each.
            if (settings.greedyMeshing != world.greedy()) {
                renderer.remesh();
                world.setGreedy(settings.greedyMeshing, renderer.chunks());
            }
            // Refuses if there was no room for the outline atlas, so the
            // setting is read back from the renderer rather than assumed.
            renderer.setWireframe(settings.wireframe);
            settings.wireframe = renderer.wireframe();
        }

        // **The renderer gave up on the quad format.** Its watchdog caught a
        // command list the GPU never finished; the sections built in that
        // encoding are being skipped, so the world has holes in it until they
        // are rebuilt. Ask for the encoding that is known to draw, through the
        // same `settings` field the debug page writes, so the page and the
        // renderer cannot end up disagreeing about which one is live.
        if (renderer.quadDrawsStopped() && settings.geometryQuads) {
            settings.geometryQuads = false;
            renderer.setCubeFormat(mesh::CubeFormat::Vertices);
            world.setCubeFormat(mesh::CubeFormat::Vertices, renderer.chunks());
        }

        const u64 tick = svcGetSystemTick();
        float dt = float(double(tick - lastTick) / double(SYSCLOCK_ARM11));
        lastTick = tick;
        // A frame that took a quarter of a second is the console having been
        // suspended, not the player having moved that far.
        dt = dt > 0.25f ? 0.25f : dt;

        // **Spectator flies; everything else has a body.** The body is not
        // moved here -- it runs on the world's 20 Hz tick further down, because
        // that is the rate every constant in it was measured at. Free flight is
        // frame-rate movement and stays that way.
        if (overlay.gamemode() == settings::Gamemode::Spectator) {
            // X is sprint and also SELECT + X is a page back, so sprint waits
            // for SELECT to be let go.
            flyCamera(camera, dt, (held & KEY_X) != 0 && !(held & KEY_SELECT));
        }

        // **Creative's flight toggle: double-tap jump.** It is the gesture the
        // game this is modelled on uses, and on a console it is the only one
        // going spare -- every face button and both shoulders are spoken for,
        // and X is the bottom screen's focus.
        //
        // Timed in frames rather than ticks because it is a gesture and not
        // physics: a fifth of a second is a double click everywhere else here,
        // and it must mean the same thing at 30 fps as at 60.
        sinceStart += dt;
        if (overlay.gamemode() == settings::Gamemode::Creative && (down & KEY_B) != 0
            && !overlay.uiFocused()) {
            constexpr float kDoubleTapSeconds = 0.25f;
            if (lastJumpTap >= 0.0f && sinceStart - lastJumpTap < kDoubleTapSeconds) {
                flying = !flying;
                // Consumed, so a third tap starts a new gesture rather than
                // toggling again off the second one.
                lastJumpTap = -1.0f;
            } else {
                lastJumpTap = sinceStart;
            }
        }
        // Leaving Creative -- the pause menu can do it without leaving the
        // world -- has to put the player back on the ground rather than leave
        // them hanging with no way to switch it off.
        flying = flying && overlay.gamemode() == settings::Gamemode::Creative;
        // **The focused circle pad, before the look.** It scrolls the map when
        // the map is the focused page and does nothing otherwise; the body's
        // heading is zeroed to match, further down.
        overlay.tickFocus(dt);

        lookWithTouch(camera, &dragging, &lastTouch, overlay.touchLookTop());
        if (haveCstick) {
            lookWithCstick(camera, dt);
        }
        tuneStereo(renderer);

        // **The world's clock.** `svcGetSystemTick` off the ARM11's fixed
        // oscillator is the one clock this console has, which is why the
        // original's two-clock correction is not reproduced; see
        // core/tick/tick_timer.hpp.
        tickTimer.advance(double(tick) / double(SYSCLOCK_ARM11));

        // Alpha's own curve, from the jar. It holds full brightness for the
        // first half of the day rather than peaking at noon, which is the
        // difference between a world that looks like Alpha and one that looks
        // permanently overcast.
        const i64 worldTicks = world.level().time;
        const i64 dayTicks = worldTicks - (worldTicks / 24000) * 24000;
        const float timeOfDay = float(double(dayTicks) / 24000.0);
        renderer.setSkyDarken(world::skyLightSubtracted(dayTicks, tickTimer.partialTicks()));

        // **Where the player is and what time it is, for whatever saves next.**
        // Four stores a frame; the autosave timer below, the flush when the
        // pause menu opens, and close() on the way out all read it. Before this
        // a world always reopened where it was first entered and at the time it
        // was created.
        //
        // **The body's position, not the camera's, wherever there is a body.**
        // The camera is interpolated between two ticks now (see the body tick
        // below), and a position part way through a tick is a picture rather
        // than a state: saving one writes a `Pos` the physics never produced,
        // and reloading it starts the world a fraction of a block off. In
        // Spectator there is no body and the camera is the whole truth.
        // **Written back on the frame it changed, not every frame.** The
        // streamer copies up to forty stacks and the NBT each one preserved, so
        // this is on the edit rather than in the frame path -- see
        // WorldStreamer::setPlayerInventory. The next autosave, the pause
        // menu's flush and close() all pick it up from there.
        if (overlay.takeInventoryChange()) {
            std::vector<mc::item::ItemStack> stacks;
            overlay.inventory().save(&stacks);
            world.setPlayerInventory(stacks);
        }

        const bool hasBody = overlay.gamemode() != settings::Gamemode::Spectator;
        world.setPlayerState(hasBody ? body.x : camera.x,
                             hasBody ? body.eyeY() : camera.y,
                             hasBody ? body.z : camera.z, camera.yaw * 180.0f / kPi,
                             camera.pitch * 180.0f / kPi, worldTicks);

        // **What the crosshair is on, for the outline.** Once a frame rather
        // than once an eye: both eyes look at the same block, and the ray walk
        // is the same work either way.
        if (overlay.gamemode() != settings::Gamemode::Spectator) {
            tick::TickWorld* aimWorld = world.worldTick();
            if (aimWorld != nullptr) {
                float lx = 0.0f;
                float ly = 0.0f;
                float lz = 0.0f;
                camera.look(&lx, &ly, &lz);
                const mc::entity::RayHit aim = mc::entity::rayTrace(
                    *aimWorld, camera.x, camera.y, camera.z, double(lx), double(ly), double(lz));
                // **An entity in front of the block takes the crosshair**, and
                // `drawSelectionBox` outlines tiles only -- so the outline goes
                // rather than promising a block the click will not reach.
                const bool onEntity =
                    aim.hit
                    && mc::item::pickEntity(effects.entities, camera.x, camera.y, camera.z,
                                            double(lx), double(ly), double(lz), aim)
                           .found();
                if (aim.hit && !onEntity) {
                    renderer.setSelection(
                        mc::block::selectionBox(aimWorld->blockAt(aim.x, aim.y, aim.z),
                                                aimWorld->dataAt(aim.x, aim.y, aim.z))
                            .offset(double(aim.x), double(aim.y), double(aim.z)));
                } else {
                    renderer.clearSelection();
                }
            }
        } else {
            renderer.clearSelection();
        }

        // **The billboard basis, once a frame.** `EntityFX.renderParticle` is
        // handed five products of the camera's yaw and pitch; two vectors say
        // the same thing and can be checked against the camera's own `look()`
        // by eye -- both are perpendicular to it and to each other.
        //
        // The eye rather than the body: a particle is drawn relative to where
        // the camera is, and in Creative the two are not the same place.
        {
            const float sy = std::sin(camera.yaw);
            const float cy = std::cos(camera.yaw);
            const float sp = std::sin(camera.pitch);
            const float cp = std::cos(camera.pitch);
            const mc::render::Billboard basis{cy, 0.0f, sy, -sy * sp, cp, cy * sp};
            renderer.setParticles(particles.get(), basis, camera.x, camera.y, camera.z,
                                  tickTimer.partialTicks());
            // The item sprites turn to face the camera about the vertical axis
            // only -- `playerViewY` in the original, which is the same yaw the
            // basis above is built from, in degrees.
            renderer.setItemEntities(droppedItems.get(), camera.yaw * 180.0f / kPi,
                                     camera.x, camera.y, camera.z,
                                     tickTimer.partialTicks());
            // Full-size cubes, not turned to face anybody -- see
            // core/render/falling_block_mesh.hpp. It rides the item pass's eye
            // and partial, which the call above just set.
            renderer.setFallingBlocks(fallingBlocks.get());
            // Paintings do not move and do not interpolate, so this is the pool
            // and nothing else -- it uses the eye the item pass just set.
            renderer.setPaintings(paintings.get());
            // Arrows ride the item pass's eye and partial too; unlike a
            // painting they do interpolate, because they move.
            renderer.setArrows(arrows.get());
            renderer.setBoats(boats.get());
            // The only entity pass that needs the world: a cart leans along the
            // track rather than along its own motion.
            renderer.setMinecarts(minecarts.get(), world.worldTick());
            // Two textures and therefore two passes: the board off the entity
            // sheet and the text off the pack's font.
            renderer.setSigns(signs.get(), &menu.fontImage());
            renderer.setChat(chat.get(), &menu.fontImage());
        }

        // Each phase timed on its own. The frame period alone cannot tell a
        // console that is at its refresh rate from one that is struggling --
        // C3D_FrameBegin blocks until VBlank -- so the loop reports what it
        // actually spent rather than what it waited for.
        ctr::FrameTiming timing;

        const u64 beforeWalk = svcGetSystemTick();
        const Frustum frustum = renderer.cullFrustum(camera);
        renderer.chunks().beginFrame(++frameCounter, frustum, camera.chunkX(), camera.sectionY(),
                                     camera.chunkZ());

        const u64 beforeStream = svcGetSystemTick();
        world.update(renderer.chunks(), camera.chunkX(), camera.chunkZ(), budget);

        // **The world tick, after the streamer and before the draw.** After,
        // because a tick may only touch columns the grid has settled for this
        // frame. Zero ticks is the common case at 30 fps and costs a compare.
        //
        // **A block it changes is drawn one frame stale, and that is now the
        // design rather than an accident.** This comment used to claim the tick
        // was placed before the draw so a changed block reached the mesher in
        // the same frame. It does not: `beginFrame` above built this frame's
        // draw list, `world.update` already spent this frame's mesh budget, and
        // the tick runs after both -- so a remesh cannot land before the frame
        // after next. Moving the tick earlier would not fix that either, since
        // the draw list is fixed before any of it.
        //
        // What fixes it is not re-ordering but keeping the old mesh drawable:
        // an invalidated section holds its geometry and is drawn one tick out of
        // date until its replacement is uploaded. See
        // SectionField::sectionDirty. Before that, a block change took the
        // section out of the draw list a frame before its replacement existed,
        // which is what made edited chunks flash transparent.
        const int ticksDue = tickTimer.elapsedTicks();
        editTick += i64(ticksDue);

        // **The body runs on the world's ticks, not on frames.** A gravity of
        // 0.08 a tick and a drag of 0.98 a tick mean nothing at any other rate;
        // scaling them by a frame time would be a different game that happened
        // to look similar. A frame spanning no tick moves the player not at
        // all, and one spanning three moves them three times.
        if (overlay.gamemode() != settings::Gamemode::Spectator) {
            // Reach, break and place. Before the body's tick, so the edit and
            // the movement in one frame see the same world.
            //
            // **Suspended while the bottom screen is focused**, because the
            // shoulders are the palette's pager then. One press does one thing;
            // see Overlay::handleFocusedInput.
            if (!overlay.uiFocused()) {
                const ChatSink chatSink{
                    chat.get(), menu.fontImage().empty() ? nullptr : menu.fontImage().widths};
                editBlocks(world, renderer.chunks(), camera, body, overlay, down, held,
                           editTick, &lastEditTick, effects, &hand, chatSink);

                // **A drops one of what is in the hand.** a1.1.2 has no drop
                // key at all -- `dropOneItem` does not exist in it and the only
                // callers of `dropPlayerItem` in the jar are the inventory
                // screens spilling the stack on the cursor -- so the button is
                // ours and the throw underneath it is the game's. A is the one
                // going spare out here: focused, it is the bottom screen's
                // pick, and this branch is the unfocused one. It used to be
                // flight's held boost as well; that is gone.
                //
                // **The entity first, the stack second.** The pool can be full,
                // and taking the item off the hand for an entity that was
                // refused would destroy it. See Overlay::dropHeldItem.
                if ((down & KEY_A) != 0) {
                    if (mc::tick::TickWorld* dropWorld = world.worldTick()) {
                        const mc::item::Inventory& inv = overlay.inventory();
                        const mc::item::ItemId held = inv.selectedItem();
                        const mc::u32 refusedBefore = droppedItems->refused();
                        if (held != 0
                            && droppedItems->dropFromPlayer(
                                   *dropWorld, body.x, body.posY, body.z,
                                   camera.yaw * 180.0f / kPi,
                                   camera.pitch * 180.0f / kPi, held, 1,
                                   inv.at(inv.selected).damage)) {
                            overlay.dropHeldItem();
                        } else if (droppedItems->refused() != refusedBefore) {
                            chatSink.limitReached(mc::item::LimitedEntity::DroppedItem);
                        }
                    }
                }
            }

            tick::TickWorld* tickWorld = world.worldTick();
            if (tickWorld != nullptr) {
                mc::entity::PlayerInput bodyInput = readBodyInput(camera, held);
                // **B belongs to the bottom screen while it is focused**, where
                // it is the back button. Jumping on the same press would be one
                // button doing two things, which is the thing the focus exists
                // to avoid. Y is left alone: nothing focused reads it.
                if (overlay.uiFocused()) {
                    bodyInput.jump = false;
                }
                // ...and the stick belongs to the map while the map is the
                // focused page. Walking and panning at once would be two things
                // fighting over one window, and the map would be dragged back
                // under the player every step.
                if (overlay.mapPanActive()) {
                    bodyInput.strafe = 0.0f;
                    bodyInput.forward = 0.0f;
                }
                // **The double-tap sprint**, read once a frame off the same
                // stick and handed to every tick this frame owes.
                //
                // **Creative only, because a1.1.2 has no sprint at all.** The
                // gesture is the console editions' and the speed is Beta's, so
                // it belongs with the other thing in this build that is openly
                // not this version's -- Creative itself. Survival gets a1.1.2's
                // walk, unmodified, which is the whole point of Survival here.
                //
                // Suppressed wherever the stick is not the player's -- a
                // focused bottom screen, a panned map -- and while flying,
                // which has one flat speed and nothing for a gesture to change.
                bodyInput.sprint = readSprint(
                    sprintGesture, bodyInput, sinceStart,
                    overlay.gamemode() != settings::Gamemode::Creative || flying
                        || overlay.uiFocused() || overlay.mapPanActive());
                for (int i = 0; i < ticksDue; ++i) {
                    // **The hand, first and unconditionally.** `Minecraft.i()`
                    // runs `ItemRenderer.updateEquippedItem` once a tick with a
                    // world open, and `EntityPlayer.onUpdate` runs the arm
                    // swing -- neither cares whether the player is walking,
                    // flying or in a boat, and the riding branch below returns
                    // early. See core/render/held_item.hpp.
                    hand.tick(overlay.inventory().selectedItem());

                    // **Riding, which is neither walking nor flying.**
                    //
                    // The body keeps turning the stick into motion exactly as
                    // it would on foot -- `EntityLiving` never checks whether
                    // it is riding -- and the vehicle reads that motion and
                    // hands back a seat. See PlayerBody::tickRiding.
                    //
                    // **Y dismounts**, which is the sneak button and is what
                    // the original uses. It is tested before the tick so that
                    // the tick the button is pressed on is a walking one.
                    const bool inBoat = boats->riddenIndex() >= 0;
                    const bool inCart = minecarts->riddenIndex() >= 0;
                    if (inBoat || inCart) {
                        if ((held & KEY_Y) != 0) {
                            boats->dismount();
                            minecarts->dismount();
                        } else {
                            mc::entity::VehicleRider seat;
                            seat.present = true;
                            seat.motionX = body.motionX;
                            seat.motionZ = body.motionZ;
                            mc::entity::RiderSeat where;
                            if (inBoat) {
                                boats->tick(*tickWorld, seat);
                                where = boats->seat();
                            } else {
                                minecarts->tick(*tickWorld, seat);
                                where = minecarts->seat();
                            }
                            if (where.valid) {
                                body.tickRiding(bodyInput, where.x, where.y, where.z);
                                continue;
                            }
                            // The vehicle went -- broken on a wall, or streamed
                            // out from under us. Fall through to walking, which
                            // is what being thrown clear looks like.
                        }
                    }
                    if (flying) {
                        // B and Y are up and down, which is exactly what they
                        // are in Spectator's `flyCamera` -- the two movers
                        // should not disagree about which button rises.
                        //
                        // **One speed.** A used to be a held boost here, copied
                        // from Spectator's X; it is the drop now. See
                        // core/entity/player_body.hpp for why Spectator keeps
                        // its boost and Creative does not.
                        body.tickFlying(*tickWorld, bodyInput, (held & KEY_B) != 0,
                                        (held & KEY_Y) != 0, mc::entity::kFlightSpeed);
                    } else {
                        body.tick(*tickWorld, bodyInput);
                    }

                    // `moveEntity`'s tail, which `PlayerBody` cannot run
                    // itself because it moves against a const world. This is
                    // what arms a pressure plate the moment it is stood on;
                    // see tick::entityCollidedWithBlocks.
                    mc::tick::entityCollidedWithBlocks(*tickWorld, body.box);

                    // A flying Creative player still has the ordinary player
                    // body, so it must participate in minecart collisions.
                    // The cart supplies the equal-and-opposite impulse; the
                    // body carries it into its next movement tick.
                    minecarts->collideWithPlayer(body.box, body.x, body.z,
                                                  &body.motionX, &body.motionZ);

                    // **Walking over what is lying about**, once per tick.
                    // `EntityPlayer.onUpdate` expands its own box by a block on
                    // the two horizontal axes and by nothing on y before it
                    // asks the world what is inside -- so an item is reached
                    // from a step away sideways and never from a floor below.
                    const AABB reach = body.box.expand(1.0, 0.0, 1.0);
                    const int picked = overlay.collectItems(*droppedItems, reach);
                    for (int p = 0; p < picked; ++p) {
                        // `random.pop` at volume 0.2, and the pitch is the
                        // original's expression rather than a constant: two
                        // draws subtracted, so most pickups land near 2.0 and
                        // few at the edges.
                        const float pitch =
                            ((pickupRand.nextFloat() - pickupRand.nextFloat()) * 0.7f
                             + 1.0f) * 2.0f;
                        sound.playSoundAt("random.pop", body.x,
                                          body.posY - double(mc::entity::kEyeHeight),
                                          body.z, 0.2f, pitch);
                    }

                    // **The footstep, once per tick and not once per frame.**
                    // `move()` decides whether this tick earned one and which
                    // block it belongs to; all that is left here is a listener
                    // and a pool key, which is the half core cannot have. The
                    // position is the feet -- `posY - yOffset` --  because
                    // that is what `playSoundAtEntity` passes.
                    const mc::audio::SoundCue step =
                        mc::audio::stepCue(body.stepSoundDue);
                    if (step.playable()) {
                        sound.playSoundAt(step.key, body.x,
                                          body.posY - double(mc::entity::kEyeHeight),
                                          body.z, step.volume, step.pitch);
                    }
                }
            }
            // **Between two ticks, not on the last one.** The body runs at
            // 20 Hz and this screen draws at 30 or 60, so pinning the camera to
            // `body.x` left it still for one or two frames and then jumping a
            // whole tick's travel -- a fifth of a block at a walk, which reads
            // as moving in steps rather than walking. This is
            // `EntityRenderer.orientCamera`'s own line: the position the body
            // was at when the tick began, plus the fraction of a tick the frame
            // is being drawn at. See PlayerBody::renderX.
            const float partial = tickTimer.partialTicks();
            camera.x = body.renderX(partial);
            camera.y = body.renderEyeY(partial);
            camera.z = body.renderZ(partial);
        }

        // **The one thing on the top screen that is not the world**, and the
        // one renderer hand-over that is *after* the ticks rather than before
        // them. Every pass above takes a pool that the tick loop then advances,
        // which costs a frame of latency nobody can see on a boat; the hand's
        // whole animation is five ticks long and a frame behind on it reads as
        // the item lagging the button. It needs no eye and no origin either --
        // it is drawn in camera space -- so there is nothing tying it to that
        // block.
        //
        // **Spectator has no hand**, for the same reason it has no hotbar:
        // there is no body to hold anything.
        {
            u8 handLight = 0;
            if (const mc::tick::TickWorld* lit = world.worldTick()) {
                const i32 hx = mc::MathHelper::floorDouble(body.x);
                const int hy = int(mc::MathHelper::floorDouble(body.posY));
                const i32 hz = mc::MathHelper::floorDouble(body.z);
                handLight = u8((lit->skyLightAt(hx, hy, hz) << 4)
                               | lit->blockLightAt(hx, hy, hz));
            }
            const float handPartial = tickTimer.partialTicks();
            if (overlay.gamemode() == settings::Gamemode::Spectator) {
                // **Not an empty hand -- no hand.** An empty hand draws the
                // player's arm, which is exactly what a camera with no body
                // should not have.
                renderer.clearHeldItem();
            } else {
                renderer.setHeldItem(hand.item(), hand.equippedProgress(handPartial),
                                     hand.swingProgress(handPartial), handLight);
            }
        }

        // **The ears go where the camera is**, once a frame, as
        // `SoundManager.setListener` does off the render view entity. Every
        // positional sound is attenuated against this; without it they would
        // all play at full volume from the origin.
        sound.setListener(camera.x, camera.y, camera.z);

        const u64 beforeTick = svcGetSystemTick();
        world.stepTicks(renderer.chunks(), ticksDue);
        timing.tickMs = ctr::millisFromTicks(svcGetSystemTick() - beforeTick);
        timing.ticksRun = ticksDue;

        // **The music counter, on the same ticks the world ran.** a1.1.2 calls
        // `of.c()` from the last statement of `PlayerControllerSP.onUpdate`,
        // which `Minecraft.i()` runs once per tick with a world open and the
        // game unpaused -- so this belongs here, beside `stepTicks`, and takes
        // the same `elapsedTicks()`. The pause menu is its own loop and does
        // not reach this line, which is the gate `Minecraft.m` was.
        //
        // Zero ticks is the common case at 30 fps and costs a compare. When it
        // is not zero this is a counter decrement and, a few times an hour, a
        // file open handed to the decode thread -- never a decode.
        sound.tick(ticksDue);
        sound.update();

        // `lu.a()` -- the chat lines age on the world's clock too, so a line
        // lasts its ten seconds of game time at any frame rate and waits out a
        // pause.
        chat->tick(ticksDue);

        // The particles are entities and run on the world's clock, not the
        // frame's -- so a console at 24 fps sees the same cloud a console at 60
        // does, only sampled less often.
        if (mc::tick::TickWorld* fxWorld = world.worldTick()) {
            for (int i = 0; i < ticksDue; ++i) {
                particles->tick(*fxWorld);
                droppedItems->tick(*fxWorld);
                // **The one entity here that writes blocks**, which is why it
                // takes the world by reference where the other two do not: it
                // empties the cell it came from and fills the one it lands in.
                fallingBlocks->tick(*fxWorld);
                // **Cheap and not skippable.** A painting's tick is a light
                // sample and, one tick in a hundred, the check that its wall is
                // still there -- which is what makes a picture fall when the
                // block behind it is mined.
                paintings->tick(*fxWorld);
                // **Boats that nobody is in.** The ridden one is ticked in the
                // body loop above, because it needs the rider's motion from
                // that same tick; this covers the rest. `present` false is what
                // tells it there is nothing steering.
                if (boats->riddenIndex() < 0) {
                    boats->tick(*fxWorld, mc::entity::VehicleRider{});
                }
                if (minecarts->riddenIndex() < 0) {
                    minecarts->tick(*fxWorld, mc::entity::VehicleRider{});
                }
                // A tile entity does not tick, but its light does: a torch put
                // beside a sign has to brighten it, and the store is the only
                // thing that knows where the signs are.
                signs->refreshLight(*fxWorld);
                // **Arrows, which move by ray rather than by sweep.** One
                // `rayTraceBlocks` per live arrow per tick, and a stuck one
                // costs a block read and nothing else -- see
                // core/entity/arrow.hpp.
                arrows->tick(*fxWorld, mc::entity::ArrowTargets{paintings.get(), boats.get(),
                                                                minecarts.get()});
                for (int n = 0; n < arrows->struckLastTick(); ++n) {
                    // `random.drr`, once per arrow that landed. The pitch is
                    // the original's `1.2F / (rand * 0.2F + 0.9F)`.
                    sound.playSoundAt("random.drr", body.x, body.eyeY(), body.z, 1.0f,
                                      1.2f / (pickupRand.nextFloat() * 0.2f + 0.9f));
                }
                // Same tail, per item: a wooden plate is pressed by a dropped
                // stack and a stone one is not. Particles are not entities in
                // the original's sense and never run it.
                for (int n = 0; n < droppedItems->count(); ++n) {
                    mc::tick::entityCollidedWithBlocks(*fxWorld, (*droppedItems)[n].box);
                }
            }
        }

        // **Fire, on the world's clock and pushed once a frame.**
        //
        // The original steps its `TextureFX` list once per *frame* from
        // `RenderEngine.updateDynamicTextures`; this steps it once per tick and
        // uploads once per frame, which is the same split the particles and the
        // music counter already use -- a console at 24 fps and one at 60 see
        // the same flame, only sampled differently. `tick(0)` is the common
        // case at 30 fps and costs a compare.
        //
        // **Before the draw, never inside it.** The two tiles are 2 KB of a
        // texture the GPU samples for every fragment of every chunk; the copy
        // has to land while nothing is reading it, which is what this position
        // -- beside `stepTicks` and ahead of `drawFrame` -- buys.
        if (ticksDue > 0 && flames->present()) {
            flames->tick(ticksDue);
            for (int which = 0; which < mc::texture::kFlameCount; ++which) {
                const int tile = flames->tile(which);
                if (tile >= 0) {
                    renderer.setAtlasTile(tile, flames->texels(which));
                }
            }
        }

        // **And the compass, on the same clock and in the same place.**
        //
        // Stepped `ticksDue` times rather than once so that a console dropping
        // frames sees the needle swing at the same rate as one that is not --
        // the spring is a per-step thing and skipping steps would make a slow
        // console's compass lag its player. It is nine sines and cosines a
        // step; the loop is not the cost, the two tile pushes are, and those
        // happen once however many steps ran.
        if (ticksDue > 0 && compassTile >= 0) {
            // **And only when a texel moved.** `tick` answers that; see
            // core/texture/compass_fx.hpp. The spring converges long after the
            // needle has stopped moving on a 16 x 16 tile, and pushing anyway
            // meant the overlay repainted the whole hotbar band -- or the
            // inventory panel -- twenty times a second for a picture that was
            // already on the screen, straight into the buffer the LCD is
            // scanning out of. The `|| moved` is after the call on purpose: the
            // step has to run for every tick due, whatever the last one said.
            bool moved = false;
            for (int i = 0; i < ticksDue; ++i) {
                moved = compass->tick(world.level().spawnX, world.level().spawnZ,
                                      hasBody ? body.x : camera.x, hasBody ? body.z : camera.z,
                                      camera.yaw * 180.0f / kPi)
                        || moved;
            }
            if (moved) {
                renderer.setItemsTile(compassTile, compass->texels());
                overlay.setAnimatedItemsTile(compassTile, compass->texels());
            }
        }
        // The autosave timer. It writes level.dat and refreshes session.lock,
        // neither of which had any trigger but close() before, and hands
        // anything still dirty to the I/O thread. Generated columns do not wait
        // for it -- see WorldStreamer::setAutosaveSeconds.
        world.tickSaves(ctr::nowMillis());
        const u64 afterStream = svcGetSystemTick();

        timing.walkMs = ctr::millisFromTicks(beforeStream - beforeWalk);
        timing.streamMs = ctr::millisFromTicks(afterStream - beforeStream);

        // **After the streamer and before the draw**, because it reads columns
        // out of the grid and the grid is settled for the frame by now. It
        // samples at most a chunk or two and does nothing at all in a mode
        // whose bottom screen has no map on it.
        overlay.tickMap(world, camera);

        renderer.drawFrame(camera);

        overlay.draw(renderer, world, camera, timing, dt * 1000.0f, timeOfDay, settings);

        // **The figures the out-of-memory reporter prints, refreshed here and
        // nowhere else.** They are copied rather than fetched at the moment of
        // failure because that failure can land on the generation worker or the
        // I/O thread, and reaching back into the streamer from a new handler
        // would ask for locks whichever of them is already holding. Eight
        // word-sized stores a frame; see heap.hpp.
        {
            const render::WorldStreamer::Stats& streaming = world.stats();
            const render::VboPool::Stats& pool = renderer.chunks().pool().stats();
            mc::ctr::MemorySnapshot snapshot;
            snapshot.blockKb = u32(streaming.blockBytes / 1024);
            snapshot.dirtyKb = u32(streaming.io.dirtyBytes / 1024);
            snapshot.cleanKb = u32(streaming.io.cleanBytes / 1024);
            snapshot.genLive = streaming.generatorPeakLive;
            snapshot.poolKb = u32(pool.resident / 1024);
            snapshot.columns = u32(streaming.columnsResident);
            snapshot.sections = u32(pool.residents);
            snapshot.chunkX = camera.chunkX();
            snapshot.chunkZ = camera.chunkZ();
            snapshot.quadFormat = renderer.cubeFormat() == mesh::CubeFormat::Quads ? 1 : 0;
            mc::ctr::setMemorySnapshot(snapshot);
        }
    }

    // The original says the same thing on its way out of a world, and it is
    // worth saying: close() rewrites level.dat and flushes every dirty column,
    // which on a card is long enough for a still screen to look like a hang.
    // **With a percentage**, because "long enough to look like a hang" is
    // exactly the case a number answers and a word does not -- and because a
    // world saved a moment ago by the pause menu owes nothing at all, which
    // this now says rather than sitting on the same still screen.
    //
    // citro2d is free here on every path out of the loop: the pause menu gives
    // it back the moment runPause returns, and the shell gives it back before
    // runGame is called at all. If it cannot be had, the world is still saved
    // -- close() is told to report to nobody, which is what it did before this
    // screen existed.
    ctr::ProgressScreen saving;
    if (saving.init()) {
        saving.begin(ctr::ProgressScreen::Kind::Saving, choice.worldName.c_str());
        SaveScreen saveScreen{&saving, &renderer, &camera};
        world.close(ctr::nowMillis(), &saveScreen, drawSaveProgress);
        saving.shutdown();
    } else {
        world.close(ctr::nowMillis());
    }
    // **The one thing the renderer borrows from the world, given back the
    // moment the world is gone.** Every other entity pass borrows a pool that
    // is still on this stack; the cart pass also holds the `TickWorld`, which
    // `close()` has just destroyed. Nothing draws between here and
    // `shutdown()` today, so this is the invariant written down rather than a
    // second fix -- the first is that `close()` now keeps the tick world alive
    // until after the last frame the save screen draws. See
    // crashlogs/009-save-with-a-minecart/.
    renderer.setMinecarts(nullptr, nullptr);
    renderer.shutdown();
    return 0;
}

// The shell: menu, game, menu, until the player quits.
//
// **C3D_Init lives here rather than in runGame**, which is the change that made
// a menu possible at all. It used to run after the no-world check, so everything
// before a world was chosen had only the bottom-screen text console to talk
// through -- which is why the "no world found" screen was a paragraph of printf.
//
// The menu's citro2d context and its top-screen render target are built and
// given back around each visit, so the 400x240 colour buffer and its depth
// buffer are not sitting in VRAM while the atlas and the VBO pool are measured
// against what is left.
// The preload, on a thread that can hold Tremor. Joined immediately: this is
// not concurrency, it is borrowing a stack. See the call site.
void preloadOnWorker(void* arg)
{
    auto& sound = *static_cast<mc::audio::SoundEngine*>(arg);
    sound.preloadSound("random.click");
    // **The five other keys a block behaviour can name**, all of them cued
    // from `core/tick/` through `TickWorld::playSoundAt` or from
    // `core/item/use.cpp`. They are listed rather than derived because there
    // is nothing to derive them from: a sound a *behaviour* plays has no
    // column in the block table, unlike a footstep. A key the player has no
    // files for costs nothing, so a short list that is slightly too long is
    // free and one that is too short is a silence.
    sound.preloadSound("random.door_open");
    sound.preloadSound("random.door_close");
    sound.preloadSound("random.pop");
    sound.preloadSound("random.fizz");
    sound.preloadSound("fire.ignite");
    // **Every footstep and break sound the block table can name.** Bounded by
    // the table rather than by the card -- nine singletons name six distinct
    // keys in a1.1.2 -- and a key the player has no files for costs nothing.
    mc::audio::preloadBlockSounds(sound);
}

void preloadInterfaceSounds(mc::audio::SoundEngine& sound)
{
    mc::WorkerSpawn spawn = mc::workerSpawn();
    mc::WorkerJoin join = mc::workerJoin();
    void* handle = spawn != nullptr ? spawn(&preloadOnWorker, &sound, mc::WorkerRole::Audio)
                                    : nullptr;
    if (handle == nullptr) {
        // No thread to borrow. Decoding here anyway would risk the main stack,
        // and silent menus are a degradation this subsystem already has a name
        // for -- so the sounds simply do not load, and the Sound screen says
        // the click was found but would not decode.
        return;
    }
    if (join != nullptr) {
        join(handle);
    }
}

int runShell(bool isNew3DS, bool haveCstick)
{
    C3D_Init(ctr::kCommandBufferBytes);

    // One menu for the process, so the options screen and the cursor remember
    // where they were between worlds.
    ctr::Menu menu;
    int result = 0;

    // **Audio is process-lifetime and comes up before the first menu.**
    //
    // Two reasons it is not per-world. ndsp is a process-scoped service, so
    // bringing it up and down around each world would hand the DSP back and
    // take it again for nothing; and a track that starts near the end of a
    // session keeps playing while the player is on the title screen, which is
    // what the original does -- `of.c()` stops being *called* when the world
    // closes, but nothing stops the track.
    //
    // The settings are read here rather than borrowed from the menu because
    // the `audio` key decides whether ndsp is initialised at all, and that has
    // to be answered before anything else happens.
    mc::io::PosixFileSystem fs;
    mc::settings::GameSettings boot;
    mc::settings::loadSettings(fs, mc::settings::kSettingsPath, &boot);

    ctr::NdspBackend audio;
    audio.init(boot.audio != 0);

    // a1.1.2 seeds the music counter from `new Random()`. The console's clock
    // is the same idea and the same lack of reproducibility; the tests pass a
    // constant instead. See core/audio/music_ticker.hpp.
    mc::audio::SoundEngine sound(fs, audio, i64(ctr::nowMillis()));
    sound.loadResources();
    sound.setMusicVolume(boot.musicVolume < 0 ? 1.0f
                                              : float(boot.musicVolume) / 100.0f);
    sound.setSoundVolume(boot.soundVolume < 0 ? 1.0f
                                              : float(boot.soundVolume) / 100.0f);

    // **The one place an effect is decoded, and it is neither on a frame nor on
    // this thread.** `random.click` is what the menus press; it costs one card
    // read and a few milliseconds of Tremor, once, while nothing is on screen
    // yet. Doing it at the click would put an SD read on the frame the button
    // was pressed, which CONTRIBUTING forbids outright -- but doing it *here*,
    // inline, would be worse in a way that is easy to miss:
    //
    // **a 3DSX's main thread has 32 KB of stack and nothing in the binary can
    // enlarge it.** `kAudioStackBytes` is 32 KB for the decode thread alone,
    // and the comment on it says why: Tremor's inverse MDCT is not a shallow
    // call. Running it on top of `runShell`'s own frames would be a stack
    // overflow on exactly the consoles that have a resources folder to decode
    // -- the ones where it works. So the preload is handed to a worker with a
    // real stack and joined before the first menu is drawn. It is still boot
    // work done once; it simply happens somewhere it fits.
    //
    // With no worker ops installed the seam falls back to `std::thread`, and on
    // a platform with a megabyte of stack per thread the distinction does not
    // arise -- which is why the host harness calls `preloadSound` directly.
    //
    // It loads nothing when the player has no resources folder, and the menus
    // are silent -- the same degradation as a missing dspfirm.cdc, and the same
    // code path. See core/audio/sound_engine.hpp.
    preloadInterfaceSounds(sound);

    menu.setSound(&sound, &audio);

    while (aptMainLoop()) {
        if (!menu.init(isNew3DS)) {
            std::printf("\x1b[31mmenu init failed\x1b[0m\n");
            result = 1;
            break;
        }
        const ctr::MenuChoice choice = menu.run();
        menu.shutdown();

        if (choice.action != ctr::MenuChoice::Action::Play) {
            break;
        }

        result = runGame(choice, menu, sound, audio, isNew3DS, haveCstick);
        if (result != 0) {
            break;
        }
    }

    // Before C3D_Fini and before main() tears the rest down: the decode thread
    // has to be joined while the heap it reads from is still there.
    audio.shutdown();

    C3D_Fini();
    return result;
}

// The console's answer to "how big is a cluster and what is left", which core
// cannot ask for itself -- see core/io/volume_info.hpp. The path is ignored
// because there is one writable volume on this device.
bool queryVolume(const char* path, mc::io::VolumeInfo* out)
{
    (void)path;

    FS_ArchiveResource sd{};
    if (R_FAILED(FSUSER_GetSdmcArchiveResource(&sd))) {
        return false;
    }
    out->clusterSize = u64(sd.clusterSize);
    out->freeBytes = u64(sd.freeClusters) * u64(sd.clusterSize);
    out->totalBytes = u64(sd.totalClusters) * u64(sd.clusterSize);
    return true;
}

}  // namespace

int main()
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);

    mc::io::setVolumeInfoQuery(&queryVolume);

    // **Before the first allocation that could fail**, which is to say before
    // anything at all. Running out of newlib heap ends this process with a
    // silent `abort` -- see heap.hpp for why, disassembled -- and the report
    // from hardware that led to this was "it rebooted to the HOME menu", which
    // is what that looks like from the couch. The handler cannot prevent it;
    // it makes it say what ran out, on the bottom screen and on the card.
    mc::ctr::installOutOfMemoryReporter();

    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    if (isNew3DS) {
        // Belt and braces: recent Luma3DS no longer applies the exheader clock
        // alone.
        osSetSpeedupEnable(true);
    }

    // The C-stick is not part of hid: it arrives through ir:rst, which is the
    // same service a Circle Pad Pro reports through, so this is worth trying on
    // an old 3DS too rather than gating it on the model. `hidCstickRead`
    // dereferences ir:rst's shared memory unconditionally, so a failed init has
    // to be remembered rather than shrugged off.
    const bool haveCstick = R_SUCCEEDED(irrstInit());

    // **Before anything asks for a thread.** Both are process-wide and neither
    // depends on a world: the audio decode thread starts with the shell, long
    // before the first world is opened, and it needs the model to pick a core.
    // See the comment in runGame where these used to live.
    gWorkerIsNew3DS = isNew3DS;
    mc::setWorkerThreadOps(&spawnWorker, &joinWorker);

    // The M0 probe is still the only way to re-derive the hardware numbers the
    // design rests on, so it stays one button away rather than one git tag away.
    hidScanInput();
    const int result = (hidKeysHeld() & KEY_SELECT) ? mc::ctr::runProbe(isNew3DS)
                                                    : runShell(isNew3DS, haveCstick);

    if (haveCstick) {
        irrstExit();
    }
    gfxExit();
    return result;
}
