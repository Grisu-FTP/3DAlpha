// A world crossing a link: what arrives, what is refused, and what is left
// behind when it goes wrong.
//
// Both consoles run in one process over a `Wire` that drops frames on purpose,
// because the transfer's whole claim is that a walk across the room does not
// corrupt a world -- and the console's own radio cannot be asked to lose the
// third frame. Real files on both sides, for the reason world_list_test gives:
// a path built wrong or a directory left behind by a failure does not exist
// against a stub.

#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/net/link.hpp"
#include "core/net/world_copy.hpp"
#include "core/world/world_list.hpp"
#include "core/world/world_transfer.hpp"
#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_copy_XXXXXX");
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

// A frame in flight. The same shape link_test uses, kept separate because these
// two ends are not a session and have no host and guest.
struct Frame {
    std::vector<u8> bytes;
};

class Wire {
public:
    class End : public link::Datagrams {
    public:
        End(Wire* wire, bool fromSender) : wire_(wire), fromSender_(fromSender) {}

        bool send(u16 node, const u8* data, usize size) override
        {
            (void)node;
            return wire_->carry(fromSender_, data, size);
        }

        bool receive(u8* buffer, usize capacity, usize* size, u16* node) override
        {
            *node = 1;
            return wire_->take(fromSender_, buffer, capacity, size);
        }

    private:
        Wire* wire_;
        bool fromSender_;
    };

    Wire() : sender(this, true), receiver(this, false) {}

    bool carry(bool fromSender, const u8* data, usize size)
    {
        ++carried;
        if (dropEvery > 0 && carried % dropEvery == 0) {
            ++dropped;
            return true;  // lost on the air, which is not a failure to send
        }
        (fromSender ? toReceiver_ : toSender_).push_back(Frame{std::vector<u8>(data, data + size)});
        return true;
    }

    bool take(bool fromSender, u8* buffer, usize capacity, usize* size)
    {
        std::deque<Frame>& queue = fromSender ? toSender_ : toReceiver_;
        if (queue.empty()) {
            return false;
        }
        const Frame frame = queue.front();
        queue.pop_front();
        if (frame.bytes.size() > capacity) {
            return false;
        }
        for (usize i = 0; i < frame.bytes.size(); ++i) {
            buffer[i] = frame.bytes[i];
        }
        *size = frame.bytes.size();
        return true;
    }

    End sender;
    End receiver;
    int dropEvery = 0;
    int carried = 0;
    int dropped = 0;

private:
    std::deque<Frame> toSender_;
    std::deque<Frame> toReceiver_;
};

// One end of the link, pumped the way a frame of the menu would pump it.
template <typename T>
void pumpEnd(T& end, link::Peer& peer, Wire::End& wire, u32 nowMs)
{
    end.pump(peer);
    peer.flush(nowMs, wire, 1);

    u8 datagram[link::kMaxDatagram];
    usize size = 0;
    u16 node = 0;
    while (wire.receive(datagram, sizeof(datagram), &size, &node)) {
        struct Ctx {
            T* end;
        } ctx{&end};
        peer.receive(datagram, size, nowMs, [](void* c, link::Msg kind, const u8* body,
                                               usize bodySize) {
            static_cast<Ctx*>(c)->end->onMessage(kind, body, bodySize);
        }, &ctx);
    }
}

// Runs both ends until they stop or `frames` go by. Returns the frames used, so
// a test can say a transfer finished rather than merely stopped failing.
int run(Wire& wire, copy::Sender& sender, copy::Receiver& receiver, int frames = 20000)
{
    link::Peer sendPeer;
    link::Peer recvPeer;
    u32 nowMs = 0;
    sendPeer.reset(nowMs);
    recvPeer.reset(nowMs);

    for (int i = 0; i < frames; ++i) {
        nowMs += 16;
        pumpEnd(sender, sendPeer, wire.sender, nowMs);
        pumpEnd(receiver, recvPeer, wire.receiver, nowMs);
        if (sender.finished() && receiver.finished()) {
            return i;
        }
    }
    return frames;
}

