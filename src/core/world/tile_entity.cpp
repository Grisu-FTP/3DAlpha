#include "core/world/tile_entity.hpp"

#include "core/block/registry.hpp"
#include "core/world/chunk.hpp"

#include <cstring>

namespace mc::world {
namespace {

// `ic`'s static initialiser, in its own order.
constexpr const char* kIds[] = {"Furnace", "Chest", "Sign", "MobSpawner"};

}  // namespace

const char* tileEntityId(TileEntityKind kind)
{
    const int i = int(kind);
    if (i < 0 || i >= int(sizeof(kIds) / sizeof(kIds[0]))) {
        return nullptr;
    }
    return kIds[i];
}

bool tileEntityKindFromId(std::string_view id, TileEntityKind* out)
{
    for (int i = 0; i < int(sizeof(kIds) / sizeof(kIds[0])); ++i) {
        if (id == kIds[i]) {
            *out = TileEntityKind(i);
            return true;
        }
    }
    return false;
}

bool tileEntityKindForBlock(block::TickBehaviour behaviour, TileEntityKind* out)
{
    switch (behaviour) {
    case block::TickBehaviour::Chest:      *out = TileEntityKind::Chest; return true;
    case block::TickBehaviour::Furnace:    *out = TileEntityKind::Furnace; return true;
    case block::TickBehaviour::SignPost:
    case block::TickBehaviour::SignWall:   *out = TileEntityKind::Sign; return true;
    case block::TickBehaviour::MobSpawner: *out = TileEntityKind::MobSpawner; return true;
    default:                               return false;
    }
}

TileEntity makeTileEntity(i32 x, int y, i32 z, TileEntityKind kind)
{
    TileEntity tile;
    tile.x = x;
    tile.y = y;
    tile.z = z;
    tile.kind = kind;
    if (const char* id = tileEntityId(kind)) {
        tile.id = id;
    }
    if (kind == TileEntityKind::MobSpawner) {
        // `bd`'s field initialisers, which is what `jt.e` hands the world.
        tile.entityId = kDefaultSpawnerMob;
        tile.delay = kDefaultSpawnerDelay;
    } else {
        tile.delay = 0;
    }
    return tile;
}

TileEntity* findTileEntity(std::vector<TileEntity>& list, i32 x, int y, i32 z)
{
    for (TileEntity& tile : list) {
        if (tile.at(x, y, z)) {
            return &tile;
        }
    }
    return nullptr;
}

const TileEntity* findTileEntity(const std::vector<TileEntity>& list, i32 x, int y, i32 z)
{
    for (const TileEntity& tile : list) {
        if (tile.at(x, y, z)) {
            return &tile;
        }
    }
    return nullptr;
}

TileEntity& putTileEntity(std::vector<TileEntity>& list, i32 x, int y, i32 z,
                          TileEntityKind kind)
{
    if (TileEntity* existing = findTileEntity(list, x, y, z)) {
        *existing = makeTileEntity(x, y, z, kind);
        return *existing;
    }
    list.push_back(makeTileEntity(x, y, z, kind));
    return list.back();
}

bool eraseTileEntity(std::vector<TileEntity>& list, i32 x, int y, i32 z)
{
    for (usize i = 0; i < list.size(); ++i) {
        if (!list[i].at(x, y, z)) {
            continue;
        }
        // Order is not meaningful -- the original's is a HashMap -- so the
        // cheap removal is the right one.
        list[i] = std::move(list.back());
        list.pop_back();
        return true;
    }
    return false;
}

int reconcileTileEntities(ChunkColumn& column)
{
    int changes = 0;

    // Drop first, so a block that changed from one container to another is
    // rebuilt rather than left holding the old tenant's contents.
    for (usize i = column.tileEntities.size(); i > 0; --i) {
        TileEntity& tile = column.tileEntities[i - 1];
        if (tile.kind == TileEntityKind::Unknown) {
            continue;  // not ours to judge; see the header
        }
        const i32 lx = tile.x - column.x * ChunkColumn::kWidth;
        const i32 lz = tile.z - column.z * ChunkColumn::kWidth;
        TileEntityKind want = TileEntityKind::Unknown;
        const bool inside = lx >= 0 && lx < ChunkColumn::kWidth && lz >= 0 &&
                            lz < ChunkColumn::kWidth && tile.y >= 0 &&
                            tile.y < ChunkColumn::kHeight;
        const bool matches =
            inside &&
            tileEntityKindForBlock(block::def(column.block(int(lx), tile.y, int(lz))).tick,
                                   &want) &&
            want == tile.kind;
        if (matches) {
            continue;
        }
        column.tileEntities[i - 1] = std::move(column.tileEntities.back());
        column.tileEntities.pop_back();
        ++changes;
    }

    // ...then heal, which is `ga.d`'s `jt.e` branch.
    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        if (!column.section(sy).mayHoldTileEntity()) {
            continue;
        }
        const int baseY = sy * Section::kSize;
        for (int ly = 0; ly < Section::kSize; ++ly) {
            for (int lz = 0; lz < ChunkColumn::kWidth; ++lz) {
                for (int lx = 0; lx < ChunkColumn::kWidth; ++lx) {
                    TileEntityKind kind = TileEntityKind::Unknown;
                    if (!tileEntityKindForBlock(
                            block::def(column.section(sy).block(lx, ly, lz)).tick, &kind)) {
                        continue;
                    }
                    const int y = baseY + ly;
                    const i32 wx = column.x * ChunkColumn::kWidth + lx;
                    const i32 wz = column.z * ChunkColumn::kWidth + lz;
                    if (findTileEntity(column.tileEntities, wx, y, wz) != nullptr) {
                        continue;
                    }
                    column.tileEntities.push_back(makeTileEntity(wx, y, wz, kind));
                    ++changes;
                }
            }
        }
    }

    return changes;
}

usize tileEntityMemoryUsage(const std::vector<TileEntity>& list)
{
    usize bytes = list.capacity() * sizeof(TileEntity);
    for (const TileEntity& tile : list) {
        bytes += tile.id.capacity() + tile.entityId.capacity();
        bytes += tile.items.capacity() * sizeof(item::ItemStack);
        for (const item::ItemStack& stack : tile.items) {
            bytes += stack.preserved.memoryUsage();
        }
        bytes += tile.preserved.memoryUsage();
    }
    return bytes;
}

void setTileSignLine(TileEntity& tile, int line, std::string_view text)
{
    if (line < 0 || line >= kTileSignLines) {
        return;
    }
    const usize n = text.size() < usize(kTileSignLineLength) ? text.size()
                                                             : usize(kTileSignLineLength);
    if (n > 0) {
        std::memcpy(tile.lines[line], text.data(), n);
    }
    std::memset(tile.lines[line] + n, 0, usize(kTileSignLineBytes) - n);
}

}  // namespace mc::world
