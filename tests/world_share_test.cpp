// A world shared through AlphaComputer: the transfer port's bytes, the archive
// that rides inside them, and the two jobs that move one.
//
// **The framing is checked against the server's own bytes**, from
// tests/ac_wire_vectors.hpp, for the reason ac_wire_test gives: a round trip
// only proves this file agrees with itself. The archive is ours on both ends,
// so it is checked the way world_copy_test checks a copy over local wireless --
// real files, fingerprinted on both sides -- and then fed what a hostile or
// broken upload would send, because the server passes it on without looking.
//
// The jobs run over a scripted stream that answers the way the server does.
// The real server is the host harness's job: `--online <server> export|import`.

#include "ac_wire_vectors.hpp"
#include "framework.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/net/world_share.hpp"
#include "core/util/compress.hpp"
#include "core/world/world_list.hpp"
#include "core/world/world_transfer.hpp"
#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_share_XXXXXX");
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

const u8 kToken[share::kTokenSize] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
                                      0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11};

bool same(const std::vector<u8>& bytes, const u8* expected, usize size)
{
    return bytes.size() == size && std::memcmp(bytes.data(), expected, size) == 0;
}

#define CHECK_BYTES(bytes, vector) CHECK(same(bytes, vector, sizeof(vector)))

// A world with something in it, as world_copy_test makes one -- plus an empty
// file, which is the one case a length-prefixed body can get wrong.
void makeWorld(io::FileSystem& fs, const std::string& dir, i64 seed, usize bytes = 200000)
{
    mcver::Storage storage(fs);
    CHECK(storage.create(dir, seed, 1000) == world::OpenResult::Ok);
    CHECK(storage.close(2000));

    usize written = 0;
    for (int i = 0; written < bytes; ++i) {
        char leaf[64];
        std::snprintf(leaf, sizeof(leaf), "%d/%d/c.%d.%d.dat", i % 4, i % 3, i, i);
        const std::string path = dir + "/" + leaf;
        CHECK(fs.makeDirectories(path.substr(0, path.rfind('/')).c_str()));

        // Noise rather than a pattern: a chunk file is gzip already and does
        // not deflate again, and a fixture that did would make the compressed
        // stream a fraction of the world and hide every size-dependent path.
        const usize size = 3000 + usize(i) * 37;
        std::vector<u8> payload(size);
        u32 state = u32(seed) * 2654435761u + u32(i) * 40503u + 1u;
        for (usize b = 0; b < size; ++b) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            payload[b] = u8(state >> 24);
        }
        CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan(payload.data(), payload.size())));
        written += size;
    }
    const std::string empty = dir + "/empty.dat";
    CHECK(fs.writeFileAtomic(empty.c_str(), ConstByteSpan(nullptr, 0)));
}

// Every file under `dir`, with its bytes, so two trees are compared for content
// and not only for shape.
void contentsOf(io::FileSystem& fs, const std::string& dir, std::vector<std::string>* out)
{
    world::TransferManifest manifest;
    CHECK(world::buildTransferManifest(fs, dir, &manifest));
    for (const world::TransferFile& file : manifest.files) {
        std::vector<u8> bytes;
        CHECK(fs.readFile((dir + "/" + file.path).c_str(), &bytes, 64u << 20));
        out->push_back(file.path + ":" + std::string(bytes.begin(), bytes.end()));
    }
}

std::vector<std::string> contents(io::FileSystem& fs, const std::string& dir)
{
    std::vector<std::string> out;
    contentsOf(fs, dir, &out);
    return out;
}

// The whole compressed archive of `dir`, drawn out in `piece`-sized helpings.
void packInto(io::FileSystem& fs, const std::string& dir, usize piece, std::vector<u8>* out)
{
    share::ArchiveWriter writer(fs, dir);
    std::string error;
    CHECK(writer.begin(&error));
    for (int guard = 0; !writer.finished() && guard < 1000000; ++guard) {
        CHECK(writer.next(out, piece));
    }
    CHECK(writer.finished());
    CHECK_EQ(writer.bytesRead(), writer.bytesTotal());
    CHECK_EQ(writer.filesRead(), writer.filesTotal());
}

