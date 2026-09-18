#include "platform/ctr/overlay.hpp"
#include "platform/ctr/bottom_screen.hpp"

#include "core/block/registry.hpp"
#include "core/item/creative_palette.hpp"
#include "core/item/registry.hpp"
#include "core/util/console_text.hpp"
#include "core/util/coord_text.hpp"
#include "core/net/session.hpp"
#include "platform/ctr/heap.hpp"
#include "platform/ctr/local_link.hpp"

#include <3ds.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mc::ctr {

namespace {

// devkitARM's newlib links a printf with no float support unless -u _printf_float
// is forced, so a "%f" here prints nothing at all and costs a trip to hardware
// to discover. Everything is formatted as integers on purpose.
int tenths(float value)
{
    return int(value * 10.0f + 0.5f);
}

constexpr int kSamplesPerUpdate = 20;

// swkbd's filter hook. Runs on the applet's side each time the player presses
// OK, and gets to say "no, and here is why" without closing the keyboard.
//
// `ppMessage` is shown as-is and not freed, so every message written to it has
// to outlive the call. parseCoordinateTriple only ever returns string
// literals, which is the reason it returns `const char*` instead of filling a
// buffer.
SwkbdCallbackResult validateCoordinates(void* user, const char** ppMessage, const char* text,
                                        size_t textlen)
{
    (void)user;

    // The applet hands over a length, so do not assume a terminator. 64 is
    // comfortably above the 47 characters the keyboard is configured to accept;
    // anything longer means the assumption broke rather than that the player
    // typed a lot.
    char buffer[64];
    if (textlen >= sizeof(buffer)) {
        *ppMessage = "too long";
        return SWKBD_CALLBACK_CONTINUE;
    }
    std::memcpy(buffer, text, textlen);
    buffer[textlen] = '\0';

    mc::CoordTriple parsed;
    const char* error = mc::parseCoordinateTriple(buffer, &parsed);
    if (error != nullptr) {
        *ppMessage = error;
        return SWKBD_CALLBACK_CONTINUE;
    }
    return SWKBD_CALLBACK_OK;
}

// libctru's bottom-screen console, from `console.c`: 40 columns, 30 rows,
// cursor addressed from 1.
constexpr int kConsoleWidth = 40;

// Row the pages start on. Rows 1 and 2 are the header, which only a page change
// reprints. The footer is pinned at 28, so a page's body is rows 4..27 -- 24
// rows, and the settings page spends every one of them.
constexpr int kBodyRow = 4;
constexpr int kFooterRow = 28;

void clearScreen()
{
    std::printf("\x1b[2J\x1b[1;1H");
}

// **One row of a page, placed absolutely, clipped, and with no newline in it.**
//
// The overlay used to end every line with `\n` and trust that to mean "next
// row". It does not. libctru wraps at 40 columns, and six of the Info page's
// lines were 41 to 44 characters long before any number grew a digit -- so each
// of them quietly took two rows. The body then ran past row 30, `newRow()`
// scrolled the whole window, and what the player saw was the frame-time line
// several times over, each one sample block older than the one below it. The
// page's own history, marching up the screen, and worse the busier the console
// got because that is when the numbers are widest.
//
// Nothing here can do that. The row is addressed, the text is clipped to the
// console's width, and no newline is ever written -- so the cursor cannot
// advance a row on its own and `newRow()` cannot be reached. A line that is too
// long now loses its tail where you can see it, and a page that asks for a row
// outside its body simply does not get one.
void row(int line, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// A row with nothing on it. Its own function because `row(r, "")` is a
// zero-length printf format, which GCC warns about and is right to.
void blank(int line)
{
    if (line < 1 || line >= kFooterRow + 2) {
        return;
    }
    std::printf("\x1b[%d;1H\x1b[2K", line);
}

void row(int line, const char* fmt, ...)
{
    if (line < 1 || line >= kFooterRow + 2) {
        return;
    }

    char text[160];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    char clipped[sizeof(text) + 8];
    usize written = clipToColumns(text, clipped, sizeof(clipped), kConsoleWidth);
    // Always reset, so a clip that fell inside a colour sequence cannot leak it
    // into the rest of the page.
    std::memcpy(clipped + written, "\x1b[0m", 4);
    clipped[written + 4] = '\0';

    std::printf("\x1b[%d;1H\x1b[2K%s", line, clipped);
}

}  // namespace

void Overlay::begin(const char* worldName, const char* model)
{
    worldName_ = worldName;
    model_ = model;
    dirty_ = true;
    // A different world: every chunk the map remembers belongs to the last one,
    // and so does whatever page was up and whichever way it was facing.
    map_.reset();
    playerPage_ = PlayerPage::Map;
    lookYawStep_ = -1;
    uiTouchActive_ = false;

    // **The death screen belongs to the world that put it up.** `au` is a
    // `GuiScreen` and closing the world closes it, but this overlay is one
    // process-long object -- so dying, choosing *Title menu* and opening
    // another world used to arrive with `dead_` still set: the game-over screen
    // over a living player, and no way out of it, because the frame loop only
    // raises it when `!vitals.alive() && !dead()` and only the screen itself
    // clears it. Cleared here rather than by the caller, because every way out
    // of a world comes back through `begin()` and only some of them come back
    // through the death screen. The fields directly and not `setDead(false)`:
    // that one closes a container session and re-syncs the inventory, and both
    // of those are about to be done below in the order this needs them.
    dead_ = false;
    deathScore_ = 0;
    deathCursor_ = 0;
    deathChoice_ = DeathChoice::None;

    // Emptied, not filled. **The world's own stacks arrive next**, through
    // `setInventory`, and only a world with none of them falls back to the
    // palette's opening nine -- see there. Filling here as well would put nine
    // blocks in the hand of a player whose save says otherwise, for the one
    // frame between the two calls.
    inventory_.clear();
    inventoryChanged_ = false;
    // A screen or a queued drop from the last world belongs to it and not to
    // this one; closeContainerIntoInventory already put anything owed back.
    {
        item::ItemStack discard[10];
        session_.close(discard, 10);
    }
    closedCount_ = 0;
    throwCursor_ = false;
    containerWorld_ = nullptr;
    heldSlot_ = -1;
    // A throw the last world asked for and this one has not: the caller answers
    // the request on the frame after the press, and a world change can fall
    // between the two.
    throwSlot_ = -1;
    palettePage_ = 0;
    paletteCursor_ = 0;
    itemsCursor_ = 0;
    focus_ = false;
    focusGrid_ = false;
    hotbarDirty_ = true;
    bodyDirty_ = true;
    // map_.reset() above already cleared the pan; this is the pair of it for a
    // reader looking for where the focus state is put back.
}

void Overlay::setGamemode(settings::Gamemode mode)
{
    // **The bottom screen changes shape here and nowhere else.** Without a
    // hotbar the band is not reserved, so the banner and every page move forty
    // pixels up into it -- see hud.hpp.
    //
    // **Before the early return, and from `mode` rather than from the field**,
    // which is the one ordering that is not an accident: `gamemode_` starts as
    // Spectator, so a Spectator world sets the mode it already has and takes
    // that return -- and would leave the screen laid out for a hotbar it does
    // not have. It is also set before anything below asks a layout question,
    // because `syncInventorySession` lays a container out.
    hud::setHotbarPresent(mode != settings::Gamemode::Spectator);

    if (mode == gamemode_) {
        return;
    }
    gamemode_ = mode;

    // **A page the new mode does not have has to be left before the strip is
    // next drawn**, or the selected index would name a tab that is not there.
    // Asked of the mapping rather than tested mode by mode, so a mode added
    // later cannot be forgotten here.
    PlayerPage pages[hud::kMaxTabs];
    const int count = playerPagesFor(pages);
    bool present = false;
    for (int i = 0; i < count; ++i) {
        present = present || pages[i] == playerPage_;
    }
    if (!present) {
        playerPage_ = PlayerPage::Map;
    }

    // Spectator has no hands to hold a chest's contents in. The Survival grid
    // follows the mode: Creative's Inv. page is the swap-only one.
    if (!hasHotbar() && containerOpen()) {
        closeContainer();
    }
    syncInventorySession();

    // The focus has nothing to sit on without a hotbar.
    if (!hasHotbar() && focus_) {
        releaseFocus();
    }

    // The strip has a different number of tabs on it now, and so has every
    // page -- the whole screen moved. `dirty_` is a clear and a full redraw,
    // which is what re-lays the map's furniture at the new window size; the
    // sampled chunks themselves are a picture of the ground at one pixel a
    // block and do not care how big the window showing them is.
    dirty_ = true;
}

void Overlay::setAnimatedItemsTile(int tile, const u8* texels)
{
    sheets_.animatedItems = texels;
    sheets_.animatedItemsTile = texels != nullptr ? tile : -1;
    if (texels == nullptr || tile < 0) {
        return;
    }

    // Does anything on screen actually draw this tile? An item's icon is a
    // property of the item, so this is a comparison against `def(id).icon` and
    // not against the id -- which is the same rule the blit uses, and has to
    // be, or a slot would redraw for a tile it does not show.
    auto shows = [tile](item::ItemId id) {
        if (id <= 0) {
            return false;
        }
        const item::ItemDef& def = item::def(id);
        return def.known && def.sheet == item::IconSheet::Items && int(def.icon) == tile;
    };

    for (int slot = 0; slot < item::kHotbarSlots; ++slot) {
        if (shows(inventory_.main[slot].id)) {
            hotbarDirty_ = true;
            break;
        }
    }

    if (page_ != Page::Player) {
        return;
    }
    if (session_.isOpen()) {
        bool visible = shows(item::ItemId(session_.cursor().id));
        for (int i = 0; i < session_.slotCount() && !visible; ++i) {
            visible = shows(item::ItemId(session_.slotAt(inventory_, i).id));
        }
        if (visible) {
            bodyDirty_ = true;
            hotbarDirty_ = true;
        }
        return;
    }
    if (playerPage_ == PlayerPage::Items) {
        for (int slot = item::kHotbarSlots; slot < item::kMainSlots; ++slot) {
            if (shows(inventory_.main[slot].id)) {
                bodyDirty_ = true;
                return;
            }
        }
        for (int slot = 0; slot < item::kArmourSlots; ++slot) {
            if (shows(inventory_.armour[slot].id)) {
                bodyDirty_ = true;
                return;
            }
        }
    } else if (playerPage_ == PlayerPage::Blocks) {
        const int base = palettePage_ * hud::kPalettePerPage;
        for (int i = 0; i < hud::kPalettePerPage; ++i) {
            if (shows(item::paletteItem(base + i))) {
                bodyDirty_ = true;
                return;
            }
        }
    }
}

void Overlay::setAtlas(const texture::AtlasImage& atlas)
{
    map_.setPalette(atlas);
    // Borrowed. `empty()` is the atlas's own "not built yet", and null is what
    // the icon blit treats as "draw nothing" rather than "draw tile zero".
    sheets_.terrain = atlas.empty() ? nullptr : atlas.rgba.data();
    // **Optional, and its absence is not a failure.** A pack with no
    // gui/items.png leaves this null and every icon that wanted it falls back
    // to the terrain tile of the block the item places, which is what this
    // screen drew before there were two sheets.
    sheets_.items = atlas.hasItems() ? atlas.itemsRgba.data() : nullptr;
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

void Overlay::setBackdropTile(const std::vector<u8>& rgba)
{
    haveBackdrop_ = rgba.size() == texture::kBackgroundBytes
                    && texture::kBackgroundEdge == hud::kTileEdge;
    if (!haveBackdrop_) {
        return;
    }
    for (usize i = 0; i < usize(hud::kTileEdge) * hud::kTileEdge; ++i) {
        backdrop_[i] = gui::rgb565(int(rgba[i * 4 + 0]), int(rgba[i * 4 + 1]), int(rgba[i * 4 + 2]));
    }
    dirty_ = true;
}

// **Which pages a mode has, in one place.** The switch has no default: a mode
// added later does not compile until somebody has decided which tabs it gets.
//
// Creative fills all four of hud::kMaxTabs, which is the ceiling and is
// asserted rather than assumed -- a fifth page needs the strip widened before
// it needs anything here.
int Overlay::playerPagesFor(PlayerPage* out) const
{
    // Creative fills the strip exactly. A fifth page needs the strip widened
    // before it needs anything in this function.
    static_assert(hud::kMaxTabs >= 4, "Creative needs four tabs");
    // **The inventory comes first, and the map after it.** The strip used to
    // open with Map because the map was the first page this screen had; what a
    // player reaches for is the inventory, and in Creative the palette beside
    // it. So the order is what a mode carries, then where it is: Inv., Items,
    // Map, Look. Spectator carries nothing and starts at Map, which is still
    // its first tab because it has no others to come before it.
    int count = 0;
    switch (gamemode_) {
    case settings::Gamemode::Spectator:
        break;  // carries nothing and places nothing, so neither page applies
    case settings::Gamemode::Survival:
        out[count++] = PlayerPage::Items;
        break;
    case settings::Gamemode::Creative:
        out[count++] = PlayerPage::Items;
        out[count++] = PlayerPage::Blocks;
        break;
    }
    out[count++] = PlayerPage::Map;
    out[count++] = PlayerPage::Look;
    return count;
}

hud::TabStrip Overlay::tabs() const
{
    // A world container is a screen of its own, not a page among the others,
    // so the strip is its way out.
    if (containerOpen()) {
        hud::TabStrip strip;
        strip.count = 1;
        strip.labels[0] = "Close";
        strip.selected = 0;
        return strip;
    }

    PlayerPage pages[hud::kMaxTabs];
    const int count = playerPagesFor(pages);

    hud::TabStrip strip;
    for (int i = 0; i < count; ++i) {
        switch (pages[i]) {
        case PlayerPage::Map:
            strip.labels[strip.count++] = "Map";
            break;
        // **"Inv." and "Items", not "Items" and "Blocks".** The palette
        // stopped being blocks-only when it grew to the whole item table -- it
        // offers swords, ingots, armour and now the two music discs -- so
        // "Blocks" was naming a third of what is on the page. It takes "Items",
        // and the inventory takes the word it always was: a1.1.2's own screen
        // is `GuiInventory`, abbreviated here because the tab is narrow. The
        // enum names are unchanged; these are labels.
        case PlayerPage::Items:
            strip.labels[strip.count++] = "Inv.";
            break;
        case PlayerPage::Blocks:
            strip.labels[strip.count++] = "Items";
            break;
        case PlayerPage::Look:
            strip.labels[strip.count++] = "Look";
            break;
        }
    }
    strip.selected = selectedTab();
    return strip;
}

int Overlay::selectedTab() const
{
    PlayerPage pages[hud::kMaxTabs];
    const int count = playerPagesFor(pages);
    for (int i = 0; i < count; ++i) {
        if (pages[i] == playerPage_) {
            return i;
        }
    }
    return 0;
}

void Overlay::selectTab(int index)
{
    PlayerPage pages[hud::kMaxTabs];
    const int count = playerPagesFor(pages);
    if (index < 0 || index >= count) {
        return;
    }
    if (pages[index] == playerPage_) {
        return;
    }
    playerPage_ = pages[index];

    // **A grid cursor only exists on a page that has a grid.** Leaving it set
    // while the Map page is up would mean the d-pad moved something the player
    // cannot see.
    if (!pageHasGrid()) {
        focusGrid_ = false;
    }
    // **A lifted stack is put back down where it came from.** Nothing is lost
    // either way -- the stack never leaves the array, only its outline follows
    // the cursor -- but a slot drawn hollow on a page the player has left is a
    // state with nothing on screen to explain it.
    heldSlot_ = -1;
    // Leaving the Survival Inv. page is closing `lo`, which drops its grid.
    syncInventorySession();
    dirty_ = true;
}

int Overlay::touchLookTop() const
{
    // A press that began on the tab strip belongs to it until it is let go, so
    // a drag that wanders down onto the pad cannot turn the view with it.
    if (uiTouchActive_) {
        return -1;
    }
    // The debug pages are the maintainer's and have nothing to touch, so they
    // hand the whole screen over -- which is what the bottom screen did
    // everywhere before the player's half had anything on it.
    if (page_ != Page::Player) {
        return 0;
    }
    if (playerPage_ != PlayerPage::Look) {
        return -1;
    }
    return hud::lookPadTop();
}

namespace {

// A block's name as something to read: `mossy_cobblestone` is what the table
// carries -- the column is ours, and snake_case is what it was written in -- and
// "Mossy cobblestone" is what a caption wants. Formatted into the caller's
// buffer rather than returned, because nothing here allocates.
// An analogue axis as -1..1 with the deadzone taken out, which is the same
// arithmetic platform/ctr/main.cpp uses on the same stick -- 156 is full
// deflection and most consoles rest a little off centre.
float padAxis(s16 raw)
{
    constexpr float kDeadzone = 0.1f;
    const float value = float(raw) / 156.0f;
    if (value > -kDeadzone && value < kDeadzone) {
        return 0.0f;
    }
    return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
}

void itemCaption(item::ItemId id, char* out, usize size)
{
    if (size == 0) {
        return;
    }
    if (id == 0 || !item::def(id).known) {
        std::snprintf(out, size, "%s", "empty");
        return;
    }
    const char* name = item::def(id).name;
    usize written = 0;
    for (; name[written] != '\0' && written + 1 < size; ++written) {
        const char c = name[written];
        out[written] = c == '_' ? ' ' : c;
    }
    out[written] = '\0';
    if (written > 0 && out[0] >= 'a' && out[0] <= 'z') {
        out[0] = char(out[0] - 'a' + 'A');
    }
}

}  // namespace

void Overlay::tickMap(render::WorldStreamer& world, const Camera& camera)
{
    map_.update(world, camera);
}

bool Overlay::handleInput(u32 down, u32 held, DebugSettings* settings, Camera* camera)
{
    // **The game-over screen takes every press on the player's half.** Its two
    // buttons are the only things a dead player can do; the tabs, the hotbar,
    // the focus and the map all wait until Respawn. The debug pages behind
    // SELECT are not the player's and keep their chord.
    if (dead_ && page_ == Page::Player && (held & KEY_SELECT) == 0) {
        uiTouchActive_ = (held & KEY_TOUCH) != 0;
        if ((down & KEY_TOUCH) != 0) {
            touchPosition touch;
            hidTouchRead(&touch);
            const int button = hud::gameOverButtonAt(int(touch.px), int(touch.py));
            if (button >= 0) {
                deathCursor_ = button;
                deathChoice_ = button == 0 ? DeathChoice::Respawn : DeathChoice::TitleMenu;
                bodyDirty_ = true;
            }
        }
        if ((down & (KEY_DUP | KEY_DDOWN)) != 0) {
            deathCursor_ = 1 - deathCursor_;
            bodyDirty_ = true;
        }
        if ((down & KEY_A) != 0) {
            deathChoice_ = deathCursor_ == 0 ? DeathChoice::Respawn : DeathChoice::TitleMenu;
        }
        return false;
    }

    // **The touch screen, before the buttons**, because a tap on a tab has to
    // be claimed in the same frame it lands: the caller asks `touchLookTop()`
    // straight afterwards to decide whether the camera gets the drag.
    if ((held & KEY_TOUCH) == 0) {
        uiTouchActive_ = false;
    } else if ((down & KEY_TOUCH) != 0 && page_ == Page::Player) {
        touchPosition touch;
        hidTouchRead(&touch);
        const int x = int(touch.px);
        const int y = int(touch.py);
        const int tab = hud::tabAt(tabs(), x, y);
        const int slot = hasHotbar() ? hud::hotbarSlotAt(x, y) : -1;
        if (tab >= 0) {
            if (containerOpen()) {
                closeContainer();
            } else {
                selectTab(tab);
            }
            uiTouchActive_ = true;
        } else if (session_.isOpen()) {
            // **A container screen owns the band as well as the page**: the
            // hand is nine of its slots. A touch is the left button, or the
            // right one with Y held -- the stylus has one tip.
            //
            // The scroll arrows are asked first, and only a tall chest has
            // any: they sit in the gutter beside the grid, where no slot is.
            const int scroll = gui::containerScrollAt(layout_, x, y);
            const int index = scroll != 0 ? -1 : gui::containerSlotAt(layout_, x, y);
            if (scroll != 0) {
                scrollContainer(scroll);
            } else if (index >= 0) {
                const bool moved = index != containerCursor_;
                containerCursor_ = index;
                clickContainer(index, (held & KEY_Y) != 0 ? 1 : 0);
                // A tap in the band is a tap on a slot the player also holds,
                // so the hand goes with it -- the same rule the d-pad follows.
                selectUnderContainerCursor();
                if (moved) {
                    bodyDirty_ = true;
                    hotbarDirty_ = true;
                }
            }
            uiTouchActive_ = true;
        } else if (slot >= 0) {
            // **The hotbar answers a touch on every page**, which is the whole
            // reason it is a band rather than something on the inventory page:
            // changing what is in your hand should not cost a page change.
            //
            // ...unless a stack is being carried, in which case the band is
            // somewhere to put it down. One press does one thing, and which
            // thing it is is decided by whether the player's hands are full.
            //
            // **The cursor comes to the band either way.** The band's cursor is
            // `inventory_.selected` itself (see `drawPlayerPage`), so leaving
            // the grid is the whole of the move -- and it is what makes a stack
            // carried over from the backpack hover over the slot that was
            // tapped rather than over the cell it came out of.
            if (focusGrid_) {
                focusGrid_ = false;
                bodyDirty_ = true;
            }
            if (heldSlot_ >= 0) {
                touchSlot(slot);
            } else if (slot != inventory_.selected) {
                inventory_.selected = slot;
                hotbarDirty_ = true;
            }
            uiTouchActive_ = true;
        } else if (playerPage_ == PlayerPage::Items) {
            const int cell = hud::itemsCellAt(x, y);
            if (cell >= 0) {
                // The cursor goes where the tap did, so the stack it picks up
                // hovers over that cell and the mark under it says which.
                focusGrid_ = true;
                itemsCursor_ = cell;
                bodyDirty_ = true;
                hotbarDirty_ = true;
                touchSlot(hud::itemsSlotForCell(cell));
            }
            uiTouchActive_ = true;
        } else if (playerPage_ == PlayerPage::Blocks) {
            const int arrow = hud::paletteArrowAt(x, y);
            const int cell = hud::paletteCellAt(x, y);
            if (arrow != 0) {
                const int wanted = palettePage_ + arrow;
                if (wanted >= 0 && wanted < hud::palettePageCount()) {
                    palettePage_ = wanted;
                    bodyDirty_ = true;
                }
            } else if (cell >= 0) {
                // Touching a block puts it in the hand, in the slot that is
                // already selected. **It does not move the selection**: a
                // player filling a hotbar picks the slot and then the block,
                // and a pick that also moved the slot would fill one slot nine
                // times.
                focusGrid_ = true;
                paletteCursor_ = cell;
                takeFromPalette();
                bodyDirty_ = true;
            }
            uiTouchActive_ = true;
        } else if (playerPage_ != PlayerPage::Look) {
            // The map and the inventory keep their own presses. Marking the
            // touch as the UI's is what stops a drag on the map from turning
            // the camera, which is what it used to do.
            uiTouchActive_ = true;
        }
    }

    // **ZL and ZR change the held slot from anywhere**, page or no page, focus
    // or no focus -- they are the New 3DS's shoulder pair and this is what a
    // mouse wheel does in the original. They do not exist on an old 3DS, which
    // is the other half of why the focused d-pad below is not a convenience.
    //
    // Read before SELECT's page cycle, because nothing in that chord uses them
    // and a held SELECT should not take the hotbar away.
    if (hasHotbar() && (down & (KEY_ZL | KEY_ZR)) != 0) {
        inventory_.cycle((down & KEY_ZR) != 0 ? 1 : -1);
        selectionMoved();
        // **And the cursor goes with it.** On the inventory and the palette the
        // focused cursor is usually down in the grid, so a shoulder press used
        // to move a white outline in the band while the amber one stayed where
        // it was -- two marks disagreeing about which slot the next press acts
        // on, which is the one thing this screen must never be ambiguous about.
        // Changing the slot in your hand *is* pointing at it, so the cursor
        // follows the hand to the band it just moved in.
        //
        // Only while there is a cursor drawn to move -- which unfocused means
        // while a stack is in hand, since a tap can put the cursor down too.
        // With empty hands and the focus off there is no mark on the screen and
        // nothing for this to do.
        if (cursorShown()) {
            cursorToHand();
        }
    }

    // SELECT is the modifier rather than a page key of its own, so the page
    // cycle cannot be hit by accident while flying: X is sprint and Y is the
    // stereo tuner, and both of those read the same buttons.
    if (held & KEY_SELECT) {
        if (down & (KEY_Y | KEY_X)) {
            // +1 forward, -1 back, modulo however many pages there are. The
            // "back" step is kPageCount - 1 rather than -1 so the arithmetic
            // stays unsigned-safe when a page is added.
            const int step = (down & KEY_Y) != 0 ? 1 : kPageCount - 1;
            page_ = Page(((int(page_) + step) % kPageCount));
            cursor_ = 0;
            dirty_ = true;
        }
        return false;
    }

    // **The map page owns the d-pad; the other player pages still leave it
    // alone**, which is what lets a hotbar be built on the Items page later
    // without taking a binding back off anybody.
    //
    // Left and right cycle the grid overlay, up and down zoom in and out. Both
    // are the MapScreen's own state -- it owns the style, marks what it has to
    // and redraws itself -- so this returns false whatever happens: the caller
    // has no pool to rebuild and no setting to apply.
    //
    // **The stereo tuner is SELECT + d-pad**, and SELECT already returned above,
    // so nothing needs guarding here any more. It used to be Y + d-pad and this
    // test excluded it; Y is sneak now, and leaving the old guard in would have
    // made the map's zoom die whenever the player crouched.
    if (page_ == Page::Player) {
        // **X focuses the bottom screen, and only B lets it go.** The screen
        // is resistive and a player walking has no stylus out; focused, the
        // d-pad and A do what a tap would, and the world keeps moving
        // underneath -- the circle pad and the camera are untouched. Only in a
        // mode that has something to focus on.
        //
        // X used to let the focus go as well, which put the one button that
        // does something *inside* a focused screen one press away from
        // throwing the player out of it. Now it only turns the focus on.
        if (hasHotbar() && (down & KEY_X) != 0) {
            // **A stack in hand takes the press first**, and is thrown -- the
            // same rule the hotbar's touch handler uses, and the reason one
            // press still does one thing. B is still the way to put it back.
            if (session_.isOpen() && !session_.cursor().empty()) {
                throwCursor_ = true;
                return false;
            }
            // **On a container screen X is a shift-click**: the stack under
            // the cursor goes across to the other side. See
            // item::ContainerSession::quickMove, which is where the routing is
            // and where it is tested.
            if (focus_ && session_.isOpen()) {
                quickMoveContainer(containerCursor_);
                return false;
            }
            // A world container keeps the focus until it is closed.
            if (containerOpen()) {
                return false;
            }
            if (heldSlot_ >= 0) {
                throwSlot_ = heldSlot_;
                return false;
            }
            // **The Creative pages have no session** and get the same move
            // through the inventory: on the hotbar row or the Items grid the
            // stack crosses between the hand and the backpack (armour goes on
            // and comes off), and on the palette a full stack goes into the
            // hand. See item::Inventory::quickMove and giveStack.
            if (focus_ && pageHasGrid()) {
                bool moved = false;
                if (!focusGrid_) {
                    moved = inventory_.quickMove(inventory_.selected);
                } else if (playerPage_ == PlayerPage::Items) {
                    moved = inventory_.quickMove(hud::itemsSlotForCell(itemsCursor_));
                } else {
                    moved = inventory_.giveStack(item::paletteItem(paletteIndex()));
                }
                if (moved) {
                    inventoryWritten();
                }
                return false;
            }
            if (!focus_) {
                focus_ = true;
                focusGrid_ = pageHasGrid();
                paletteCursor_ = 0;
                itemsCursor_ = 0;
                // The page draws a cursor while it is focused and none when
                // it is not, so both directions are a full redraw. It happens
                // on a button press, not in a loop.
                dirty_ = true;
            }
            return false;
        }
        // **Focused, the screen gets first refusal on the press** -- and
        // refuses the map's own d-pad, which falls through below. On the map
        // page the stick is doing the moving, so zoom and the grids keep the
        // buttons they have always had; there is nothing for a cursor to walk
        // over on a map.
        if (focus_ && handleFocusedInput(down)) {
            return false;
        }
        if (playerPage_ != PlayerPage::Map) {
            return false;
        }
        // **Once the d-pad walks or turns the player, the map's own d-pad wants
        // the focus.** Under the New 3DS scheme nothing in gameplay uses it and
        // zoom is a free press; under either Old scheme the same press is a
        // step forward, and answering both would zoom the map every time the
        // player walked past it. X focuses, and a focused page has the d-pad --
        // see `dpadTakenByScreen`.
        if (dpadIsGameplay_ && !focus_) {
            return false;
        }
        if (down & (KEY_DLEFT | KEY_DRIGHT)) {
            map_.cycleGrid((down & KEY_DRIGHT) != 0 ? 1 : -1);
        }
        // Up magnifies. The map is a picture of the ground, and pushing up
        // towards it is the gesture every other map has.
        if (down & (KEY_DUP | KEY_DDOWN)) {
            map_.cycleZoom((down & KEY_DUP) != 0 ? 1 : -1);
        }
        return false;
    }

    if (page_ != Page::Settings) {
        return false;
    }

    if (down & (KEY_DUP | KEY_DDOWN)) {
        cursor_ += (down & KEY_DDOWN) != 0 ? 1 : kSettingCount - 1;
        cursor_ %= kSettingCount;
        dirty_ = true;
    }

    // One step per press, never a repeat: a held d-pad on a render distance
    // would rebuild the pool every frame.
    const int delta = (down & KEY_DRIGHT) != 0 ? 1 : ((down & KEY_DLEFT) != 0 ? -1 : 0);
    if (delta == 0) {
        return false;
    }

    dirty_ = true;
    if (cursor_ == 0) {
        const int wanted = settings->renderDistance + delta;
        if (wanted < settings->minDistance || wanted > settings->maxDistance) {
            return false;
        }
        settings->renderDistance = wanted;
        return true;
    }

    if (cursor_ == 1) {
        settings->geometryQuads = !settings->geometryQuads;
        return true;
    }

    if (cursor_ == 2) {
        settings->greedyMeshing = !settings->greedyMeshing;
        return true;
    }

    if (cursor_ == 3) {
        settings->wireframe = !settings->wireframe;
        return true;
    }

    // Teleport. Either direction opens it, because the row has no value to step
    // through and refusing left would only be a way to be unhelpful.
    //
    // Returns false whatever happens: nothing in DebugSettings changed, so the
    // caller has no pool to rebuild. The camera moved or it did not, and the
    // next frame reads it either way.
    teleportViaKeyboard(camera);
    return false;
}

// The applet. Both screens belong to it while it runs and this thread is
// suspended, which has two consequences the caller depends on:
//
//   * The bottom-screen console is gone when it returns, so `dirty_` is set by
//     the caller to force a full reprint rather than the usual in-place update.
//   * The frame that contains this call takes as long as the player took to
//     type. main.cpp already clamps dt to 0.25 s for exactly this reason -- it
//     was written for the console being suspended, and a keyboard is the same
//     shape of pause -- so nobody flies 400 blocks on the next frame.
//
// It must not be called between C3D_FrameBegin and C3D_FrameEnd. Input is
// handled at the top of the loop, well before the frame opens, so it is not.
bool Overlay::editSignViaKeyboard(world::SignStore* store, int index)
{
    if (store == nullptr || index < 0 || index >= store->count()) {
        return false;
    }

    // Four lines of fifteen, plus the newlines between them and a terminator.
    constexpr int kMaxText =
        world::kSignLines * (world::kSignLineLength + 1) + 1;

    // Prefilled with whatever is already on it, so editing a sign is editing
    // rather than retyping.
    char initial[kMaxText];
    int at = 0;
    for (int line = 0; line < world::kSignLines; ++line) {
        const char* text = (*store)[index].lines[line];
        for (int i = 0; text[i] != '\0' && at < kMaxText - 2; ++i) {
            initial[at++] = text[i];
        }
        if (line + 1 < world::kSignLines && at < kMaxText - 2) {
            initial[at++] = '\n';
        }
    }
    initial[at] = '\0';

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "sign text");
    swkbdSetFeatures(&swkbd, SWKBD_MULTILINE | SWKBD_DARKEN_TOP_SCREEN);
    // **No validation.** A blank sign is a legal sign -- it is what every sign
    // starts as -- so there is nothing here for a filter to refuse.

    char text[kMaxText];
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    // The same re-init `teleportViaKeyboard` documents at length: libctru's
    // console caches a framebuffer address that an applet invalidates.
    bottom::initConsole();
    dirty_ = true;
    bodyDirty_ = true;
    hotbarDirty_ = true;

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }

