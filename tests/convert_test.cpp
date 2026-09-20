#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/io/volume_info.hpp"
#include "core/settings/world_settings.hpp"
#include "core/world/any_storage.hpp"
#include "core/world/format/converter.hpp"
#include "core/world/format/manifest.hpp"
#include "core/world/world_format.hpp"
#include "core/world/world_list.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::world;
using namespace mc::world::format;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_cnv_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }

    std::string at(const char* name) const { return std::string(path) + "/" + name; }
};

ChunkColumn makeChunk(i32 x, i32 z, BlockId fill)
{
    ChunkColumn chunk(x, z);
    for (int lx = 0; lx < ChunkColumn::kWidth; ++lx) {
        for (int lz = 0; lz < ChunkColumn::kWidth; ++lz) {
            for (int y = 0; y < 20; ++y) {
                chunk.setBlock(lx, y, lz, fill);
            }
            chunk.heightMap[usize(lz) * ChunkColumn::kWidth + usize(lx)] = 20;
        }
    }
    return chunk;
}

void writeText(io::FileSystem& fs, const std::string& path, const char* text)
{
    CHECK(fs.writeFileAtomic(
        path.c_str(), ConstByteSpan(reinterpret_cast<const u8*>(text), std::strlen(text))));
}

// Every file under a directory, by relative path, with its bytes -- so a whole
// tree can be compared against a whole tree rather than file by file.
using Snapshot = std::map<std::string, std::vector<u8>>;

void snapshotInto(io::FileSystem& fs, const std::string& root, const std::string& relative,
                  Snapshot* out)
{
    struct Children {
        std::vector<std::pair<std::string, bool>> entries;
    } found;
    const std::string absolute = relative.empty() ? root : root + "/" + relative;
    if (!fs.listDirectory(absolute.c_str(), &found, [](void* context, const io::DirEntry& e) {
            static_cast<Children*>(context)->entries.emplace_back(e.name, e.isDirectory);
            return true;
        })) {
        return;
    }
    if (!relative.empty() && found.entries.empty()) {
        (*out)["<dir>" + relative] = std::vector<u8>();
    }
    for (const auto& entry : found.entries) {
        const std::string child =
            relative.empty() ? entry.first : relative + "/" + entry.first;
        if (entry.second) {
            snapshotInto(fs, root, child, out);
            continue;
        }
        std::vector<u8> bytes;
        fs.readFile((root + "/" + child).c_str(), &bytes, 64u << 20);
        (*out)[child] = std::move(bytes);
    }
}

Snapshot snapshot(io::FileSystem& fs, const std::string& root)
{
    Snapshot out;
    snapshotInto(fs, root, std::string(), &out);
    return out;
}

// A folder world with chunks on both sides of both axes, plus the kind of
// clutter a world picks up from tools and older servers.
void buildFolderWorld(io::FileSystem& fs, const std::string& world)
{
    AnyStorage storage(fs);
    CHECK(storage.create(world, 123456789LL, 1000, WorldFormat::Folder) == OpenResult::Ok);
    CHECK(storage.saveChunk(makeChunk(0, 0, 1)));
    CHECK(storage.saveChunk(makeChunk(-1, -1, 2)));
    CHECK(storage.saveChunk(makeChunk(-37, 42, 3)));
    CHECK(storage.saveChunk(makeChunk(100, -100, 4)));
    CHECK(storage.saveChunk(makeChunk(33, 33, 5)));
    CHECK(storage.close(2000));

    writeText(fs, world + "/notes.txt", "a file nobody planned for\n");
    CHECK(fs.makeDirectories((world + "/mods").c_str()));
    writeText(fs, world + "/mods/config.json", "{\"kept\": true}\n");
    CHECK(fs.makeDirectories((world + "/empty").c_str()));

    // A chunk file in the wrong leaf directory. Tools do produce these, and it
    // is NOT a chunk: it does not sit where that chunk belongs, so it has to
    // survive as a stray file rather than be silently relocated or dropped.
    CHECK(fs.makeDirectories((world + "/0/0").c_str()));
    writeText(fs, world + "/0/0/c.-d.18.dat", "not really a chunk\n");
}

int gAllowed = 0;
bool observeCancelAfter(void* context, const ConvertProgress& progress)
{
    (void)progress;
    int& budget = *static_cast<int*>(context);
    return budget-- > 0;
}

// A card with room to spare, so on-disk cost is cluster-rounded rather than
// reported raw.
bool roomyVolume(const char* path, io::VolumeInfo* out)
{
    (void)path;
    out->clusterSize = 16384;
    out->freeBytes = 1u << 30;
    out->totalBytes = 1u << 30;
    return true;
}

