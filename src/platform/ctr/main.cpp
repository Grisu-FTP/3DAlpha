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

#include "core/settings/control_scheme.hpp"
#include "core/settings/sensitivity.hpp"
#include "core/net/client_session.hpp"
#include "platform/ctr/audio.hpp"
#include "platform/ctr/guest_play.hpp"
#include "platform/ctr/host_play.hpp"
#include "platform/ctr/net_play.hpp"
#include "platform/ctr/network.hpp"
#include "platform/ctr/heap.hpp"
#include "platform/ctr/menu.hpp"
#include "platform/ctr/overlay.hpp"
#include "platform/ctr/probe.hpp"
#include "platform/ctr/progress_screen.hpp"
#include "platform/ctr/renderer.hpp"
#include "core/audio/block_sound.hpp"
#include "core/audio/effect_preload.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/entity/falling_block.hpp"
#include "core/entity/primed_tnt.hpp"
#include "core/gui/chat_log.hpp"
#include "core/entity/item_entity.hpp"
#include "core/entity/arrow.hpp"
#include "core/entity/boat.hpp"
#include "core/entity/minecart.hpp"
#include "core/entity/mob.hpp"
#include "core/entity/mob_spawn.hpp"
#include "core/entity/mob_spawner.hpp"
#include "core/world/sign_store.hpp"
#include "core/entity/painting.hpp"
#include "core/entity/particle.hpp"
#include "core/entity/entity_boxes.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/io/volume_info.hpp"
#include "core/block/collision.hpp"
#include "core/entity/block_contact.hpp"
#include "core/entity/fire_entry.hpp"
#include "core/entity/player_body.hpp"
#include "core/entity/player_vitals.hpp"
#include "core/entity/sprint_gesture.hpp"
#include "core/item/block_breaking.hpp"
#include "core/item/tool_rules.hpp"
#include "core/item/registry.hpp"
#include "core/tick/behaviour.hpp"
#include "core/tick/display.hpp"
#include "core/entity/ray_trace.hpp"
#include "core/texture/compass_fx.hpp"
#include "core/texture/fluid_fx.hpp"
#include "core/texture/texture_fx.hpp"
#include "core/tick/tick_timer.hpp"
#include "core/render/held_item.hpp"
#include "core/render/world_streamer.hpp"
#include "core/util/coord_text.hpp"
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
// How long a body put at a spawn point will wait for the column under it before
// it gives up and moves anyway. Ten seconds of ticks; see `spawnWaitTicks`.
constexpr int kSpawnWaitTicks = 200;

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

// **A two-axis reading off whichever device the Controls row named**, in the
// circle pad's own convention: +x right, +y up (away from the player), each
// clamped to -1..1. Everything downstream -- walking, flying, turning -- is
// written against that convention and never against a button.
//
// The d-pad is a button pair per axis and so is only ever -1, 0 or 1. That is
// not a limitation to apologise for: it is a keyboard, which is what a1.1.2 was
// played on, and `moveFlying` already clamps the diagonal it produces the same
// way it clamps the original's two arrow keys. A rate-based look off it is a
// turn at one speed, which is why the Sensitivity row matters more under the
// Old schemes than under the New one.
//
// **`held` rather than the hardware**, for the d-pad alone: the frame loop
// zeroes `held` while a screen is up, so the d-pad is silenced by the same rule
// that silences every other button and needs no guard of its own. The two
// sticks are read straight from libctru and do need one -- see the call sites.
//
// **SELECT takes the d-pad back.** SELECT + d-pad tunes the 3D, and under the
// two Old schemes that d-pad is also walking or turning; without this the world
// would spin while the stereo was being dialled in.
void readStick(mc::settings::Stick stick, u32 held, float* x, float* y)
{
    if (stick == mc::settings::Stick::Dpad) {
        if ((held & KEY_SELECT) != 0) {
            *x = 0.0f;
            *y = 0.0f;
            return;
        }
        *x = float(((held & KEY_DRIGHT) != 0 ? 1 : 0) - ((held & KEY_DLEFT) != 0 ? 1 : 0));
        *y = float(((held & KEY_DUP) != 0 ? 1 : 0) - ((held & KEY_DDOWN) != 0 ? 1 : 0));
        return;
    }

    circlePosition pad;
    if (stick == mc::settings::Stick::CStick) {
        hidCstickRead(&pad);
    } else {
        hidCircleRead(&pad);
    }
    *x = axis(pad.dx);
    *y = axis(pad.dy);
}

// Free flight, which is now **Spectator's** movement rather than the only one.
// It stays exactly as honest as it was: no body, no collision, no gravity, and
// it is offered under a name that says so.
//
// Up and down are B and Y rather than R and L, because the shoulders are the
// two mouse buttons now. See the controls table in docs/status.md.
void flyCamera(ctr::Camera& camera, float dt, bool sprint, mc::settings::ControlScheme scheme,
               u32 stickHeld)
{
    float px = 0.0f;
    float pz = 0.0f;
    // The caller's `held` with the d-pad taken out if a screen has claimed it;
    // the B and Y below stay on the hardware, which is where this function has
    // always read them.
    readStick(mc::settings::moveStick(scheme), stickHeld, &px, &pz);

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

    // Not the world's height: Spectator may fly above or below it, and a
    // teleport may put it there. See mc::kCameraYLimit.
    camera.y = camera.y < -mc::kCameraYLimit
                   ? -mc::kCameraYLimit
                   : (camera.y > mc::kCameraYLimit ? mc::kCameraYLimit : camera.y);
}

// Puts the body under a camera that moved without it -- Spectator handing back
// to a body mode, or a teleport. `camera.y` is the eye, which is what `eyeY()`
// hands out, so the feet are that minus the offset -- the same conversion world
// entry makes from a saved `Pos[1]`.
//
// **A crouching camera is 0.08 below the eye it came from**, so the drop is
// added back before the conversion rather than being quietly banked as a
// position: without it every teleport taken while sneaking would sink the
// player that much, the same shape of bug as the 1.62 that used to be added
// twice on world entry.
//
// A camera that was flying has no velocity worth inheriting, and a banked
// `fallDistance` would be cashed in the moment the body touched down. `setFeet`
// has already snapped the interpolation, so the first frame draws where the
// camera already was rather than sliding there.
void placeBodyAtEye(mc::entity::PlayerBody& body, const ctr::Camera& camera, bool sneaking)
{
    const double eye = camera.y + (sneaking ? mc::entity::kSneakEyeDrop : 0.0);
    body.setFeet(camera.x, eye - double(mc::entity::kEyeHeight), camera.z);
    body.motionX = 0.0;
    body.motionY = 0.0;
    body.motionZ = 0.0;
    body.fallDistance = 0.0f;
    body.onGround = false;
}

// `Minecraft.o()` -- **Respawn**, which throws the dead player away and builds a
// new one. `dm`'s constructor stands it at `spawnX + 0.5, spawnY + 1,
// spawnZ + 0.5` through `setLocationAndAngles`, and that method adds `yOffset`
// to the y it is handed -- so `spawnY + 1` is where the *feet* go, not the eye.
// Size, eye height and motion all come back to the constructor's with the fresh
// body.
//
// **The lift out of the ground is not done here**, though `bi.q()` runs on the
// very next line of `Minecraft.o()`. See `liftIntoTheWorld`: on this console the
// column the spawn point is in may not be resident yet, and a lift against a
// world that is all air lifts nothing.
void respawnBody(mc::entity::PlayerBody& body, const mc::world::LevelData& level)
{
    body = mc::entity::PlayerBody{};
    body.setFeet(double(level.spawnX) + 0.5, double(level.spawnY) + 1.0,
                 double(level.spawnZ) + 0.5);
}

// `kh.q()` -- **preparePlayerToSpawn**, the tail of it: walk the body up until
// nothing collides with it, then stop it dead and level its pitch.
//
// a1.1.2 runs this the moment a player is built, which it can because the world
// it is built into has already generated the spawn area. Here the columns
// stream in behind the player, so the call is owed rather than made: the frame
// loop holds the body still and runs this on the first tick its own column is
// resident. A lift made any earlier would find air, decide there was nothing to
// climb out of, and leave the player to be buried by the terrain arriving
// underneath them -- which in Survival is a suffocation the player never sees
// the cause of.
void liftIntoTheWorld(mc::entity::PlayerBody& body, const mc::tick::TickWorld& world,
                      ctr::Camera* camera)
{
    // **The whole height of the world, not `liftOutOfGround`'s default eight.**
    // `kh.q()` climbs until nothing collides and has no bound at all, and the
    // eight-block default is a teleport's "did I land in a wall", which is a
    // different question. A world whose spawn point predates the search in
    // core/world/spawn_point.hpp has `spawnY` at 64 with whatever the ground at
    // x = 0, z = 0 happens to be doing on top of it, and eight blocks of climb
    // out of a hillside leaves the player just as buried as before.
    body.liftOutOfGround(world, 128);
    body.motionX = 0.0;
    body.motionY = 0.0;
    body.motionZ = 0.0;
    body.fallDistance = 0.0f;
    if (camera != nullptr) {
        camera->pitch = 0.0f;
    }
}

