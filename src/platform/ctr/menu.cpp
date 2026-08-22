#include "platform/ctr/menu.hpp"

#include "platform/ctr/overlay.hpp"
#include "platform/ctr/renderer.hpp"

#include "core/util/java_random.hpp"
#include "core/util/seed_text.hpp"

#include "version_config.hpp"
#include "version_slots.hpp"

#include <3ds.h>

#include <cstdio>
#include <cstring>
#include <ctime>

namespace mc::ctr {

namespace {

constexpr float kScreenWidth = 400.0f;
constexpr float kScreenHeight = 240.0f;

// The palette. Ours, not Mojang's -- see the note at the top of menu.hpp.
constexpr u32 kInk = C2D_Color32(0xF0, 0xF0, 0xF0, 0xFF);
constexpr u32 kInkDim = C2D_Color32(0xA0, 0xA0, 0xA0, 0xFF);
constexpr u32 kInkWarn = C2D_Color32(0xFF, 0xA8, 0x60, 0xFF);
constexpr u32 kShadow = C2D_Color32(0x14, 0x14, 0x14, 0xC0);
constexpr u32 kOutline = C2D_Color32(0x14, 0x14, 0x14, 0xFF);
constexpr u32 kFill = C2D_Color32(0x6A, 0x6A, 0x6A, 0xFF);
constexpr u32 kFillSelected = C2D_Color32(0x7A, 0x85, 0xA3, 0xFF);
constexpr u32 kFillDisabled = C2D_Color32(0x4A, 0x4A, 0x4A, 0xFF);
constexpr u32 kBevelLight = C2D_Color32(0xFF, 0xFF, 0xFF, 0x30);
constexpr u32 kBevelDark = C2D_Color32(0x00, 0x00, 0x00, 0x40);

// Row and button geometry, in one place so the two list screens line up.
constexpr float kButtonWidth = 220.0f;
constexpr float kButtonHeight = 26.0f;
constexpr float kRowHeight = 28.0f;
constexpr float kRowGap = 4.0f;
constexpr float kRowsTop = 36.0f;
constexpr int kVisibleRows = 6;

// Anything above this and a2 is `- empty -` on a1.1.2's own screen; here it is
// only the point at which the list scrolls.
int rowCount(usize worlds)
{
    return int(worlds) + 1;  // + "Create New World"
}

// A per-tile shade for the backdrop. Deterministic, so the menu does not
// shimmer between frames.
u32 tileHash(int x, int y)
{
    u32 h = u32(x) * 73856093u ^ u32(y) * 19349663u;
    h ^= h >> 13;
    h *= 0x9E3779B1u;
    return h ^ (h >> 16);
}

u8 clampByte(int value)
{
    return u8(value < 0 ? 0 : (value > 255 ? 255 : value));
}

void formatWhen(i64 lastPlayed, char* out, usize size)
{
    if (lastPlayed <= 0) {
        std::snprintf(out, size, "never played");
        return;
    }
    const std::time_t seconds = std::time_t(lastPlayed / 1000);
    std::tm parts{};
    if (::gmtime_r(&seconds, &parts) == nullptr) {
        std::snprintf(out, size, "-");
        return;
    }
    // UTC, and deliberately unlabelled: the 3DS has a time zone in its system
    // config and newlib on this toolchain does not read it, so a local time
    // here would be a lie on every console east or west of Greenwich.
    std::snprintf(out, size, "%04d-%02d-%02d %02d:%02d", parts.tm_year + 1900,
                  parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min);
}

// swkbd's filter hook for the world name. The names already on the card are
// reached through a file-static because the applet API has one user pointer and
// libctru's own callback signature takes it -- see swkbdSetFilterCallback --
// and the alternative is letting the player type a name that then silently
// fails to create.
const std::vector<world::WorldEntry>* gExistingWorlds = nullptr;

SwkbdCallbackResult validateWorldName(void* user, const char** message, const char* text,
                                      size_t length)
{
    (void)user;

    char buffer[80];
    if (length >= sizeof(buffer)) {
        *message = "that name is too long";
        return SWKBD_CALLBACK_CONTINUE;
    }
    std::memcpy(buffer, text, length);
    buffer[length] = '\0';

    std::string name;
    if (!world::sanitizeWorldName(buffer, &name)) {
        *message = "that name has nothing a card can store";
        return SWKBD_CALLBACK_CONTINUE;
    }
    if (gExistingWorlds != nullptr) {
        for (const world::WorldEntry& entry : *gExistingWorlds) {
            if (entry.name == name) {
                // Storage::create refuses to write over a level.dat, so this
                // would fail anyway -- it just fails here, where the player can
                // fix it, instead of after the keyboard has closed.
                *message = "there is already a world with that name";
                return SWKBD_CALLBACK_CONTINUE;
            }
        }
    }
    return SWKBD_CALLBACK_OK;
}

// The console's clock, folded into something an LCG can be seeded with. The
// system tick is 268 MHz, so two worlds made a second apart are nowhere near
// each other in the sequence.
i64 clockSeed()
{
    return i64(svcGetSystemTick()) ^ (i64(osGetTime()) << 20);
}

}  // namespace

i64 nowMillis()
{
    return i64(osGetTime()) - 2208988800000LL;
}

bool Menu::init(bool isNew3DS)
{
    isNew3DS_ = isNew3DS;
    maxDistance_ = isNew3DS ? kPlayMaxDistanceNew3DS : kPlayMaxDistanceOld3DS;
    if (renderDistance_ == 0) {
        // The two configurations the VBO pool was measured against, which is
        // what makes them the defaults rather than the maxima above.
        renderDistance_ = isNew3DS ? 10 : 6;
    }

    // **1024 objects, and the number is arithmetic rather than taste.** citro2d
    // sizes its vertex buffer from this and silently drops geometry once it is
    // full, so it has to cover the busiest frame: 240 backdrop tiles, six rows
    // at five quads each, and a few hundred glyphs. It is 128 KB of linear
    // memory, which is why this is not simply C2D_DEFAULT_MAX_OBJECTS -- the
    // menu gives it back before the renderer asks for its pool.
    if (!C2D_Init(1024)) {
        return false;
    }
    C2D_Prepare();

    // Both failures unwind through shutdown() rather than by hand, so the
    // shader-park rule below is obeyed on the way out of a half-built menu too.
    // Every step of it is null-safe and C2D_Fini is a no-op when citro2d is not
    // up, so it is safe to call at any point after C2D_Init succeeded.
    textBuf_ = C2D_TextBufNew(1024);
    if (textBuf_ == nullptr) {
        shutdown();
        return false;
    }

    target_ = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    if (target_ == nullptr) {
        shutdown();
        return false;
    }

    // The left eye only. Nothing here has any depth to it, and the right eye's
    // framebuffer would otherwise hold whatever the last game frame left in it.
    gfxSet3D(false);

    refreshWorlds();
    consoleDirty_ = true;
    return true;
}

void Menu::shutdown()
{
    // **Before C2D_Fini, which frees citro2d's shader program.** citro3d holds
    // the last program bound and dereferences it on the next bind, so without
    // this the game's first frame reads through freed memory and data-aborts --
    // which is precisely what `crashlogs/004-loading-a-world-from-the-menu` is.
    // See parkShaderProgram in renderer.hpp.
    parkShaderProgram();

    if (target_ != nullptr) {
        C3D_RenderTargetDelete(target_);
        target_ = nullptr;
    }
    if (textBuf_ != nullptr) {
        C2D_TextBufDelete(textBuf_);
        textBuf_ = nullptr;
    }
    C2D_Fini();
}

void Menu::refreshWorlds()
{
    world::listWorlds(fs_, kSavesDir, &worlds_);

    const int rows = rowCount(worlds_.size());
    if (worldCursor_ >= rows) {
        worldCursor_ = rows - 1;
    }
    if (worldCursor_ < 0) {
        worldCursor_ = 0;
    }
    if (worldScroll_ > worldCursor_) {
        worldScroll_ = worldCursor_;
    }
}

void Menu::setScreen(Screen screen)
{
    screen_ = screen;
    consoleDirty_ = true;
}

void Menu::printConsoleHelp()
{
    if (!consoleDirty_ && printedScreen_ == screen_) {
        return;
    }
    printedScreen_ = screen_;
    consoleDirty_ = false;

    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("\x1b[32m3DAlpha %s\x1b[0m\n\n", mcver::kDisplay);

    switch (screen_) {
    case Screen::Title:
        std::printf("Up/Down  choose\n");
        std::printf("A        select\n");
        std::printf("START    exit to the home menu\n\n");
        std::printf("Multiplayer arrives at M5.\n");
        break;
    case Screen::Worlds:
        std::printf("Up/Down  choose a world\n");
        std::printf("A        play it\n");
        std::printf("X        delete it\n");
        std::printf("B        back\n\n");
        std::printf("Worlds live on the card at:\n");
        std::printf("  \x1b[33m%s/\x1b[0m\n\n", kSavesDir);
        if (worlds_.empty()) {
            std::printf("There are none yet. Make one, or\n");
            std::printf("copy an Alpha save into that folder\n");
            std::printf("-- a level.dat and the base36 chunk\n");
            std::printf("folders beside it.\n");
        }
        break;
    case Screen::Options:
        std::printf("Left/Right  change the value\n");
        std::printf("Up/Down     choose a row\n");
        std::printf("B           back\n\n");
        std::printf("Render distance is what a player is\n");
        std::printf("offered; the debug page (SELECT+Y in\n");
        std::printf("game) goes further for measuring.\n");
        break;
    case Screen::ConfirmDelete:
        std::printf("\x1b[31mDeleting a world cannot be undone.\x1b[0m\n\n");
        std::printf("A  delete it\n");
        std::printf("B  keep it\n");
        break;
    }

    if (message_ != nullptr) {
        std::printf("\n\x1b[31m%s\x1b[0m\n", message_);
    }
}

MenuChoice Menu::run()
{
    MenuChoice choice;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();

        bool done = false;
        switch (screen_) {
        case Screen::Title:
            done = handleTitle(down, &choice);
            break;
        case Screen::Worlds:
            done = handleWorlds(down, &choice);
            break;
        case Screen::Options:
            handleOptions(down);
            break;
        case Screen::ConfirmDelete:
            handleConfirmDelete(down);
            break;
        }
        if (done) {
            choice.renderDistance = renderDistance_;
            return choice;
        }

        printConsoleHelp();
        drawFrame();
    }

