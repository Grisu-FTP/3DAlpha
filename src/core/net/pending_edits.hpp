#pragma once

// A multiplayer client's own block edits are provisional for four seconds:
// unless the server says something about that block in the meantime, the edit
// is put back.
//
// This is `gs` -- WorldClient -- in a1.1.2. Its three write overrides
// (`c(IIII)`, `a(IIII)`, `a(IIIII)`) record the block and metadata being
// replaced as an `lc` with a countdown of **80 ticks**, and its tick walks the
// list, restoring any entry that reaches zero. `c(IIIIII)`, which runs first
// thing whenever the server sends a block change or a region, drops every entry
// inside the box it is given. So an edit the server accepts is confirmed by the
// server's own echo of it -- 0.2.1 answers every dig and every place with a
// Block Change for the target -- and one it refuses (spawn protection, a place
// it would not allow) snaps back instead of leaving the client in a world the
// server does not have.
//
// **Entries for the same block stack**, as they do in the original's LinkedList:
// breaking a block and then placing one in the hole records two, and if neither
// is confirmed they expire in order and the older one wins last. The list is
// bounded here, where the original's is not; past the bound the oldest entry is
// forgotten without being restored, which can only ever leave an edit standing.

#include "core/util/types.hpp"

namespace mc::net {

class PendingEdits {
public:
    static constexpr int kRevertTicks = 80;
    static constexpr int kCapacity = 256;

    void record(i32 x, int y, i32 z, u16 oldBlock, u8 oldData);

    // `c(IIIIII)`: forget every entry with x0 <= x <= x1 and so on, inclusive.
    void confirm(i32 x0, int y0, i32 z0, i32 x1, int y1, i32 z1);

    using Revert = void (*)(void* ctx, i32 x, int y, i32 z, u16 block, u8 data);

    // One tick. Entries that expire are removed first and restored after, in
    // the order they were recorded, so a restore that records a new entry --
    // it must not, but a caller could -- cannot disturb the walk.
    void tick(Revert revert, void* ctx);

    int count() const { return count_; }
    void clear() { count_ = 0; }

private:
    struct Entry {
        i32 x;
        i32 z;
        i16 y;
        u16 block;
        u8 data;
        u8 ticks;
    };

    void removeAt(int index);

    Entry entries_[kCapacity];
    int count_ = 0;
};

}  // namespace mc::net
