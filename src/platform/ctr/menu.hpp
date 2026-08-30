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
//   * **It draws with the pack's own art when the pack has any, and with
//     citro2d and the 3DS system font when it does not.** We ship no Mojang
//     assets, so the backdrop and the font are whatever the player's pack
//     carries: `dirt.png` tiled and darkened the way `GuiScreen` does it, and
//     `default.png` as a bitmap font with a1.1.2's own glyph widths. A pack
//     with neither -- Dev Art, or a pack carrying terrain.png alone -- still
//     gets a backdrop out of its own dirt tile, and falls back to the system
//     font for text. See platform/ctr/gui_art.hpp and docs/assets.md. The
//     buttons are still rectangles: there is no widget sheet consumer yet.
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
#include "core/audio/sound_engine.hpp"
#include "core/settings/settings_file.hpp"
#include "platform/ctr/audio.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/font.hpp"
#include "core/texture/pack_list.hpp"
#include "core/util/types.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/world_format.hpp"
#include "core/world/world_list.hpp"

#include "platform/ctr/gui_art.hpp"

#include <citro2d.h>

#include <string>
#include <string_view>
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

    // The world's own gamemode, read out of its 3dalpha.ini when it was
    // chosen. Handed over with the world rather than looked up by the caller,
    // because the menu has already opened that file to draw the row.
    settings::Gamemode gamemode = settings::Gamemode::Spectator;

    // What the player picked on the options screen, already clamped to what
    // this model will be offered. The shell hands it to the renderer.
    int renderDistance = 0;

    // Seconds between autosaves, and megabytes of chunk cache. Both come off
    // 3ds.ini through the options screen; the shell hands them to the streamer
    // before it opens the world, because both are read at open() and neither
    // can be resized under a running I/O thread.
    int autosaveSeconds = 0;
    int chunkCacheMB = 0;

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

    // What the Options screen settled on for the autosave timer. Applied to
    // the running world -- unlike the cache size, which is fixed at open().
    int autosaveSeconds = 0;

    // The player chose a different texture pack. The image is Menu::atlas();
    // the caller hands it to Renderer::setAtlas.
    bool atlasChanged = false;

    // What the World Settings screen settled on. Already written to the
    // world's 3dalpha.ini by the time this comes back -- this field is for the
    // caller to *apply*, the way renderDistance above is, not to persist.
    settings::Gamemode gamemode = settings::Gamemode::Spectator;
};

// How the pause menu gets the world behind it.
//
// **The pause menu used to draw its own dirt backdrop over a world it had
// hidden**, with a scrim on top to say the world was still there. The reason
// was real -- the menu had its own render target and its own frame, and putting
// citro2d and citro3d in one frame was the seam `crashlogs/004` came out of --
// but the answer was the wrong way round. What the original does is draw the
// world and then a gradient over it, and that is what this makes possible: the
// caller owns the frame, draws the frozen world into it, and hands each eye's
// target to the menu to draw 2D on.
//
// The crash that shaped the old arrangement is not what stops this: it was
// `C2D_Fini` freeing a shader program citro3d still pointed at, which
// parkShaderProgram already answers. Interleaving inside a live frame is
// supported -- `C2D_Prepare` re-establishes every piece of state citro2d needs,
// and Renderer::applyWorldState does the same for the world.
//
// A default-constructed one means "no world behind me", which is the main menu.
struct PauseBackdrop {
    void* context = nullptr;

    // Opens a frame, draws the frozen world on every eye, and calls
    // `overlay(overlayContext, target)` on each one before ending the frame.
    void (*drawFrame)(void* context, void* overlayContext,
                      void (*overlay)(void* overlayContext, C3D_RenderTarget* target)) =
        nullptr;
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

    // The same, for a pause menu that draws into somebody else's frame: no
    // render target of its own, and the top screen is left exactly as the
    // renderer has it. **Nothing here reads the card**, which is the whole
    // difference the player feels -- see the note on init().
    bool initOverlay(bool isNew3DS);

    void shutdown();

    // Runs the menu until the player picks a world or leaves. Owns the frame
    // loop while it runs, including its own vsync.
    //
    // The world list and the pack list are read here rather than in init(),
    // because this is the only entry point that shows either.
    MenuChoice run();

