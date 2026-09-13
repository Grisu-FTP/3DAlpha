#pragma once

// Parsing a coordinate triple that a person typed.
//
// This lives in core rather than beside the keyboard that collects it for one
// reason: `src/platform/ctr` cannot be unit-tested on the host, and text
// parsing is where the bugs are. The 3DS side does the applet and the console
// drawing; the decision about whether "12550824 70 -3.5" is a place you can go
// is made here, where a test can ask it 40 questions.
//
// **Deliberately not strtod.** It is available and it would be shorter, and it
// also accepts `inf`, `nan`, and hexadecimal floats like `0x1p10`. A NaN
// coordinate is the worst possible outcome here -- it propagates into the
// camera, then into the view matrix, and every comparison against it is false,
// so the world quietly stops drawing instead of reporting anything. strtod is
// also locale-sensitive about the decimal separator, which matters when comma
// is one of the accepted separators *between* numbers. Neither risk is worth
// the twenty lines saved.

#include "core/util/types.hpp"

namespace mc {

// a1.1.2's own horizontal limit, the bound `World.setBlock` checks against.
// Not a rounder number of our choosing: it is what the original refuses past.
inline constexpr double kWorldHorizontalLimit = 32000000.0;

// **Not a height limit.** The world is 128 blocks tall, but a teleport may go
// anywhere above or below it -- every block lookup in TickWorld already answers
// for heights outside the column, and the visibility walk clamps its starting
// section itself. The bound exists only so the readouts
// that print `int(camera.y)` stay defined, and it is the horizontal one reused
// because there is no reason for the two to differ. Free flight clamps to the
// same value, so a teleport cannot put the camera somewhere the next frame of
// movement would drag it out of.
inline constexpr double kCameraYLimit = kWorldHorizontalLimit;

struct CoordTriple {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// Parses "x y z" -- separated by spaces, commas, or both, with any amount of
// surrounding whitespace. Accepts a leading sign and a decimal point; rejects
// exponents, hex, infinities and NaN, because none of them name a block and
// all of them are easier to type by accident than on purpose.
//
// **Returns nullptr on success, or a short message on failure**, which is the
// shape the 3DS keyboard's filter callback wants: it takes a `const char**` and
// shows whatever is written to it without taking ownership, so every message
// here is a string literal.
const char* parseCoordinateTriple(const char* text, CoordTriple* out);

}  // namespace mc
