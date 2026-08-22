#include "core/world/nibble_array.hpp"

#include <cassert>
#include <cstring>

namespace mc::world {

u8* NibbleArray::materialise()
{
    if (!data_) {
        data_.reset(new u8[kBytes]);
        std::memset(data_.get(), nibblePair(uniform_), kBytes);
    }
    return data_.get();
}

NibbleArray NibbleArray::clone() const
{
    NibbleArray copy(uniform_);
    if (data_) {
        std::memcpy(copy.materialise(), data_.get(), kBytes);
    }
    return copy;
}

void NibbleArray::set(int index, u8 value)
{
    assert(index >= 0 && index < kCount);
    value = u8(value & 0x0F);
    // Writing the value a uniform plane already holds must not allocate --
    // relighting rewrites whole sections with the value they already have.
    if (!data_ && value == uniform_) {
        return;
    }
    nibbleSet(materialise(), index, value);
}

void NibbleArray::fill(u8 value)
{
    data_.reset();
    uniform_ = u8(value & 0x0F);
}

bool NibbleArray::assign(ConstByteSpan packed)
{
    if (packed.size() != usize(kBytes)) {
        return false;
    }

    const u8 first = packed[0];
    bool uniform = (u8(first >> 4) == u8(first & 0x0F));
    for (usize i = 1; uniform && i < usize(kBytes); ++i) {
        uniform = (packed[i] == first);
    }

    if (uniform) {
        fill(u8(first & 0x0F));
        return true;
    }

    data_.reset(new u8[kBytes]);
    std::memcpy(data_.get(), packed.data(), kBytes);
    return true;
}

void NibbleArray::compact()
{
    if (!data_) {
        return;
    }

    const u8 first = data_[0];
    if (u8(first >> 4) != u8(first & 0x0F)) {
        return;
    }
    for (int i = 1; i < kBytes; ++i) {
        if (data_[i] != first) {
            return;
        }
    }

    fill(u8(first & 0x0F));
}

void NibbleArray::writeTo(u8* dst) const
{
    if (data_) {
        std::memcpy(dst, data_.get(), kBytes);
    } else {
        std::memset(dst, nibblePair(uniform_), kBytes);
    }
}

}  // namespace mc::world