    // The pause menu: Resume, World Settings, Options, Exit World, over a world
    // that is still open behind it. Owns the frame loop the same way `run`
    // does, so the game is genuinely paused while this is up -- nothing is
    // streamed, nothing is meshed and the sun does not move.
    //
    // `renderDistance` is the live one rather than the saved one; see
    // PauseChoice. `worldName` is drawn under the heading and is only read
    // while this runs; `worldPath` is what the World Settings screen writes
    // its 3dalpha.ini to, and is copied rather than borrowed.
    //
    // **init() has to have been called and shutdown() has to follow**, exactly
    // as around `run`, and for the same reason: the 400x240 target and
    // citro2d's vertex buffer are not worth holding through a game session.
    // `backdrop` is what makes the menu transparent: with one, the caller draws
    // the frozen world and this draws over it; without one, it falls back to
    // the dirt backdrop and needs a target of its own from init().
    PauseChoice runPause(const char* worldName, const char* worldPath, int renderDistance,
                         const PauseBackdrop& backdrop = PauseBackdrop{});

    // The live block atlas. Borrowed -- it belongs to the Menu, which outlives
    // every game session in the shell. Read after runPause when
    // PauseChoice::atlasChanged says the pack was swapped.
    const texture::AtlasImage& atlas() const { return atlas_; }

    // The pack's darkened dirt tile, as `buildBackground` produced it: 32 x 32
    // RGBA, or empty when the pack has none and the atlas could not supply one
    // either. **The bottom screen's HUD tiles it behind its panels**, which is
    // the same picture and the same reasoning as the menu's own backdrop -- the
    // two screens should not disagree about what this pack looks like. Held for
    // the process, so it survives `shutdown()` and a world may read it.
    const std::vector<u8>& backgroundTile() const { return backgroundTile_; }

