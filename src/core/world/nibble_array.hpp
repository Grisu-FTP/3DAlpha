#pragma once

// One 4-bit plane of a 16^3 section: metadata, block light, or sky light.
//
// The representation is either a single uniform value costing no storage at
// all, or a materialised 2048-byte array in Minecraft's nibble packing. The
// uniform case is not an optimisation for rare inputs -- it is the common case.
// Above the terrain surface every section is air with sky light 15 and metadata
// 0; below it, solid rock has sky light 0 and block light 0. Three planes at
// 2 KB each is 6 KB per section, so collapsing them is worth more than the
// block palette on a typical column.
//
// Copies are explicit (clone()) rather than implicit. A hidden 2 KB memcpy in a
// mesh or save path is exactly the kind of cost that disappears into a profile
// on this device, so it has to be visible at the call site.

#include "core/util/nibble.hpp"
#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <memory>

namespace mc::world {

class NibbleArray {
public:
    static constexpr int kCount = 4096;
    static constexpr int kBytes = kCount / 2;

    NibbleArray() = default;
    explicit NibbleArray(u8 uniform) : uniform_(u8(uniform & 0x0F)) {}

    NibbleArray(const NibbleArray&) = delete;
    NibbleArray& operator=(const NibbleArray&) = delete;
    NibbleArray(NibbleArray&&) = default;
    NibbleArray& operator=(NibbleArray&&) = default;

    NibbleArray clone() const;

    bool isUniform() const { return data_ == nullptr; }
    u8 uniformValue() const { return uniform_; }

    u8 get(int index) const
    {
        return data_ ? nibbleGet(data_.get(), index) : uniform_;
    }

    void set(int index, u8 value);

    // Collapses back to uniform, releasing the array.
    void fill(u8 value);

    // Replaces the contents from 2048 packed bytes, collapsing to uniform when
    // every nibble matches. This is the load path: the storage layer gathers a
    // section's slice of a column array and hands it over, and sections that
    // are uniform on disk cost nothing in memory without a separate check.
    bool assign(ConstByteSpan packed);

    // Expands into 2048 caller-owned bytes, uniform or not. The save path.
    void writeTo(u8* dst) const;

    // Collapses back to uniform if every nibble now matches. set() cannot do
    // this itself -- it would have to rescan 2048 bytes per write -- so a plane
    // filled one block at a time by worldgen or the lighting engine keeps its
    // array until something asks. Sky light above the terrain is the case that
    // matters: those sections are otherwise empty, and the plane is all they
    // cost.
    void compact();

    // Null while uniform.
    const u8* bytes() const { return data_.get(); }

    usize memoryUsage() const { return data_ ? usize(kBytes) : 0; }

private:
    u8* materialise();

    std::unique_ptr<u8[]> data_;
    u8 uniform_ = 0;
};

}  // namespace mc::world
