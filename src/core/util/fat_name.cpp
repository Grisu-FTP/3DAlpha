#include "core/util/fat_name.hpp"

#include <cstring>

namespace mc::util {

bool sanitizeFatName(std::string_view typed, std::string* out)
{
    out->clear();
    for (const char c : typed) {
        const u8 byte = u8(c);
        // FAT's reserved set, plus every control character and DEL. Bytes above
        // 0x7F are kept: the card's long-name entries are UTF-16 and libctru
        // transcodes, so an accented name is fine.
        if (byte < 0x20 || byte == 0x7F) {
            continue;
        }
        if (std::strchr("\\/:*?\"<>|", c) != nullptr) {
            continue;
        }
        if (c == ' ' && (out->empty() || out->back() == ' ')) {
            continue;  // no leading name and no doubled spaces
        }
        out->push_back(c);
        if (out->size() >= kMaxFatNameLength) {
            break;
        }
    }

    // Trailing spaces and dots: FAT stores them, Windows refuses to open what
    // it made, and a name that is only dots is "." or ".." to every listing
    // there is.
    while (!out->empty() && (out->back() == ' ' || out->back() == '.')) {
        out->pop_back();
    }
    while (!out->empty() && out->front() == '.') {
        out->erase(out->begin());
    }
    return !out->empty();
}

}  // namespace mc::util
