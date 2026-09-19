#pragma once

// A world shared through AlphaComputer: one console uploads it and gets a code,
// and any console that types the code downloads a copy.
//
// **The server never opens what it holds.** AlphaComputer stores the bytes and
// sends them back out (its docs/transfer.md); what they are is decided here, on
// both ends, and checked here on the receiving one. So this file owns two
// formats and they are kept apart on purpose:
//
//   * **The transfer port's framing** -- `ACWT`, a `u16` version, then frames of
//     `u8 kind, u32 length, payload` over TCP. The server's; tests/ac_wire_test
//     checks the bytes against ones the Rust encoder printed.
//   * **The archive** -- what rides inside the `Data` frames. Ours alone: a
//     zlib stream of a header and every file of the world, each one a path, a
//     size and its bytes. The server sees a blob.
//
// **The receiver trusts nothing, the same as a copy over local wireless.** The
// archive arrives off the internet by way of a server that did not look at it,
// so every path goes through `world::safeRelativePath`, every size is held to
// the limits in core/world/world_transfer.hpp, and it all lands in the same
// staging directory `net::copy::Receiver` uses -- a world only once the last
// byte has arrived and `ImportStaging::commit` agrees it is one.
//
// **Format-agnostic, for the reason world_transfer.hpp gives.** A world crosses
// file for file in whichever shape it is in; a shared world is a backup that
// travelled, not a conversion.
//
// **The jobs block.** `Upload::run` and `Download::run` read the card, deflate
// or inflate, and wait on a socket, start to finish, on whatever thread calls
// them -- on the console a worker (platform/ctr/online_share.hpp), in the tests
// and the host harness the caller's own. Everything the screen needs crosses
// back through atomics, and the one thing the screen can do to a job is ask it
// to stop.

#include "core/io/file_system.hpp"
#include "core/net/tcp_socket.hpp"
#include "core/util/types.hpp"
#include "core/world/world_transfer.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mc::net::share {

// ---------------------------------------------------------------------------
// The transfer port.
// ---------------------------------------------------------------------------

// "ACWT", then this. The server's `TRANSFER_PROTOCOL`, which is versioned apart
// from the control port's `ac::kProtocol` -- a change to one does not refuse
// consoles that only use the other.
inline constexpr u8 kMagic[4] = {'A', 'C', 'W', 'T'};
inline constexpr u16 kProtocol = 1;

// The largest payload either end may send; a longer length is refused before
// anything past it is read.
inline constexpr usize kMaxFrame = 64 * 1024;

// What this console puts in one upload `Data` frame. The server sends 16 KiB
// back; the same here keeps one buffer size on both paths.
inline constexpr usize kUploadChunk = 16 * 1024;

inline constexpr usize kTokenSize = 16;
// The server's `MAX_NAME` and `MAX_CODE`. A world name longer than this is cut
// at a character boundary, as the server's own writer would; it is only shown
// on the downloading console, which saves the world under a name of its own.
inline constexpr usize kMaxWorldName = 32;
inline constexpr usize kMaxCode = 16;
inline constexpr usize kMaxReason = 128;

enum class Kind : u8 {
    Upload = 0x01,
    Download = 0x02,
    Stop = 0x03,
    Data = 0x10,
    End = 0x11,
    Code = 0x81,
    Stored = 0x82,
    Found = 0x83,
    Stopped = 0x84,
    Error = 0xFF,
};

// Every connection opens with this, before its one request.
void appendOpening(std::vector<u8>* out);

// One whole frame: kind, length, payload.
void appendFrame(Kind kind, const u8* payload, usize size, std::vector<u8>* out);

void appendUpload(const u8 token[kTokenSize], std::string_view worldName, std::vector<u8>* out);
void appendDownload(const u8 token[kTokenSize], std::string_view code, std::vector<u8>* out);
void appendStop(const u8 token[kTokenSize], std::vector<u8>* out);
void appendEnd(u64 total, std::vector<u8>* out);

