// See sign_store.hpp.

#include "core/world/sign_store.hpp"

#include <cstring>

namespace mc::world {

int SignStore::find(i32 x, int y, i32 z) const
{
    for (int i = 0; i < signs_.size(); ++i) {
        const SignText& s = signs_[i];
        if (s.used && s.x == x && s.y == y && s.z == z) {
            return i;
        }
    }
    return -1;
}

int SignStore::put(i32 x, int y, i32 z, bool wall, u8 metadata)
{
    // **Replacing rather than adding**, when there is already one here. A block
    // broken and rebuilt in the same hole is a different sign; `erase` is what
    // makes that true, and this is the case where it was not called.
    const int existing = find(x, y, z);
    if (existing >= 0) {
        signs_[existing].wall = wall;
        signs_[existing].metadata = metadata;
        justPlaced_ = existing;
        return existing;
    }

    SignText* slot = signs_.push();
    if (slot == nullptr) {
        ++refused_;
        return -1;
    }

    SignText& s = *slot;
    s.x = x;
    s.y = y;
    s.z = z;
    s.wall = wall;
    s.metadata = metadata;
    s.used = true;
    justPlaced_ = signs_.size() - 1;
    return justPlaced_;
}

void SignStore::erase(i32 x, int y, i32 z)
{
    const int index = find(x, y, z);
    if (index < 0) {
        return;
    }
    signs_.swapRemove(index);
    signs_.trim();
}

void SignStore::setLine(int index, int line, std::string_view text)
{
    if (index < 0 || index >= signs_.size() || line < 0 || line >= kSignLines) {
        return;
    }
    char* dst = signs_[index].lines[line];
    // **Truncated by bytes, and that is the editor's rule too**: `GuiEditSign`
    // counts characters and this counts bytes, which agree for everything the
    // font can draw -- the glyph table is 144 entries starting at the space and
    // nothing above U+00FF is in it.
    usize n = text.size();
    if (n > usize(kSignLineLength)) {
        n = usize(kSignLineLength);
    }
    std::memcpy(dst, text.data(), n);
    dst[n] = '\0';
}

}  // namespace mc::world