    // Split on newline into four lines, each truncated to fifteen characters.
    // A player who types one long line gets it on the first row and three blank
    // rows, which is what the original's editor would have made them do by
    // hand.
    int line = 0;
    int start = 0;
    for (int i = 0; i <= int(std::strlen(text)) && line < world::kSignLines; ++i) {
        if (text[i] != '\n' && text[i] != '\0') {
            continue;
        }
        store->setLine(index, line, std::string_view(text + start, usize(i - start)));
        ++line;
        start = i + 1;
        if (text[i] == '\0') {
            break;
        }
    }
    // Anything the player deleted has to actually go.
    for (; line < world::kSignLines; ++line) {
        store->setLine(index, line, std::string_view());
    }
    return true;
}

bool Overlay::teleportViaKeyboard(Camera* camera)
{
    // Long enough for three signed coordinates at full length with decimals,
    // and short enough that the whole thing sits on one keyboard line.
    constexpr int kMaxText = 48;

    // Prefilled with where the camera is, so the common case is editing one
    // number rather than typing three. Floored rather than truncated, so it
    // names the block the camera is standing in on the negative side too --
    // int(-3.7) is -3, which is the block next door.
    char initial[kMaxText];
    std::snprintf(initial, sizeof(initial), "%d %d %d", int(std::floor(camera->x)),
                  int(std::floor(camera->y)), int(std::floor(camera->z)));

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "x y z");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);

    // **Validated inside the keyboard rather than after it.** SWKBD_FILTER_CALLBACK
    // lets the parser reject bad input with its own message while the keyboard
    // stays open, so a mistyped coordinate is fixed in place instead of
    // dismissing the applet, printing an error onto a console the player is not
    // looking at, and making them navigate back to reopen it.
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    swkbdSetFilterCallback(&swkbd, validateCoordinates, nullptr);

    char text[kMaxText];
    // **The session is told before the applet starts, not after.** This
    // suspends the whole application for as long as somebody is typing; see
    // `ctr::linkPausing`.
    linkPausing(net::link::kAppletAwayMs);
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    // **Re-init the console before anything else.** libctru's console caches
    // the framebuffer address at consoleInit and never looks it up again --
    // objdump on console.o shows the only call to gfxGetFramebuffer is inside
    // consoleInit -- while gfx.o reallocates framebuffers through
    // linearAlloc/vramAlloc when the screen format changes and flips which
    // buffer is current. An applet does both, so after one returns, the
    // cached pointer is at best pointing at the wrong buffer.
    //
    // consoleInit is safe to repeat: it calls no allocator at all (memcpy,
    // gfxSetScreenFormat, gfxSetDoubleBuffering, gfxSwapBuffersGpu,
    // gspWaitForEvent, gfxGetFramebuffer, a clear, and setvbuf), so this
    // leaks nothing and costs one VBlank wait on a frame where the player has
    // just spent seconds typing. The caller has already set `dirty_`, which is
    // what reprints the header and the page over the cleared screen.
    bottom::initConsole();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }

    // Parsed a second time rather than smuggled out of the callback. The filter
    // has already guaranteed this succeeds; doing it again is a few microseconds
    // against a global, and the callback runs on text that the player may still
    // have edited afterwards in ways the filter is not called for.
    CoordTriple target;
    if (parseCoordinateTriple(text, &target) != nullptr) {
        return false;
    }

    camera->x = target.x;
    camera->y = target.y;
    camera->z = target.z;
    return true;
}

