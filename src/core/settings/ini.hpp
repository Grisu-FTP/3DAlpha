#pragma once

// The `key=value` reader both settings files share.
//
// There are two of them now -- `3ds.ini` beside the game and `3dalpha.ini`
// inside each world -- and they differ only in their key set. Two copies of a
// parser is how the two quietly stop agreeing about what a comment is or
// whether a trailing `\r` counts, so the parsing lives here and the files carry
// nothing but their own keys.
//
// Deliberately not a general ini library: no sections, no escapes, no
// continuation lines. What it parses is what a person edits by hand on a PC
// after taking the card out, which is the only reason the format is text.

#include "core/util/types.hpp"

#include <string_view>

namespace mc::settings {

// Spaces, tabs and a trailing `\r` -- a card can be edited on a PC, and a file
// written there arrives with CRLF line endings.
std::string_view trim(std::string_view text);

// A decimal integer, or false. **Deliberately not atoi**: "12abc" is a line
// somebody mistyped, and taking the 12 out of it silently is worse than
// ignoring the line.
//
// A leading '-' is accepted because the settings that mean "not chosen yet" say
// so with -1, and a value the game writes has to be one the game can read back.
bool parseInt(std::string_view text, int* out);

// Splits the next line off `text`, advancing it past the newline. Returns false
// when there is nothing left. Blank lines and `#` comments are skipped here, so
// no caller has to remember to.
bool nextEntry(std::string_view* text, std::string_view* key, std::string_view* value);

}  // namespace mc::settings
