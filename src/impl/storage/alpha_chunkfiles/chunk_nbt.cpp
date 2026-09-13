#include "impl/storage/alpha_chunkfiles/chunk_nbt.hpp"

#include "core/nbt/nbt.hpp"
#include "core/nbt/writer.hpp"
#include "core/world/tile_entity.hpp"
#include "impl/storage/alpha_chunkfiles/item_nbt.hpp"

#include <cstring>

namespace mc::alpha {
namespace {

using world::ChunkColumn;
using world::Section;
using world::TileEntity;
using world::TileEntityKind;

constexpr int kRuns = ChunkColumn::kArea;          // one per (x, z) column
constexpr int kRunBlocks = Section::kSize;         // 16 blocks tall
constexpr int kColumnStride = ChunkColumn::kHeight;  // bytes between runs, Blocks
constexpr int kColumnNibbleStride = ChunkColumn::kHeight / 2;

static_assert(ChunkColumn::kHeight % 2 == 0, "nibble runs must stay byte-aligned");
static_assert(Section::kSize % 2 == 0, "section runs must stay byte-aligned");

// A column array is 256 vertical runs laid end to end; a section takes the same
// slice out of every one of them.
void gatherBlocks(ConstByteSpan column, int sy, u8* dst)
{
    const u8* src = column.data() + sy * kRunBlocks;
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kRunBlocks, src + run * kColumnStride, kRunBlocks);
    }
}

void scatterBlocks(const u8* src, int sy, u8* column)
{
    u8* dst = column + sy * kRunBlocks;
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kColumnStride, src + run * kRunBlocks, kRunBlocks);
    }
}

void gatherNibbles(ConstByteSpan column, int sy, u8* dst)
{
    const u8* src = column.data() + sy * (kRunBlocks / 2);
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * (kRunBlocks / 2), src + run * kColumnNibbleStride,
                    kRunBlocks / 2);
    }
}

void scatterNibbles(const u8* src, int sy, u8* column)
{
    u8* dst = column + sy * (kRunBlocks / 2);
    for (int run = 0; run < kRuns; ++run) {
        std::memcpy(dst + run * kColumnNibbleStride, src + run * (kRunBlocks / 2),
                    kRunBlocks / 2);
    }
}

// Which of the tags we model have been seen. Blocks and the three nibble planes
// are required: a chunk missing one is not a chunk, and defaulting it to zero
// would write that guess back over the real world on the next save.
struct Seen {
    bool xPos = false;
    bool zPos = false;
    bool blocks = false;
    bool data = false;
    bool blockLight = false;
    bool skyLight = false;

    bool complete() const
    {
        return xPos && zPos && blocks && data && blockLight && skyLight;
    }
};

