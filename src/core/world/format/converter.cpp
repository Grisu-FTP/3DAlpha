#include "core/world/format/converter.hpp"

#include "core/io/volume_info.hpp"
#include "core/settings/world_settings.hpp"
#include "core/util/crc32.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/format/manifest.hpp"
#include "core/world/format/region_file.hpp"
#include "core/world/world_list.hpp"

#include "version_slots.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mc::world::format {

namespace {

constexpr int kMaxDepth = 8;
constexpr usize kMaxStashedFileBytes = 16u << 20;

// What packing writes and unpacking removes. A source folder already holding
// one of these is refused rather than silently collided with.
bool isReservedName(std::string_view name)
{
    if (name == kManifestName || name == kReadmeName) {
        return true;
    }
    int rx = 0;
    int rz = 0;
    int consumed = 0;
    return std::sscanf(std::string(name).c_str(), "r.%d.%d.3dr%n", &rx, &rz, &consumed) == 2
           && name.size() == usize(consumed);
}

std::string join(std::string_view dir, std::string_view name)
{
    std::string path(dir);
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path.append(name);
    return path;
}

std::string_view baseName(std::string_view path)
{
    const usize slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

struct Children {
    std::vector<std::pair<std::string, bool>> entries;
};

bool collectChildren(void* context, const io::DirEntry& entry)
{
    static_cast<Children*>(context)->entries.emplace_back(entry.name, entry.isDirectory);
    return true;
}

// One file the packer found, already classified.
struct Found {
    std::string relative;  // relative to the world directory, '/' separated
    bool isDirectory = false;
    bool isChunk = false;
    i32 chunkX = 0;
    i32 chunkZ = 0;
    u64 bytes = 0;
};

// Names are gathered inside the visitor and opened after it returns: on the
// console a file operation inside a directory walk is one IPC round trip nested
// inside another's iterator.
bool scanTree(io::FileSystem& fs, const std::string& root, const std::string& relative,
              std::vector<Found>* out, int depth)
{
    if (depth > kMaxDepth) {
        return false;
    }
    const std::string absolute = relative.empty() ? root : join(root, relative);
    Children found;
    if (!fs.listDirectory(absolute.c_str(), &found, collectChildren)) {
        return false;
    }
    if (!relative.empty() && found.entries.empty()) {
        // An empty directory is worth recording: a tree restored without one is
        // not the tree that was packed.
        Found entry;
        entry.relative = relative;
        entry.isDirectory = true;
        out->push_back(std::move(entry));
    }

    for (const auto& child : found.entries) {
        const std::string childRelative =
            relative.empty() ? child.first : relative + "/" + child.first;
        if (child.second) {
            if (!scanTree(fs, root, childRelative, out, depth + 1)) {
                return false;
            }
            continue;
        }
        Found entry;
        entry.relative = childRelative;
        usize bytes = 0;
        if (fs.fileSize(join(root, childRelative).c_str(), &bytes)) {
            entry.bytes = u64(bytes);
        }
        out->push_back(std::move(entry));
    }
    return true;
}

// **The rule that makes packing lossless without listing chunks in the
// manifest.** A file is a chunk only if its name parses as one *and* it sits at
// exactly the path that chunk belongs at. A chunk file in the wrong leaf
// directory -- which third-party tools do produce -- is not a chunk, it is a
// stray file, and it is stashed at its own path like any other.
void classifyChunks(const std::string& worldDir, std::vector<Found>* files)
{
    std::vector<u64> claimed;
    for (Found& entry : *files) {
        if (entry.isDirectory) {
            continue;
        }
        i32 x = 0;
        i32 z = 0;
        if (!mcver::ChunkLayout::parseFileName(baseName(entry.relative), &x, &z)) {
            continue;
        }
        typename mcver::ChunkLayout::Path canonical;
        if (!mcver::ChunkLayout::filePath(worldDir, x, z, &canonical)) {
            continue;
        }
        if (canonical.view() != join(worldDir, entry.relative)) {
            continue;
        }
        const u64 key = (u64(u32(x)) << 32) | u64(u32(z));
        if (std::find(claimed.begin(), claimed.end(), key) != claimed.end()) {
            continue;  // two files claiming one chunk: only the first is one
        }
        claimed.push_back(key);
        entry.isChunk = true;
        entry.chunkX = x;
        entry.chunkZ = z;
    }
}

bool tell(const ConvertOptions& options, const char* stage, u32 done, u32 total)
{
    if (options.observe == nullptr) {
        return true;
    }
    ConvertProgress progress;
    progress.stage = stage;
    progress.filesDone = done;
    progress.filesTotal = total;
    return options.observe(options.context, progress);
}

const char* kReadmeText =
    "This folder is a 3DAlpha packed world.\n"
    "\n"
    "It is deliberately NOT a Minecraft world any more. The chunks live in the\n"
    "r.*.3dr files and level.dat lives inside world.3dm, so that opening this\n"
    "folder in Minecraft cannot generate new terrain over the top of it.\n"
    "\n"
    "Nothing has been lost. To get an ordinary Minecraft world back, open the\n"
    "world on the console, press X on it in the world list, and set Format to\n"
    "Folder. Everything is restored exactly as it was, including any files\n"
    "3DAlpha did not recognise.\n";

// Both directions end by moving a staged tree into place. Only the pack
// direction stages, so this only moves files -- there are never subdirectories
// in a staged packed world.
bool moveStagedInto(io::FileSystem& fs, const std::string& staging, const std::string& world)
{
    Children found;
    if (!fs.listDirectory(staging.c_str(), &found, collectChildren)) {
        return false;
    }
    bool ok = true;
    for (const auto& entry : found.entries) {
        if (entry.first == kCommitMarker) {
            continue;
        }
        ok = fs.rename(join(staging, entry.first).c_str(), join(world, entry.first).c_str())
             && ok;
    }
    fs.removeFile(join(staging, kCommitMarker).c_str());
    fs.removeDirectory(staging.c_str());
    return ok;
}

// Empties a world directory without removing the directory itself.
bool clearWorld(io::FileSystem& fs, const std::string& world)
{
    Children found;
    if (!fs.listDirectory(world.c_str(), &found, collectChildren)) {
        return false;
    }
    bool ok = true;
    for (const auto& entry : found.entries) {
        const std::string path = join(world, entry.first);
        ok = (entry.second ? removeTree(fs, path) : fs.removeFile(path.c_str())) && ok;
    }
    return ok;
}

ConvertResult packWorld(io::FileSystem& fs, const std::string& world,
                        const ConvertOptions& options)
{
    std::vector<Found> files;
    if (!scanTree(fs, world, std::string(), &files, 0)) {
        return ConvertResult::SourceUnreadable;
    }
    for (const Found& entry : files) {
        if (!entry.isDirectory && isReservedName(baseName(entry.relative))) {
            return ConvertResult::ReservedName;
        }
    }
    classifyChunks(world, &files);

    // The seed and last-played the world list will read out of the manifest,
    // taken before anything is written.
    LevelData level;
    {
        AnyStorage probe(fs);
        if (!probe.peekLevel(world, &level)) {
            return ConvertResult::SourceUnreadable;
        }
    }

    const std::string staging = world + kConvertingSuffix;
    removeTree(fs, staging);
    if (!fs.makeDirectories(staging.c_str())) {
        return ConvertResult::WriteFailed;
    }

    // Chunks in region order, so each container is opened once rather than
    // thrashed through the four-slot cache.
    std::vector<const Found*> chunks;
    for (const Found& entry : files) {
        if (entry.isChunk) {
            chunks.push_back(&entry);
        }
    }
    std::sort(chunks.begin(), chunks.end(), [](const Found* a, const Found* b) {
        const i32 ax = regionCoord(a->chunkX);
        const i32 az = regionCoord(a->chunkZ);
        const i32 bx = regionCoord(b->chunkX);
        const i32 bz = regionCoord(b->chunkZ);
        if (az != bz) {
            return az < bz;
        }
        if (ax != bx) {
            return ax < bx;
        }
        return regionSlot(a->chunkX, a->chunkZ) < regionSlot(b->chunkX, b->chunkZ);
    });

    Manifest manifest;
    manifest.summary.lastPlayed = level.lastPlayed;
    manifest.summary.randomSeed = level.randomSeed;

    // What the source held, kept so verification can compare against the bytes
    // that were read rather than against the bytes that were written.
    struct Expected {
        i32 x = 0;
        i32 z = 0;
        u32 bytes = 0;
        u32 crc = 0;
    };
    std::vector<Expected> expected;
    expected.reserve(chunks.size());

    const u32 total = u32(files.size());
    u32 done = 0;
    u64 unpackedBytes = 0;

    {
        RegionFile region;
        i32 openX = 0;
        i32 openZ = 0;
        std::vector<u8> bytes;

        for (const Found* entry : chunks) {
            const i32 rx = regionCoord(entry->chunkX);
            const i32 rz = regionCoord(entry->chunkZ);
            if (!region.isOpen() || rx != openX || rz != openZ) {
                if (region.isOpen() && !region.close()) {
                    return ConvertResult::WriteFailed;
                }
                if (!region.open(fs, regionPath(staging, rx, rz).c_str(), rx, rz, true)) {
                    return ConvertResult::WriteFailed;
                }
                openX = rx;
                openZ = rz;
                ++manifest.summary.regionCount;
            }

            bytes.clear();
            if (!fs.readFile(join(world, entry->relative).c_str(), &bytes,
                             mcver::ChunkCodec::kMaxPayloadBytes)) {
                return ConvertResult::SourceUnreadable;
            }
            // **Verbatim.** The gzip stream the folder held goes in untouched;
            // nothing is inflated, nothing is re-deflated, and that is what
            // makes the restoration exact rather than merely equivalent.
            const ConstByteSpan payload(bytes.data(), bytes.size());
            if (!region.write(entry->chunkX, entry->chunkZ, payload)) {
                return ConvertResult::WriteFailed;
            }
            expected.push_back(
                Expected{entry->chunkX, entry->chunkZ, u32(bytes.size()), util::crc32(payload)});
            unpackedBytes += u64(bytes.size());
            ++manifest.summary.chunkCount;

            if (++done % options.observeEvery == 0 && !tell(options, "packing", done, total)) {
                removeTree(fs, staging);
                return ConvertResult::Cancelled;
            }
        }
        if (region.isOpen() && !region.close()) {
            return ConvertResult::WriteFailed;
        }
    }

    // Everything that is not a chunk, byte for byte.
    std::vector<u8> bytes;
    for (const Found& entry : files) {
        if (entry.isChunk) {
            continue;
        }
        if (entry.isDirectory) {
            manifest.addDirectory(entry.relative);
            continue;
        }
        bytes.clear();
        if (!fs.readFile(join(world, entry.relative).c_str(), &bytes, kMaxStashedFileBytes)) {
            return ConvertResult::SourceUnreadable;
        }
        if (!manifest.addFile(entry.relative, ConstByteSpan(bytes.data(), bytes.size()))) {
            removeTree(fs, staging);
            return ConvertResult::NoSpace;
        }
        unpackedBytes += u64(bytes.size());

        if (++done % options.observeEvery == 0 && !tell(options, "packing", done, total)) {
            removeTree(fs, staging);
            return ConvertResult::Cancelled;
        }
    }
    manifest.summary.unpackedBytes = unpackedBytes;

    if (!manifest.save(fs, join(staging, kManifestName).c_str())) {
        removeTree(fs, staging);
        return ConvertResult::WriteFailed;
    }
    if (!fs.writeFileAtomic(
            join(staging, kReadmeName).c_str(),
            ConstByteSpan(reinterpret_cast<const u8*>(kReadmeText), std::strlen(kReadmeText)))) {
        removeTree(fs, staging);
        return ConvertResult::WriteFailed;
    }

    // Verify from fresh objects and fresh handles, which is the point: it
    // proves the representation on disk is readable, not merely that the one in
    // memory was consistent.
    if (!tell(options, "verifying", 0, total)) {
        removeTree(fs, staging);
        return ConvertResult::Cancelled;
    }
    {
        RegionFile region;
        i32 openX = 0;
        i32 openZ = 0;
        for (const Expected& want : expected) {
            const i32 rx = regionCoord(want.x);
            const i32 rz = regionCoord(want.z);
            if (!region.isOpen() || rx != openX || rz != openZ) {
                region.close();
                if (!region.open(fs, regionPath(staging, rx, rz).c_str(), rx, rz, false)) {
                    removeTree(fs, staging);
                    return ConvertResult::VerifyFailed;
                }
                openX = rx;
                openZ = rz;
            }
            u32 gotBytes = 0;
            u32 gotCrc = 0;
            if (!region.payloadInfo(want.x, want.z, &gotBytes, &gotCrc)
                || gotBytes != want.bytes || gotCrc != want.crc) {
                removeTree(fs, staging);
                return ConvertResult::VerifyFailed;
            }
        }
        region.close();
    }
    {
        Manifest reloaded;
        if (!reloaded.load(fs, join(staging, kManifestName).c_str())) {
            removeTree(fs, staging);
            return ConvertResult::VerifyFailed;
        }
        for (const ManifestEntry& entry : reloaded.entries()) {
            if (entry.isDirectory) {
                continue;
            }
            ConstByteSpan stored;
            if (!reloaded.file(entry.path, &stored) || util::crc32(stored) != entry.crc) {
                removeTree(fs, staging);
                return ConvertResult::VerifyFailed;
            }
        }
    }

    // From here the staged copy is known good, so the source may go.
    if (!fs.writeFileAtomic(join(staging, kCommitMarker).c_str(), ConstByteSpan())) {
        removeTree(fs, staging);
        return ConvertResult::WriteFailed;
    }
    if (!tell(options, "committing", total, total)) {
        // Too late to cancel: the marker is down and the staged copy is the
        // world now. Finishing is the safe answer, not stopping half way.
        (void)0;
    }
    clearWorld(fs, world);
    return moveStagedInto(fs, staging, world) ? ConvertResult::Ok : ConvertResult::WriteFailed;
}

ConvertResult unpackWorld(io::FileSystem& fs, const std::string& world,
                          const ConvertOptions& options)
{
    Manifest manifest;
    if (!manifest.load(fs, join(world, kManifestName).c_str())) {
        return ConvertResult::SourceUnreadable;
    }

    Children found;
    if (!fs.listDirectory(world.c_str(), &found, collectChildren)) {
        return ConvertResult::SourceUnreadable;
    }
    std::vector<std::pair<i32, i32>> regions;
    for (const auto& entry : found.entries) {
        if (entry.second) {
            continue;
        }
        int rx = 0;
        int rz = 0;
        int consumed = 0;
        if (std::sscanf(entry.first.c_str(), "r.%d.%d.3dr%n", &rx, &rz, &consumed) == 2
            && entry.first.size() == usize(consumed)) {
            regions.emplace_back(i32(rx), i32(rz));
        }
    }

    struct Pending {
        i32 x = 0;
        i32 z = 0;
    };
    std::vector<Pending> chunks;
    for (const auto& coords : regions) {
        RegionFile region;
        if (!region.open(fs, regionPath(world, coords.first, coords.second).c_str(),
                         coords.first, coords.second, false)) {
            return ConvertResult::SourceUnreadable;
        }
        struct Collect {
            std::vector<Pending>* out;
        } collect{&chunks};
        region.forEachChunk(&collect, [](void* context, i32 x, i32 z) {
            static_cast<Collect*>(context)->out->push_back(Pending{x, z});
            return true;
        });

        // **Dry run first, with no writes at all.** Every payload is read back
        // and checked against the CRC the directory recorded while the packed
        // copy is still the only representation there is.
        std::vector<u8> bytes;
        for (const Pending& chunk : chunks) {
            if (regionCoord(chunk.x) != coords.first || regionCoord(chunk.z) != coords.second) {
                continue;
            }
            u32 wantBytes = 0;
            u32 wantCrc = 0;
            bytes.clear();
            if (!region.payloadInfo(chunk.x, chunk.z, &wantBytes, &wantCrc)
                || !region.read(chunk.x, chunk.z, &bytes) || bytes.size() != wantBytes
                || util::crc32(ConstByteSpan(bytes.data(), bytes.size())) != wantCrc) {
                return ConvertResult::VerifyFailed;
            }
        }
    }

    const u32 total = u32(chunks.size() + manifest.entries().size());
    u32 done = 0;
    if (!tell(options, "verifying", 0, total)) {
        return ConvertResult::Cancelled;
    }

    // Unpacking writes in place rather than staging. Staging would mean
    // creating a leaf directory and renaming a file for every chunk at commit
    // time -- for a 660-chunk world, some 1,350 extra IPC round trips -- and
    // the marker below buys the same recoverability for one write.
    if (!fs.writeFileAtomic(join(world, kUnpackMarker).c_str(), ConstByteSpan())) {
        return ConvertResult::WriteFailed;
    }

    for (const auto& coords : regions) {
        RegionFile region;
        if (!region.open(fs, regionPath(world, coords.first, coords.second).c_str(),
                         coords.first, coords.second, false)) {
            return ConvertResult::SourceUnreadable;
        }
        std::vector<u8> bytes;
        for (const Pending& chunk : chunks) {
            if (regionCoord(chunk.x) != coords.first || regionCoord(chunk.z) != coords.second) {
                continue;
            }
            bytes.clear();
            if (!region.read(chunk.x, chunk.z, &bytes)) {
                return ConvertResult::SourceUnreadable;
            }
            typename mcver::ChunkLayout::Path directory;
            typename mcver::ChunkLayout::Path file;
            if (!mcver::ChunkLayout::directoryPath(world, chunk.x, chunk.z, &directory)
                || !mcver::ChunkLayout::filePath(world, chunk.x, chunk.z, &file)) {
                return ConvertResult::WriteFailed;
            }
            if (!fs.makeDirectories(directory.text)
                || !fs.writeFileAtomic(file.text,
                                       ConstByteSpan(bytes.data(), bytes.size()))) {
                return ConvertResult::WriteFailed;
            }
            if (++done % options.observeEvery == 0
                && !tell(options, "unpacking", done, total)) {
                // The marker is still down, so the recovery pass rolls this
                // back to the packed world it came from.
                return ConvertResult::Cancelled;
            }
        }
    }

    for (const ManifestEntry& entry : manifest.entries()) {
        const std::string path = join(world, entry.path);
        if (entry.isDirectory) {
            if (!fs.makeDirectories(path.c_str())) {
                return ConvertResult::WriteFailed;
            }
            continue;
        }
        ConstByteSpan stored;
        if (!manifest.file(entry.path, &stored) || util::crc32(stored) != entry.crc) {
            return ConvertResult::VerifyFailed;
        }
        // A stashed file can sit in a directory of its own that nothing else
        // recreates.
        const usize slash = entry.path.rfind('/');
        if (slash != std::string::npos
            && !fs.makeDirectories(join(world, entry.path.substr(0, slash)).c_str())) {
            return ConvertResult::WriteFailed;
        }
        if (!fs.writeFileAtomic(path.c_str(), stored)) {
            return ConvertResult::WriteFailed;
        }
        if (++done % options.observeEvery == 0 && !tell(options, "unpacking", done, total)) {
            return ConvertResult::Cancelled;
        }
    }

    // The folder is complete, so the packed representation may go.
    for (const auto& coords : regions) {
        fs.removeFile(regionPath(world, coords.first, coords.second).c_str());
    }
    fs.removeFile(join(world, kManifestName).c_str());
    fs.removeFile(join(world, kReadmeName).c_str());
    fs.removeFile(join(world, kUnpackMarker).c_str());
    return ConvertResult::Ok;
}

}  // namespace

const char* describeConvertResult(ConvertResult result)
{
    switch (result) {
    case ConvertResult::Ok:
        return "done";
    case ConvertResult::Cancelled:
        return "cancelled; the world is unchanged";
    case ConvertResult::NotAWorld:
        return "that is not a world";
    case ConvertResult::AlreadyInFormat:
        return "it is already in that format";
    case ConvertResult::NoSpace:
        return "not enough space on the card";
    case ConvertResult::SourceUnreadable:
        return "the world could not be read";
    case ConvertResult::VerifyFailed:
        return "the copy did not verify; nothing was changed";
    case ConvertResult::WriteFailed:
        return "the card could not be written";
    case ConvertResult::ReservedName:
        return "the world holds a file this format needs the name of";
    }
    return "unknown";
}

ConvertResult estimateConversion(io::FileSystem& fs, std::string_view worldDir,
                                 WorldFormat target, ConvertEstimate* out)
{
    *out = ConvertEstimate();

    const std::string world(worldDir);
    const WorldFormat current = detectFormat(fs, world);
    if (current == WorldFormat::Unknown) {
        return ConvertResult::NotAWorld;
    }
    if (current == target) {
        return ConvertResult::AlreadyInFormat;
    }

    io::VolumeInfo volume;
    if (io::queryVolumeInfo(world.c_str(), &volume)) {
        out->clusterSize = volume.clusterSize;
        out->freeBytes = volume.freeBytes;
    }

    WorldSize size;
    if (!worldSize(fs, world, &size)) {
        return ConvertResult::SourceUnreadable;
    }
    out->sourceOnDisk = size.onDiskBytes;
    out->files = size.fileCount;

    if (target == WorldFormat::Packed) {
        std::vector<Found> files;
        if (!scanTree(fs, world, std::string(), &files, 0)) {
            return ConvertResult::SourceUnreadable;
        }
        classifyChunks(world, &files);

        u64 payloadSectors = 0;
        u64 blobBytes = 0;
        std::vector<u64> regions;
        for (const Found& entry : files) {
            if (entry.isDirectory) {
                continue;
            }
            if (!entry.isChunk) {
                blobBytes += entry.bytes;
                continue;
            }
            ++out->chunks;
            payloadSectors += (entry.bytes + kSectorBytes - 1) / kSectorBytes;
            const u64 key = (u64(u32(regionCoord(entry.chunkX))) << 32)
                            | u64(u32(regionCoord(entry.chunkZ)));
            if (std::find(regions.begin(), regions.end(), key) == regions.end()) {
                regions.push_back(key);
            }
        }
        // Payload sectors, plus each region's fixed header and directory, plus
        // the manifest -- then every file rounded to a cluster, because that is
        // what the card actually gives up.
        const u64 regionBytes =
            (payloadSectors + u64(regions.size()) * kFirstDataSector) * kSectorBytes;
        out->targetOnDisk = io::onDiskSize(regionBytes, out->clusterSize)
                            + io::onDiskSize(blobBytes + 4096, out->clusterSize)
                            + out->clusterSize;
    } else {
        ManifestSummary summary;
        if (!Manifest::peek(fs, join(world, kManifestName).c_str(), &summary)) {
            return ConvertResult::SourceUnreadable;
        }
        out->chunks = summary.chunkCount;
        // **Unpacking is the expensive direction and the estimate has to say
        // so.** Every chunk becomes its own file in its own leaf directory, and
        // below 64x64 chunks those directories are one per chunk -- so the
        // folder costs roughly two clusters per chunk however small the chunks
        // are. A 200 MB packed world really can need 600 MB.
        const u64 perChunk = out->clusterSize != 0 ? out->clusterSize * 2 : 0;
        out->targetOnDisk = u64(summary.chunkCount) * perChunk
                            + io::onDiskSize(summary.unpackedBytes, out->clusterSize);
    }
    return ConvertResult::Ok;
}

ConvertResult convertWorld(io::FileSystem& fs, std::string_view worldDir, WorldFormat target,
                           const ConvertOptions& options)
{
    const std::string world(worldDir);

    ConvertEstimate estimate;
    const ConvertResult estimated = estimateConversion(fs, world, target, &estimate);
    if (estimated != ConvertResult::Ok) {
        return estimated;
    }
    // Refused up front, with real numbers, rather than at 80 %. Skipped
    // entirely when the card's size is unknown -- that is a normal answer, and
    // it must never be the reason a conversion cannot run.
    if (estimate.clusterSize != 0
        && estimate.freeBytes < estimate.targetOnDisk + options.headroomBytes) {
        return ConvertResult::NoSpace;
    }

    // alpha.ini is ours, and it is carried *as well as* stashed: the manifest
    // holds it like any other file the packer did not recognise, and it is also
    // put back as a plain readable file on the packed side. That is what lets
    // the world list read a gamemode without opening a container.
    //
    // **Its raw bytes, not its parsed value.** Saving rewrites the file from
    // the keys this build knows, so round-tripping through WorldSettings would
    // quietly drop a key a later build wrote -- which is exactly the data loss
    // a conversion is not allowed to cause.
    //
    // A world last played before the rename has only `3dalpha.ini`. Carry that
    // too, under the new name, or converting such a world would leave the
    // packed side with no readable gamemode -- the manifest still holds the old
    // file, so nothing is lost either way, but the world list would stop being
    // able to read it without opening the container.
    std::vector<u8> ourSettings;
    bool hadSettings =
        fs.readFile(settings::worldSettingsPath(world).c_str(), &ourSettings, 64u << 10);
    if (!hadSettings) {
        hadSettings = fs.readFile(settings::legacyWorldSettingsPath(world).c_str(),
                                  &ourSettings, 64u << 10);
    }

    const ConvertResult result = target == WorldFormat::Packed
                                     ? packWorld(fs, world, options)
                                     : unpackWorld(fs, world, options);
    if (result == ConvertResult::Ok && hadSettings) {
        fs.writeFileAtomic(settings::worldSettingsPath(world).c_str(),
                           ConstByteSpan(ourSettings.data(), ourSettings.size()));
    }
    return result;
}

void recoverConversions(io::FileSystem& fs, std::string_view savesDir)
{
    Children found;
    if (!fs.listDirectory(std::string(savesDir).c_str(), &found, collectChildren)) {
        return;
    }

    for (const auto& entry : found.entries) {
        if (!entry.second) {
            continue;
        }
        const std::string path = join(savesDir, entry.first);

        // A staging directory: finish it if it was complete, discard it if not.
        const std::string suffix = kConvertingSuffix;
        if (entry.first.size() > suffix.size()
            && entry.first.compare(entry.first.size() - suffix.size(), suffix.size(), suffix)
                   == 0) {
            const std::string world =
                join(savesDir, entry.first.substr(0, entry.first.size() - suffix.size()));
            if (fs.exists(join(path, kCommitMarker).c_str())) {
                // The staged copy verified before the marker went down, so it
                // is the world now; finishing is idempotent.
                fs.makeDirectories(world.c_str());
                clearWorld(fs, world);
                moveStagedInto(fs, path, world);
            } else {
                removeTree(fs, path);
            }
            continue;
        }

        // An interrupted unpack, which wrote in place.
        if (!fs.exists(join(path, kUnpackMarker).c_str())) {
            continue;
        }
        if (fs.exists(join(path, kManifestName).c_str())) {
            // The packed representation is still whole, so the half-written
            // folder half is what has to go: everything except what packing
            // produced.
            Children inside;
            if (fs.listDirectory(path.c_str(), &inside, collectChildren)) {
                for (const auto& child : inside.entries) {
                    if (!child.second && (child.first == kManifestName
                                          || child.first == kReadmeName
                                          || isReservedName(child.first))) {
                        continue;
                    }
                    const std::string childPath = join(path, child.first);
                    if (child.second) {
                        removeTree(fs, childPath);
                    } else if (child.first != kUnpackMarker) {
                        fs.removeFile(childPath.c_str());
                    }
                }
            }
        }
        // Either the folder is complete and only the marker is left over, or
        // the rollback above just finished. Both end the same way.
        fs.removeFile(join(path, kUnpackMarker).c_str());
    }
}

}  // namespace mc::world::format
