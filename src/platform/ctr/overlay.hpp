#pragma once

// The bottom screen.
//
// Four pages, cycled with SELECT + Y (forward) and SELECT + X (back):
//
//   Normal    what a player sees, and **it is a different screen in every
//             gamemode**. That is the split the bottom screen has been waiting
//             for: the three debug pages below are the maintainer's and are the
//             same wherever you are, while the Normal page belongs to whatever
//             the world is being played as. Spectator gets a map and the
//             player's coordinates (see map_screen.hpp); Survival and Creative
//             get the screens their hotbars and inventories will be built on,
//             which today say so and list what the buttons currently do.
//
//             **A mode with no screen of its own is not possible**, because the
//             page is chosen by a switch over the enum with no default -- a
//             mode added later will not compile until it has been given one.
//   Info      the debug readout. Every number here answers a question the
//             design has an opinion about, so a wrong opinion shows up as a
//             number rather than as a vague sense that the game feels slow:
//               * frame split busy/vsync -- whether there is headroom at all
//               * quads and draw calls   -- whether the visibility walk works
//               * pool residency, churn  -- whether the VBO budget holds
//               * free linear and VRAM   -- fragmentation, over minutes
//   Settings  the knobs that change what the renderer does rather than what
//             it reports, plus the teleport row.
//
// **The settings page owns the d-pad, and that is what makes it the right home
// for anything needing a button.** Nothing global has to be spent on a debug
// action: the page is behind a SELECT chord, and while it is up the d-pad is
// consumed here and returns before the rest of the frame sees it. So the
// teleport row is opened with right on the d-pad, the same gesture that
// already edits every other row, and it costs no binding that survival mode
// will want later -- A and B in particular stay free.
//
// It is a text console, redrawn in place with ANSI cursor moves rather than
// cleared, so it does not flicker and costs nothing worth measuring. Only a
// page change clears.

#include "core/render/world_streamer.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/atlas_image.hpp"
#include "platform/ctr/map_screen.hpp"
#include "platform/ctr/renderer.hpp"

namespace mc::ctr {

// What the game loop spent, in milliseconds, outside the renderer. Filled in by
// main.cpp because that is where the phases are.
//
// These exist to settle one question and settle it with numbers: a 17.5 ms
// frame against a 0.8 ms GPU says nothing on its own, because the frame is
// vsync-locked at 59.83 Hz and 16.7 ms of it is *supposed* to be waiting.
// Walk + stream + submit against 16.7 is the figure that says whether there is
// any headroom left, and it is the one that matters for M3.
struct FrameTiming {
    float walkMs = 0.0f;    // frustum + the visibility walk
    float streamMs = 0.0f;  // columns in and out, and the meshing budget
};

// How far the *debug* settings page will let the render distance go.
//
// Not a play limit and deliberately not derived from one. It is the point past
// which the console runs out of newlib heap and the chunk decode aborts -- not
// gracefully, because `Section` allocates its palette and index arrays through
// ordinary `new` and the build has no exceptions, so an allocation failure is
// std::terminate rather than a column that fails to load. Making that
// survivable means threading nothrow through the whole section decode, which
// is worth doing when something needs it and is not worth doing for a debug
// page.
//
// The number: the streamer holds (2d+3)^2 columns at a measured mean of 18,013
// bytes, against a 40 MB newlib heap that also carries the mesher's scratch,
// the builder's vectors and the storage buffers. Reserving 8 MB for those
// leaves room for about 1,860 columns, which is 2d+3 = 43, so d = 20. 24 is
// past that on purpose: a sparse world holds far less than the mean and a
// maintainer asking for 24 should get 24 and find out, rather than be told no
// by an estimate. What this bound prevents is only the case where the number
// is so far past the heap that the console dies before drawing anything.
//
// A denser world than the measured one will abort below this. That is what a
// debug page is for.
inline constexpr int kDebugMaxDistance = 24;

// What a *player* is offered, on the main menu's options screen. The
// distinction from the debug ceiling above is the point: an old 3DS is bounded
// by the 12 MB VBO pool it was measured against, a New one by the heap the
// columns live in. Neither is a cliff -- the pool evicts and the streamer just
// gets slower -- so these are where a player stops getting anything back for
// the cost, and nothing more. Menu::init reads them.
inline constexpr int kPlayMaxDistanceOld3DS = 8;
inline constexpr int kPlayMaxDistanceNew3DS = 12;

// The M2 frame-rate gate the settings page quotes, so the person holding the
// console knows what the number on the Info page is being judged against.
//
// **A floor, not a target, and provisional.** The New 3DS gate was distance 10
// until the sixth launch measured 0.208 us per quad and missed it by 3.2x.
// 8 is what the rest of the engine is already sized around and puts the
// baseline an estimated 2.1x away instead -- a gap the geometry-shader path
// could plausibly close. Raise it back if it does. See docs/status.md section 2.
inline constexpr int kGateDistanceOld3DS = 6;
inline constexpr int kGateDistanceNew3DS = 8;

// The settings page's state. The Overlay edits it; the caller applies it,
// because applying a render distance means rebuilding the field, the pool and
// the streamer's grid and none of that belongs to a text console.
struct DebugSettings {
    int renderDistance = 8;