// The movement device and the two body buttons, as the original's heading
// inputs. **Which device that is comes off the Controls row** -- see
// `readStick`, which reports all of them in one convention so nothing below
// this line knows the difference.
//
// **Strafe is negated.** `moveFlying` sends a positive strafe to +X at yaw 0,
// and yaw 0 faces +Z, so +X is the player's *left*; the stick's positive x is
// their right. One of the two has to flip and it is this one.
mc::entity::PlayerInput readBodyInput(const ctr::Camera& camera, u32 held, bool sneaking,
                                      mc::settings::ControlScheme scheme)
{
    float px = 0.0f;
    float pz = 0.0f;
    readStick(mc::settings::moveStick(scheme), held, &px, &pz);

    mc::entity::PlayerInput input;
    input.strafe = -px;
    input.forward = pz;
    input.yawDegrees = camera.yaw * 180.0f / kPi;
    input.jump = (held & KEY_B) != 0;
    // **Sneak is a toggle and is handed in**, not read off Y here: it is the
    // one movement input this build holds state for, because a crouch is a
    // stance rather than a key. See `sneaking` in the world loop.
    input.sneak = sneaking;
    // ...and then slowed, which a1.1.2 does to the stick in this very function
    // -- `MovementInputFromOptions` scales `moveStrafe` and `moveForward` by
    // 0.3 once the sneak key is read. See `applySneakSlowdown`.
    mc::entity::applySneakSlowdown(input);
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

    // **The animals**, which are the first thing in this build that is a mob
    // and not the player -- so a stone pressure plate finally has something
    // other than the player to tell apart from a dropped stack.
    const mc::entity::MobSystem* mobs = nullptr;

    // The same pool again, writable, and the world the drop lands in. Two
    // fields rather than casting the const away, because the two seams really
    // do ask different things: the plate wants to *read* what is standing on
    // it and a broken torch wants to *add* to the pool.
    mc::entity::ItemEntitySystem* mutableItems = nullptr;
    const mc::tick::TickWorld* world = nullptr;
    mc::entity::FallingBlockSystem* falling = nullptr;
    mc::entity::PrimedTntSystem* primedTnt = nullptr;
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

    // **An animal answers two of the three.** `js.b` selects `EntityLiving`,
    // which a cow is, and `js.c` selects `EntityPlayer`, which it is not -- so
    // a cow presses a stone plate and not a wooden one's player-only cousin.
    if (filter != mc::tick::EntityFilter::Players && scene->mobs != nullptr) {
        for (int i = 0; i < scene->mobs->count(); ++i) {
            const mc::entity::Mob& mob = (*scene->mobs)[i];
            if (mob.alive && mob.body.box.intersects(box)) {
                return true;
            }
        }
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

// **A stack thrown with its own velocity** -- a broken chest's spill. The same
// pool and the same world as the block drop above; see
// `TickWorld::openContainer`: a chest, a workbench or a furnace took a right
// click. **Held, not opened here**, because the click lands inside the edit
// path with the Overlay's screen state halfway through a frame; the loop opens
// it straight after the edit returns.
struct PendingContainer {
    bool open = false;
    mc::tick::TickWorld::ContainerKind kind = mc::tick::TickWorld::ContainerKind::Chest;
    mc::i32 x = 0;
    int y = 0;
    mc::i32 z = 0;

    // **A chest minecart instead of a cell**, when it is not zero. It is a
    // second field rather than a fourth `ContainerKind` because that enum is
    // the world's seam and a cart is not a block; see
    // `item::EntityInteraction::opensMinecartChest`.
    mc::u32 cart = 0;
};

void requestContainer(void* ctx, mc::tick::TickWorld::ContainerKind kind, mc::i32 x, int y,
                      mc::i32 z)
{
    PendingContainer& pending = *static_cast<PendingContainer*>(ctx);
    pending.open = true;
    pending.kind = kind;
    pending.x = x;
    pending.y = y;
    pending.z = z;
    pending.cart = 0;
}

// `TickWorld::spawnItemStack`.
void spawnItemStack(void* ctx, double x, double y, double z, mc::u16 item, int count,
                    mc::i16 damage, double motionX, double motionY, double motionZ)
{
    EntityScene* scene = static_cast<EntityScene*>(ctx);
    if (scene->mutableItems == nullptr || scene->world == nullptr) {
        return;
    }
    scene->mutableItems->spawnMoving(*scene->world, x, y, z, mc::item::ItemId(item), count,
                                     damage, motionX, motionY, motionZ);
}

// `ic.j_()` reaching the streamer: a furnace's or a chest's contents changed,
// so the column is written at the next save. No remesh -- nothing drawn moved.
void markTileEntityColumn(void* ctx, mc::i32 x, mc::i32 z)
{
    static_cast<render::WorldStreamer*>(ctx)->markColumnModified(x, z);
}

// `cn.a(DDDLjava/lang/String;FF)V` -- what a block behaviour asks the mixer
// for. The engine outlives the world, so the context is the engine itself
// rather than the scene.
void playTickSound(void* ctx, const char* key, double x, double y, double z, float volume,
                   float pitch)
{
    static_cast<mc::audio::SoundEngine*>(ctx)->playSoundAt(key, x, y, z, volume, pitch);
}

// `cn.a(Ljava/lang/String;III)V` -- World.playRecord, the jukebox's one seam.
// A null track is the stop, which is `BlockJukeBox.ejectRecord`'s own call; the
// engine takes both. The cell's centre is where the source stands, as
// `RenderGlobal.playRecord` passes `i, j, k` straight through.
void playRecordSound(void* ctx, const char* track, mc::i32 x, int y, mc::i32 z)
{
    static_cast<mc::audio::SoundEngine*>(ctx)->playRecord(track, double(x), double(y),
                                                          double(z));
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

// `q`'s `new jd(...)`, and the cell it names is already air. A full pool loses
// the blast; the `random.fuse` that goes with a broken block is played by
// `core/tick/drop.cpp` either way, which is the one place this differs from
// the original and is argued there.
bool spawnPrimedTnt(void* ctx, mc::i32 x, int y, mc::i32 z, int fuse)
{
    EntityScene* scene = static_cast<EntityScene*>(ctx);
    if (scene->primedTnt == nullptr || scene->world == nullptr) {
        return false;
    }
    return scene->primedTnt->spawn(*scene->world, x, y, z, fuse);
}

// `cn.a(IIILic;)V` and `cn.l(III)V` -- World.setBlockTileEntity and
// World.removeBlockTileEntity. **Two stores now**, so the context is both of
// them rather than one; a position is in at most one of the two and neither
// store minds being asked about a block it has never heard of.
//
// The other three sinks are the column's: a column arriving fills the stores
// out of its decoded `TileEntities`, a column being saved takes their state
// back, and a column leaving empties them of it. Chests and furnaces have no
// store and need none -- nothing in this build opens one -- and they are
// carried in the column itself, reconciled against the blocks at the save.
// See core/world/tile_entity.hpp.
struct TileEntities {
    mc::world::SignStore* signs = nullptr;
    mc::entity::MobSpawnerStore* spawners = nullptr;

    // Only the spawner has anything to build here: `jt.e` hands the world a
    // fresh `bd`, and a sign's tile entity is built by the placement itself
    // because it has to refuse the click when the heap is full (see
    // core/item/use.cpp).
    static void added(void* ctx, mc::i32 x, int y, mc::i32 z)
    {
        TileEntities& self = *static_cast<TileEntities*>(ctx);
        if (self.spawners != nullptr) {
            self.spawners->put(x, y, z);
        }
    }

    static void removed(void* ctx, mc::i32 x, int y, mc::i32 z)
    {
        TileEntities& self = *static_cast<TileEntities*>(ctx);
        if (self.signs != nullptr) {
            self.signs->erase(x, y, z);
        }
        if (self.spawners != nullptr) {
            self.spawners->erase(x, y, z);
        }
    }

    static void adopted(void* ctx, const mc::world::ChunkColumn& column)
    {
        TileEntities& self = *static_cast<TileEntities*>(ctx);
        if (self.spawners != nullptr) {
            mc::entity::readMobSpawners(column.tileEntities, *self.spawners);
        }
        if (self.signs != nullptr) {
            mc::world::readSigns(column, *self.signs);
        }
    }

    static void saving(void* ctx, mc::world::ChunkColumn& column)
    {
        TileEntities& self = *static_cast<TileEntities*>(ctx);
        // **The blocks first**, which is `ga.d`'s heal and `jt.b`'s removal in
        // one pass: a chest placed this session gains an empty entry, and one
        // broken by a creeper loses its. Then the two stores write over what
        // they own, so a spawner the store never took keeps the mob the file
        // gave it rather than being reset to `bd`'s default.
        mc::world::reconcileTileEntities(column);
        if (self.signs != nullptr) {
            mc::world::writeSigns(*self.signs, column);
        }
        if (self.spawners != nullptr) {
            mc::entity::writeMobSpawners(*self.spawners, column);
        }
    }

    static void dropped(void* ctx, mc::i32 chunkX, mc::i32 chunkZ)
    {
        TileEntities& self = *static_cast<TileEntities*>(ctx);
        if (self.signs != nullptr) {
            self.signs->eraseColumn(chunkX, chunkZ);
        }
        if (self.spawners != nullptr) {
            self.spawners->eraseColumn(chunkX, chunkZ);
        }
    }
};

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

// `ItemStack.hitEntity` and `hitBlock` as the hand feels them: the held stack
// wears by `amount`, and one that wears through is taken out of the hand --
// `destroyCurrentEquippedItem`. Survival only; see core/item/tool_rules.hpp.
void wearHeld(ctr::Overlay& overlay, int amount)
{
    if (amount <= 0) {
        return;
    }
    mc::item::Inventory& inventory = overlay.editInventory();
    mc::item::ItemStack& stack = inventory.main[inventory.selected];
    if (stack.empty()) {
        return;
    }
    if (mc::entity::wearStack(stack, amount)) {
        stack.id = mc::item::kEmptyItemId;
        stack.count = 0;
        stack.damage = 0;
    }
    overlay.inventoryEdited();
}

// `stackSize -= count`, and a stack that reaches nothing is taken out of the
// hand -- what every Survival use method does to the stack it was called with.
void spendHeld(ctr::Overlay& overlay, int count)
{
    if (count <= 0) {
        return;
    }
    mc::item::Inventory& inventory = overlay.editInventory();
    mc::item::ItemStack& stack = inventory.main[inventory.selected];
    if (stack.empty()) {
        return;
    }
    const int left = int(stack.count) - count;
    if (left <= 0) {
        stack.id = mc::item::kEmptyItemId;
        stack.count = 0;
        stack.damage = 0;
    } else {
        stack.count = mc::i8(left);
    }
    overlay.inventoryEdited();
}

void editBlocks(render::WorldStreamer& world, render::ChunkRenderer& chunks,
                const ctr::Camera& camera, const mc::entity::PlayerBody& body,
                ctr::Overlay& overlay, u32 down, u32 heldButtons, i64 nowTick,
                i64* lastEditTick, const mc::item::Effects& effects,
                mc::render::HeldItemState* hand, const ChatSink& chat,
                mc::item::BlockBreaker* breaker, mc::entity::PlayerVitals* vitals,
                ctr::NetPlay* net, ctr::HostPlay* host, PendingContainer* pendingContainer)
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
        if (net != nullptr) {
            net->swing();
        }
        // **An entity in the way is hit instead of the block behind it**, the
        // same precedence the right-click branch below already gives one. This
        // is the only way to break a boat, a cart or a painting, which is why
        // none of the three left anything on the ground before it existed. See
        // core/item/use.hpp.
        if (target.found()) {
            // **On a session next door the hit is sent, not applied.** The
            // animal belongs to the other console: running the damage here as
            // well would be a second world arguing with the first, and the
            // host's next position update would overwrite everything except
            // the health it had already taken off. See `packet::UseEntity`.
            if (net != nullptr && net->useEntityAllowed()
                && target.kind == mc::item::EntityTarget::Kind::Mob
                && effects.entities.mobs != nullptr) {
                const i32 targetId = effects.entities.mobs->at(target.index).entityId;
                if (targetId != 0) {
                    net->attackEntity(targetId, int(heldItem));
                    // The tool still wears here: this console owns its own pack
                    // in a1.1.2's multiplayer and pushes it back every second.
                    if (overlay.gamemode() == mc::settings::Gamemode::Survival) {
                        wearHeld(overlay, mc::item::wearOnHit(heldItem));
                    }
                    return;
                }
            }
            // **Another player, which no console resolves for itself.** The
            // blow is sent to whoever is running them and their own vitals
            // decide what it costs -- there is no health on this wire. A host
            // sends it through its server; a guest sends it up the stream it
            // is already on. See `packet::UseEntity` and `net::IncomingHit`.
            if (target.kind == mc::item::EntityTarget::Kind::Player
                && effects.entities.players != nullptr) {
                const i32 targetId = effects.entities.players->player(target.index).id;
                bool sent = false;
                if (net != nullptr && net->useEntityAllowed()) {
                    net->attackEntity(targetId, int(heldItem));
                    sent = true;
                } else if (host != nullptr) {
                    sent = host->attackPlayer(targetId, int(heldItem));
                }
                if (sent && overlay.gamemode() == mc::settings::Gamemode::Survival) {
                    wearHeld(overlay, mc::item::wearOnHit(heldItem));
                }
                return;
            }
            // The attacker's position, which only a living target reads: a
            // struck animal is knocked away from whoever hit it.
            mc::item::Attacker attacker;
            attacker.present = true;
            attacker.x = camera.x;
            attacker.z = camera.z;
            // **A Creative punch does not start a fight.** The same player the
            // target search refuses (see below, and `MobPlayer::targetable`)
            // must not be handed a target through the back door of having hit
            // something.
            attacker.provokes = overlay.gamemode() != mc::settings::Gamemode::Creative;
            mc::item::attackEntity(*tickWorld, target, heldItem, effects, attacker);
            // `bi.a(Lkh;)V`: **the stack wears on anything living it hits**,
            // whether or not the hit got past the target's window -- the call
            // follows `attackEntityFrom` without looking at its answer. A boat,
            // a cart and a painting are not `ge` and cost it nothing.
            if (overlay.gamemode() == mc::settings::Gamemode::Survival
                && target.kind == mc::item::EntityTarget::Kind::Mob) {
                wearHeld(overlay, mc::item::wearOnHit(heldItem));
            }
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
        //
        // **Except in Survival**, where it is taken away: a press is
        // `Minecraft.clickMouse`'s block branch -- `cn.i` puts out fire on the
        // struck face, then `nj.a(IIII)` clicks the block, which opens a door,
        // flips a lever, lights redstone ore, and breaks only what one tick of
        // progress already finishes. The held button's per-tick progress is in
        // the tick loop. See core/item/block_breaking.hpp.
        if (overlay.gamemode() == mc::settings::Gamemode::Survival && breaker != nullptr) {
            mc::render::WorldStreamer::RenderBracket draws(world, chunks);
            mc::item::extinguishFireOnFace(*tickWorld, hit.x, hit.y, hit.z, int(hit.face));
            mc::item::BreakContext ctx{*tickWorld, overlay.editInventory(), effects};
            ctx.eyeInWater = mc::entity::playerEyeInWater(*tickWorld, body);
            ctx.onGround = body.onGround;
            const auto clicked = tickWorld->blockAt(hit.x, hit.y, hit.z);
            if (breaker->click(ctx, hit.x, hit.y, hit.z, int(hit.face))) {
                overlay.inventoryEdited();
            }
            if (net != nullptr) {
                net->digStart(hit.x, hit.y, hit.z, int(hit.face),
                              tickWorld->blockAt(hit.x, hit.y, hit.z) != clicked,
                              int(heldItem));
            }
            return;
        }
        world.breakBlock(chunks, hit.x, hit.y, hit.z, effects);
        return;
    }
    if ((acting & KEY_L) == 0) {
        return;
    }

    // **An entity in the way is asked instead of the block**, which is
    // `Minecraft.clickMouse`'s own order: `objectMouseOver` is the entity, so
    // `interact` runs and the block behind it is never clicked -- even when
    // `interact` refuses. A boat or a plain cart takes the player aboard, a
    // chest cart opens its slots and a furnace cart takes coal, and any of
    // those ends the click.
    if (target.found()) {
        const mc::item::EntityInteraction answer = mc::item::interactWithEntity(
            *tickWorld, target, effects.entities, heldItem, body.x, body.z);
        if (answer.taken) {
            // **A chest cart's screen cannot come through the container sink**,
            // which names a cell; the cart hands back its own id instead. Held
            // rather than opened here, for the reason `PendingContainer` gives:
            // this runs inside the edit path with the Overlay's screen state
            // halfway through a frame.
            if (answer.opensMinecartChest != 0 && pendingContainer != nullptr) {
                pendingContainer->open = true;
                pendingContainer->cart = answer.opensMinecartChest;
            }
            // **A furnace cart swallowed the coal**, and Survival pays for it
            // the same way a placement does.
            if (answer.spentFuel && overlay.gamemode() == mc::settings::Gamemode::Survival) {
                spendHeld(overlay, 1);
            }
            // **A bucket becomes a milk bucket and a saddle becomes nothing**,
            // which is the same "what did the stack turn into" the item path
            // below already handles -- see `ItemUse::becomes`.
            // **Creative's bucket stays what it is**, the same rule as the
            // pour and fill below.
            if (answer.becomes != heldItem
                && (overlay.gamemode() == mc::settings::Gamemode::Survival
                    || mc::item::def(heldItem).bucket == mc::item::ItemDef::kNotABucket)) {
                hand->reequip();
                // **A saddle put on a pig is spent in Survival** -- `jw.b`
                // decrements the stack -- which is the one "becomes nothing"
                // `replaceHeldItem` deliberately ignores for Creative.
                if (answer.becomes == 0
                    && overlay.gamemode() == mc::settings::Gamemode::Survival) {
                    spendHeld(overlay, 1);
                } else {
                    overlay.replaceHeldItem(answer.becomes);
                }
            }
            return;
        }
    }

    // **Everything the right hand does, and it is not only placing.**
    // `PlayerController.onPlayerRightClick` asks the block first -- which is
    // what opens a door, flicks a lever and presses a button -- and only then
    // hands the click to the item. All of it is `item::rightClick`, in core and
    // under test; what is left here is the button and the renderer.
    //
    // **Creative spends nothing; Survival spends what the item's own class
    // does.** `itemTook` is what tells a placement from a door opening -- only
    // the first costs the stack -- and the cost itself is the item table's
    // answer. See item::spendOnUse.
    // **Marked before either entry point runs**, so a click the heap turned
    // down can be told apart from one that simply had nowhere to go. See
    // item::RefusalMark.
    const mc::item::RefusalMark refusals = mc::item::markRefusals(effects.entities);
    const bool survival = overlay.gamemode() == mc::settings::Gamemode::Survival;

    // `nj.a(dm, cn, ev, ...)`: every right click on a block goes to the server
    // before the client tries it, whether or not the client makes anything of
    // it. The server's placing is the one that counts; see
    // core/net/pending_edits.hpp.
    if (net != nullptr && hit.hit && !target.found()) {
        net->place(int(heldItem), hit.x, hit.y, hit.z, int(hit.face));
    }

    bool itemTook = false;
    if (hit.hit && !target.found()
        && world.rightClick(chunks, heldItem, hit, body.box, camera.yaw * 180.0f / kPi,
                            effects, &itemTook)) {
        // **Only when the click was taken**, which is the other half of
        // `clickMouse`'s split: `if (onPlayerRightClick(...)) swingItem()`.
        // Waving at a wall that refuses the block does not swing.
        hand->swing();
        if (survival && itemTook) {
            wearHeld(overlay, mc::item::wearOnUse(heldItem));
            spendHeld(overlay, mc::item::spendOnUse(heldItem));
        }
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
                // `nv` closing calls `ob.j_()`, which is `ga.f()` --
                // setChunkModified. Text is not a block, so nothing else marks
                // the column, and without this the first line typed onto a
                // sign would never reach the card.
                const mc::world::SignText& sign = (*effects.entities.signs)[placedSign];
                world.markColumnModified(sign.x, sign.z);
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
    if (survival) {
        // `oj.a(Lev;Lcn;Ldm;)Lev;` -- **ItemFood**: one off the stack and the
        // health back, on every right-click and whatever the health already is.
        const int heals = mc::item::foodHeals(heldItem);
        if (heals > 0) {
            spendHeld(overlay, 1);
            if (vitals != nullptr) {
                vitals->heal(heals);
            }
            return;
        }
        // `jg.a(...)` -- **the bow takes an arrow from anywhere in the
        // inventory first, and does nothing at all without one.** The arrow is
        // gone whether or not the shot's entity found room, because the jar
        // ignores `spawnEntityInWorld`'s answer.
        if (mc::item::def(heldItem).spawns == mc::item::SpawnsEntity::Arrow) {
            if (!overlay.editInventory().consumeOne(mc::item::ItemId(mcver::Item::Arrow))) {
                return;
            }
            overlay.inventoryEdited();
        }
    }
    const mc::item::ItemUse used = world.useItem(chunks, heldItem, camera.x, camera.y,
                                                 camera.z, double(dx), double(dy),
                                                 double(dz), effects);
    // The boat's own `stackSize--`, on the same success path. A bucket turns
    // into something else below instead, and the bow spent its arrow above.
    if (survival && used.changed && used.becomes == heldItem) {
        spendHeld(overlay, mc::item::spendOnUse(heldItem));
    }
    // Either entry point may have been the one refused -- a cart or a sign
    // goes through the first, a boat or an arrow through the second -- and
    // both have run by here whenever neither was taken.
    chat.limitReached(mc::item::refusedSince(effects.entities, refusals));
    // **`onItemRightClick` raises the item again and never swings the arm.**
    // Both are in `clickMouse`'s tail: it calls `ItemRenderer.resetEquippedProgress`
    // when the stack it got back is not the stack it handed in -- a bucket
    // becoming a water bucket -- and there is no `swingItem` on this path at
    // all.
    //
    // **Creative's bucket stays what it is.** The water still goes down and
    // still comes up -- it is the hand that does not change, so an empty
    // bucket keeps scooping and a full one keeps pouring, the way every later
    // Creative mode has it. a1.1.2 has no Creative, so this is ours; Survival
    // keeps `ItemBucket`'s swap.
    mc::item::ItemId becomes = used.becomes;
    if (!survival && mc::item::def(heldItem).bucket != mc::item::ItemDef::kNotABucket) {
        becomes = heldItem;
    }
    if (becomes != heldItem) {
        hand->reequip();
    }
    overlay.replaceHeldItem(becomes);
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
void lookWithTouch(ctr::Camera& camera, bool* dragging, touchPosition* last, int top,
                   float gain)
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
        // Radians per pixel of drag at the default sensitivity. `gain` is the
        // Options row, and it is 1.0 there -- see core/settings/sensitivity.hpp
        // for why a1.1.2's own curve lands exactly on unity at 100%, which is
        // what lets this constant stay the number it was tuned to.
        constexpr float kSensitivity = 0.012f;
        camera.yaw += float(int(touch.px) - int(last->px)) * kSensitivity * gain;
        camera.pitch += float(int(touch.py) - int(last->py)) * kSensitivity * gain;
        clampPitch(camera);
    }

    *last = touch;
    *dragging = true;
}

// **Turning the view with a stick**, whichever one the Controls row named.
//
// The C-stick was the first and is still the New 3DS scheme's: it arrives
// through ir:rst rather than hid -- the same service a Circle Pad Pro reports
// through, so an old 3DS with one attached gets it for free. The Old schemes
// point this at the circle pad or the d-pad instead, and nothing else about it
// changes.
//
// Both axes are rate rather than position: the further it is pushed the faster
// the view turns, which is what a stick with a return spring wants, and what
// makes a d-pad usable here at all -- a held direction is a steady turn rather
// than a jump.
void lookWithStick(ctr::Camera& camera, float dt, float gain, mc::settings::Stick stick,
                   u32 held)
{
    // Radians a second at full deflection. About 100 degrees, which is a little
    // brisker than the original's default mouse sensitivity and reads as normal
    // on a stick this short.
    constexpr float kTurnRate = 1.8f;

    float x = 0.0f;
    float y = 0.0f;
    readStick(stick, held, &x, &y);

    camera.yaw += x * kTurnRate * gain * dt;
    // Push up, look up. Positive pitch looks down, so this subtracts.
    camera.pitch -= y * kTurnRate * gain * dt;
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
            // **Core 2, one step above the generation worker already there.**
            //
            // It used to be the same priority as generation, on the argument
            // that the decoder does not need to win a race it is not in: it
            // wants ~15% of a core against a deep ring, and the scheduler would
            // round-robin the two. **That last clause was wrong, and it is why
            // the music skipped under load.** The ARM11 kernel is SCHED_FIFO --
            // no round-robin, no time slice (3dbrew, Multi-threading) -- so a
            // runnable thread never displaces one of equal priority, and
            // `WorldStreamer::workerMain` takes its own next job the moment it
            // finishes one. A full slate is a thread that does not block, and
            // the decoder got only the gaps generation's card reads left.
            //
            // So it asks for a step up. The risk that argued against it is real
            // and is handled rather than avoided: the kernel refuses a priority
            // numerically below what the process was granted, and a refused
            // `threadCreate` is silent -- so the refusal falls back to the main
            // thread's own priority on the *same core* before it falls through
            // to core 0, which is the outcome that would actually have hurt.
            //
            // **The fallback still wins**, because the other half of the change
            // is that generation now takes core 2 one step *below* the main
            // thread rather than at it (see the Generation branch below). On a
            // core with nothing else on it that costs generation nothing, and it
            // means the decoder outranks it whether or not this console grants
            // the step up. Generation pays a few percent of a core it is not
            // frame-coupled to, and only while a track is decoding.
            Thread thread =
                threadCreate(entry, arg, kAudioStackBytes, mainPriority - 1, 2, false);
            if (thread == nullptr) {
                thread = threadCreate(entry, arg, kAudioStackBytes, mainPriority, 2, false);
            }
            if (thread != nullptr) {
                return thread;
            }
            // A New 3DS whose exheader did not grant core 2 falls through to
            // the Old 3DS policy, exactly as generation does.
        }

        // **Old 3DS: core 0, one step below the main thread** -- and one step
        // *above* the I/O and net threads, which used to share this slot.
        //
        // Below the main thread because the steady state must cost a frame
        // nothing: it runs in the slack the main thread leaves while blocked on
        // VBlank. Above the other two because under SCHED_FIFO a tie is not
        // shared -- whichever of them is already running keeps core 0 until it
        // blocks, so a chunk being inflated could hold the whole of that slack
        // while the only thread here with a deadline waited behind it.
        //
        // That still leaves the case the slack itself runs out, which is a
        // frame already over budget and is what made the music skip. Nothing
        // that lives below the main thread can fix that, so the decode thread
        // lifts itself above the main thread for as long as it takes to refill
        // and drops straight back; see kBoostBelow in ctr/audio.hpp.
        //
        // A 3DSX has no other core to move any of this to, so CONTRIBUTING's
        // "no decompression on core 0" is met in substance rather than
        // literally: never on the main thread, bounded per wake, and a ring
        // deep enough that a missed frame is inaudible.
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
        // Two steps rather than 0x3F so it is not sitting behind every other
        // low-priority thread in the process for the slack it is meant to use --
        // and two rather than one so the audio decoder, which is the only thread
        // on this core with a deadline, is in front of it. Under SCHED_FIFO the
        // two cannot share a priority: whichever started first would hold core 0
        // until it blocked. This thread is blocked in IPC nearly always, so the
        // step costs it nothing it can measure.
        const s32 priority = mainPriority + 2 > 0x3F ? 0x3F : mainPriority + 2;
        return threadCreate(entry, arg, kIoStackBytes, priority, 0, false);
    }

    if (role == mc::WorkerRole::Net) {
        // **A multiplayer session sits where the I/O thread does**, one step
        // under the main thread on core 0, and for the same reason: it spends
        // its life blocked in `poll`. What it does when it wakes -- parsing, and
        // inflating a column -- has to keep pace with the server, which a
        // bottom-priority thread living on the frame's leftovers would not. Core
        // 2 stays the generator's, so `gWorkerOnCore2` still describes the
        // generator. The generation worker's stack size, because building a
        // column puts a section's worth of scratch on it.
        //
        // Two steps down rather than one, for the reason the I/O thread is:
        // inflating a column must not hold core 0 in front of the audio
        // decoder's deadline, and a session waiting on the network is not
        // waiting on the CPU.
        const s32 priority = mainPriority + 2 > 0x3F ? 0x3F : mainPriority + 2;
        return threadCreate(entry, arg, kWorkerStackBytes, priority, 0, false);
    }

    if (gWorkerIsNew3DS) {
        // Core 2 has one other thread on it and generation is the one that can
        // afford to be polite: the audio decoder is there too, and under
        // SCHED_FIFO equal priority is not a share -- `workerMain` takes its own
        // next job the moment it finishes one, so a full slate is a thread that
        // never blocks and never lets the decoder in. That is what made the
        // music skip under load.
        //
        // **One step below the main thread, and it costs nothing.** Nothing else
        // is on this core, so a lower number would buy generation no more of it
        // than it already gets; all the step does is put the thread with the
        // deadline in front. It also means the fix does not depend on the
        // kernel granting a priority above the process's own, which is the half
        // of the arrangement that can be refused. See ctr/audio.hpp.
        const s32 generationPriority =
            mainPriority + 1 > 0x3F ? 0x3F : mainPriority + 1;
        Thread thread =
            threadCreate(entry, arg, kWorkerStackBytes, generationPriority, 2, false);
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
// `net` is the session this console *joined*, and `host` the one it is
// running. They are never both set: a world is either somebody else's or this
// console's, and hosting changes nothing about how the world is played -- see
// platform/ctr/host_play.hpp.
int runGame(const ctr::MenuChoice& choice, ctr::Menu& menu, mc::audio::SoundEngine& sound,
            ctr::NdspBackend& audio, bool isNew3DS, bool haveCstick, ctr::NetPlay* net,
            ctr::HostPlay* host, ctr::GuestPlay* guest)
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

    // **Before the world opens**, because the generator is built inside open()
    // and reads this once. A guest's terrain then reaches it without the
    // streamer or the generator knowing a session exists.
    if (host != nullptr) {
        world.setTerrainSource(host->terrainSource());
    }

    // **What the other end actually sends.** A Java server sends ten chunks
    // each way; the console next door sends fewer, and the difference is not a
    // preference -- it decides whether a cell with no column is one to wait for
    // or one to mesh against. See WorldStreamer::setServerViewRadius.
    if (guest != nullptr) {
        world.setServerViewRadius(mc::net::WorldServer::kViewDistance);
    }

    // **A multiplayer world opens with nothing in it**: the server fills it. See
    // WorldStreamer::openRemote and platform/ctr/net_play.hpp.
    const bool opened = net != nullptr
                            ? world.openRemote(config.meshDistance)
                            : world.open(choice.worldPath.c_str(), config.meshDistance,
                                         ctr::nowMillis());
    if (!opened) {
        // A world that will not open is not a reason to end the process: the
        // player picked it from a list and can pick another. The message goes
        // to the console under the menu that is about to come back up.
        std::printf("\x1b[31mcannot open %s\x1b[0m\n", choice.worldPath.c_str());
        renderer.shutdown();
        return 0;
    }

    // **The session opens after the world does**, because what a guest needs
    // in order to make terrain for it -- the seed and the world's own
    // generation switches -- is not known until level.dat has been read.
    if (host != nullptr) {
        mc::io::PosixFileSystem hostFs;
        mc::settings::WorldSettings hostWorldSettings;
        mc::settings::loadWorldSettings(hostFs, choice.worldPath.c_str(), &hostWorldSettings);
        mcver::WorldGenOptions options;
        options.snowCovered = world.level().snowCovered;
        options.fixOreVeinBounds = hostWorldSettings.fixOreGeneration;
        options.fixBedrockHole = hostWorldSettings.fixBedrockHole;

        mc::net::link::GeneratorId id;
        id.seed = world.level().randomSeed;
        id.options = mc::net::link::packOptions(options);
        id.version = mc::net::link::generatorVersion();

        // **Every block this world writes, so the guests can be told.** The
        // one choke point every edit goes through -- the player's, the tick's,
        // a fluid's -- so a session never has to be told about a change twice
        // and never misses one. See WorldStreamer::setBlockWatcher.
        world.setBlockWatcher(&ctr::HostPlay::blockWatcher, host);

        std::string sessionError;
        if (!host->open(choice.worldName, ctr::loginName(), id, &sessionError)) {
            // The world is open and playable; only the session failed. Say so
            // where the player is about to be looking rather than dropping
            // them back to a menu with a world half-loaded behind it.
            std::printf("\x1b[33mlocal session: %s\x1b[0m\n", sessionError.c_str());
        }

        // **How this world is played, before anybody can join it.** A guest has
        // no other way to learn it -- protocol 2 has nowhere to put it, and a
        // joining console must not simply keep its own. See
        // `net::link::WorldRules`.
        host->setWorldRules(choice.gamemode, choice.difficulty);
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

    // **The player's health**, which is Survival's and which Creative keeps too,
    // flagged invulnerable, so a mode change mid-world carries the counters
    // across rather than inventing new ones. Loaded from the saved player
    // below. Time-seeded: `aQ` is a `new Random()` in the original.
    mc::entity::PlayerVitals vitals(i64(ctr::nowMillis()) ^ 0x7ea1);

    // **Survival's break in progress** -- `nj`'s cell, progress and delay. See
    // core/item/block_breaking.hpp. Creative never touches it.
    mc::item::BlockBreaker breaker;

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

    // **The `new Random()` `cn.m` hands each block**, made once here rather
    // than once a tick: the jar builds a fresh time-seeded one per call and
    // reproduces nothing with it either, so what matters is that it is not the
    // world's -- the *offsets* come off the world's generator and the block's
    // own draws do not. See core/tick/display.hpp.
    mc::JavaRandom displayRand(i64(ctr::nowMillis()) ^ 0x5eed);

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

    // **What is grazing.** The four peaceful animals, on the heap for the same
    // stack reason the rest are, and time-seeded because everything an animal
    // does is a draw -- where it wanders, what it drops, when it makes a noise.
    // The pool holds 32 from construction, which is a1.1.2's spawn cap of 15
    // plus room for a bred pen; past that it grows. See core/entity/mob.hpp.
    auto mobs = std::make_unique<mc::entity::MobSystem>(i64(ctr::nowMillis()) ^ 0x1108);

    // **Where every hit on the player lands** -- `PlayerVitals::attack`, which
    // is `dm.a(Lkh;I)Z`: difficulty, armour, the window, the knockback, the
    // sound and, at zero, death. See core/entity/player_vitals.hpp.
    //
    // The pools that deal damage reach it through a function pointer, because
    // core/entity/ must not know what a player is; this is the other end of that
    // seam, holding what a hit needs that the pools do not have. `world`,
    // `difficulty` and `yawDegrees` are refreshed on every tick before anything
    // that can hit runs. Creative's invulnerability is the vitals' own flag, so
    // nothing here asks the gamemode.
    //
    // `taken` and `hits` are the Info page's running totals.
    struct PlayerHarm {
        mc::entity::PlayerVitals* vitals = nullptr;
        mc::entity::PlayerBody* body = nullptr;
        ctr::Overlay* overlay = nullptr;
        mc::entity::ItemEntitySystem* drops = nullptr;
        mc::tick::TickWorld* world = nullptr;
        int difficulty = 2;
        float yawDegrees = 0.0f;
        int taken = 0;
        int hits = 0;
        // Set when a hit killed; the frame loop shows the game-over screen and
        // clears it.
        bool died = false;

        bool ready() const
        {
            return vitals != nullptr && body != nullptr && overlay != nullptr && world != nullptr;
        }

        mc::entity::PlayerContext context()
        {
            mc::entity::PlayerContext ctx{*world, *body, overlay->editInventory()};
            ctx.drops = drops;
            ctx.difficulty = difficulty;
            ctx.yawDegrees = yawDegrees;
            return ctx;
        }

        // A hit that landed can have worn the armour, and one that killed has
        // emptied the inventory; both are writes the bottom screen and the
        // save have to hear about.
        void record(const mc::entity::Harm& harm, int healthBefore)
        {
            if (harm.landed) {
                taken += healthBefore - int(vitals->health);
                ++hits;
                overlay->inventoryEdited();
            }
            if (harm.died) {
                died = true;
            }
        }

        void deal(int amount, mc::entity::DamageSource source, double fromX, double fromZ)
        {
            if (!ready()) {
                return;
            }
            mc::entity::PlayerContext ctx = context();
            mc::entity::Attacker from;
            from.source = source;
            from.x = fromX;
            from.z = fromZ;
            const int before = vitals->health;
            record(vitals->attack(ctx, amount, from), before);
        }

        void fall(float distance)
        {
            if (!ready()) {
                return;
            }
            mc::entity::PlayerContext ctx = context();
            const int before = vitals->health;
            record(vitals->fall(ctx, distance), before);
        }

        void tick(bool inWater)
        {
            if (!ready()) {
                return;
            }
            mc::entity::PlayerContext ctx = context();
            const int before = vitals->health;
            record(vitals->tick(ctx, inWater), before);
        }

        static void hurt(void* ctx, int amount, mc::entity::DamageSource source, double fromX,
                         double fromZ)
        {
            static_cast<PlayerHarm*>(ctx)->deal(amount, source, fromX, fromZ);
        }
    };
    PlayerHarm harm;

    // `cw.a(Lkh;F)V`'s arrow, reaching the pool it belongs to. The mob tick has
    // no arrow system and must not grow one -- see `MobSurroundings::shootArrow`
    // -- so this is the seam's other end, and it carries the world with it
    // because `shootFrom` reads the light where the arrow appears.
    struct SkeletonBow {
        mc::entity::ArrowSystem* arrows = nullptr;
        const mc::tick::TickWorld* world = nullptr;

        static void shoot(void* ctx, double x, double y, double z, double dx, double dy,
                          double dz, float velocity, float inaccuracy, u32 shooterMob)
        {
            SkeletonBow& self = *static_cast<SkeletonBow*>(ctx);
            if (self.arrows == nullptr || self.world == nullptr) {
                return;
            }
            self.arrows->shootFrom(*self.world, x, y, z, dx, dy, dz, velocity, inaccuracy,
                                   mc::entity::ArrowShooter::Skeleton, shooterMob);
        }
    };
    SkeletonBow skeletonBow;

    // The spawner's own stream. a1.1.2 draws it from the *world's* random --
    // `az` has no `Random` of its own -- and ours is separate for the reason
    // every other pool's is: a draw made here must not shift what the tick
    // does. See core/entity/mob_spawn.hpp.
    mc::JavaRandom spawnRand(i64(ctr::nowMillis()) ^ 0x5a2d);

    // **Kept across the whole session**, because the numbers only mean anything
    // as totals: three passes a tick each find nothing at all, and a single
    // tick's worth of them says nothing about whether the spawner works. Shown
    // on the Info page -- see Overlay::MobStats.
    mc::entity::SpawnCounters monsterSpawns;

    // **What is written on the walls.** A tile entity rather than an entity --
    // see core/world/sign_store.hpp. Sign text still needs tile entity
    // persistence, separate from the entity snapshot below.
    auto signs = std::make_unique<mc::world::SignStore>();

    // **What is in the cages.** The other tile entity with a store, and the
    // only one of the four that ticks -- see core/entity/mob_spawner.hpp. Its
    // contents come out of the chunk's `TileEntities` as columns arrive, so a
    // dungeon written by the real client keeps the mob it was generated with.
    auto spawners = std::make_unique<mc::entity::MobSpawnerStore>();
    mc::entity::MobSpawnerCounters spawnerCounters;
    TileEntities tileEntities{signs.get(), spawners.get()};

    // **Where a saved spawner's mob comes from, and where a written sign's text
    // goes.** The chunk's `TileEntities` is decoded into the column now rather
    // than carried as an opaque tag, so this reads it on the way in and writes
    // it on the way out. See core/world/tile_entity.hpp.
    world.setColumnSinks(TileEntities::adopted, TileEntities::dropped, TileEntities::saving,
                         &tileEntities);

    // **What the game says to the player**, bottom left of the top screen --
    // a1.1.2's chat lines, which is where a spawn the heap refused is
    // reported. Two kilobytes, on the heap for the stack reason below.
    auto chat = std::make_unique<mc::gui::ChatLog>();

    mc::item::EntityPools pools;
    pools.paintings = paintings.get();
    pools.arrows = arrows.get();
    pools.boats = boats.get();
    pools.minecarts = minecarts.get();
    pools.mobs = mobs.get();
    pools.signs = signs.get();
    // **The other people in the room, for the crosshair to find.** Whichever
    // end of a session this console is; null in single player. A click on one
    // is never resolved here -- see `EntityTarget::remote`.
    pools.players = net != nullptr    ? &net->entities()
                    : host != nullptr ? &host->entities()
                                      : nullptr;
    const mc::item::Effects effects{particles.get(), &sound, pools};

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
    // **What is counting down.** Time-seeded, and the seed buys one thing: the
    // little hop a primed block makes comes off `Math.random()` in the
    // original, not off the world's generator, so lighting TNT must not move
    // the block-tick stream. See core/entity/primed_tnt.hpp.
    auto primedTnt = std::make_unique<mc::entity::PrimedTntSystem>(
        i64(ctr::nowMillis()) ^ 0x746e74);

    // **A server's items live in the same pool the single-player ones do**, so
    // one render pass and one tick serve both. See core/net/entities.hpp.
    if (net != nullptr) {
        net->entities().bind(droppedItems.get());
        net->entities().bindMobs(mobs.get());
    }

    world.bindEntities(mc::entity::EntityPools{paintings.get(), arrows.get(), boats.get(),
        minecarts.get(), droppedItems.get(), fallingBlocks.get(), primedTnt.get(),
        mobs.get()});

    // `random.pop`'s pitch, which is `((r - r) * 0.7 + 1) * 2` per pickup. Its
    // own generator so that drawing a pitch cannot shift the scatter on the
    // next thing thrown.
    mc::JavaRandom pickupRand(i64(ctr::nowMillis()));

    // The harm sink's borrowed pointers, set once everything a hit reaches
    // exists. `world` is the tick's and is set per tick. See `PlayerHarm` above.
    harm.vitals = &vitals;
    harm.body = &body;
    harm.drops = droppedItems.get();

    // **The fire tiles, still running.** `applyAnimatedTiles` baked a settled
    // flame into the atlas when the pack loaded, which is what the bottom
    // screen and the map read; this is the same two simulations kept going so
    // the flame in the *world* moves. On the heap for the stack reason above --
    // two 16 x 20 float fields each is 7 KB, and a 3DSX main thread has 32.
    auto flames = std::make_unique<mc::texture::FlameAnimation>();

    // **And the four fluid tiles, for the same reason.** `TextureWaterFX`,
    // `TextureWaterFlowFX`, `TextureLavaFX` and `TextureLavaFlowFX` are the
    // other four `TextureFX` a1.1.2 registers against terrain.png, and a build
    // that draws what the pack holds there draws lava in a red the client never
    // shows -- see core/texture/fluid_fx.hpp. On the heap with the flames and
    // for the same reason: four 16 x 16 float fields each is 16 KB, and a 3DSX
    // main thread has 32.
    auto fluids = std::make_unique<mc::texture::FluidAnimation>();

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
    EntityScene scene{&body,          droppedItems.get(),   mobs.get(),
                      droppedItems.get(), world.worldTick(), fallingBlocks.get(),
                      primedTnt.get()};

    // **The boxes `moveEntity` has to stop at**, which is a different question
    // from the one the scene above answers: a pressure plate wants to know what
    // is standing on it, and the sweep wants what is in the way. The first two
    // are solid to everything; the rest are what a moving boat or minecart
    // alone collides with. See core/entity/entity_boxes.hpp.
    const mc::entity::EntityBoxes entityBoxes{boats.get(),      minecarts.get(),
                                              mobs.get(),       droppedItems.get(),
                                              arrows.get(),     paintings.get(),
                                              primedTnt.get(),  fallingBlocks.get(),
                                              &body};
    static PendingContainer pendingContainer;
    pendingContainer = PendingContainer{};
    if (mc::tick::TickWorld* entityWorld = world.worldTick()) {
        entityWorld->setEntityQuery(anyEntityIn, &scene);
        // **And the entity half of `getCollidingBoundingBoxes`.** Without this
        // the block sweep is the whole of it and a player walks through a
        // parked minecart; with it they stop at one and stand on it, and the
        // cart in turn stops at the cow on the track. See
        // core/entity/entity_boxes.hpp.
        mc::entity::bindEntityBoxes(*entityWorld, &entityBoxes);
        // **And what a block leaves behind when it falls off a wall.** Without
        // this every `dropBlockAsItem` in core is a draw from the world's random
        // and nothing else, which is what it was before there was a pool to put
        // the answer in. See core/tick/drop.hpp.
        // `dropBlockAsItem` returns at once on a multiplayer world -- the
        // server drops the item and sends it -- so a session leaves both unset.
        entityWorld->setDropSink(net != nullptr ? nullptr : &spawnDroppedItem, &scene);
        entityWorld->setStackSink(net != nullptr ? nullptr : &spawnItemStack, &scene);
        entityWorld->setColumnModifiedSink(markTileEntityColumn, &world);
        // **And the screens a chest, a workbench and a furnace open.** Without
        // it `blockActivated` still takes the click, which is what headless
        // callers get: a container that opens nothing.
        entityWorld->setContainerSink(requestContainer, &pendingContainer);
        // **And the sand that is falling rather than teleporting.** Setting
        // this is what turns `BlockSand.fallInstantly` off for the parts of the
        // world a player is watching; nothing else in the process sets it, so
        // world generation and the headless tools keep the instant path. See
        // core/entity/falling_block.hpp.
        entityWorld->setFallingBlockSink(spawnFallingBlock, &scene);
        // **And the TNT that is now an entity rather than a comment.** Three
        // block paths reach it -- a break, a fire and a blast -- and without
        // this the block still disappears and still makes its noise, which is
        // what every headless caller gets. See core/entity/primed_tnt.hpp.
        entityWorld->setPrimedTntSink(spawnPrimedTnt, &scene);
        // **And the click a plate makes.** A pressure plate is flush with the
        // floor and its whole state is one bit of metadata, so without this the
        // only way to know it had armed was to look at what it was wired to --
        // which is why it was reported as feeling unresponsive rather than as
        // being silent. The lever, the button and the door were in the same
        // position. See core/tick/tick_world.hpp.
        entityWorld->setSoundSink(playTickSound, &sound);
        // **And the disc a jukebox is playing.** Unlike every other sound in
        // the game this one keeps going after the call and has to be stopped
        // again, so it is its own seam -- see `TickWorld::playRecord`. A
        // headless caller with no sink gets a jukebox that takes and gives back
        // discs in silence.
        entityWorld->setRecordSink(playRecordSound, &sound);
        // **And the text that goes with a sign.** Every removal of a sign
        // block reaches this -- the player's break, and a sign dropped because
        // the block it stood or hung on went. See core/tick/tick_world.hpp.
        entityWorld->setTileEntityRemovedSink(TileEntities::removed, &tileEntities);
        // **And the one that builds them.** `jt.e` -- BlockContainer's
        // onBlockAdded -- runs on every way a mob spawner can appear, not just
        // on a click, which is why this hangs off the block dispatch.
        entityWorld->setTileEntityAddedSink(TileEntities::added, &tileEntities);
        // **And `World.spawnParticle` itself.** Every puff in the game past a
        // broken block comes through here: a torch's smoke, a creeper's blast,
        // an animal drowning, a boat's wake. Without it the block behaviours
        // and the entity ticks run exactly as they do now and simply throw
        // nothing, which is what the host harness gets. See
        // core/entity/particle.hpp.
        mc::entity::bindParticles(*entityWorld, particles.get());
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

    // **Sneaking is a toggle**, held here rather than read off the button each
    // frame. On a keyboard a crouch is a key you can rest a finger on; on this
    // console Y is also the dismount, the descent while flying and half the
    // page cycle, and a crouch that has to be held is a finger that cannot
    // also be on the shoulders to break and place -- which is what a player
    // sneaking at a cliff edge is doing.
    //
    // Off at world entry, like flight and the sprint: `level.dat` has no key
    // for a stance and inventing one would make a world this build saved mean
    // something to nothing else.
    bool sneaking = false;
    // **A respawn waits for its ground.** The spawn point can be far from where
    // the player died, and a column that has not streamed in contributes no
    // collision -- which is right at the edge of the world and wrong for a body
    // that has just been put there. So the body holds still until the column
    // under it is resident, and this is cleared by the first tick it moves.
    bool respawnPending = false;
    // **The lift `kh.q()` owes this body**, paid on the tick its column becomes
    // resident. See `liftIntoTheWorld`. A world with a saved player in it does
    // not owe one: a1.1.2 builds the player, lifts it, and *then* reads `Pos`
    // over the top of both -- so a saved position is used exactly as saved,
    // however buried it is.
    bool spawnLiftOwed = false;
    // **And a bound on the wait**, which the original needs no equivalent of: a
    // world whose spawn column never becomes resident -- an Alpha save opened
    // with nothing to generate the missing chunk with -- would otherwise hold
    // the body still for the rest of the session. Ten seconds of ticks, then
    // the lift is taken against whatever is there and the player can move.
    int spawnWaitTicks = 0;
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
        vitals.load(world.level().player);
    }
    // **Entering a world is the same standing start a respawn is.** The body is
    // at a spawn point whose column has not arrived, so it waits for it -- and
    // a world with no player in it yet gets the lift `dm`'s constructor's
    // `q()` would have given it. `spawnY` defaults to 64 whatever the ground
    // there does, exactly as a1.1.2's does, and that lift is the whole of what
    // stops a new world starting the player inside a hill.
    respawnPending = true;
    // The server places a multiplayer player, and a1.1.2 lets it fall from there.
    spawnLiftOwed = net == nullptr && !world.level().player.present;
    // Only Survival can be hurt. See core/entity/player_vitals.hpp.
    //
    // **Nor can anyone on a Java server**: protocol 2 carries no health, so a
    // fall could hurt a player the server would never hear was hurt, and the
    // two ends would disagree with no packet able to settle it.
    //
    // A session next door is the exception, and deliberately. Nothing carries
    // health there either -- it is the same protocol -- but both ends are this
    // port, both run the same `PlayerVitals`, and the damage a player takes is
    // their own console's business in a1.1.2 anyway (`kHasServerSideDamage` is
    // false for this version). So a guest in a Survival world is mortal, which
    // is the whole of what makes it a Survival world.
    const bool localGuest = guest != nullptr;
    vitals.invulnerable = (net != nullptr && !localGuest)
                          || choice.gamemode != settings::Gamemode::Survival;

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
    // The last of the harm sink's pointers: a hit writes the inventory the
    // overlay holds. See `PlayerHarm`.
    harm.overlay = &overlay;
    overlay.setAudio(&audio);
    // **The bottom screen is the world's gamemode's**, which is why this is read
    // off the choice rather than assumed: Spectator gets the map and no hotbar,
    // Creative gets the palette page as well, and Survival gets the inventory
    // frame without one.
    overlay.setGamemode(choice.gamemode);
    // **The world's difficulty**, which is the game's own setting rather than
    // this port's -- `cn.l`. Unlike gamemode nothing in the overlay draws it,
    // so it is a plain local that the pause menu writes back into; see
    // core/settings/world_settings.hpp.
    mc::settings::Difficulty difficulty = choice.difficulty;
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
    renderer.setParticleSheet(menu.particleSheet());
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

    // **Whose B press this is.** Jump is a *held* button and the back button is
    // a *pressed* one, so the press that closes the inventory was still on B a
    // frame later and the player left the screen in mid-air. Ownership is
    // decided while the screen is still up and held until the finger comes off:
    // a B that belonged to a screen never becomes a jump, however long it is
    // held afterwards.
    bool backHeldByScreen = false;

    // **The Options row, as the number the look actually multiplies by.** Held
    // as the gain rather than the percentage so the curve is evaluated when the
    // row moves and not twice a frame; `applyPause` puts a new one here without
    // the world being closed. See core/settings/sensitivity.hpp.
    float lookGain = settings::sensitivityGain(choice.lookSensitivity);

    // **The Controls row, live.** Like the gain above it, `applyPause` puts a
    // new one here without the world being closed -- which is the point of the
    // row being on the pause menu: the only way to find out which scheme suits
    // you is to walk around under it. See core/settings/control_scheme.hpp.
    settings::ControlScheme controls = choice.controlScheme;

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
    // **A multiplayer world arrives rather than being there**, and a1.1.2 covers
    // that with `dg`, "Downloading terrain", until the server's first Player
    // Position & Look has put the player somewhere -- `gy.a(eh)` closes the
    // screen on that packet and on nothing else.
    //
    // **Waiting for the ground under the player as well was a mistake, and it
    // is the shape of hang a player would report as "it never downloads".**
    // Nothing guarantees that column arrives: 0.2.1 sends columns around the
    // player from its own tracker, and if the one under the feet is late, or
    // the player is standing where the tracker has already sent and unloaded,
    // the loop waits for something that is not coming and the session's own
    // read timeout is the only thing that ends it. The body does not need it
    // anyway -- `respawnPending` already holds it still until its chunk is
    // resident, for a bounded number of ticks, which is the same guard with an
    // end to it. So the ground is waited for briefly and then given up on.
    // START leaves, as a closed connection does.
    constexpr i64 kGroundGraceMs = 5000;
    i64 placedAtMs = 0;
    bool leaveBeforePlay = false;
    if (net != nullptr) {
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
            progress.begin(ctr::ProgressScreen::Kind::Downloading, choice.worldName.c_str());
            progress.setNote("START to disconnect");
        }
        const mc::u8* widths = menu.fontImage().empty() ? nullptr : menu.fontImage().widths;
        char note[96];

        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                leaveBeforePlay = true;
                break;
            }

            const Frustum frustum = renderer.cullFrustum(camera);
            renderer.chunks().beginFrame(++frameCounter, frustum, camera.chunkX(),
                                         camera.sectionY(), camera.chunkZ());
            // The radio, for a session next door: the bytes have to come off it
            // before the channel behind it has anything to hand over.
            if (guest != nullptr) {
                guest->pump();
            }
            net->pump(world, renderer.chunks(), overlay, *chat, widths, body, camera);
            if (net->closed()) {
                leaveBeforePlay = true;
                break;
            }
            world.update(renderer.chunks(), camera.chunkX(), camera.chunkZ(), budget);

            const render::WorldStreamer::ProgressCount made =
                world.progressWithin(camera.chunkX(), camera.chunkZ(), radius);
            if (haveScreen) {
                world.progressGrid(camera.chunkX(), camera.chunkZ(), radius, gProgressCells);
                progress.setGrid(gProgressCells, radius * 2 + 1);
                progress.setCounts(u32(made.done), u32(made.total));
                std::snprintf(note, sizeof(note), "%s  %lu KB  %d columns   START to leave",
                              net->stage(),
                              (unsigned long)(net->bytesIn() / 1024), net->columns());
                progress.setNote(note);
                progress.present(renderer, camera);
            } else {
                renderer.drawFrame(camera);
            }

            if (!net->placed()) {
                continue;
            }
            if (placedAtMs == 0) {
                placedAtMs = ctr::nowMillis();
            }
            const mc::tick::TickWorld* ground = world.worldTick();
            if ((ground != nullptr && ground->chunkResident(body.chunkX(), body.chunkZ()))
                || ctr::nowMillis() - placedAtMs > kGroundGraceMs) {
                break;
            }
        }

        if (haveScreen) {
            progress.shutdown();
        }
        overlay.invalidate();
        lastTick = svcGetSystemTick();
    }

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

    // **What the pause menu changed, applied to the world it was opened over.**
    // Shared because there are two ways out of that menu now -- the blocking
    // one single player takes and the stepped one a session takes -- and the
    // settings they hand back are the same settings either way. True means the
    // player chose Exit World and the caller is to leave the loop.
    const auto applyPause = [&](const ctr::PauseChoice& paused) -> bool {
        if (net != nullptr && !paused.chat.empty()) {
            net->chat(paused.chat);
        }
        if (host != nullptr && !paused.chat.empty()) {
            host->say(paused.chat);
            chat->add(menu.fontImage().empty() ? nullptr : menu.fontImage().widths,
                      paused.chat);
        }

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
            return true;
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
        // A server's world has no World Settings row, so what the menu
        // hands back is some other world's gamemode: a session keeps its own.
        const settings::Gamemode pausedMode = net != nullptr ? previousMode : paused.gamemode;
        overlay.setGamemode(pausedMode);
        if (net == nullptr) {
            difficulty = paused.difficulty;
        }
        if (world.worldTick() != nullptr) {
            world.worldTick()->setImprovedFencePlacement(paused.improvedFencePlacement);
        }
        vitals.invulnerable = (net != nullptr && !localGuest)
                              || pausedMode != settings::Gamemode::Survival;
        // **And the guests, who are playing this world's way.** The pause
        // menu can turn a Survival world Creative without anybody leaving
        // it, and a guest still flying in a world that has gone back to
        // Survival is the same bug as a guest who never learned it was
        // Survival in the first place.
        if (host != nullptr) {
            host->setWorldRules(pausedMode, difficulty);
        }
        breaker.reset();
        if (previousMode == settings::Gamemode::Spectator
            && pausedMode != settings::Gamemode::Spectator) {
            // Spectator's camera is a bare eye with no crouch in it, so
            // the body it hands over to is standing up.
            sneaking = false;
            placeBodyAtEye(body, camera, sneaking);
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
            // ...and `particles.png` beside it, on the same terms: a pack
            // without one gets the stand-in, not a blank particle.
            renderer.setParticleSheet(menu.particleSheet());
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
        // **Applied to the running world, like the distance above it.** The row
        // is on the pause menu precisely so it can be: a look rate is set by
        // feeling it, and feeling it means going back to the world and turning.
        lookGain = settings::sensitivityGain(paused.lookSensitivity);
        controls = paused.controlScheme;

        return false;
    };

    // **The pause menu is up and the world is still running**, which is what
    // START does in a session. See the note on the START branch below.
    bool pauseMenuUp = false;

    while (!leaveBeforePlay && aptMainLoop()) {
        hidScanInput();
        if (haveCstick) {
            // ir:rst has its own scan; hidScanInput knows nothing about the
            // C-stick.
            irrstScanInput();
        }
        // **Not const: a screen that is open takes them.** See the pause
        // branch below.
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        // **START: the pause menu, and whether the world stops for it.**
        //
        // Single player stops dead -- `runPause` takes the frame loop and the
        // world is a still picture behind it, which is `Minecraft.runTick`'s
        // own rule: a screen that `doesGuiPauseGame` stops the clock, and in
        // a1.1.2 that is checked behind `!isMultiplayerWorld()`.
        //
        // **A session cannot stop, because stopping is not a thing one console
        // gets to decide for the others.** A guest that froze would stop
        // answering its host and be dropped; a host that froze would take every
        // guest's world with it. So the menu is stepped inside this loop
        // instead: the link is pumped, chunks stream, the clock runs, mobs and
        // the other players move, and the body goes on being simulated with
        // nothing pressed -- which is the original's answer too, since a screen
        // being open is what stops the keys reaching the player, not what stops
        // the world.
        if (!pauseMenuUp && (down & KEY_START) != 0) {
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

            const bool online = net != nullptr || host != nullptr;
            menu.setMultiplayer(online);
            if (!online) {
                // The world as it stood when START was pressed, redrawn every
                // frame the menu is up. Nothing is ticked, streamed or meshed
                // while it is -- the camera does not move and the sun does not
                // either -- so every one of those frames is the same picture
                // with the menu over it.
                PausedWorld backdropWorld{&renderer, &camera};
                ctr::PauseBackdrop backdrop;
                backdrop.context = &backdropWorld;
                backdrop.drawFrame = drawPausedWorld;

                const ctr::PauseChoice paused =
                    menu.runPause(choice.worldName.c_str(), choice.worldPath.c_str(),
                                  settings.renderDistance, backdrop);
                menu.setMultiplayer(false);
                menu.shutdown();
                if (applyPause(paused)) {
                    break;
                }
                // **The press that closed the menu was never seen by this
                // loop** -- `runPause` has its own -- so the claim above cannot
                // be made from `screenHadB` and is made here instead. Without
                // it, leaving the pause menu with B jumps on the way out.
                backHeldByScreen = true;
                continue;
            }

            menu.beginPause(choice.worldName.c_str(), choice.worldPath.c_str(),
                            settings.renderDistance);
            pauseMenuUp = true;
            // **This frame's press opened the menu and is not also an answer to
            // it.** START is Resume as well as Pause, so a step that still had
            // it in hand would close the menu in the frame that opened it. The
            // blocking path gets this for free, because `runPause` scans the
            // pad again before its first step.
            down = 0;
            held = 0;
        }

        if (pauseMenuUp) {
            if (menu.stepPause(down)) {
                pauseMenuUp = false;
                const ctr::PauseChoice paused = menu.endPause();
                menu.setMultiplayer(false);
                menu.shutdown();
                if (applyPause(paused)) {
                    break;
                }
                // The same as the blocking path below: `held` is zeroed while
                // the menu is up, so the claim has to be made where the press
                // was answered.
                backHeldByScreen = true;
                continue;
            }
            // **The buttons belong to the menu, so the world hears nothing.**
            // a1.1.2 releases every key when a screen opens and reads the
            // movement state from keys that are therefore all up; this is that,
            // and it is why the body below can go on being ticked without any
            // of it having to know a menu is open. The two look paths read the
            // pad and the C-stick for themselves rather than from `held`, so
            // they are told separately -- see the calls below.
            down = 0;
            held = 0;
        }

        // The bottom screen owns SELECT, so nothing below fires while it is
        // held. Applying is the caller's job because a render distance means
        // rebuilding the field, the pool and the streamer's grid -- in that
        // order, since the streamer republishes into whatever field it finds.
        const double cameraWasX = camera.x;
        const double cameraWasY = camera.y;
        const double cameraWasZ = camera.z;

        // **Read before the screen is stepped, because stepping it is what
        // takes it away.** A B press that lands on a focused bottom screen
        // closes it, and by the time the body is ticked further down there is
        // no focus left to say the press was not a jump.
        //
        // The pause menu is not here: it answers its own press, inside
        // `runPause` or `stepPause`, and `held` is already zero by this line
        // while it is up -- so its two claims are made where the press is
        // answered instead. See `backHeldByScreen`.
        const bool focusHadB = overlay.uiFocused();

        // **Whether the bottom screen may treat the d-pad as its own**, which
        // it may not once the Controls row has pointed walking or looking at
        // it. Told rather than asked, because the scheme can change under a
        // running world -- the pause menu has the row.
        overlay.setDpadIsGameplay(settings::moveStick(controls) == settings::Stick::Dpad
                                  || settings::lookStick(controls) == settings::Stick::Dpad);

        const bool settingsChanged = overlay.handleInput(down, held, &settings, &camera);

        // **`held`, less the d-pad when a screen has taken it.** The focused
        // pages walk a cursor with it and the debug pages behind SELECT drive
        // their rows with it, and a frame that is using the d-pad for one of
        // those is not also using it to walk. Only the stick reads take this;
        // the face buttons stay on `held`, because nothing here is claiming
        // them. The pause menu needs no mention: `held` is already zero by then
        // and this is derived from it.
        constexpr u32 kDpad = KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT;
        const u32 stickHeld = overlay.dpadTakenByScreen() ? (held & ~kDpad) : held;

        // Cleared by letting go, claimed by any frame a screen was up for. The
        // release is checked first, so a claim is never carried past the press
        // it was made for.
        if ((held & KEY_B) == 0) {
            backHeldByScreen = false;
        } else if (focusHadB) {
            backHeldByScreen = true;
        }

        // **A teleport moves the camera, and outside Spectator the camera is
        // not where the position lives.** Every body mode overwrites it from the
        // body at the end of the frame, so a teleport the body was not told
        // about was undone before it was ever drawn. The debug page's teleport
        // row is the only thing handleInput moves the camera for.
        //
        // A vehicle would pull the body straight back to its seat, so the
        // teleport leaves it behind -- the same as pressing Y first.
        if (overlay.gamemode() != settings::Gamemode::Spectator
            && (camera.x != cameraWasX || camera.y != cameraWasY || camera.z != cameraWasZ)) {
            boats->dismount();
            minecarts->dismount();
            placeBodyAtEye(body, camera, sneaking);
            sprintGesture.cancel();
        }

        // **Death, and the screen that follows it.** `Minecraft.a(Lbh;)V` puts
        // up `au` whenever no screen is open and the player's health is not
        // above zero -- so a hit that killed, and equally a world saved with a
        // dead player in it, both land here. The world keeps running under it.
        harm.died = false;
        if (overlay.gamemode() != settings::Gamemode::Spectator && !vitals.alive()
            && !overlay.dead()) {
            overlay.setDead(true, int(world.level().player.score));
        }
        // **What the world is allowed to hear this frame.** The death screen
        // answers A, and answering it clears `dead()` -- so every guard below
        // that asks `!overlay.dead()` would come back true again with this
        // frame's A still in `down`, and the press that asked to respawn would
        // also break a block or throw the stack in hand. The screen was up when
        // the buttons were read; that is the state the rest of the frame runs
        // against.
        const bool wasDead = overlay.dead();
        bool leaveWorld = false;
        switch (overlay.takeDeathChoice()) {
        case ctr::Overlay::DeathChoice::Respawn: {
            // `Minecraft.o()`: a new player at the spawn point, the old one
            // gone with everything it carried, and `nj.a(dm)` turning the new
            // one to face -180.
            boats->dismount();
            minecarts->dismount();
            mobs->dismount();
            vitals.respawn();
            breaker.reset();
            respawnBody(body, world.level());
            respawnPending = true;
            spawnLiftOwed = true;
            spawnWaitTicks = 0;
            sneaking = false;
            sprintGesture.cancel();
            camera.x = body.x;
            camera.y = body.eyeY();
            camera.z = body.z;
            camera.yaw = -kPi;
            camera.pitch = 0.0f;
            overlay.setDead(false, 0);
            break;
        }
        case ctr::Overlay::DeathChoice::TitleMenu:
            // `au`'s second button, which is the pause menu's Exit World: the
            // world is saved and closed on the way out, as it is there.
            leaveWorld = true;
            break;
        case ctr::Overlay::DeathChoice::None:
            break;
        }
        if (leaveWorld) {
            break;
        }

        if (settingsChanged) {
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
            flyCamera(camera, dt, (held & KEY_X) != 0 && !(held & KEY_SELECT), controls,
                      stickHeld);
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

        // **Y toggles the crouch**, everywhere Y is not already spoken for.
        //
        // It is a toggle rather than a held button because of what a player
        // sneaks *for*: edging out over a drop to place a block under
        // themselves, which on this console means a thumb on the circle pad and
        // a finger on a shoulder. A held Y takes the thumb that is steering.
        // Nothing about the physics changes -- `PlayerInput::sneak` is still
        // read once a tick and still means the same thing -- only where the
        // state lives.
        //
        // **Three places already own Y and keep it**: Spectator descends with
        // it, Creative's flight descends with it, and a rider dismounts with
        // it. SELECT + Y is the bottom screen's page cycle, so a held SELECT
        // takes the press as well.
        const bool yIsSpokenFor = flying
                                  || overlay.gamemode() == settings::Gamemode::Spectator
                                  || boats->riddenIndex() >= 0
                                  || minecarts->riddenIndex() >= 0
                                  || mobs->riddenIndex() >= 0;
        if ((down & KEY_Y) != 0 && !yIsSpokenFor && !(held & KEY_SELECT)
            && !overlay.containerTakesY()) {
            sneaking = !sneaking;
        }
        // ...and the same three stand the player back up, rather than leaving a
        // toggle on that its own button can no longer reach. A crouch is
        // something done on your own feet: there is none in a seat, none in the
        // air, and none in a camera with no body.
        sneaking = sneaking && !yIsSpokenFor;
        // **The focused circle pad, before the look.** It scrolls the map when
        // the map is the focused page and does nothing otherwise; the body's
        // heading is zeroed to match, further down.
        // **The three that read the hardware for themselves**, and so cannot
        // be told a menu is open by having `held` taken away from them: the
        // focused pad, the touch drag and the C-stick all go straight to
        // libctru. A drag on the bottom screen while the pause menu is up is a
        // press meant for the menu, not a look.
        if (!pauseMenuUp) {
            overlay.tickFocus(dt);

            lookWithTouch(camera, &dragging, &lastTouch, overlay.touchLookTop(), lookGain);

            // **The C-stick turns the view under every scheme**, not only the
            // one that names it: a New 3DS set to an Old scheme still has its
            // right stick, and so does an old one with a Circle Pad Pro on it.
            // Taking it away would be taking a device off the console rather
            // than reassigning it, and nothing else wants it.
            const settings::Stick lookStick = settings::lookStick(controls);
            if (haveCstick) {
                lookWithStick(camera, dt, lookGain, settings::Stick::CStick, stickHeld);
            }

            // **And then the scheme's own look device, if it is not that one.**
            //
            // A focused bottom screen takes it, which the C-stick above never
            // needs: the d-pad is walking the cursor over the palette and the
            // circle pad is panning the map, and a page that has taken a device
            // cannot also be turning the camera with it. This is the same rule
            // `uiFocused` applies to the body's heading further down -- see
            // Overlay::uiCursorActive -- and it is spelled out here because the
            // circle pad is read off the hardware and so cannot be silenced by
            // `held` going to zero the way the d-pad is.
            if (lookStick != settings::Stick::CStick && !overlay.uiFocused()) {
                lookWithStick(camera, dt, lookGain, lookStick, stickHeld);
            }
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
        // **The same clock, and a different function of it.** The line above
        // takes the day as the integer the block light steps down by; this one
        // takes it as the sky's three colours, the star brightness and the
        // angle the sun is at. Alpha dims the ground in eleven jumps and fades
        // the sky smoothly, so the two cannot share an answer.
        renderer.setWorldTime(dayTicks, tickTimer.partialTicks());

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
        // Health beside it, and only from a mode with a body: Spectator must
        // leave the counters the real client saved exactly as they were.
        if (hasBody) {
            world.setPlayerVitals(vitals.health, vitals.hurtTime, vitals.deathTime,
                                  vitals.attackTime, vitals.air, vitals.fire);
        }

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
                // **The crack follows the break, not the crosshair**: it is
                // drawn on the cell the controller is damaging, at the stage
                // interpolated between its last two ticks, and only while there
                // is progress to show. See core/item/block_breaking.hpp.
                const int crack = overlay.gamemode() == settings::Gamemode::Survival
                                      ? breaker.crackStage(tickTimer.partialTicks())
                                      : -1;
                if (crack >= 0) {
                    renderer.setBreakOverlay(
                        aimWorld->blockAt(breaker.x(), breaker.y(), breaker.z()),
                        aimWorld->dataAt(breaker.x(), breaker.y(), breaker.z()), breaker.x(),
                        breaker.y(), breaker.z(), crack);
                } else {
                    renderer.clearBreakOverlay();
                }
            }
        } else {
            renderer.clearSelection();
            renderer.clearBreakOverlay();
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
            // The same cube again, with a swell and a flash on it -- see
            // core/render/primed_tnt_mesh.hpp. It rides the item pass's eye
            // and partial too.
            renderer.setPrimedTnt(primedTnt.get());
            // Paintings do not move and do not interpolate, so this is the pool
            // and nothing else -- it uses the eye the item pass just set.
            renderer.setPaintings(paintings.get());
            // Arrows ride the item pass's eye and partial too; unlike a
            // painting they do interpolate, because they move.
            renderer.setArrows(arrows.get());
            renderer.setBoats(boats.get());
            // The animals, which interpolate like a boat and pose like nothing
            // else in the build -- see core/render/mob_mesh.hpp.
            renderer.setMobs(mobs.get());
            // Whichever end of the session this console is: a guest reads the
            // stream it is sent, a host reads what it is sending everybody
            // else. Both are `RemoteEntities`.
            renderer.setRemotePlayers(net != nullptr    ? &net->entities()
                                      : host != nullptr ? &host->entities()
                                                        : nullptr);
            // The same pool again, for the markers on the bottom screen. The
            // host's own entity id is fixed -- it is the first of the reserved
            // ones -- and a guest is told theirs by the Login packet.
            overlay.setSession(net != nullptr    ? &net->entities()
                               : host != nullptr ? &host->entities()
                                                 : nullptr,
                               net != nullptr    ? net->selfEntityId()
                               : host != nullptr ? mc::net::kFirstPlayerEntityId
                                                 : 0);
            renderer.setSpawners(spawners.get());
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

        // **What the server sent, before the streamer looks at the grid**, which
        // is where `gs.g()` polls its handler: columns join, blocks change and
        // the player is put where the server says before anything reads them.
        if (net != nullptr) {
            // For a session next door, the radio first: `NetPlay` reads a
            // channel and the channel is fed from the link.
            if (guest != nullptr) {
                guest->pump();

                // **The host changed how the world is played.** The same
                // hand-over the pause menu makes below, for the same reason:
                // Spectator flies a bare camera and every other mode reads the
                // camera off the body, so leaving Spectator has to put the body
                // under the eye or the next tick teleports the player back to
                // wherever it was last ticked.
                if (guest->takeRulesChange()) {
                    const settings::Gamemode previousMode = overlay.gamemode();
                    const settings::Gamemode mode = guest->gamemode();
                    overlay.setGamemode(mode);
                    difficulty = guest->difficulty();
                    vitals.invulnerable = mode != settings::Gamemode::Survival;
                    breaker.reset();
                    if (previousMode == settings::Gamemode::Spectator
                        && mode != settings::Gamemode::Spectator) {
                        sneaking = false;
                        placeBodyAtEye(body, camera, sneaking);
                        sprintGesture.cancel();
                    }
                }
            }
            net->pump(world, renderer.chunks(), overlay, *chat,
                      menu.fontImage().empty() ? nullptr : menu.fontImage().widths, body,
                      camera);
            if (net->closed()) {
                break;
            }
        }

        // The other half of the same moment, for a console that is hosting:
        // what the guests said, answered before anything reads the world, and
        // then the world itself on its way out to them.
        if (host != nullptr) {
            host->pump(world, renderer.chunks(), effects, *droppedItems, *chat,
                       menu.fontImage().empty() ? nullptr : menu.fontImage().widths);
            host->reportPose(body, camera);
        }

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
            if (!overlay.uiFocused() && !wasDead) {
                const ChatSink chatSink{
                    chat.get(), menu.fontImage().empty() ? nullptr : menu.fontImage().widths};
                editBlocks(world, renderer.chunks(), camera, body, overlay, down, held,
                           editTick, &lastEditTick, effects, &hand, chatSink, &breaker,
                           &vitals, net, host, &pendingContainer);
                if (pendingContainer.open) {
                    pendingContainer.open = false;
                    const mc::u32 cart = pendingContainer.cart;
                    pendingContainer.cart = 0;
                    // **A chest's and a furnace's contents are the client's in
                    // a1.1.2 multiplayer**, sent back to the server as Complex
                    // Entity NBT -- which is not built yet, so opening one here
                    // would lose whatever was put in. A workbench holds nothing.
                    // A chest cart is refused for the same reason and is not
                    // even spawned by a server this build can talk to.
                    const bool refused =
                        net != nullptr
                        && (cart != 0
                            || pendingContainer.kind
                                   != mc::tick::TickWorld::ContainerKind::Workbench);
                    if (refused) {
                        chat->post(chatSink.widths,
                                   "Chests and furnaces do not work in multiplayer yet.");
                    }
                    if (mc::tick::TickWorld* screenWorld =
                            refused ? nullptr : world.worldTick()) {
                        if (cart != 0) {
                            overlay.openMinecartChest(*screenWorld, *minecarts, cart);
                        } else {
                            overlay.openContainer(*screenWorld, pendingContainer.kind,
                                                  pendingContainer.x, pendingContainer.y,
                                                  pendingContainer.z);
                        }
                    }
                }

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

            // **X throws the whole of a stack picked up on the bottom screen**,
            // which is the original's click outside an open container window
            // rather than its drop key -- see Overlay::throwRequest.
            //
            // **Outside the focus guard above**, unlike A's single drop: a
            // stack is picked up *with* the screen focused, so a throw that
            // only ran unfocused could never be asked for in the first place.
            // The press itself is the Overlay's and is already spent there.
            if (const mc::item::ItemStack* throwing = overlay.throwRequest()) {
                mc::tick::TickWorld* dropWorld = world.worldTick();
                const mc::u32 refusedBefore = droppedItems->refused();
                // The entity first and the stack second, for the reason the A
                // path gives: a refused spawn must not have spent anything.
                if (dropWorld != nullptr
                    && droppedItems->dropFromPlayer(
                           *dropWorld, body.x, body.posY, body.z, camera.yaw * 180.0f / kPi,
                           camera.pitch * 180.0f / kPi, mc::item::ItemId(throwing->id),
                           int(throwing->count), throwing->damage)) {
                    overlay.finishThrow();
                } else {
                    if (droppedItems->refused() != refusedBefore) {
                        const ChatSink throwSink{chat.get(), menu.fontImage().empty()
                                                                 ? nullptr
                                                                 : menu.fontImage().widths};
                        throwSink.limitReached(mc::item::LimitedEntity::DroppedItem);
                    }
                    overlay.cancelThrow();
                }
            }

            // **The open container screen, and what shutting one left.** A
            // furnace cooks under an open screen and a chest can be blown up
            // under one, so it is re-read every frame; the cursor and a
            // crafting grid go on the ground the way a throw does, and one the
            // pool refuses goes back into the inventory.
            overlay.tickContainer(world.worldTick());
            while (const mc::item::ItemStack* closing = overlay.closedStack()) {
                mc::tick::TickWorld* dropWorld = world.worldTick();
                const bool spawned =
                    dropWorld != nullptr
                    && droppedItems->dropFromPlayer(
                           *dropWorld, body.x, body.posY, body.z, camera.yaw * 180.0f / kPi,
                           camera.pitch * 180.0f / kPi, mc::item::ItemId(closing->id),
                           int(closing->count), closing->damage);
                overlay.finishClosedStack(spawned);
            }

            // `la.a(dx)`: everything the player threw this frame is the
            // server's to make, and the client keeps none of it.
            if (net != nullptr) {
                net->forwardDrops(*droppedItems);
            }

            tick::TickWorld* tickWorld = world.worldTick();
            if (tickWorld != nullptr) {
                // **The body's ticks write blocks**, and they do it outside
                // `stepTicks`: `moveEntity`'s tail tramples farmland and arms
                // pressure plates. Held for the whole of the body loop rather
                // than per write, and it is what keeps a trampled furrow from
                // leaving its crop standing on screen. See
                // WorldStreamer::RenderBracket.
                mc::render::WorldStreamer::RenderBracket draws(world, renderer.chunks());
                mc::entity::PlayerInput bodyInput =
                    readBodyInput(camera, stickHeld, sneaking, controls);
                // **A dead player does not move**: the game-over screen owns
                // the buttons, and the body falls where it lies.
                //
                // **And neither does a player with the pause menu open**, for
                // the same reason and by the same rule -- a screen is open, so
                // nothing reaches the player. `held` is already zero by here;
                // the stick is not, because `readBodyInput` reads the pad off
                // the hardware itself.
                if (!vitals.alive() || pauseMenuUp) {
                    bodyInput.strafe = 0.0f;
                    bodyInput.forward = 0.0f;
                    bodyInput.jump = false;
                    bodyInput.sneak = false;
                    bodyInput.sprint = false;
                }
                // **A focused bottom screen has the stick and B**, which is
                // a1.1.2's own rule rather than an addition: an open
                // `GuiScreen` is what stops the original reading the movement
                // keys at all, and every focused page here is a screen in that
                // sense. The stick is panning a map or walking a cursor -- see
                // `Overlay::uiCursorActive` -- and B is the back button;
                // walking and jumping off the same press would be one control
                // doing two things, which is what the focus exists to prevent.
                //
                // Y is left alone: nothing focused reads it.
                if (overlay.uiFocused()) {
                    bodyInput.strafe = 0.0f;
                    bodyInput.forward = 0.0f;
                    bodyInput.jump = false;
                }
                // **And the press that closed a screen is still not a jump.**
                // The screen went on the press; the finger is still on B for a
                // few frames after it, by which time `uiFocused` is false and
                // the guard above has nothing left to say. See
                // `backHeldByScreen`.
                if (backHeldByScreen) {
                    bodyInput.jump = false;
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
                // `uiFocused` now covers the panned map as well -- it is one
                // of the screens the stick belongs to -- so the second flag it
                // used to need is gone rather than merely redundant.
                bodyInput.sprint = readSprint(
                    sprintGesture, bodyInput, sinceStart,
                    overlay.gamemode() != settings::Gamemode::Creative || flying
                        || overlay.uiFocused());
                for (int i = 0; i < ticksDue; ++i) {
                    // **The hand, first and unconditionally.** `Minecraft.i()`
                    // runs `ItemRenderer.updateEquippedItem` once a tick with a
                    // world open, and `EntityPlayer.onUpdate` runs the arm
                    // swing -- neither cares whether the player is walking,
                    // flying or in a boat, and the riding branch below returns
                    // early. See core/render/held_item.hpp.
                    hand.tick(overlay.inventory().selectedItem());

                    // **The owed spawn lift, and it has to run before `y()`.**
                    //
                    // This used to sit down in the mover, below -- which put a
                    // whole `onEntityUpdate` in front of it, run against a body
                    // still standing where `respawnBody` left it. That is
                    // `spawnY + 1`, and `spawnY` is 64 whatever the ground
                    // there is doing, so as often as not it is inside a
                    // hillside. `ge.y()` asks whether the eye is in an opaque
                    // block and takes a point off for it, so a fresh Survival
                    // world greeted the player with half a heart gone and
                    // nothing on screen to say why.
                    //
                    // a1.1.2 cannot have that bug: `kh.q()` is called from the
                    // constructor, so the body is out of the ground before
                    // anything ticks it at all. The order is the same here now
                    // -- lift, then `y()`, then move -- and the wait for the
                    // column is the whole of what is left of the difference.
                    // See `liftIntoTheWorld`.
                    //
                    // **Flight is left out**, as the mover leaves it out: a
                    // Creative player under their own power is not waiting for
                    // the ground to arrive under them.
                    bool spawnHolding = false;
                    if (respawnPending && !flying) {
                        if (++spawnWaitTicks < kSpawnWaitTicks
                            && !tickWorld->chunkResident(body.chunkX(), body.chunkZ())) {
                            // Held where it was put until its column arrives.
                            // See `respawnPending`.
                            spawnHolding = true;
                        } else {
                            if (spawnLiftOwed) {
                                liftIntoTheWorld(body, *tickWorld, &camera);
                            }
                            respawnPending = false;
                            spawnLiftOwed = false;
                            spawnWaitTicks = 0;
                        }
                    }

                    // **`Entity.onEntityUpdate`'s splash, which the player has
                    // to be given explicitly.** `PlayerBody::tick` is
                    // `onLivingUpdate` and `moveEntityWithHeading`; `y()` runs
                    // before both, and the animals reach it inside
                    // `MobSystem::updateCounters`. So this is the player's
                    // `y()`, and it belongs here rather than in the body for
                    // the reason `PlayerBody::updateWaterEntry` gives.
                    //
                    // **Before the riding branch**, which returns early: a
                    // player rowing a boat into a lake still splashes, and in
                    // the jar so does the boat, separately and at its own
                    // volume.
                    //
                    // The volume comes off the motion the *previous* tick
                    // left, which is the whole character of the sound: a dive
                    // from a cliff is loud and wading in is not. The position
                    // is the feet -- `posY - yOffset`, and the player's
                    // `yOffset` is 1.62 -- which is what `playSoundAtEntity`
                    // passes and what the footstep below already uses.
                    //
                    // **Skipped entirely while the body is held for its
                    // column.** It is standing at an unlifted spawn point in a
                    // chunk that is not there yet; `y()` against that is the
                    // suffocation the lift above exists to prevent, and a
                    // splash off water that has not streamed in would be a
                    // sound with nothing under it.
                    if (!spawnHolding) {
                        const mc::entity::WaterEntryResult wet =
                            body.updateWaterEntry(*tickWorld);
                        if (wet.splash) {
                            sound.playSoundAt(mc::entity::kSplashSound, body.x,
                                              body.posY - double(mc::entity::kEyeHeight), body.z,
                                              wet.volume,
                                              mc::entity::splashPitch(pickupRand));
                            // ...and the spray. The player is 0.6 across, so
                            // thirteen bubbles and thirteen splashes off the
                            // surface of the cell the feet are in. See
                            // core/entity/water_entry.hpp.
                            mc::entity::waterEntryParticles(
                                *tickWorld, pickupRand, body.x, body.box.minY, body.z,
                                body.width, body.motionX, body.motionY, body.motionZ);
                        }

                        // **The rest of `onEntityUpdate`, and `onLivingUpdate`'s
                        // regeneration**: fire, lava, the void, suffocation,
                        // drowning, the counters and death -- after the water
                        // branch, which puts a fire out first, and before the
                        // body moves. See PlayerVitals::tick.
                        harm.world = tickWorld;
                        harm.difficulty = int(difficulty);
                        harm.yawDegrees = camera.yaw * 180.0f / kPi;
                        harm.tick(wet.inWater);
                    }

                    // `Minecraft.a(IZ)V` -- **Survival's held button, once a
                    // tick**: `nj.c(IIII)` on the block under the crosshair
                    // while R is down on one, `nj.a()` -- reset -- on every tick
                    // it is not. Before the body moves, as `runTick` runs it
                    // before the world updates its entities.
                    if (overlay.gamemode() == settings::Gamemode::Survival) {
                        breaker.update();
                        bool digging = false;
                        if (vitals.alive() && !overlay.uiFocused() && !wasDead
                            && (held & KEY_R) != 0) {
                            float lx = 0.0f;
                            float ly = 0.0f;
                            float lz = 0.0f;
                            camera.look(&lx, &ly, &lz);
                            const mc::entity::RayHit aim = mc::entity::rayTrace(
                                *tickWorld, camera.x, camera.y, camera.z, double(lx),
                                double(ly), double(lz));
                            const mc::item::EntityTarget onEntity = mc::item::pickEntity(
                                effects.entities, camera.x, camera.y, camera.z, double(lx),
                                double(ly), double(lz), aim);
                            if (aim.hit && !onEntity.found()) {
                                mc::item::BreakContext dig{*tickWorld, overlay.editInventory(),
                                                           effects};
                                dig.eyeInWater = mc::entity::playerEyeInWater(*tickWorld, body);
                                dig.onGround = body.onGround;
                                const auto dug = tickWorld->blockAt(aim.x, aim.y, aim.z);
                                if (breaker.damage(dig, aim.x, aim.y, aim.z, int(aim.face))) {
                                    overlay.inventoryEdited();
                                }
                                if (net != nullptr) {
                                    net->digProgress(
                                        aim.x, aim.y, aim.z, int(aim.face),
                                        tickWorld->blockAt(aim.x, aim.y, aim.z) != dug,
                                        int(overlay.inventory().selectedItem()));
                                }
                                digging = true;
                            }
                        }
                        if (!digging) {
                            breaker.reset();
                            if (net != nullptr) {
                                net->digStop();
                            }
                        }
                    }

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
                    //
                    // Still the held button rather than the crouch toggle: a
                    // rider has no crouch -- `yIsSpokenFor` above suppresses
                    // the toggle and stands the player up for as long as they
                    // are in a seat -- so Y here means one thing only.
                    const bool inBoat = boats->riddenIndex() >= 0;
                    const bool inCart = minecarts->riddenIndex() >= 0;
                    // **A saddled pig is the third thing that can be ridden**,
                    // and the one that steers itself: `mv` reads nothing from
                    // its rider, so the movement keys still become the
                    // *player's* motion and go nowhere, and the pig walks where
                    // its own AI says. See core/entity/mob.hpp.
                    const bool onPig = mobs->riddenIndex() >= 0;
                    if (inBoat || inCart || onPig) {
                        if ((held & KEY_Y) != 0) {
                            // **Getting off puts the player on the roof**, not
                            // in the seat: that is `mountEntity`'s own tail
                            // (see core/entity/rider.hpp), and with a boat and
                            // a minecart now solid it is a place to stand
                            // rather than a place to sink through.
                            const mc::entity::RiderSeat leftBoat = boats->dismount();
                            const mc::entity::RiderSeat leftCart = minecarts->dismount();
                            const mc::entity::RiderSeat leftPig = mobs->dismount();
                            const mc::entity::RiderSeat off =
                                leftBoat.valid ? leftBoat
                                               : (leftCart.valid ? leftCart : leftPig);
                            if (off.valid) {
                                body.setFeet(off.x, off.y, off.z);
                            }
                        } else {
                            mc::entity::VehicleRider seat;
                            seat.present = true;
                            seat.motionX = body.motionX;
                            seat.motionZ = body.motionZ;
                            mc::entity::RiderSeat where;
                            if (inBoat) {
                                boats->tick(*tickWorld, seat);
                                where = boats->seat();
                            } else if (inCart) {
                                minecarts->tick(*tickWorld, seat);
                                where = minecarts->seat();
                            } else {
                                // The pig is ticked with every other animal,
                                // below; all this needs is where it ended up.
                                where = mobs->seat();
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
                    } else if (spawnHolding) {
                        // Held where it was put until its column arrives. The
                        // wait, the lift and the counter are all decided above,
                        // before anything is allowed to hurt the body.
                    } else {
                        body.tick(*tickWorld, bodyInput);
                    }

                    // The footstep's other half, and out here for the same
                    // reason the loop below is: `moveEntity` hands the cell
                    // underfoot to `onEntityWalking` before it runs the
                    // collision scan, and that is what trample a field back to
                    // dirt as it is crossed. See tick::entityWalkedOnBlock.
                    if (body.steppedOn) {
                        mc::tick::entityWalkedOnBlock(*tickWorld, body.stepBlockX,
                                                      body.stepBlockY, body.stepBlockZ);
                    }

                    // `ge.c(F)V` -- **fall damage**, which `moveEntity` deals
                    // the moment it lands and before its tail runs. The body
                    // only reports the distance; see PlayerBody::landedFall.
                    if (body.landedFall > 0.0f) {
                        harm.fall(body.landedFall);
                        body.landedFall = 0.0f;
                    }

                    // `moveEntity`'s tail, which `PlayerBody` cannot run
                    // itself because it moves against a const world. This is
                    // what arms a pressure plate the moment it is stood on;
                    // see tick::entityCollidedWithBlocks.
                    mc::tick::entityCollidedWithBlocks(*tickWorld, body.box);

                    // **The same tail's two costs**, in its order: a cactus
                    // pricks once per cell the box is in, and then fire and
                    // lava catch or a burning player goes under and hisses.
                    // Both are `attackEntityFrom(null, 1)` -- see
                    // core/entity/block_contact.hpp and fire_entry.hpp.
                    {
                        const int pricks = mc::entity::blockContactHits(*tickWorld, body.box);
                        for (int n = 0; n < pricks; ++n) {
                            harm.deal(mc::entity::kContactDamage, mc::entity::DamageSource::World,
                                      0.0, 0.0);
                        }
                        const mc::entity::FireEntryResult burn = mc::entity::updateFireEntry(
                            &vitals.fire, mc::entity::boundingBoxBurning(*tickWorld, body.box),
                            mc::entity::fireWetProbe(*tickWorld, body.box),
                            mc::entity::kPlayerFireResistance);
                        if (burn.damage) {
                            harm.deal(1, mc::entity::DamageSource::World, 0.0, 0.0);
                        }
                        if (burn.fizz) {
                            sound.playSoundAt(mc::entity::kFizzSound, body.x,
                                              body.posY - double(body.yOffset), body.z, 0.7f,
                                              mc::entity::fizzPitch(pickupRand));
                        }
                    }

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
                    //
                    // **A corpse picks nothing up.** `dm.j()` guards the whole
                    // sweep with `if (health > 0)`, and it has to: death drops
                    // the inventory at the player's feet with the pool's own
                    // pickup delay on it, and the death screen stands there for
                    // longer than that delay. Without the guard the body
                    // vacuums its own grave back up and the player respawns
                    // holding everything, which is what "dying did not drop my
                    // items" looks like from the outside.
                    // **Nobody picks anything up on a multiplayer client.** The
                    // server decides who collected what and says so with
                    // Collect and Add To Inventory; a client that also took it
                    // would be holding an item the server never gave it.
                    const AABB reach = body.box.expand(1.0, 0.0, 1.0);
                    const int picked = vitals.alive() && net == nullptr
                                           ? overlay.collectItems(*droppedItems, reach)
                                           : 0;
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

                    // `la.J()`, at the end of the player's tick, and the other
                    // entities' own.
                    if (net != nullptr) {
                        net->tick(body, camera, overlay.inventory(), tickWorld);
                    }
                    // **A host has the same bodies to walk and no session to
                    // do it.** Its guests are entities exactly as they are on
                    // a guest's screen; the only difference is where the
                    // packets describing them came from. See
                    // `WorldServer::setLocalSink`.
                    if (host != nullptr) {
                        host->tickEntities(tickWorld);
                    }

                    // **A blow from another console, on the tick it arrived.**
                    // No health crosses the link -- protocol 2 has no packet
                    // for it -- so the wire carries only who swung and with
                    // what, and the cost is worked out here by the same
                    // `damageVsEntity` lookup the attacker's own console would
                    // have used. `Other` rather than `Monster`: `dm.a(Lkh;I)Z`
                    // scales by difficulty only for a `dq` or a `kg`, and a
                    // player is neither. See core/net/entities.hpp.
                    mc::net::IncomingHit hit;
                    const bool struck = net != nullptr    ? net->takeHit(&hit)
                                        : host != nullptr ? host->takeHit(&hit)
                                                          : false;
                    if (struck) {
                        const int amount =
                            int(mc::item::def(mc::item::ItemId(hit.item)).damageVsEntity);
                        if (amount > 0) {
                            harm.world = tickWorld;
                            harm.difficulty = int(difficulty);
                            harm.yawDegrees = camera.yaw * 180.0f / kPi;
                            harm.deal(amount, mc::entity::DamageSource::Other, hit.fromX,
                                      hit.fromZ);
                        }
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
            // **Not `renderEyeY`**: a crouching player's camera sits 0.08
            // lower, and this is the only place that difference is applied.
            // The aim ray is cast from `camera` too, so the crosshair drops
            // with the view rather than staying at a standing eye.
            camera.y = body.cameraEyeY(partial);
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

            // **The hearts**, Survival's alone -- `lu` draws the rows only while
            // `PlayerController.shouldDrawHUD` is true, and a Creative or
            // Spectator player has nothing for them to count. Dead or alive:
            // the game-over screen is drawn over the overlay, not instead of it.
            if (overlay.gamemode() == settings::Gamemode::Survival) {
                mc::render::HudInput hud;
                hud.health = vitals.health;
                hud.prevHealth = vitals.prevHealth;
                hud.hurtResistant = vitals.hurtResistant;
                hud.armour = mc::entity::armourValue(overlay.inventory());
                hud.air = vitals.air;
                const mc::tick::TickWorld* wet = world.worldTick();
                hud.eyeInWater = wet != nullptr && mc::entity::playerEyeInWater(*wet, body);
                // `lu.h` counts ticks; the world's tick count is the same clock.
                hud.updateCounter = u32(editTick);
                renderer.setHud(hud);
            } else {
                renderer.clearHud();
            }

            // **The band that says the bottom screen has the buttons.** Set
            // every frame rather than on the edge, because it costs a bool and
            // the alternative is two places that have to agree about a mode.
            renderer.setFocusHint(overlay.uiFocused());
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
            // The same bracket, for the same reason: a mob's footstep tramples
            // a field, a falling block lands, a dropped stack presses a plate
            // -- all of it out here rather than inside `stepTicks`, and all of
            // it invisible without a renderer held. One scope for every pool
            // and every tick in the frame, so the light it queues is drained
            // once. See WorldStreamer::RenderBracket.
            mc::render::WorldStreamer::RenderBracket draws(world, renderer.chunks());
            for (int i = 0; i < ticksDue; ++i) {
                particles->tick(*fxWorld);
                // **`cn.m(III)V` -- a thousand darts at the blocks round the
                // player**, which is where almost every particle in quiet play
                // comes from: the torch that smokes, the fire that smoulders,
                // the redstone that glitters, the lava that spits. It runs on
                // the world's clock beside the pool it feeds and is skipped
                // entirely by anything with no particle sink. See
                // core/tick/display.hpp.
                mc::tick::displayTick(*fxWorld,
                                      mc::MathHelper::floorDouble(camera.x),
                                      int(mc::MathHelper::floorDouble(camera.y)),
                                      mc::MathHelper::floorDouble(camera.z),
                                      displayRand);
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
                // **The animals.** They need to know where the player is --
                // they despawn by distance, watch one that comes close and are
                // shoved by one that walks into them -- and the shove goes both
                // ways, which is why the body's motion is handed over by
                // pointer. See core/entity/mob.hpp.
                {
                    // **The camera, not the body**, because Spectator has no
                    // body to speak of: it flies and leaves `body` wherever it
                    // was last placed, and animals would then spawn and despawn
                    // around a ghost. In every other mode the camera *is* the
                    // body's eye -- it is assigned from it at the end of each
                    // tick -- so this is the same number.
                    const bool hasBody =
                        overlay.gamemode() != settings::Gamemode::Spectator;
                    mc::entity::MobSurroundings around;
                    around.player.present = true;
                    around.player.x = camera.x;
                    around.player.y = camera.y;
                    around.player.z = camera.z;
                    // **Creative is not hunted.** Present all the same: the
                    // animals still despawn around this position and a cow
                    // still turns its head. See core/entity/mob.hpp.
                    around.player.targetable =
                        overlay.gamemode() != settings::Gamemode::Creative && vitals.alive();
                    // What a hit from this tick's mobs needs and the pools do
                    // not carry. See `PlayerHarm`.
                    harm.world = fxWorld;
                    harm.difficulty = int(difficulty);
                    harm.yawDegrees = camera.yaw * 180.0f / kPi;
                    if (hasBody) {
                        // Only a body can be shoved, and only a body is
                        // something to shove against -- and only a body can be
                        // hit or thrown by a blast. **Spectator is not
                        // invulnerable by a special case**; it simply has
                        // nothing for a fist to reach.
                        around.player.box = body.box;
                        around.player.motionX = &body.motionX;
                        around.player.motionY = &body.motionY;
                        around.player.motionZ = &body.motionZ;
                        around.hurtPlayer.sink = &PlayerHarm::hurt;
                        around.hurtPlayer.ctx = &harm;
                    }
                    around.difficulty = int(difficulty);
                    skeletonBow.arrows = arrows.get();
                    skeletonBow.world = fxWorld;
                    around.shootArrow = &SkeletonBow::shoot;
                    around.shootArrowCtx = &skeletonBow;
                    // A creeper's blast and TNT's both destroy what is lying
                    // near them. See `MobSurroundings::items`.
                    around.items = droppedItems.get();
                    mobs->tick(*fxWorld, around);

                    // **Primed TNT, ticked here and not beside the falling
                    // blocks above**, because the last tick of a fuse is a
                    // blast and a blast needs to know who is standing in it --
                    // which is the `around` this block has just built and
                    // nothing outside it has. It writes blocks like a falling
                    // block does, over a five-block radius rather than one
                    // cell. See core/entity/primed_tnt.hpp.
                    primedTnt->tick(*fxWorld, mobs.get(), &around);

                    // `ia.c()` -- the spawner, every tick and not on a timer.
                    // a1.1.2 has no world-generation spawning at all, so this
                    // is the only thing that ever makes an animal.
                    mc::entity::SpawnContext spawnAt;
                    spawnAt.playerPresent = true;
                    spawnAt.playerX = camera.x;
                    spawnAt.playerY = camera.y;
                    spawnAt.playerZ = camera.z;
                    spawnAt.spawnX = world.level().spawnX;
                    spawnAt.spawnY = world.level().spawnY;
                    spawnAt.spawnZ = world.level().spawnZ;
                    spawnAt.difficulty = int(difficulty);
                    spawnAt.worldSeed = world.level().randomSeed;
                    // `ia.c()` runs both spawners every tick, monsters first,
                    // out of the same random. The order is observable -- the
                    // two share `world.rand` -- so it is the jar's.
                    // A multiplayer client spawns nothing: the server does.
                    if (net == nullptr) {
                        mc::entity::spawnMonsters(*fxWorld, *mobs, spawnRand, spawnAt,
                                                  &monsterSpawns);
                        mc::entity::spawnAnimals(*fxWorld, *mobs, spawnRand, spawnAt);
                    }

                    // **The tile-entity tick list, and it has one member.**
                    // `cn`'s own loop walks every tile entity and calls
                    // `ic.b()`; only `ke` (furnace) and `bd` (mob spawner)
                    // override it, and the furnace is not ported -- so this is
                    // the whole of it. The draws come out of the *world's*
                    // random, which is `fxWorld`'s and not `spawnRand`. See
                    // core/entity/mob_spawner.hpp.
                    mc::entity::tickMobSpawners(*spawners, *fxWorld, *mobs,
                                                fxWorld->random(), spawnAt,
                                                &spawnerCounters);

                    // **A pig moves after its rider does**, unlike a boat or a
                    // cart: it ignores the rider entirely, so it is ticked here
                    // with the other animals rather than in the body loop, and
                    // the rider is put back on it afterwards. See
                    // PlayerBody::followSeat.
                    if (mobs->riddenIndex() >= 0) {
                        const mc::entity::RiderSeat seat = mobs->seat();
                        if (seat.valid) {
                            body.followSeat(seat.x, seat.y, seat.z);
                        } else {
                            mobs->dismount();
                        }
                    }

                    // The Info page's mob line. Counts only; see
                    // Overlay::MobStats on why the damage is one of them.
                    ctr::Overlay::MobStats stats;
                    stats.animals = mobs->animalCount();
                    stats.monsters = mobs->monsterCount();
                    stats.searches = mobs->pathFinder().searches();
                    stats.exhausted = mobs->pathFinder().exhausted();
                    stats.peakSearches = mobs->peakSearchesPerTick();
                    stats.taken = harm.taken;
                    stats.hits = harm.hits;
                    stats.spawned = monsterSpawns.spawned;
                    stats.chunksTried = unsigned(monsterSpawns.chunksTried);
                    stats.floors = unsigned(monsterSpawns.positions - monsterSpawns.noFloor);
                    stats.cages = spawners->count();
                    stats.cagesFired = spawnerCounters.fired;
                    stats.cagesSpawned = spawnerCounters.spawned;
                    overlay.setMobStats(stats);
                }
                // A tile entity does not tick, but its light does: a torch put
                // beside a sign has to brighten it, and the store is the only
                // thing that knows where the signs are.
                signs->refreshLight(*fxWorld);
                spawners->refreshLight(*fxWorld);
                // **Arrows, which move by ray rather than by sweep.** One
                // `rayTraceBlocks` per live arrow per tick, and a stuck one
                // costs a block read and nothing else -- see
                // core/entity/arrow.hpp.
                {
                    // **Arrows reach the mobs and the player now**, which is
                    // the other half of the bow landing and the whole of a
                    // skeleton being dangerous.
                    mc::entity::ArrowTargets hits;
                    hits.paintings = paintings.get();
                    hits.boats = boats.get();
                    hits.minecarts = minecarts.get();
                    hits.mobs = mobs.get();
                    if (overlay.gamemode() != mc::settings::Gamemode::Spectator
                        && vitals.alive()) {
                        hits.playerPresent = true;
                        hits.playerBox = body.box;
                        hits.hurtPlayer = &PlayerHarm::hurt;
                        hits.hurtPlayerCtx = &harm;
                    }
                    arrows->tick(*fxWorld, hits);
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

        // **Water and lava, on the same clock and in the same place.** Six
        // runs rather than six tiles: a flowing fluid's four tiles hold one
        // picture, and a row of two of them is a single pair of copies. See
        // `Atlas::updateTile`.
        if (ticksDue > 0 && fluids->present()) {
            fluids->tick(ticksDue);
            for (int i = 0; i < fluids->writeCount(); ++i) {
                renderer.setAtlasTile(fluids->tile(i), fluids->texels(i), fluids->across(i));
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
        // **The tick comes back out of the stream number.** `beforeStream` to
        // `afterStream` spans the whole of the streaming half of the frame, and
        // the tick runs inside that span -- so adding `tickMs` to `streamMs`, as
        // the overlay's `CPU busy` does, counted the tick twice and made the
        // busy figure larger than the work. The buckets are meant to partition
        // the frame, not overlap it.
        const float streamSpanMs = ctr::millisFromTicks(afterStream - beforeStream);
        timing.streamMs = streamSpanMs > timing.tickMs ? streamSpanMs - timing.tickMs : 0.0f;

        // **After the streamer and before the draw**, because it reads columns
        // out of the grid and the grid is settled for the frame by now. It
        // samples at most a chunk or two and does nothing at all in a mode
        // whose bottom screen has no map on it.
        overlay.tickMap(world, camera);

        if (pauseMenuUp) {
            // **The world, and the menu drawn into the same frame** -- the same
            // arrangement `PauseBackdrop` makes for single player, with the two
            // halves the other way round: there the menu owns the frame and
            // calls back for the world, here the world owns it and calls back
            // for the menu. The bottom screen is the menu's while it is up, so
            // the HUD is not drawn over its console.
            renderer.drawFrame(camera, &menu, &ctr::Menu::pauseOverlayEntry);
        } else {
            renderer.drawFrame(camera);

            overlay.draw(renderer, world, camera, timing, dt * 1000.0f, timeOfDay, settings);
        }

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

    // **The stepped pause menu can be left standing by the exits it does not
    // own.** A link that closed under the player, and the system taking the
    // application away, both leave this loop from below the pause branch -- and
    // a Menu still holding `inGame_`, the Pause screen and citro2d would meet
    // the main menu in that state. The blocking path cannot reach here at all;
    // this is the pair of it. The choice is dropped on purpose: there is no
    // world left to apply a render distance to.
    if (pauseMenuUp) {
        pauseMenuUp = false;
        menu.endPause();
        menu.setMultiplayer(false);
        menu.shutdown();
    }

    // **A disc stops with the world it was in.** The engine is the process's
    // and outlives every world, which is right for background music and wrong
    // for a jukebox: a record left playing followed the player back to the
    // title screen and went on playing over the menu. Reported from play.
    // `stopRecord` touches the voice only if a disc is on it, so a menu track
    // is not cut by this.
    sound.stopRecord();

    // **Anything still in a crafting grid or on the cursor goes back in the
    // inventory**, since the ground it would have dropped on is about to be
    // saved without it. Then the inventory is handed over one last time.
    overlay.closeContainerIntoInventory();
    if (overlay.takeInventoryChange()) {
        std::vector<mc::item::ItemStack> stacks;
        overlay.inventory().save(&stacks);
        world.setPlayerInventory(stacks);
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
    // A multiplayer world owes this console nothing, so there is no screen.
    if (net == nullptr && saving.init()) {
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
    renderer.setRemotePlayers(nullptr);
    // **And the one the *streamer* borrows.** `world` is static and outlives
    // this call (see the note on its declaration), while the spawner store is a
    // local; leaving the sinks bound would point the next world's first
    // adoption at freed memory. `close()` above still needs them -- it drops
    // every cell, and each drop is a column's spawners leaving -- so this is
    // after it and not before.
    world.setColumnSinks(nullptr, nullptr, nullptr, nullptr);
    renderer.setSpawners(nullptr);
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
    // Every effect key this build can name, in one list shared with the host
    // harness that measures what it costs. See core/audio/effect_preload.hpp.
    mc::audio::preloadEffects(*static_cast<mc::audio::SoundEngine*>(arg));
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

// **A multiplayer game is the single-player loop with a session beside it.**
// The socket service comes up the first time it is wanted, the session thread
// connects and logs in, and `runGame` plays the world the server sends; when it
// returns, the menu is told why -- a1.1.2's disconnect screen when the server or
// the network ended it, the server list when the player did.
int runMultiplayer(const ctr::MenuChoice& choice, ctr::Menu& menu, mc::audio::SoundEngine& sound,
                   ctr::NdspBackend& audio, bool isNew3DS, bool haveCstick)
{
    std::string error;
    if (!ctr::startNetwork(&error)) {
        menu.showDisconnected("Failed to connect to the server", error);
        return 0;
    }

    // On the heap: the session's parser buffers and queue, and NetPlay's
    // scratch packets, have no business on a 32 KB main-thread stack.
    auto session = std::make_unique<mc::net::ClientSession>();
    if (!session->start(choice.serverHost, choice.serverPort, choice.username)) {
        menu.showDisconnected("Failed to connect to the server",
                              "The network thread could not be started.");
        return 0;
    }
    auto net = std::make_unique<ctr::NetPlay>(*session);

    const int result =
        runGame(choice, menu, sound, audio, isNew3DS, haveCstick, net.get(), nullptr, nullptr);

    session->stop("Quitting");
    if (net->closed()) {
        std::string detail = net->closeDetail();
        if (!net->placed()) {
            // Which address it actually used, because the one thing a player
            // cannot check from this screen is whether the row says what they
            // think it says.
            char tried[96];
            std::snprintf(tried, sizeof(tried), "  (tried %s port %u)",
                          choice.serverHost.c_str(), unsigned(choice.serverPort));
            detail += tried;
        }
        if (!ctr::haveAddress()) {
            detail += "  The console has no network address at all: check that Wi-Fi is on and "
                      "connected (WPA2 at most -- a 3DS cannot join WPA3).";
        }
        menu.showDisconnected(net->closeTitle(), detail);
    } else {
        menu.showMultiplayer();
    }
    return result;
}

// **A local game is the same loop again, with the radio where the socket was.**
//
// Nothing about `runGame` changes for it: `NetPlay` is handed a
// `PacketChannel` and does not ask what is behind it, and the channel behind
// this one is `core/net/local_channel.hpp` over the link the menu already
// joined. The one extra call is `guest->pump()`, which moves bytes between the
// radio and that channel at the top of each frame.
//
// **The session outlives this function either way.** It is taken from the menu
// on the way in and given back on the way out, so leaving a world does not
// drop a link the player may be about to rejoin through.
int runJoinedLocal(const ctr::MenuChoice& choice, ctr::Menu& menu, mc::audio::SoundEngine& sound,
                   ctr::NdspBackend& audio, bool isNew3DS, bool haveCstick)
{
    std::unique_ptr<ctr::GuestPlay> guest = menu.takeGuest();
    if (!guest) {
        menu.showMultiplayer();
        return 0;
    }

    // On the heap for the same reason the server one is: NetPlay's scratch
    // packets have no business on a 32 KB main-thread stack.
    auto net = std::make_unique<ctr::NetPlay>(guest->channel());
    // The other end is this port, so it understands the one packet protocol 2
    // does not have. See `packet::UseEntity`.
    net->allowUseEntity(true);

    const int result = runGame(choice, menu, sound, audio, isNew3DS, haveCstick, net.get(),
                               nullptr, guest.get());

    guest->leave("left the world");
    if (net->closed()) {
        menu.showDisconnected(net->closeTitle(), net->closeDetail());
    } else {
        menu.showMultiplayer();
    }
    return result;
}

// **Hosting is single player with the radio on.** The world is opened exactly
// as Play opens it -- same loop, same authority, same save -- and the session
// beside it lets other consoles in. A failure to open the session is not a
// failure to open the world, but it is reported before the world opens rather
// than after: a player who chose Host and got single player without being told
// would have no way to know why nobody could find them.
int runHosted(const ctr::MenuChoice& choice, ctr::Menu& menu, mc::audio::SoundEngine& sound,
              ctr::NdspBackend& audio, bool isNew3DS, bool haveCstick)
{
    // **Opened inside runGame, once the world is.** The session has to tell a
    // guest which world it is joining -- seed and generation switches -- and
    // none of that is known until level.dat has been read.
    auto host = std::make_unique<ctr::HostPlay>();

    const int result =
        runGame(choice, menu, sound, audio, isNew3DS, haveCstick, nullptr, host.get(), nullptr);

    host->close("the host closed the world");
    if (!host->everOpened()) {
        menu.showDisconnected("Could not open the session",
                              "Local wireless would not start. Check the wireless switch.");
    } else {
        menu.showMultiplayer();
    }
    return result;
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

        if (choice.action == ctr::MenuChoice::Action::Join) {
            result = choice.link == ctr::MenuChoice::Link::Local
                         ? runJoinedLocal(choice, menu, sound, audio, isNew3DS, haveCstick)
                         : runMultiplayer(choice, menu, sound, audio, isNew3DS, haveCstick);
            if (result != 0) {
                break;
            }
            continue;
        }
        if (choice.action == ctr::MenuChoice::Action::Host) {
            result = runHosted(choice, menu, sound, audio, isNew3DS, haveCstick);
            if (result != 0) {
                break;
            }
            continue;
        }
        if (choice.action != ctr::MenuChoice::Action::Play) {
            break;
        }

        result = runGame(choice, menu, sound, audio, isNew3DS, haveCstick, nullptr, nullptr,
                         nullptr);
        if (result != 0) {
            break;
        }
    }

    ctr::stopNetwork();
    ctr::stopLocalWireless();

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
