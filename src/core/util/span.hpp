#pragma once

// A minimal non-owning view over contiguous memory. C++17 has no std::span and
// the 3DS build has no exceptions, so this stays small and never throws:
// out-of-range access is a caller error that bounds-checked parsers must
// prevent, so the parsers do the checking, not this type.

#include "core/util/types.hpp"

#include <vector>

namespace mc {

template <typename T>
class Span {
public:
    constexpr Span() = default;
    constexpr Span(T* data, usize size) : data_(data), size_(size) {}

    template <typename U>
    Span(std::vector<U>& v) : data_(v.data()), size_(v.size()) {}

    template <typename U>
    Span(const std::vector<U>& v) : data_(v.data()), size_(v.size()) {}

    constexpr T* data() const { return data_; }
    constexpr usize size() const { return size_; }
    constexpr bool empty() const { return size_ == 0; }

    constexpr T* begin() const { return data_; }
    constexpr T* end() const { return data_ + size_; }

    constexpr T& operator[](usize i) const { return data_[i]; }

    // Unchecked -- callers clamp first. Used by parsers that have already
    // validated the range.
    constexpr Span subspan(usize offset, usize count) const
    {
        return Span(data_ + offset, count);
    }

private:
    T* data_ = nullptr;
    usize size_ = 0;
};

using ByteSpan = Span<u8>;
using ConstByteSpan = Span<const u8>;

}  // namespace mc
