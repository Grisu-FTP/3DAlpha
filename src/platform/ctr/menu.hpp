#pragma once

// The main menu: the title screen, the world list, and creating a world.
//
// **This is where the game starts now.** Until it existed, `runGame` opened
// `worlds[0]` -- whatever readdir handed back first -- and a console with no
// world on the card printed a paragraph on the bottom screen saying worldgen
// was not written yet. Both are gone: a world is chosen, or made, before
// anything else happens.
//
// Three things about its shape are decisions rather than accidents:
//
//   * **The list is every world on the card, with "+ Create New World" pinned
//     at the top.** a1.1.2's own screen is five fixed slots -- `World1`..
//     `World5`, `- empty -`, `Delete world...`, `Cancel`, verified in the
//     bytecode of `jq.class` -- and a card that holds hundreds of saves has no
//     use for five. Recorded here as a deliberate deviation.
//   * **The seed can be typed.** a1.1.2 never asks: `new World(File, String)`
//     seeds itself with `new Random().nextLong()`. Being able to type one is
//     the player-facing proof that the generator is seed-exact, which is worth
//     a deviation on a screen that is ours anyway.
//   * **It draws with citro2d and the 3DS system font.** We ship no Mojang
//     assets, and there is no PNG decoder or RomFS in the tree yet, so a
//     Minecraft-style `default.png` is not available -- see docs/assets.md.
//     Nothing here is textured at all: the background is shaded quads and the
//     buttons are rectangles, in the same spirit as the placeholder atlas. When
//     the asset pipeline lands, `C2D_FontLoad` and a real widget sheet replace
//     the drawing without touching the menu's logic.
//
// The bottom screen stays the text console throughout, printing the controls
// for whichever screen is up.
//
// **The same class is also the in-game pause menu**, through `runPause`, and
// that is a reuse rather than a coincidence. A pause menu wants the Options
// screen and the Texture Pack list that are already here, and it wants them to
// be the *same* ones: the alternative is a second copy of both, plus a rule
// for reconciling what a player changed mid-world with what the main menu is
// still holding. Sharing the object makes that reconciliation nothing at all --
// there is one render distance, one pack list and one atlas, and both entry
// points read and write them.

#include "core/io/posix_file_system.hpp"
#include "core/settings/settings_file.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/pack_list.hpp"
#include "core/util/types.hpp"
#include "core/world/world_list.hpp"

#include <citro2d.h>

#include <string>
#include <vector>

namespace mc::ctr {

// The console clock in the units level.dat wants: milliseconds since the Unix
// epoch. `osGetTime()` counts from 1900, which is 2,208,988,800 seconds earlier.
i64 nowMillis();

// Where worlds live on the card. The menu creates it on demand, so a fresh
// install has nothing to do first.
inline constexpr char kSavesDir[] = "sdmc:/3dalpha/saves";

// The folder above it, which is where a player who dropped a jar onto the card
// without reading anything is most likely to have put it. The jar picker looks
// here as well as in packs/.
inline constexpr char kRootDir[] = "sdmc:/3dalpha";

struct MenuChoice {
    enum class Action {
        Quit,  // the player chose Quit, or the system asked us to exit
        Play,
    };

    Action action = Action::Quit;

    // The world to open, and its directory name for the overlay's header.
    // **Both are read as pointers by the overlay**, so this struct has to
    // outlive the game loop that uses it.
    std::string worldPath;
    std::string worldName;

    // True when this world was made a moment ago and is therefore empty. The
    // game waits for the spawn area before handing over; an existing world has
    // chunks on the card and needs no such wait.
    bool created = false;

    // What the player picked on the options screen, already clamped to what
    // this model will be offered. The shell hands it to the renderer.
    int renderDistance = 0;

    // The block atlas for the chosen texture pack, already assembled and
    // already known to decode -- the pack screen builds it at the moment of
    // selection so a broken pack is refused there, in front of the player,
    // rather than in `runGame` where the only symptom would be an untextured
    // world. `runGame` points Renderer::Config at it, so **it must outlive the
    // game loop**, like worldPath and worldName above.
    texture::AtlasImage atlas;
};

// What the pause menu decided, and what changed while it was up.
//
// Separate from MenuChoice because the two answer different questions: one
// picks a world to open, the other is handed a world that is already open and
// says whether to carry on with it. The two fields below it are the settings
// that can be applied to a running world -- the caller applies them, because
// a render distance means rebuilding the pool and an atlas means a re-upload,
// and neither belongs to a menu.
struct PauseChoice {
    enum class Action {
        Resume,
        ExitWorld,
    };