void Overlay::draw(const Renderer& renderer, const render::WorldStreamer& world,
                   const Camera& camera, const FrameTiming& timing, float frameMs,
                   float timeOfDay, const DebugSettings& settings)
{
    // **What the frame is claimed to be made of**, and therefore what is left.
    // `blockedMs` and `submitMs` are the renderer's own halves of `drawFrame`;
    // the other three are main.cpp's. The GPU numbers are deliberately not in
    // here: they run alongside the CPU rather than inside it, so subtracting
    // them would count the same milliseconds twice in the other direction.
    //
    // `frameMs` is measured one loop-top to the next, so a single frame's
    // residual carries part of the previous frame and can come out negative.
    // Over a block of twenty it does not, and the clamp is there for the frame
    // it does rather than as a correction to the mean.
    const float accounted = timing.walkMs + timing.streamMs + timing.tickMs
                            + renderer.submitMs() + renderer.blockedMs();
    const float other = frameMs > accounted ? frameMs - accounted : 0.0f;

    accum_.frame += frameMs;
    accum_.draw += renderer.gpuDrawMs();
    accum_.process += renderer.gpuProcessMs();
    accum_.blocked += renderer.blockedMs();
    accum_.submit += renderer.submitMs();
    accum_.walk += timing.walkMs;
    accum_.stream += timing.streamMs;
    accum_.tick += timing.tickMs;
    accum_.other += other;

    peak_.frame = frameMs > peak_.frame ? frameMs : peak_.frame;
    peak_.draw = renderer.gpuDrawMs() > peak_.draw ? renderer.gpuDrawMs() : peak_.draw;
    peak_.process = renderer.gpuProcessMs() > peak_.process ? renderer.gpuProcessMs()
                                                            : peak_.process;
    peak_.blocked = renderer.blockedMs() > peak_.blocked ? renderer.blockedMs() : peak_.blocked;
    peak_.submit = renderer.submitMs() > peak_.submit ? renderer.submitMs() : peak_.submit;
    peak_.walk = timing.walkMs > peak_.walk ? timing.walkMs : peak_.walk;
    peak_.stream = timing.streamMs > peak_.stream ? timing.streamMs : peak_.stream;
    peak_.tick = timing.tickMs > peak_.tick ? timing.tickMs : peak_.tick;
    peak_.other = other > peak_.other ? other : peak_.other;

    const bool tick = ++samples_ >= kSamplesPerUpdate;
    if (tick) {
        const float n = float(samples_);
        shown_ = {accum_.frame / n,  accum_.draw / n,   accum_.process / n, accum_.blocked / n,
                  accum_.submit / n, accum_.walk / n,    accum_.stream / n,  accum_.tick / n,
                  accum_.other / n};
        shownPeak_ = peak_;
        accum_ = Accum{};
        peak_ = Accum{};
        samples_ = 0;

        // What the cache did over the block that just ended, against what it
        // had done at the start of it. See the note on ioPrevious_.
        const world::ChunkCache::Stats& io = world.stats().io;
        ioDelta_.mainThreadMicros = io.mainThreadMicros - ioPrevious_.mainThreadMicros;
        ioDelta_.stats = io.stats - ioPrevious_.stats;
        ioDelta_.reads = io.reads - ioPrevious_.reads;
        ioDelta_.writes = io.writes - ioPrevious_.writes;
        ioDelta_.listings = io.listings - ioPrevious_.listings;
        ioDelta_.hits = io.hits - ioPrevious_.hits;
        ioDelta_.misses = io.misses - ioPrevious_.misses;
        ioDelta_.prefetchHits = io.prefetchHits - ioPrevious_.prefetchHits;
        ioDelta_.evicted = io.evicted - ioPrevious_.evicted;
        ioPrevious_ = io;
    }

    const bool cleared = dirty_;

    // **The player's half is not on the sample-block clock.** Every debug page
    // is a set of numbers that would be unreadable if they changed every frame;
    // these are a picture of where the player is and which way they are facing,
    // and a third of a second of lag in either is the difference between a map
    // and a memory. It costs nothing to ask every frame: each page redraws only
    // when what it draws has moved, and returns immediately when it has not.
    if (page_ == Page::Player) {
        if (drawPlayerPage(camera, cleared)) {
            // The paint is finished: one copy puts all of it on the glass.
            bottom::flush();
        }
        return;
    }

    // **The header and the footer are reprinted only on a page change.**
    // `row()` blanks a whole line -- forty columns, the full width of the
    // screen -- which is why nothing on the player's half above uses it.
    if (dirty_) {
        clearScreen();
        // Clipped like everything else: a world name is a path off the SD card
        // and there is nothing stopping it being longer than the screen.
        row(1, "\x1b[32m3DAlpha %s\x1b[0m  %s", mcver::kDisplay, model_);
        row(2, "%s", worldName_);
        row(kFooterRow, "SELECT+Y / SELECT+X  change page");
        row(kFooterRow + 1, "START pause");
        dirty_ = false;
    }

    // Printing is the expensive part -- libctru's console renders every glyph
    // into the bottom framebuffer on the CPU -- so it happens once per sample
    // block, not once per frame. An overlay that costs several milliseconds
    // would be measuring itself. A page change or a settings edit is the
    // exception: waiting a third of a second to see a button press is worse.
    if (!tick && !cleared) {
        return;
    }

    int next = kBodyRow;
    switch (page_) {
    case Page::Player:
        break;  // handled above, before the sample-block throttle
    case Page::Info:
        next = drawInfo(renderer, world, camera, timeOfDay);
        break;
    case Page::Storage:
        next = drawStorage(world);
        break;
    case Page::Settings:
        next = drawSettings(renderer, settings, camera);
        break;
    }

    // **Blank what the page did not use.** Pages are different lengths and the
    // Info page's own length moves -- the generation rows come and go with the
    // world -- so a shorter draw over a longer one would leave the tail of the
    // longer one on screen, still being read as current.
    while (next < kFooterRow) {
        blank(next++);
    }
}