bool tinyVolume(const char* path, io::VolumeInfo* out)
{
    (void)path;
    out->clusterSize = 16384;
    out->freeBytes = 4096;  // a card with nothing left on it
    out->totalBytes = 1u << 30;
    return true;
}

}  // namespace

TEST(a_world_survives_a_round_trip_through_the_packed_format)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    const Snapshot before = snapshot(fs, world);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);
    CHECK(detectFormat(fs, world) == WorldFormat::Packed);
    // level.dat has moved inside the manifest, so a PC client cannot open the
    // folder and generate terrain over it.
    CHECK(!fs.exists((world + "/level.dat").c_str()));
    CHECK(fs.exists((world + "/" + kReadmeName).c_str()));

    CHECK(convertWorld(fs, world, WorldFormat::Folder, options) == ConvertResult::Ok);
    CHECK(detectFormat(fs, world) == WorldFormat::Folder);

    const Snapshot after = snapshot(fs, world);

    // session.lock is rewritten every time a world is opened, in both formats,
    // so its contents are not part of what a round trip has to preserve.
    Snapshot expected = before;
    Snapshot got = after;
    expected.erase("session.lock");
    got.erase("session.lock");

    CHECK_EQ(got.size(), expected.size());
    for (const auto& entry : expected) {
        const auto found = got.find(entry.first);
        CHECK(found != got.end());
        if (found != got.end()) {
            CHECK(found->second == entry.second);
        }
    }
    // And nothing appeared that was not there before.
    for (const auto& entry : got) {
        CHECK(expected.find(entry.first) != expected.end());
    }
}

TEST(a_packed_world_still_plays_every_chunk_it_was_given)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);

    AnyStorage storage(fs);
    CHECK(storage.open(world, 5000) == OpenResult::Ok);
    CHECK(storage.format() == WorldFormat::Packed);
    CHECK_EQ(storage.level().randomSeed, i64(123456789LL));

    const i32 coords[5][2] = {{0, 0}, {-1, -1}, {-37, 42}, {100, -100}, {33, 33}};
    const int fills[5] = {1, 2, 3, 4, 5};
    for (int i = 0; i < 5; ++i) {
        ChunkColumn loaded(0, 0);
        CHECK(storage.loadChunk(coords[i][0], coords[i][1], &loaded));
        CHECK_EQ(int(loaded.block(1, 1, 1)), fills[i]);
    }
    CHECK(storage.close(6000));
}

TEST(converting_carries_the_per_world_settings_across)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    settings::WorldSettings mine;
    mine.gamemode = settings::Gamemode::Creative;
    CHECK(settings::saveWorldSettings(fs, world, mine));

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);

    // Ours, so it stays a plain readable file rather than being stashed in the
    // manifest -- which is what lets the world list read it without opening a
    // container.
    CHECK(fs.exists(settings::worldSettingsPath(world).c_str()));
    settings::WorldSettings read;
    CHECK(settings::loadWorldSettings(fs, world, &read));
    CHECK(read.gamemode == settings::Gamemode::Creative);

    CHECK(convertWorld(fs, world, WorldFormat::Folder, options) == ConvertResult::Ok);
    CHECK(settings::loadWorldSettings(fs, world, &read));
    CHECK(read.gamemode == settings::Gamemode::Creative);
}

TEST(a_settings_key_this_build_does_not_know_survives_a_conversion)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    // What a later build that added a setting would leave behind. Saving
    // rewrites the file from the keys this build knows, so carrying the value
    // rather than the bytes would drop this -- which is the data loss a
    // conversion is not allowed to cause.
    const char* written = "gamemode=creative\nfrom-a-later-build=42\n";
    writeText(fs, settings::worldSettingsPath(world), written);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);

    std::vector<u8> back;
    CHECK(fs.readFile(settings::worldSettingsPath(world).c_str(), &back, 64u << 10));
    CHECK_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()),
             std::string(written));

    CHECK(convertWorld(fs, world, WorldFormat::Folder, options) == ConvertResult::Ok);
    back.clear();  // readFile appends, deliberately -- see io::FileSystem
    CHECK(fs.readFile(settings::worldSettingsPath(world).c_str(), &back, 64u << 10));
    CHECK_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()),
             std::string(written));
}

