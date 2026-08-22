#include "core/util/console_text.hpp"

namespace mc {

usize clipToColumns(const char* in, char* out, usize outSize, int columns)
{
    if (outSize == 0) {
        return 0;
    }
    const usize limit = outSize - 1;

    usize written = 0;
    int drawn = 0;
    const char* p = in;
    while (*p != '\0' && written < limit) {
        if (*p == '\x1b') {
            // The introducer is always copied; then bytes up to and including
            // the first in `@`..`~`, which is the final byte of a CSI sequence.
            // `[` is inside that range and is the *second* byte of every
            // sequence the overlay writes, so it cannot be treated as an end.
            out[written++] = *p++;
            while (*p != '\0' && written < limit) {
                const char c = *p++;
                out[written++] = c;
                if (c >= '@' && c <= '~' && c != '[') {
                    break;
                }
            }
            continue;
        }
        if (drawn >= columns) {
            break;
        }
        out[written++] = *p++;
        ++drawn;
    }

    out[written] = '\0';
    return written;
}

}  // namespace mc