    Action action = Action::Resume;

    // What the Options screen settled on. It starts at whatever the caller
    // passed in -- which is the *live* distance, not the saved one, so opening
    // the pause menu over a world the debug page has pushed to distance 20 does
    // not quietly pull it back to the play maximum.
    int renderDistance = 0;

    // The player chose a different texture pack. The image is Menu::atlas();
    // the caller hands it to Renderer::setAtlas.
    bool atlasChanged = false;
};

class Menu {
public:
    // Builds the citro2d context and the top-screen target. C3D_Init must have
    // run already. False when there is no memory for either, which the shell
    // reports rather than looping on a blank screen.
    //
    // **Call it around each visit to the menu, not once for the process.** The
    // target is a 400x240 colour buffer plus depth, and holding that through a
    // game session would take it out of the VRAM the atlas and the VBO pool are
    // measured against.
    bool init(bool isNew3DS);
    void shutdown();

    // Runs the menu until the player picks a world or leaves. Owns the frame
    // loop while it runs, including its own vsync.
    MenuChoice run();

    // The pause menu: Resume, Options, Exit World, over a world that is still
    // open behind it. Owns the frame loop the same way `run` does, so the game
    // is genuinely paused while this is up -- nothing is streamed, nothing is
    // meshed and the sun does not move.
    //
    // `renderDistance` is the live one rather than the saved one; see
    // PauseChoice. `worldName` is drawn under the heading and is only read
    // while this runs.
    //
    // **init() has to have been called and shutdown() has to follow**, exactly
    // as around `run`, and for the same reason: the 400x240 target and
    // citro2d's vertex buffer are not worth holding through a game session.
    PauseChoice runPause(const char* worldName, int renderDistance);

    // The live block atlas. Borrowed -- it belongs to the Menu, which outlives
    // every game session in the shell. Read after runPause when
    // PauseChoice::atlasChanged says the pack was swapped.
    const texture::AtlasImage& atlas() const { return atlas_; }

private:
    enum class Screen {
        Title,
        Worlds,
        Options,
        ConfirmDelete,
        TexturePacks,
        PickJar,
        ConfirmDeleteJar,
        // Only ever reached through runPause. Options and TexturePacks are
        // shared with the main menu and know which of the two they are under
        // from `inGame_`, because that is the only thing that differs: where
        // B goes back to.
        Pause,
    };

    void refreshWorlds();
    void refreshPacks();
    void refreshJars();
    void setScreen(Screen screen);
    void printConsoleHelp();

    // Returns true when the choice is made and `run` should return.
    bool handleTitle(u32 down, MenuChoice* choice);
    bool handlePause(u32 down, PauseChoice* choice);
    bool handleWorlds(u32 down, MenuChoice* choice);
    void handleOptions(u32 down);
    void handleConfirmDelete(u32 down);
    void handleTexturePacks(u32 down);
    void handlePickJar(u32 down);
    void handleConfirmDeleteJar(u32 down);

    // Loads the pack at `index` in packs_ and makes it the live one. On failure
    // the previous pack stays selected and `message_` names the reason, which
    // is the whole point of doing this here instead of in the renderer.
    bool selectPack(int index);

    // Reads the jar at `index` in jars_, writes a pack beside it and verifies
    // the result. Only a verified import moves on to ConfirmDeleteJar -- that
    // verification is what makes offering to delete the source jar safe.
    void extractJar(int index);

    // Both are cheap and neither is called per frame. Settings are written when
    // a value changes rather than on the way out, because the way out of this
    // menu is often the player launching a world and never coming back.
    void loadSettings();
    void saveSettings();

    // The two keyboards behind "+ Create New World". Both re-init the bottom
    // console on the way out, because an applet takes both screens and libctru's
    // console caches a framebuffer pointer that is no longer current -- the
    // same reason Overlay::teleportViaKeyboard does it.
    bool askWorldName(std::string* out);
    bool askSeed(i64* out);

    // Makes the world on the card and fills in the choice. False leaves a
    // message on the console and stays in the menu.
    bool createWorld(const std::string& name, i64 seed, MenuChoice* choice);

