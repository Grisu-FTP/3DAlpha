#pragma once

// The bottom screen.
//
// **Two halves, and the split is who they are for.** The player's half is a
// tabbed HUD -- a map, an inventory and a pad to look around with -- drawn as
// panels and slots in a1.1.2's own GUI colours, switched by touching the tabs
// along the top. The maintainer's half is three text pages behind SELECT + Y,
// unchanged and deliberately still a text console.
//
//   Player    the hotbar along the top, the tab strip along the bottom, and
//             one of:
//               **Map**    the world around the player, with their coordinates
//                          beside it and nothing else. The d-pad zooms it and
//                          cycles the chunk and map-tile grids over it, and
//                          that is the one player page that reads the d-pad at
//                          all -- unless the screen is focused, see below. See
//                          map_screen.hpp.
//               **Inventory** what the player is carrying, drawn empty. Only
//                          in Survival and Creative -- Spectator carries
//                          nothing, so it is not offered one.
//               **Items**  the Creative palette, which is **not** the
//                          inventory and is a page of its own for that reason:
//                          a catalogue of every item the version defines, held
//                          by nobody. Creative only.
//
//             **The two labels were "Items" and "Blocks" and are the other way
//             about now.** The palette was blocks-only when it was named and is
//             the whole item table today -- swords, ingots, armour, and the two
//             music discs -- so "Blocks" described a third of the page. The
//             enum members are still `Items` and `Blocks`; a page's name in
//             code is not what a player reads.
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
//             **X is the throw while a stack is picked up**, and the focus
//             toggle only when the hands are empty. A stack lifted off a slot
//             is the cursor of a1.1.2's own container screens, and clicking
//             outside the window with a full cursor spills it -- there is no
//             window to click outside of here, so the button that would
//             otherwise be spare takes the gesture. One press still does one
//             thing; which thing is decided by whether anything is in hand.
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

