#pragma once

// The bottom screen.
//
// **Two halves, and the split is who they are for.** The player's half is a
// tabbed HUD -- a map, an inventory and a pad to look around with -- drawn as
// panels and slots in a1.1.2's own GUI colours, switched by touching the tabs
// along the top. The maintainer's half is three text pages behind SELECT + Y,
// unchanged and deliberately still a text console.
//
//   Player    the tab strip, the hotbar along the bottom, and one of:
//               **Map**    the world around the player, with their coordinates
//                          beside it and nothing else. The d-pad zooms it and
//                          cycles the chunk and map-tile grids over it, and
//                          that is the one player page that reads the d-pad at
//                          all -- unless the screen is focused, see below. See
//                          map_screen.hpp.
//               **Items**  the inventory, drawn empty. Only in Survival and
//                          Creative -- Spectator carries nothing, so it is not
//                          offered one.
//               **Blocks** the Creative block palette, which is **not** the
//                          inventory and is a page of its own for that reason:
//                          a catalogue of every block the version defines, held
//                          by nobody. Creative only.
//               **Look**   a pad to drag on, with a compass ribbon over it.
//
//             **The hotbar is a band under all four of them**, in every mode
//             that has one, so what is in your hand is on the screen whatever
//             page you left it on. See hud.hpp for why it is here and not over
//             the world.
//
//             **X focuses the bottom screen, and that is what makes it usable
//             without touching it.** A resistive screen wants a stylus, and a
//             player holding the console to walk does not have one out. Focused,
//             the d-pad drives a cursor over the palette and the hotbar and A
//             picks; unfocused, the d-pad goes back to the map. ZL and ZR change
//             the held slot in either state, which is the New 3DS's shoulder
//             pair doing what a mouse wheel does -- on an old console the
//             focused d-pad is the way, which is the other reason the focus
//             exists.
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
#include "platform/ctr/audio.hpp"
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

    // **The world tick, on its own.** It used to be inside streamMs along with
    // update(), sound.tick() and tickSaves(), which is four different things
    // under one number and no way to tell from a console which of them a frame
    // went into. The tick is the one that varies most: TickTimer lets ten whole
    // ticks fall due in a single frame after any stall (tick_timer.hpp), and a
    // tick random-ticks every loaded column, so this is the number that says
    // whether a dropped frame was the tick catching up.
    float tickMs = 0.0f;
    int ticksRun = 0;
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
    // 12-byte ones. See Renderer::setCubeFormat.
    //
    // **On by default, as of the run that drew the whole render distance
    // through it without a stall.** It was off for five hardware launches while
    // it was an experiment that hung the GPU (docs/3ds-performance.md §2), and
    // "off by default" was the right answer for exactly as long as that was
    // true. It cuts vertex traffic 7.5x and vertex-shader invocations 4x, and
    // the M2 gate is 3.2x away; booting into the slow path to protect against a
    // failure that no longer happens costs that on every frame.
    //
    // **The 4-vertex path stays**, and this flag is what selects it -- by hand
    // on this page, or by the watchdog in main.cpp when a frame does not come
    // back. It is also the only path that can ever carry per-corner light or a
    // biome tint, so it is a baseline rather than a legacy.
    //
    // Either direction costs a re-mesh of everything resident, so unlike
    // wireframe it is not an instant A/B: give the world a second to settle
    // before reading the numbers back.
    bool geometryQuads = true;

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
        Blocks,
        Look,
    };
    // Every page, in tab order, for whichever gamemode is set; returns how many
    // were written. **The one place the mapping lives**, so the tab strip, the
    // selected index and a tap on a tab cannot end up disagreeing about which
    // page a mode's third tab is -- which they could when each of the three
    // worked it out for itself.
    int playerPagesFor(PlayerPage* out) const;

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

    // The pack, for the two things on this screen that are made of it: the
    // map's colours and the block icons in the hotbar and the palette. The same
    // atlas the world is drawn with, because a map or a slot that disagreed
    // with the world about what stone looks like would be worse than one with
    // no colour at all.
    //
    // **The pixels are borrowed, not copied** -- 256 KB is not something this
    // object should hold a second time -- so this has to be called again
    // whenever the pack changes, which is at world open and after the pause
    // menu's Texture Pack screen. An atlas that has not been built yet leaves
    // the icons unpainted rather than painting the wrong thing.
    void setAtlas(const texture::AtlasImage& atlas);

    // What is in the player's hand, for the placement path. **Held here because
    // the hotbar is HUD state**: it is drawn, touched and cursored on this
    // screen and nowhere else, and nothing outside reads it but the one line in
    // the edit path that asks what to place. It is not saved -- see
    // core/item/hotbar.hpp.
    const item::Hotbar& hotbar() const { return hotbar_; }

    // Whether this gamemode has a hotbar at all. Spectator does not: it has no
    // body, no reach and nothing to hold.
    bool hasHotbar() const { return gamemode_ != settings::Gamemode::Spectator; }

    // **Whether the bottom screen has the buttons.** X toggles it; while it is
    // on, the d-pad drives the cursor and the shoulders change tab, so the
    // caller has to suspend break and place for as long as it is true. One
    // press does one thing, and this is the flag that guarantees it.
    bool uiFocused() const { return focus_; }

    // **Whether the circle pad is scrolling the map rather than walking.** True
    // only while the screen is focused *and* the map is the page up, which is
    // the one combination where the stick has somewhere better to be. The
    // caller zeroes the body's heading while it holds -- panning and walking at
    // once would be two things fighting over the same window.
    bool mapPanActive() const { return focus_ && playerPage_ == PlayerPage::Map; }

    // Once a frame, after handleInput. Reads the circle pad and scrolls the map
    // when `mapPanActive()`; does nothing at all otherwise. `dt` is seconds,
    // because a pan is a gesture and belongs on the frame clock -- unlike the
    // body, whose every constant is per tick.
    void tickFocus(float dt);

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
    // moves the cursor and changes the value under it, and on the map page it
    // zooms and cycles the grids. Returns true when `settings` changed and the
    // caller has work to do -- which the map page never does, since everything
    // it changes is its own.
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

    // The audio backend, borrowed, for the Info page's decode and underrun
    // rows. Null on any build without audio, in which case those rows say so
    // rather than disappearing -- a missing row reads as "fine" and a silent
    // console is exactly what these numbers are for.
    void setAudio(const NdspBackend* audio) { audio_ = audio; }

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
        float tick = 0.0f;
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

    // The player's half. Each returns true when it put pixels on the screen, so
    // the cache flush the LCD needs happens when there was something to flush
    // and not once a frame.
    bool drawPlayerPage(const Camera& camera, bool cleared);
    bool drawLook(const gui::Surface& surface, const Camera& camera, bool cleared);
    void drawBlocks(const gui::Surface& surface);

    // The dark strip under the tab strip that says the screen is focused, and
    // what the buttons mean while it is. Drawn last, over the page -- see
    // hud::drawFocusBanner.
    void drawFocusBanner(const gui::Surface& surface) const;

    // Turns the focus off and puts everything it owned back: the palette
    // cursor, the map's pan, and a full redraw, because the banner darkened
    // pixels that can only be restored by drawing them again.
    void releaseFocus();

    // The focused d-pad, A and B, and the shoulder pair. Returns true when
    // something it changed has to be redrawn.
    bool handleFocusedInput(u32 down);

    // Where the palette cursor is, as a palette index rather than a cell.
    int paletteIndex() const { return palettePage_ * hud::kPalettePerPage + paletteCursor_; }
    void showBlockInPalette(block::BlockId id);

    const NdspBackend* audio_ = nullptr;

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

    // Render distance, cube format, wireframe, teleport. The map's grids were a
    // fifth; they are under the d-pad on the map page now.
    static constexpr int kSettingCount = 4;

    settings::Gamemode gamemode_ = settings::Gamemode::Spectator;
    MapScreen map_;

    // The nine slots, and where the palette and the focus are looking.
    item::Hotbar hotbar_;
    int palettePage_ = 0;
    int paletteCursor_ = 0;

    // **Focus is two booleans and not a mode enum**, because there are exactly
    // three states and the third is not reachable: the screen is unfocused, or
    // it is focused on the hotbar row, or it is focused on the palette grid --
    // and the last is only possible on the Blocks page, which is enforced where
    // the page changes rather than represented here.
    bool focus_ = false;
    bool focusPalette_ = false;

    // The atlas's pixels, borrowed. Null until a pack has been handed over.
    const u8* atlasRgba_ = nullptr;

    // **Two dirty flags rather than one**, because the hotbar and the page
    // above it change at completely different rates: a shoulder press moves the
    // selection sixty times a second if it is held, and the palette behind it
    // has not changed at all. Redrawing the page for a hotbar move would be
    // 45 slot bevels and 45 icon blits to move one white rectangle.
    bool hotbarDirty_ = true;
    bool bodyDirty_ = true;

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