// **`Items`** -- `fe.a(hm)` and `ke.a(hm)`, which are the same loop.
//
// The slot byte is bounds-checked against the array and an out-of-range one is
// dropped, exactly as the original does; the two differ only in that `fe` masks
// it with 255 and `ke` does not, and with arrays of 27 and 3 neither can accept
// a byte either reading admits and the other refuses. `limit` is the class's
// own `c()`.
bool decodeItems(nbt::Reader& r, TileEntity* tile, int limit)
{
    nbt::TagType elemType;
    i32 count = 0;
    if (!r.enterList(&elemType, &count)) {
        return false;
    }
    if (count != 0 && elemType != nbt::TagType::Compound) {
        r.fail();
        return false;
    }
    for (i32 i = 0; i < count; ++i) {
        item::ItemStack stack;
        if (!decodeItemStack(r, &stack)) {
            return false;
        }
        if (stack.slot < 0 || stack.slot >= limit) {
            continue;  // `if (slot >= 0 && slot < a.length)`
        }
        // One stack per slot: a file with two entries for the same slot loses
        // the earlier one, which is what `a[slot] = new ev(...)` does.
        bool replaced = false;
        for (item::ItemStack& held : tile->items) {
            if (held.slot == stack.slot) {
                held = std::move(stack);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            tile->items.push_back(std::move(stack));
        }
    }
    return r.ok();
}

// How many slots a kind's array has, which is what bounds the read.
int slotLimit(TileEntityKind kind)
{
    switch (kind) {
    case TileEntityKind::Chest:   return world::kChestSlots;
    case TileEntityKind::Furnace: return world::kFurnaceSlots;
    default:                      return 0;
    }
}

// Which of `ic`'s own four tags this element carried. All four are required:
// an element without them is what `ic.c(hm)` calls "Skipping TileEntity with
// id", and the original neither keeps it nor writes it back.
struct TileSeen {
    bool id = false;
    bool x = false;
    bool y = false;
    bool z = false;

    bool complete() const { return id && x && y && z; }
};

// **`id` has to be known before any other tag can be claimed**, and NBT
// promises no order. `hm` is an `NBTTagCompound` backed by a `HashMap` and
// writes its members in bucket order, so `Delay` really can arrive before the
// `id` that says a `Delay` is what it is -- and claiming a tag off the wrong
// class would both misread it and emit it twice on the way out, once from the
// field and once from `preserved`.
//
// So the element is read twice over the same bytes: once for the id, once
// against it. The cursor is skipped over the compound first and both passes run
// on the range it covered, which is memory already resident. A column holds a
// handful of these.
//
// Anything other than a `TAG_String` id is no id at all -- `hm.i` answers ""
// for a wrong type, `ic.c(hm)` then finds no class and skips the element -- so
// it is left for `usable` to reject.
bool readTileEntityId(ConstByteSpan payload, TileEntity* tile)
{
    nbt::Reader r(payload);
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "id" && type == nbt::TagType::String) {
            const std::string_view id = r.string();
            tile->id.assign(id.data(), id.size());
            if (!world::tileEntityKindFromId(id, &tile->kind)) {
                tile->kind = TileEntityKind::Unknown;
            }
            return r.skipToEnd() && r.ok();
        }
        if (!r.skipValue(type)) {
            return false;
        }
    }
    return false;
}

bool decodeTileEntity(nbt::Reader& outer, TileEntity* tile, bool* usable)
{
    const usize start = outer.offset();
    if (!outer.skipValue(nbt::TagType::Compound)) {
        return false;
    }
    const ConstByteSpan payload = outer.rangeSince(start);

    TileSeen seen;
    seen.id = readTileEntityId(payload, tile);

    nbt::Reader r(payload);
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "id") {
            // Already taken, and captured rather than dropped when it was not
            // a string -- the one shape where an `id` survives into preserved.
            if (seen.id) {
                if (!r.skipValue(type)) return false;
            } else if (!tile->preserved.capture(r, name, type)) {
                return false;
            }
        } else if (name == "x" || name == "y" || name == "z") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            const i32 v = r.intValue();
            if (name == "x") {
                tile->x = v;
                seen.x = true;
            } else if (name == "y") {
                tile->y = int(v);
                seen.y = true;
            } else {
                tile->z = v;
                seen.z = true;
            }
        } else if (tile->kind == TileEntityKind::MobSpawner && name == "EntityId") {
            if (!nbt::expectType(r, type, nbt::TagType::String)) return false;
            const std::string_view mob = r.string();
            tile->entityId.assign(mob.data(), mob.size());
        } else if (tile->kind == TileEntityKind::MobSpawner && name == "Delay") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            tile->delay = r.shortValue();
        } else if (tile->kind == TileEntityKind::Furnace && name == "BurnTime") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            tile->burnTime = r.shortValue();
        } else if (tile->kind == TileEntityKind::Furnace && name == "CookTime") {
            if (!nbt::expectType(r, type, nbt::TagType::Short)) return false;
            tile->cookTime = r.shortValue();
        } else if (tile->kind == TileEntityKind::Sign && name.size() == 5 &&
                   name.compare(0, 4, "Text") == 0 && name[4] >= '1' &&
                   name[4] <= '0' + world::kTileSignLines) {
            if (!nbt::expectType(r, type, nbt::TagType::String)) return false;
            world::setTileSignLine(*tile, name[4] - '1', r.string());
        } else if ((tile->kind == TileEntityKind::Chest ||
                    tile->kind == TileEntityKind::Furnace) &&
                   name == "Items") {
            if (!nbt::expectType(r, type, nbt::TagType::List)) return false;
            if (!decodeItems(r, tile, slotLimit(tile->kind))) return false;
        } else if (!tile->preserved.capture(r, name, type)) {
            return false;
        }
    }

    *usable = seen.complete();
    return r.ok();
}