std::vector<u8> pack(io::FileSystem& fs, const std::string& dir, usize piece)
{
    std::vector<u8> out;
    packInto(fs, dir, piece, &out);
    return out;
}

// An archive written by hand, for the ones this console would never write.
struct RawArchive {
    std::vector<u8> bytes;

    RawArchive(u32 files, u64 total, u16 version = share::kArchiveVersion)
    {
        bytes.insert(bytes.end(), share::kArchiveMagic, share::kArchiveMagic + 4);
        put(version, 2);
        put(files, 4);
        put(total, 8);
    }

    void put(u64 value, int width)
    {
        for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) {
            bytes.push_back(u8(value >> shift));
        }
    }

    RawArchive& file(const std::string& path, const std::string& body, u64 declared = ~0ull)
    {
        put(path.size(), 2);
        bytes.insert(bytes.end(), path.begin(), path.end());
        put(declared == ~0ull ? body.size() : declared, 8);
        bytes.insert(bytes.end(), body.begin(), body.end());
        return *this;
    }

    std::vector<u8> deflated() const
    {
        std::vector<u8> out;
        if (!zip::compress(ConstByteSpan(bytes.data(), bytes.size()), out, zip::Wrapper::Zlib)) {
            out.clear();
        }
        return out;
    }
};

// Feeds `archive` to a reader under `saves` and says whether it became a world.
bool unpack(io::FileSystem& fs, const std::string& saves, const std::vector<u8>& archive,
            usize piece, const char* name, std::string* error = nullptr)
{
    share::ArchiveReader reader(fs, saves);
    std::string why;
    if (!reader.begin(&why)) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    }
    for (usize at = 0; at < archive.size(); at += piece) {
        const usize take = archive.size() - at < piece ? archive.size() - at : piece;
        if (!reader.feed(archive.data() + at, take)) {
            if (error != nullptr) {
                *error = reader.error();
            }
            return false;
        }
    }
    const bool ok = reader.finish(name);
    if (!ok && error != nullptr) {
        *error = reader.error();
    }
    return ok;
}

// ---- the scripted server ----------------------------------------------------

// A stream whose far end is a list of frames to answer with. `answerAfter`
// holds a reply back until the console has sent that many bytes, which is how
// the server's "Code before the first Data" and "Stored after End" are acted.
class ScriptedStream : public share::Stream {
public:
    struct Reply {
        usize afterSent = 0;
        std::vector<u8> bytes;
    };

    bool sendAll(const u8* data, usize size, int stallMs, const std::atomic<bool>& cancel,
                 std::string* error) override
    {
        (void)stallMs;
        (void)cancel;
        if (closeAfterSent > 0 && sent.size() + size > closeAfterSent) {
            *error = "The server closed the connection.";
            released = sent.size() + size;
            return false;
        }
        sent.insert(sent.end(), data, data + size);
        return true;
    }

    Status receive(u8* buffer, usize capacity, usize* got, int waitMs) override
    {
        (void)waitMs;
        *got = 0;
        const usize seen = sent.size() > released ? sent.size() : released;
        while (!replies.empty() && replies.front().afterSent <= seen) {
            pending.insert(pending.end(), replies.front().bytes.begin(),
                           replies.front().bytes.end());
            replies.pop_front();
        }
        if (pending.empty()) {
            ++idleReads;
            return closeWhenDrained && replies.empty() ? Status::Closed : Status::Timeout;
        }
        // A byte or a few at a time: the reader has to put frames back
        // together across reads, which a real socket makes it do eventually.
        const usize take = pending.size() < trickle ? pending.size() : trickle;
        const usize n = take < capacity ? take : capacity;
        for (usize i = 0; i < n; ++i) {
            buffer[i] = pending.front();
            pending.pop_front();
        }
        *got = n;
        return Status::Ok;
    }

    void answer(usize afterSent, const u8* bytes, usize size)
    {
        replies.push_back(Reply{afterSent, std::vector<u8>(bytes, bytes + size)});
    }

    void answer(usize afterSent, const std::vector<u8>& bytes)
    {
        replies.push_back(Reply{afterSent, bytes});
    }

    std::vector<u8> sent;
    std::deque<Reply> replies;
    std::deque<u8> pending;
    usize trickle = 7;
    usize closeAfterSent = 0;
    usize released = 0;
    bool closeWhenDrained = false;
    int idleReads = 0;
};