#include "core/entity/arrow.hpp"
#include "core/entity/item_entity.hpp"
#include "core/gui/container_layout.hpp"
#include "core/gui/stick_cursor.hpp"
#include "core/gui/paint.hpp"
#include "core/item/container_session.hpp"
#include "core/tick/tick_world.hpp"
#include "core/render/world_streamer.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/background.hpp"
#include "core/world/sign_store.hpp"
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

    // Greedy meshing: equal neighbouring cube faces drawn as one quad, runs of
    // up to 3x3. See MeshBuilder::setGreedy and core/mesh/cube_atlas.hpp.
    //
    // **On by default.** The real 1119-column world meshes to 2.02x fewer cube
    // quads with it, and about half the pool memory, for ~2 % more mesh time;
    // on a console, runs of 4x4 took a frame from 34 ms GPU / 44 ms CPU to
    // 22 / 25 (docs/status.md §22). This row is the A/B: flip it and read the
    // Info page. Like the cube format it re-meshes everything resident, so give
    // the world a moment before reading.
    bool greedyMeshing = true;

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

    // The player's pages. **This is not the order their tabs are laid out in**
    // -- that is `playerPagesFor`, which is also the one place it lives -- and
    // it is not the order they were added in either any more. Which of them a
    // gamemode offers is `tabs()`; Spectator has no inventory and so has no
    // `Items` page.
    //
    // **These names are older than the labels on them.** `Items` is the
    // inventory and is drawn "Inventory"; `Blocks` is the Creative palette and
    // is drawn "Items", because it stopped being blocks-only. See `tabs()`.
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
    // world's own alpha.ini, and again whenever the pause menu's World
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

    // **The rest of the session, for the map's markers.** Straight through to
    // `MapScreen::setSession`; the overlay holds the map and the frame loop
    // holds the session, and this is where the two meet. Null in single
    // player.
    void setSession(const net::RemoteEntities* players, i32 selfEntityId)
    {
        map_.setSession(players, selfEntityId);
    }

    // What the player is carrying. **Held here because it is what this screen
    // is**: every slot in it is drawn, touched and cursored on the bottom
    // screen and nowhere else, and the only thing outside that reads it is the
    // one line in the edit path asking what to place.
    //
    // It *is* saved now -- see core/item/inventory.hpp -- which is why the two
    // methods below exist: `setInventory` hands the world's own stacks in at
    // world open, and `takeInventoryChange` tells the caller when to hand them
    // back. Polling the whole thing every frame would be a forty-stack compare
    // for a thing that changes when a button is pressed.
    const item::Inventory& inventory() const { return inventory_; }

    // **The live compass face, handed down from the world tick.**
    //
    // `texels` is 16 x 16 RGBA or null, and `tile` is which items-sheet tile it
    // stands in for. See core/texture/compass_fx.hpp for why a compass is a
    // texture; the override itself is `gui::IconSheets::animatedItems`.
    //
    // **It marks the screen dirty only when something visible draws that
    // tile.** A compass in a chest twenty blocks away must not cost a bottom
    // screen redraw twenty times a second, and a player carrying none must
    // cost nothing at all -- so this scans the nine hotbar slots and, when a
    // page that shows slots is open, that page's, and returns without touching
    // a dirty flag when none of them is it.
    void setAnimatedItemsTile(int tile, const u8* texels);

    // **The sign editor**, which is `GuiEditSign` and is the half of "signs
    // don't work" that is not the renderer: a sign placed with no keyboard can
    // never say anything.
    //
    // **One keyboard for all four lines, not four keyboards.** The original
    // edits a sign in place with the arrow keys moving between lines; a console
    // has a system keyboard applet and opening it four times to write one sign
    // would be worse than the thing it replaces. So this is a multi-line
    // keyboard and the lines come back split on newline, truncated to fifteen
    // characters each -- which is the editor's own limit.
    //
    // Returns whether anything was confirmed. Must not be called between
    // C3D_FrameBegin and C3D_FrameEnd, for the reason `teleportViaKeyboard`
    // gives at length.
    bool editSignViaKeyboard(world::SignStore* store, int index);

    // The stacks a world was loaded with. Called once, after `begin`. An empty
    // list means a player who has never carried anything, and gets the
    // palette's opening hand instead of an empty screen -- which is what
    // Creative wants and is not something that can be saved as "no inventory".
    void setInventory(const std::vector<item::ItemStack>& stacks);

    // **One item off the held stack**, which is `InventoryPlayer.decrStackSize`
    // and the half of a drop that is not the entity. Returns what came off, or
    // 0 for an empty hand.
    //
    // **Called after the entity exists, never before.** The pool that holds
    // dropped items can be full, and an item taken off the hand for an entity
    // that was refused is an item destroyed; the caller spawns first and spends
    // the stack only once that succeeded.
    //
    // Here rather than at the call site because the inventory is this screen's
    // and the hotbar it redraws is too: a caller reaching through `inventory()`
    // could not mark either dirty, and the slot would empty without the bottom
    // screen noticing.
    item::ItemId dropHeldItem();

    // **The whole of the stack in hand, thrown.** X with a stack picked up is
    // a1.1.2's own "click outside the window": `GuiContainer.mouseClicked`
    // passes slot -999 and `PlayerController.windowClick` spills the entire
    // cursor stack into the world. A stack picked up here is the cursor, so X
    // spills it -- and X keeps toggling the focus whenever nothing is in hand,
    // because a press that means two things is still one press at a time.
    //
    // **Split in two for the reason `dropHeldItem` is split**: the pool that
    // holds dropped items can refuse a spawn, and a stack spent for an entity
    // that never appeared is a stack destroyed. `throwRequest` says what the
    // player asked to throw and leaves it exactly where it is; the caller
    // spawns, and then calls `finishThrow` on success or `cancelThrow` on
    // failure. Either one clears the request, so a frame that answers neither
    // cannot leave one standing.
    const item::ItemStack* throwRequest() const;
    void finishThrow();
    void cancelThrow();

    // **What the held stack turns into**, which is one item in a1.1.2 and it is
    // the bucket: emptied it becomes full, poured it becomes empty. `id` is
    // `item::ItemUse::becomes`, so passing back what was already there is a
    // no-op rather than a case the caller has to filter.
    //
    // The count and the damage are left alone deliberately, and that is right
    // because a bucket stacks to one: `ac.a` sets `itemstack.id` on the stack
    // it was handed and never splits it, so a filled bucket goes back into the
    // slot the empty one came out of in Survival too. Here for the same reason
    // `dropHeldItem` is: it writes the inventory, so it is what marks the
    // bottom screen dirty.
    void replaceHeldItem(item::ItemId id);

    // `dx.b(dm)` from the player's side -- **walking over what is lying about**.
    // Returns how many entities were taken whole, so the caller can make the
    // noise; this has no sound engine and no listener to attenuate against.
    //
    // Here for the same reason `dropHeldItem` is: it writes to the inventory,
    // so it is what marks the screen dirty.
    int collectItems(mc::entity::ItemEntitySystem& items, const AABB& playerBox);

    // `kg.b(dm)`, the same walk for arrows: the player's own, stuck and still,
    // one at a time into the inventory. See `ArrowSystem::collect`.
    int collectArrows(mc::entity::ArrowSystem& arrows, const AABB& playerBox);

    // True once after anything in the inventory changed, and false until it
    // changes again. The caller writes it back to the world on a true.
    bool takeInventoryChange();

    // **The inventory, for a rule that is not this screen's.** Survival writes
    // the forty slots from outside the bottom screen: a death empties them, a
    // hit wears the armour, a break wears the tool, food and placement spend
    // the stack. Each of those is core code that takes an `item::Inventory&`,
    // so this hands one over -- and **the caller must follow any write with
    // `inventoryEdited`**, which is what redraws the band and the open page and
    // queues the save, exactly as `dropHeldItem` does for its own write.
    item::Inventory& editInventory() { return inventory_; }
    void inventoryEdited() { inventoryWritten(); }

    // **The game-over screen** -- `au`, GuiGameOver. a1.1.2 opens it the moment
    // health reaches zero and draws "Game over!", the score, and two buttons:
    // Respawn, and Title menu. **It does not pause the game** (`au.b()` is
    // false), so the world keeps ticking under it, which is why it is a state of
    // this screen and not a trip through the pause menu.
    //
    // While it is up it owns the bottom screen's input: the d-pad moves between
    // the two buttons, A presses one, and a touch presses whichever it lands
    // on. `takeDeathChoice` hands the press to the caller once.
    enum class DeathChoice : u8 { None, Respawn, TitleMenu };
    void setDead(bool dead, int score);
    bool dead() const { return dead_; }
    DeathChoice takeDeathChoice();

    // **The container screens** -- `hx` the workbench, `id` the furnace, `ea`
    // a chest -- opened by `tick::blockActivated` through the world's container
    // sink, and **the Survival inventory's own 2 x 2 grid** on the Inv. page.
    // All four are one `item::ContainerSession` with a cursor stack, clicked by
    // a1.1.2's `ee.a(III)V` rules: A (or a touch) is the left button, Y (or a
    // touch with Y held) the right one, X throws what is on the cursor. See
    // core/item/container_session.hpp.
    //
    // **A world container takes the whole screen and the buttons**: the tabs
    // become one Close tab, the screen is focused for as long as it is up, and
    // the caller stops walking -- an open `GuiScreen` is what stops a1.1.2's
    // player reading the movement keys. B or Close shuts it.
    //
    // **Closing drops the cursor and a crafting grid**, as `ar.a(Ldm;)V` and
    // its overrides do, and the Overlay cannot spawn anything: the stacks wait
    // in `closedStack` for the caller, one at a time, like a throw. Creative's
    // Inv. page keeps its swap-only edit and never opens a session.
    void openContainer(tick::TickWorld& world, tick::TickWorld::ContainerKind kind, i32 x,
                       int y, i32 z);
    // **A chest minecart**, which is the same screen on an entity rather than
    // on a cell -- so it cannot come through `ContainerKind`, which names a
    // block. See `item::ContainerSession::openMinecartChest`.
    void openMinecartChest(tick::TickWorld& world, entity::MinecartSystem& carts, u32 cartId);
    bool containerOpen() const
    {
        return session_.isOpen() && session_.kind() != item::ScreenKind::Inventory;
    }
    // Once a frame with a world: re-reads a furnace or a chest, closes a screen
    // whose block has gone, and redraws what a tick changed.
    void tickContainer(tick::TickWorld* world);
    // The next stack a close left for the ground, or null. `finishClosedStack`
    // with whether the caller spawned it; one that was refused goes back into
    // the inventory where it fits, rather than nowhere.
    const item::ItemStack* closedStack() const
    {
        return closedCount_ > 0 ? &closed_[closedCount_ - 1] : nullptr;
    }
    void finishClosedStack(bool spawned);
    // On the way out of a world: shuts any screen and puts what it would have
    // dropped back in the inventory, since there is no ground left to drop it
    // on.
    void closeContainerIntoInventory();
    // Whether Y is the screen's right click this frame, so the caller does not
    // also crouch on it.
    bool containerTakesY() const { return focus_ && session_.isOpen(); }

    // Whether this gamemode has a hotbar at all. Spectator does not: it has no
    // body, no reach and nothing to hold.
    bool hasHotbar() const { return gamemode_ != settings::Gamemode::Spectator; }

    // **Whether the bottom screen has the buttons.** X toggles it; while it is
    // on, the d-pad drives the cursor and the shoulders change tab, so the
    // caller has to suspend break and place for as long as it is true. One
    // press does one thing, and this is the flag that guarantees it.
    bool uiFocused() const { return focus_; }

    // **Whether the d-pad is walking or turning the player** rather than being
    // free for this screen to use. The Controls row's two Old schemes take it;
    // the New 3DS one leaves it alone. See core/settings/control_scheme.hpp.
    //
    // It changes one thing in here -- the map's zoom wants the focus once the
    // d-pad is spoken for, rather than answering a press that was a step
    // forward -- and one thing in the caller, which is `dpadTakenByScreen`.
    void setDpadIsGameplay(bool gameplay) { dpadIsGameplay_ = gameplay; }

    // **True while the bottom screen is the d-pad's owner**, which the caller
    // reads to stop the same press also moving the player. Three cases: a
    // focused player page, whose cursor the d-pad walks; any of the debug pages
    // behind SELECT, whose rows it drives and which have no focus of their own
    // to check; and the game-over screen, which chooses between Respawn and
    // Title with it. It is the rule the circle pad already follows -- a screen
    // that is open takes the input, which is a1.1.2's -- applied to one device.
    //
    // The body is already still on the last two; what this stops is the camera
    // turning under a press that was meant for a menu.
    bool dpadTakenByScreen() const { return focus_ || dead_ || page_ != Page::Player; }

    // **Whether the circle pad is scrolling the map rather than walking.** True
    // while the screen is focused *and* the map is what is in front of the
    // player, which is the one place the stick has a window to move. The caller
    // zeroes the body's heading while it holds -- panning and walking at once
    // would be two things fighting over the same window.
    //
    // **A world container is drawn over the page**, so a chest opened while the
    // map tab happened to be up is not a map to pan: what the stick should be
    // moving then is the chest's cursor, which is what `uiCursorActive` says.
    bool mapPanActive() const
    {
        return focus_ && playerPage_ == PlayerPage::Map && !containerOpen();
    }

    // **Whether the circle pad is walking a cursor rather than the player.**
    // Every focused screen except a map being panned has a cursor on it, and
    // while one does the stick drives it exactly as the d-pad does -- a grid is
    // a grid, and having to let go of the stick and find the d-pad to cross one
    // is a seam the player has no reason to feel. The caller zeroes the body's
    // heading for this too: a screen is open, so nothing reaches the player.
    bool uiCursorActive() const { return focus_ && !mapPanActive(); }

    // Once a frame, after handleInput. Reads the circle pad and either scrolls
    // the map (`mapPanActive`) or steps the focused screen's cursor
    // (`uiCursorActive`); does nothing at all when the screen is not focused.
    // `dt` is seconds, because both are gestures and belong on the frame clock
    // -- unlike the body, whose every constant is per tick.
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
    void tickMap(render::WorldStreamer& world, const Camera& camera);

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

        // **The frame minus everything above it, so the page reconciles by
        // construction.** The five measured buckets do not span the game loop:
        // `beforeWalk` is a long way down from the top of it, and `afterStream`
        // is not the bottom. Everything outside them -- the input scan, the
        // pause and death branches, block breaking, item use, the net pump,
        // `Overlay::tickMap`, this very function's console printing and
        // `gfxFlushBuffers` -- used to be invisible, so a 25 ms frame could sit
        // over a 15 ms `CPU busy` with nothing on the screen to say where the
        // other ten went. This is where they go. It is a residual rather than a
        // measurement, which is the point: nothing can fall out of it.
        float other = 0.0f;
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

    // Turns the focus off and puts everything it owned back: the palette
    // cursor and the map's pan, and a full redraw, because the focused pages
    // draw a cursor that can only be taken away by drawing them again.
    void releaseFocus();

    // The focused d-pad, A and B, and the shoulder pair. Returns true when
    // something it changed has to be redrawn.
    bool handleFocusedInput(u32 down);

    // `tickFocus`'s other half: the circle pad stepping a focused cursor, with
    // the wait-then-repeat a held direction needs.
    void tickCursorStick(float dt);

    // Where the palette cursor is, as a palette index rather than a cell.
    int paletteIndex() const { return palettePage_ * hud::kPalettePerPage + paletteCursor_; }

    // **Whether the page up has cells for the focused d-pad to walk over.** Two
    // do now -- the palette's 45 and the backpack's 27 -- so every place that
    // used to test for the Blocks page asks this instead. A page added later
    // that forgets to answer here gets a cursor that cannot be moved, which is
    // visible; the alternative was four copies of the same comparison drifting
    // apart, which is not.
    bool pageHasGrid() const
    {
        return playerPage_ == PlayerPage::Blocks || playerPage_ == PlayerPage::Items;
    }
    void showItemInPalette(item::ItemId id);

    // Picking a stack up and putting it down, which is the only edit the
    // inventory takes. `slot` is an inventory slot number, so the hotbar band
    // and the backpack grid go through the same two lines.
    //
    // **One click picks up and the next puts down**, rather than a drag. A
    // resistive screen sampled once a frame reports a drag as a sequence of
    // jumps, and the d-pad has no drag at all -- so a gesture that works with
    // one press is the only one both input routes can make.
    void touchSlot(int slot);
    void cancelHeld();

    // **Every write to the forty slots goes through here**, which is what keeps
    // the page above the band in step with the band. Marking only the hotbar
    // was right while the hotbar was the only thing drawing stacks; the Items
    // page draws twenty-seven more of them and the palette's caption names
    // what is in the hand, so an item picked up off the floor used to appear in
    // the band and not in the open backpack above it until something else
    // forced a redraw.
    void inventoryWritten();

    // The same, for a change of *which* slot is in hand rather than of what is
    // in it. Not a save -- `selected` is not written to the file -- so this is
    // the redraw half of `inventoryWritten` on its own.
    void selectionMoved();

    // **Puts the focused cursor on the slot that is now in hand.** ZL and ZR
    // change the hand from anywhere, and on a page with a grid the cursor is
    // usually down in that grid -- so without this the two marks on the screen
    // end up on different slots and neither one is obviously the live one. It
    // knows the two cursor spaces this screen has: a container session numbers
    // the hand as its last nine slots, and every other page has the band as the
    // place the cursor is when it is not on the grid.
    //
    // Does nothing on the map page, which has no cursor to move.
    void cursorToHand();

    // `cursorToHand` the other way round: on a container screen, a cursor
    // sitting in the nine hand slots moves the selection to that slot, so the
    // amber mark and the white one never name two different cells of the band.
    // Does nothing off the band, and nothing without a session.
    void selectUnderContainerCursor();

    // **Is a cursor drawn at all?** Focused, always. Unfocused, only while a
    // stack is in hand -- see the definition.
    bool cursorShown() const;

    // **Where the carried stack is drawn floating**, as a cell on the Items
    // page and as an index into the hotbar band; at most one of the two is set,
    // and both are -1 when nothing is in hand.
    //
    // It follows the cursor, because the cursor is the slot the next press acts
    // on -- a stack in hand is drawn on the slot it is about to go into. A tap
    // puts the cursor where it landed, so this is as true of the stylus as of
    // the d-pad; on a page with no cursor at all the stack hovers over the slot
    // it came out of instead, so the player can still see what they are
    // carrying.
    void carriedPosition(int* itemsCell, int* hotbarSlot) const;

    // Fills the selected hand slot from the palette cursor's cell. A copy
    // rather than a move -- the palette holds nothing to take away.
    void takeFromPalette();

    // The container session's half. `closeSession` shuts whatever is open and
    // queues its drops; `closeContainer` is the player's B, which also gives
    // the focus back and reopens the inventory grid if that page is up;
    // `syncInventorySession` opens or shuts the Survival Inv. page's grid to
    // match the page and the mode.
    void closeSession();
    void closeContainer();
    void syncInventorySession();
    bool handleContainerInput(u32 down);
    void clickContainer(int index, int button);
    // X on a focused screen: `item::ContainerSession::quickMove`.
    void quickMoveContainer(int index);
    // Moves the chest window by `rows` and rebuilds the layout. False when it
    // was already at that end, or when the screen does not scroll.
    bool scrollContainer(int rows);
    bool scrollAtChestEdge(int dy);
    // L and R's tab change, shared by the palette's focus and the inventory's.
    void stepTab(bool forward);

    const NdspBackend* audio_ = nullptr;