// A `u16`-length UTF-8 string that is the whole payload, as `Code`, `Found` and
// `Error` carry. False on anything else, including trailing bytes.
bool decodeString(const u8* payload, usize size, usize max, std::string* out);
// A `u64` that is the whole payload, as `End` and `Stored` carry.
bool decodeU64(const u8* payload, usize size, u64* out);

// ---------------------------------------------------------------------------
// The archive: what the server holds without opening.
// ---------------------------------------------------------------------------
//
//   zlib( "3DAW"  u16 version  u32 files  u64 bytes
//         { u16 pathLength  path  u64 size  [size] bytes }  x files )
//
// Big-endian. The counts come first so the receiving console can draw a bar
// from the first kilobyte, and are checked against what actually arrives.

inline constexpr u8 kArchiveMagic[4] = {'3', 'D', 'A', 'W'};
inline constexpr u16 kArchiveVersion = 1;

// **Level 2, measured rather than assumed.** On the real 1122-file a1.1.2 world
// (3.4 MB of archive, 1121 of the files gzip already): level 0 stores it at
// 1.000 of its size, **level 1 makes it 2.9% larger** -- deflate_fast's short
// matches cost more than they save on data that is compressed already -- level
// 2 makes it 5.9% smaller, and 6 and 9 win 0.1% more for 1.2x and 1.7x level
// 2's time (zlib on a PC, 2026-09-19). Every download pays for the size again,
// so the cheapest level that wins is the one. Not timed on an ARM11.
inline constexpr int kArchiveLevel = 2;

// Reads a world off the card and hands it out deflated, a piece at a time.
class ArchiveWriter {
public:
    ArchiveWriter(io::FileSystem& fs, std::string worldDir);
    ~ArchiveWriter();

    ArchiveWriter(const ArchiveWriter&) = delete;
    ArchiveWriter& operator=(const ArchiveWriter&) = delete;

    // Reads the world's shape. False with `*error` in words a player can act on.
    bool begin(std::string* error);

    // Appends up to `want` bytes of the compressed stream to `out`. False with
    // `error()` set when a file will not read; true and `finished()` once the
    // stream has ended.
    bool next(std::vector<u8>* out, usize want);

    bool finished() const { return finished_; }
    const std::string& error() const { return error_; }

    u64 bytesTotal() const { return manifest_.totalBytes; }
    u32 filesTotal() const { return u32(manifest_.files.size()); }
    u64 bytesRead() const { return bytesRead_; }
    u32 filesRead() const { return filesRead_; }

private:
    // Fills `raw_` with the next piece of the uncompressed archive. False only
    // on a read error; an empty `raw_` afterwards means there is no more.
    bool refill();

    struct Deflate;

    io::FileSystem& fs_;
    std::string worldDir_;
    world::TransferManifest manifest_;
    std::unique_ptr<Deflate> z_;

    std::vector<u8> raw_;
    usize rawUsed_ = 0;
    bool headerDone_ = false;
    usize fileIndex_ = 0;
    bool fileHeaderDone_ = false;
    u64 fileOffset_ = 0;
    std::unique_ptr<io::RandomAccessFile> file_;
    bool inputDone_ = false;
    bool finished_ = false;

    u64 bytesRead_ = 0;
    u32 filesRead_ = 0;
    std::string error_;
};

// Takes the compressed stream as it arrives and writes the world it describes
// into the import staging directory under `savesDir`.
class ArchiveReader {
public:
    ArchiveReader(io::FileSystem& fs, std::string savesDir);
    ~ArchiveReader();

    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;

    // Clears the staging directory and makes it again. False when the card
    // refuses.
    bool begin(std::string* error);

    // The next piece of the stream, of any size. False with `error()` set the
    // moment the stream stops being a world this console will accept.
    bool feed(const u8* data, usize size);

    // The stream has ended. True when it was whole -- every file, every byte,
    // nothing after -- and the world is now `savesDir`/`name`.
    bool finish(std::string_view name);

