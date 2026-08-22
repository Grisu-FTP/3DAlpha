#pragma once

// Packed 4-bit accessors, in Minecraft's packing order.
//
// Every nibble array in the game -- block metadata, block light, sky light, and
// our own 4-bit palette indices -- uses the same convention: an even index is
// the low nibble of its byte, an odd index the high nibble. Keeping our
// in-memory arrays in that exact packing is what lets the storage layer move
// them to and from disk with memcpy instead of unpacking 4096 values per
// section.

#include "core/util/types.hpp"

namespace mc {

inline u8 nibbleGet(const u8* packed, int index)
{
    const u8 byte = packed[index >> 1];
    return u8((index & 1) ? (byte >> 4) : (byte & 0x0F));
}

inline void nibbleSet(u8* packed, int index, u8 value)
{
    u8& byte = packed[index >> 1];
    if (index & 1) {
        byte = u8((byte & 0x0F) | u8(value << 4));
    } else {
        byte = u8((byte & 0xF0) | (value & 0x0F));
    }
}

// A byte holding the same nibble twice -- the fill pattern for a uniform plane.
inline constexpr u8 nibblePair(u8 value)
{
    return u8(((value & 0x0F) << 4) | (value & 0x0F));
}

}  // namespace mc
