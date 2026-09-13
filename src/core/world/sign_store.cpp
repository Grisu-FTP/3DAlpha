// See sign_store.hpp.

#include "core/world/sign_store.hpp"

#include "core/block/registry.hpp"
#include "core/world/chunk.hpp"
#include "core/world/tile_entity.hpp"

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

void SignStore::eraseColumn(i32 chunkX, i32 chunkZ)
{
    const i32 minX = chunkX * ChunkColumn::kWidth;
    const i32 minZ = chunkZ * ChunkColumn::kWidth;
    for (int i = signs_.size() - 1; i >= 0; --i) {
        const SignText& s = signs_[i];
        if (s.x < minX || s.x >= minX + ChunkColumn::kWidth || s.z < minZ ||
            s.z >= minZ + ChunkColumn::kWidth) {
            continue;
        }
        signs_.swapRemove(i);
    }
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
    // `memcpy` with a null source is undefined even for zero bytes, and a
    // default-constructed `string_view` has one.
    if (n > 0) {
        std::memcpy(dst, text.data(), n);
    }
    dst[n] = '\0';
}

int readSigns(const ChunkColumn& column, SignStore& store)
{
    int taken = 0;
    for (const TileEntity& tile : column.tileEntities) {
        if (tile.kind != TileEntityKind::Sign) {
            continue;
        }
        const int lx = int(tile.x - column.x * ChunkColumn::kWidth);
        const int lz = int(tile.z - column.z * ChunkColumn::kWidth);
        if (lx < 0 || lx >= ChunkColumn::kWidth || lz < 0 || lz >= ChunkColumn::kWidth ||
            tile.y < 0 || tile.y >= ChunkColumn::kHeight) {
            continue;
        }
        const block::TickBehaviour behaviour =
            block::def(column.block(lx, tile.y, lz)).tick;
        if (behaviour != block::TickBehaviour::SignPost &&
            behaviour != block::TickBehaviour::SignWall) {
            continue;
        }
        const int index =
            store.put(tile.x, tile.y, tile.z, behaviour == block::TickBehaviour::SignWall,
                      column.blockData(lx, tile.y, lz));
        if (index < 0) {
            continue;  // the heap refused it; counted by SignStore::refused()
        }
        for (int line = 0; line < kSignLines; ++line) {
            store.setLine(index, line, tile.lines[line]);
        }
        ++taken;
    }
    // **Taken, not claimed**: `justPlaced_` is the seam that puts a keyboard
    // up, and a column arriving must not open one.
    store.takeJustPlaced();
    return taken;
}

int writeSigns(const SignStore& store, ChunkColumn& column)
{
    const i32 minX = column.x * ChunkColumn::kWidth;
    const i32 minZ = column.z * ChunkColumn::kWidth;

    int written = 0;
    for (int i = 0; i < store.count(); ++i) {
        const SignText& s = store[i];
        if (!s.used) {
            continue;
        }
        if (s.x < minX || s.x >= minX + ChunkColumn::kWidth || s.z < minZ ||
            s.z >= minZ + ChunkColumn::kWidth) {
            continue;
        }
        TileEntity* tile = findTileEntity(column.tileEntities, s.x, s.y, s.z);
        if (tile == nullptr || tile->kind != TileEntityKind::Sign) {
            tile = &putTileEntity(column.tileEntities, s.x, s.y, s.z, TileEntityKind::Sign);
        }
        for (int line = 0; line < kSignLines; ++line) {
            setTileSignLine(*tile, line, s.lines[line]);
        }
        ++written;
    }
    return written;
}

}  // namespace mc::world