    // aptMainLoop said no: the system is taking the application away, and the
    // only honest answer is to stop rather than to open a world.
    choice.action = MenuChoice::Action::Quit;
    choice.renderDistance = renderDistance_;
    return choice;
}

namespace {

// The circle pad reports through the same button mask as the d-pad, so both
// work everywhere without a second code path.
constexpr u32 kUp = KEY_DUP | KEY_CPAD_UP;
constexpr u32 kDown = KEY_DDOWN | KEY_CPAD_DOWN;
constexpr u32 kLeft = KEY_DLEFT | KEY_CPAD_LEFT;
constexpr u32 kRight = KEY_DRIGHT | KEY_CPAD_RIGHT;

int step(u32 down, int cursor, int count)
{
    if (down & kUp) {
        cursor = cursor == 0 ? count - 1 : cursor - 1;
    }
    if (down & kDown) {
        cursor = cursor + 1 >= count ? 0 : cursor + 1;
    }
    return cursor;
}

}  // namespace

bool Menu::handleTitle(u32 down, MenuChoice* choice)
{
    constexpr int kRows = 4;  // singleplayer, multiplayer, options, quit
    titleCursor_ = step(down, titleCursor_, kRows);

    if (down & KEY_START) {
        choice->action = MenuChoice::Action::Quit;
        return true;
    }
    if ((down & KEY_A) == 0) {
        return false;
    }

    switch (titleCursor_) {
    case 0:
        message_ = nullptr;
        refreshWorlds();
        setScreen(Screen::Worlds);
        break;
    case 1:
        // Multiplayer is drawn and does nothing on purpose: the protocol work
        // is M5, and a button that is missing reads as an oversight while one
        // that is greyed out reads as a plan.
        break;
    case 2:
        setScreen(Screen::Options);
        break;
    default:
        choice->action = MenuChoice::Action::Quit;
        return true;
    }
    return false;
}