std::vector<u8> frame(share::Kind kind, const std::vector<u8>& payload)
{
    std::vector<u8> out;
    share::appendFrame(kind, payload.data(), payload.size(), &out);
    return out;
}

std::vector<u8> u64Payload(u64 value)
{
    std::vector<u8> out;
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(u8(value >> shift));
    }
    return out;
}

// The Data payloads a console sent, joined: what the server would have stored.
void uploadedBlobInto(const std::vector<u8>& sent, u64* endTotal, std::vector<u8>* out)
{
    std::vector<u8>& blob = *out;
    usize at = sizeof(test::kAcwtOpening);
    // The request frame.
    const u32 requestLength = (u32(sent[at + 1]) << 24) | (u32(sent[at + 2]) << 16)
                              | (u32(sent[at + 3]) << 8) | u32(sent[at + 4]);
    at += 5 + requestLength;
    while (at + 5 <= sent.size()) {
        const u8 kind = sent[at];
        const u32 length = (u32(sent[at + 1]) << 24) | (u32(sent[at + 2]) << 16)
                           | (u32(sent[at + 3]) << 8) | u32(sent[at + 4]);
        CHECK(length <= share::kMaxFrame);
        if (kind == u8(share::Kind::Data)) {
            blob.insert(blob.end(), sent.begin() + std::ptrdiff_t(at + 5),
                        sent.begin() + std::ptrdiff_t(at + 5 + length));
        } else if (kind == u8(share::Kind::End)) {
            u64 total = 0;
            CHECK(share::decodeU64(sent.data() + at + 5, length, &total));
            *endTotal = total;
        }
        at += 5 + length;
    }
    CHECK_EQ(at, sent.size());
}

std::vector<u8> uploadedBlob(const std::vector<u8>& sent, u64* endTotal)
{
    std::vector<u8> blob;
    uploadedBlobInto(sent, endTotal, &blob);
    return blob;
}

// The server's download: Found, the blob in 16 KiB frames, End.
void serveDownload(ScriptedStream* stream, const std::vector<u8>& blob, const char* name,
                   usize requestBytes)
{
    std::vector<u8> found;
    found.push_back(0);
    found.push_back(u8(std::strlen(name)));
    found.insert(found.end(), name, name + std::strlen(name));
    stream->answer(requestBytes, frame(share::Kind::Found, found));
    for (usize at = 0; at < blob.size(); at += 16 * 1024) {
        const usize take = blob.size() - at < 16 * 1024 ? blob.size() - at : 16 * 1024;
        stream->answer(requestBytes,
                       frame(share::Kind::Data,
                             std::vector<u8>(blob.begin() + std::ptrdiff_t(at),
                                             blob.begin() + std::ptrdiff_t(at + take))));
    }
    stream->answer(requestBytes, frame(share::Kind::End, u64Payload(blob.size())));
}

usize downloadRequestBytes(const char* code)
{
    std::vector<u8> request;
    share::appendOpening(&request);
    share::appendDownload(kToken, code, &request);
    return request.size();
}

bool stagingLeft(io::FileSystem& fs, const std::string& saves)
{
    return fs.exists((saves + "/" + world::kImportStagingName).c_str());
}

}  // namespace

// ---- framing ----------------------------------------------------------------

TEST(the_transfer_vectors_were_generated_for_this_transfer_protocol)
{
    CHECK_EQ(int(share::kProtocol), int(test::kAcwtVectorProtocol));
}

TEST(every_transfer_request_is_the_servers_own_bytes)
{
    std::vector<u8> out;
    share::appendOpening(&out);
    CHECK_BYTES(out, test::kAcwtOpening);

    out.clear();
    share::appendUpload(kToken, "New World", &out);
    CHECK_BYTES(out, test::kAcwtUpload);

    out.clear();
    share::appendDownload(kToken, "K7QX2M", &out);
    CHECK_BYTES(out, test::kAcwtDownload);

    out.clear();
    share::appendStop(kToken, &out);
    CHECK_BYTES(out, test::kAcwtStop);

    out.clear();
    const u8 data[] = {1, 2, 3};
    share::appendFrame(share::Kind::Data, data, sizeof(data), &out);
    CHECK_BYTES(out, test::kAcwtDataUp);

    out.clear();
    share::appendEnd(3, &out);
    CHECK_BYTES(out, test::kAcwtEndUp);
}