// **The palette and the hotbar are nine columns wide on purpose.** The focused
// d-pad steps between them straight down, and a column in one meaning a
// different column in the other would make that step a translation nobody could
// predict.
static_assert(hud::kPaletteColumns == hud::kHotbarColumns,
              "the palette and the hotbar share a column");

void Overlay::releaseFocus()
{
    focus_ = false;
    focusGrid_ = false;
    // Same reason as the page change: the outline has nowhere to be once the
    // cursor it was following is gone.
    heldSlot_ = -1;

    // **Going out puts the map back on the player.** A pan is a thing you did
    // with the focus on, and leaving the focus with the window parked four
    // hundred blocks away would be a mode the player had no way left to get out
    // of -- the stick walks again the moment X is let go.
    map_.clearPan();

    // A full clear rather than the two page flags: the cursor and the map's
    // pan window are drawn *into* the page, so the only way to take them away
    // is to paint the page again.
    dirty_ = true;
}

void Overlay::tickFocus(float dt)
{
    if (uiCursorActive()) {
        tickCursorStick(dt);
        return;
    }
    // Centred as far as the cursor is concerned, so the next time a grid is
    // focused the first push is a fresh one rather than a repeat left over from
    // whatever the stick was doing on the way out.
    stick_.reset();
    if (!mapPanActive()) {
        return;
    }

    circlePosition pad;
    hidCircleRead(&pad);
    const float x = padAxis(pad.dx);
    const float z = padAxis(pad.dy);
    if (x == 0.0f && z == 0.0f) {
        return;
    }

    // **Half a window a second, at every zoom.** Expressed in windows rather
    // than blocks because that is what the gesture means: pushing the stick
    // over should take about the same time to cross the picture whether the
    // picture is 104 blocks across or 416. A fixed blocks-per-second would feel
    // like four different speeds.
    const double windowsPerSecond = 0.5;
    const double blocksWide = double(map::mapWindowBlocks(kMapWidth, map_.zoom()));
    const double blocksHigh = double(map::mapWindowBlocks(mapHeight(), map_.zoom()));

    // Pad +y is *up* on the stick, which is north, which is -Z.
    map_.pan(double(x) * blocksWide * windowsPerSecond * double(dt),
             double(-z) * blocksHigh * windowsPerSecond * double(dt));
}

// **The circle pad, walked like a d-pad.** The focused screens are grids and
// rows, and a grid is crossed by stepping -- so the stick does what the d-pad
// does rather than an analogue thing of its own, and a player who was holding
// it to walk does not have to find another control to move a cursor.
//
// The rule is core's -- see core/gui/stick_cursor.hpp for which way a diagonal
// counts and how the repeat is timed. This is the two lines that are actually
// the console's: reading the pad, and the `KEY_D*` bit the screens below expect
// a direction to arrive as.
void Overlay::tickCursorStick(float dt)
{
    circlePosition pad;
    hidCircleRead(&pad);

    // Raw rather than through `padAxis`: the deadzone is part of the rule and
    // is applied on the other side of this call, not twice.
    const float x = float(pad.dx) / 156.0f;
    const float y = float(pad.dy) / 156.0f;

    u32 direction = 0;
    switch (stick_.step(x, y, dt)) {
    case gui::StickStep::Left:  direction = KEY_DLEFT; break;
    case gui::StickStep::Right: direction = KEY_DRIGHT; break;
    case gui::StickStep::Up:    direction = KEY_DUP; break;
    case gui::StickStep::Down:  direction = KEY_DDOWN; break;
    case gui::StickStep::None:  return;
    }
    handleFocusedInput(direction);
}

// True when the press was the focused screen's and the caller should stop.
// **False is not "nothing happened"** -- it is "this belongs to the page", which
// on the map means the zoom and the grids.
bool Overlay::handleFocusedInput(u32 down)
{
    if (session_.isOpen()) {
        return handleContainerInput(down);
    }

    // B lets the screen go, which is the same thing B does everywhere else in
    // this shell.
    if ((down & KEY_B) != 0) {
        releaseFocus();
        return true;
    }

    // **The shoulders change tab, and the focus survives the change.** That is
    // the whole reason they are not the palette's pager any more: a focused
    // screen with no button route between Map, Items and Blocks could only be
    // navigated by touching it, which is exactly what the focus exists to avoid.
    // The palette pages instead by running the cursor off either end of its
    // grid, and by the two arrows on its title row for anyone using the stylus.
    //
    // Unfocused, L and R are break and place; main.cpp reads the same focus
    // flag and suspends the edit path, so no press ever does both.
    if ((down & (KEY_L | KEY_R)) != 0) {
        stepTab((down & KEY_R) != 0);
        return true;
    }

    // **The map keeps its own d-pad even focused.** The stick is what moves a
    // focused map, so the zoom and the grids are not competing with anything --
    // and there is no cursor on this page to walk over. ZL and ZR still change
    // the held slot; they are read before this is ever called.
    if (playerPage_ == PlayerPage::Map) {
        return false;
    }

    const bool onBlocks = playerPage_ == PlayerPage::Blocks;
    const int pages = hud::palettePageCount();

    if (focusGrid_ && onBlocks) {
        if ((down & (KEY_DLEFT | KEY_DRIGHT)) != 0) {
            // Linear through the page and off its ends into the next one, which
            // is how a list of 70 things reads. Stopping at the last cell of
            // the last page is the only clamp.
            int cell = paletteCursor_ + ((down & KEY_DRIGHT) != 0 ? 1 : -1);
            if (cell < 0) {
                cell = 0;
                if (palettePage_ > 0) {
                    --palettePage_;
                    cell = hud::kPalettePerPage - 1;
                }
            } else if (cell >= hud::kPalettePerPage) {
                cell = hud::kPalettePerPage - 1;
                if (palettePage_ + 1 < pages) {
                    ++palettePage_;
                    cell = 0;
                }
            }
            paletteCursor_ = cell;
            bodyDirty_ = true;
        }
        // **Off the top row is the hotbar**, in the same column -- the grid
        // and the band are one cursor space with a gap in it. It used to be off
        // the *bottom*; the band moved to the top of the screen, and a cursor
        // that left a grid downwards to reach something drawn above it would be
        // the kind of wrongness that is felt rather than seen.
        if ((down & KEY_DUP) != 0) {
            if (paletteCursor_ >= hud::kPaletteColumns) {
                paletteCursor_ -= hud::kPaletteColumns;
                bodyDirty_ = true;
            } else {
                focusGrid_ = false;
                inventory_.selected = paletteCursor_ % hud::kPaletteColumns;
                bodyDirty_ = true;
                hotbarDirty_ = true;
            }
        }
        if ((down & KEY_DDOWN) != 0
            && paletteCursor_ + hud::kPaletteColumns < hud::kPalettePerPage) {
            paletteCursor_ += hud::kPaletteColumns;
            bodyDirty_ = true;
        }
        if ((down & KEY_A) != 0) {
            // Into the slot that is already selected. **A does not move the
            // selection**: filling a hotbar is pick a slot, then pick a block,
            // and a pick that moved the slot too would fill one slot nine times.
            takeFromPalette();
            bodyDirty_ = true;
        }
        return true;
    }

    // The backpack grid, which walks exactly like the palette above it and
    // acts differently: there is nothing to take a copy of here, so A picks a
    // stack up and the next A puts it down.
    if (focusGrid_ && playerPage_ == PlayerPage::Items) {
        const bool onArmour = itemsCursor_ >= item::kBackpackSlots;
        const int armourRow = onArmour ? itemsCursor_ - item::kBackpackSlots : 0;

        // **Left off the first column is the armour**, which is where it is
        // drawn, and right off the armour comes back to the row it left from.
        // The armour is one cell taller than the backpack, so the fourth row
        // lands on the last backpack row rather than nowhere.
        if ((down & (KEY_DLEFT | KEY_DRIGHT)) != 0) {
            const bool right = (down & KEY_DRIGHT) != 0;
            if (onArmour) {
                if (right) {
                    const int row = armourRow < hud::kItemsRows ? armourRow
                                                                : hud::kItemsRows - 1;
                    itemsCursor_ = row * hud::kItemsColumns;
                }
            } else if (!right && itemsCursor_ % hud::kItemsColumns == 0) {
                itemsCursor_ = item::kBackpackSlots + itemsCursor_ / hud::kItemsColumns;
            } else {
                int cell = itemsCursor_ + (right ? 1 : -1);
                cell = cell < 0 ? 0
                                : (cell >= item::kBackpackSlots ? item::kBackpackSlots - 1
                                                                : cell);
                itemsCursor_ = cell;
            }
            bodyDirty_ = true;
        }
        if ((down & KEY_DUP) != 0) {
            if (onArmour && armourRow > 0) {
                --itemsCursor_;
                bodyDirty_ = true;
            } else if (!onArmour && itemsCursor_ >= hud::kItemsColumns) {
                itemsCursor_ -= hud::kItemsColumns;
                bodyDirty_ = true;
            } else {
                // Off the top row is the hotbar, in the same column -- the
                // same one cursor space with a gap in it that the palette has.
                focusGrid_ = false;
                inventory_.selected = onArmour ? 0 : itemsCursor_ % hud::kItemsColumns;
                bodyDirty_ = true;
                hotbarDirty_ = true;
            }
        }
        if ((down & KEY_DDOWN) != 0) {
            if (onArmour) {
                if (armourRow + 1 < item::kArmourSlots) {
                    ++itemsCursor_;
                    bodyDirty_ = true;
                }
            } else if (itemsCursor_ + hud::kItemsColumns < item::kBackpackSlots) {
                itemsCursor_ += hud::kItemsColumns;
                bodyDirty_ = true;
            }
        }
        if ((down & KEY_A) != 0) {
            touchSlot(hud::itemsSlotForCell(itemsCursor_));
        }
        if ((down & KEY_B) != 0 && heldSlot_ >= 0) {
            cancelHeld();
        }
        return true;
    }

    // The hotbar row. Left and right are the selection, which is what ZL and ZR
    // do and is the reason an old 3DS is not shut out of changing it.
    if ((down & (KEY_DLEFT | KEY_DRIGHT)) != 0) {
        inventory_.cycle((down & KEY_DRIGHT) != 0 ? 1 : -1);
        selectionMoved();
    }
    const bool onItems = playerPage_ == PlayerPage::Items;
    // **Down into the grid, because the grid is below the band now.** The cell
    // entered is the *first* row's, in the column the hand is on, which is the
    // cell directly under the slot the cursor just left.
    if ((onBlocks || onItems) && (down & KEY_DDOWN) != 0) {
        focusGrid_ = true;
        if (onBlocks) {
            paletteCursor_ = inventory_.selected;
        } else {
            itemsCursor_ = inventory_.selected;
        }
        bodyDirty_ = true;
        hotbarDirty_ = true;
    }
    if (onBlocks && (down & KEY_A) != 0) {
        // "Where did this come from" -- the cursor jumps to the held item's own
        // cell, paging the palette to find it. Useful precisely when the
        // palette is two pages and the item is on the other one.
        showItemInPalette(inventory_.selectedItem());
    }
    if (onItems && (down & KEY_A) != 0) {
        // On the hotbar row of the Items page, A picks the held slot up or puts
        // the carried stack into it -- the same two lines the grid above uses,
        // so a stack can be moved between the band and the backpack without
        // leaving the row it started on.
        touchSlot(inventory_.selected);
    }
    if ((down & KEY_B) != 0 && heldSlot_ >= 0) {
        cancelHeld();
    }
    return true;
}

void Overlay::stepTab(bool forward)
{
    PlayerPage pages[hud::kMaxTabs];
    const int count = playerPagesFor(pages);
    const int tab = selectedTab() + (forward ? 1 : count - 1);
    selectTab(tab % count);
    // selectTab drops the grid cursor when it leaves a page that has one and
    // does not put it back on the way in, so say where the focus goes.
    focusGrid_ = pageHasGrid();
    hotbarDirty_ = true;
    bodyDirty_ = true;
}

// Nine, because a chest row is nine slots wide wherever the screen puts it --
// the same number `container_layout.cpp` lays the grid out in.
constexpr int kChestColumns = 9;