// A world with something in it: the storage layer's own level.dat, plus files
// in the nested shape the Alpha layout puts chunks in.
//
// The filler is not decoration. A world of one small file crosses in a handful
// of datagrams, which is too few for a link that drops one frame in five to
// drop anything at all -- so the lossy test would pass without ever having lost
// a frame. `bytes` is what makes the transfer long enough to be a transfer.
void makeWorld(io::FileSystem& fs, const std::string& dir, i64 seed, usize bytes = 200000)
{
    mcver::Storage storage(fs);
    CHECK(storage.create(dir, seed, 1000) == world::OpenResult::Ok);
    CHECK(storage.close(2000));

    // Deterministic rather than random: a byte that differs between the two
    // trees should be a failure of the transfer, not of the fixture.
    usize written = 0;
    for (int i = 0; written < bytes; ++i) {
        char leaf[64];
        std::snprintf(leaf, sizeof(leaf), "%d/%d/c.%d.%d.dat", i % 4, i % 3, i, i);
        const std::string path = dir + "/" + leaf;
        const std::string parent = path.substr(0, path.rfind('/'));
        CHECK(fs.makeDirectories(parent.c_str()));

        const usize size = 3000 + usize(i) * 37;
        std::vector<u8> payload(size);
        for (usize b = 0; b < size; ++b) {
            payload[b] = u8((b * 31 + usize(i) * 7 + usize(seed)) & 0xFF);
        }
        CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan(payload.data(), payload.size())));
        written += size;
    }
}

// Everything under `dir`, as "path:size" lines, for comparing two trees without
// caring what readdir handed back.
void fingerprint(io::FileSystem& fs, const std::string& dir, std::vector<std::string>* out)
{
    world::TransferManifest manifest;
    CHECK(world::buildTransferManifest(fs, dir, &manifest));
    for (const world::TransferFile& file : manifest.files) {
        out->push_back(file.path + ":" + std::to_string(file.size));
    }
}

}  // namespace

TEST(a_relative_path_off_a_link_may_not_climb_out_of_the_folder_it_is_written_in)
{
    CHECK(world::safeRelativePath("level.dat"));
    CHECK(world::safeRelativePath("0/0/c.0.0.dat"));

    CHECK(!world::safeRelativePath(""));
    CHECK(!world::safeRelativePath("/etc/passwd"));
    CHECK(!world::safeRelativePath("../outside"));
    CHECK(!world::safeRelativePath("a/../../b"));
    CHECK(!world::safeRelativePath("a/./b"));
    CHECK(!world::safeRelativePath("a//b"));
    CHECK(!world::safeRelativePath("a/"));
    CHECK(!world::safeRelativePath("..\\windows"));
    CHECK(!world::safeRelativePath("C:/x"));
    CHECK(!world::safeRelativePath(std::string("a\nb")));
    CHECK(!world::safeRelativePath("trailing."));
    CHECK(!world::safeRelativePath("trailing "));
    CHECK(!world::safeRelativePath(std::string(world::kMaxTransferPath + 1, 'a')));
}

TEST(a_manifest_lists_a_worlds_files_in_an_order_that_does_not_depend_on_readdir)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string world = temp.at("World1");
    makeWorld(fs, world, 42);

    world::TransferManifest manifest;
    CHECK(world::buildTransferManifest(fs, world, &manifest));
    CHECK(!manifest.files.empty());
    CHECK(manifest.totalBytes > 0);
    for (usize i = 1; i < manifest.files.size(); ++i) {
        CHECK(manifest.files[i - 1].path < manifest.files[i].path);
    }

    // A folder that is not a world is not something to offer under a world's
    // name, which is the same guard copyWorld and deleteWorld use.
    const std::string notAWorld = temp.at("junk");
    CHECK(fs.makeDirectories(notAWorld.c_str()));
    world::TransferManifest refused;
    CHECK(!world::buildTransferManifest(fs, notAWorld, &refused));
}