bool decodeTileEntities(nbt::Reader& r, ChunkColumn* chunk)
{
    nbt::TagType elemType;
    i32 count = 0;
    if (!r.enterList(&elemType, &count)) {
        return false;
    }
    // An empty list is written as a list of TAG_End; see nbt::Writer::endList.
    if (count != 0 && elemType != nbt::TagType::Compound) {
        r.fail();
        return false;
    }
    chunk->tileEntities.reserve(chunk->tileEntities.size() + usize(count));
    for (i32 i = 0; i < count; ++i) {
        TileEntity tile;
        bool usable = false;
        if (!decodeTileEntity(r, &tile, &usable)) {
            return false;
        }
        if (usable) {
            chunk->tileEntities.push_back(std::move(tile));
        }
    }
    return r.ok();
}

void encodeTileEntity(nbt::Writer& w, const TileEntity& tile)
{
    // `ic.b(hm)` first, then the subclass, which is the order the original
    // builds the compound in. (The order the bytes come out in is Java's
    // HashMap iteration order and is reproducible by nothing; this file has
    // never matched it for `Level` either.)
    w.writeString("id", tile.id);
    w.writeInt("x", tile.x);
    w.writeInt("y", i32(tile.y));
    w.writeInt("z", tile.z);

    switch (tile.kind) {
    case TileEntityKind::MobSpawner:
        w.writeString("EntityId", tile.entityId);
        w.writeShort("Delay", tile.delay);
        break;
    case TileEntityKind::Sign:
        for (int line = 0; line < world::kTileSignLines; ++line) {
            const char name[] = {'T', 'e', 'x', 't', char('1' + line), '\0'};
            w.writeString(name, tile.lines[line]);
        }
        break;
    case TileEntityKind::Furnace:
        w.writeShort("BurnTime", tile.burnTime);
        w.writeShort("CookTime", tile.cookTime);
        [[fallthrough]];
    case TileEntityKind::Chest:
        w.beginList("Items", nbt::TagType::Compound);
        for (const item::ItemStack& stack : tile.items) {
            w.beginListElementCompound();
            encodeItemStack(w, stack);
            w.endCompound();
        }
        w.endList();
        break;
    case TileEntityKind::Unknown:
        break;
    }

    tile.preserved.writeTo(w);
}

bool decodeLevel(nbt::Reader& r, ChunkColumn* chunk, u8* scratch)
{
    Seen seen;
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "xPos") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            chunk->x = r.intValue();
            seen.xPos = true;
        } else if (name == "zPos") {
            if (!nbt::expectType(r, type, nbt::TagType::Int)) return false;
            chunk->z = r.intValue();
            seen.zPos = true;
        } else if (name == "TerrainPopulated") {
            if (!nbt::expectType(r, type, nbt::TagType::Byte)) return false;
            chunk->terrainPopulated = r.byteValue() != 0;
        } else if (name == "LastUpdate") {
            if (!nbt::expectType(r, type, nbt::TagType::Long)) return false;
            chunk->lastUpdate = r.longValue();
        } else if (name == "Blocks") {
            if (!nbt::expectType(r, type, nbt::TagType::ByteArray)) return false;
            const ConstByteSpan blocks = r.byteArray();
            if (blocks.size() != kBlocksBytes) {
                r.fail();
                break;
            }
            for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
                gatherBlocks(blocks, sy, scratch);
                chunk->section(sy).assignBlocks(ConstByteSpan(scratch, Section::kVolume));
            }
            seen.blocks = true;
        } else if (name == "Data" || name == "BlockLight" || name == "SkyLight") {
            if (!nbt::expectType(r, type, nbt::TagType::ByteArray)) return false;
            const ConstByteSpan plane = r.byteArray();
            if (plane.size() != kNibbleBytes) {
                r.fail();
                break;
            }
            for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
                gatherNibbles(plane, sy, scratch);
                const ConstByteSpan slice(scratch, world::NibbleArray::kBytes);
                Section& s = chunk->section(sy);
                if (name == "Data") {
                    s.data().assign(slice);
                } else if (name == "BlockLight") {
                    s.blockLight().assign(slice);
                } else {
                    s.skyLight().assign(slice);
                }
            }
            if (name == "Data") {
                seen.data = true;
            } else if (name == "BlockLight") {
                seen.blockLight = true;
            } else {
                seen.skyLight = true;
            }
        } else if (name == "HeightMap") {
            if (!nbt::expectType(r, type, nbt::TagType::ByteArray)) return false;
            const ConstByteSpan heights = r.byteArray();
            if (heights.size() != kHeightMapBytes) {
                r.fail();
                break;
            }
            std::memcpy(chunk->heightMap, heights.data(), kHeightMapBytes);
        } else if (name == "TileEntities") {
            if (!nbt::expectType(r, type, nbt::TagType::List)) return false;
            if (!decodeTileEntities(r, chunk)) return false;
        } else if (!chunk->preserved.capture(r, name, type)) {
            return false;
        }
    }

    return r.ok() && seen.complete();
}

}  // namespace

