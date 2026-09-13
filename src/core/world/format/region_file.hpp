#pragma once

// One `r.<rx>.<rz>.3dr` container: 32 x 32 chunk payloads in one file,
// allocated in sectors, reached by seeking rather than by opening.
//
// **What this buys, and what it does not.** It is not compression -- an Alpha
// chunk file is already a gzip stream and re-deflating it would gain ~10 % for
// real CPU on a 268 MHz ARM11. Payloads are stored **verbatim**, exactly the
// bytes the folder held, which costs nothing and is what makes an exact
// restoration provable rather than hopeful. What it buys is the two things the
// Alpha layout is worst at on this console:
//
//   * **Cluster slack.** FAT32 gives a file a whole cluster, and a real card
//     measured 16 KB. A median chunk is 2,917 bytes, so nine tenths of every
//     cluster is waste -- and because the leaf directory is `<x&63>/<z&63>`,
//     any world smaller than 64x64 chunks also spends a whole cluster on a
//     directory per chunk. Here a chunk costs a few 1 KB sectors inside one
//     file.
//   * **Operations.** The cost of chunk I/O on a 3DS is the IPC round trip to
//     the FS sysmodule, four to six per file operation, not the transfer. The
//     Alpha layout pays an open per chunk and a directory listing per leaf;
//     this pays one open per region and then seeks, and its directory is its
//     own index -- one 16 KB read settles the existence of all 1,024 chunks.
//
// ## Layout
//
//     sector  0        header copy A          1024-byte sectors
//     sector  1        header copy B
//     sectors 2..17    directory slot 0       1024 entries x 16 bytes
//     sectors 18..33   directory slot 1       (double-buffered)
//     sectors 34+      payloads
//
// Little-endian, and written a byte at a time rather than by copying a struct:
// no packing assumptions, no alignment traps on ARM, and the file says what it
// means. NBT stays big-endian because that is a compatibility requirement;
// this container is ours and nothing else reads it.
//
// `sectorBytes` is recorded in the header and honoured on read, so the size can
// be revisited later without breaking cards written today.
//
// ## Crash safety, and its deliberate limit
//
// The rule is **never overwrite live data**: a rewritten chunk is always
// allocated a fresh run, and the sectors it vacates are not reusable until the
// commit that stops referring to them lands. A commit is then three ordered,
// flushed steps -- payload, then the *inactive* directory slot, then a header
// carrying `generation + 1` and pointing at it. Opening takes the header copy
// with the highest generation whose CRC checks, so a commit torn anywhere
// leaves the previous generation whole.
//
// The worst case of a power cut is losing the last commit, never the region.
// That is the intended bar and not a higher one: **payload CRCs are recorded
// but not verified on the read path.** A payload is a gzip stream and inflate
// already checks gzip's own CRC-32 and length, so checking ours first would be
// a second pass over the same bytes for the same answer, in the chunk-load
// path, on a 268 MHz CPU. The recorded CRC is what a conversion verifies
// against without inflating anything.
//
// ## Why `commit()` is separate from `write()`
//
// Committing per chunk would write a 3 KB payload, a 16 KB directory and a
// 1 KB header for every column -- 6.6x write amplification and three
// operations where the folder format needs one. Packed mode would then be
// *slower* to save than the format it replaces. So writes stage, and the
// caller commits when it flushes; `ChunkCache` already batches on the autosave
// timer. A self-imposed bound commits anyway after enough staged work, so a
// caller that never flushes cannot lose an unbounded amount of world.

#include "core/io/file_system.hpp"
#include "core/util/span.hpp"
#include "core/util/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc::world::format {

// 32 x 32 chunks, as McRegion uses -- the sector allocator and the commit are
// meant to be reusable for real McRegion when a later version needs it.
inline constexpr i32 kRegionChunks = 32;
inline constexpr u32 kRegionArea = u32(kRegionChunks) * u32(kRegionChunks);

// **1024, not the 4096 the design document guessed at.** Chunk payloads are
// 1,194 to 5,872 bytes with a 2,945-byte mean, so at a 4 KB sector nine tenths
// of chunks take exactly one sector and waste 1.2 KB of it -- 53 % overhead,
// not the 12 % a "half a sector" model predicts, because the distribution is
// narrower than the sector. At 1 KB the mean chunk takes three sectors and
// wastes half of a fourth: 17 %. 512 would save another 7 % and doubles the
// bookkeeping; that trade is not worth making twice.
inline constexpr u32 kSectorBytes = 1024;

