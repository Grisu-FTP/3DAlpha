#pragma once

// Fixed-width types for core code.
//
// These deliberately mirror libctru's spelling (u8/u16/u32/u64, plus i* for the
// signed forms) so 3DS platform code reads the same as core code -- but they
// live in namespace mc, so core never depends on <3ds.h> and the host build has
// no conflicting global typedefs.

#include <cstddef>
#include <cstdint>

namespace mc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using usize = std::size_t;

}  // namespace mc
