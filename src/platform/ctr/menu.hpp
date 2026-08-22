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

#include "core/io/posix_file_system.hpp"
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

private:
    enum class Screen {
        Title,
        Worlds,
        Options,
        ConfirmDelete,
    };

    void refreshWorlds();
    void setScreen(Screen screen);
    void printConsoleHelp();

    // Returns true when the choice is made and `run` should return.
    bool handleTitle(u32 down, MenuChoice* choice);
    bool handleWorlds(u32 down, MenuChoice* choice);
    void handleOptions(u32 down);
    void handleConfirmDelete(u32 down);

    // The two keyboards behind "+ Create New World". Both re-init the bottom
    // console on the way out, because an applet takes both screens and libctru's
    // console caches a framebuffer pointer that is no longer current -- the
    // same reason Overlay::teleportViaKeyboard does it.
    bool askWorldName(std::string* out);
    bool askSeed(i64* out);

    // Makes the world on the card and fills in the choice. False leaves a
    // message on the console and stays in the menu.
    bool createWorld(const std::string& name, i64 seed, MenuChoice* choice);

    void drawFrame();
    void drawBackground();
    void drawTitle();
    void drawWorlds();
    void drawOptions();
    void drawConfirmDelete();

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
    int worldCursor_ = 0;  // 0 is "+ Create New World"
    int worldScroll_ = 0;
    int optionsCursor_ = 0;

    int renderDistance_ = 0;
    int maxDistance_ = 0;
    bool isNew3DS_ = false;

    // Held so a message can survive a frame or two on the console: a failed
    // create is the one thing here that can go wrong silently.
    const char* message_ = nullptr;
};

}  // namespace mc::ctr