inline constexpr u32 kDirectoryEntryBytes = 16;
inline constexpr u32 kDirectorySectors =
    (kRegionArea * kDirectoryEntryBytes + kSectorBytes - 1) / kSectorBytes;
inline constexpr u32 kFirstDataSector = 2 + 2 * kDirectorySectors;

// **What `readMany` will swallow to save an operation.** An operation is the
// IPC round trip to the FS sysmodule, modelled at ~4 ms across this codebase
// (`MC_IO_LATENCY_US`); 16 KB of sectors nobody asked for is far less than
// that, so a gap that small is cheaper to read than to skip.
//
// Measured over the diorama's 24x24 window on a real packed world -- 576
// chunks, 1.52 MB of payload, four regions -- against one read per chunk:
//
//     gap   scratch    reads   transferred
//       -         -      576       1.52 MB
//       4     64 KB      122       2.14 MB
//      16     64 KB       63       2.84 MB
//      16    128 KB       41       2.89 MB
//      64    256 KB       19       3.26 MB
//
// 64 KB is where the curve stops being steep for what an Old 3DS can spare;
// the next 64 KB of scratch buys 22 reads, the 128 KB after that buys 22 more.
inline constexpr u32 kBatchGapSectors = 16;
inline constexpr u32 kBatchReadBytes = 64 * 1024;

// Floor division by 32, which is what turns a chunk coordinate into the region
// holding it. A named function with its own test because `>> 5` on a negative
// signed value was implementation-defined before C++20 and half of a world's
// coordinates are negative.
i32 regionCoord(i32 chunkCoord);

// Where a chunk sits in its region's directory. Masking is safe on two's
// complement for negatives, the same trick the base36 directory names use.
u32 regionSlot(i32 chunkX, i32 chunkZ);

// Builds `<dir>/r.<rx>.<rz>.3dr`.
std::string regionPath(std::string_view worldDir, i32 regionX, i32 regionZ);

class RegionFile {
public:
    RegionFile() = default;
    ~RegionFile();

    RegionFile(const RegionFile&) = delete;
    RegionFile& operator=(const RegionFile&) = delete;

    // Opens a region, or creates an empty one when `create` is set. False when
    // the file is missing and `create` is false, and also when both header
    // copies fail their CRC -- a region whose generation cannot be established
    // is reported, never guessed at.
    bool open(io::FileSystem& fs, const char* path, i32 regionX, i32 regionZ, bool create);

    // Commits anything staged, then drops the handle. Failing to commit is
    // reported; the handle is dropped either way.
    bool close();

    bool isOpen() const { return file_ != nullptr; }
    i32 regionX() const { return regionX_; }
    i32 regionZ() const { return regionZ_; }

    // Chunk coordinates, not slots -- callers think in world coordinates and
    // the masking belongs here.
    bool has(i32 chunkX, i32 chunkZ) const;

    // Appends the payload bytes to *out. False for an absent chunk and for a
    // file that ends before the payload does.
    bool read(i32 chunkX, i32 chunkZ, std::vector<u8>* out);

    // One chunk out of a batch read: a span into the batch's scratch buffer,
    // valid only until the call returns, and **empty for a chunk this region
    // does not hold**. False stops the batch.
    using BatchVisitor = bool (*)(void* context, i32 chunkX, i32 chunkZ, ConstByteSpan payload);

    // **Many chunks of this region in as few operations as its sector layout
    // allows.** The entries are sorted by sector offset, runs within
    // `kBatchGapSectors` of each other are merged while they fit
    // `kBatchReadBytes`, and each merged run is one `readAt`. What that is
    // worth is the table above those constants: on the diorama's window, 576
    // reads become 63.
    //
    // **Every chunk asked for is visited exactly once**, so a caller can count
    // a group of them down to nothing. The ones this region does not hold come
    // first, with an empty payload and before one byte is read -- they cost no
    // I/O, so a group made entirely of them finishes without touching the card.
    // The rest follow **in sector order, not the order asked for**: the whole
    // point is that the caller's order is not the card's. Asking twice for one
    // chunk reads and visits it once.
    //
    // `scratch` is the caller's, not a member, because only a caller that
    // batches should pay 64 KB for one: `PackedStorage` keeps four regions
    // open and never calls this. It is resized as runs need it and is reusable
    // across calls; one payload larger than `kBatchReadBytes` grows it past
    // the cap rather than failing, since the cap bounds merging and not a
    // chunk.
    //
    // False means the read failed, or a chunk was asked for that this region
    // does not cover. **A batch the visitor stopped is not a failure** -- the
    // visitor knows it stopped, so the return value keeps one meaning.
    bool readMany(const i32* chunkXs, const i32* chunkZs, usize count, std::vector<u8>* scratch,
                  void* context, BatchVisitor visit);