TEST(every_transfer_answer_reads_back_as_what_the_server_put_in_it)
{
    std::string text;
    CHECK(share::decodeString(test::kAcwtCode + 5, sizeof(test::kAcwtCode) - 5, share::kMaxCode,
                              &text));
    CHECK_EQ(text, std::string("K7QX2M"));
    CHECK(share::decodeString(test::kAcwtFound + 5, sizeof(test::kAcwtFound) - 5,
                              share::kMaxWorldName, &text));
    CHECK_EQ(text, std::string("New World"));
    CHECK(share::decodeString(test::kAcwtError + 5, sizeof(test::kAcwtError) - 5,
                              share::kMaxReason, &text));
    CHECK_EQ(text, std::string("no world is being shared with that code"));

    u64 value = 0;
    CHECK(share::decodeU64(test::kAcwtEnd + 5, sizeof(test::kAcwtEnd) - 5, &value));
    CHECK_EQ(value, u64(0x0102030405060708ull));
    CHECK(share::decodeU64(test::kAcwtStored + 5, sizeof(test::kAcwtStored) - 5, &value));
    CHECK_EQ(value, u64(4096));
    CHECK_EQ(int(test::kAcwtStopped[0]), int(share::Kind::Stopped));

    // A string that claims more than it carries, or carries more than it claims,
    // or is longer than its field, or is not UTF-8, is not read.
    const u8 shortString[] = {0x00, 0x05, 'a', 'b'};
    CHECK(!share::decodeString(shortString, sizeof(shortString), 16, &text));
    const u8 trailing[] = {0x00, 0x01, 'a', 'b'};
    CHECK(!share::decodeString(trailing, sizeof(trailing), 16, &text));
    const u8 tooLong[] = {0x00, 0x03, 'a', 'b', 'c'};
    CHECK(!share::decodeString(tooLong, sizeof(tooLong), 2, &text));
    const u8 notUtf8[] = {0x00, 0x02, 0xC3, 0x28};
    CHECK(!share::decodeString(notUtf8, sizeof(notUtf8), 16, &text));
    const u8 cutUtf8[] = {0x00, 0x01, 0xC3};
    CHECK(!share::decodeString(cutUtf8, sizeof(cutUtf8), 16, &text));
    const u8 seven[] = {0, 0, 0, 0, 0, 0, 0};
    CHECK(!share::decodeU64(seven, sizeof(seven), &value));
}

TEST(a_world_name_too_long_for_the_server_is_cut_on_a_character)
{
    // 31 ASCII bytes and then a two-byte character straddling the 32-byte
    // ceiling: the character goes, not half of it.
    std::string name(31, 'a');
    name += "\xC3\xA9";
    std::vector<u8> out;
    share::appendUpload(kToken, name, &out);
    const usize length = (usize(out[5 + 16]) << 8) | out[5 + 16 + 1];
    CHECK_EQ(length, usize(31));
}

TEST(a_typed_code_is_cleaned_the_way_the_server_reads_it)
{
    std::string code;
    CHECK(share::cleanCode(" k7qx-2m ", &code));
    CHECK_EQ(code, std::string("K7QX2M"));
    CHECK(!share::cleanCode("", &code));
    CHECK(!share::cleanCode("   ", &code));
    CHECK(!share::cleanCode("ABC/../", &code));
    CHECK(!share::cleanCode(std::string(17, 'A'), &code));
}

// ---- the archive ------------------------------------------------------------

TEST(a_world_packed_and_unpacked_in_any_size_of_piece_is_the_same_world)
{
    TempDir temp;
    CHECK(temp.path[0] != '\0');
    io::PosixFileSystem fs;

    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));
    const std::string source = temp.at("Original");
    makeWorld(fs, source, 424242);
    const std::vector<std::string> before = contents(fs, source);

    // Pieces that split every field somewhere: a byte, a prime, a frame.
    const usize pieces[] = {1, 7, 4093, 16 * 1024};
    int copy = 0;
    for (const usize outPiece : pieces) {
        const std::vector<u8> archive = pack(fs, source, outPiece == 1 ? 13 : outPiece);
        for (const usize inPiece : pieces) {
            char name[16];
            std::snprintf(name, sizeof(name), "Copy%d", copy++);
            std::string error;
            CHECK(unpack(fs, saves, archive, inPiece, name, &error));
            CHECK_EQ(error, std::string());
            CHECK(contents(fs, saves + "/" + name) == before);
            CHECK(!stagingLeft(fs, saves));
        }
    }
    // The original was only read.
    CHECK(contents(fs, source) == before);
}

