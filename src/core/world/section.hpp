#pragma once

// A 16^3 section: the unit of block storage, meshing and lighting.
//
// A raw Alpha column is 80 KB, so at render distance 8 the block data alone
// would be 23 MB on a 64 MB device. Sections fix that in two ways, and the
// second matters more than the first:
//
//   * Block ids are palette-compressed: 4 bits per block while the section
//     holds 16 or fewer distinct blocks, 8 bits up to 256, and a plain 16-bit
//     array beyond that so correctness never depends on the palette fitting.
//   * A section that holds a single block -- overwhelmingly air, but also solid
//     rock deep underground -- stores no array at all. Most of the eight
//     sections in a typical column are in that state. See
//     docs/world-format.md for the resulting per-column figures.
//
// Index order is Y-fastest (index = y + z*16 + x*256), which is the order the
// Alpha column arrays already use. A section's slice of a column is therefore
// 256 contiguous 16-byte runs, moved with memcpy rather than unpacked block by
// block. Changing this order would cost a transpose on every chunk load.

#include "core/block/block_def.hpp"
#include "core/util/span.hpp"
#include "core/util/types.hpp"
#include "core/world/nibble_array.hpp"
#include "version_config.hpp"

#include <cassert>
#include <memory>
#include <vector>

namespace mc::world {

// Block identity is the block layer's to define -- see core/block/block_def.hpp
// for why it is a plain integer and not the generated enum. Re-exported here
// because world code reads far better saying `BlockId` than `block::BlockId`.
using block::BlockId;
inline constexpr BlockId kAirBlock = block::kAir;

enum class SectionEncoding : u8 {
    Uniform,   // one block id, no array
    Palette4,  // <=16 distinct ids, 4 bits each  (2048 bytes)
    Palette8,  // <=256 distinct ids, 8 bits each (4096 bytes)
    Direct16,  // no palette, one u16 per block   (8192 bytes)
};

class Section {
public:
    static constexpr int kSize = mcver::kSectionSize;
    static constexpr int kVolume = kSize * kSize * kSize;

    static_assert(kVolume == NibbleArray::kCount,
                  "the nibble planes are sized for exactly one section");

    static constexpr int index(int x, int y, int z)
    {
        return y + z * kSize + x * kSize * kSize;
    }

    Section() = default;
    explicit Section(BlockId uniform) : uniform_(uniform) {}

    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;
    Section(Section&&) = default;
    Section& operator=(Section&&) = default;

    Section clone() const;

    SectionEncoding encoding() const { return encoding_; }
    bool isUniform() const { return encoding_ == SectionEncoding::Uniform; }
    // Meaningful only while uniform.
    BlockId uniformBlock() const { return uniform_; }
    bool isUniformAir() const { return isUniform() && uniform_ == kAirBlock; }

    BlockId block(int i) const
    {
        assert(i >= 0 && i < kVolume);
        switch (encoding_) {
        case SectionEncoding::Uniform:  return uniform_;
        case SectionEncoding::Palette4: return palette_[nibbleGet(indices_.get(), i)];
        case SectionEncoding::Palette8: return palette_[indices_[i]];
        case SectionEncoding::Direct16: return direct_[i];
        }
        return kAirBlock;
    }

    BlockId block(int x, int y, int z) const { return block(index(x, y, z)); }

    void setBlock(int i, BlockId id);
    void setBlock(int x, int y, int z, BlockId id) { setBlock(index(x, y, z), id); }

    // Copies `count` blocks from flat index `start`.
    //
    // The point is what does *not* happen per block: the encoding switch, the
    // palette pointer load, and -- at the caller -- ChunkColumn's bounds check
    // and division by the section height. The mesher reads 4,096 interior
    // blocks per section and was paying all of that on every one of them, which
    // measured at 46 % of the time to mesh a section. Because both this class
    // and MeshScratch index Y-fastest, one call moves a whole 16-block Y run.
    void readBlocks(int start, int count, BlockId* dst) const;

    // The (sky << 4) | block byte the vertex carries, for the same run. Both
    // planes are read together because the mesher never wants one without the
    // other, and because either being uniform -- which is the common case, 87 %
    // of planes -- then collapses the whole run to a constant.
    void readPackedLight(int start, int count, u8* dst) const;

    // The metadata nibble for the same run. Separate from the light pair
    // because most sections want light and never look at metadata: only the
    // shapes that bend to it -- fluid levels, which way a door swings, a rail's
    // curve -- read this at all.
    void readData(int start, int count, u8* dst) const;

    NibbleArray& data() { return data_; }
    NibbleArray& blockLight() { return blockLight_; }
    NibbleArray& skyLight() { return skyLight_; }
    const NibbleArray& data() const { return data_; }
    const NibbleArray& blockLight() const { return blockLight_; }
    const NibbleArray& skyLight() const { return skyLight_; }

    // Bulk load from one byte per block, the form every pre-Anvil format uses
    // on disk and on the wire. Chooses the tightest encoding for the input, so
    // a freshly loaded chunk is already as small as it will get. Returns false
    // if the span is not exactly kVolume bytes.
    bool assignBlocks(ConstByteSpan flat);

    // Bulk store into kVolume caller-owned bytes. Ids above 255 truncate, which
    // is why formats limited to a byte must check maxBlockId() first rather
    // than discover the loss afterwards.
    void writeBlocks(u8* dst) const;

    BlockId maxBlockId() const;

    // Recomputes the palette from the actual contents and drops to the smallest
    // encoding that fits. Nothing calls this on the edit path -- setBlock only
    // ever grows the palette, exactly like the original game -- so it exists for
    // after a bulk edit, and before a save, when a section that was mined out or
    // filled in would otherwise keep an array it no longer needs.
    void compact();

    usize memoryUsage() const;

private:
    void promoteToPalette4();
    void promoteToPalette8();
    void promoteToDirect16();
    // Replaces the palette outright rather than assigning into it, so a section
    // that shrinks actually releases the buffer.
    void setPalette(const BlockId* first, usize count);
    // Index of id in the palette, or -1.
    int findInPalette(BlockId id) const;
    void setIndex(int i, int paletteIndex);
    void adoptFlat(const BlockId* flat, const BlockId* distinct, usize distinctCount);

    SectionEncoding encoding_ = SectionEncoding::Uniform;
    BlockId uniform_ = kAirBlock;
    std::vector<BlockId> palette_;
    std::unique_ptr<u8[]> indices_;      // Palette4: kVolume/2, Palette8: kVolume
    std::unique_ptr<BlockId[]> direct_;  // Direct16: kVolume entries

    NibbleArray data_;
    NibbleArray blockLight_;
    NibbleArray skyLight_;
};

}  // namespace mc::world
