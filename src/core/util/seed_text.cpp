#include "core/util/seed_text.hpp"

namespace mc {

namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string_view trim(std::string_view text)
{
    usize begin = 0;
    usize end = text.size();
    while (begin < end && isSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && isSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// One code point, and how many bytes it took. Anything malformed -- a stray
// continuation byte, a truncated sequence, an overlong encoding, a surrogate
// written as UTF-8 -- consumes one byte and yields U+FFFD, so the decoder
// always makes progress and never reads past the end.
u32 nextCodePoint(std::string_view text, usize* index)
{
    constexpr u32 kReplacement = 0xFFFD;

    const u8 lead = u8(text[*index]);
    ++*index;

    if (lead < 0x80) {
        return lead;
    }

    int extra = 0;
    u32 value = 0;
    u32 lowest = 0;
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
        lowest = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
        lowest = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
        lowest = 0x10000;
    } else {
        return kReplacement;
    }

    if (*index + usize(extra) > text.size()) {
        return kReplacement;
    }
    for (int i = 0; i < extra; ++i) {
        const u8 byte = u8(text[*index + usize(i)]);
        if ((byte & 0xC0) != 0x80) {
            return kReplacement;  // truncated: the lead byte alone is consumed
        }
        value = (value << 6) | (byte & 0x3F);
    }
    *index += usize(extra);

    if (value < lowest || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        return kReplacement;
    }
    return value;
}

}  // namespace

i32 javaStringHash(std::string_view text)
{
    u32 hash = 0;
    usize index = 0;
    while (index < text.size()) {
        const u32 code = nextCodePoint(text, &index);
        if (code < 0x10000) {
            hash = hash * 31u + code;
            continue;
        }
        // Java holds this as a surrogate pair and hashes both halves.
        const u32 rest = code - 0x10000;
        hash = hash * 31u + (0xD800 + (rest >> 10));
        hash = hash * 31u + (0xDC00 + (rest & 0x3FF));
    }
    return i32(hash);
}

bool seedFromText(std::string_view text, i64* out)
{
    const std::string_view seed = trim(text);
    if (seed.empty()) {
        return false;
    }

    // **Not strtoll**, for the reason coord_text gives at more length: it
    // accepts leading whitespace we have already handled, an 0x prefix nobody
    // typed on purpose, and it reports overflow through errno, which is easy to
    // forget to read. Here overflow is not an error at all -- it is precisely
    // the case Java's Long.parseLong throws on, and therefore the case that
    // falls through to the hash -- so it has to be detected, not clamped.
    usize index = 0;
    bool negative = false;
    if (seed[0] == '+' || seed[0] == '-') {
        negative = seed[0] == '-';
        index = 1;
    }

    bool numeric = index < seed.size();
    u64 magnitude = 0;
    for (usize i = index; i < seed.size() && numeric; ++i) {
        const char c = seed[i];
        if (c < '0' || c > '9') {
            numeric = false;
            break;
        }
        const u64 digit = u64(c - '0');
        // The limit is asymmetric: -9223372036854775808 is a valid long and
        // +9223372036854775808 is not, which is the off-by-one Long.parseLong
        // gets right by parsing negatives and negating at the end.
        const u64 limit = negative ? 0x8000000000000000ULL : 0x7FFFFFFFFFFFFFFFULL;
        if (magnitude > (limit - digit) / 10) {
            numeric = false;
            break;
        }
        magnitude = magnitude * 10 + digit;
    }

    if (numeric) {
        *out = negative ? i64(~magnitude + 1) : i64(magnitude);
        return true;
    }

    *out = i64(javaStringHash(seed));
    return true;
}

}  // namespace mc