    // One turn of either frame loop: the console help for whichever screen is
    // up, then the top screen. Shared so `run` and `runPause` cannot drift.
    void present();

    void drawFrame();
    void drawBackground();
    void drawTitle();
    void drawPause();
    void drawWorlds();
    void drawOptions();
    void drawConfirmDelete();
    void drawTexturePacks();
    void drawPickJar();
    void drawConfirmDeleteJar();

    // The three list screens differ in what a row holds, not in how the list
    // moves, so the arrows and the visible window are shared and the row loop
    // is not. Returns how many rows starting at `scroll` are on screen.
    int drawListChrome(int rows, int scroll);

    // "Dev Art" or the pack's file name -- what the Options row shows.
    const char* packLabel() const;

    struct Rect {
        float x, y, w, h;
    };

    void drawButton(const Rect& rect, const char* label, bool selected, bool enabled);

    // `flags` is citro2d's alignment set; x is the left edge, the centre or the
    // right edge to match it. The shadow is one pixel down and right, which is
    // what makes light text readable over a busy backdrop.
    void drawLabel(const char* text, float x, float y, float scale, u32 color, u32 flags,
                   bool shadow);
    void drawLabelCentered(const char* text, float cx, float cy, float scale, u32 color,
                           bool shadow);

    // Left-aligned, and shortened with an ellipsis if it will not fit in
    // `maxWidth`. A world name comes off a card and can be anything a PC let
    // someone type, and citro2d does not clip -- it would draw straight over
    // the column beside it and off the edge of the screen.
    void drawLabelClipped(const char* text, float x, float y, float scale, u32 color,
                          float maxWidth);

    C3D_RenderTarget* target_ = nullptr;
    C2D_TextBuf textBuf_ = nullptr;

    io::PosixFileSystem fs_;
    std::vector<world::WorldEntry> worlds_;

    Screen screen_ = Screen::Title;
    Screen printedScreen_ = Screen::Title;
    bool consoleDirty_ = true;

    int titleCursor_ = 0;
    int pauseCursor_ = 0;
    int worldCursor_ = 0;  // 0 is "+ Create New World"
    int worldScroll_ = 0;
    int optionsCursor_ = 0;

    // The pack list, with Dev Art pinned at index 0 and always present, and
    // which of them is live. `packName_` is the file name inside the packs
    // folder, empty for Dev Art -- the same value 3ds.ini stores.
    std::vector<texture::PackEntry> packs_;
    std::vector<texture::JarEntry> jars_;
    texture::AtlasImage atlas_;
    std::string packName_;

    // Bumped by every successful selectPack. The pause menu reports a changed
    // atlas by comparing this across the visit rather than by comparing
    // packName_, because re-extracting a jar can replace a pack's contents
    // under the same name -- and then the name would say nothing changed while
    // every texel had.
    u32 packRevision_ = 0;

    int packCursor_ = 0;   // 0 is "+ Extract from a jar..."
    int packScroll_ = 0;
    int jarCursor_ = 0;
    int jarScroll_ = 0;

    // What the last import wrote and what it was made from, held across the
    // ConfirmDeleteJar screen. The jar path is what the player is being asked
    // about, so it has to survive the frame the import finished on.
    std::string importedJar_;
    std::string importedPack_;
    int importedCount_ = 0;

    int renderDistance_ = 0;
    int maxDistance_ = 0;
    bool isNew3DS_ = false;

    // True for as long as runPause owns the loop. The shared screens read it in
    // two places and nowhere else: where B goes back to, and what the console
    // says the controls are.
    bool inGame_ = false;

    // Where the main menu was standing when the world was opened, so leaving
    // the pause menu puts it back rather than dropping the player onto whatever
    // screen the pause menu happened to end on. Without it, exiting a world
    // would come back to the pause menu itself.
    Screen resumeScreen_ = Screen::Title;

    // The world behind the pause menu, for the line under the heading. Borrowed
    // from the caller for the length of runPause.
    const char* pauseWorldName_ = "";
    // init() runs around every visit to the menu; the card is read once.
    bool settingsLoaded_ = false;

    // Held so a message can survive a frame or two on the console: a failed
    // create is the one thing here that can go wrong silently.
    const char* message_ = nullptr;
};

}  // namespace mc::ctr
