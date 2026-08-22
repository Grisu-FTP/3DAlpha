#pragma once

// Turning what a player typed into a world seed.
//
// **a1.1.2 has no seed entry at all** -- `new World(File, String)` seeds itself
// with `new Random().nextLong()` and never asks -- so every rule here is ours,
// and the one it follows is the convention Minecraft itself adopted later:
// a string that reads as a number *is* that number, and anything else is
// hashed. Getting the hash right is what makes a seed swapped with someone on
// a PC produce the same world, which is also the player-facing proof that the
// generator is seed-exact.
//
// It lives in core rather than beside the keyboard that collects it for the
// same reason as coord_text: `src/platform/ctr` cannot be unit-tested, and text
// parsing is where the bugs are.

#include "core/util/types.hpp"

#include <string_view>

namespace mc {

// Java's `String.hashCode()`, which is specified rather than implementation
// defined: s[0]*31^(n-1) + s[1]*31^(n-2) + ... + s[n-1], over **UTF-16 code
// units**, wrapping in 32 bits.
//
// The text arrives from the system keyboard as UTF-8, so it is decoded to
// UTF-16 on the way through -- a character above U+FFFF is two units in Java
// and has to be two here, or a seed with an emoji in it would hash differently
// on a console than on a PC. Malformed UTF-8 hashes its bytes as U+FFFD, which
// is what a decoder is allowed to do and what keeps this total.
i32 javaStringHash(std::string_view text);

// Parses a seed the way the create-world screen wants it.
//
// Returns false when the text is blank, which means "roll one" and is the
// caller's job -- there is no clock in core to roll it from.
//
// Otherwise, surrounding whitespace is ignored and:
//   * a decimal integer that fits an i64 is that seed, sign and all, including
//     zero. Later Minecraft treats a typed 0 as "random" -- that is a side
//     effect of how it defaults the field, not a rule worth copying, and a
//     player who types 0 here means 0.
//   * anything else -- letters, punctuation, or a number too big for an i64,
//     which is exactly what makes Java's Long.parseLong throw -- is hashed with
//     javaStringHash and sign-extended, as Java's `(long)s.hashCode()` does.
bool seedFromText(std::string_view text, i64* out);

}  // namespace mc
