#pragma once

// Which of the two on-disk shapes a world folder is in.
//
// **The state is the disk, not a setting.** There is no stored preference and
// no registration step: the mode is read off the folder every time it is
// needed, so it cannot desync from reality, and a folder dropped onto the card
// from a PC is recognised the moment it appears.
//
//   `world.3dm` present         => Packed
//   `level.dat` present         => Folder
//   neither                     => Unknown, which is not a world
//
// Packed is checked first. A folder holding both is not a shape this code
// writes -- a packed world keeps its level.dat inside the manifest -- so it can
// only be a half-finished conversion, and reading it as packed is the reading
// that does not hand a caller a level with no chunks under it.

#include "core/io/file_system.hpp"

#include <string_view>

namespace mc::world {

enum class WorldFormat {
    Unknown,
    Folder,
    Packed,
};

WorldFormat detectFormat(io::FileSystem& fs, std::string_view worldDir);

// "Folder" / "Packed" -- what the world options screen draws.
const char* formatName(WorldFormat format);

}  // namespace mc::world