bool Menu::handleWorlds(u32 down, MenuChoice* choice)
{
    const int rows = rowCount(worlds_.size());
    worldCursor_ = step(down, worldCursor_, rows);

    // Keep the cursor on screen, scrolling by the smallest amount that does it.
    if (worldCursor_ < worldScroll_) {
        worldScroll_ = worldCursor_;
    }
    if (worldCursor_ >= worldScroll_ + kVisibleRows) {
        worldScroll_ = worldCursor_ - kVisibleRows + 1;
    }

    if (down & KEY_B) {
        message_ = nullptr;
        setScreen(Screen::Title);
        return false;
    }

    if ((down & KEY_X) != 0 && worldCursor_ > 0) {
        setScreen(Screen::ConfirmDelete);
        return false;
    }

    if ((down & KEY_A) == 0) {
        return false;
    }

    if (worldCursor_ == 0) {
        std::string name;
        if (!askWorldName(&name)) {
            return false;
        }
        i64 seed = 0;
        if (!askSeed(&seed)) {
            return false;
        }
        return createWorld(name, seed, choice);
    }

    const world::WorldEntry& entry = worlds_[usize(worldCursor_ - 1)];
    choice->action = MenuChoice::Action::Play;
    choice->worldPath = entry.path;
    choice->worldName = entry.name;
    return true;
}