public:
    // **What the mobs are doing**, which is otherwise invisible on a console.
    // Set once a tick by the frame loop; every field is a count and none of it
    // is drawn outside the Info page.
    //
    // `taken` and `hits` are the player-damage seam's running total, kept now
    // that hearts say what health is left: a session total answers a different
    // question from a bar -- whether anything is reaching you at all, and how
    // often -- and a mob hitting a Creative player subtracts nothing from
    // anywhere else. See `PlayerHarm` in platform/ctr/main.cpp.
    struct MobStats {
        int animals = 0;
        int monsters = 0;
        unsigned searches = 0;
        unsigned exhausted = 0;
        // The most searches any one tick has run. The per-tick cap of one was
        // removed because it was dropping nine path requests in ten once the
        // monsters arrived; this is what says what that costs. See
        // `MobSystem::peakSearchesPerTick`.
        int peakSearches = 0;
        int taken = 0;
        int hits = 0;

        // **What the monster spawner has been doing since the world opened**,
        // which is the one question a console cannot otherwise answer. "No
        // monsters" and "monsters, ninety blocks under you" look the same from
        // the surface and are completely different here -- see
        // core/entity/mob_spawn.hpp's SpawnCounters.
        int spawned = 0;
        unsigned chunksTried = 0;
        unsigned floors = 0;

        // **And the *block* spawners**, which answer a different question
        // entirely: how many dungeon cages are resident, and whether any of
        // them has had a player near enough to run. A `cage` that is non-zero
        // with `fire` at zero is a player who has not stood close enough; both
        // at zero is a world with no dungeons in range. See
        // core/entity/mob_spawner.hpp.
        int cages = 0;
        int cagesFired = 0;
        int cagesSpawned = 0;
    };
    void setMobStats(const MobStats& stats) { mobStats_ = stats; }

