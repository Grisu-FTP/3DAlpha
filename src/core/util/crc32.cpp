#include "core/util/crc32.hpp"

#include <zlib.h>

namespace mc::util {

u32 crc32Seed()
{
    return u32(::crc32(0, nullptr, 0));
}

u32 crc32Update(u32 crc, ConstByteSpan data)
{
    if (data.empty()) {
        return crc;
    }
    // zlib takes the length as uInt, which is 32-bit; nothing here checksums
    // more than a chunk payload at a time, but the loop costs nothing and
    // removes the question.
    usize done = 0;
    while (done < data.size()) {
        const usize block = data.size() - done > 0x40000000u ? 0x40000000u : data.size() - done;
        crc = u32(::crc32(uLong(crc), data.data() + done, uInt(block)));
        done += block;
    }
    return crc;
}

u32 crc32(ConstByteSpan data)
{
    return crc32Update(crc32Seed(), data);
}

}  // namespace mc::util
