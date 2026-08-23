#pragma once

// Making a string safe to be a file or directory name on a FAT card.
//
// This started life inside `world::sanitizeWorldName` and was lifted out when
// the texture-pack importer needed exactly the same rule for a pack file's
// name. Two copies of a rule about which bytes a card will accept is two
// chances to get it subtly different, and the difference would only show up on
// somebody else's SD card.
//
// The rule is deliberately conservative about *where* a name is read rather
// than only where it is written: a 3DS card is plugged into a PC as often as
// not, so anything Windows refuses to open is dropped here even when FAT itself
// would store it.

#include "core/util/types.hpp"

#include <string>
#include <string_view>

namespace mc::util {

// The longest name this will produce, before the trailing trim. Comfortably
// under FAT32's 255-character long-name limit, and short enough that a name
// plus a ".zip" or a "/level.dat" still fits in io::kMaxNameLength.
inline constexpr usize kMaxFatNameLength = 63;

// Drops every character FAT forbids and every control character, collapses runs
// of spaces, and trims leading and trailing spaces and dots. Returns false if
// nothing usable is left, which callers report rather than silently inventing a
// name.
//
// Bytes above 0x7F are kept: the card's long-name entries are UTF-16 and
// libctru transcodes, so an accented name is fine.
bool sanitizeFatName(std::string_view typed, std::string* out);

}  // namespace mc::util