void Menu::handleOptions(u32 down)
{
    constexpr int kRows = 2;  // render distance, back
    optionsCursor_ = step(down, optionsCursor_, kRows);

    if (optionsCursor_ == 0) {
        if ((down & kLeft) != 0 && renderDistance_ > 2) {
            --renderDistance_;
        }
        if ((down & kRight) != 0 && renderDistance_ < maxDistance_) {
            ++renderDistance_;
        }
    }

    if ((down & KEY_B) != 0 || ((down & KEY_A) != 0 && optionsCursor_ == 1)) {
        setScreen(Screen::Title);
    }
}

void Menu::handleConfirmDelete(u32 down)
{
    if (down & KEY_B) {
        setScreen(Screen::Worlds);
        return;
    }
    if ((down & KEY_A) == 0) {
        return;
    }

    // The cursor cannot be on the create row here -- ConfirmDelete is only
    // reachable from a world row -- but the list is re-read on every refresh,
    // so the index is bounds-checked rather than trusted.
    if (worldCursor_ > 0 && usize(worldCursor_ - 1) < worlds_.size()) {
        const std::string path = worlds_[usize(worldCursor_ - 1)].path;
        message_ = world::deleteWorld(fs_, path) ? nullptr : "could not delete that world";
    }
    refreshWorlds();
    setScreen(Screen::Worlds);
}

bool Menu::askWorldName(std::string* out)
{
    constexpr int kMaxText = 64;

    char initial[kMaxText];
    const std::string suggestion = world::defaultWorldName(worlds_);
    std::snprintf(initial, sizeof(initial), "%s", suggestion.c_str());

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetInitialText(&swkbd, initial);
    swkbdSetHintText(&swkbd, "World name");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, SWKBD_FILTER_CALLBACK, 0);
    gExistingWorlds = &worlds_;
    swkbdSetFilterCallback(&swkbd, validateWorldName, nullptr);

    char text[kMaxText];
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));
    gExistingWorlds = nullptr;

    // The applet owned both screens; libctru's console caches a framebuffer
    // address at consoleInit and never looks it up again, so it has to be told
    // again. Same reason, and same safety argument, as
    // Overlay::teleportViaKeyboard.
    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }
    // Sanitised again rather than smuggled out of the filter: the player can
    // still edit after the last time it ran.
    return world::sanitizeWorldName(text, out);
}

