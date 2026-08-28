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

#include "platform/ctr/heap.hpp"
#include "platform/ctr/menu.hpp"
#include "platform/ctr/overlay.hpp"
#include "platform/ctr/probe.hpp"
#include "platform/ctr/renderer.hpp"
#include "core/io/volume_info.hpp"
#include "core/render/world_streamer.hpp"
#include "core/util/memory.hpp"
#include "core/util/worker.hpp"
#include "core/world/chunk_cache.hpp"
#include "core/world/daylight.hpp"

#include "version_config.hpp"

#include <3ds.h>
#include <citro3d.h>

#include <cmath>
#include <cstdio>

namespace {

using namespace mc;

constexpr float kPi = 3.14159265358979f;

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

// The "Saving level.." screen's counter.
//
// It is the bottom-screen console rather than the top, because the top screen
// still holds the last frame of the world and the console is where every other
// word this shell says to the player goes. `owed` is 0 when there was nothing
// to write, which is worth its own line: it is the answer to "did pressing
// START a moment ago already do this", and the answer is yes.
struct SaveScreen {
    // **Redrawn on the number, not on the poll.** close() reports every 16 ms
    // and consoleInit turns double buffering off, so each print is a clear and
    // a redraw of the bottom screen straight into the framebuffer -- sixty a
    // second of that, to show the same figure, is work taken off the thread
    // that is trying to write the world.
    int lastPercent = -1;
};

void printSaveProgress(void* context, u32 written, u32 owed)
{
    SaveScreen& screen = *static_cast<SaveScreen*>(context);
    const int percent = owed == 0 ? 100 : int((written * 100u) / owed);
    if (percent == screen.lastPercent) {
        return;
    }
    screen.lastPercent = percent;

    std::printf("\x1b[2J\x1b[1;1H\x1b[32mSaving level..\x1b[0m ");
    if (owed == 0) {
        std::printf("already saved\n");
        return;
    }
    // `u32` is `unsigned long` on this toolchain, hence the casts rather than
    // %u on the value itself.
    std::printf("%3d%%\n\n", percent);
    std::printf("%lu of %lu columns\n", (unsigned long)written, (unsigned long)owed);
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

// Free flight. There is no collision because there is no player body yet, and
// pretending otherwise would be the wrong kind of faithful.
void moveCamera(ctr::Camera& camera, float dt, bool sprint)
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
    if (held & KEY_R) {
        camera.y += speed;
    }
    if (held & KEY_L) {
        camera.y -= speed;
    }

    camera.y = camera.y < 1.0 ? 1.0 : (camera.y > 254.0 ? 254.0 : camera.y);
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
    // SELECT + Y changes the bottom-screen page, so Y with SELECT held is not
    // this. Without the guard, cycling pages would leave the d-pad live on the
    // stereo numbers for as long as the finger stayed on Y.
    const u32 held = hidKeysHeld();
    if (!(held & KEY_Y) || (held & KEY_SELECT)) {
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

void* spawnWorker(void (*entry)(void*), void* arg, mc::WorkerRole role)
{
    // Read on the main thread, which is where this runs. The kernel refuses a
    // thread priority numerically below the process's own, so the main
    // thread's value is the highest this may ask for.
    s32 mainPriority = 0x30;
    svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);

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
int runGame(const ctr::MenuChoice& choice, ctr::Menu& menu, bool isNew3DS, bool haveCstick)
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
    gWorkerIsNew3DS = isNew3DS;
    gWorkerOnCore2 = false;
    mc::setWorkerThreadOps(&spawnWorker, &joinWorker);

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

    ctr::Camera camera;
    world.spawnPosition(&camera.x, &camera.y, &camera.z);
    camera.y += 1.62;  // eye height, the original's own number
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
    // Double rather than i64 because a frame is a fraction of a tick and the
    // sky needs the fraction; a world would have to run for millions of days
    // before the mantissa stopped naming individual ticks.
    double worldTicks = double(world.level().time);

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
    // **The bottom screen is the world's gamemode's**, which is why this is read
    // off the choice rather than assumed: Spectator gets the map, and the modes
    // that will have a hotbar get the screens it is going in.
    overlay.setGamemode(choice.gamemode);
    overlay.configureMap(isNew3DS);
    // The same atlas the world is drawn with. `choice` outlives the game loop,
    // but the palette is copied out of it here and not held.
    overlay.setMapAtlas(choice.atlas);
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
    // pre-generated by a second code path, and the wait ends when there is
    // something on the screen rather than at a fixed column count.
    //
    // Three ways out, because "generating" must never become "hung": geometry
    // exists, or nine columns are resident and the spawn is genuinely open sky
    // and air, or the player presses START. The cap is 30 seconds at the
    // refresh rate -- long enough for a New 3DS worker to make the nine columns
    // a section needs before it can be meshed, short enough that a console that
    // cannot is still usable.
    if (choice.created) {
        constexpr int kMaxWaitFrames = 1800;
        for (int waited = 0; waited < kMaxWaitFrames && aptMainLoop(); ++waited) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                break;
            }

            const Frustum frustum = renderer.cullFrustum(camera);
            renderer.chunks().beginFrame(++frameCounter, frustum, camera.chunkX(),
                                         camera.sectionY(), camera.chunkZ());
            world.update(renderer.chunks(), camera.chunkX(), camera.chunkZ(), budget);
            renderer.drawFrame(camera);

            if (renderer.frameStats().quads > 0 || world.stats().columnsResident >= 9) {
                break;
            }

            // Once a second, and the numbers are the streamer's own: `owed` is
            // columns in range that do not exist yet, `asking` how many cells
            // are still waiting to be told whether the world already has them.
            // Owed falling is progress; owed high with nothing being asked is
            // the stall the debug page was built to name.
            if (waited % 60 == 0) {
                const render::WorldStreamer::Stats& stats = world.stats();
                std::printf("\x1b[2J\x1b[1;1H");
                std::printf("\x1b[32mGenerating %s\x1b[0m\n\n", choice.worldName.c_str());
                std::printf("seed  %lld\n", (long long)world.level().randomSeed);
                std::printf("owed  %d\n", stats.pendingGeneration);
                std::printf("asking %d cells\n", stats.unclassified);
                std::printf("here  %d columns\n\n", stats.columnsResident);
                std::printf("worker %s\n\n", stats.workerRunning ? "running" : "\x1b[31mnot running\x1b[0m");
                std::printf("START to go in anyway.\n");
            }
        }
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
            // **Save now, but only what is owed.** Everything the autosave
            // timer would write: dirty columns, level.dat with the position and
            // the world clock, and the session.lock refresh. The world stops
            // dead while the menu is up, so the I/O thread has the whole of it
            // to itself and is finished long before the player resumes --
            // nothing here waits for it. It is also more than the original
            // does: a1.1.2 only writes everything out on Save and quit to
            // title.
            //
            // **Nothing dirty means nothing written at all**, which is what
            // makes opening the pause menu twice in a row, or opening it and
            // leaving, cost one save rather than two. What is given up is the
            // level.dat refresh that would have ridden along with it -- the
            // position and the world clock -- and that is covered twice over
            // already: the autosave timer writes it on its own interval, and
            // close() writes it on the way out. `dirtyColumns` is read from the
            // cache rather than from the frame's copy of its counters, which is
            // as old as the last update().
            const bool owed = world.dirtyColumns() > 0;
            if (owed) {
                world.saveNow(ctr::nowMillis());
            }

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
            overlay.setGamemode(paused.gamemode);

            if (paused.atlasChanged) {
                if (!renderer.setAtlas(menu.atlas())) {
                    std::printf("\x1b[31mcould not upload that pack\x1b[0m\n");
                    overlay.invalidate();
                }
                // The map is drawn from the same pack as the world, so ground
                // sampled under the old one is recoloured rather than redrawn:
                // the store holds block ids, not pixels.
                overlay.setMapAtlas(menu.atlas());
                overlay.setBackdropTile(menu.backgroundTile());
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
            }
            // Refuses if there was no room for the outline atlas, so the
            // setting is read back from the renderer rather than assumed.
            renderer.setWireframe(settings.wireframe);
            settings.wireframe = renderer.wireframe();
        }