    // The geometry-shader cube path: one 8-byte vertex per quad instead of four
    // 12-byte ones. **The measurement the M2 gate is waiting on**, which is why
    // it is here at all -- see Renderer::setCubeFormat.
    //
    // Off by default, so what boots is the path that is known to draw correctly
    // and the experiment is something a maintainer turns on deliberately. It
    // costs a re-mesh of everything resident in either direction, so unlike
    // wireframe it is not an instant A/B: give the world a second to settle
    // before reading the numbers back.
    bool geometryQuads = false;

    bool wireframe = false;

    // Bounds for the render distance, set once by the caller.
    //
    // **This is the debug page, so these are not the play limits.** The 8 and
    // 12 a player will get at M3 are a judgement about where a 3DS stops
    // giving anything back for the cost; they have no business stopping a
    // maintainer from looking at distance 20 to see what breaks. The only
    // ceiling left here is the one the hardware actually imposes -- see
    // kDebugMaxDistance.
    int minDistance = 2;
    int maxDistance = kDebugMaxDistance;
};

class Overlay {
public:
    enum class Page {
        Normal,
        Info,
        Storage,
        Settings,
    };
    static constexpr int kPageCount = 4;

    // Remembered so a page change can reprint the header. Also forgets
    // everything the last world's map remembered, which is the one piece of
    // state here that would be actively wrong carried across.
    void begin(const char* worldName, const char* model);

    // **Which screen the Normal page is.** Set at world open from the world's
    // own 3dalpha.ini, and again whenever the pause menu's World Settings
    // screen changes it, so a mode switched in a world takes effect without
    // leaving it.
    void setGamemode(settings::Gamemode mode);
    settings::Gamemode gamemode() const { return gamemode_; }

    // Sizes the map's memory against the model. Once, at world open.
    void configureMap(bool isNew3DS) { map_.configure(isNew3DS); }

    // The pack the map takes its colours from -- the same atlas the world is
    // drawn with, because a map that disagreed with the world about what stone
    // looks like would be worse than one with no colour at all. Called at world
    // open and again when the pause menu changes the pack.
    void setMapAtlas(const texture::AtlasImage& atlas) { map_.setPalette(atlas); }

    // Once a frame. Samples a chunk or two into the map, and does nothing at
    // all in a mode whose screen has no map -- there is no point paying for a
    // picture nothing will draw.
    void tickMap(const render::WorldStreamer& world, const Camera& camera);

    // Which core the generation worker actually got, as a label for the debug
    // page. Asked for and got are different questions -- a New 3DS launched
    // without the core-2 exheader flag falls back to core 0 -- and the page is
    // where that difference has to be visible.
    void setWorkerCore(const char* label) { workerCore_ = label; }