    // Throws away whatever arrived. Safe to call at any point, and twice.
    void discard();

    const std::string& error() const { return error_; }

    // Zero until the header has arrived.
    u64 bytesTotal() const { return bytesTotal_; }
    u32 filesTotal() const { return filesTotal_; }
    u64 bytesDone() const { return bytesDone_; }
    u32 filesDone() const { return filesDone_; }

private:
    bool consume(const u8* data, usize size);
    bool fail(const char* reason);

    struct Inflate;

    enum class Part : u8 { Header, PathLength, Path, Size, Body, Done };

    io::FileSystem& fs_;
    world::ImportStaging staging_;
    std::unique_ptr<Inflate> z_;
    bool staged_ = false;
    bool streamEnded_ = false;

    Part part_ = Part::Header;
    // A field that is split across two pieces of the stream is collected here.
    u8 field_[world::kMaxTransferPath + 18] = {};
    usize fieldUsed_ = 0;
    usize pathLength_ = 0;
    u64 fileSize_ = 0;
    u64 fileDone_ = 0;
    u64 declaredSoFar_ = 0;

    std::vector<u8> out_;

    u64 bytesTotal_ = 0;
    u32 filesTotal_ = 0;
    u64 bytesDone_ = 0;
    u32 filesDone_ = 0;
    std::string error_;
};

// ---------------------------------------------------------------------------
// The byte stream the jobs run over.
// ---------------------------------------------------------------------------

// A connected, ordered, reliable byte stream: a TCP socket on the console and
// the harness, and a scripted one in the tests.
class Stream {
public:
    enum class Status { Ok, Timeout, Closed, Error };

    virtual ~Stream() = default;

    // Everything, or false with `*error` set. Gives up when the far end takes
    // nothing for `stallMs`, and when `cancel` is raised.
    virtual bool sendAll(const u8* data, usize size, int stallMs,
                         const std::atomic<bool>& cancel, std::string* error) = 0;

    // Some bytes, waiting up to `waitMs` for the first of them.
    virtual Status receive(u8* buffer, usize capacity, usize* got, int waitMs) = 0;
};

// A `TcpSocket` as a `Stream`.
class TcpStream : public Stream {
public:
    explicit TcpStream(TcpSocket& socket) : socket_(socket) {}

    bool sendAll(const u8* data, usize size, int stallMs, const std::atomic<bool>& cancel,
                 std::string* error) override;
    Status receive(u8* buffer, usize capacity, usize* got, int waitMs) override;

private:
    TcpSocket& socket_;
};

// ---------------------------------------------------------------------------
// The jobs.
// ---------------------------------------------------------------------------

// Where a job has got to, in the order it gets there.
enum class Stage : u8 {
    Connecting,
    // The request is out; the server has not answered it.
    Asking,
    // Bytes are crossing.
    Sending,
    Receiving,
    // Upload: everything is out and the server has not said it has it all.
    // Download: every byte is in and the world is being checked and named.
    Finishing,
    // Upload only: the server has the whole world and is handing it to anyone
    // with the code, for as long as this console's login lasts.
    Shared,
    // Download only: the world is on this card.
    Done,
    // Upload only: the share was withdrawn.
    Stopped,
    Failed,
};

const char* describeStage(Stage stage);

// What a screen draws, read off the job's atomics in one go.
struct Progress {
    Stage stage = Stage::Connecting;
    u64 bytesDone = 0;
    u64 bytesTotal = 0;
    u32 filesDone = 0;
    u32 filesTotal = 0;
    // Compressed bytes that have crossed the socket.
    u64 wireBytes = 0;
};

// How long either end may go without a byte before the other is given up on.
// Longer than the server's own 30-second stall timeout, so that a download
// waiting on a slow upload is ended by the server, with a reason, rather than
// by this console with none.
inline constexpr int kSilenceMs = 45000;
// How long a request may go unanswered.
inline constexpr int kAnswerMs = 15000;
inline constexpr int kConnectMs = 8000;

