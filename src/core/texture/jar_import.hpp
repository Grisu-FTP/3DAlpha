#pragma once

// Turning a player's own minecraft.jar into a texture pack.
//
// a1.1.2 predates the in-game texture-pack selector -- that arrived in a1.2.2 --
// so "an a1.1.2 texture pack" means the pre-1.5 jar layout, which is what every
// alpha- and beta-era pack uses. A jar already *is* that layout: the extraction
// is therefore not a conversion but a filter, keeping the textures and dropping
// the code.
//
// **Entries are copied verbatim.** Method, CRC and both sizes come out of the
// source's central directory and the compressed bytes are re-emitted untouched,
// so a 900 KB jar becomes a pack without one inflate or deflate call. That is
// what makes this fast enough to do on a 268 MHz ARM11 without a progress bar
// that means anything.
//
// **The rule is "every .png", not an allow-list.** It is simpler, it is exactly
// the 58 files a real a1.1.2 jar holds, and it keeps working on a jar from a
// version whose layout we have not measured. META-INF and anything with an
// unsafe name are dropped.
//
// Licensing: nothing here is redistributed and nothing enters the repository or
// the build. The player's own file is filtered into another file on the player's
// own card. See docs/assets.md.

#include "core/io/file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>

namespace mc::texture {

struct ImportResult {
    PackError error = PackError::Ok;
    // Set when `error` is BadPng or the archive would not open, so the caller
    // can say which of the two went wrong without re-deriving it.
    std::string outPath;
    int copied = 0;   // png entries written
    int skipped = 0;  // entries that were not png, or whose name was unsafe
    int known = 0;    // how many of kA112Files came across

    bool ok() const { return error == PackError::Ok; }
};

// Reads `jarPath`, writes `packsDir/<sanitised jar stem>.zip`, and then
// **verifies the result before calling it a success**: the written pack is
// re-opened, its terrain.png decoded, and the atlas built from it. Only a pack
// that survives that round trip is reported as ok.
//
// That verification is not belt-and-braces. It is the thing that makes offering
// to delete the source jar afterwards a safe thing to do, and the caller must
// not offer that on any other basis.
ImportResult importJar(io::FileSystem& fs, std::string_view jarPath,
                       std::string_view packsDir);

// The pack file name a jar would produce, without doing the import. The menu
// uses it to say what is about to be written. Empty when the jar's name
// sanitises to nothing.
std::string packNameForJar(std::string_view jarPath);

}  // namespace mc::texture