TEST(an_archive_that_names_a_path_outside_the_world_writes_nothing_anywhere)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    const char* hostile[] = {"../escaped.dat", "/tmp/absolute.dat", "a/../../b.dat",
                             "level.dat/../../x", "C:/x.dat", "a\\b.dat"};
    for (const char* path : hostile) {
        RawArchive raw(1, 4);
        raw.file(path, "evil");
        std::string error;
        CHECK(!unpack(fs, saves, raw.deflated(), 5, "Hostile", &error));
        CHECK(!error.empty());
    }
    CHECK(!fs.exists(temp.at("escaped.dat").c_str()));
    CHECK(!fs.exists((saves + "/escaped.dat").c_str()));
    CHECK(!fs.exists((saves + "/Hostile").c_str()));
    CHECK(!stagingLeft(fs, saves));
}

TEST(an_archive_that_breaks_its_own_promises_is_refused_and_leaves_nothing)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    std::vector<std::vector<u8>> broken;
    // More bytes in its files than its header declared.
    broken.push_back(RawArchive(1, 3).file("level.dat", "four").deflated());
    // A file larger than the limit on one file.
    broken.push_back(
        RawArchive(1, world::kMaxTransferFileBytes + 1)
            .file("level.dat", "", world::kMaxTransferFileBytes + 1)
            .deflated());
    // More files than it said.
    broken.push_back(RawArchive(1, 2).file("a.dat", "a").file("b.dat", "b").deflated());
    // Fewer files than it said.
    broken.push_back(RawArchive(2, 1).file("a.dat", "a").deflated());
    // No files at all, and a count past the ceiling.
    broken.push_back(RawArchive(0, 0).deflated());
    broken.push_back(RawArchive(world::kMaxTransferFiles + 1, 0).deflated());
    // Another version of the archive.
    broken.push_back(RawArchive(1, 1, share::kArchiveVersion + 1).file("a.dat", "a").deflated());
    // Not an archive at all.
    {
        std::vector<u8> junk(64, 0x5A);
        std::vector<u8> out;
        CHECK(zip::compress(ConstByteSpan(junk.data(), junk.size()), out, zip::Wrapper::Zlib));
        broken.push_back(out);
    }
    // Not even zlib.
    broken.push_back(std::vector<u8>(100, 0xFF));

    for (const std::vector<u8>& archive : broken) {
        std::string error;
        CHECK(!unpack(fs, saves, archive, 3, "Broken", &error));
        CHECK(!error.empty());
        CHECK(!fs.exists((saves + "/Broken").c_str()));
        CHECK(!stagingLeft(fs, saves));
    }

    // A whole archive with something after the end of its stream.
    TempDir other;
    const std::string source = other.at("Real");
    makeWorld(fs, source, 7, 20000);
    std::vector<u8> padded = pack(fs, source, 4096);
    padded.push_back(0);
    std::string error;
    CHECK(!unpack(fs, saves, padded, 4096, "Padded", &error));
    CHECK(!fs.exists((saves + "/Padded").c_str()));

    // And one cut short: every byte it had was fine, and it is still not a world.
    std::vector<u8> cut = pack(fs, source, 4096);
    cut.resize(cut.size() / 2);
    CHECK(!unpack(fs, saves, cut, 4096, "Cut", &error));
    CHECK_EQ(error, std::string("The world arrived incomplete."));
    CHECK(!fs.exists((saves + "/Cut").c_str()));
    CHECK(!stagingLeft(fs, saves));
}

TEST(an_archive_of_files_that_are_not_a_world_does_not_get_a_worlds_name)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    std::string error;
    CHECK(!unpack(fs, saves, RawArchive(1, 5).file("notes.txt", "hello").deflated(), 64, "Junk",
                  &error));
    CHECK(!fs.exists((saves + "/Junk").c_str()));
    CHECK(!stagingLeft(fs, saves));
}