bool Overlay::handleContainerInput(u32 down)
{
    const bool worldScreen = containerOpen();
    // B shuts a world container. On the inventory page it is the usual B --
    // the focus goes and the grid stays, since the page is still up.
    if ((down & KEY_B) != 0) {
        if (worldScreen) {
            closeContainer();
        } else {
            releaseFocus();
        }
        return true;
    }
    if ((down & (KEY_L | KEY_R)) != 0) {
        // **L and R scroll a tall chest**, and step the tabs everywhere else.
        // A world screen has no tabs to step -- the strip is the player's
        // pages and a chest is not one of them -- so the pair was free, and a
        // shoulder button is the one control a player can use without taking
        // a hand off the screen.
        if (worldScreen) {
            scrollContainer((down & KEY_R) != 0 ? 1 : -1);
        } else {
            stepTab((down & KEY_R) != 0);
        }
        return true;
    }

    const int dx = ((down & KEY_DRIGHT) != 0 ? 1 : 0) - ((down & KEY_DLEFT) != 0 ? 1 : 0);
    const int dy = ((down & KEY_DDOWN) != 0 ? 1 : 0) - ((down & KEY_DUP) != 0 ? 1 : 0);
    if (dx != 0 || dy != 0) {
        // **Stepping off the top or the bottom of a scrolled chest scrolls
        // it**, and the cursor stays on the row it is on: the content moves
        // under the cursor rather than the cursor running off the window. Only
        // inside the chest's own slots, so the backpack is still one press
        // below the last row once there is nothing left to scroll to.
        if (dy != 0 && scrollAtChestEdge(dy)) {
            const int moved = containerCursor_ + dy * kChestColumns;
            if (moved >= 0 && moved < session_.containerSlots()) {
                containerCursor_ = moved;
            }
            bodyDirty_ = true;
            hotbarDirty_ = true;
            return true;
        }
        // One axis a press; a diagonal on a d-pad is a slipped thumb.
        const int next = gui::containerStep(layout_, containerCursor_, dx, dx != 0 ? 0 : dy);
        if (next != containerCursor_) {
            containerCursor_ = next;
            selectUnderContainerCursor();
            bodyDirty_ = true;
            hotbarDirty_ = true;
        }
    }
    if ((down & KEY_A) != 0) {
        clickContainer(containerCursor_, 0);
    }
    if ((down & KEY_Y) != 0) {
        clickContainer(containerCursor_, 1);
    }
    return true;
}

bool Overlay::scrollContainer(int rows)
{
    if (layout_.chestWindowRows <= 0 || layout_.chestWindowRows >= layout_.chestRows) {
        return false;
    }
    const int wanted = layout_.chestFirstRow + rows;
    if (wanted < 0 || wanted > layout_.chestRows - layout_.chestWindowRows) {
        return false;
    }
    containerScroll_ = wanted;
    gui::buildContainerLayout(session_, hud::pageTop(), &layout_, containerScroll_);

    // **The cursor never scrolls out of sight.** A slot outside the window has
    // no rectangle, so a cursor left on one would be invisible, unmovable by
    // the d-pad -- which steps between rectangles -- and still what A clicks.
    // It comes back to the nearest row of the window, in its own column.
    if (containerCursor_ >= 0 && containerCursor_ < session_.containerSlots()
        && layout_.rect[containerCursor_].w <= 0) {
        const int first = layout_.chestFirstRow;
        const int last = first + layout_.chestWindowRows - 1;
        int row = containerCursor_ / kChestColumns;
        row = row < first ? first : (row > last ? last : row);
        containerCursor_ = row * kChestColumns + containerCursor_ % kChestColumns;
        hotbarDirty_ = true;
    }
    bodyDirty_ = true;
    return true;
}

// Whether the cursor is on the chest row a press in `dy` would leave the window
// from, **and** there is somewhere to scroll to -- in which case it scrolls and
// this answers true.
bool Overlay::scrollAtChestEdge(int dy)
{
    if (layout_.chestWindowRows <= 0 || layout_.chestWindowRows >= layout_.chestRows) {
        return false;
    }
    if (containerCursor_ < 0 || containerCursor_ >= session_.containerSlots()) {
        return false;
    }
    const int row = containerCursor_ / kChestColumns;
    const int edge = dy < 0 ? layout_.chestFirstRow
                            : layout_.chestFirstRow + layout_.chestWindowRows - 1;
    if (row != edge) {
        return false;
    }
    return scrollContainer(dy);
}