    // Stages a payload. The bytes reach the card immediately; what waits for
    // `commit()` is the directory entry that makes them findable.
    bool write(i32 chunkX, i32 chunkZ, ConstByteSpan payload);

    bool erase(i32 chunkX, i32 chunkZ);

    // The three ordered flushes. Cheap and a no-op when nothing is staged.
    bool commit();

    // What the directory records for a chunk, for a converter that wants to
    // check its work without inflating anything. False if absent.
    bool payloadInfo(i32 chunkX, i32 chunkZ, u32* byteLength, u32* crc32) const;

    u32 chunkCount() const;

    // Visits every chunk present, in slot order, as world coordinates. The
    // visitor returning false stops the walk, which is success.
    using Visitor = bool (*)(void* context, i32 chunkX, i32 chunkZ);
    bool forEachChunk(void* context, Visitor visit) const;

    // Sectors the file holds against sectors its payloads use -- what a
    // fragmentation readout would report. Cheap; no I/O.
    u32 sectorCount() const { return sectorCount_; }
    u32 usedSectorCount() const;

private:
    struct Entry {
        u32 sectorOffset = 0;  // 0 is never a payload: the header lives there
        u16 sectorCount = 0;
        u16 flags = 0;
        u32 byteLength = 0;
        u32 crc = 0;

        bool present() const { return sectorOffset != 0; }
        bool sameRun(const Entry& other) const
        {
            return sectorOffset == other.sectorOffset && sectorCount == other.sectorCount;
        }
    };

    // Where a directory slot sits in the world.
    i32 chunkXOf(u32 slot) const
    {
        return regionX_ * kRegionChunks + i32(slot % u32(kRegionChunks));
    }
    i32 chunkZOf(u32 slot) const
    {
        return regionZ_ * kRegionChunks + i32(slot / u32(kRegionChunks));
    }

    bool readHeader(u32 sector, u64* generation, u32* dirSlot, u32* sectorCount);
    bool writeHeader();
    bool readDirectory(u32 slot);
    bool writeDirectory(u32 slot);
    void rebuildUsedFromDirectory();

    bool allocate(u32 sectors, u32* offset);
    void markUsed(u32 offset, u32 count, bool used);
    bool isFree(u32 offset, u32 count) const;

    // Retires the run an entry currently points at: held until the next commit
    // if the committed directory still names it, freed at once if it was only
    // ever staged. Without that second case a burst of saves to one chunk
    // would leak the region.
    void retire(u32 slot);

    std::unique_ptr<io::RandomAccessFile> file_;
    i32 regionX_ = 0;
    i32 regionZ_ = 0;

    // 1024 entries, 16 KB -- **a member, never a stack local**. The 3DS main
    // thread has 32 KB of stack it cannot enlarge, and the host build has 8 MB
    // and no -Werror=stack-usage, so a local here would pass every test and
    // fault on hardware.
    std::vector<Entry> dir_;
    std::vector<Entry> committed_;

    // Sector occupancy, one bit each, derived from the directory at open
    // rather than stored. One fewer on-disk structure, and one fewer thing a
    // torn commit could leave disagreeing with the directory.
    std::vector<u64> used_;

    struct Run {
        u32 offset = 0;
        u32 count = 0;
    };
    std::vector<Run> held_;

    // One requested chunk of a batch, as the card sees it. Sorted by `sector`,
    // which is a total order: allocated runs never overlap.
    struct BatchSlot {
        u32 sector = 0;
        u32 slot = 0;
    };
    // Only ever allocated by readMany, so a region the game opens for play
    // carries an empty vector and nothing else.
    std::vector<BatchSlot> batch_;

    std::vector<u8> scratch_;

    u32 sectorCount_ = 0;
    u64 generation_ = 0;
    u32 dirSlot_ = 0;
    bool staged_ = false;

    // The bound that keeps an unflushing caller honest. Chosen so it never
    // fires in ordinary play: ChunkCache flushes on the autosave timer, which
    // is 45 seconds.
    static constexpr u32 kAutoCommitChunks = 64;
    u32 stagedChunks_ = 0;
};

}  // namespace mc::world::format