bool Menu::askSeed(i64* out)
{
    constexpr int kMaxText = 64;

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, kMaxText - 1);
    swkbdSetHintText(&swkbd, "Seed -- leave blank for a random one");
    swkbdSetFeatures(&swkbd, SWKBD_DARKEN_TOP_SCREEN);
    // No validation: **blank is a valid answer here** and means "roll one",
    // which is what a1.1.2 does every time because it never asks at all.
    swkbdSetValidation(&swkbd, SWKBD_ANYTHING, 0, 0);

    char text[kMaxText];
    const SwkbdButton pressed = swkbdInputText(&swkbd, text, sizeof(text));

    consoleInit(GFX_BOTTOM, nullptr);
    consoleDirty_ = true;
    C2D_Prepare();

    if (pressed != SWKBD_BUTTON_CONFIRM) {
        return false;
    }

    if (!seedFromText(text, out)) {
        // a1.1.2's own answer: `new World(File, String)` seeds itself with
        // `new Random().nextLong()`. Java's no-argument Random draws from a
        // process-wide uniquifier mixed with nanoTime, which is not something a
        // console can reproduce and not something worth reproducing -- what
        // matters is that it is a nextLong off a clock-seeded LCG.
        JavaRandom random(clockSeed());
        *out = random.nextLong();
    }
    return true;
}