TEST(a_world_crosses_a_link_intact_and_the_original_is_left_alone)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string sourceSaves = temp.at("from");
    const std::string targetSaves = temp.at("to");
    CHECK(fs.makeDirectories(sourceSaves.c_str()));
    CHECK(fs.makeDirectories(targetSaves.c_str()));

    const std::string source = sourceSaves + "/Alpha";
    makeWorld(fs, source, 987654321);

    std::vector<std::string> before;
    fingerprint(fs, source, &before);

    Wire wire;
    copy::Sender sender(fs, source, "Alpha");
    std::string error;
    CHECK(sender.begin(&error));
    copy::Receiver receiver(fs, targetSaves, "Copied");

    run(wire, sender, receiver);

    CHECK_EQ(sender.error(), std::string());
    CHECK_EQ(receiver.error(), std::string());
    CHECK(sender.stage() == copy::Stage::Done);
    CHECK(receiver.stage() == copy::Stage::Done);
    CHECK_EQ(receiver.sourceName(), std::string("Alpha"));

    // The copy is a world, under the name that was asked for.
    std::vector<world::WorldEntry> listed;
    world::listWorlds(fs, targetSaves, &listed);
    CHECK_EQ(listed.size(), usize(1));
    CHECK_EQ(listed[0].name, std::string("Copied"));

    // File for file, byte for byte.
    std::vector<std::string> arrived;
    fingerprint(fs, targetSaves + "/Copied", &arrived);
    CHECK(arrived == before);

    world::TransferManifest manifest;
    CHECK(world::buildTransferManifest(fs, source, &manifest));
    for (const world::TransferFile& file : manifest.files) {
        std::vector<u8> original;
        std::vector<u8> copied;
        CHECK(fs.readFile((source + "/" + file.path).c_str(), &original, 1u << 22));
        CHECK(fs.readFile((targetSaves + "/Copied/" + file.path).c_str(), &copied, 1u << 22));
        CHECK(original == copied);
    }

    // **The original is untouched**, which is the whole promise of an export.
    std::vector<std::string> after;
    fingerprint(fs, source, &after);
    CHECK(after == before);

    // Nothing is left staged.
    CHECK(!fs.exists((targetSaves + "/" + world::kImportStagingName).c_str()));
}

TEST(a_world_still_arrives_intact_over_a_link_that_loses_frames)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string sourceSaves = temp.at("from");
    const std::string targetSaves = temp.at("to");
    CHECK(fs.makeDirectories(sourceSaves.c_str()));
    CHECK(fs.makeDirectories(targetSaves.c_str()));

    const std::string source = sourceSaves + "/Lossy";
    makeWorld(fs, source, 5);

    std::vector<std::string> before;
    fingerprint(fs, source, &before);

    Wire wire;
    wire.dropEvery = 5;  // one frame in five never lands
    copy::Sender sender(fs, source, "Lossy");
    std::string error;
    CHECK(sender.begin(&error));
    copy::Receiver receiver(fs, targetSaves, "Lossy");

    run(wire, sender, receiver);

    CHECK(wire.dropped > 0);
    CHECK(sender.stage() == copy::Stage::Done);
    CHECK(receiver.stage() == copy::Stage::Done);

    std::vector<std::string> arrived;
    fingerprint(fs, targetSaves + "/Lossy", &arrived);
    CHECK(arrived == before);
}

