#pragma once

// Alpha chunk file paths.
//
//   <world>/<b36(x & 63)>/<b36(z & 63)>/c.<b36(x)>.<b36(z)>.dat
//
// Two different base36 encodings appear in one path, and mixing them up is the
// classic way to write a world PC Minecraft cannot open:
//
//   * The directory components mask with 63 first. The mask happens on the
//     two's-complement bit pattern, so x = -13 gives 51 -> "1f", never "-d".
//   * The filename keeps the signed value, written as '-' followed by the
//     base36 of the magnitude: x = -13 -> "-d".
//
// So chunk (-13, 44) lives at "1f/18/c.-d.18.dat".
//
// Paths are built into a caller-provided buffer rather than a std::string:
// loading a render-distance-8 area touches 289 of these, and none of them
// should allocate.

#include "core/util/types.hpp"

#include <string_view>

namespace mc::alpha {

// Longest possible: 6 base36 digits for a full i32 magnitude, plus sign.
inline constexpr usize kMaxBase36 = 7;
// "<dir>/<2>/<2>/c.<7>.<7>.dat" plus a terminator, with room for a world path.
inline constexpr usize kMaxChunkPathLength = 256;

// Writes the lowercase base36 form of value into out, which must hold at least
// kMaxBase36 + 1 bytes. Returns the number of characters written, terminator
// excluded. Negative values get a leading '-', matching Java's
// Integer.toString(i, 36).
usize base36(i32 value, char* out);

// Parses lowercase or uppercase base36. Returns false on an empty string, a
// stray character, or a value that does not fit an i32.
bool parseBase36(std::string_view text, i32* out);

struct ChunkPath {
    char text[kMaxChunkPathLength];
    usize length;

    std::string_view view() const { return std::string_view(text, length); }
};

// Builds "<worldDir>/<b36(x&63)>/<b36(z&63)>/c.<b36(x)>.<b36(z)>.dat".
// Returns false if worldDir is long enough to overflow the buffer.
bool chunkFilePath(std::string_view worldDir, i32 chunkX, i32 chunkZ, ChunkPath* out);

// Just the two directory components, for creating them before a write.
bool chunkDirPath(std::string_view worldDir, i32 chunkX, i32 chunkZ, ChunkPath* out);

// Recovers the chunk coordinates from a "c.<x>.<z>.dat" filename. The world
// index scan uses this: the filename is authoritative, and a file whose name
// disagrees with the xPos/zPos inside it is corrupt.
bool parseChunkFileName(std::string_view fileName, i32* chunkX, i32* chunkZ);

}  // namespace mc::alpha