bool Menu::createWorld(const std::string& name, i64 seed, MenuChoice* choice)
{
    if (!fs_.makeDirectories(kSavesDir)) {
        message_ = "could not make the saves folder on the card";
        consoleDirty_ = true;
        return false;
    }

    const std::string path = world::worldPath(kSavesDir, name);
    const i64 now = nowMillis();

    mcver::Storage storage(fs_);
    if (storage.create(path, seed, now) != world::OpenResult::Ok) {
        message_ = "could not create the world -- is the card full or locked?";
        consoleDirty_ = true;
        return false;
    }

    // **SnowCovered is rolled here because a1.1.2 rolls it here.** In `cn`'s
    // constructor, the branch taken when there is no level.dat reads
    // `this.snowCovered = this.rand.nextInt(4) == 0`, and `rand` is the World's
    // **unseeded** `new Random()` -- so it is a one-in-four coin flip at
    // creation, not a function of the seed, and it is then persisted and never
    // rolled again. The generator reads it: it puts ice at sea level - 1 across
    // every ocean. Storage::create cannot do this itself because core has no
    // clock, and the harnesses want it off and deterministic.
    JavaRandom random(clockSeed());
    storage.level().snowCovered = random.nextInt(4) == 0;
    const bool saved = storage.saveLevel();
    storage.close(now);
    if (!saved) {
        message_ = "could not write level.dat";
        consoleDirty_ = true;
        return false;
    }

    message_ = nullptr;
    choice->action = MenuChoice::Action::Play;
    choice->worldPath = path;
    choice->worldName = name;
    choice->created = true;
    return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void Menu::drawFrame()
{
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(textBuf_);
    C2D_TargetClear(target_, C2D_Color32(0x18, 0x14, 0x10, 0xFF));
    C2D_SceneBegin(target_);

    drawBackground();
    switch (screen_) {
    case Screen::Title:
        drawTitle();
        break;
    case Screen::Worlds:
        drawWorlds();
        break;
    case Screen::Options:
        drawOptions();
        break;
    case Screen::ConfirmDelete:
        drawConfirmDelete();
        break;
    }

    C3D_FrameEnd(0);
}

void Menu::drawBackground()
{
    // a1.1.2's own menu backdrop is the dirt tile drawn dark and tiled
    // (`GuiScreen` multiplies it by 0x404040). We have no dirt tile and will
    // not ship one, so each tile is a flat colour with a stable per-tile shade
    // -- the same reasoning as the placeholder atlas, and the same replacement
    // path when a real pack lands.
    constexpr float kTile = 20.0f;
    constexpr int kCols = int(kScreenWidth / kTile);
    constexpr int kRows = int(kScreenHeight / kTile);

    for (int y = 0; y < kRows; ++y) {
        for (int x = 0; x < kCols; ++x) {
            const int shade = int(tileHash(x, y) & 15u) - 7;
            const u32 colour = C2D_Color32(clampByte(70 + shade), clampByte(52 + shade),
                                           clampByte(37 + shade), 0xFF);
            C2D_DrawRectSolid(float(x) * kTile, float(y) * kTile, 0.0f, kTile, kTile, colour);
        }
    }
}

void Menu::drawTitle()
{
    drawLabelCentered("3DAlpha", kScreenWidth * 0.5f, 36.0f, 1.4f, kInk, true);
    drawLabelCentered(mcver::kDisplay, kScreenWidth * 0.5f, 72.0f, 0.5f, kInkDim, true);

    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    const char* labels[] = {"Singleplayer", "Multiplayer", "Options", "Quit"};
    for (int i = 0; i < 4; ++i) {
        const Rect rect{x, 100.0f + float(i) * (kButtonHeight + 6.0f), kButtonWidth,
                        kButtonHeight};
        drawButton(rect, labels[i], titleCursor_ == i, i != 1);
    }
}

void Menu::drawWorlds()
{
    drawLabelCentered("Select World", kScreenWidth * 0.5f, 16.0f, 0.7f, kInk, true);

    const int rows = rowCount(worlds_.size());
    const float rowX = 20.0f;
    const float rowWidth = kScreenWidth - 2.0f * rowX;

    for (int i = 0; i < kVisibleRows; ++i) {
        const int index = worldScroll_ + i;
        if (index >= rows) {
            break;
        }
        const Rect rect{rowX, kRowsTop + float(i) * (kRowHeight + kRowGap), rowWidth,
                        kRowHeight};
        const bool selected = index == worldCursor_;

        if (index == 0) {
            drawButton(rect, "+ Create New World", selected, true);
            continue;
        }

        // A world row is a button with two texts on it rather than a centred
        // label, so the name and when it was last played both fit.
        drawButton(rect, "", selected, true);
        const world::WorldEntry& entry = worlds_[usize(index - 1)];

        // Wide enough for the format's worst case rather than its usual one:
        // tm_year is an int and gmtime_r will hand back a five-digit year for a
        // level.dat with a nonsense LastPlayed in it.
        char when[64];
        formatWhen(entry.lastPlayed, when, sizeof(when));

        // The date's column is fixed, and the name gets whatever is left.
        constexpr float kWhenWidth = 100.0f;
        drawLabelClipped(entry.name.c_str(), rect.x + 10.0f, rect.y + 3.0f, 0.55f, kInk,
                         rect.w - 20.0f - kWhenWidth);
        drawLabel(when, rect.x + rect.w - 10.0f, rect.y + 6.0f, 0.4f, kInkDim, C2D_AlignRight,
                  true);
    }

    // Which way there is more list. Cheaper than a scrollbar and it answers the
    // only question a player has here.
    if (worldScroll_ > 0) {
        drawLabelCentered("^", kScreenWidth * 0.5f, kRowsTop - 10.0f, 0.5f, kInkDim, true);
    }
    if (worldScroll_ + kVisibleRows < rows) {
        drawLabelCentered("v", kScreenWidth * 0.5f, kScreenHeight - 10.0f, 0.5f, kInkDim,
                          true);
    }
}

void Menu::drawOptions()
{
    drawLabelCentered("Options", kScreenWidth * 0.5f, 24.0f, 0.8f, kInk, true);

    char distance[48];
    std::snprintf(distance, sizeof(distance), "Render distance: %d  (max %d)", renderDistance_,
                  maxDistance_);

    const float x = (kScreenWidth - kButtonWidth) * 0.5f;
    drawButton(Rect{x, 80.0f, kButtonWidth, kButtonHeight}, distance, optionsCursor_ == 0,
               true);
    drawButton(Rect{x, 120.0f, kButtonWidth, kButtonHeight}, "Back", optionsCursor_ == 1,
               true);

    drawLabelCentered(isNew3DS_ ? "New 3DS" : "Old 3DS", kScreenWidth * 0.5f, 170.0f, 0.45f,
                      kInkDim, true);
}

void Menu::drawConfirmDelete()
{
    const char* name = worldCursor_ > 0 && usize(worldCursor_ - 1) < worlds_.size()
                           ? worlds_[usize(worldCursor_ - 1)].name.c_str()
                           : "";

    drawLabelCentered("Delete this world?", kScreenWidth * 0.5f, 60.0f, 0.8f, kInk, true);
    drawLabelCentered(name, kScreenWidth * 0.5f, 100.0f, 0.6f, kInkWarn, true);
    drawLabelCentered("Everything in it goes with it.", kScreenWidth * 0.5f, 130.0f, 0.45f,
                      kInkDim, true);

    const float half = kButtonWidth * 0.5f;
    drawButton(Rect{kScreenWidth * 0.5f - half - 8.0f, 170.0f, half, kButtonHeight}, "A Delete",
               true, true);
    drawButton(Rect{kScreenWidth * 0.5f + 8.0f, 170.0f, half, kButtonHeight}, "B Keep", false,
               true);
}

void Menu::drawButton(const Rect& rect, const char* label, bool selected, bool enabled)
{
    const u32 fill = !enabled ? kFillDisabled : (selected ? kFillSelected : kFill);

    C2D_DrawRectSolid(rect.x - 2.0f, rect.y - 2.0f, 0.1f, rect.w + 4.0f, rect.h + 4.0f,
                      kOutline);
    C2D_DrawRectSolid(rect.x, rect.y, 0.2f, rect.w, rect.h, fill);
    C2D_DrawRectSolid(rect.x, rect.y, 0.3f, rect.w, 2.0f, kBevelLight);
    C2D_DrawRectSolid(rect.x, rect.y + rect.h - 2.0f, 0.3f, rect.w, 2.0f, kBevelDark);

    if (label[0] != '\0') {
        drawLabelCentered(label, rect.x + rect.w * 0.5f, rect.y + rect.h * 0.5f, 0.55f,
                          enabled ? kInk : kInkDim, true);
    }
}

void Menu::drawLabel(const char* text, float x, float y, float scale, u32 color, u32 flags,
                     bool shadow)
{
    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, text);
    C2D_TextOptimize(&parsed);

    if (shadow) {
        C2D_DrawText(&parsed, C2D_WithColor | flags, x + 1.0f, y + 1.0f, 0.4f, scale, scale,
                     kShadow);
    }
    C2D_DrawText(&parsed, C2D_WithColor | flags, x, y, 0.5f, scale, scale, color);
}

