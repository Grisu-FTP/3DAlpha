#pragma once

// A PNG decoder, scoped to what a texture pack actually contains.
//
// **The scope is measured, not guessed.** Every one of the 58 PNGs in a real
// a1.1.2 client jar is bit depth 8 and non-interlaced; 54 are colour type 6
// (RGBA) and 4 are colour type 3 (palette). So this decodes:
//
//     bit depth 8, non-interlaced, colour types 0/2/3/4/6, PLTE and tRNS
//
// and refuses everything else with a reason that names what it found. Refusing
// loudly is the point: a pack that silently decoded wrong would show up as a
// world with the wrong colours in it, which is a much longer walk back to the
// cause than "16-bit PNGs are not supported".
//
// Interlacing, 16-bit samples and sub-byte palettes are the three real
// omissions. All three are easy to add on top of this and none of them has ever
// appeared in a pack we can find, so they are refusals rather than dead code.
//
// The inflate comes from core/util/compress.hpp -- a PNG's IDAT stream is
// zlib-wrapped, which is one of the three framings that file already offers, so
// this adds no dependency at all.

#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <vector>

namespace mc::texture {

// Why a decode was refused. Ordered roughly by how early it is detected.
enum class PngError {
    Ok,
    NotPng,          // no signature, or a truncated header
    Truncated,       // a chunk claims more bytes than the file holds
    BadHeader,       // zero dimensions, or a size that would overflow
    UnsupportedDepth,      // not 8 bits per sample
    UnsupportedColour,     // not one of 0/2/3/4/6
    UnsupportedInterlace,  // Adam7
    UnsupportedCompression,  // a filter or compression method PNG does not define
    MissingPalette,  // colour type 3 with no PLTE
    BadFilter,       // a scanline filter byte outside 0..4
    InflateFailed,   // the IDAT stream is corrupt
    WrongSize,       // the inflated data is not width*height*channels + height
    TooLarge,        // beyond the caller's ceiling
};

const char* pngErrorText(PngError error);

struct Image {
    int width = 0;
    int height = 0;
    // width*height*4 bytes, R,G,B,A in **memory order** -- the order PNG itself
    // uses. The 3DS's GPU_RGBA8 wants the reverse; the reversal happens once,
    // at upload, in platform/ctr/textures.cpp. See the note there.
    std::vector<u8> rgba;

    bool empty() const { return rgba.empty(); }
};

// A pack's terrain.png at 64x per tile is 1024x1024, which is 4 MB decoded. The
// ceiling is on the decoded size rather than the file size, because that is the
// allocation a hostile file would be asking for.
inline constexpr usize kDefaultMaxPixels = 2048u * 2048u;

// Decodes into *out. On failure *out is left empty and the reason is returned.
PngError decodePng(ConstByteSpan in, Image* out, usize maxPixels = kDefaultMaxPixels);

}  // namespace mc::texture