TEST(a_transfer_stopped_half_way_leaves_nothing_on_either_console)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string sourceSaves = temp.at("from");
    const std::string targetSaves = temp.at("to");
    CHECK(fs.makeDirectories(sourceSaves.c_str()));
    CHECK(fs.makeDirectories(targetSaves.c_str()));

    const std::string source = sourceSaves + "/Half";
    makeWorld(fs, source, 11);

    std::vector<std::string> before;
    fingerprint(fs, source, &before);

    Wire wire;
    copy::Sender sender(fs, source, "Half");
    std::string error;
    CHECK(sender.begin(&error));
    copy::Receiver receiver(fs, targetSaves, "Half");

    link::Peer sendPeer;
    link::Peer recvPeer;
    u32 nowMs = 0;
    sendPeer.reset(nowMs);
    recvPeer.reset(nowMs);

    // Far enough in that files are crossing, and nowhere near the end.
    for (int i = 0; i < 12; ++i) {
        nowMs += 16;
        pumpEnd(sender, sendPeer, wire.sender, nowMs);
        pumpEnd(receiver, recvPeer, wire.receiver, nowMs);
    }
    CHECK(receiver.stage() == copy::Stage::Copying);
    CHECK(receiver.progress().bytesDone > 0);

    // The player on the sending console presses B.
    sender.abandon("the other console cancelled");
    for (int i = 0; i < 40; ++i) {
        nowMs += 16;
        pumpEnd(sender, sendPeer, wire.sender, nowMs);
        pumpEnd(receiver, recvPeer, wire.receiver, nowMs);
    }

    // **The receiver was told**, rather than left to a ten-second silence, and
    // it threw away what had arrived.
    CHECK(receiver.stage() == copy::Stage::Failed);
    CHECK_EQ(receiver.error(), std::string("the other console cancelled"));
    CHECK(!fs.exists((targetSaves + "/" + world::kImportStagingName).c_str()));

    std::vector<world::WorldEntry> listed;
    world::listWorlds(fs, targetSaves, &listed);
    CHECK_EQ(listed.size(), usize(0));

    // And the world that was being sent is exactly as it was.
    std::vector<std::string> after;
    fingerprint(fs, source, &after);
    CHECK(after == before);
}

TEST(an_import_onto_a_name_that_is_taken_is_refused_before_anything_is_written)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string sourceSaves = temp.at("from");
    const std::string targetSaves = temp.at("to");
    CHECK(fs.makeDirectories(sourceSaves.c_str()));
    CHECK(fs.makeDirectories(targetSaves.c_str()));

    const std::string source = sourceSaves + "/Alpha";
    makeWorld(fs, source, 1);
    const std::string existing = targetSaves + "/Alpha";
    makeWorld(fs, existing, 2);

    std::vector<std::string> before;
    fingerprint(fs, existing, &before);

    Wire wire;
    copy::Sender sender(fs, source, "Alpha");
    std::string error;
    CHECK(sender.begin(&error));
    copy::Receiver receiver(fs, targetSaves, "Alpha");

    run(wire, sender, receiver);

    CHECK(receiver.stage() == copy::Stage::Failed);
    // The refusal reached the sending console rather than being left to a
    // timeout, which is what puts a reason on the far screen.
    CHECK(sender.stage() == copy::Stage::Failed);
    CHECK(!sender.error().empty());

    // The world that was already there is exactly as it was.
    std::vector<std::string> after;
    fingerprint(fs, existing, &after);
    CHECK(after == before);
    CHECK(!fs.exists((targetSaves + "/" + world::kImportStagingName).c_str()));
}

TEST(an_abandoned_import_leaves_no_world_behind_and_the_staging_is_recoverable)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    {
        world::ImportStaging staging(fs, saves);
        CHECK(staging.begin());
        CHECK(staging.beginFile("level.dat", 4));
        const u8 half[] = {1, 2};
        CHECK(staging.writeChunk(half, sizeof(half)));
        // Destroyed here, the way a console switched off leaves it: the staging
        // directory is still on the card and nothing is a world.
    }

    std::vector<world::WorldEntry> listed;
    world::listWorlds(fs, saves, &listed);
    CHECK_EQ(listed.size(), usize(0));

    CHECK(fs.exists((saves + "/" + world::kImportStagingName).c_str()));
    world::discardStagedImport(fs, saves);
    CHECK(!fs.exists((saves + "/" + world::kImportStagingName).c_str()));
}

TEST(a_staging_directory_refuses_a_path_that_would_write_outside_it)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    world::ImportStaging staging(fs, saves);
    CHECK(staging.begin());
    CHECK(!staging.beginFile("../escaped.dat", 1));
    CHECK(!staging.beginFile("/absolute.dat", 1));
    CHECK(!fs.exists(temp.at("escaped.dat").c_str()));

    staging.discard();
}