bool decodeChunk(ConstByteSpan nbt, ChunkColumn* out)
{
    // Built aside and moved in on success, so a truncated or corrupt file
    // leaves the caller's chunk untouched rather than half-loaded.
    ChunkColumn chunk;

    nbt::Reader r(nbt);
    if (!r.enterRoot()) {
        return false;
    }

    // One buffer for every section and every plane: the gather is 4 KB at its
    // widest and allocating per section would be 32 allocations per chunk load.
    std::vector<u8> scratch(Section::kVolume);

    bool sawLevel = false;
    nbt::TagType type;
    std::string_view name;

    while (r.nextField(&type, &name)) {
        if (name == "Level") {
            if (!nbt::expectType(r, type, nbt::TagType::Compound)) {
                return false;
            }
            if (sawLevel || !decodeLevel(r, &chunk, scratch.data())) {
                return false;
            }
            sawLevel = true;
        } else if (!chunk.preservedRoot.capture(r, name, type)) {
            return false;
        }
    }

    if (!r.ok() || !sawLevel) {
        return false;
    }

    *out = std::move(chunk);
    return true;
}

bool encodeChunk(const ChunkColumn& chunk, std::vector<u8>* out)
{
    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        if (chunk.section(sy).maxBlockId() > 255) {
            return false;
        }
    }

    std::vector<u8> blocks(kBlocksBytes);
    std::vector<u8> data(kNibbleBytes);
    std::vector<u8> blockLight(kNibbleBytes);
    std::vector<u8> skyLight(kNibbleBytes);
    std::vector<u8> scratch(Section::kVolume);

    for (int sy = 0; sy < ChunkColumn::kSectionCount; ++sy) {
        const Section& s = chunk.section(sy);

        s.writeBlocks(scratch.data());
        scatterBlocks(scratch.data(), sy, blocks.data());

        s.data().writeTo(scratch.data());
        scatterNibbles(scratch.data(), sy, data.data());

        s.blockLight().writeTo(scratch.data());
        scatterNibbles(scratch.data(), sy, blockLight.data());

        s.skyLight().writeTo(scratch.data());
        scatterNibbles(scratch.data(), sy, skyLight.data());
    }

    nbt::Writer w(*out);
    w.beginRoot();
    w.beginCompound("Level");
    w.writeInt("xPos", chunk.x);
    w.writeInt("zPos", chunk.z);
    w.writeByte("TerrainPopulated", chunk.terrainPopulated ? 1 : 0);
    w.writeLong("LastUpdate", chunk.lastUpdate);
    w.writeByteArray("Blocks", blocks);
    w.writeByteArray("Data", data);
    w.writeByteArray("BlockLight", blockLight);
    w.writeByteArray("SkyLight", skyLight);
    w.writeByteArray("HeightMap", ConstByteSpan(chunk.heightMap, kHeightMapBytes));
    w.beginList("TileEntities", nbt::TagType::Compound);
    for (const TileEntity& tile : chunk.tileEntities) {
        w.beginListElementCompound();
        encodeTileEntity(w, tile);
        w.endCompound();
    }
    w.endList();
    chunk.preserved.writeTo(w);
    w.endCompound();
    chunk.preservedRoot.writeTo(w);
    w.endRoot();

    return w.ok();
}

}  // namespace mc::alpha