// ---- the jobs ---------------------------------------------------------------

TEST(an_upload_shows_the_code_before_the_world_and_ends_shared)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string source = temp.at("Shared World");
    makeWorld(fs, source, 99);

    std::vector<u8> request;
    share::appendOpening(&request);
    share::appendUpload(kToken, "Shared World", &request);

    ScriptedStream stream;
    // Code the moment the request is in; Stored only once End has been.
    stream.answer(request.size(), test::kAcwtCode, sizeof(test::kAcwtCode));

    share::Upload upload(fs, source, "Shared World", kToken);
    CHECK(upload.prepare());
    CHECK(upload.progress().bytesTotal > 0);

    // Stored has to name the total the console sent, which is only known once
    // it has sent it. So: run once to learn the blob, with a server that hangs
    // up after the code, then again with the Stored it would send for exactly
    // that blob.
    stream.closeWhenDrained = true;
    upload.run(stream);
    CHECK(upload.stage() == share::Stage::Failed);  // no Stored in the script
    CHECK_EQ(upload.code(), std::string("K7QX2M"));
    u64 endTotal = 0;
    const std::vector<u8> blob = uploadedBlob(stream.sent, &endTotal);
    CHECK_EQ(endTotal, u64(blob.size()));
    CHECK(std::memcmp(stream.sent.data(), request.data(), request.size()) == 0);

    ScriptedStream again;
    again.answer(request.size(), test::kAcwtCode, sizeof(test::kAcwtCode));
    again.answer(stream.sent.size(), frame(share::Kind::Stored, u64Payload(blob.size())));
    share::Upload second(fs, source, "Shared World", kToken);
    CHECK(second.prepare());
    second.run(again);
    CHECK(second.stage() == share::Stage::Shared);
    CHECK_EQ(second.error(), std::string());
    const share::Progress progress = second.progress();
    CHECK_EQ(progress.bytesDone, progress.bytesTotal);
    CHECK_EQ(progress.filesDone, progress.filesTotal);
    CHECK_EQ(progress.wireBytes, u64(blob.size()));
    // Deterministic: the same world packs to the same bytes.
    CHECK(again.sent == stream.sent);
}

TEST(a_shared_world_downloads_intact_under_the_name_typed_for_it)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string source = temp.at("Alpha");
    makeWorld(fs, source, 31337);
    const std::vector<std::string> before = contents(fs, source);
    const std::vector<u8> blob = pack(fs, source, share::kUploadChunk);

    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    ScriptedStream stream;
    stream.trickle = 5000;
    serveDownload(&stream, blob, "Alpha", downloadRequestBytes("K7QX2M"));

    share::Download download(fs, saves, "From Friend", "K7QX2M", kToken);
    download.run(stream);
    CHECK_EQ(download.error(), std::string());
    CHECK(download.stage() == share::Stage::Done);
    CHECK_EQ(download.worldName(), std::string("Alpha"));

    std::vector<u8> request;
    share::appendOpening(&request);
    share::appendDownload(kToken, "K7QX2M", &request);
    CHECK(stream.sent == request);

    std::vector<world::WorldEntry> listed;
    world::listWorlds(fs, saves, &listed);
    CHECK_EQ(listed.size(), usize(1));
    CHECK_EQ(listed[0].name, std::string("From Friend"));
    CHECK(contents(fs, saves + "/From Friend") == before);
    const share::Progress progress = download.progress();
    CHECK_EQ(progress.bytesDone, progress.bytesTotal);
    CHECK_EQ(progress.wireBytes, u64(blob.size()));
}

TEST(a_wrong_code_is_the_servers_refusal_in_its_own_words_and_touches_no_card)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    ScriptedStream stream;
    stream.answer(downloadRequestBytes("ZZZZZZ"), test::kAcwtError, sizeof(test::kAcwtError));
    stream.closeWhenDrained = true;
    share::Download download(fs, saves, "Nope", "ZZZZZZ", kToken);
    download.run(stream);
    CHECK(download.stage() == share::Stage::Failed);
    CHECK_EQ(download.error(), std::string("No world is being shared with that code."));
    CHECK(!stagingLeft(fs, saves));
    CHECK(!fs.exists((saves + "/Nope").c_str()));
}

