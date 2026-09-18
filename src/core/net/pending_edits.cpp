// WorldClient's revert list. See pending_edits.hpp.

#include "core/net/pending_edits.hpp"

namespace mc::net {

void PendingEdits::record(i32 x, int y, i32 z, u16 oldBlock, u8 oldData)
{
    if (count_ == kCapacity) {
        removeAt(0);
    }
    entries_[count_++] = Entry{x, z, i16(y), oldBlock, oldData, u8(kRevertTicks)};
}

void PendingEdits::confirm(i32 x0, int y0, i32 z0, i32 x1, int y1, i32 z1)
{
    int kept = 0;
    for (int i = 0; i < count_; ++i) {
        const Entry& e = entries_[i];
        const bool inside = e.x >= x0 && e.y >= y0 && e.z >= z0 && e.x <= x1 && e.y <= y1
                            && e.z <= z1;
        if (!inside) {
            entries_[kept++] = e;
        }
    }
    count_ = kept;
}

void PendingEdits::tick(Revert revert, void* ctx)
{
    Entry expired[kCapacity];
    int expiredCount = 0;
    int kept = 0;
    for (int i = 0; i < count_; ++i) {
        Entry e = entries_[i];
        if (--e.ticks == 0) {
            expired[expiredCount++] = e;
        } else {
            entries_[kept++] = e;
        }
    }
    count_ = kept;

    for (int i = 0; i < expiredCount; ++i) {
        const Entry& e = expired[i];
        if (revert != nullptr) {
            revert(ctx, e.x, e.y, e.z, e.block, e.data);
        }
    }
}

void PendingEdits::removeAt(int index)
{
    for (int i = index + 1; i < count_; ++i) {
        entries_[i - 1] = entries_[i];
    }
    --count_;
}

}  // namespace mc::net