void Overlay::clickContainer(int index, int button)
{
    const item::SlotClick click = session_.click(containerWorld_, inventory_, index, button);
    if (click.changed) {
        // A click can land on the hand or the backpack, and those save.
        inventoryWritten();
    }
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

void Overlay::quickMoveContainer(int index)
{
    const item::SlotClick click = session_.quickMove(containerWorld_, inventory_, index);
    if (click.changed) {
        inventoryWritten();
    }
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

void Overlay::openContainer(tick::TickWorld& world, tick::TickWorld::ContainerKind kind, i32 x,
                            int y, i32 z)
{
    if (!hasHotbar() || dead_) {
        return;
    }
    // One screen at a time: the inventory grid, or a chest the player somehow
    // reached with another open, is shut first and drops what it held.
    closeSession();
    heldSlot_ = -1;
    throwSlot_ = -1;
    containerWorld_ = &world;

    bool opened = false;
    switch (kind) {
    case tick::TickWorld::ContainerKind::Workbench:
        session_.openWorkbench(x, y, z);
        opened = true;
        break;
    case tick::TickWorld::ContainerKind::Furnace:
        opened = session_.openFurnace(world, x, y, z);
        break;
    case tick::TickWorld::ContainerKind::Chest:
        opened = session_.openChest(world, x, y, z);
        break;
    }
    if (!opened) {
        syncInventorySession();
        return;
    }
    session_.takeChanged();
    containerScroll_ = 0;
    gui::buildContainerLayout(session_, hud::pageTop(), &layout_, containerScroll_);
    // The first of the screen's own slots that takes a stack: a crafting
    // grid's first cell rather than its take-only result.
    containerCursor_ = kind == tick::TickWorld::ContainerKind::Workbench ? 1 : 0;
    shownCook_ = -1;
    shownBurn_ = -1;
    focus_ = true;
    focusGrid_ = true;
    dirty_ = true;
}

void Overlay::openMinecartChest(tick::TickWorld& world, entity::MinecartSystem& carts,
                                u32 cartId)
{
    if (!hasHotbar() || dead_) {
        return;
    }
    closeSession();
    heldSlot_ = -1;
    throwSlot_ = -1;
    // **The world is still held**, even though the cart's slots do not live in
    // it: everything else the screen does -- dropping what a close leaves,
    // syncing the inventory -- goes through it exactly as a chest's does.
    containerWorld_ = &world;

    if (!session_.openMinecartChest(carts, cartId)) {
        syncInventorySession();
        return;
    }
    session_.takeChanged();
    containerScroll_ = 0;
    gui::buildContainerLayout(session_, hud::pageTop(), &layout_, containerScroll_);
    containerCursor_ = 0;
    shownCook_ = -1;
    shownBurn_ = -1;
    focus_ = true;
    focusGrid_ = true;
    dirty_ = true;
}

void Overlay::closeSession()
{
    if (!session_.isOpen()) {
        return;
    }
    item::ItemStack dropped[10];
    const int count = session_.close(dropped, 10);
    for (int i = 0; i < count; ++i) {
        if (closedCount_ < kMaxClosedStacks) {
            closed_[closedCount_++] = std::move(dropped[i]);
        } else {
            // No room left in the queue, which takes more closes in one frame
            // than there are screens: into the inventory instead.
            inventory_.addStack(item::ItemId(dropped[i].id), int(dropped[i].count),
                                dropped[i].damage);
            inventoryWritten();
        }
    }
    throwCursor_ = false;
    dirty_ = true;
}

void Overlay::closeContainer()
{
    const bool worldScreen = containerOpen();
    closeSession();
    if (worldScreen && focus_) {
        releaseFocus();
    }
    syncInventorySession();
}

void Overlay::syncInventorySession()
{
    const bool wanted = gamemode_ == settings::Gamemode::Survival
                        && playerPage_ == PlayerPage::Items && !dead_;
    if (session_.kind() == item::ScreenKind::Inventory && !wanted) {
        closeSession();
    } else if (!session_.isOpen() && wanted) {
        session_.openInventory();
        containerScroll_ = 0;
        gui::buildContainerLayout(session_, hud::pageTop(), &layout_, containerScroll_);
        // The first backpack cell, which is where the Creative page's cursor
        // starts too.
        containerCursor_ = session_.containerSlots();
        dirty_ = true;
    }
}

void Overlay::tickContainer(tick::TickWorld* world)
{
    if (!containerOpen()) {
        return;
    }
    containerWorld_ = world;
    if (world == nullptr || !session_.pull(world)) {
        closeContainer();
        return;
    }
    if (session_.takeChanged()) {
        bodyDirty_ = true;
    }
    if (layout_.arrowIsProgress) {
        const int cook = session_.furnaceCookScaled(layout_.arrow.w);
        const int burn = session_.furnaceBurnScaled(layout_.flame.h - 2);
        if (cook != shownCook_ || burn != shownBurn_) {
            shownCook_ = cook;
            shownBurn_ = burn;
            progressDirty_ = true;
        }
    }
}

void Overlay::finishClosedStack(bool spawned)
{
    if (closedCount_ <= 0) {
        return;
    }
    item::ItemStack& stack = closed_[closedCount_ - 1];
    if (!spawned && !stack.empty()) {
        inventory_.addStack(item::ItemId(stack.id), int(stack.count), stack.damage);
        inventoryWritten();
    }
    stack = item::ItemStack{};
    --closedCount_;
}

void Overlay::closeContainerIntoInventory()
{
    closeSession();
    while (closedCount_ > 0) {
        finishClosedStack(false);
    }
}

void Overlay::touchSlot(int slot)
{
    if (heldSlot_ < 0) {
        // Nothing to put down, so this is a pick-up -- and an empty slot is not
        // something to pick up. Refusing rather than lifting nothing is what
        // stops a mistap arming a move the player did not ask for.
        if (inventory_.at(slot).empty()) {
            return;
        }
        heldSlot_ = slot;
    } else {
        // A swap either way. Into an empty slot it is a move; onto a full one
        // the two exchange places, which is what a1.1.2's own container click
        // does with a full cursor and is the behaviour that needs no rule about
        // what happens when there is no room.
        //
        // **A refused swap keeps the stack in hand rather than dropping it.**
        // An armour slot takes only its own piece (`Inventory::accepts`), and
        // letting go of the carried stack on a slot that would not have it
        // reads as the move having happened when it did not -- the player looks
        // back at the slot they took it from and it is empty. Holding on is
        // also the original's behaviour: a cursor that a container slot refuses
        // stays full.
        if (!item::Inventory::accepts(slot, inventory_.at(heldSlot_).id)
            || !item::Inventory::accepts(heldSlot_, inventory_.at(slot).id)) {
            bodyDirty_ = true;
            return;
        }
        inventory_.swap(heldSlot_, slot);
        heldSlot_ = -1;
        inventoryWritten();
    }
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

void Overlay::cancelHeld()
{
    heldSlot_ = -1;
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

void Overlay::setInventory(const std::vector<item::ItemStack>& stacks)
{
    inventory_.load(stacks);
    heldSlot_ = -1;
    throwSlot_ = -1;
    if (inventory_.empty() && gamemode_ == settings::Gamemode::Creative) {
        // **A player who has never carried anything gets the opening hand**,
        // which is what makes a brand new Creative world usable. It counts as a
        // change, so the next save writes it: an empty inventory and one that
        // happens to hold the first nine palette entries are different states
        // and the file should say which this is.
        //
        // **Creative only.** a1.1.2 starts every player with nothing, and
        // Survival means it: nine free blocks are the difference between
        // mining the first tree and not having to. Spectator is left out for
        // the same reason from the other side -- it carries nothing, has no
        // hotbar to show it in, and a hand filled here would follow the player
        // into Survival the moment the pause menu changed the mode.
        inventory_.fillHandFromPalette();
        inventoryChanged_ = true;
    }
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

bool Overlay::takeInventoryChange()
{
    const bool changed = inventoryChanged_;
    inventoryChanged_ = false;
    return changed;
}

void Overlay::selectionMoved()
{
    hotbarDirty_ = true;
    // **The hand moved and none of the forty slots did**, so there is nothing
    // new to save -- but the palette page names what is in the hand under its
    // grid and outlines that item's cell in it, and both of those were as stale
    // after a shoulder press as the backpack was after a pickup.
    if (playerPage_ == PlayerPage::Blocks) {
        bodyDirty_ = true;
    }
}

void Overlay::selectUnderContainerCursor()
{
    // **The amber mark and the white one are the same slot on the band.** A
    // container screen's last nine slots are the hand, so a cursor that walks
    // into them is pointing at a slot the player is also holding -- and two
    // outlines disagreeing about which one that is, on a screen whose whole job
    // is to say where the next press lands, is the one thing it must never
    // show. This is `cursorToHand` the other way round: there the hand moves
    // the cursor, here the cursor moves the hand.
    //
    // Off the band nothing happens: a cursor in the chest or the backpack is
    // not pointing at anything the hand holds.
    if (!session_.isOpen()) {
        return;
    }
    const int slot = containerCursor_ - (session_.slotCount() - item::kHotbarSlots);
    if (slot < 0 || slot >= item::kHotbarSlots || slot == inventory_.selected) {
        return;
    }
    inventory_.selected = slot;
    hotbarDirty_ = true;
}

void Overlay::cursorToHand()
{
    if (session_.isOpen()) {
        // `lo`, `hx`, `id` and `ea` all end with the nine slots of the hand,
        // which is the numbering `container_layout` lays out and `ee` clicks.
        const int first = session_.slotCount() - item::kHotbarSlots;
        const int wanted = first + inventory_.selected;
        if (wanted != containerCursor_ && wanted >= 0 && wanted < session_.slotCount()) {
            containerCursor_ = wanted;
            bodyDirty_ = true;
            hotbarDirty_ = true;
        }
        return;
    }
    if (!focusGrid_ || playerPage_ == PlayerPage::Map) {
        return;
    }
    // The band's cursor is `inventory_.selected` itself -- see `drawPlayerPage`
    // -- so leaving the grid is the whole of the move.
    focusGrid_ = false;
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

void Overlay::inventoryWritten()
{
    inventoryChanged_ = true;
    hotbarDirty_ = true;
    // The two pages that are a view of the inventory rather than of the world:
    // the backpack grid draws all forty slots, and the palette's caption names
    // whatever is in the hand. The map and the look pad draw neither, so a
    // pickup while one of those is up costs the band and nothing else.
    if (playerPage_ == PlayerPage::Items || playerPage_ == PlayerPage::Blocks
        || session_.isOpen()) {
        bodyDirty_ = true;
    }
}

void Overlay::takeFromPalette()
{
    // **A copy, not a move.** The palette is a catalogue and holds nothing, so
    // taking from it fills the selected slot with a fresh stack and leaves the
    // catalogue exactly as it was. The count is the item's own maximum, which
    // is what Creative means by "as many as you want" without an infinite-stack
    // concept the save format has nowhere to put.
    const item::ItemId id = item::paletteItem(paletteIndex());
    inventory_.set(inventory_.selected, id, i8(item::def(id).stack));
    inventoryWritten();
}

item::ItemId Overlay::dropHeldItem()
{
    const item::ItemId dropped = inventory_.dropOne();
    if (dropped == 0) {
        return 0;
    }
    inventoryWritten();
    return dropped;
}

const item::ItemStack* Overlay::throwRequest() const
{
    if (throwCursor_) {
        return session_.cursor().empty() ? nullptr : &session_.cursor();
    }
    if (throwSlot_ < 0) {
        return nullptr;
    }
    const item::ItemStack& stack = inventory_.at(throwSlot_);
    // Emptied between the press and the answer -- there is no path that does
    // that today, and a throw of nothing is still not something to hand back.
    return stack.empty() ? nullptr : &stack;
}

void Overlay::finishThrow()
{
    if (throwCursor_) {
        // `ee`'s click outside the window with the left button: the whole
        // cursor goes.
        item::ItemStack thrown;
        session_.throwCursor(0, &thrown);
        throwCursor_ = false;
        bodyDirty_ = true;
        hotbarDirty_ = true;
        return;
    }
    if (throwSlot_ < 0) {
        return;
    }
    // The whole stack, so the slot empties rather than counting down: this is
    // `windowClick`'s spill of the cursor and not `dropOneItem`.
    inventory_.set(throwSlot_, item::ItemId(0), 0);
    if (heldSlot_ == throwSlot_) {
        heldSlot_ = -1;
    }
    throwSlot_ = -1;
    inventoryWritten();
    bodyDirty_ = true;
}

void Overlay::cancelThrow()
{
    // The stack stays exactly where it is, and so does the hand holding it:
    // a throw the world had no room for has to read as a throw that did not
    // happen, not as one that lost the stack.
    throwSlot_ = -1;
    throwCursor_ = false;
}

void Overlay::replaceHeldItem(item::ItemId id)
{
    const int slot = inventory_.selected;
    const item::ItemStack& stack = inventory_.at(slot);
    if (id == 0 || stack.empty() || stack.id == i16(id)) {
        return;
    }
    inventory_.set(slot, id, stack.count);
    inventoryWritten();
}

void Overlay::setDead(bool dead, int score)
{
    if (dead == dead_) {
        return;
    }
    dead_ = dead;
    deathScore_ = score;
    deathCursor_ = 0;
    deathChoice_ = DeathChoice::None;
    // Whatever the screen was doing is over: a stack in hand has already been
    // dropped with the rest, and a focus left on would come back pointing at
    // a page that was never redrawn under it.
    // An open screen shuts with the death, as `au` replacing it does, and its
    // cursor and grid go on the ground with everything else.
    closeSession();
    heldSlot_ = -1;
    throwSlot_ = -1;
    if (focus_) {
        releaseFocus();
    }
    syncInventorySession();
    dirty_ = true;
}

Overlay::DeathChoice Overlay::takeDeathChoice()
{
    const DeathChoice choice = deathChoice_;
    deathChoice_ = DeathChoice::None;
    return choice;
}

int Overlay::collectItems(mc::entity::ItemEntitySystem& items, const AABB& playerBox)
{
    const int taken = items.collect(playerBox, inventory_);
    if (taken > 0) {
        // **This is the one that made the open backpack look frozen.** Walking
        // over a dropped item writes whichever of the thirty-six slots it
        // merges into, and marking only the band left the grid above showing
        // the inventory as it was when the page was opened.
        inventoryWritten();
    }
    return taken;
}

void Overlay::showItemInPalette(item::ItemId id)
{
    const int index = item::paletteIndexOf(id);
    if (index < 0) {
        return;
    }
    palettePage_ = index / hud::kPalettePerPage;
    paletteCursor_ = index % hud::kPalettePerPage;
    focusGrid_ = true;
    bodyDirty_ = true;
    hotbarDirty_ = true;
}

bool Overlay::cursorShown() const
{
    // **Focused, the cursor is always drawn; unfocused, only while a stack is
    // in hand.** A tap picks a stack up and the next tap puts it down, and
    // between the two the mark under the lifted stack is the only thing on the
    // screen that says which cell it came from -- which is what "tapping an
    // item to move it does not show the hover" was. With empty hands the
    // screen goes quiet again, as it always did.
    return focus_ || heldSlot_ >= 0 || (session_.isOpen() && !session_.cursor().empty());
}

void Overlay::carriedPosition(int* itemsCell, int* hotbarSlot) const
{
    *itemsCell = -1;
    *hotbarSlot = -1;
    if (heldSlot_ < 0) {
        return;
    }
    // **The cursor, wherever it was last put -- by the d-pad or by a tap.**
    // Both move it now, which is what makes a stack picked up with the stylus
    // hover over the cell that was touched rather than over whichever slot the
    // hand happened to be on. The two tests below are the same ones
    // `drawPlayerPage` passes as cursors, because a stack hovering over a cell
    // no cursor is on would be marking a slot no press acts on.
    if (focusGrid_ && playerPage_ == PlayerPage::Items) {
        *itemsCell = itemsCursor_;
    } else if (!focusGrid_ && playerPage_ != PlayerPage::Map) {
        *hotbarSlot = inventory_.selected;
    } else if (heldSlot_ < item::kHotbarSlots) {
        *hotbarSlot = heldSlot_;
    } else {
        *itemsCell = hud::itemsCellForSlot(heldSlot_);
    }
}

void Overlay::drawBlocks(const gui::Surface& surface)
{
    // The caption names whatever the player is pointing at: the cursor's cell
    // while the grid is focused, and what is in the hand otherwise.
    const item::ItemId named =
        focusGrid_ ? item::paletteItem(paletteIndex()) : inventory_.selectedItem();
    char caption[40];
    itemCaption(named, caption, sizeof caption);

    hud::drawBlocksPage(surface, sheets_, palettePage_,
                        cursorShown() && focusGrid_ ? paletteCursor_ : -1,
                        inventory_.selectedItem(), caption);
}

bool Overlay::drawPlayerPage(const Camera& camera, bool cleared)
{
    gui::Surface screen;
    // False when libctru is not handing back the framebuffer this expects,
    // which is the cue to draw nothing at all rather than to write pixels at
    // computed offsets into whatever is there.
    if (!hud::bottomSurface(&screen)) {
        dirty_ = true;
        return false;
    }

    if (cleared) {
        // The console first: `\x1b[2J` blanks every cell, and every panel below
        // is drawn over the top of that. Doing it the other way round would
        // erase the panels.
        clearScreen();
        hud::drawBackdrop(screen, haveBackdrop_ ? backdrop_ : nullptr);
        hud::drawTabs(screen, tabs());
        dirty_ = false;
        // The pages below all key off "has this moved", and after a clear
        // nothing on the screen is theirs any more.
        lookYawStep_ = -1;
        bodyDirty_ = true;
        hotbarDirty_ = true;
    }

    // **Dead, the page is the game-over panel and nothing else.** The hotbar is
    // left as the clear drew it -- empty, since the death dropped everything --
    // and the page underneath is not drawn at all, so nothing can show through.
    if (dead_) {
        if (bodyDirty_) {
            hud::drawGameOverPage(screen, deathScore_, deathCursor_);
            bodyDirty_ = false;
            hotbarDirty_ = false;
            return true;
        }
        return cleared;
    }

    // **A container screen is the page and the band both**, since the hand is
    // nine of its slots. The furnace's arrow has a cheaper redraw of its own.
    if (session_.isOpen()) {
        bool drewContainer = cleared;
        if (bodyDirty_) {
            hud::drawContainerPage(screen, layout_, session_, inventory_, sheets_,
                                   containerCursor_, cursorShown());
            drewContainer = true;
        } else if (progressDirty_) {
            hud::drawContainerProgress(screen, layout_, session_);
            drewContainer = true;
        }
        if (hotbarDirty_ || bodyDirty_) {
            hud::drawContainerBand(screen, layout_, session_, inventory_, sheets_,
                                   containerCursor_, cursorShown());
            drewContainer = true;
        }
        bodyDirty_ = false;
        hotbarDirty_ = false;
        progressDirty_ = false;
        return drewContainer;
    }

    // Asked once and used by both halves of the screen: the stack in hand is
    // drawn over the grid or over the band, and never over both.
    int carriedCell = -1;
    int carriedSlot = -1;
    carriedPosition(&carriedCell, &carriedSlot);

    bool drew = cleared;
    switch (playerPage_) {
    case PlayerPage::Map:
        // The map times and flushes its own writes -- see MapScreen::draw --
        // and it is the one page here that draws on most frames, which is why
        // its answer is folded into this function's rather than assumed.
        drew = map_.draw(screen, camera, cleared) || drew;
        break;
    case PlayerPage::Items:
        // Nothing on it changes, so once drawn it stays drawn.
        if (bodyDirty_) {
            hud::drawItemsPage(screen, inventory_, sheets_,
                               cursorShown() && focusGrid_ ? itemsCursor_ : -1, heldSlot_,
                               carriedCell);
            drew = true;
        }
        break;
    case PlayerPage::Blocks:
        if (bodyDirty_) {
            drawBlocks(screen);
            drew = true;
        }
        break;
    case PlayerPage::Look:
        drew = drawLook(screen, camera, cleared) || drew;
        break;
    }
    bodyDirty_ = false;

    // **The band last, and only when it moved.** It is over the page rather
    // than beside it in drawing order for one reason: nothing above may write
    // into the top 40 pixels, and if something ever does, the hotbar is what
    // covers it up rather than what gets covered.
    if (hasHotbar() && hotbarDirty_) {
        // **No cursor on the map page.** The focused d-pad is the map's there,
        // so a cursor on the hotbar would be marking a slot that no button
        // moves -- which is worse than not marking one at all.
        const bool cursorOnHotbar =
            cursorShown() && !focusGrid_ && playerPage_ != PlayerPage::Map;
        hud::drawHotbar(screen, inventory_, sheets_,
                        cursorOnHotbar ? inventory_.selected : -1, heldSlot_, carriedSlot);
        hotbarDirty_ = false;
        drew = true;
    }

    return drew;
}

bool Overlay::drawLook(const gui::Surface& surface, const Camera& camera, bool cleared)
{
    constexpr float kPi = 3.14159265358979f;
    const int step = map::yawStep(camera.yaw * 180.0f / kPi);

    if (cleared) {
        hud::drawLookPage(surface);
    } else if (step == lookYawStep_) {
        return false;
    }
    lookYawStep_ = step;

    // **Only the ribbon, on a turn.** The pad and its crosshair are 52,000
    // pixels and do not move; the ribbon is 9,000 and does, and separating them
    // is the difference between a redraw the player can feel and one they
    // cannot. The angle drawn is the rounded one, so what is on the screen and
    // what this thinks is on it can never disagree.
    hud::drawCompassRibbon(surface, map::yawFromStep(step));
    return true;
}

int Overlay::drawInfo(const Renderer& renderer, const render::WorldStreamer& world,
                      const Camera& camera, float timeOfDay)
{
    const render::ChunkRenderer::FrameStats& frame = renderer.chunks().frameStats();
    const render::VboPool::Stats& pool = renderer.chunks().pool().stats();
    const Renderer::FrameStats& gpu = renderer.frameStats();
    const render::WorldStreamer::Stats& streaming = world.stats();

    constexpr int kKb = 1024;
    constexpr int kMb = 1024 * 1024;

    // **Every line here is measured against 40 columns at its widest values,
    // not its typical ones.** Six of them used to be 41 to 44 characters wide
    // with ordinary numbers in them, which cost a row each and scrolled the
    // page; see row(). The field widths below are the widest each number can
    // actually get -- a Far Lands coordinate is eight digits, a count of cells
    // in the grid is four -- so a busy console does not silently need more room
    // than a quiet one.
    int r = kBodyRow;
    row(r++, "frame %2d.%d ms   fps %2d      %s",
        shown_.frame < 0.0f ? 0 : int(shown_.frame), tenths(shown_.frame) % 10,
        shown_.frame > 0.0f ? int(1000.0f / shown_.frame) : 0, gpu.stereo ? "3D on " : "3D off");
    row(r++, "  GPU draw %2d.%d ms  proc %2d.%d ms", int(shown_.draw), tenths(shown_.draw) % 10,
        int(shown_.process), tenths(shown_.process) % 10);
    // The frame minus the vsync wait. If `busy` is well under 16.7 the console
    // is at its refresh rate and there is nothing to fix; if `vsync` is near
    // zero the CPU is the thing missing frames, and the four numbers under it
    // say which part.
    //
    // **`other` is in it, which is what makes the row mean anything.** `busy`
    // used to be the four measured buckets and nothing else, so it answered a
    // question nobody asked: not "what did the CPU spend" but "what did the
    // CPU spend in the parts of the loop that happen to be instrumented". A
    // frame could read 25 ms over a 15 ms `CPU busy` and every line on the page
    // was telling the truth. It also used to double-count the tick, which the
    // stream bucket already contained; main.cpp subtracts it there now.
    const float busy =
        shown_.walk + shown_.stream + shown_.tick + shown_.submit + shown_.other;
    row(r++, "  CPU busy %2d.%d ms  vsync %2d.%d ms", int(busy), tenths(busy) % 10,
        int(shown_.blocked), tenths(shown_.blocked) % 10);
    row(r++, "  walk %d.%d  stream %2d.%d  submit %d.%d", int(shown_.walk),
        tenths(shown_.walk) % 10, int(shown_.stream), tenths(shown_.stream) % 10,
        int(shown_.submit), tenths(shown_.submit) % 10);
    // **The rest of the loop, and the worst single frame behind the means.**
    // `other` is everything outside the instrumented spans -- input, the net
    // pump, the map sampler, this console's own printing. `frm` against the
    // `frame` mean two rows up is the vsync quantum made visible: 16.7 and 33.4
    // are the only two frame times the top screen has, so a mean between them
    // is a mixture and `frm` says which of the two the bad half was.
    row(r++, "  other %2d.%d ms  peak frm %2d.%d tk %2d.%d", int(shown_.other),
        tenths(shown_.other) % 10, int(shownPeak_.frame), tenths(shownPeak_.frame) % 10,
        int(shownPeak_.tick), tenths(shownPeak_.tick) % 10);
    // **The tick, separately, with what the pool and the tick refused.** All
    // four of these were invisible on a console and all four are things that
    // cost a frame or lose behaviour when they are not zero: the tick catching
    // up after a stall, the pool holding blocks the GPU is still reading, the
    // scheduler's pool overflowing, and the notify cascade hitting its stack
    // budget.
    {
        const tick::TickWorld* t = world.worldTick();
        const i64 scheduleDrops = t != nullptr ? t->stats().scheduledDropped : 0;
        const i64 cascade = t != nullptr ? t->stats().notifyDeferred + t->stats().notifyDropped
                                             + t->stats().wireRefused
                                         : 0;
        row(r++, "  tick %2d.%d ms  held %u  drop %d/%d", int(shown_.tick),
            tenths(shown_.tick) % 10, unsigned(renderer.chunks().pool().stats().heldInFlight),
            int(scheduleDrops), int(cascade));

        // The relighter: what it still owes, what it has settled, and what it
        // had to throw away. `pend` sitting high means the per-frame budget is
        // below what the world is producing; `drop` above zero means a queue
        // overflowed and a patch of world is holding stale light.
        const world::LightUpdater* light = world.lighting();
        if (light != nullptr) {
            row(r++, "  light pend %4u  lit %8u  drop %u", unsigned(light->pending()),
                unsigned(light->stats().cellsSettled), unsigned(light->stats().dropped));
        }
    }

    // **The mobs**, which nothing else on this console reports. `path` is the
    // pathfinder's lifetime search count and `ex` the ones that hit the
    // 1,024-node budget -- `ex` climbing means the budget is shaping mob
    // movement rather than the world is. `pk` is the most searches any single
    // tick has run, which is the number the removed per-tick cap used to hold
    // at one. `hurt` is what a monster has cost the player so far.
    if (mobStats_.animals != 0 || mobStats_.monsters != 0 || mobStats_.hits != 0) {
        row(r++, "  mobs %3d + %3d  path %5u/%u pk %d", mobStats_.animals,
            mobStats_.monsters, mobStats_.searches, mobStats_.exhausted,
            mobStats_.peakSearches);
        if (mobStats_.hits != 0) {
            row(r++, "  hurt %4d over %d hit(s)", mobStats_.taken, mobStats_.hits);
        }
    }

    // **The spawner's own tally**, and it is here because "nothing spawns" was
    // reported twice and could not be told apart from "you have not found
    // one". `chunks` climbing at all means passes are running and columns are
    // resident; `floors` is how many of the drawn positions had somewhere to
    // stand, which is under two per hundred in any real world and is where
    // `az`'s whole yield goes; `spawn` is what survived the light and the box.
    if (mobStats_.chunksTried != 0) {
        row(r++, "  spawn %4d  floor %5u  chunk %6u", mobStats_.spawned, mobStats_.floors,
            mobStats_.chunksTried);
    }
    // The dungeon cages, which are the other spawner and have their own version
    // of the same report -- see MobStats.
    if (mobStats_.cages != 0) {
        row(r++, "  cage %4d  fire %5d  made %5d", mobStats_.cages, mobStats_.cagesFired,
            mobStats_.cagesSpawned);
    }

    blank(r++);
    row(r++, "drawn   %4d sections  %6lu quads", gpu.drawCalls,
        static_cast<unsigned long>(gpu.quads));
    // **The percentage is the one to watch.** It is the frame's share of a hard
    // per-frame budget that nothing can extend once it is spent, so a view
    // sitting near 100 is a view about to start losing geometry -- and the
    // thing to raise ctr::kCommandBufferBytes against. `drop` counts the
    // sections that budget cost the frame, and should read 0.
    //
    // `geo` is splits taken on purpose to drain the pipeline around the
    // geometry-shader cube pass, two per eye, and expected rather than a
    // warning. It reads 0 in the 4-vertex format. See Renderer::drawEye.
    const unsigned long cmdWords = static_cast<unsigned long>(gpu.commandWords);
    const unsigned long cmdTotal = static_cast<unsigned long>(kCommandBufferBytes / 4);
    const int cmdPercent = int((cmdWords * 100) / (cmdTotal != 0 ? cmdTotal : 1));
    if (gpu.droppedSections == 0) {
        row(r++, "cmd    %5luk %3d%%   geo %2d", cmdWords / 1024, cmdPercent, gpu.geoSplits);
    } else {
        row(r++, "cmd    %5luk %3d%%   \x1b[31mdrop %d\x1b[0m", cmdWords / 1024, cmdPercent,
            gpu.droppedSections);
    }
    // Y + d-pad moves these. The disparity is what a point at infinity gets at
    // full slider, in pixels of the 400 across the top screen; the focal
    // distance is what sits at the screen plane.
    row(r++, "3D      %2d.%d px inf   focus %3d blocks", int(renderer.stereoDisparityPixels()),
        tenths(renderer.stereoDisparityPixels()) % 10, int(renderer.stereoFocalBlocks()));
    // **The graph's half of the culling, which nothing reported before.**
    // docs/3ds-performance.md asks the overlay for "sections culled by frustum
    // vs by visibility graph" and only the frustum's half existed, so the
    // question "is the occlusion culling doing anything" could only be answered
    // by turning on wireframe and looking -- which answers a different question,
    // because wireframe's atlas discards its interiors and so writes no depth.
    //
    // No new counter is needed: the walk visits a section or it never arrives,
    // so the sections in range that it never reached are the ones the masks
    // stopped. Underground this is nearly all of them; on the surface under open
    // sky it is close to nothing, which is the measured result recorded in that
    // same document and worth being able to see.
    const render::SectionField& field = renderer.chunks().field();
    const int inRange = field.cellCount() * render::SectionField::kSectionsY;
    const int graphCut = inRange - frame.sectionsVisited;
    row(r++, "walked  %4d seen   %4d frustum-cut", frame.sectionsVisited,
        frame.rejectedByFrustum);
    row(r++, "        %4d graph-cut  %5d in range", graphCut < 0 ? 0 : graphCut,
        inRange);
    row(r++, "queued  %4d mesh   %2d done, %2d empty", frame.queued, streaming.meshedThisFrame,
        streaming.emptyThisFrame);

    blank(r++);
    row(r++, "pool %3lu.%lu MB  %4d slots  %5lu KB free",
        static_cast<unsigned long>(pool.resident / kMb),
        static_cast<unsigned long>((pool.resident % kMb) * 10 / kMb), pool.residents,
        static_cast<unsigned long>((pool.reserved - pool.resident) / kKb));
    row(r++, "  %6lu up  %6lu reused  %6lu evict", static_cast<unsigned long>(pool.uploads),
        static_cast<unsigned long>(pool.reused), static_cast<unsigned long>(pool.evictions));
    row(r++, "  %6lu alloc  %6lu refused", static_cast<unsigned long>(pool.allocations),
        static_cast<unsigned long>(pool.failures));

    blank(r++);
    row(r++, "cols  %4d in  %4d pending  %4d absent", streaming.columnsResident,
        streaming.pendingColumns, streaming.columnsMissing);

    // **Only while ground is being made**, and quiet the moment the player is
    // walking over chunks that already exist.
    //
    // `owed` and `ask` answer different questions and the gap between them is
    // the diagnosis: `owed` is columns in range that do not exist yet, `ask` is
    // cells whose group listing has not arrived, so the streamer does not yet
    // know whether the world already has them. Owed high with `ask` at zero is
    // a worker that cannot keep up, and the core named on the left is the first
    // thing to look at. `ask` high with `GATE` is the streamer waiting on the
    // card rather than on the generator -- ordinary for a moment after a
    // chunk-boundary crossing, a fault if it stays.
    //
    // `ms` should read 0: it is main-thread time, so anything above zero means
    // the worker did not start and generation is happening inside the frame,
    // which a player sees as the game stopping. `lost` must read 0 too -- a
    // non-zero one means the generator's cache was too small and columns came
    // back without their neighbours' population. See
    // ChunkGenerator::cacheColumnsFor.
    if (streaming.pendingGeneration > 0 || streaming.generatedThisFrame > 0) {
        row(r++, "gen %-5s owed %4d  ask %4d  done %2d",
            streaming.workerRunning ? workerCore_ : "MAIN", streaming.pendingGeneration,
            streaming.unclassified, streaming.generatedThisFrame);
        // The tail of the row is whichever of the two things is wrong, and
        // `fail` wins because it is the worse one: a sweep that cannot finish
        // means the frontier never advances again, where a gate clears itself
        // as soon as a directory listing lands.
        //
        // **Which failure it is, in one letter.** `L` is a sweep whose 3x3
        // never became final and `C` is one the generator's cache could not
        // hold; they used to be one number, reported as the cache, and the time
        // it actually fired it was `L`. A page that names the wrong cause is
        // worse than one that names none.
        char tail[16] = {};
        if (streaming.generationFailures != 0) {
            // Clamped to two digits: the row is 40 columns and the count runs
            // into the thousands within seconds of this firing. Whether it is
            // 99 or 4,897 changes nothing a reader would do about it.
            const u32 failures = streaming.generationFailures > 99u
                                     ? 99u
                                     : streaming.generationFailures;
            std::snprintf(tail, sizeof(tail), " fail%c%2lu",
                          streaming.generationUnlightable >= streaming.generationIncomplete ? 'L'
                                                                                            : 'C',
                          static_cast<unsigned long>(failures));
        } else if (streaming.generationGated) {
            std::snprintf(tail, sizeof(tail), " GATE");
        }
        // `live` is the generator's high-water mark of columns it is still
        // being written into, `ret` what it has let go of behind the player and
        // `lost` what it dropped without meaning to. **`ret` rising is the
        // healthy case and `lost` above zero is a corrupted world** -- see
        // ChunkGenerator::retire. Both are clamped to the width they have; what
        // matters is zero against not-zero.
        row(r++, "  %2d.%dms live%3lu ret%4lu lost%2lu%s",
            int(streaming.generateMicros / 1000), int((streaming.generateMicros % 1000) / 100),
            static_cast<unsigned long>(streaming.generatorPeakLive),
            static_cast<unsigned long>(streaming.generatorRetiredLive > 9999u
                                           ? 9999u
                                           : streaming.generatorRetiredLive),
            static_cast<unsigned long>(streaming.generatorEvictedLive > 99u
                                           ? 99u
                                           : streaming.generatorEvictedLive),
            tail);
    }
    // **What the memory budget is doing**, and silent while it is doing
    // nothing. `admit` equal to the render distance's load radius is the
    // healthy reading; below it the world in view is shorter than the setting
    // asks for, and the alternative was running the heap out and dropping to
    // the HOME menu. See WorldStreamer::setMemoryBudget and crashlogs/007.
    if (streaming.admitRadius < world.loadRadius()) {
        row(r++, "  \x1b[33madmit %2d of %2d rings  evicted %lu\x1b[0m", streaming.admitRadius,
            world.loadRadius(), static_cast<unsigned long>(streaming.evictedForMemory));
    }
    row(r++, "  blocks %3lu.%lu MB in the heap",
        static_cast<unsigned long>(streaming.blockBytes / kMb),
        static_cast<unsigned long>((streaming.blockBytes % kMb) * 10 / kMb));
    // **Three heaps, and the application one was missing.** `crashlogs/005`
    // ended with three dumps whose own stack pointers were unmapped -- the
    // shape of a process that has run out of memory rather than of any
    // particular line -- and its notes named the number that would settle it:
    // the free heap at the moment before. It was not on this page, so the next
    // report could not carry it either. It is here now.
    //
    // `heap` is what malloc has not handed out and cannot see fragmentation, so
    // it is a ceiling rather than a promise; see heap.hpp. It costs a
    // `mallinfo` and the malloc lock with it, which the generation worker also
    // takes -- a debug page's price, and the same one ChunkCache already pays
    // every time it sizes what may be owed to the card.
    row(r++, "free KB heap %5lu lin %5lu vram %4lu",
        static_cast<unsigned long>(heapFreeBytes() / kKb),
        static_cast<unsigned long>(renderer.freeLinearBytes() / kKb),
        static_cast<unsigned long>(renderer.freeVramBytes() / kKb));
    // **The number that found the map's patch cache**, and the reason it is
    // here rather than on the map: it is a cost to be watched, and everything
    // else on this page is too. A redraw happens when the player crosses a
    // block or turns far enough to move the marker, so a figure near 700 is the
    // copy and a figure in the thousands is every patch being redrawn -- a
    // texture pack change, or the grid above being toggled.
    //
    // **And what the card is filling in beside it.** The map reaches past the
    // render distance, so the band the grid can never answer for is read off
    // the card a chunk at a time at the lowest priority there is; `fill` is how
    // many chunks are still waiting to be asked about and how many have come
    // back with ground in them. See map_screen.hpp.
    //
    // **After the plus, the bottom screen's copy** -- the whole picture moved
    // from off-screen onto the framebuffer, which every redraw now pays on top
    // of drawing it. See bottom_screen.hpp.
    row(r++, "map %5lu+%4lu us fill %4d/%5lu",
        static_cast<unsigned long>(map_.lastDrawMicros()),
        static_cast<unsigned long>(bottom::lastCopyMicros()), map_.fillWaiting(),
        static_cast<unsigned long>(map_.filled()));

    blank(r++);
    // **Two rows, because the Far Lands are the point.** x reaches 12,550,824
    // and its chunk 784,426, and a single row wide enough for both plus labels
    // is 50 characters. The one place these numbers matter most is the one
    // place they would have been clipped.
    row(r++, "xyz %8d %4d %8d", int(camera.x), int(camera.y), int(camera.z));
    row(r++, "chunk %6d %6d   time %2d:%02d", int(camera.chunkX()), int(camera.chunkZ()),
        int(timeOfDay * 24.0f) % 24, int(timeOfDay * 1440.0f) % 60);
    return r;
}

// What the card is doing, and who is waiting for it.
//
// The page exists because the fix it reports on is invisible from every other
// one: a chunk read costs the same microseconds wherever it happens, and the
// only thing that changed is which thread pays them. So the first row is the
// answer -- `main` is main-thread time inside a storage call, and **on this
// build it is expected to be 0.0 always**, not merely usually.
//
// It used to climb whenever the player moved, which is the report this row
// earned its place on: a cell whose directory listing had not arrived fell back
// to a `stat`, that `stat` waits for the storage lock, and the I/O thread holds
// the lock for a whole chunk write. A row of freshly exposed cells behind a
// flush is a second or two of frozen game. Nothing on the render thread asks
// storage anything now -- an unlisted cell waits for its listing instead -- so
// any number above zero here is a fault to chase, not a busy moment.
//
// `hit` is the second thing to read. It counts columns served without an SD
// operation at all -- retained after leaving the grid, or read ahead of the
// player -- and `pre` is how many of those the read-ahead band earned rather
// than retention. A low `pre` with plenty of `hit` means the band is memory
// spent for nothing and can go to zero; both low means the cap is too small for
// the render distance.
int Overlay::drawStorage(const render::WorldStreamer& world)
{
    const render::WorldStreamer::Stats& streaming = world.stats();
    const world::ChunkCache::Stats& io = streaming.io;

    constexpr int kKb = 1024;
    constexpr int kMb = 1024 * 1024;

    int r = kBodyRow;
    // **The first rows are `now`, not `ever`.** `d` is the delta over the last
    // sample block -- two thirds of a second at 30 fps -- and it is the one
    // that answers whether the card is on the render thread *at the moment*.
    // The totals beside them are history: opening a world stats a few hundred
    // chunks before its directory listings land, and that cost then sits in the
    // total for the rest of the session whether or not anything is still wrong.
    // Read as a total this line said 4000 ms on a session that felt perfectly
    // smooth, which is exactly the misreading the split fixes.
    const world::ChunkCache::Stats& d = ioDelta_;

    row(r++, "main %2d.%d ms now          %s",
        int(d.mainThreadMicros / 1000), int((d.mainThreadMicros % 1000) / 100),
        io.workerRunning ? "io thread" : "[31mNO THREAD[0m");
    row(r++, "  %4lu stats now", static_cast<unsigned long>(d.stats));
    row(r++, "  %6lu ms, %6lu stats all session",
        static_cast<unsigned long>(io.mainThreadMicros / 1000),
        static_cast<unsigned long>(io.stats));

    blank(r++);
    row(r++, "ops  %4lu rd %4lu wr %4lu ls  now",
        static_cast<unsigned long>(d.reads), static_cast<unsigned long>(d.writes),
        static_cast<unsigned long>(d.listings));
    row(r++, "     %5lu read %5lu write all", static_cast<unsigned long>(io.reads),
        static_cast<unsigned long>(io.writes));
    row(r++, "queue %4lu rd %4lu wr %4lu ls",
        static_cast<unsigned long>(io.readsQueued), static_cast<unsigned long>(io.writesQueued),
        static_cast<unsigned long>(io.groupsQueued));
    row(r++, "  %4d columns still being read", streaming.pendingReads);

    blank(r++);
    const unsigned long asked = static_cast<unsigned long>(io.hits) + io.misses;
    const unsigned long askedNow = static_cast<unsigned long>(d.hits) + d.misses;
    row(r++, "hit  %4lu miss %4lu  %3lu%%  now", static_cast<unsigned long>(d.hits),
        static_cast<unsigned long>(d.misses),
        askedNow != 0 ? static_cast<unsigned long>(d.hits) * 100 / askedNow : 0);
    row(r++, "     %5lu     %5lu  %3lu%%  all", static_cast<unsigned long>(io.hits),
        static_cast<unsigned long>(io.misses),
        asked != 0 ? static_cast<unsigned long>(io.hits) * 100 / asked : 0);
    row(r++, "  %5lu read ahead and used",
        static_cast<unsigned long>(io.prefetchHits));

    blank(r++);
    row(r++, "cache %3lu.%lu MB  %4lu cols  %5lu evict",
        static_cast<unsigned long>(io.cleanBytes / kMb),
        static_cast<unsigned long>((io.cleanBytes % kMb) * 10 / kMb),
        static_cast<unsigned long>(io.cleanColumns), static_cast<unsigned long>(io.evicted));
    // Owed to the card. It is bounded by the cache's own dirty cap rather than
    // by the autosave timer, so a number that sits near the cap means the I/O
    // thread is being outrun and the generation worker is paying for writes
    // itself -- which is the back-pressure working, not a fault.
    //
    // **The cap is printed because it moves.** It follows the free heap between
    // a floor and a ceiling, so the same dirty figure can be comfortable on one
    // frame and about to cost the worker a write on another; without the
    // denominator the two look identical. See ChunkCache::dirtyCapLocked.
    row(r++, "dirty %4lu KB of %4lu  %4lu cols",
        static_cast<unsigned long>(io.dirtyBytes / kKb),
        static_cast<unsigned long>(io.dirtyCapBytes / kKb),
        static_cast<unsigned long>(io.dirtyColumns));

    blank(r++);
    row(r++, "autosave %s", world.autosaveSeconds() > 0 ? "on" : "off");

    // **Audio, and specifically the numbers a host cannot answer.**
    //
    // The decode thread runs below the main thread, on core 0 on an Old 3DS
    // because a 3DSX has nowhere else to put it, and whether a ring deep enough
    // makes that safe is the one thing in the subsystem only hardware can
    // settle. It did not: the music skipped under load, which is what the boost
    // in ctr/audio.hpp exists for.
    //
    // So four numbers, and each answers a different question. `decode` is what
    // one 23 ms buffer costs this console -- the figure the thread policy has to
    // be argued from. `low` is the fewest buffers the DSP held at any point in
    // this track: comfortably above kBoostBelow means the ring was never tested.
    // `boost` is how often the decoder had to outrank the frame loop to stay
    // ahead, and `under` is it having failed anyway -- a gap the player heard.
    blank(r++);
    if (audio_ == nullptr) {
        row(r++, "audio  not built");
    } else {
        switch (audio_->status()) {
        case AudioStatus::Ready:
            row(r++, "audio  on   %s", audio_->musicPlaying() ? "playing" : "quiet");
            row(r++, "  decode %4lu us / buffer",
                static_cast<unsigned long>(audio_->decodeMicros()));
            row(r++, "  ring   %2d of %2d low  boost %4lu", audio_->ringLow(),
                kRingBuffers, static_cast<unsigned long>(audio_->boosts()));
            row(r++, "  under  %4lu", static_cast<unsigned long>(audio_->underruns()));
            break;
        case AudioStatus::NoFirmware:
            row(r++, "audio  no dspfirm.cdc");
            break;
        case AudioStatus::Unavailable:
            row(r++, "audio  DSP unavailable");
            break;
        case AudioStatus::Disabled:
            row(r++, "audio  off");
            break;
        }
    }
    return r;
}

int Overlay::drawSettings(const Renderer& renderer, const DebugSettings& settings,
                          const Camera& camera)
{
    const char* cursor[kSettingCount] = {"  ", "  ", "  ", "  ", "  "};
    cursor[cursor_] = "\x1b[33m> \x1b[0m";

    int r = kBodyRow;
    row(r++, "debug settings");
    blank(r++);
    row(r++, "%srender distance   %2d chunks", cursor[0], settings.renderDistance);
    // The stall count is on this row rather than a line of its own because it
    // is only ever about this setting: the watchdog only runs under the quad
    // format, and a non-zero count means the renderer took the setting away
    // again. Silent when it is zero, which is every healthy session.
    if (renderer.gpuStalls() == 0) {
        row(r++, "%scube format       %s", cursor[1],
            renderer.cubeFormat() == mesh::CubeFormat::Quads ? "geoshader" : "4-vertex ");
    } else {
        // **What the frame that never came back had in it**, which is the
        // whole reason the watchdog gives up rather than blocking: these four
        // numbers are what turns "the quad path hangs" into a bisect that has
        // somewhere to start. `spl` above zero says a command list was split
        // mid-frame; `3d` says the second eye was in it.
        const Renderer::FrameStats& st = renderer.stalledStats();
        row(r++, "%scube format       %s \x1b[31mGPU STALL\x1b[0m", cursor[1],
            renderer.cubeFormat() == mesh::CubeFormat::Quads ? "geoshader" : "4-vertex ");
        row(r++, "   wedged on %d draws %lu quads", st.drawCalls, (unsigned long)(st.quads));
        row(r++, "   cmd %luk/%d  stale %d  clamp %d  3d %s",
            static_cast<unsigned long>(st.commandWords) / 1024, st.geoSplits, st.staleSkipped,
            st.clampedDraws, st.stereo ? "on" : "off");
        if (renderer.geoRampName() != nullptr) {
            row(r++, "   at ramp step [%s]", renderer.geoRampName());
        }
    }
    row(r++, "%sgreedy meshing    %s", cursor[2], settings.greedyMeshing ? "on " : "off");
    row(r++, "%swireframe         %s", cursor[3], renderer.wireframe() ? "on " : "off");
    // **The map's grids are not on this page any more.** They were here on the
    // reasoning that "is the map aligned with the chunks" is a maintainer's
    // question -- true, and beside the point, because a chunk grid is also the
    // most useful thing a map can draw for a player. They are under the d-pad
    // on the map's own page now; see map_screen.hpp.
    //
    // The row doubles as the readout: after a teleport it shows where you
    // landed, which is the only confirmation the player needs and costs no
    // extra state to keep. Integers because newlib's printf here has no float
    // support -- see the note on tenths().
    row(r++, "%steleport          %d %d %d", cursor[4], int(std::floor(camera.x)),
        int(std::floor(camera.y)), int(std::floor(camera.z)));
    blank(r++);
    row(r++, "d-pad up/down choose, l/r change");
    // Every setting here is honest about what it costs, because all three are
    // easy to leave switched on and then wonder at the numbers on the Info
    // page. The page is 30 rows and the footer is pinned at 28, so this text
    // has to earn its lines -- rows 4..27 is the whole budget, and the setting
    // whose question is already answered gives up its share to the one whose
    // is not.
    row(r++, "range %d-%d, the hardware's limit,", settings.minDistance, settings.maxDistance);
    row(r++, "not the player's: menu offers %d/%d.", kPlayMaxDistanceOld3DS,
        kPlayMaxDistanceNew3DS);
    row(r++, "Past ~20 the heap runs out. Changing");
    row(r++, "it rebuilds the pool; columns stay.");
    blank(r++);
    // The live questions on this page, so they get the room. Says what to
    // look at, because the number that decides them is not on this screen.
    // One line shorter than when it spoke for the cube format alone, which is
    // what paid for the greedy-meshing row.
    row(r++, "Cube format and greedy both re-mesh:");
    row(r++, "wait, then compare GPU draw and quads");
    row(r++, "on Info. Geoshader (8 bytes a quad,");
    row(r++, "not 60) and greedy on are default.");
    // The number the comparison is against, on the screen where the comparison
    // is made. 30 fps is 33.3 ms a frame, less the ~0.8 ms fixed cost.
    row(r++, "Gate: 30 fps at %d, 3D on, so GPU", kGateDistanceNew3DS);
    row(r++, "draw under 32 ms both eyes.");
    blank(r++);
    row(r++, "Wireframe is a texture, not lines.");
    // **Wireframe gave up a line of explanation for these.** The
    // budget is rows 4..27 and it was already exactly full, so the teleport row
    // and its hint had to come from somewhere. Wireframe was the cheapest to
    // shorten: you switch it on and the screen tells you what it does, which is
    // not true of either of the other two settings or of this one -- nothing on
    // the console would ever hint that the interesting coordinate is 12,550,824.
    row(r++, "Teleport: type x y z. The Far Lands");
    row(r++, "are at 12550824 70 0.");
    return r;
}

}  // namespace mc::ctr