void Menu::drawLabelClipped(const char* text, float x, float y, float scale, u32 color,
                            float maxWidth)
{
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s", text);

    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, buffer);

    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);

    // **One corrective step, not a loop.** Every parse spends glyphs out of the
    // frame's text buffer, so a shrink-until-it-fits loop over a pathological
    // name would empty it and take the rest of the screen with it. The font is
    // proportional, so the estimate is not exact -- but it is always shorter
    // than what did not fit, and one visibly clipped name is better than a
    // frame with no text in it at all.
    const usize length = std::strlen(buffer);
    if (width > maxWidth && length > 4) {
        usize keep = usize(float(length) * (maxWidth / width));
        if (keep + 3 >= length) {
            keep = length - 4;
        }
        if (keep < 1) {
            keep = 1;
        }
        std::snprintf(buffer + keep, sizeof(buffer) - keep, "...");
        C2D_TextParse(&parsed, textBuf_, buffer);
    }

    C2D_TextOptimize(&parsed);
    C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignLeft, x + 1.0f, y + 1.0f, 0.4f, scale, scale,
                 kShadow);
    C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignLeft, x, y, 0.5f, scale, scale, color);
}

void Menu::drawLabelCentered(const char* text, float cx, float cy, float scale, u32 color,
                             bool shadow)
{
    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, text);
    C2D_TextOptimize(&parsed);

    // C2D_AlignCenter handles x; y is the *top* of the line, so the height has
    // to come back out of the buffer to centre it.
    float width = 0.0f;
    float height = 0.0f;
    C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
    const float y = cy - height * 0.5f;

    if (shadow) {
        C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignCenter, cx + 1.0f, y + 1.0f, 0.4f,
                     scale, scale, kShadow);
    }
    C2D_DrawText(&parsed, C2D_WithColor | C2D_AlignCenter, cx, y, 0.5f, scale, scale, color);
}

}  // namespace mc::ctr