TEST(a_download_the_server_ends_part_way_leaves_nothing_behind)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string source = temp.at("Alpha");
    makeWorld(fs, source, 5);
    const std::vector<u8> blob = pack(fs, source, share::kUploadChunk);
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    const usize requestBytes = downloadRequestBytes("K7QX2M");
    ScriptedStream stream;
    stream.trickle = 5000;
    std::vector<u8> found = {0, 1, 'A'};
    stream.answer(requestBytes, frame(share::Kind::Found, found));
    for (usize at = 0; at < blob.size() / 2; at += 16 * 1024) {
        const usize take = blob.size() / 2 - at < 16 * 1024 ? blob.size() / 2 - at : 16 * 1024;
        stream.answer(requestBytes,
                      frame(share::Kind::Data,
                            std::vector<u8>(blob.begin() + std::ptrdiff_t(at),
                                            blob.begin() + std::ptrdiff_t(at + take))));
    }
    const std::string reason = "the world is no longer shared: its owner went offline";
    std::vector<u8> error;
    error.push_back(0);
    error.push_back(u8(reason.size()));
    error.insert(error.end(), reason.begin(), reason.end());
    stream.answer(requestBytes, frame(share::Kind::Error, error));
    stream.closeWhenDrained = true;

    share::Download download(fs, saves, "Half", "K7QX2M", kToken);
    download.run(stream);
    CHECK(download.stage() == share::Stage::Failed);
    CHECK_EQ(download.error(),
             std::string("The world is no longer shared: its owner went offline."));
    CHECK(!stagingLeft(fs, saves));
    CHECK(!fs.exists((saves + "/Half").c_str()));
}

TEST(an_upload_the_server_refuses_part_way_says_why)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string source = temp.at("Big");
    makeWorld(fs, source, 11, 400000);

    std::vector<u8> request;
    share::appendOpening(&request);
    share::appendUpload(kToken, "Big", &request);

    // The server stores 50 KB, then says the world is too large and hangs up.
    ScriptedStream stream;
    stream.answer(request.size(), test::kAcwtCode, sizeof(test::kAcwtCode));
    const std::string reason = "the world is larger than the 0 MiB this server accepts";
    std::vector<u8> error;
    error.push_back(0);
    error.push_back(u8(reason.size()));
    error.insert(error.end(), reason.begin(), reason.end());
    stream.answer(request.size() + 50000, frame(share::Kind::Error, error));
    stream.closeAfterSent = request.size() + 50000;
    stream.closeWhenDrained = true;

    share::Upload upload(fs, source, "Big", kToken);
    upload.run(stream);
    CHECK(upload.stage() == share::Stage::Failed);
    CHECK_EQ(upload.error(),
             std::string("The world is larger than the 0 MiB this server accepts."));
}

TEST(a_cancelled_job_stops_and_says_so)
{
    TempDir temp;
    io::PosixFileSystem fs;
    const std::string saves = temp.at("saves");
    CHECK(fs.makeDirectories(saves.c_str()));

    // A server that never answers; the cancel is raised before the run, which
    // is the earliest a player could press B.
    ScriptedStream stream;
    share::Download download(fs, saves, "Never", "K7QX2M", kToken);
    download.cancel();
    download.run(stream);
    CHECK(download.stage() == share::Stage::Failed);
    CHECK_EQ(download.error(), std::string("Cancelled."));
    CHECK(!stagingLeft(fs, saves));
}

TEST(stopping_a_share_is_one_round_trip)
{
    ScriptedStream stream;
    std::vector<u8> request;
    share::appendOpening(&request);
    share::appendStop(kToken, &request);
    stream.answer(request.size(), test::kAcwtStopped, sizeof(test::kAcwtStopped));
    std::string error;
    CHECK(share::stopSharing(stream, kToken, &error));
    CHECK(stream.sent == request);

    ScriptedStream refused;
    refused.answer(request.size(), test::kAcwtError, sizeof(test::kAcwtError));
    CHECK(!share::stopSharing(refused, kToken, &error));
    CHECK_EQ(error, std::string("No world is being shared with that code."));
}