// A world last played before the file was renamed is carried under the new
// name, so the packed side still has a gamemode the world list can read without
// opening the container. The bytes are the old file's, untouched.
TEST(packing_carries_a_pre_rename_settings_file_under_the_new_name)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    const char* written = "gamemode=creative\nfrom-a-later-build=42\n";
    writeText(fs, settings::legacyWorldSettingsPath(world), written);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);

    std::vector<u8> back;
    CHECK(fs.readFile(settings::worldSettingsPath(world).c_str(), &back, 64u << 10));
    CHECK_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()),
             std::string(written));

    // And the old file itself came back through the manifest like any other
    // file the packer did not recognise, so nothing was dropped on the way.
    CHECK(convertWorld(fs, world, WorldFormat::Folder, options) == ConvertResult::Ok);
    back.clear();
    CHECK(fs.readFile(settings::legacyWorldSettingsPath(world).c_str(), &back, 64u << 10));
    CHECK_EQ(std::string(reinterpret_cast<const char*>(back.data()), back.size()),
             std::string(written));
}

TEST(converting_a_world_that_is_already_in_that_format_changes_nothing)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    const Snapshot before = snapshot(fs, world);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Folder, options)
          == ConvertResult::AlreadyInFormat);
    CHECK(snapshot(fs, world) == before);

    CHECK(convertWorld(fs, temp.at("Nothing"), WorldFormat::Packed, options)
          == ConvertResult::NotAWorld);
}

TEST(a_world_holding_a_reserved_name_is_refused_rather_than_collided_with)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    // A world that has been through an unpack keeps nothing of ours, but a
    // player who copied one folder over another could well leave this behind.
    // Packing would overwrite it, so it is refused instead.
    writeText(fs, world + "/" + kReadmeName, "left over from somewhere\n");
    const Snapshot before = snapshot(fs, world);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::ReservedName);
    CHECK(snapshot(fs, world) == before);
}

TEST(a_reserved_name_is_caught_at_any_depth_not_just_the_root)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    // Nested, so it never reaches the manifest's own name -- but unpacking
    // would restore it into a folder that by then holds ours, so the collision
    // is real either way and the check is by file name, not by path.
    writeText(fs, world + "/mods/" + kManifestName, "not a manifest\n");
    const Snapshot before = snapshot(fs, world);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::ReservedName);
    CHECK(snapshot(fs, world) == before);
}

TEST(a_world_holding_the_manifests_own_name_reads_as_a_half_finished_conversion)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    writeText(fs, world + "/" + kManifestName, "something else entirely\n");

    // **Packed wins the probe**, deliberately: a folder holding both is not a
    // shape this code ever writes, so reading it as Folder would hand a caller
    // a level.dat whose chunks may already have been moved into a manifest.
    // The consequence here is that packing it is a no-op rather than a
    // reserved-name refusal.
    CHECK(detectFormat(fs, world) == WorldFormat::Packed);
    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options)
          == ConvertResult::AlreadyInFormat);
}

TEST(a_conversion_that_will_not_fit_is_refused_before_it_starts)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    const Snapshot before = snapshot(fs, world);

    io::setVolumeInfoQuery(&tinyVolume);
    ConvertOptions options;
    const ConvertResult result = convertWorld(fs, world, WorldFormat::Packed, options);
    io::setVolumeInfoQuery(nullptr);

    CHECK(result == ConvertResult::NoSpace);
    // Refused up front means nothing was written at all -- not a staging
    // directory, not a byte of the world.
    CHECK(snapshot(fs, world) == before);
    CHECK(!fs.exists((world + kConvertingSuffix).c_str()));
}

TEST(cancelling_a_pack_leaves_the_world_exactly_as_it_was)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");

    buildFolderWorld(fs, world);
    const Snapshot before = snapshot(fs, world);

    gAllowed = 1;
    ConvertOptions options;
    options.context = &gAllowed;
    options.observe = &observeCancelAfter;
    options.observeEvery = 1;

    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Cancelled);
    CHECK(snapshot(fs, world) == before);
    CHECK(!fs.exists((world + kConvertingSuffix).c_str()));

    // And it still opens.
    AnyStorage storage(fs);
    CHECK(storage.open(world, 9000) == OpenResult::Ok);
    CHECK(storage.close(9000));
}

TEST(a_staging_directory_is_never_offered_as_a_world)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    const std::string world = saves + "/World1";
    buildFolderWorld(fs, world);

    // Mid-pack, a staging directory holds a perfectly valid world.3dm. Listing
    // it would offer the player a half-written world to play.
    const std::string staging = world + kConvertingSuffix;
    CHECK(fs.makeDirectories(staging.c_str()));
    writeText(fs, staging + "/" + kManifestName, "pretend manifest\n");

    std::vector<WorldEntry> worlds;
    listWorlds(fs, saves, &worlds);
    CHECK_EQ(worlds.size(), usize(1));
    CHECK_EQ(worlds[0].name, std::string("World1"));
}