    // SELECT + Y / SELECT + X cycles the page; on the settings page the d-pad
    // moves the cursor and changes the value under it. Returns true when
    // `settings` changed and the caller has work to do.
    //
    // **`camera` is here because the teleport row writes to it directly**, and
    // it is worth being explicit about why that is not a layering slip. A
    // render distance is a *setting* -- the caller has to rebuild the pool and
    // the streamer grid, so it is reported back and applied outside. A
    // teleport is not a setting; it is a one-shot write of three numbers that
    // the next frame picks up on its own, because WorldStreamer::update
    // already re-centres on whatever chunk the camera is in and evicts what
    // fell outside. Routing it through DebugSettings would mean inventing a
    // "pending teleport" field that exists for one frame and means nothing
    // afterwards.
    bool handleInput(u32 down, u32 held, DebugSettings* settings, Camera* camera);

    Page page() const { return page_; }

    // **Anything that takes the bottom screen away has to call this.** The
    // console is redrawn in place with cursor moves rather than cleared, so a
    // page that is still on screen is never reprinted -- and after the swkbd
    // applet or the pause menu has written over it, "still on screen" is no
    // longer true and the player is left looking at a menu's help text with the
    // world running behind it.
    void invalidate() { dirty_ = true; }

    void draw(const Renderer& renderer, const render::WorldStreamer& world, const Camera& camera,
              const FrameTiming& timing, float frameMs, float timeOfDay,
              const DebugSettings& settings);

private:
    // Averaged before being believed: the GPU timers are per-frame and noisy,
    // and a number that jumps every frame cannot be read off a screen anyway.
    // Accumulated on every page, so switching to Info shows a settled figure
    // rather than one frame's.
    struct Accum {
        float frame = 0.0f;
        float draw = 0.0f;
        float process = 0.0f;
        float blocked = 0.0f;
        float submit = 0.0f;
        float walk = 0.0f;
        float stream = 0.0f;
    };

    // Each draws its page starting at the body's first row and returns the
    // first row it did not use, so the caller can blank the rest. They place
    // every line absolutely and never write a newline -- see `row()` in
    // overlay.cpp for why that is the whole point.
    // The Normal page, per gamemode. Each draws its own body and returns the
    // first row it did not use, exactly like the debug pages -- except the
    // spectator screen, which owns its rows *and* the pixels beside them and so
    // is drawn straight from `draw` rather than through this shape.
    int drawSurvival();
    int drawCreative();
    int drawInfo(const Renderer& renderer, const render::WorldStreamer& world,
                 const Camera& camera, float timeOfDay);
    // **The page this whole arrangement is answerable to.** `main` is
    // main-thread microseconds spent inside a storage call; it is expected to
    // read 0.0, and anything else means the card is back on the render thread.
    int drawStorage(const render::WorldStreamer& world);

    int drawSettings(const Renderer& renderer, const DebugSettings& settings,
                     const Camera& camera);

    // Opens the system keyboard and, if it comes back with three valid numbers,
    // moves the camera. Returns true if the camera moved.
    //
    // The applet takes over both screens while it runs, so the caller has to
    // treat the bottom-screen console as destroyed and reprint it.
    static bool teleportViaKeyboard(Camera* camera);

    static constexpr int kSettingCount = 4;

    settings::Gamemode gamemode_ = settings::Gamemode::Spectator;
    MapScreen map_;

    Page page_ = Page::Normal;
    int cursor_ = 0;
    bool dirty_ = true;  // the page changed, so clear before drawing it

    const char* worldName_ = "";
    const char* model_ = "";
    const char* workerCore_ = "?";

    Accum accum_;
    Accum shown_;

    // **The chunk cache's counters are cumulative, and a cumulative counter
    // cannot answer "is it happening now".**
    //
    // The one that matters most is main-thread time inside a storage call,
    // which is supposed to be zero. Reported as a session total it never reads
    // zero -- opening a world stats a few hundred chunks before the directory
    // listings land, and that number then sits on the screen for the rest of
    // the session looking like a fault. Read on hardware as "4000 ms", which is
    // four seconds accumulated over a smooth session rather than four seconds
    // in a frame.
    //
    // So the page shows the delta over one sample block as well as the total.
    // The delta is the diagnosis; the total is the history.
    world::ChunkCache::Stats ioPrevious_;
    world::ChunkCache::Stats ioDelta_;
    int samples_ = 0;
};

}  // namespace mc::ctr