    // Handed the process's audio before the first menu is drawn. Both pointers
    // are borrowed and must outlive the menu; the shell owns them. Optional --
    // a menu with neither still works and simply says there is no audio.
    void setSound(mc::audio::SoundEngine* sound, ctr::NdspBackend* backend)
    {
        sound_ = sound;
        audioBackend_ = backend;
    }

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
        // The world's own settings, as against the console's. Reached from
        // **both** loops, and `inGame_` says which -- but unlike Options, that
        // flag changes more than where B goes: a world that is open and
        // streaming cannot be copied, deleted or converted, so in game this
        // screen is Gamemode and Back and nothing else.
        WorldSettings,
        // The estimate a conversion is worth showing before it starts. Home
        // screen only, and its numbers are taken from the card rather than
        // guessed, which is why it is a screen and not a line of text.
        ConfirmConvert,
        // Volumes and the audio toggle, plus the one line that explains a
        // console that is silent. It is a sub-screen rather than three more
        // rows on Options because 240 pixels does not stretch -- the same
        // reason Texture Pack is one -- and because the explanation needs room
        // that a value row does not have.
        Sound,
    };

    // The shared half of init() and initOverlay(): citro2d, the text buffer,
    // the settings and the pack's art. Neither the card nor the screen.
    // `objects` is citro2d's vertex budget in quads, which differs between the
    // two because an overlay is drawn once per eye.
    bool initCommon(bool isNew3DS, int objects);

    // The atlas the game is handed, built from the saved pack name without
    // listing the packs folder. Listing it costs a full read of every zip on
    // the card -- see texture::listPacks -- and that is a price for the screen
    // that shows the list, not for every visit to the menu.
    void ensureAtlas();

    void refreshWorlds();
    void refreshPacks();
    void refreshJars();
    void setScreen(Screen screen);
    void printConsoleHelp();

    // The four facts that separate the four unrelated causes of silence. See
    // the comment on the definition.
    void printSoundDiagnostics();

    // `GuiScreen.mouseClicked` in a1.1.2 plays `random.click` at volume 1 and
    // pitch 1 for every press that lands on an enabled button -- sliders
    // included, because `GuiSlider` is a `GuiButton` and the press is what is
    // heard, not the drag. Verified in `bh.a(int,int,int)`; the volume table is
    // in docs/audio-a1.1.2.md. Every A press in this file that does something
    // goes through here.
    //
    // Escape is silent in the original and B and START are silent here, for the
    // same reason: leaving a screen is not pressing a button on it.
    void playClick();

    // The cursor moved. **This one is a deviation and there is no original to
    // be faithful to**: a1.1.2's menus are pointed at with a mouse and have no
    // cursor to move, so there is nothing for a d-pad press to sound like.
    //
    // Rather than invent a tone, it plays the quieter, lower click a1.1.2 makes
    // for itself -- `random.click` at volume 0.3 and pitch 0.5, which is what a
    // button block plays when it pops back out (`no.class`) and a lever plays
    // when it is thrown down (`hu.class`). So the pair a player hears is two
    // settings of one sound the game already had: quiet and low for moving,
    // full and open for choosing.
    void playMoveClick();

    // The cursor step every screen shares, wrapping at both ends -- and the one
    // place the move click is played, so no screen can forget it and none can
    // play it twice. The circle pad reports through the d-pad's mask, so both
    // work everywhere without a second code path.
    int step(u32 down, int cursor, int count);

    // Returns true when the choice is made and `run` should return.
    bool handleTitle(u32 down, MenuChoice* choice);
    bool handlePause(u32 down, PauseChoice* choice);
    bool handleWorlds(u32 down, MenuChoice* choice);
    void handleOptions(u32 down);
    void handleSound(u32 down);
    void handleWorldSettings(u32 down);
    void handleConfirmConvert(u32 down);
    void handleConfirmDelete(u32 down);
    void handleTexturePacks(u32 down);
    void handlePickJar(u32 down);
    void handleConfirmDeleteJar(u32 down);

    // Loads the pack at `index` in packs_ and makes it the live one. On failure
    // the previous pack stays selected and `message_` names the reason, which
    // is the whole point of doing this here instead of in the renderer.
    bool selectPack(int index);

    // Decodes the live pack's `default.png` and `dirt.png`, then uploads both.
    //
    // Separate from the atlas because they fail separately and none of the
    // three is required: a pack with no font leaves the system font in place,
    // and a pack with no dirt.png draws the dirt tile of its own terrain.png.
    // `force` re-reads the card even when the pack's name has not changed,
    // which is what a jar re-extracted over an existing pack needs.
    void loadPackArt(bool force);

    // Puts the decoded art on the GPU. Called on every visit to the menu,
    // because init() and shutdown() bracket each one and the textures go with
    // them -- the decoded copies above do not.
    void uploadPackArt();

    // The bitmap font's scale for a label written in the system font's units.
    //
    // The call sites all say things like 0.5f, which was a multiplier on the
    // 3DS system font's ~30-pixel line box. A bitmap font has one honest size,
    // its cell, and whole multiples of it; this maps the one to the other so
    // the layout keeps its hierarchy without every call site being rewritten.
    int fontScale(float scale) const;

    // Where a bitmap line starts so that it sits where the system font's line
    // box would have put its middle. Keeps every y in the drawing code meaning
    // what it meant before the font changed.
    float fontTop(float y, float scale, int pixels) const;

    // Points the World Settings screen at a world and reads what it needs off
    // the card: the per-world settings file, the format, and -- when the world
    // is closed -- its size. Called on the way in, so the screen never walks a
    // tree from inside a draw.
    void openWorldSettings(const std::string& name, const std::string& path);

    // Adds up the selected world's size, drawing a frame first because on a
    // folder world this is a stat per chunk file and the screen would
    // otherwise simply stop.
    void measureSelectedWorld();

    // Writes the selected world's 3dalpha.ini. Called when a row changes
    // rather than on the way out, for the same reason saveSettings is: the way
    // out of this screen is often the player launching a world.
    void saveWorldSettings();

    // Reads what the pending conversion would cost, and puts up the screen
    // that says so. False leaves a message and stays where it is.
    bool beginConvert();

    // Runs the conversion the ConfirmConvert screen was about, drawing
    // progress and polling B to cancel. Synchronous: the world is closed and
    // the console has nothing else to do.
    void runConvert();

    // swkbd for the new name, then a tree copy. The copy keeps whichever
    // format the source is in -- it is a backup, not a conversion.
    void copySelectedWorld();

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

    // The same keyboard and the same validation as askWorldName, with the
    // source world's name offered as the starting point. Separate only because
    // the hint text and the suggestion differ.
    bool askCopyName(std::string_view sourceName, std::string* out);

    // Makes the world on the card and fills in the choice. False leaves a
    // message on the console and stays in the menu.
    bool createWorld(const std::string& name, i64 seed, MenuChoice* choice);

    // One turn of either frame loop: the console help for whichever screen is
    // up, then the top screen. Shared so `run` and `runPause` cannot drift.
    void present();

    void drawFrame();
    void drawBackground();

    // The dim over a world that is still open behind the menu, gradient and
    // all. Its own function because both backdrops end in it: the dirt one
    // draws it on top, and the transparent one is nothing else.
    void drawScrim();

    // citro2d's drawing state, put back over whatever drew last. Both paths
    // call it, because after a game session the renderer's state is what a
    // fresh citro2d frame would otherwise inherit.
    void prepare2D();

    // The 2D half of a frame, drawn on whichever target is current. Shared by
    // both paths: the menu's own frame and the caller's.
    void drawScreen();

    // Renderer::Overlay2D's shape. Unpacks the Menu and draws one eye.
    static void drawOverlayEntry(void* context, C3D_RenderTarget* target);
    void drawOverlay(C3D_RenderTarget* target);
    void drawTitle();
    void drawPause();
    void drawWorlds();
    void drawOptions();
    void drawSound();

    // What the Sound screen says about a console that is not making any. Never
    // null: "audio works" is a case too.
    const char* soundStatus() const;
    void drawWorldSettings();
    void drawConfirmConvert();
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

    // Set for as long as runPause is drawing into somebody else's frame. Null
    // means this menu owns its frames and its target, which is the main menu
    // and a pause menu with no backdrop to draw on.
    PauseBackdrop backdrop_;

    // The system font's line box at scale 1, measured through citro2d rather
    // than written down: fontTop() converts against it, and a system font that
    // is not 30 pixels on some console would otherwise shift every label.
    float systemLineHeight_ = 30.0f;

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
    int soundCursor_ = 0;
    int worldSettingsCursor_ = 0;

    // The world the World Settings screen is about, and everything it reads
    // off the card on the way in.
    //
    // **Its own copy of the name and path, not an index into worlds_.** The
    // list is re-read on every refresh and a conversion or a copy refreshes it,
    // so an index would be pointing at a different row -- or at nothing --
    // halfway through the operation it started. It is also what lets the same
    // screen serve the pause menu, where there is no list at all.
    std::string selectedWorldName_;
    std::string selectedWorldPath_;
    world::WorldFormat selectedFormat_ = world::WorldFormat::Unknown;
    world::WorldSize selectedSize_;
    bool selectedSizeKnown_ = false;

    // The world's own settings, as they are on the card. Read in
    // openWorldSettings and written back the moment a row changes, so the file
    // and the screen never disagree.
    //
    // Gamemode lives here rather than in level.dat -- a key no version of the
    // original ever wrote would travel back to a PC copy of the world -- and
    // rather than in 3ds.ini, where one value would serve every world on the
    // card. See core/settings/world_settings.hpp.
    settings::WorldSettings worldSettings_;

    // Which entry of kGamemodeOrder the row is *showing*, which is not always
    // the world's mode. Survival and Creative can be stepped onto and read --
    // greyed out, with the reason under them -- without being adopted; only an
    // implemented mode is written to the file. Reset from the file whenever the
    // screen is pointed at a world, so browsing never leaks into the next one.
    int gamemodeCursor_ = 0;

    // What ConfirmConvert is about: the format being converted *to*, and what
    // the card said it would cost.
    world::WorldFormat convertTarget_ = world::WorldFormat::Unknown;
    world::format::ConvertEstimate convertEstimate_;

    // The pack list, with Dev Art pinned at index 0 and always present, and
    // which of them is live. `packName_` is the file name inside the packs
    // folder, empty for Dev Art -- the same value 3ds.ini stores.
    std::vector<texture::PackEntry> packs_;
    std::vector<texture::JarEntry> jars_;
    texture::AtlasImage atlas_;
    std::string packName_;

    // The pack's menu art, decoded once per pack rather than once per visit --
    // 68 KB held for the process against three card reads per visit to the
    // menu, which on a pause menu is three reads a player waits on with a world
    // already open. Empty is a real state for both: it means "this pack has
    // none", and the drawing falls back rather than refusing.
    texture::FontImage fontImage_;
    std::vector<u8> backgroundTile_;
    // Which pack the two above came from, so a second visit to the menu costs
    // an upload and not three card reads.
    std::string artPackName_;
    bool artLoaded_ = false;
    // Whether the two above are on the GPU. False after every shutdown(),
    // because the textures go with it.
    bool artUploaded_ = false;

    BitmapFont font_;
    Background background_;

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
    int autosaveSeconds_ = 0;
    int chunkCacheMB_ = 0;

    // 0..100, the original's own units. -1 from the settings file means "not
    // chosen yet" and is resolved to 100 on the way in, so nothing downstream
    // has to know about the sentinel.
    int musicVolume_ = 100;
    int soundVolume_ = 100;
    bool audioEnabled_ = true;

    // Borrowed, process-lifetime, and both null in the overlay's copy of this
    // class -- the pause menu's Options screen reaches the same engine the
    // shell made. Null means the Sound row is drawn but says audio is not
    // available, which is exactly what it would say anyway.
    mc::audio::SoundEngine* sound_ = nullptr;
    ctr::NdspBackend* audioBackend_ = nullptr;
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
