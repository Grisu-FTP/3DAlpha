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
// for whichever screen is up -- except under the two settings lists, where it
// is **painted instead: the pack's dirt, the row's name and a short tooltip**
// in the pack's own font. The tooltip turns into pages, with arrows either side
// of the row's name on both screens, when it does not fit in one box.
//
// **The same class is also the in-game pause menu**, through `runPause`, and
// that is a reuse rather than a coincidence. A pause menu wants the Options
// screen and the Texture Pack list that are already here, and it wants them to
// be the *same* ones: the alternative is a second copy of both, plus a rule
// for reconciling what a player changed mid-world with what the main menu is
// still holding. Sharing the object makes that reconciliation nothing at all --
// there is one render distance, one pack list and one atlas, and both entry
// points read and write them.

#include "core/net/server_list.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/settings/control_scheme.hpp"
#include "core/settings/online_privacy.hpp"
#include "core/settings/sensitivity.hpp"
#include "platform/ctr/guest_play.hpp"
#include "platform/ctr/host_play.hpp"
#include "platform/ctr/online.hpp"
#include "platform/ctr/world_transfer.hpp"
#include "platform/ctr/local_link.hpp"
#include "core/audio/sound_engine.hpp"
#include "core/settings/settings_file.hpp"
#include "platform/ctr/audio.hpp"
#include "platform/ctr/menu_preview.hpp"
#include "core/preview/diorama.hpp"
#include "core/settings/world_settings.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/font.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/particle_sheet.hpp"
#include "core/texture/skin_list.hpp"
#include "core/util/types.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/size_scan.hpp"
#include "core/world/world_format.hpp"
#include "core/world/world_list.hpp"

#include "platform/ctr/gui_art.hpp"

#include <citro2d.h>

