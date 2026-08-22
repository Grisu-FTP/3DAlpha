#include "impl/storage/alpha_chunkfiles/chunk_path.hpp"

#include <cstring>

namespace mc::alpha {

namespace {

constexpr char kDigits[] = "0123456789abcdefghijklmnopqrstuvwxyz";

// The directory component is base36 of the low 6 bits of the two's-complement
// pattern, which is always 0..63 and so never signed.
u32 directoryKey(i32 value)
{
    return static_cast<u32>(value) & 63u;
}

usize appendUnsigned(u32 value, char* out)
{
    char reversed[kMaxBase36];
    usize count = 0;
    do {
        reversed[count++] = kDigits[value % 36];
        value /= 36;
    } while (value != 0);

    for (usize i = 0; i < count; ++i) {
        out[i] = reversed[count - 1 - i];
    }
    return count;
}

struct Appender {
    char* out;
    usize length = 0;
    bool overflow = false;

    void put(std::string_view text)
    {
        if (overflow || length + text.size() + 1 > kMaxChunkPathLength) {
            overflow = true;
            return;
        }
        std::memcpy(out + length, text.data(), text.size());
        length += text.size();
    }

    void put(char c) { put(std::string_view(&c, 1)); }

    void putBase36(i32 value)
    {
        if (overflow || length + kMaxBase36 + 1 > kMaxChunkPathLength) {
            overflow = true;
            return;
        }
        length += base36(value, out + length);
    }

    void putDirectory(i32 value)
    {
        if (overflow || length + kMaxBase36 + 1 > kMaxChunkPathLength) {
            overflow = true;
            return;
        }
        length += appendUnsigned(directoryKey(value), out + length);
    }

    bool finish(usize* lengthOut)
    {
        if (overflow) {
            return false;
        }
        out[length] = '\0';
        *lengthOut = length;
        return true;
    }
};

}  // namespace

usize base36(i32 value, char* out)
{
    usize offset = 0;
    // Negating i32 min overflows, so widen before taking the magnitude.
    i64 wide = value;
    if (wide < 0) {
        out[offset++] = '-';
        wide = -wide;
    }
    offset += appendUnsigned(static_cast<u32>(wide), out + offset);
    out[offset] = '\0';
    return offset;
}

bool parseBase36(std::string_view text, i32* out)
{
    if (text.empty()) {
        return false;
    }

    bool negative = false;
    usize index = 0;
    if (text[0] == '-') {
        negative = true;
        index = 1;
        if (text.size() == 1) {
            return false;
        }
    }

    i64 value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        i32 digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'z') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'Z') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        value = value * 36 + digit;
        // Bail as soon as the magnitude cannot be an i32, so a long digit run
        // cannot wrap around into a plausible-looking coordinate.
        if (value > 0x80000000LL) {
            return false;
        }
    }

    if (negative) {
        value = -value;
    }
    if (value > 0x7FFFFFFFLL || value < -0x80000000LL) {
        return false;
    }
    *out = static_cast<i32>(value);
    return true;
}

bool chunkDirPath(std::string_view worldDir, i32 chunkX, i32 chunkZ, ChunkPath* out)
{
    Appender appender{out->text};
    appender.put(worldDir);
    appender.put('/');
    appender.putDirectory(chunkX);
    appender.put('/');
    appender.putDirectory(chunkZ);
    return appender.finish(&out->length);
}

bool chunkFilePath(std::string_view worldDir, i32 chunkX, i32 chunkZ, ChunkPath* out)
{
    Appender appender{out->text};
    appender.put(worldDir);
    appender.put('/');
    appender.putDirectory(chunkX);
    appender.put('/');
    appender.putDirectory(chunkZ);
    appender.put("/c.");
    appender.putBase36(chunkX);
    appender.put('.');
    appender.putBase36(chunkZ);
    appender.put(".dat");
    return appender.finish(&out->length);
}

bool parseChunkFileName(std::string_view fileName, i32* chunkX, i32* chunkZ)
{
    constexpr std::string_view kPrefix = "c.";
    constexpr std::string_view kSuffix = ".dat";

    if (fileName.size() <= kPrefix.size() + kSuffix.size()) {
        return false;
    }
    if (fileName.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    if (fileName.compare(fileName.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
        return false;
    }

    const std::string_view body =
        fileName.substr(kPrefix.size(), fileName.size() - kPrefix.size() - kSuffix.size());
    const usize dot = body.find('.');
    if (dot == std::string_view::npos) {
        return false;
    }

    return parseBase36(body.substr(0, dot), chunkX) &&
           parseBase36(body.substr(dot + 1), chunkZ);
}

}  // namespace mc::alpha