TEST(an_abandoned_staging_directory_is_discarded_on_the_next_scan)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    const std::string world = saves + "/World1";
    buildFolderWorld(fs, world);
    const Snapshot before = snapshot(fs, world);

    // A power cut before the commit marker went down. The staged copy is worth
    // nothing and the original is whole.
    const std::string staging = world + kConvertingSuffix;
    CHECK(fs.makeDirectories(staging.c_str()));
    writeText(fs, staging + "/" + kManifestName, "half written\n");

    recoverConversions(fs, saves);
    CHECK(!fs.exists(staging.c_str()));
    CHECK(snapshot(fs, world) == before);
}

TEST(an_interrupted_unpack_rolls_back_to_the_packed_world)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    const std::string world = saves + "/World1";
    buildFolderWorld(fs, world);

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);
    const Snapshot packed = snapshot(fs, world);

    // A power cut part way through writing the folder half: the marker is down
    // and some chunk files exist, but world.3dm is still whole.
    writeText(fs, world + "/" + kUnpackMarker, "");
    CHECK(fs.makeDirectories((world + "/1f/18").c_str()));
    writeText(fs, world + "/1f/18/c.-d.18.dat", "half a chunk\n");

    recoverConversions(fs, saves);

    CHECK(detectFormat(fs, world) == WorldFormat::Packed);
    CHECK(!fs.exists((world + "/" + kUnpackMarker).c_str()));
    CHECK(snapshot(fs, world) == packed);

    // And it still plays.
    AnyStorage storage(fs);
    CHECK(storage.open(world, 7000) == OpenResult::Ok);
    ChunkColumn loaded(0, 0);
    CHECK(storage.loadChunk(-37, 42, &loaded));
    CHECK_EQ(int(loaded.block(1, 1, 1)), 3);
    CHECK(storage.close(7000));
}

TEST(the_estimate_says_what_a_conversion_will_cost_before_it_runs)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");
    buildFolderWorld(fs, world);

    ConvertEstimate estimate;
    CHECK(estimateConversion(fs, world, WorldFormat::Packed, &estimate) == ConvertResult::Ok);
    CHECK_EQ(estimate.chunks, u32(5));
    CHECK(estimate.files >= 5);

    // Nothing is written by an estimate.
    CHECK(!fs.exists((world + kConvertingSuffix).c_str()));
    CHECK(detectFormat(fs, world) == WorldFormat::Folder);
}

TEST(deleting_and_copying_work_on_both_formats)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");
    buildFolderWorld(fs, world);

    // A copy preserves whichever format the source is in: it is a backup, not
    // a conversion.
    const std::string copy = temp.at("Copy");
    CHECK(copyWorld(fs, world, copy));
    CHECK(detectFormat(fs, copy) == WorldFormat::Folder);
    CHECK(snapshot(fs, copy) == snapshot(fs, world));

    // Never merges into something already there.
    CHECK(!copyWorld(fs, world, copy));

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);

    const std::string packedCopy = temp.at("PackedCopy");
    CHECK(copyWorld(fs, world, packedCopy));
    CHECK(detectFormat(fs, packedCopy) == WorldFormat::Packed);

    // Delete accepts both shapes and still refuses what is not a world.
    CHECK(deleteWorld(fs, packedCopy));
    CHECK(!fs.exists(packedCopy.c_str()));
    CHECK(deleteWorld(fs, copy));
    CHECK(!deleteWorld(fs, temp.at("nothing-here")));
}

TEST(a_packed_world_reports_a_smaller_footprint_than_the_folder_it_came_from)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string world = temp.at("World1");
    buildFolderWorld(fs, world);

    // With a real cluster size in play, the folder format pays one cluster per
    // chunk file *and* one per leaf directory. That is the whole argument for
    // the packed format, so it is worth asserting rather than assuming.
    io::setVolumeInfoQuery(&roomyVolume);

    WorldSize folder;
    CHECK(worldSize(fs, world, &folder));

    ConvertOptions options;
    CHECK(convertWorld(fs, world, WorldFormat::Packed, options) == ConvertResult::Ok);

    WorldSize packed;
    CHECK(worldSize(fs, world, &packed));
    io::setVolumeInfoQuery(nullptr);

    CHECK(packed.onDiskBytes < folder.onDiskBytes);
    CHECK(packed.fileCount < folder.fileCount);
}
