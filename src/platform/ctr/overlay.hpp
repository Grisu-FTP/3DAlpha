#pragma once

// The bottom screen.
//
// **Two halves, and the split is who they are for.** The player's half is a
// tabbed HUD -- a map, an inventory and a pad to look around with -- drawn as
// panels and slots in a1.1.2's own GUI colours, switched by touching the tabs
// along the top. The maintainer's half is three text pages behind SELECT + Y,
// unchanged and deliberately still a text console.
//
//   Player    the tab strip, and one of:
//               **Map**    the world around the player at one pixel per block,
//                          with their coordinates beside it and nothing else.
//                          See map_screen.hpp.
//               **Items**  the inventory, drawn empty until M3 fills it. Only
//                          in Survival and Creative -- Spectator has no
//                          inventory, so it is not offered one.
//               **Look**   a pad to drag on, with a compass ribbon over it.
//
//             **The Look page exists because a drag has to belong to someone.**
//             The bottom screen is both the game's UI and the only pointing
//             device an old 3DS has, and while the whole screen was a debug
//             console those two never collided. A map you can touch does
//             collide: dragging on it used to turn the camera, which is the
//             opposite of what touching a map means. So the pages say who owns
//             the touch -- the UI everywhere except here, and the camera here.
//
//             **A mode with no page set is not possible**, because the tabs are
//             built by a switch over the gamemode enum with no default -- a
//             mode added later will not compile until it has been given one.
//   Info      the debug readout. Every number here answers a question the
//             design has an opinion about, so a wrong opinion shows up as a
//             number rather than as a vague sense that the game feels slow:
//               * frame split busy/vsync -- whether there is headroom at all
//               * quads and draw calls   -- whether the visibility walk works
//               * pool residency, churn  -- whether the VBO budget holds
//               * free linear and VRAM   -- fragmentation, over minutes
//               * what a map redraw cost -- which used to be on the map itself
//   Storage   what the card is doing, and who is waiting for it.
//   Settings  the knobs that change what the renderer does rather than what
//             it reports, plus the teleport row and the map's debug grids.
//
// **The settings page owns the d-pad, and that is what makes it the right home
// for anything needing a button.** Nothing global has to be spent on a debug
// action: the page is behind a SELECT chord, and while it is up the d-pad is
// consumed here and returns before the rest of the frame sees it. So the
// teleport row is opened with right on the d-pad, the same gesture that
// already edits every other row, and it costs no binding that survival mode
// will want later -- A and B in particular stay free.
//
// The text pages are a console, redrawn in place with ANSI cursor moves rather
// than cleared, so they do not flicker and cost nothing worth measuring. Only a
// page change clears. The player's pages are the same, one step further: the
// panels behind the text are drawn once on a page change and the text over them
// only when the number in it moved.

#include "core/gui/paint.hpp"
#include "core/render/world_streamer.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/background.hpp"
#include "platform/ctr/hud.hpp"
#include "platform/ctr/map_screen.hpp"
#include "platform/ctr/renderer.hpp"

#include <vector>

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
        Player,
        Info,
        Storage,
        Settings,
    };
    static constexpr int kPageCount = 4;

    // The player's pages, in the order their tabs are laid out. Which of them
    // a gamemode offers is `tabs()`; Spectator has no inventory and so has no
    // Items tab.
    enum class PlayerPage {
        Map,
        Items,
        Look,
    };

    // Remembered so a page change can reprint the header. Also forgets
    // everything the last world's map remembered, which is the one piece of
    // state here that would be actively wrong carried across.
    void begin(const char* worldName, const char* model);

    // **Which tabs the player's half offers.** Set at world open from the
    // world's own 3dalpha.ini, and again whenever the pause menu's World
    // Settings screen changes it, so a mode switched in a world takes effect
    // without leaving it. Leaving Survival or Creative for Spectator on the
    // Items page moves off it, because the page is gone.
    void setGamemode(settings::Gamemode mode);
    settings::Gamemode gamemode() const { return gamemode_; }

    // Sizes the map's memory against the model. Once, at world open.
    void configureMap(bool isNew3DS) { map_.configure(isNew3DS); }

    // The pack the map takes its colours from -- the same atlas the world is
    // drawn with, because a map that disagreed with the world about what stone
    // looks like would be worse than one with no colour at all. Called at world
    // open and again when the pause menu changes the pack.
    void setMapAtlas(const texture::AtlasImage& atlas) { map_.setPalette(atlas); }

    // The backdrop behind the player's panels: the pack's `dirt.png`, tiled and
    // darkened exactly as a1.1.2's own menus tile it -- `Menu::backgroundTile`,
    // 32 x 32 RGBA, and empty for a pack that has neither a dirt.png nor an
    // atlas to take one from. Empty leaves a flat colour, which is a backdrop
    // and not a failure. Converted to RGB565 here and held, because the
    // framebuffer wants it in that format 76,800 times per page change.
    void setBackdropTile(const std::vector<u8>& rgba);

    // Once a frame. Samples a chunk or two into the map.
    //
    // **In every gamemode, and no longer only in the one looking at it.** The
    // map used to be Spectator's alone; every mode has the tab now, and a map
    // that only remembered ground while its own page was up would be blank
    // every time a player opened it. A sample is 1.3 microseconds and the store
    // is allocated at world open whatever the mode, so what this costs is what
    // it always cost.
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

    // **The first pixel row a drag may look around on, or -1 for none.**
    //
    // The bottom screen is the only pointing device an old 3DS has and it is
    // also the game's UI, so exactly one of the two owns each press. The Look
    // page hands the pad below the tab strip to the camera; the map and the
    // inventory keep it; the debug pages hand over the whole screen, which is
    // what they have always done. A touch that began on a tab keeps the strip
    // for as long as the finger is down, so a drag that wanders out of it
    // cannot end up turning the view.
    int touchLookTop() const;

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
    hud::TabStrip tabs() const;
    // Which tab index the current page is, and which page a tab index is. The
    // two are not the same mapping in every gamemode, because Spectator has no
    // Items tab, and going through a pair of functions is what keeps the tab
    // strip and the page from ever disagreeing about that.
    int selectedTab() const;
    void selectTab(int index);
    const char* footerHint() const;

    // The player's half. Each returns true when it put pixels on the screen, so
    // the cache flush the LCD needs happens when there was something to flush
    // and not once a frame.
    bool drawPlayerPage(const Camera& camera, bool cleared);
    bool drawLook(const gui::Surface& surface, const Camera& camera, bool cleared);

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

    // Render distance, cube format, wireframe, the map's debug grid, teleport.
    static constexpr int kSettingCount = 5;

    settings::Gamemode gamemode_ = settings::Gamemode::Spectator;
    MapScreen map_;

    // The pack's dirt, in the format the framebuffer wants it. 2 KB, and the
    // Overlay lives for the process, so it is not on anybody's stack.
    gui::Pixel backdrop_[hud::kTileEdge * hud::kTileEdge] = {};
    bool haveBackdrop_ = false;

    Page page_ = Page::Player;
    PlayerPage playerPage_ = PlayerPage::Map;

    // Set when a touch began on the tab strip, cleared when the finger comes
    // up. See touchLookTop().
    bool uiTouchActive_ = false;

    // The compass ribbon's last angle, rounded the way the map's marker is, so
    // the look pad redraws when the player has turned far enough to move it and
    // not sixty times a second while they hold still.
    int lookYawStep_ = -1;
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