        const u64 tick = svcGetSystemTick();
        float dt = float(double(tick - lastTick) / double(SYSCLOCK_ARM11));
        lastTick = tick;
        // A frame that took a quarter of a second is the console having been
        // suspended, not the player having moved that far.
        dt = dt > 0.25f ? 0.25f : dt;

        // X is sprint and also SELECT + X is a page back, so sprint waits for
        // SELECT to be let go.
        moveCamera(camera, dt, (held & KEY_X) != 0 && !(held & KEY_SELECT));
        lookWithTouch(camera, &dragging, &lastTouch, overlay.touchLookTop());
        if (haveCstick) {
            lookWithCstick(camera, dt);
        }
        tuneStereo(renderer);

        // Day and night advance at the original's rate and touch nothing but
        // the lightmap: 20 ticks per second out of 24000 in a day.
        worldTicks += double(dt) * 20.0;

        // Alpha's own curve, from the jar. It holds full brightness for the
        // first half of the day rather than peaking at noon, which is the
        // difference between a world that looks like Alpha and one that looks
        // permanently overcast.
        const double dayTicks = worldTicks - std::floor(worldTicks / 24000.0) * 24000.0;
        const float timeOfDay = float(dayTicks / 24000.0);
        renderer.setSkyDarken(
            world::skyLightSubtracted(i64(dayTicks), float(dayTicks - std::floor(dayTicks))));

        // **Where the player is and what time it is, for whatever saves next.**
        // Four stores a frame; the autosave timer below, the flush when the
        // pause menu opens, and close() on the way out all read it. Before this
        // a world always reopened where it was first entered and at the time it
        // was created.
        world.setPlayerState(camera.x, camera.y, camera.z, camera.yaw * 180.0f / kPi,
                             camera.pitch * 180.0f / kPi, i64(worldTicks));

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
    }

    // The original says the same thing on its way out of a world, and it is
    // worth saying: close() rewrites level.dat and flushes every dirty column,
    // which on a card is long enough for a still screen to look like a hang.
    // **With a percentage**, because "long enough to look like a hang" is
    // exactly the case a number answers and a word does not -- and because a
    // world saved a moment ago by the pause menu owes nothing at all, which
    // this now says rather than sitting on the same still screen.
    SaveScreen saveScreen;
    world.close(ctr::nowMillis(), &saveScreen, printSaveProgress);
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
int runShell(bool isNew3DS, bool haveCstick)
{
    C3D_Init(ctr::kCommandBufferBytes);

    // One menu for the process, so the options screen and the cursor remember
    // where they were between worlds.
    ctr::Menu menu;
    int result = 0;

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

        result = runGame(choice, menu, isNew3DS, haveCstick);
        if (result != 0) {
            break;
        }
    }

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
