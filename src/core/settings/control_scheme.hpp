#pragma once

// Which stick walks the player and which one turns the view.
//
// **This port was written for a New 3DS**: the circle pad moves and the
// C-stick looks, which is the pairing every console Minecraft has used and the
// reason the touch screen could go back to being a screen. An Old 3DS has no
// C-stick -- `ir:rst` answers for a Circle Pad Pro and for nothing else -- so
// under that scheme it is left steering with one stick and looking with a
// finger, which is not a way to play the game.
//
// So the second stick becomes the d-pad, and there are two ways round to pair
// it because there are two kinds of player: one who wants the analogue stick
// under the thumb that aims, and one who wants it under the thumb that walks.
// Neither is more correct than the other, which is why both are offered rather
// than argued about.
//
// **Nothing else moves between the schemes.** Every other binding -- the
// shoulders, the face buttons, SELECT's modifiers -- is the same in all three,
// so the row changes two lines of the controls table and not the table.
//
// It is core rather than platform code for the reason `sensitivity.hpp` is:
// which device does which job is a rule, the console cannot run a test, and
// the platform layer is where `KEY_DUP` is allowed to be spelled. Nothing here
// names a button; `platform/ctr/main.cpp` turns a `Stick` into a reading.

#include "core/util/types.hpp"

#include <string_view>

namespace mc::settings {

// **The names are the consoles they suit, not the consoles they run on.** A
// New 3DS can be set to either Old scheme -- a player who would rather walk
// with the d-pad is entitled to -- and an Old 3DS with a Circle Pad Pro can be
// set to New3DS, because that accessory is exactly what it is for.
enum class ControlScheme : u8 {
    // Circle pad moves, C-stick looks. What this port shipped with.
    New3DS = 0,
    // D-pad moves, circle pad looks. The pairing that puts the analogue stick
    // under the aiming thumb, which is what the second stick is for.
    Old3DS,
    // The same two devices with the jobs swapped: circle pad moves, d-pad
    // looks. For a player who wants walking to be analogue.
    Old3DSAlt,
};

inline constexpr int kControlSchemeCount = 3;

// The two-axis inputs this console can be read for. **Not a button mask** --
// the platform layer owns those.
enum class Stick : u8 {
    CirclePad,
    Dpad,
    // Through `ir:rst`, so a Circle Pad Pro on an Old 3DS reports as one.
    CStick,
};

// Which device walks and which one turns, under a given scheme. **They are
// never the same device**, which is the invariant the whole row rests on and
// which `tests/control_scheme_test.cpp` states.
Stick moveStick(ControlScheme scheme);
Stick lookStick(ControlScheme scheme);

// The word written to `3ds.ini`. Stable across builds -- a word rather than an
// ordinal, the same choice `gamemodeToken` makes, so a file written by a later
// build that adds a scheme is readable here instead of being a number that
// silently means something else.
const char* controlSchemeToken(ControlScheme scheme);

// What the Options row draws.
const char* controlSchemeLabel(ControlScheme scheme);

// What the bottom screen calls a device, for the keybind list under the row.
const char* stickLabel(Stick stick);

// False when the word is not one of the three; the caller keeps its default.
bool controlSchemeFromToken(std::string_view token, ControlScheme* out);

// **What a console with no answer in its settings file gets.** The model is
// the whole of it: an Old 3DS under `New3DS` has one stick and a touch screen,
// which is the state this row exists to fix, so it is not what a player who
// has never seen the row should be left in.
ControlScheme defaultControlScheme(bool isNew3DS);

}  // namespace mc::settings