#include <memory>
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
        Join,  // a server off the multiplayer list, or a session next door
        Host,  // one of this console's own worlds, opened for others to join
    };

    // Which wire a Join or a Host uses. **Internet is the Java server on the
    // list; Local is another 3DS in the room** -- see platform/ctr/local_link.hpp
    // for what "local" means on this console and why it is not StreetPass.
    enum class Link {
        Internet,
        Local,
        // **Another 3DS, anywhere.** The same session the Local link runs --
        // the same handshake, the same world server, the same terrain coming
        // back -- over a UDP socket that AlphaComputer introduced the two
        // consoles through. See platform/ctr/online.hpp.
        Online,
    };

    Action action = Action::Quit;
    Link link = Link::Internet;



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

    // The world's difficulty, out of the same file and for the same reason.
    // **It is the game's own setting, not this port's** -- `cn.l` -- and three
    // things read it: the monster spawner, the peaceful removal and incoming
    // damage. See core/settings/world_settings.hpp.
    settings::Difficulty difficulty = settings::Difficulty::Normal;

    // What the player picked on the options screen, already clamped to what
    // this model will be offered. The shell hands it to the renderer.
    int renderDistance = 0;

    // Seconds between autosaves, and megabytes of chunk cache. Both come off
    // 3ds.ini through the options screen; the shell hands them to the streamer
    // before it opens the world, because both are read at open() and neither
    // can be resized under a running I/O thread.
    int autosaveSeconds = 0;
    int chunkCacheMB = 0;

    // How fast the view turns, as the percentage the Sensitivity row shows.
    // The shell turns it into a multiplier with `settings::sensitivityGain`
    // and applies it to both look devices.
    int lookSensitivity = settings::kDefaultSensitivity;

    // Which stick the shell reads movement off and which one it turns the view
    // with. The Controls row; see core/settings/control_scheme.hpp.
    settings::ControlScheme controlScheme = settings::ControlScheme::New3DS;

    // The block atlas for the chosen texture pack, already assembled and
    // already known to decode -- the pack screen builds it at the moment of
    // selection so a broken pack is refused there, in front of the player,
    // rather than in `runGame` where the only symptom would be an untextured
    // world. `runGame` points Renderer::Config at it, so **it must outlive the
    // game loop**, like worldPath and worldName above.
    texture::AtlasImage atlas;

    // **For Join**: the row's name, which the overlay's header and the
    // Downloading Terrain screen show where a world's name would go; where it
    // is, already parsed -- the list will not join an address that does not
    // parse; and whom to log in as. `worldName` is set to the server's name too.
    std::string serverName;
    std::string serverHost;
    u16 serverPort = 0;
    std::string username;
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

    // The same for the Sensitivity row. **It is one of the rows that can be
    // changed without leaving the world**, which is the whole point of it being
    // on this screen: a rate is something you set by feeling it, and feeling it
    // means looking around.
    int lookSensitivity = settings::kDefaultSensitivity;

    // And the Controls row, which is on this screen for exactly the same
    // reason: a player finds out which of the two Old schemes they want by
    // walking around under each of them, not by reading the names.
    settings::ControlScheme controlScheme = settings::ControlScheme::New3DS;

    // The player chose a different texture pack. The image is Menu::atlas();
    // the caller hands it to Renderer::setAtlas.
    bool atlasChanged = false;

    // What the World Settings screen settled on. Already written to the
    // world's 3dalpha.ini by the time this comes back -- this field is for the
    // caller to *apply*, the way renderDistance above is, not to persist.
    settings::Gamemode gamemode = settings::Gamemode::Spectator;
    settings::Difficulty difficulty = settings::Difficulty::Normal;
    // The one Extra Setting that changes play rather than generation.
    bool improvedFencePlacement = false;

    // Multiplayer's Chat row: what was typed, for the caller to send. Empty
    // when nothing was.
    std::string chat;
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

    // **The local session the player just joined**, once `run` has returned a
    // Join over Link::Local. The menu lets go of it entirely: the link, the
    // session and the parse thread belong to the game loop from here, and
    // leaving the world ends them -- coming back means joining again, which is
    // what a session the host may have closed in the meantime has to mean.
    std::unique_ptr<GuestPlay> takeGuest() { return std::move(guest_); }

    // **The host half of an internet session, already open.** A host on the
    // internet runs the session from the lobby -- the screen with the join
    // code on it -- so guests can arrive while the host is still deciding to
    // press Start. The game loop takes it here and opens the world behind it;
    // null for a session in a room, which has no lobby and starts with the
    // world. See `HostPlay::openLobby`.
    std::unique_ptr<HostPlay> takeHost() { return std::move(host_); }

    // **The link an internet session runs over**, or null when there is none.
    // Handed to `HostPlay` so a hosted world serves the guests the rendezvous
    // server introduces, and kept by the menu because the login was made here
    // and has to be taken down here.
    SessionLink* onlineLink();

    // After an online session: the socket, the login and the directory entry
    // all go. Safe when there was never one.
    void endOnline();

    // The pause menu: Resume, World Settings, Options, Exit World, over a world
    // that is still open behind it. Owns the frame loop the same way `run`
    // does, so the game is genuinely paused while this is up -- nothing is
    // streamed, nothing is meshed and the sun does not move.
    //
    // **That is single player's answer and only single player's.** A session
    // cannot stop, because one console does not get to decide that for the
    // others: see `beginPause`/`stepPause`/`endPause` below, which are this
    // function taken apart so the game loop can keep running around it.
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

    // **The same pause menu, a frame at a time, inside a loop somebody else
    // owns** -- which is what a session that must not stop looks like.
    //
    // `runPause` above is these three with a frame loop around them, so the
    // screens, the cursor and every decision they make are literally the same
    // code down both paths. What differs is who owns the frame: there the menu
    // draws the world through a `PauseBackdrop` and nothing else in the process
    // runs, and here the game loop keeps running -- streaming, ticking, pumping
    // the link -- and draws the menu into its own frame with
    // `pauseOverlayEntry` below.
    //
    // `beginPause` takes the same three arguments as `runPause` and on the same
    // terms; `stepPause` is one frame and answers true when the player has
    // chosen; `endPause` puts the menu back where it was and hands over the
    // choice, exactly as the tail of `runPause` does. **The caller reads no
    // input into the world while this is up**: a screen being open is what
    // stops the player moving, the same way `au` does to a dead one.
    void beginPause(const char* worldName, const char* worldPath, int renderDistance);
    bool stepPause(u32 down);
    PauseChoice endPause();

    // `Renderer::drawFrame`'s overlay callback, for a caller drawing the menu
    // into its own frame. The context is the Menu.
    static void pauseOverlayEntry(void* context, C3D_RenderTarget* target)
    {
        drawOverlayEntry(context, target);
    }

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

    // **The pack's bitmap font, for the world and not for the menu.** The menu
    // draws its own text through `BitmapFont`; a sign draws text *in the world*
    // and needs the sheet and the widths on the GPU side. Empty for a pack with
    // no `default.png`, which is the case a sign shows a blank board in.
    const texture::FontImage& fontImage() const { return fontImage_; }

    // **The pack's `particles.png`**, built beside the font and on the same
    // terms: a pack without one gets the generated stand-in rather than a
    // failure, so this is always `kParticleSheetBytes` long. See
    // core/texture/particle_sheet.hpp.
    const std::vector<u8>& particleSheet() const { return particleSheet_; }

    // **Multiplayer's pause menu**, which is a1.1.2's `ie` with a server behind
    // it: Resume, Chat, Options, Disconnect. World Settings is not offered,
    // because a server's world is not this console's to change. The caller
    // that owns the session sets it around `runPause`.
    void setMultiplayer(bool multiplayer) { multiplayer_ = multiplayer; }

    // Where the next `run()` opens after a session: `cj`, a1.1.2's
    // GuiDisconnected, with its title and the reason -- or, for a player who
    // left on their own, straight back to the server list.
    void showDisconnected(const std::string& title, const std::string& detail);

    // Back to the multiplayer screen, which is where a session that ended
    // leaves the player.
    void showMultiplayer();

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
        // screen is World Info, Gamemode, Difficulty and Back.
        WorldSettings,
        // The estimate a conversion is worth showing before it starts. Home
        // screen only, and its numbers are taken from the card rather than
        // guessed, which is why it is a screen and not a line of text.
        ConfirmConvert,
        // Which skin the arm of an empty hand is drawn with. A list rather
        // than a value row for the reason Texture Pack is one: the rows come
        // off a card and there can be any number of them.
        Skins,
        // **The one screen on this menu that is not trying to be a1.1.2.**
        // Every row on it either fixes a bug the original has or offers a
        // choice it never did, and all of them belong to one world. Reached
        // from the bottom of World Settings and, like Format, Copy and Delete,
        // **only outside a game**: two of its rows rewrite level.dat, which a
        // running world holds, and two more want the world diorama, which only
        // the main menu has.
        ExtraSettings,
        // Extra Settings' own pack list: the same rows the Options screen
        // offers plus "Default" pinned above them, which means "whatever the
        // console is set to".
        WorldPack,
        // Where the world's diorama stands, moved a map tile at a time with
        // the picture itself on the bottom screen.
        MovePanorama,
        // **Everything about a world that has to be decided before it exists**,
        // laid out like World Settings because it is the same question asked
        // one moment earlier. It replaced a pair of keyboards that opened back
        // to back: a name, then a seed, and no way to see either again or to
        // reach anything else. See kCreateRows.
        CreateWorld,
        // **Everything multiplayer**, which a1.1.2 does not have at all -- its
        // `gc` is one address field, remembered as `lastServer`. Two buttons
        // for starting or finding a session between two consoles, and below
        // them, kept apart from both, the list: the sessions a scan found in
        // the room and the Java servers saved on the card. See
        // core/net/server_list.hpp and platform/ctr/local_link.hpp.
        Multiplayer,
        // Local or Internet, asked once for whichever of the two buttons was
        // pressed. Internet is the existing server list and is not offered for
        // hosting; Local is the console's own wireless.
        NetMode,
        // One server's name and address, with Delete for a row that exists.
        // Nothing reaches the list or the card until Save.
        EditServer,
        ConfirmDeleteServer,
        // **Joined, and waiting for the world.** Where a guest sits between the
        // handshake and the game: who else is here, how the link is doing, and
        // B to leave. It is also where the world's arrival will be shown when
        // the host learns to send one -- see core/net/session.hpp.
        Session,
        // Why a session ended.
        Disconnected,
        // **The world offers a scan found**, which is the Join list's shape
        // asked about something else: consoles in the room that have pressed
        // Export and are waiting for somebody to take the world. Import only;
        // an export puts the beacon up and has nothing to look at.
        ImportScan,
        // **A world crossing the room**, from either end. One screen for both,
        // because a transfer looks the same from both sides -- a name, a bar
        // and a way out -- and the two halves differ only in which of them is
        // reading the card. See platform/ctr/world_transfer.hpp.
        Transfer,
        // **Who this console is online**, at the bottom of Options: which
        // server it talks to, what that server calls it, and the one button
        // that puts it on an account or takes it off one. It is the only screen
        // outside multiplayer that opens a socket, and it opens one only while
        // it is on screen. See platform/ctr/online.hpp.
        Profile,
        // **How far this world is about to reach**, asked before the world is
        // even chosen: the answer is what the rendezvous server is told when
        // the session is registered, and that happens the moment one is. See
        // core/settings/online_privacy.hpp.
        OnlinePrivacy,
        // **Between choosing a world and playing it, for a host on the
        // internet, and a room to wait in while people arrive.** There is
        // nothing like it for a session in a room, because a room needs no
        // introduction: this is where the console logs in, registers the world
        // with the server's directory and shows the six characters that are
        // the way in -- and then holds the session open, so a guest can be in
        // the player list on the bottom screen before the world exists. START
        // opens the world.
        OnlineHost,
        // The other end of those six characters.
        OnlineJoin,
    };

    // **Which of the four buttons asked the Local-or-Internet question.** It
    // used to be a bool for Host, and four answers do not fit in one: Import
    // and Export ask the same question for a reason that is not multiplayer at
    // all, and the screen's own wording has to say which.
    enum class NetPurpose {
        Join,
        Host,
        Import,
        Export,
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

    // **Lists the packs first**, because a skin row exists for every pack that
    // carries one and `PackEntry::hasSkin` is where that is known. On the
    // console that is a full read of every zip on the card, which is why this
    // runs when the screen is opened and not at boot -- the same price the
    // Texture Pack screen already pays, and the reason `ensureSkin` below
    // exists to apply a saved choice without it.
    void refreshSkins();

    // Applies the saved skin key to the live atlas, reading only the one file
    // it names. Called wherever the atlas is rebuilt, because rebuilding it
    // puts the *active pack's* skin back in the player's page.
    void applySavedSkin();
    void setScreen(Screen screen);
    void printConsoleHelp();

    // What a settings row is to the controls line under its explanation: a
    // value Left and Right change, a button A presses, or a row that is only
    // there to be read.
    enum class RowKind {
        Value,
        Action,
        Info,
    };

    // Paints the bottom screen for a settings row straight into its
    // framebuffer: the dirt backdrop, `title` (between arrows when there is
    // more than one page), `infoBody_` wrapped into a tooltip box and cut to
    // the current page, `message_`, and the controls for `kind`. Sets
    // `infoPages_`, which the top screen reads to give the selected button the
    // same arrows.
    void paintSettingInfo(const char* title, RowKind kind);

    // The pack's darkened dirt as bottom-screen texels, built into
    // `bottomTile_` on demand; null when the pack has not handed one over.
    const u16* bottomBackdropTile();

    // The backdrop alone, for the screens whose bottom half is still to be
    // decided: the title, the multiplayer list and the pause menu.
    void paintBackdropOnly();

    // The tooltip for each row, into `infoBody_`. Colour codes, not escapes:
    // it is drawn in the pack's font.
    void buildOptionsInfo(int row);
    void buildWorldSettingsInfo(int row);
    void buildExtraSettingsInfo(int row);

    // Why this console is silent, as one short line, or null when nothing is
    // wrong.
    const char* soundProblem() const;

    // L and R turn the explanation's page on every row; Left and Right as well
    // when `arrowsTurn`, which is a row with no value for them to change.
    // Wraps at both ends, because both arrows are always drawn.
    void turnInfoPage(u32 down, bool arrowsTurn);

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
    bool handleMultiplayer(u32 down, MenuChoice* choice);
    bool handleNetMode(u32 down, MenuChoice* choice);
    void handleEditServer(u32 down);
    void handleConfirmDeleteServer(u32 down);
    void handleDisconnected(u32 down);

    // The lobby: connecting to the session at `index` in `sessions_`, carrying
    // it forward once a frame, and leaving it.
    void beginLocalJoin(int index);
    void pumpSession();
    bool handleSession(u32 down, MenuChoice* choice);
    void endSession(const std::string& reason);

    void refreshServers();

    // **The Profile screen**, and the one object behind it. `ensureOnline`
    // makes it on first use and `start` is what opens a socket -- neither
    // happens on any path a single-player session takes.
    void ensureOnline(bool hosting);
    void startOnline(bool hosting);
    void stopOnline();
    void pumpOnline();
    void handleProfile(u32 down);
    void drawProfile();
    void buildProfileInfo(int row);
    const char* profileStatusText() const;
    const char* profileAccountText() const;

    // Remembers what the server last said this console is called, so the screen
    // has something to draw before it has connected. See
    // `settings::GameSettings::accountHandle`.
    void rememberAccount();

    void handleOnlinePrivacy(u32 down);
    void drawOnlinePrivacy();

    bool handleOnlineHost(u32 down, MenuChoice* choice);
    void drawOnlineHost();
    // The bottom screen of the lobby: who is here, on the dirt rather than on
    // the console's text grid. Drawn into the preview's render target -- see
    // `PreviewScreen::Plain` -- and skipped entirely when there is none, in
    // which case the console prints the same list.
    void drawLobbyPlayers();
    bool handleOnlineJoin(u32 down, MenuChoice* choice);
    void drawOnlineJoin();

    // **The session under the lobby.** Opened once the server has answered
    // with a join code: the world's own generator id is read out of its
    // level.dat without opening the world, and from then on guests can join,
    // be welcomed and be listed while the host is still looking at the code.
    void openOnlineLobby();

    // Once a frame while the lobby is up: the session, over the link the menu
    // is already servicing. Ends the lobby if the link goes.
    void pumpOnlineLobby();

    // Takes the lobby's session down without opening the world -- the host
    // pressed B. Everyone in it is told why.
    void cancelOnlineLobby(const std::string& reason);

    // The world the player picked, opened for the internet: logs in if it is
    // not already, then asks the server for a session and a join code.
    void beginOnlineHost();

    // Types the six characters and asks to be introduced to whoever is behind
    // them.
    void beginOnlineJoin(const std::string& code, u64 sessionId);

    // Searches the room for sessions, which takes about a second on the radio.
    // Called from the frame *after* the one that says it is searching, so the
    // message is on screen while it happens -- see `scanPending_`.
    void refreshLocalSessions();

    // **Import and Export, which are one feature asked from two screens.**
    // Import is a row on the world list and Export a row on a world's own
    // settings; both end up at the same Local-or-Internet question and then at
    // the same progress screen. See platform/ctr/world_transfer.hpp.
    //
    // Asks what the incoming world should be called and, if that is answered,
    // opens the Local-or-Internet question. The keyboard is first on purpose:
    // it suspends the console, and every applet in this build happens before a
    // link rather than during one.
    void openImport();
    bool askImportName(std::string* out);

    // The scan that fills `offers_`, run on the frame after the one that drew
    // "Searching", for the reason `scanPending_` gives.
    void refreshWorldOffers();

    // Puts this console's chosen world on the air and opens the progress
    // screen. Nothing is read off the card until somebody connects.
    void startExport();

    // Connects to `offers_[index]` and opens the progress screen.
    void startImport(int index);

    // Once a frame, whatever the player is pressing: a link that is not read is
    // a link that times out.
    void pumpTransfer();

    // Ends whatever is running, telling the other console if it still can, and
    // goes back to wherever the transfer was started from.
    void endTransfer(const char* why);

    void handleImportScan(u32 down);
    void handleTransfer(u32 down);

    // How many rows the multiplayer list has under its two buttons, and what
    // the row at `index` is.
    int multiplayerRows() const;
    // The keyboard for a server's name, its address, its port, or a line of
    // chat. `numeric` swaps the letters for the numpad, which is the whole of
    // what a port row wants from a keyboard.
    bool askServerText(const char* hint, const std::string& current, int maxChars,
                       std::string* out, bool numeric = false);
    void handleOptions(u32 down);
    void handleWorldSettings(u32 down);
    void handleExtraSettings(u32 down);
    void handleWorldPack(u32 down);
    void handleMovePanorama(u32 down);
    void handleConfirmConvert(u32 down);
    void handleConfirmDelete(u32 down);
    void handleTexturePacks(u32 down);
    void handleSkins(u32 down);
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
    // is closed -- the start of its size measurement. Called on the way in, so
    // the screen never walks a tree from inside a draw.
    void openWorldSettings(const std::string& name, const std::string& path);

    // Sets the selected world's size being added up on the I/O thread. On a
    // folder world that is a stat per chunk file, which is long enough to be
    // seen, so the screen says "loading..." and carries on drawing until
    // pollWorldSize picks the answer up. See core/world/size_scan.hpp.
    void measureSelectedWorld();

    // Once a frame: takes the finished measurement, if there is one, and marks
    // the bottom screen for repainting so the row stops saying "loading...".
    void pollWorldSize();

    // Writes the selected world's 3dalpha.ini. Called when a row changes
    // rather than on the way out, for the same reason saveSettings is: the way
    // out of this screen is often the player launching a world.
    void saveWorldSettings();

    // ---- Extra Settings -------------------------------------------------

    // Reads the selected world's level.dat without claiming it, for the two
    // rows that are level.dat values rather than 3dalpha.ini ones: the seed
    // and SnowCovered. `extraLevelKnown_` is false when it would not decode,
    // and both rows then draw as unavailable rather than as a value that is
    // really a default.
    void openExtraSettings();
    // **The same screen, over the world being made rather than one on the
    // card.** Reached from Create World's Extra Settings row; see
    // `kExtraRowsNew` for the three rows it does not offer.
    void openExtraSettingsForNewWorld();
    // Which settings the Extra Settings screen -- and the World Texture Pack
    // list under it -- is editing: `newWorld_`'s while a world is being made,
    // and the selected world's otherwise.
    settings::WorldSettings& extraSettings();
    const settings::WorldSettings& extraSettings() const;
    // Writes them to the card, or does nothing at all when the world they
    // belong to has not been made yet -- `createWorld` saves those.
    void saveExtraSettings();

    // Opens the world, applies whatever `openExtraSettings` last read plus the
    // caller's change, saves the level and closes again. **The one place this
    // menu writes a world's level.dat**, and the reason the screen is not
    // offered in game: the running world holds the same file.
    //
    // False leaves `message_` saying why, and leaves the row showing what is
    // actually on the card.
    bool saveExtraLevel();

    // swkbd for a new seed, then `saveExtraLevel`. The world keeps every chunk
    // it already has -- this is the seed the *next* chunk is generated from,
    // which is said on the bottom screen because it is the whole of what the
    // row does and is not what a player expects from "set seed".
    void askNewSeed();

    // The pack list for the world, with Default pinned on top. Listing the
    // packs folder is a full read of every zip on the card, so it happens on
    // the way into the screen and nowhere else -- the same price the Options
    // screen's list pays.
    void openWorldPack();

    // Which row of `worldPackRows()` the world's saved choice is, so the list
    // opens on it. 0 is Default.
    int worldPackRow() const;

    // Default, then Dev Art, then every pack on the card.
    int worldPackRows() const;

    // The label for the world's pack choice, for the value row and the list's
    // "in use" mark: "Default", "Dev Art", or the pack's name.
    const char* worldPackLabel() const;

    // The atlas the *world* is drawn with, which is the console's unless the
    // world names a pack of its own. Called on the way into a world; the menu
    // puts its own pack back the next time it is entered.
    void applyWorldPack();

    // Writes the panorama position being edited and makes the preview read it
    // back. Every d-pad step on the Move Panorama screen goes through here:
    // the file is the only channel between this screen and the worker that
    // builds the table.
    void commitPanorama();

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

    // The two keyboards the Create World screen's Name and Seed rows open.
    // Both re-init the bottom console on the way out, because an applet takes
    // both screens and libctru's console caches a framebuffer pointer that is
    // no longer current -- the same reason Overlay::teleportViaKeyboard does
    // it.
    //
    // `askSeed` still treats **blank as a valid answer meaning "roll one"**,
    // which is what a1.1.2 does every time because it never asks at all. The
    // screen shows that state as `Random` rather than as a number it has
    // already picked, so the row says what will happen instead of pretending
    // the choice is made.
    bool askWorldName(std::string_view current, std::string* out);
    bool askSeed(bool* chosen, i64* out);

    // The same keyboard and the same validation as askWorldName, with the
    // source world's name offered as the starting point. Separate only because
    // the hint text and the suggestion differ.
    bool askCopyName(std::string_view sourceName, std::string* out);

    // **What the Create World screen has collected so far.** A world is not
    // touched on the card until Create is pressed, so every row writes here
    // and nowhere else -- which is also what lets B leave without having made
    // anything.
    struct NewWorld {
        std::string name;

        // False means Random: no seed has been typed, and one is drawn from
        // the clock when Create is pressed. Kept apart from `seed` rather than
        // folded into a sentinel because **0 is a perfectly good seed** and a
        // player who types it must get it.
        bool seedChosen = false;
        i64 seed = 0;

        // Gamemode, difficulty and the two generation fixes, written straight
        // into the new world's 3dalpha.ini. The fixes matter more here than
        // anywhere else: they only affect chunks that have not been generated
        // yet, and at this moment none of them have.
        settings::WorldSettings settings;

        // a1.1.2 rolls SnowCovered once, `rand.nextInt(4) == 0`, with the
        // World's unseeded Random -- so Roll is the faithful answer and the
        // default. Yes and No are the deviation, and are the same switch the
        // Extra Settings screen offers afterwards.
        enum class Secret { Roll, Yes, No };
        Secret secret = Secret::Roll;

        // Packed unless the player says otherwise; see createWorld for why
        // that is the default on this hardware.
        world::WorldFormat format = world::WorldFormat::Packed;
    };

    // Fills `newWorld_` with the defaults a fresh screen offers: the first
    // free "World<n>", a random seed, and a1.1.2 everywhere else.
    void openCreateWorld();

    // Returns true when a world was made and `run` should return -- the same
    // contract handleWorlds has, and for the same reason: Create ends in a
    // MenuChoice.
    bool handleCreateWorld(u32 down, MenuChoice* choice);
    void drawCreateWorld();
    void buildCreateWorldInfo(int row);

    // Makes the world on the card from `newWorld_` and fills in the choice.
    // False leaves a message on the console and stays on the screen, with
    // everything the player typed still in it.
    bool createWorld(MenuChoice* choice);

    // One turn of either frame loop: the console help for whichever screen is
    // up, then the top screen. Shared so `run` and `runPause` cannot drift.
    void present();
    // Everything `present` does except draw the top screen, for the caller that
    // owns the frame itself. See `stepPause`.
    void presentState();
    // The main menu's bottom-screen previews: which of them the current screen
    // wants, the cursors and the pack they follow, and what they say under
    // the picture. All three do nothing in game, where there is no preview.
    PreviewScreen previewScreenFor(Screen screen) const;
    void syncPreview();
    void updatePreview();
    void drawPreviewScreen();
    void drawPreviewLabels();

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
    void drawMultiplayer();
    void drawNetMode();
    void drawSession();
    void drawImportScan();
    void drawTransfer();
    void drawEditServer();
    void drawConfirmDeleteServer();
    void drawDisconnected();
    void drawOptions();
    void drawWorldSettings();
    void drawExtraSettings();
    void drawWorldPack();
    void drawMovePanorama();
    void drawConfirmConvert();
    void drawConfirmDelete();
    void drawTexturePacks();
    void drawSkins();

    // The Options row's label, built from `skinKey_` alone -- see the note on
    // the definition for why it does not go near the card. `skinLabel_` is the
    // storage it hands back, and is why this is not `const char*` off a local.
    const char* skinLabel() const;
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

    // One row of a settings list: a centred label when `value` is null, and a
    // name on the left with its value beside it otherwise.
    struct SettingRowView {
        const char* name = "";
        const char* value = nullptr;
        bool dimValue = false;
    };

    // Draws the rows of a settings list that fit under `top`, starting at
    // `scroll`, with a caret beside the list where there is more of it. The
    // selected row gets arrows either side while its explanation has pages.
    void drawSettingsRows(const SettingRowView* rows, const u8* groups, int count, int cursor,
                          int scroll, float top);

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
    // **Which world the cursor is on, by name rather than by row.** The list is
    // sorted by last played, so playing a world moves it to the top and every
    // row below it down one: a remembered *index* points at a different world
    // the moment you come back from the one you chose. Empty means the Create
    // row, which is row 0 whatever the list does. `refreshWorlds` puts the
    // cursor back on this world if it is still there.
    std::string worldCursorName_;
    int optionsCursor_ = 0;
    int optionsScroll_ = 0;
    int worldSettingsCursor_ = 0;
    int worldSettingsScroll_ = 0;
    int extraCursor_ = 0;
    int extraScroll_ = 0;
    // True while Extra Settings is standing over `newWorld_` -- which is what
    // decides the row list, which settings the rows edit, whether a change is
    // written to the card at all, and where B goes back to.
    bool extraForNewWorld_ = false;
    int worldPackCursor_ = 0;
    int worldPackScroll_ = 0;
    int createCursor_ = 0;
    int createScroll_ = 0;
    NewWorld newWorld_;

    // **What level.dat says about the selected world**, read by
    // openExtraSettings and written back by saveExtraLevel. Held rather than
    // re-read per frame because reading it means decoding a compressed NBT
    // file, which is not something a draw may do.
    bool extraLevelKnown_ = false;
    i64 extraSeed_ = 0;
    bool extraSnowCovered_ = false;

    // The panorama position the Move Panorama screen is editing, and the one
    // it was opened with, so B can put it back. In 128-block map tiles; see
    // core/preview/diorama.hpp.
    i32 panoramaTileX_ = 0;
    i32 panoramaTileZ_ = 0;
    i32 panoramaUndoX_ = 0;
    i32 panoramaUndoZ_ = 0;
    // Which place in the world tile zero stands on, and the places this world
    // has to offer. Both read in openExtraSettings off the level that screen
    // already decodes; Move Panorama is reached through it and no other way.
    settings::PanoramaAnchor panoramaAnchor_ = settings::PanoramaAnchor::Spawn;
    settings::PanoramaAnchor panoramaUndoAnchor_ = settings::PanoramaAnchor::Spawn;
    preview::DioramaAnchors panoramaAnchors_;

    // True while the live atlas is a world's pack rather than the console's,
    // so the next visit to the menu knows to build the console's again. See
    // applyWorldPack.
    bool packOverridden_ = false;

    // The selected settings row's tooltip, wrapped for the bottom screen, and
    // which page of it is up. Kept rather than rebuilt on the stack because the
    // top screen reads `infoPages_` every frame and the text is only rebuilt
    // when the bottom screen is.
    std::string infoBody_;
    std::vector<std::string> infoLines_;
    int infoPage_ = 0;
    int infoPages_ = 1;

    // **The console's own 8x8 font, as a FontImage**, for a pack with no
    // `default.png` -- Dev Art among them. Built the first time it is needed.
    texture::FontImage consoleFont_;
    // The dirt tile in the bottom screen's RGB565, converted on each paint. A
    // vector rather than an array so the Menu does not grow by 2 KB wherever it
    // is kept.
    std::vector<u16> bottomTile_;

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
    // The walk behind that number, on its own thread. Cancelled on the way out
    // of the screen and before anything rewrites the world it is walking.
    world::SizeScan sizeScan_;
    // Out of the world list's own entry, which already read level.dat -- so
    // World Info costs no second read, and a world open behind the pause menu
    // is not read while it is being written. Unknown when there is no entry.
    i64 selectedSeed_ = 0;
    i64 selectedLastPlayed_ = 0;
    bool selectedLevelKnown_ = false;

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
    int difficultyCursor_ = int(settings::Difficulty::Normal);

    // What ConfirmConvert is about: the format being converted *to*, and what
    // the card said it would cost.
    world::WorldFormat convertTarget_ = world::WorldFormat::Unknown;
    world::format::ConvertEstimate convertEstimate_;

    // The pack list, with Dev Art pinned at index 0 and always present, and
    // which of them is live. `packName_` is the file name inside the packs
    // folder, empty for Dev Art -- the same value 3ds.ini stores.
    std::vector<texture::PackEntry> packs_;
    std::vector<texture::SkinEntry> skins_;
    std::vector<texture::JarEntry> jars_;
    texture::AtlasImage atlas_;
    std::string packName_;

    // `SkinEntry::key`: empty is Default, which is the active pack's own
    // `char.png` or the black silhouette. See core/texture/skin_list.hpp.
    std::string skinKey_;
    // Scratch for `skinLabel()`, which returns a pointer into it.
    mutable std::string skinLabel_;

    // The pack's menu art, decoded once per pack rather than once per visit --
    // 68 KB held for the process against three card reads per visit to the
    // menu, which on a pause menu is three reads a player waits on with a world
    // already open. Empty is a real state for both: it means "this pack has
    // none", and the drawing falls back rather than refusing.
    texture::FontImage fontImage_;
    std::vector<u8> particleSheet_;
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
    int skinCursor_ = 0;   // 0 is Default
    int skinScroll_ = 0;
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

    // The Sensitivity row, as the percentage a1.1.2's slider prints. 100 is
    // this port's own look rate, so a console that has never touched the row
    // turns exactly as it did before the row existed. See
    // core/settings/sensitivity.hpp.
    int lookSensitivity_ = settings::kDefaultSensitivity;

    // The Controls row: which stick walks and which one turns. The flag is the
    // settings file's "the player has never been asked" -- see
    // `initCommon`, which answers it with the console model and then sets it,
    // so a later trip through the menu cannot walk over a row the player has
    // since moved. See core/settings/control_scheme.hpp.
    settings::ControlScheme controlScheme_ = settings::ControlScheme::New3DS;
    bool controlSchemeChosen_ = false;

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

    // **What the pause menu has decided so far**, because a stepped pause
    // outlives the frame that started it. `beginPause` seeds it, `stepPause`
    // fills it in and `endPause` completes and returns it; `runPause` goes
    // through all three and so reads it the same way.
    PauseChoice pauseChoice_;
    // `packRevision_` as `beginPause` found it, which is what tells the caller
    // the pack was swapped while the menu was up.
    u32 pauseRevisionAtEntry_ = 0;
    // init() runs around every visit to the menu; the card is read once.
    bool settingsLoaded_ = false;

    // The bottom-screen previews on the Skins, Texture Pack and World screens.
    // Made by init() and never by initOverlay(): the pause menu keeps its
    // console. On the heap because the Menu lives on a 32 KB stack.
    std::unique_ptr<MenuPreview> preview_;
    u64 previewClockMs_ = 0;

    // Held so a message can survive a frame or two on the console: a failed
    // create is the one thing here that can go wrong silently.
    const char* message_ = nullptr;

    // ---- multiplayer ----------------------------------------------------

    std::vector<net::ServerEntry> servers_;

    // The multiplayer screen's cursor covers the two buttons and the list
    // under them: 0 is Host, 1 is Join, 2 is "+ Add Server", and the rest are
    // the sessions found nearby followed by the saved servers. `serverScroll_`
    // is the list's own first visible row, counted from 2.
    int serverCursor_ = 0;
    int serverScroll_ = 0;

    // What a scan found, newest answer wins. Empty until Join -> Local has
    // been chosen once, which is why the screen says so rather than showing an
    // empty list and leaving it at that.
    std::vector<LocalSession> sessions_;
    bool scanned_ = false;
    bool scanPending_ = false;
    std::string localError_;

    // Which button opened the Local/Internet question, and its cursor.
    NetPurpose netPurpose_ = NetPurpose::Join;
    int netModeCursor_ = 0;

    // **A world on its way between two consoles**, and everything the two
    // screens above need to draw it. Null except while one is running; see
    // platform/ctr/world_transfer.hpp.
    std::unique_ptr<WorldTransfer> transfer_;

    // What an incoming world will be called on this card. Asked on a keyboard
    // before the radio is touched, because a keyboard suspends the console --
    // see `ctr::linkPausing` -- and a link cannot be told about a suspension
    // that starts before it does.
    std::string importName_;

    // The consoles offering a world, and where the cursor is in them. Separate
    // from `sessions_` because the two lists answer different questions and a
    // player looking for a game should not be shown a folder transfer.
    std::vector<LocalSession> offers_;
    int offerCursor_ = 0;
    bool offerScanPending_ = false;

    // The world list is being used to pick a world to host rather than one to
    // play. It changes the title, where B goes back to, and which action the
    // choice carries.
    bool pickingHost_ = false;

    // The joined session, while the lobby is up. On the heap for the reason
    // everything else here is: a `Peer` carries 90 KB of window and the menu
    // lives on a 32 KB stack.
    // **The guest half of a local session, from the radio up.** Held by the
    // menu because that is where a join starts and where it goes back to when
    // it ends; handed to the game with `takeGuest` when the host starts
    // sending a world, and not owned here again after that.
    std::unique_ptr<GuestPlay> guest_;
    // The row EditServer is changing, -1 for a new one, and its working copy.
    int editServerIndex_ = -1;
    net::ServerEntry editServer_;
    int editServerCursor_ = 0;
    // The friend list's name, asked for on the first visit to the list rather
    // than at boot: the friend service is one more thing to start, and most
    // sessions never open this screen.
    std::string username_;

    // **Null until somebody asks to be online.** Constructing it costs a
    // socket, a key read off the card and a name lookup, and a player who never
    // opens the Profile screen and never picks Internet pays for none of it.
    std::unique_ptr<Online> online_;

    // What the Profile screen is showing, and where `message_` points while it
    // is up -- `message_` is a borrowed pointer, so the string it names has to
    // live somewhere that outlasts the frame.
    std::string onlineMessage_;
    int profileCursor_ = 0;
    int profileScroll_ = 0;

    // The URL the Profile screen edits, and the account it last heard about.
    // Both come off 3ds.ini; the account is a cache, so the screen can say who
    // this console is before it has connected to ask.
    std::string serverUrl_;
    std::string accountHandle_;
    std::string accountName_;

    // **The session an internet host is running from the lobby**, before the
    // world is open and before the game loop has it. Handed over by `takeHost`
    // when Start is pressed, and dropped where it stands if the host backs
    // out. Null on every other path, including a session in a room.
    std::unique_ptr<HostPlay> host_;
    // Who the session says is in it, rebuilt when it changes rather than every
    // frame, because it is a vector of strings and this is a menu.
    std::vector<net::link::Player> lobbyPlayers_;
    // The lobby is up: the session is open and the world is not.
    bool onlineLobbyOpen_ = false;
    // How far the world about to be hosted reaches. Read from 3ds.ini and
    // written back when it is answered; see core/settings/online_privacy.hpp.
    settings::OnlinePrivacy onlinePrivacy_ = settings::OnlinePrivacy::CodeOnly;
    int privacyCursor_ = 0;

    // Which world a host is opening for the internet, and the code the server
    // gave it. The code is what a host reads out; it is the only way in to a
    // session this build opens, which is hosted unlisted on purpose.
    std::string onlineWorldName_;
    std::string onlineWorldPath_;
    std::string onlineJoinCode_;
    bool onlineSessionOpen_ = false;
    // Whether the session has been asked for. A flag rather than "the message
    // happens to be empty": a refusal would otherwise leave it never asked.
    bool onlineHostAsked_ = false;
    // A host that chose Internet rather than Local, from the moment the
    // question is answered until the world opens.
    bool hostingOnline_ = false;
    // A guest that has asked to be introduced and is waiting for the punch.
    bool onlineJoining_ = false;
    int onlineJoinCursor_ = 0;
    int onlineJoinScroll_ = 0;
    // A guest that was introduced over the internet rather than found in the
    // room, so the lobby hands the game the right kind of link.
    bool guestOnline_ = false;

    bool multiplayer_ = false;
    std::string disconnectTitle_;
    std::string disconnectDetail_;
};

}  // namespace mc::ctr
