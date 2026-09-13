#include "core/world/format/region_file.hpp"

#include "core/util/crc32.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace mc::world::format {

namespace {

constexpr u8 kMagic[4] = {'3', 'D', 'R', '1'};
constexpr u16 kFormatVersion = 1;

// 44 bytes used, the rest of the sector left zero. The CRC covers everything
// before it, which is what makes a torn header detectable rather than merely
// unlikely.
constexpr u32 kHeaderBytes = 44;
constexpr u32 kHeaderCrcOffset = 40;

void putU16(u8* p, u16 v)
{
    p[0] = u8(v);
    p[1] = u8(v >> 8);
}

void putU32(u8* p, u32 v)
{
    p[0] = u8(v);
    p[1] = u8(v >> 8);
    p[2] = u8(v >> 16);
    p[3] = u8(v >> 24);
}

void putU64(u8* p, u64 v)
{
    putU32(p, u32(v));
    putU32(p + 4, u32(v >> 32));
}

u16 getU16(const u8* p)
{
    return u16(u16(p[0]) | u16(u16(p[1]) << 8));
}

u32 getU32(const u8* p)
{
    return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

u64 getU64(const u8* p)
{
    return u64(getU32(p)) | (u64(getU32(p + 4)) << 32);
}

}  // namespace

i32 regionCoord(i32 chunkCoord)
{
    // Floor division, not truncation: chunk -1 belongs to region -1, not 0.
    // Written as a branch rather than an arithmetic shift because a right
    // shift of a negative value was implementation-defined before C++20, and
    // this project builds as C++17.
    return chunkCoord >= 0 ? chunkCoord / kRegionChunks
                           : -(((-chunkCoord) + kRegionChunks - 1) / kRegionChunks);
}

u32 regionSlot(i32 chunkX, i32 chunkZ)
{
    const u32 x = u32(chunkX) & u32(kRegionChunks - 1);
    const u32 z = u32(chunkZ) & u32(kRegionChunks - 1);
    return x + z * u32(kRegionChunks);
}

std::string regionPath(std::string_view worldDir, i32 regionX, i32 regionZ)
{
    std::string path(worldDir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    char name[64];
    std::snprintf(name, sizeof(name), "r.%d.%d.3dr", int(regionX), int(regionZ));
    path += name;
    return path;
}

RegionFile::~RegionFile()
{
    // Deliberately not committing here. A destructor cannot report failure, and
    // silently writing a commit that might fail is worse than losing staged
    // work the caller never asked to keep. `close()` is the way out.
    file_.reset();
}

bool RegionFile::open(io::FileSystem& fs, const char* path, i32 regionX, i32 regionZ,
                      bool create)
{
    file_.reset();
    regionX_ = regionX;
    regionZ_ = regionZ;
    dir_.assign(kRegionArea, Entry());
    committed_.assign(kRegionArea, Entry());
    held_.clear();
    staged_ = false;
    stagedChunks_ = 0;
    generation_ = 0;
    dirSlot_ = 0;
    sectorCount_ = kFirstDataSector;

    const bool existed = fs.exists(path);
    if (!existed && !create) {
        return false;
    }

    file_ = fs.openRandomAccess(path, create);
    if (file_ == nullptr) {
        return false;
    }

    u64 fileBytes = 0;
    if (!file_->size(&fileBytes)) {
        file_.reset();
        return false;
    }

    if (fileBytes == 0) {
        // A brand new region: generation 1, an empty directory in slot 0, and
        // a header pointing at it, so the file on disk is valid from the first
        // moment rather than only after something is written to it.
        generation_ = 1;
        dirSlot_ = 0;
        rebuildUsedFromDirectory();
        if (!writeDirectory(0) || !writeHeader()) {
            file_.reset();
            return false;
        }
        committed_ = dir_;
        return true;
    }

    // Both copies, and the live one is the newer of whichever pass their CRC.
    // A torn header leaves the other intact; two torn headers is a region we
    // refuse to guess about.
    u64 genA = 0, genB = 0;
    u32 slotA = 0, slotB = 0;
    u32 countA = 0, countB = 0;
    const bool okA = readHeader(0, &genA, &slotA, &countA);
    const bool okB = readHeader(1, &genB, &slotB, &countB);
    if (!okA && !okB) {
        file_.reset();
        return false;
    }

    const bool useA = okA && (!okB || genA >= genB);
    generation_ = useA ? genA : genB;
    dirSlot_ = useA ? slotA : slotB;
    sectorCount_ = useA ? countA : countB;
    if (dirSlot_ > 1) {
        file_.reset();
        return false;
    }
    if (sectorCount_ < kFirstDataSector) {
        sectorCount_ = kFirstDataSector;
    }

    if (!readDirectory(dirSlot_)) {
        file_.reset();
        return false;
    }
    committed_ = dir_;
    rebuildUsedFromDirectory();
    return true;
}

bool RegionFile::close()
{
    if (file_ == nullptr) {
        return true;
    }
    const bool ok = commit();
    file_.reset();
    return ok;
}

bool RegionFile::readHeader(u32 sector, u64* generation, u32* dirSlot, u32* sectorCount)
{
    u8 buffer[kHeaderBytes];
    if (!file_->readAt(u64(sector) * kSectorBytes, ByteSpan(buffer, sizeof(buffer)))) {
        return false;
    }
    if (std::memcmp(buffer, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }
    if (getU16(buffer + 4) != kFormatVersion) {
        return false;
    }
    // A reader honours what the file says its sectors are, so the size can be
    // revisited without orphaning cards written today. This build only knows
    // how to address one, so a different one is refused rather than misread.
    if (getU16(buffer + 6) != u16(kSectorBytes) || getU16(buffer + 8) != u16(kRegionChunks)) {
        return false;
    }
    const u32 stored = getU32(buffer + kHeaderCrcOffset);
    if (stored != util::crc32(ConstByteSpan(buffer, kHeaderCrcOffset))) {
        return false;
    }
    const u64 gen = getU64(buffer + 20);
    if (gen == 0) {
        return false;  // 0 is never a committed generation
    }
    *generation = gen;
    *dirSlot = getU32(buffer + 28);
    *sectorCount = getU32(buffer + 32);
    return true;
}

bool RegionFile::writeHeader()
{
    u8 buffer[kSectorBytes] = {};
    std::memcpy(buffer, kMagic, sizeof(kMagic));
    putU16(buffer + 4, kFormatVersion);
    putU16(buffer + 6, u16(kSectorBytes));
    putU16(buffer + 8, u16(kRegionChunks));
    putU16(buffer + 10, u16(kHeaderBytes));
    putU32(buffer + 12, u32(regionX_));
    putU32(buffer + 16, u32(regionZ_));
    putU64(buffer + 20, generation_);
    putU32(buffer + 28, dirSlot_);
    putU32(buffer + 32, sectorCount_);
    putU32(buffer + 36, 0);
    putU32(buffer + kHeaderCrcOffset, util::crc32(ConstByteSpan(buffer, kHeaderCrcOffset)));

    // Into the copy that is *not* live, so a tear here leaves the previous
    // generation's header whole and readable.
    const u32 sector = (generation_ & 1u) == 0 ? 0u : 1u;
    if (!file_->writeAt(u64(sector) * kSectorBytes,
                        ConstByteSpan(buffer, sizeof(buffer)))) {
        return false;
    }
    return file_->flush();
}

bool RegionFile::readDirectory(u32 slot)
{
    scratch_.assign(usize(kDirectorySectors) * kSectorBytes, 0);
    const u64 offset = u64(2 + slot * kDirectorySectors) * kSectorBytes;
    if (!file_->readAt(offset, ByteSpan(scratch_.data(), scratch_.size()))) {
        return false;
    }
    for (u32 i = 0; i < kRegionArea; ++i) {
        const u8* p = scratch_.data() + usize(i) * kDirectoryEntryBytes;
        Entry entry;
        entry.sectorOffset = getU32(p);
        entry.sectorCount = getU16(p + 4);
        entry.flags = getU16(p + 6);
        entry.byteLength = getU32(p + 8);
        entry.crc = getU32(p + 12);

        // A run that runs off the end, or claims the header's own sectors, is
        // a corrupt entry. Drop the entry rather than the region: every other
        // chunk in the file is still perfectly readable.
        if (entry.present()
            && (entry.sectorCount == 0 || entry.sectorOffset < kFirstDataSector
                || u64(entry.sectorOffset) + entry.sectorCount > sectorCount_)) {
            entry = Entry();
        }
        dir_[i] = entry;
    }
    return true;
}

bool RegionFile::writeDirectory(u32 slot)
{
    scratch_.assign(usize(kDirectorySectors) * kSectorBytes, 0);
    for (u32 i = 0; i < kRegionArea; ++i) {
        u8* p = scratch_.data() + usize(i) * kDirectoryEntryBytes;
        const Entry& entry = dir_[i];
        putU32(p, entry.sectorOffset);
        putU16(p + 4, entry.sectorCount);
        putU16(p + 6, entry.flags);
        putU32(p + 8, entry.byteLength);
        putU32(p + 12, entry.crc);
    }
    const u64 offset = u64(2 + slot * kDirectorySectors) * kSectorBytes;
    if (!file_->writeAt(offset, ConstByteSpan(scratch_.data(), scratch_.size()))) {
        return false;
    }
    return file_->flush();
}

void RegionFile::rebuildUsedFromDirectory()
{
    used_.assign((usize(sectorCount_) + 63) / 64, 0);
    for (u32 i = 0; i < kFirstDataSector; ++i) {
        markUsed(i, 1, true);
    }
    for (const Entry& entry : dir_) {
        if (entry.present()) {
            markUsed(entry.sectorOffset, entry.sectorCount, true);
        }
    }
}

void RegionFile::markUsed(u32 offset, u32 count, bool used)
{
    const usize needed = (usize(offset) + count + 63) / 64;
    if (used_.size() < needed) {
        used_.resize(needed, 0);
    }
    for (u32 i = 0; i < count; ++i) {
        const usize bit = usize(offset) + i;
        const u64 mask = u64(1) << (bit & 63);
        if (used) {
            used_[bit >> 6] |= mask;
        } else {
            used_[bit >> 6] &= ~mask;
        }
    }
}

bool RegionFile::isFree(u32 offset, u32 count) const
{
    for (u32 i = 0; i < count; ++i) {
        const usize bit = usize(offset) + i;
        const usize word = bit >> 6;
        if (word < used_.size() && (used_[word] & (u64(1) << (bit & 63))) != 0) {
            return false;
        }
    }
    return true;
}

bool RegionFile::allocate(u32 sectors, u32* offset)
{
    if (sectors == 0) {
        return false;
    }
    // First fit. Chunk sizes cluster tightly -- 1,194 to 5,872 bytes -- so a
    // rewritten chunk almost always fits the run it vacated once that run is
    // released, and the gaps that do open get reused by the next chunk of a
    // similar size. That is why there is no compaction pass.
    for (u32 start = kFirstDataSector; start + sectors <= sectorCount_; ++start) {
        if (isFree(start, sectors)) {
            markUsed(start, sectors, true);
            *offset = start;
            return true;
        }
    }
    *offset = sectorCount_;
    sectorCount_ += sectors;
    markUsed(*offset, sectors, true);
    return true;
}

void RegionFile::retire(u32 slot)
{
    const Entry& current = dir_[slot];
    if (!current.present()) {
        return;
    }
    if (committed_[slot].present() && committed_[slot].sameRun(current)) {
        // The durable directory still points here. These sectors have to stay
        // untouched until a commit stops referring to them, or a power cut
        // between now and then would leave the previous generation pointing at
        // bytes something else had overwritten.
        held_.push_back(Run{current.sectorOffset, current.sectorCount});
        return;
    }
    // Only ever staged, so nothing durable names it: free it at once. Without
    // this, saving one chunk repeatedly between commits would leak the region.
    markUsed(current.sectorOffset, current.sectorCount, false);
}

bool RegionFile::has(i32 chunkX, i32 chunkZ) const
{
    return isOpen() && dir_[regionSlot(chunkX, chunkZ)].present();
}

bool RegionFile::read(i32 chunkX, i32 chunkZ, std::vector<u8>* out)
{
    if (!isOpen()) {
        return false;
    }
    const Entry& entry = dir_[regionSlot(chunkX, chunkZ)];
    if (!entry.present() || entry.byteLength == 0) {
        return false;
    }
    const usize base = out->size();
    out->resize(base + entry.byteLength);
    if (!file_->readAt(u64(entry.sectorOffset) * kSectorBytes,
                       ByteSpan(out->data() + base, entry.byteLength))) {
        out->resize(base);
        return false;
    }
    return true;
}

bool RegionFile::readMany(const i32* chunkXs, const i32* chunkZs, usize count,
                          std::vector<u8>* scratch, void* context, BatchVisitor visit)
{
    if (!isOpen() || scratch == nullptr || visit == nullptr) {
        return false;
    }

    // Slots rather than coordinates from here on. The directory is what says
    // where the bytes are, and a slot is also what makes a repeated request
    // collapse into one read instead of two.
    batch_.clear();
    for (usize i = 0; i < count; ++i) {
        // Refused rather than masked. `regionSlot` would happily fold a chunk
        // from the next region onto one of ours and read the wrong terrain --
        // silently, and into a picture nobody would think to distrust.
        if (regionCoord(chunkXs[i]) != regionX_ || regionCoord(chunkZs[i]) != regionZ_) {
            return false;
        }
        const u32 slot = regionSlot(chunkXs[i], chunkZs[i]);
        const Entry& entry = dir_[slot];
        // Sector 0 is the header's, never a payload's, so it doubles as "this
        // region does not hold that chunk" -- and sorts every absent answer to
        // the front, which is where a caller wants them.
        batch_.push_back(BatchSlot{
            entry.present() && entry.byteLength != 0 ? entry.sectorOffset : 0u, slot});
    }
    if (batch_.empty()) {
        return true;
    }

    std::sort(batch_.begin(), batch_.end(),
              [](const BatchSlot& a, const BatchSlot& b) { return a.sector < b.sector; });
    batch_.erase(std::unique(batch_.begin(), batch_.end(),
                             [](const BatchSlot& a, const BatchSlot& b) {
                                 return a.slot == b.slot;
                             }),
                 batch_.end());

    usize first = 0;

    // Absent first, and before one byte is read: they cost no I/O at all, so a
    // caller counting a group down can finish one that is wholly unexplored
    // without waiting on the card for it.
    while (first < batch_.size() && batch_[first].sector == 0) {
        if (!visit(context, chunkXOf(batch_[first].slot), chunkZOf(batch_[first].slot),
                   ConstByteSpan())) {
            return true;
        }
        ++first;
    }

    while (first < batch_.size()) {
        // How far one operation reaches: everything that starts within the gap
        // of what has been taken so far and still fits the scratch. The first
        // entry is always taken, so a single payload over the cap is read on
        // its own rather than refused.
        const u32 startSector = batch_[first].sector;
        usize last = first;
        u32 endSector = startSector + dir_[batch_[first].slot].sectorCount;
        for (usize i = first + 1; i < batch_.size(); ++i) {
            const Entry& entry = dir_[batch_[i].slot];
            if (entry.sectorOffset > endSector + kBatchGapSectors) {
                break;
            }
            const u32 reach = entry.sectorOffset + entry.sectorCount;
            if (u64(reach - startSector) * kSectorBytes > kBatchReadBytes) {
                break;
            }
            endSector = reach;
            last = i;
        }

        // **Not the whole of the last sector.** `write` stores a payload's
        // exact length and pads nothing, so the final sector of the file is
        // partial and reading it whole would run off the end and fail the
        // batch. The last run's recorded length is where the bytes provably
        // stop -- and it is the last, because runs do not overlap, so a higher
        // offset also means a higher end.
        const Entry& tail = dir_[batch_[last].slot];
        const u64 offset = u64(startSector) * kSectorBytes;
        const usize length =
            usize(u64(tail.sectorOffset) * kSectorBytes + tail.byteLength - offset);
        scratch->resize(length);
        if (!file_->readAt(offset, ByteSpan(scratch->data(), length))) {
            return false;
        }

        for (usize i = first; i <= last; ++i) {
            const Entry& entry = dir_[batch_[i].slot];
            const usize at = usize(u64(entry.sectorOffset) * kSectorBytes - offset);
            const i32 x = chunkXOf(batch_[i].slot);
            const i32 z = chunkZOf(batch_[i].slot);
            if (!visit(context, x, z, ConstByteSpan(scratch->data() + at, entry.byteLength))) {
                return true;  // the caller stopped it, which is not a failure
            }
        }
        first = last + 1;
    }
    return true;
}

bool RegionFile::write(i32 chunkX, i32 chunkZ, ConstByteSpan payload)
{
    if (!isOpen() || payload.empty()) {
        return false;
    }
    const u32 sectors = u32((payload.size() + kSectorBytes - 1) / kSectorBytes);
    if (sectors > 0xFFFFu) {
        return false;  // more than a u16 of sectors is not a chunk
    }

    u32 offset = 0;
    if (!allocate(sectors, &offset)) {
        return false;
    }

    // The payload reaches the card now; what waits for commit() is the
    // directory entry that makes it findable. Writing it into free space is
    // what makes that safe -- nothing durable is pointing here yet.
    if (!file_->writeAt(u64(offset) * kSectorBytes, payload)) {
        markUsed(offset, sectors, false);
        return false;
    }
    // The tail of the last sector is whatever was there before. byteLength is
    // what says where the payload ends, so it is never read.

    const u32 slot = regionSlot(chunkX, chunkZ);
    retire(slot);

    Entry entry;
    entry.sectorOffset = offset;
    entry.sectorCount = u16(sectors);
    entry.flags = 0;
    entry.byteLength = u32(payload.size());
    entry.crc = util::crc32(payload);
    dir_[slot] = entry;

    staged_ = true;
    ++stagedChunks_;
    if (stagedChunks_ >= kAutoCommitChunks) {
        return commit();
    }
    return true;
}

bool RegionFile::erase(i32 chunkX, i32 chunkZ)
{
    if (!isOpen()) {
        return false;
    }
    const u32 slot = regionSlot(chunkX, chunkZ);
    if (!dir_[slot].present()) {
        return true;
    }
    retire(slot);
    dir_[slot] = Entry();
    staged_ = true;
    return true;
}

bool RegionFile::commit()
{
    if (!isOpen()) {
        return false;
    }
    if (!staged_) {
        return true;
    }

    // Payloads are already written and flushed by write(). What is ordered
    // here is the directory before the header that points at it.
    if (!file_->flush()) {
        return false;
    }
    const u32 target = 1 - dirSlot_;
    if (!writeDirectory(target)) {
        return false;
    }

    const u64 nextGeneration = generation_ + 1;
    const u64 previousGeneration = generation_;
    const u32 previousSlot = dirSlot_;
    generation_ = nextGeneration;
    dirSlot_ = target;
    if (!writeHeader()) {
        generation_ = previousGeneration;
        dirSlot_ = previousSlot;
        return false;
    }

    // Only now is the old generation unreachable, so only now may the sectors
    // it referred to be handed out again.
    for (const Run& run : held_) {
        markUsed(run.offset, run.count, false);
    }
    held_.clear();
    committed_ = dir_;
    staged_ = false;
    stagedChunks_ = 0;
    return true;
}

bool RegionFile::payloadInfo(i32 chunkX, i32 chunkZ, u32* byteLength, u32* crc32) const
{
    if (!isOpen()) {
        return false;
    }
    const Entry& entry = dir_[regionSlot(chunkX, chunkZ)];
    if (!entry.present()) {
        return false;
    }
    if (byteLength != nullptr) {
        *byteLength = entry.byteLength;
    }
    if (crc32 != nullptr) {
        *crc32 = entry.crc;
    }
    return true;
}

u32 RegionFile::chunkCount() const
{
    u32 count = 0;
    for (const Entry& entry : dir_) {
        if (entry.present()) {
            ++count;
        }
    }
    return count;
}

bool RegionFile::forEachChunk(void* context, Visitor visit) const
{
    if (!isOpen()) {
        return false;
    }
    for (u32 slot = 0; slot < kRegionArea; ++slot) {
        if (!dir_[slot].present()) {
            continue;
        }
        const i32 x = regionX_ * kRegionChunks + i32(slot % u32(kRegionChunks));
        const i32 z = regionZ_ * kRegionChunks + i32(slot / u32(kRegionChunks));
        if (!visit(context, x, z)) {
            break;
        }
    }
    return true;
}

u32 RegionFile::usedSectorCount() const
{
    u32 count = kFirstDataSector;
    for (const Entry& entry : dir_) {
        if (entry.present()) {
            count += entry.sectorCount;
        }
    }
    return count;
}

}  // namespace mc::world::format
