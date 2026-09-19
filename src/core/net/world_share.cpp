// A world shared through AlphaComputer. See world_share.hpp.

#include "core/net/world_share.hpp"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace mc::net::share {

namespace {

void put16(std::vector<u8>* out, u16 value)
{
    out->push_back(u8(value >> 8));
    out->push_back(u8(value));
}

void put32(std::vector<u8>* out, u32 value)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out->push_back(u8(value >> shift));
    }
}

void put64(std::vector<u8>* out, u64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(u8(value >> shift));
    }
}

u16 get16(const u8* p)
{
    return u16((u16(p[0]) << 8) | u16(p[1]));
}

u32 get32(const u8* p)
{
    return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | u32(p[3]);
}

u64 get64(const u8* p)
{
    return (u64(get32(p)) << 32) | u64(get32(p + 4));
}

// Cut at `max` bytes without splitting a UTF-8 sequence, which is what the
// server's own writer does -- and what its reader requires, since it refuses a
// string that is not valid UTF-8.
std::string_view clip(std::string_view text, usize max)
{
    usize end = std::min(text.size(), max);
    while (end > 0 && end < text.size() && (u8(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

void putString(std::vector<u8>* out, std::string_view text, usize max)
{
    const std::string_view clipped = clip(text, max);
    put16(out, u16(clipped.size()));
    out->insert(out->end(), clipped.begin(), clipped.end());
}

// Whether `bytes` is well-formed UTF-8. The server's strings always are; this is
// for the one that is shown on screen having come off the network.
bool validUtf8(const u8* bytes, usize size)
{
    usize i = 0;
    while (i < size) {
        const u8 lead = bytes[i];
        usize extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0 && lead >= 0xC2) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4) {
            extra = 3;
        } else {
            return false;
        }
        if (extra > 0 && i + extra >= size) {
            return false;
        }
        for (usize k = 1; k <= extra; ++k) {
            if ((bytes[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

u32 elapsedMs(std::chrono::steady_clock::time_point since)
{
    const auto now = std::chrono::steady_clock::now();
    return u32(std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count());
}

// The slice a blocking wait is cut into, so a cancel is noticed within it.
constexpr int kSliceMs = 100;

// The size of one raw read off the card, and of the inflate's output buffer.
constexpr usize kRawChunk = 16 * 1024;

}  // namespace

// ---- framing --------------------------------------------------------------

void appendOpening(std::vector<u8>* out)
{
    out->insert(out->end(), kMagic, kMagic + sizeof(kMagic));
    put16(out, kProtocol);
}

void appendFrame(Kind kind, const u8* payload, usize size, std::vector<u8>* out)
{
    out->push_back(u8(kind));
    put32(out, u32(size));
    if (size > 0) {
        out->insert(out->end(), payload, payload + size);
    }
}

namespace {

void appendRequest(Kind kind, const std::vector<u8>& body, std::vector<u8>* out)
{
    appendFrame(kind, body.data(), body.size(), out);
}

}  // namespace

void appendUpload(const u8 token[kTokenSize], std::string_view worldName, std::vector<u8>* out)
{
    std::vector<u8> body(token, token + kTokenSize);
    putString(&body, worldName, kMaxWorldName);
    appendRequest(Kind::Upload, body, out);
}

void appendDownload(const u8 token[kTokenSize], std::string_view code, std::vector<u8>* out)
{
    std::vector<u8> body(token, token + kTokenSize);
    putString(&body, code, kMaxCode);
    appendRequest(Kind::Download, body, out);
}

void appendStop(const u8 token[kTokenSize], std::vector<u8>* out)
{
    const std::vector<u8> body(token, token + kTokenSize);
    appendRequest(Kind::Stop, body, out);
}

void appendEnd(u64 total, std::vector<u8>* out)
{
    std::vector<u8> body;
    put64(&body, total);
    appendRequest(Kind::End, body, out);
}

bool decodeString(const u8* payload, usize size, usize max, std::string* out)
{
    if (size < 2) {
        return false;
    }
    const usize length = get16(payload);
    if (length > max || size != 2 + length || !validUtf8(payload + 2, length)) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(payload + 2), length);
    return true;
}

bool decodeU64(const u8* payload, usize size, u64* out)
{
    if (size != 8) {
        return false;
    }
    *out = get64(payload);
    return true;
}

// ---- the archive, writing -------------------------------------------------

struct ArchiveWriter::Deflate {
    z_stream stream = {};
    bool open = false;

    ~Deflate()
    {
        if (open) {
            deflateEnd(&stream);
        }
    }
};

ArchiveWriter::ArchiveWriter(io::FileSystem& fs, std::string worldDir)
    : fs_(fs), worldDir_(std::move(worldDir))
{
}

ArchiveWriter::~ArchiveWriter() = default;

bool ArchiveWriter::begin(std::string* error)
{
    if (!world::buildTransferManifest(fs_, worldDir_, &manifest_)) {
        *error = "That world could not be read, or is too large to share.";
        return false;
    }
    z_ = std::make_unique<Deflate>();
    if (deflateInit(&z_->stream, kArchiveLevel) != Z_OK) {
        *error = "Out of memory to compress the world.";
        return false;
    }
    z_->open = true;
    raw_.reserve(kRawChunk + world::kMaxTransferPath + 16);
    return true;
}

bool ArchiveWriter::refill()
{
    raw_.clear();
    rawUsed_ = 0;

    if (!headerDone_) {
        raw_.insert(raw_.end(), kArchiveMagic, kArchiveMagic + sizeof(kArchiveMagic));
        put16(&raw_, kArchiveVersion);
        put32(&raw_, u32(manifest_.files.size()));
        put64(&raw_, manifest_.totalBytes);
        headerDone_ = true;
        return true;
    }

    // Past zero-length files in one step, so a run of them does not cost a
    // refill each.
    while (fileIndex_ < manifest_.files.size()) {
        const world::TransferFile& entry = manifest_.files[fileIndex_];
        if (!fileHeaderDone_) {
            put16(&raw_, u16(entry.path.size()));
            raw_.insert(raw_.end(), entry.path.begin(), entry.path.end());
            put64(&raw_, entry.size);
            fileHeaderDone_ = true;
            fileOffset_ = 0;
        }
        if (fileOffset_ == entry.size) {
            file_.reset();
            ++fileIndex_;
            ++filesRead_;
            fileHeaderDone_ = false;
            if (raw_.size() >= kRawChunk) {
                return true;
            }
            continue;
        }

        if (file_ == nullptr) {
            const std::string path = worldDir_ + "/" + entry.path;
            file_ = fs_.openRandomAccess(path.c_str(), false);
            if (file_ == nullptr) {
                error_ = "A file in the world could not be opened: " + entry.path;
                return false;
            }
        }
        const usize take = usize(std::min<u64>(kRawChunk, entry.size - fileOffset_));
        const usize at = raw_.size();
        raw_.resize(at + take);
        // **The size is the manifest's, not the file's now.** A file that
        // shrank since the walk fails this read, which is the honest outcome:
        // the receiver was promised that many bytes.
        if (!file_->readAt(fileOffset_, ByteSpan(raw_.data() + at, take))) {
            error_ = "A file in the world could not be read: " + entry.path;
            return false;
        }
        fileOffset_ += take;
        bytesRead_ += take;
        return true;
    }

    inputDone_ = true;
    return true;
}

bool ArchiveWriter::next(std::vector<u8>* out, usize want)
{
    if (finished_ || z_ == nullptr || !z_->open) {
        return error_.empty() && finished_;
    }
    const usize start = out->size();
    out->resize(start + want);
    z_stream& z = z_->stream;
    z.next_out = out->data() + start;
    z.avail_out = uInt(want);

    while (z.avail_out > 0) {
        if (rawUsed_ == raw_.size() && !inputDone_) {
            if (!refill()) {
                out->resize(start);
                return false;
            }
        }
        z.next_in = raw_.data() + rawUsed_;
        z.avail_in = uInt(raw_.size() - rawUsed_);
        const int flush = inputDone_ ? Z_FINISH : Z_NO_FLUSH;
        const int result = deflate(&z, flush);
        rawUsed_ = raw_.size() - z.avail_in;
        if (result == Z_STREAM_END) {
            finished_ = true;
            break;
        }
        if (result != Z_OK && result != Z_BUF_ERROR) {
            error_ = "The world could not be compressed.";
            out->resize(start);
            return false;
        }
    }
    out->resize(start + (want - z.avail_out));
    return true;
}

// ---- the archive, reading -------------------------------------------------

struct ArchiveReader::Inflate {
    z_stream stream = {};
    bool open = false;

    ~Inflate()
    {
        if (open) {
            inflateEnd(&stream);
        }
    }
};

ArchiveReader::ArchiveReader(io::FileSystem& fs, std::string savesDir)
    : fs_(fs), staging_(fs, savesDir)
{
}

ArchiveReader::~ArchiveReader()
{
    discard();
}

bool ArchiveReader::begin(std::string* error)
{
    if (!staging_.begin()) {
        *error = "The card would not take a new folder. Is it full or locked?";
        return false;
    }
    staged_ = true;
    z_ = std::make_unique<Inflate>();
    if (inflateInit(&z_->stream) != Z_OK) {
        *error = "Out of memory to unpack the world.";
        return false;
    }
    z_->open = true;
    out_.resize(kRawChunk);
    return true;
}

bool ArchiveReader::fail(const char* reason)
{
    if (error_.empty()) {
        error_ = reason;
    }
    return false;
}

bool ArchiveReader::feed(const u8* data, usize size)
{
    if (!error_.empty() || z_ == nullptr || !z_->open) {
        return false;
    }
    if (size > 0 && streamEnded_) {
        return fail("The world arrived with something after its end.");
    }
    z_stream& z = z_->stream;
    z.next_in = const_cast<u8*>(data);
    z.avail_in = uInt(size);
    while (z.avail_in > 0 && !streamEnded_) {
        z.next_out = out_.data();
        z.avail_out = uInt(out_.size());
        const int result = inflate(&z, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            return fail("The world arrived damaged.");
        }
        const usize produced = out_.size() - z.avail_out;
        if (produced > 0 && !consume(out_.data(), produced)) {
            return false;
        }
        if (result == Z_STREAM_END) {
            streamEnded_ = true;
        }
    }
    if (z.avail_in > 0) {
        return fail("The world arrived with something after its end.");
    }
    return true;
}

bool ArchiveReader::consume(const u8* data, usize size)
{
    usize at = 0;
    // Gathers `need` bytes of a field into `field_`; true once it is whole.
    const auto gather = [&](usize need) {
        const usize take = std::min(need - fieldUsed_, size - at);
        std::memcpy(field_ + fieldUsed_, data + at, take);
        fieldUsed_ += take;
        at += take;
        return fieldUsed_ == need;
    };

    while (at < size) {
        switch (part_) {
        case Part::Header: {
            constexpr usize kHeader = 4 + 2 + 4 + 8;
            if (!gather(kHeader)) {
                return true;
            }
            fieldUsed_ = 0;
            if (std::memcmp(field_, kArchiveMagic, sizeof(kArchiveMagic)) != 0) {
                return fail("That code is not for a world this build can read.");
            }
            if (get16(field_ + 4) != kArchiveVersion) {
                return fail("That world was shared by a different build of 3DAlpha.");
            }
            const u32 files = get32(field_ + 6);
            const u64 bytes = get64(field_ + 10);
            // **The declared totals are a promise, held to below.** Refused here
            // when they could not describe a world at all.
            if (files == 0 || files > world::kMaxTransferFiles || bytes > world::kMaxTransferBytes) {
                return fail("That world is larger than this console accepts.");
            }
            filesTotal_ = files;
            bytesTotal_ = bytes;
            part_ = Part::PathLength;
            break;
        }
        case Part::PathLength:
            if (!gather(2)) {
                return true;
            }
            fieldUsed_ = 0;
            pathLength_ = get16(field_);
            if (pathLength_ == 0 || pathLength_ > world::kMaxTransferPath) {
                return fail("The world named a file this console will not write.");
            }
            part_ = Part::Path;
            break;
        case Part::Path:
            if (!gather(pathLength_)) {
                return true;
            }
            // The path stays in `field_`; the size is gathered after it.
            part_ = Part::Size;
            break;
        case Part::Size: {
            if (!gather(pathLength_ + 8)) {
                return true;
            }
            fieldUsed_ = 0;
            fileSize_ = get64(field_ + pathLength_);
            fileDone_ = 0;
            declaredSoFar_ += fileSize_;
            if (fileSize_ > world::kMaxTransferFileBytes || declaredSoFar_ > bytesTotal_) {
                return fail("The world is larger than it said it was.");
            }
            const std::string_view path(reinterpret_cast<const char*>(field_), pathLength_);
            // `safeRelativePath` is inside: a path that could climb out of the
            // staging directory is refused before anything is opened.
            if (!staging_.beginFile(path, fileSize_)) {
                return fail("The world named a file this console will not write.");
            }
            part_ = Part::Body;
            break;
        }
        case Part::Body: {
            const usize take = usize(std::min<u64>(fileSize_ - fileDone_, size - at));
            if (take > 0 && !staging_.writeChunk(data + at, take)) {
                return fail("The card would not take the world. Is it full?");
            }
            at += take;
            fileDone_ += take;
            bytesDone_ += take;
            if (fileDone_ < fileSize_) {
                return true;
            }
            if (!staging_.endFile()) {
                return fail("The card would not take the world. Is it full?");
            }
            ++filesDone_;
            if (filesDone_ == filesTotal_) {
                part_ = Part::Done;
            } else {
                part_ = Part::PathLength;
            }
            break;
        }
        case Part::Done:
            return fail("The world arrived with more files than it said it had.");
        }
    }

    // A zero-length file ends the moment its header does, with no body bytes
    // to arrive and drive the switch above.
    if (part_ == Part::Body && fileSize_ == 0) {
        if (!staging_.endFile()) {
            return fail("The card would not take the world. Is it full?");
        }
        ++filesDone_;
        part_ = filesDone_ == filesTotal_ ? Part::Done : Part::PathLength;
    }
    return true;
}

bool ArchiveReader::finish(std::string_view name)
{
    if (!error_.empty()) {
        return false;
    }
    if (!streamEnded_ || part_ != Part::Done || bytesDone_ != bytesTotal_) {
        return fail("The world arrived incomplete.");
    }
    if (!staging_.commit(name)) {
        return fail("What arrived is not a world, or that name has been taken since.");
    }
    staged_ = false;
    return true;
}

void ArchiveReader::discard()
{
    if (staged_) {
        staging_.discard();
        staged_ = false;
    }
}

// ---- the stream -----------------------------------------------------------

bool TcpStream::sendAll(const u8* data, usize size, int stallMs, const std::atomic<bool>& cancel,
                        std::string* error)
{
    usize sent = 0;
    auto lastProgress = std::chrono::steady_clock::now();
    while (sent < size) {
        if (cancel.load(std::memory_order_acquire)) {
            *error = "Cancelled.";
            return false;
        }
        usize n = 0;
        const TcpSocket::Status status = socket_.send(data + sent, size - sent, &n);
        if (status == TcpSocket::Status::Ok && n > 0) {
            sent += n;
            lastProgress = std::chrono::steady_clock::now();
            continue;
        }
        if (status == TcpSocket::Status::Closed) {
            *error = "The server closed the connection.";
            return false;
        }
        if (status == TcpSocket::Status::Error) {
            *error = TcpSocket::lastError("send");
            return false;
        }
        if (int(elapsedMs(lastProgress)) >= stallMs) {
            *error = "The server stopped taking the world.";
            return false;
        }
        bool readable = false;
        bool writable = false;
        if (!socket_.wait(true, kSliceMs, &readable, &writable)) {
            *error = TcpSocket::lastError("poll");
            return false;
        }
    }
    return true;
}

Stream::Status TcpStream::receive(u8* buffer, usize capacity, usize* got, int waitMs)
{
    *got = 0;
    bool readable = false;
    bool writable = false;
    if (!socket_.wait(false, waitMs, &readable, &writable)) {
        return Status::Error;
    }
    if (!readable) {
        return Status::Timeout;
    }
    switch (socket_.receive(buffer, capacity, got)) {
    case TcpSocket::Status::Ok:         return *got > 0 ? Status::Ok : Status::Timeout;
    case TcpSocket::Status::WouldBlock: return Status::Timeout;
    case TcpSocket::Status::Closed:     return Status::Closed;
    case TcpSocket::Status::Error:      return Status::Error;
    }
    return Status::Error;
}

// ---- the jobs -------------------------------------------------------------

const char* describeStage(Stage stage)
{
    switch (stage) {
    case Stage::Connecting: return "Connecting";
    case Stage::Asking:     return "Asking the server";
    case Stage::Sending:    return "Uploading";
    case Stage::Receiving:  return "Downloading";
    case Stage::Finishing:  return "Finishing";
    case Stage::Shared:     return "Shared";
    case Stage::Done:       return "Done";
    case Stage::Stopped:    return "No longer shared";
    case Stage::Failed:     return "Stopped";
    }
    return "";
}

Progress Job::progress() const
{
    Progress out;
    out.stage = stage();
    out.bytesDone = bytesDone_.load(std::memory_order_relaxed);
    out.bytesTotal = bytesTotal_.load(std::memory_order_relaxed);
    out.filesDone = filesDone_.load(std::memory_order_relaxed);
    out.filesTotal = filesTotal_.load(std::memory_order_relaxed);
    out.wireBytes = wireBytes_.load(std::memory_order_relaxed);
    return out;
}

bool Job::finished() const
{
    const Stage now = stage();
    return now == Stage::Shared || now == Stage::Done || now == Stage::Stopped
           || now == Stage::Failed;
}

void Job::fail(const std::string& reason)
{
    if (stage() == Stage::Failed) {
        return;
    }
    error_ = cancelled() ? std::string("Cancelled.") : reason;
    setStage(Stage::Failed);
}

bool Job::readFrame(Stream& stream, int waitMs, Kind* kind, const u8** payload, usize* size,
                    std::string* why)
{
    // **Allocated once, at the ceiling**, before any length has been read: a
    // length can only ever be compared with this, never size it.
    if (in_.size() != kMaxFrame + 5) {
        in_.assign(kMaxFrame + 5, 0);
        inUsed_ = 0;
        inFrameEnd_ = 0;
    }
    // The previous frame is dropped here rather than when it was returned,
    // because its payload pointed into this buffer until now.
    if (inFrameEnd_ > 0) {
        std::memmove(in_.data(), in_.data() + inFrameEnd_, inUsed_ - inFrameEnd_);
        inUsed_ -= inFrameEnd_;
        inFrameEnd_ = 0;
    }

    auto lastByte = std::chrono::steady_clock::now();
    bool closed = false;
    for (;;) {
        if (inUsed_ >= 5) {
            const u32 length = get32(in_.data() + 1);
            if (length > kMaxFrame) {
                *why = "The server sent a frame this build cannot read.";
                return false;
            }
            if (inUsed_ >= 5 + usize(length)) {
                *kind = Kind(in_[0]);
                *payload = in_.data() + 5;
                *size = length;
                inFrameEnd_ = 5 + usize(length);
                return true;
            }
        }
        if (closed) {
            *why = "The server closed the connection.";
            return false;
        }
        if (cancelled()) {
            *why = "Cancelled.";
            return false;
        }
        if (int(elapsedMs(lastByte)) >= waitMs) {
            *why = "The server stopped answering.";
            return false;
        }

        usize got = 0;
        switch (stream.receive(in_.data() + inUsed_, in_.size() - inUsed_, &got, kSliceMs)) {
        case Stream::Status::Ok:
            inUsed_ += got;
            lastByte = std::chrono::steady_clock::now();
            break;
        case Stream::Status::Timeout:
            break;
        case Stream::Status::Closed:
            // What arrived before the close is still read -- a refusal is
            // written and then the connection is shut.
            closed = true;
            break;
        case Stream::Status::Error:
            *why = "The connection to the server failed.";
            return false;
        }
    }
}

bool Job::expectFrame(Stream& stream, int waitMs, Kind* kind, const u8** payload, usize* size)
{
    std::string why;
    if (!readFrame(stream, waitMs, kind, payload, size, &why)) {
        fail(why);
        return false;
    }
    return true;
}

namespace {

// The server's words for a refusal, or ours when it sent something unreadable.
std::string reasonFrom(const u8* payload, usize size)
{
    std::string reason;
    if (!decodeString(payload, size, kMaxReason, &reason) || reason.empty()) {
        return "The server refused.";
    }
    // Its sentences start in lower case, because they are written to follow a
    // colon in its own logs; on a screen they stand alone.
    if (reason[0] >= 'a' && reason[0] <= 'z') {
        reason[0] = char(reason[0] - 'a' + 'A');
    }
    if (reason.back() != '.' && reason.back() != '?' && reason.back() != '!') {
        reason += '.';
    }
    return reason;
}

}  // namespace

Upload::Upload(io::FileSystem& fs, std::string worldDir, std::string worldName,
               const u8 token[kTokenSize])
    : archive_(fs, std::move(worldDir))
{
    worldName_ = std::move(worldName);
    std::memcpy(token_, token, kTokenSize);
}

bool Upload::prepare()
{
    std::string error;
    if (!archive_.begin(&error)) {
        fail(error);
        return false;
    }
    bytesTotal_.store(archive_.bytesTotal(), std::memory_order_relaxed);
    filesTotal_.store(archive_.filesTotal(), std::memory_order_relaxed);
    prepared_ = true;
    return true;
}

void Upload::run(Stream& stream)
{
    if (!prepared_ && !prepare()) {
        return;
    }
    setStage(Stage::Asking);

    std::vector<u8> out;
    appendOpening(&out);
    appendUpload(token_, worldName_, &out);
    std::string error;
    if (!stream.sendAll(out.data(), out.size(), kAnswerMs, cancel_, &error)) {
        fail(error);
        return;
    }

    Kind kind = Kind::Error;
    const u8* payload = nullptr;
    usize size = 0;
    if (!expectFrame(stream, kAnswerMs, &kind, &payload, &size)) {
        return;
    }
    if (kind == Kind::Error) {
        fail(reasonFrom(payload, size));
        return;
    }
    std::string code;
    if (kind != Kind::Code || !decodeString(payload, size, kMaxCode, &code) || code.empty()) {
        fail("The server answered with something this build cannot read.");
        return;
    }
    // **The code is on screen before the first byte goes**: the server made it
    // so, and a player reading it out while the world is still compressing is
    // the reason it did.
    code_ = code;
    setStage(Stage::Sending);

    // A header and a `u32` length in front of each piece, so the payload is
    // written straight into the frame rather than copied into it.
    std::vector<u8> frame;
    frame.reserve(5 + kUploadChunk);
    u64 sent = 0;
    while (!archive_.finished()) {
        if (cancelled()) {
            fail("Cancelled.");
            return;
        }
        frame.assign(5, 0);
        if (!archive_.next(&frame, kUploadChunk)) {
            fail(archive_.error());
            return;
        }
        const usize length = frame.size() - 5;
        bytesDone_.store(archive_.bytesRead(), std::memory_order_relaxed);
        filesDone_.store(archive_.filesRead(), std::memory_order_relaxed);
        if (length == 0) {
            continue;
        }
        frame[0] = u8(Kind::Data);
        frame[1] = u8(length >> 24);
        frame[2] = u8(length >> 16);
        frame[3] = u8(length >> 8);
        frame[4] = u8(length);
        if (!stream.sendAll(frame.data(), frame.size(), kSilenceMs, cancel_, &error)) {
            // **A refusal arrives as a closed connection.** The server writes
            // its reason and hangs up -- over the size limit, or the login
            // ended -- and the send is what notices. The reason is in the
            // socket, if it got there.
            Kind refusal = Kind::Data;
            std::string ignored;
            if (!cancelled() && readFrame(stream, 2000, &refusal, &payload, &size, &ignored)
                && refusal == Kind::Error) {
                fail(reasonFrom(payload, size));
                return;
            }
            fail(error);
            return;
        }
        sent += length;
        wireBytes_.store(sent, std::memory_order_relaxed);
    }

    setStage(Stage::Finishing);
    out.clear();
    appendEnd(sent, &out);
    if (!stream.sendAll(out.data(), out.size(), kAnswerMs, cancel_, &error)) {
        fail(error);
        return;
    }
    if (!expectFrame(stream, kAnswerMs, &kind, &payload, &size)) {
        return;
    }
    if (kind == Kind::Error) {
        fail(reasonFrom(payload, size));
        return;
    }
    u64 stored = 0;
    if (kind != Kind::Stored || !decodeU64(payload, size, &stored) || stored != sent) {
        fail("The server did not keep the whole world.");
        return;
    }
    setStage(Stage::Shared);
}

Download::Download(io::FileSystem& fs, std::string savesDir, std::string targetName,
                   std::string code, const u8 token[kTokenSize])
    : archive_(fs, std::move(savesDir)), targetName_(std::move(targetName)),
      wantedCode_(std::move(code))
{
    std::memcpy(token_, token, kTokenSize);
}

void Download::run(Stream& stream)
{
    setStage(Stage::Asking);

    std::vector<u8> out;
    appendOpening(&out);
    appendDownload(token_, wantedCode_, &out);
    std::string error;
    if (!stream.sendAll(out.data(), out.size(), kAnswerMs, cancel_, &error)) {
        fail(error);
        return;
    }

    Kind kind = Kind::Error;
    const u8* payload = nullptr;
    usize size = 0;
    if (!expectFrame(stream, kAnswerMs, &kind, &payload, &size)) {
        return;
    }
    if (kind == Kind::Error) {
        fail(reasonFrom(payload, size));
        return;
    }
    std::string name;
    if (kind != Kind::Found || !decodeString(payload, size, kMaxWorldName, &name)) {
        fail("The server answered with something this build cannot read.");
        return;
    }

    // **The card only once the server has said the code is good**, so a
    // mistyped code costs a round trip and nothing on the card.
    if (!archive_.begin(&error)) {
        fail(error);
        return;
    }
    worldName_ = name;
    setStage(Stage::Receiving);

    u64 received = 0;
    for (;;) {
        if (!expectFrame(stream, kSilenceMs, &kind, &payload, &size)) {
            archive_.discard();
            return;
        }
        if (kind == Kind::Data) {
            received += size;
            wireBytes_.store(received, std::memory_order_relaxed);
            if (!archive_.feed(payload, size)) {
                fail(archive_.error());
                archive_.discard();
                return;
            }
            bytesDone_.store(archive_.bytesDone(), std::memory_order_relaxed);
            bytesTotal_.store(archive_.bytesTotal(), std::memory_order_relaxed);
            filesDone_.store(archive_.filesDone(), std::memory_order_relaxed);
            filesTotal_.store(archive_.filesTotal(), std::memory_order_relaxed);
            continue;
        }
        if (kind == Kind::End) {
            u64 total = 0;
            if (!decodeU64(payload, size, &total) || total != received) {
                fail("The world arrived incomplete.");
                archive_.discard();
                return;
            }
            break;
        }
        if (kind == Kind::Error) {
            fail(reasonFrom(payload, size));
        } else {
            fail("The server sent something this build cannot read.");
        }
        archive_.discard();
        return;
    }

    setStage(Stage::Finishing);
    if (cancelled()) {
        fail("Cancelled.");
        archive_.discard();
        return;
    }
    if (!archive_.finish(targetName_)) {
        fail(archive_.error());
        archive_.discard();
        return;
    }
    setStage(Stage::Done);
}

bool stopSharing(Stream& stream, const u8 token[kTokenSize], std::string* error)
{
    std::vector<u8> out;
    appendOpening(&out);
    appendStop(token, &out);
    const std::atomic<bool> never{false};
    if (!stream.sendAll(out.data(), out.size(), kAnswerMs, never, error)) {
        return false;
    }
    // A minimal read of the one small answer; `Job::readFrame` is more than a
    // five-byte frame needs.
    u8 answer[5 + 2 + kMaxReason];
    usize have = 0;
    const auto start = std::chrono::steady_clock::now();
    while (int(elapsedMs(start)) < kAnswerMs) {
        usize got = 0;
        const Stream::Status status =
            stream.receive(answer + have, sizeof(answer) - have, &got, kSliceMs);
        have += got;
        if (have >= 5) {
            const u32 length = get32(answer + 1);
            if (Kind(answer[0]) == Kind::Stopped && length == 0) {
                return true;
            }
            if (Kind(answer[0]) == Kind::Error && have >= 5 + usize(length)
                && length <= sizeof(answer) - 5) {
                *error = reasonFrom(answer + 5, length);
                return false;
            }
        }
        if (status == Stream::Status::Closed || status == Stream::Status::Error) {
            break;
        }
    }
    *error = "The server did not confirm the world was withdrawn.";
    return false;
}

bool connectTransfer(TcpSocket& socket, u32 address, u16 port, std::string* error)
{
    if (port == 0) {
        *error = "This server does not share worlds.";
        return false;
    }
    char host[20];
    std::snprintf(host, sizeof(host), "%u.%u.%u.%u", unsigned((address >> 24) & 0xFF),
                  unsigned((address >> 16) & 0xFF), unsigned((address >> 8) & 0xFF),
                  unsigned(address & 0xFF));
    return socket.connect(host, port, kConnectMs, error);
}

bool cleanCode(std::string_view typed, std::string* out)
{
    std::string code;
    for (const char c : typed) {
        if (c == ' ' || c == '-' || c == '\t') {
            continue;
        }
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        const bool digit = c >= '0' && c <= '9';
        if (!letter && !digit) {
            return false;
        }
        code += c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    }
    if (code.empty() || code.size() > kMaxCode) {
        return false;
    }
    *out = code;
    return true;
}

}  // namespace mc::net::share