// State both jobs share: the progress, the strings a screen shows, and the
// flag that stops one.
class Job {
public:
    Progress progress() const;
    Stage stage() const { return Stage(stage_.load(std::memory_order_acquire)); }
    bool finished() const;

    // Only read once `stage()` says it is there: the code from `Sending` on, the
    // world name from `Receiving` on, and the reason once `Failed`.
    const std::string& code() const { return code_; }
    const std::string& worldName() const { return worldName_; }
    const std::string& error() const { return error_; }

    // Asks the job to stop at its next chance. Safe from any thread.
    void cancel() { cancel_.store(true, std::memory_order_release); }

    // Ends a job that never got to run, with the reason: a connection that
    // could not be made. From the thread that would have run it.
    void abandon(const std::string& reason) { fail(reason); }
    bool cancelled() const { return cancel_.load(std::memory_order_acquire); }

protected:
    void setStage(Stage stage) { stage_.store(u8(stage), std::memory_order_release); }
    void fail(const std::string& reason);

    // One frame off `stream`, waiting up to `waitMs` for it to begin and for as
    // long as bytes keep coming after that. The payload points into this job's
    // buffer and lasts until the next read. False with `*why` set.
    bool readFrame(Stream& stream, int waitMs, Kind* kind, const u8** payload, usize* size,
                   std::string* why);
    // `readFrame`, failing the job when there is no frame.
    bool expectFrame(Stream& stream, int waitMs, Kind* kind, const u8** payload, usize* size);

    std::atomic<u8> stage_{u8(Stage::Connecting)};
    std::atomic<bool> cancel_{false};
    std::atomic<u64> bytesDone_{0};
    std::atomic<u64> bytesTotal_{0};
    std::atomic<u32> filesDone_{0};
    std::atomic<u32> filesTotal_{0};
    std::atomic<u64> wireBytes_{0};

    std::string code_;
    std::string worldName_;
    std::string error_;

    std::vector<u8> in_;
    usize inUsed_ = 0;
    usize inFrameEnd_ = 0;
};

class Upload : public Job {
public:
    Upload(io::FileSystem& fs, std::string worldDir, std::string worldName,
           const u8 token[kTokenSize]);

    // Reads the world's shape before anything touches the network, so a world
    // that cannot be read is refused in front of the player rather than after a
    // connection. False with `error()` set.
    bool prepare();

    // The rest, start to finish: the request, the code, every byte, and the
    // server's word that it has them. Ends `Shared`, `Stopped` or `Failed`.
    void run(Stream& stream);

private:
    ArchiveWriter archive_;
    u8 token_[kTokenSize] = {};
    bool prepared_ = false;
};

class Download : public Job {
public:
    // `targetName` is already sanitised and already known to be free, for the
    // reason `net::copy::Receiver` gives: it was typed on a keyboard, which
    // suspends the console, before anything was connected.
    Download(io::FileSystem& fs, std::string savesDir, std::string targetName, std::string code,
             const u8 token[kTokenSize]);

    // The request, the world, and the commit. Ends `Done` or `Failed`, and a
    // failure leaves nothing on the card.
    void run(Stream& stream);

private:
    ArchiveReader archive_;
    std::string targetName_;
    std::string wantedCode_;
    u8 token_[kTokenSize] = {};
};

// Withdraws whatever this login is sharing. One round trip; false with
// `*error` set when it did not happen, which costs little -- the server drops a
// share within a minute of its owner's login ending anyway.
bool stopSharing(Stream& stream, const u8 token[kTokenSize], std::string* error);

// Connects a `TcpSocket` to the transfer port, for the three callers that need
// one: the console's worker, the host harness and nothing else.
bool connectTransfer(TcpSocket& socket, u32 address, u16 port, std::string* error);

// Trims a typed code and refuses one that could not be a code at all, before
// a connection is spent finding that out.
bool cleanCode(std::string_view typed, std::string* out);

}  // namespace mc::net::share