private:
    MobStats mobStats_{};

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
    static constexpr int kSettingCount = 5;

    settings::Gamemode gamemode_ = settings::Gamemode::Spectator;
    MapScreen map_;

    // The forty slots, and where the palette, the backpack grid and the focus
    // are looking.
    item::Inventory inventory_;
    int palettePage_ = 0;
    int paletteCursor_ = 0;
    int itemsCursor_ = 0;

    // The slot a stack has been lifted out of, or -1. It is a slot number and
    // not a copy of the stack, so nothing can be duplicated or lost by a page
    // change: the stack never leaves the array, and putting it down is a swap.
    int heldSlot_ = -1;

    // The slot X asked to throw, or -1. A slot number for the same reason
    // `heldSlot_` is one, and it outlives the button press by exactly as long
    // as it takes the caller to spawn the entity -- see `throwRequest`.
    int throwSlot_ = -1;

    // Set by every edit, cleared by takeInventoryChange.
    bool inventoryChanged_ = false;

    // The open container screen, where its slots are, the slot the next press
    // acts on, and the world its furnace or chest is in -- borrowed from the
    // frame loop, which hands it over again every tickContainer.
    item::ContainerSession session_;
    gui::ContainerLayout layout_;
    int containerCursor_ = 0;
    // **The top chest row on the screen**, for a chest with more rows than the
    // band can hold -- three chests joined together or more, which is a thing
    // a1.1.2 lets you build through a puddle. 0 for every other screen, and
    // clamped by the layout rather than here.
    int containerScroll_ = 0;
    tick::TickWorld* containerWorld_ = nullptr;
    // X asked to throw the cursor stack; answered like `throwSlot_`.
    bool throwCursor_ = false;
    // The furnace's arrow and flame, as last drawn.
    bool progressDirty_ = false;
    int shownCook_ = -1;
    int shownBurn_ = -1;
    // What closes left for the ground. Two closes' worth -- a cursor and a
    // 3 x 3 grid each -- since a world container and the inventory grid can
    // both shut in one frame.
    static constexpr int kMaxClosedStacks = 20;
    item::ItemStack closed_[kMaxClosedStacks];
    int closedCount_ = 0;

    // **Focus is two booleans and not a mode enum**, because there are exactly
    // three states and the third is not reachable: the screen is unfocused, or
    // it is focused on the hotbar row, or it is focused on the grid above it --
    // and the last is only possible on a page that has a grid, which is
    // enforced where the page changes rather than represented here.
    //
    // `focusGrid_` used to be `focusPalette_`, when the Blocks page was the only
    // one with cells to walk over. The Items page has twenty-seven of them now
    // and the cursor behaves identically on both, so the flag says "the grid"
    // and the page says which grid.
    bool focus_ = false;

    // Set by the caller from the Controls row; false is the New 3DS scheme,
    // where nothing in gameplay wants the d-pad. See `setDpadIsGameplay`.
    bool dpadIsGameplay_ = false;
    bool focusGrid_ = false;

    // **The circle pad, read as a d-pad while a cursor is focused.** A repeat
    // is a thing with a memory -- the first step lands the moment the stick
    // moves and the ones after it are slow enough to stop on -- so the state
    // lives here and the rule lives in core/gui/stick_cursor.hpp.
    gui::StickRepeat stick_;

    // The game-over screen: whether it is up, the score it shows, which of its
    // two buttons the d-pad is on, and a press not yet collected.
    bool dead_ = false;
    int deathScore_ = 0;
    int deathCursor_ = 0;
    DeathChoice deathChoice_ = DeathChoice::None;

    // The two sheets' pixels, borrowed. Null until a pack has been handed over;
    // the items one stays null for a pack that has no gui/items.png, which
    // core/gui/item_icon.hpp falls back from rather than fails on.
    gui::IconSheets sheets_;

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

    // **The largest single frame in the block, beside the mean of it.** The top
    // screen is 59.83 Hz, so a frame costs 16.71 ms or 33.4 ms and nothing in
    // between; a mean of 25 is not a frame anybody had, it is half of each. And
    // because each bucket's spike lands in a different frame from the others,
    // no mean of a part can ever add up to a mean of the whole. The peaks are
    // what say which part actually spiked.
    Accum peak_;
    Accum shownPeak_;

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
