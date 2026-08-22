#pragma once

// Fitting text to a fixed-width character console, escape sequences and all.
//
// This exists because of a bug that was invisible until it was catastrophic.
// The 3DS debug overlay ended every line with `\n` and trusted that to mean
// "next row". libctru's console is 40 columns wide and wraps, so a line 41
// characters long quietly took *two* rows -- and six of the Info page's lines
// were 41 to 44 characters with ordinary numbers in them. The page overran the
// bottom of the screen, the console scrolled, and what the player saw was the
// frame-time line repeated several times over, each copy one sample block older
// than the one below it. It got worse the busier the console was, because that
// is when the numbers are widest.
//
// A byte count is the wrong measure: an ANSI escape costs bytes and draws
// nothing, and the overlay's selection cursor is `"\x1b[33m> \x1b[0m"` -- nine
// bytes, two columns. Clipping on length would cut that row seven characters
// short.
//
// Kept in core, away from the platform, because the escape handling is fiddly
// enough to be worth a test and none of it needs a console to run.

#include "core/util/types.hpp"

namespace mc {

// Copies `in` into `out`, stopping after `columns` *drawn* characters. Escape
// sequences -- ESC, then bytes up to and including one in `@`..`~` -- are copied
// through whole and counted as zero columns.
//
// Writes at most `outSize - 1` bytes and always terminates. Returns the number
// of bytes written, not counting the terminator, so a caller can append its own
// reset.
usize clipToColumns(const char* in, char* out, usize outSize, int columns);

}  // namespace mc
