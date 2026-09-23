#include "core/world/section.hpp"

#include "core/block/registry.hpp"

#include <cstring>

namespace mc::world {
namespace {

constexpr usize kPalette4Max = 16;
constexpr usize kPalette8Max = 256;

}  // namespace

Section Section::clone() const
{
    Section copy;
    copy.encoding_ = encoding_;
    copy.uniform_ = uniform_;
    copy.palette_ = palette_;

    switch (encoding_) {
    case SectionEncoding::Uniform:
        break;
    case SectionEncoding::Palette4:
        copy.indices_.reset(new u8[kVolume / 2]);
        std::memcpy(copy.indices_.get(), indices_.get(), kVolume / 2);
        break;
    case SectionEncoding::Palette8:
        copy.indices_.reset(new u8[kVolume]);
        std::memcpy(copy.indices_.get(), indices_.get(), kVolume);
        break;
    case SectionEncoding::Direct16:
        copy.direct_.reset(new BlockId[kVolume]);
        std::memcpy(copy.direct_.get(), direct_.get(), kVolume * sizeof(BlockId));
        break;
    }

    copy.data_ = data_.clone();
    copy.blockLight_ = blockLight_.clone();
    copy.skyLight_ = skyLight_.clone();
    return copy;
}

void Section::setPalette(const BlockId* first, usize count)
{
    // Assigning into the existing vector keeps whatever capacity a wider
    // encoding left behind -- 512 bytes per section that ever reached Palette8,
    // which is the opposite of the point. shrink_to_fit does not help: it is
    // advisory and libstdc++ ignores it here. Replacing the vector outright is
    // the only form that actually releases the buffer.
    palette_ = count > 0 ? std::vector<BlockId>(first, first + count)
                         : std::vector<BlockId>();
}

int Section::findInPalette(BlockId id) const
{
    // Linear, because the palette is typically under a dozen entries and a
    // sorted palette would mean rewriting every index on each insertion.
    for (usize i = 0; i < palette_.size(); ++i) {
        if (palette_[i] == id) {
            return int(i);
        }
    }
    return -1;
}

void Section::setIndex(int i, int paletteIndex)
{
    if (encoding_ == SectionEncoding::Palette4) {
        nibbleSet(indices_.get(), i, u8(paletteIndex));
    } else {
        indices_[i] = u8(paletteIndex);
    }
}

void Section::promoteToPalette4()
{
    setPalette(&uniform_, 1);
    indices_.reset(new u8[kVolume / 2]);
    std::memset(indices_.get(), 0, kVolume / 2);
    encoding_ = SectionEncoding::Palette4;
}

void Section::promoteToPalette8()
{
    std::unique_ptr<u8[]> wide(new u8[kVolume]);
    for (int i = 0; i < kVolume; ++i) {
        wide[i] = nibbleGet(indices_.get(), i);
    }
    indices_ = std::move(wide);
    encoding_ = SectionEncoding::Palette8;
}

void Section::promoteToDirect16()
{
    std::unique_ptr<BlockId[]> direct(new BlockId[kVolume]);
    for (int i = 0; i < kVolume; ++i) {
        direct[i] = palette_[indices_[i]];
    }
    direct_ = std::move(direct);
    indices_.reset();
    setPalette(nullptr, 0);
    encoding_ = SectionEncoding::Direct16;
}

void Section::setBlock(int i, BlockId id)
{
    assert(i >= 0 && i < kVolume);

    if (encoding_ == SectionEncoding::Uniform) {
        if (id == uniform_) {
            return;
        }
        promoteToPalette4();
    }

    if (encoding_ == SectionEncoding::Direct16) {
        direct_[i] = id;
        return;
    }

    int slot = findInPalette(id);
    if (slot < 0) {
        const usize limit =
            (encoding_ == SectionEncoding::Palette4) ? kPalette4Max : kPalette8Max;
        if (palette_.size() == limit) {
            if (encoding_ == SectionEncoding::Palette4) {
                promoteToPalette8();
            } else {
                promoteToDirect16();
                direct_[i] = id;
                return;
            }
        }
        slot = int(palette_.size());
        palette_.push_back(id);
    }

    setIndex(i, slot);
}

void Section::adoptFlat(const BlockId* flat, const BlockId* distinct, usize distinctCount)
{
    indices_.reset();
    direct_.reset();

    if (distinctCount <= 1) {
        setPalette(nullptr, 0);
        encoding_ = SectionEncoding::Uniform;
        uniform_ = distinctCount == 1 ? distinct[0] : kAirBlock;
        return;
    }

    if (distinctCount > kPalette8Max) {
        setPalette(nullptr, 0);
        direct_.reset(new BlockId[kVolume]);
        std::memcpy(direct_.get(), flat, kVolume * sizeof(BlockId));
        encoding_ = SectionEncoding::Direct16;
        return;
    }

    setPalette(distinct, distinctCount);

    // Reverse map, so building the index array stays one pass instead of a
    // palette scan per block. 128 KB of stack is out of the question here, so
    // it is keyed by the position of the id within the sorted distinct list.
    const bool narrow = distinctCount <= kPalette4Max;
    encoding_ = narrow ? SectionEncoding::Palette4 : SectionEncoding::Palette8;
    indices_.reset(new u8[narrow ? kVolume / 2 : kVolume]);
    if (narrow) {
        std::memset(indices_.get(), 0, kVolume / 2);
    }

    for (int i = 0; i < kVolume; ++i) {
        // distinct is sorted, so this is a binary search rather than a scan.
        usize lo = 0, hi = distinctCount;
        while (lo + 1 < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (distinct[mid] <= flat[i]) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        setIndex(i, int(lo));
    }
}

bool Section::assignBlocks(ConstByteSpan flat)
{
    if (flat.size() != usize(kVolume)) {
        return false;
    }

    // One byte per block means at most 256 distinct values, so presence fits a
    // flat table and the distinct list comes out sorted for free.
    bool seen[256] = {};
    for (usize i = 0; i < usize(kVolume); ++i) {
        seen[flat[i]] = true;
    }

    BlockId distinct[256];
    usize distinctCount = 0;
    for (int id = 0; id < 256; ++id) {
        if (seen[id]) {
            distinct[distinctCount++] = BlockId(id);
        }
    }

    if (distinctCount <= 1) {
        adoptFlat(nullptr, distinct, distinctCount);
        return true;
    }

    // Reverse map from id to palette slot: the input alphabet is small enough
    // that this beats a search per block.
    u8 slotOf[256];
    for (usize s = 0; s < distinctCount; ++s) {
        slotOf[distinct[s]] = u8(s);
    }

    setPalette(distinct, distinctCount);
    direct_.reset();

    if (distinctCount <= kPalette4Max) {
        encoding_ = SectionEncoding::Palette4;
        indices_.reset(new u8[kVolume / 2]);
        for (int i = 0; i < kVolume; i += 2) {
            indices_[i >> 1] = u8(slotOf[flat[i]] | (slotOf[flat[i + 1]] << 4));
        }
    } else {
        encoding_ = SectionEncoding::Palette8;
        indices_.reset(new u8[kVolume]);
        for (int i = 0; i < kVolume; ++i) {
            indices_[i] = slotOf[flat[i]];
        }
    }
    return true;
}

void Section::writeBlocks(u8* dst) const
{
    // Mirrors readBlocks: the encoding switch and the palette/indices pointer
    // loads happen once here rather than once per cell inside block(i). That
    // dispatch measured at 46 % of section-mesh time before readBlocks was
    // rewritten this way (see its comment above) -- this is the same fix for
    // the save path, which walks the same kVolume cells on every dirty
    // section a save writes.
    switch (encoding_) {
    case SectionEncoding::Uniform:
        std::memset(dst, u8(uniform_), kVolume);
        return;
    case SectionEncoding::Palette4: {
        const u8* packed = indices_.get();
        const BlockId* palette = palette_.data();
        for (int i = 0; i < kVolume; ++i) {
            dst[i] = u8(palette[nibbleGet(packed, i)]);
        }
        return;
    }
    case SectionEncoding::Palette8: {
        const u8* indices = indices_.get();
        const BlockId* palette = palette_.data();
        for (int i = 0; i < kVolume; ++i) {
            dst[i] = u8(palette[indices[i]]);
        }
        return;
    }
    case SectionEncoding::Direct16: {
        const BlockId* direct = direct_.get();
        for (int i = 0; i < kVolume; ++i) {
            dst[i] = u8(direct[i]);
        }
        return;
    }
    }
}

void Section::readBlocks(int start, int count, BlockId* dst) const
{
    assert(start >= 0 && count >= 0 && start + count <= kVolume);

    switch (encoding_) {
    case SectionEncoding::Uniform:
        for (int i = 0; i < count; ++i) {
            dst[i] = uniform_;
        }
        return;
    case SectionEncoding::Palette4: {
        const u8* packed = indices_.get();
        const BlockId* palette = palette_.data();
        for (int i = 0; i < count; ++i) {
            dst[i] = palette[nibbleGet(packed, start + i)];
        }
        return;
    }
    case SectionEncoding::Palette8: {
        const u8* indices = indices_.get();
        const BlockId* palette = palette_.data();
        for (int i = 0; i < count; ++i) {
            dst[i] = palette[indices[start + i]];
        }
        return;
    }
    case SectionEncoding::Direct16:
        std::memcpy(dst, direct_.get() + start, usize(count) * sizeof(BlockId));
        return;
    }
}

void Section::readPackedLight(int start, int count, u8* dst) const
{
    assert(start >= 0 && count >= 0 && start + count <= kVolume);

    const u8* sky = skyLight_.bytes();
    const u8* block = blockLight_.bytes();

    // Both planes uniform is the ordinary case above the terrain and below it
    // alike, and it turns the whole run into one repeated byte.
    if (sky == nullptr && block == nullptr) {
        const u8 packed = u8((skyLight_.uniformValue() << 4) | blockLight_.uniformValue());
        for (int i = 0; i < count; ++i) {
            dst[i] = packed;
        }
        return;
    }

    const u8 skyUniform = skyLight_.uniformValue();
    const u8 blockUniform = blockLight_.uniformValue();
    for (int i = 0; i < count; ++i) {
        const int j = start + i;
        const u8 s = sky != nullptr ? nibbleGet(sky, j) : skyUniform;
        const u8 b = block != nullptr ? nibbleGet(block, j) : blockUniform;
        dst[i] = u8((s << 4) | b);
    }
}

void Section::readData(int start, int count, u8* dst) const
{
    assert(start >= 0 && count >= 0 && start + count <= kVolume);

    const u8* packed = data_.bytes();
    if (packed == nullptr) {
        const u8 uniform = data_.uniformValue();
        for (int i = 0; i < count; ++i) {
            dst[i] = uniform;
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        dst[i] = nibbleGet(packed, start + i);
    }
}

BlockId Section::maxBlockId() const
{
    switch (encoding_) {
    case SectionEncoding::Uniform:
        return uniform_;
    case SectionEncoding::Palette4:
    case SectionEncoding::Palette8: {
        BlockId highest = kAirBlock;
        for (BlockId id : palette_) {
            if (id > highest) {
                highest = id;
            }
        }
        return highest;
    }
    case SectionEncoding::Direct16: {
        BlockId highest = kAirBlock;
        for (int i = 0; i < kVolume; ++i) {
            if (direct_[i] > highest) {
                highest = direct_[i];
            }
        }
        return highest;
    }
    }
    return kAirBlock;
}

void Section::compact()
{
    data_.compact();
    blockLight_.compact();
    skyLight_.compact();

    if (encoding_ == SectionEncoding::Uniform) {
        return;
    }

    // Collect the ids actually present, kept sorted by insertion so adoptFlat's
    // binary search stays valid. Real sections hold a handful of distinct
    // blocks, so the insertion cost is far below a 4096-entry sort.
    BlockId distinct[kPalette8Max + 1];
    usize distinctCount = 0;
    bool overflow = false;

    std::unique_ptr<BlockId[]> flat(new BlockId[kVolume]);
    for (int i = 0; i < kVolume; ++i) {
        const BlockId id = block(i);
        flat[i] = id;
        if (overflow) {
            continue;
        }

        usize lo = 0, hi = distinctCount;
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (distinct[mid] < id) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo < distinctCount && distinct[lo] == id) {
            continue;
        }
        if (distinctCount == kPalette8Max + 1) {
            overflow = true;
            continue;
        }
        for (usize s = distinctCount; s > lo; --s) {
            distinct[s] = distinct[s - 1];
        }
        distinct[lo] = id;
        distinctCount++;
    }

    adoptFlat(flat.get(), distinct, overflow ? kPalette8Max + 1 : distinctCount);
}

bool Section::mayTickRandomly() const
{
    switch (encoding_) {
    case SectionEncoding::Uniform:
        return block::ticksRandomly(uniform_);
    case SectionEncoding::Palette4:
    case SectionEncoding::Palette8:
        for (BlockId id : palette_) {
            if (block::ticksRandomly(id)) {
                return true;
            }
        }
        return false;
    case SectionEncoding::Direct16:
        return true;
    }
    return true;
}

bool Section::mayHoldTileEntity() const
{
    switch (encoding_) {
    case SectionEncoding::Uniform:
        return block::tileEntityBearing(block::def(uniform_).tick);
    case SectionEncoding::Palette4:
    case SectionEncoding::Palette8:
        for (BlockId id : palette_) {
            if (block::tileEntityBearing(block::def(id).tick)) {
                return true;
            }
        }
        return false;
    case SectionEncoding::Direct16:
        return true;
    }
    return true;
}

usize Section::memoryUsage() const
{
    usize bytes = palette_.capacity() * sizeof(BlockId);
    switch (encoding_) {
    case SectionEncoding::Uniform:  break;
    case SectionEncoding::Palette4: bytes += kVolume / 2; break;
    case SectionEncoding::Palette8: bytes += kVolume; break;
    case SectionEncoding::Direct16: bytes += kVolume * sizeof(BlockId); break;
    }
    return bytes + data_.memoryUsage() + blockLight_.memoryUsage() +
           skyLight_.memoryUsage();
}

}  // namespace mc::world
