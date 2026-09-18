#include "core/net/world_copy.hpp"

#include "core/net/wire.hpp"
#include "core/world/world_list.hpp"

#include <algorithm>

namespace mc::net::copy {

namespace {

// Long enough for a card that is full and short enough for the bottom screen.
// Every refusal in this file is written to fit it.
constexpr usize kMaxReason = 120;

void putReason(ByteWriter& writer, const std::string& reason)
{
    writer.putString(reason.size() > kMaxReason ? reason.substr(0, kMaxReason) : reason);
}

bool getReason(ByteReader& reader, std::string* out)
{
    bool malformed = false;
    if (!reader.getString(out, &malformed) || malformed) {
        return false;
    }
    if (out->size() > kMaxReason) {
        out->resize(kMaxReason);
    }
    return true;
}

}  // namespace

const char* describeStage(Stage stage)
{
    switch (stage) {
    case Stage::Offering:  return "Connecting";
    case Stage::Waiting:   return "Waiting for the other console";
    case Stage::Copying:   return "Copying";
    case Stage::Finishing: return "Finishing";
    case Stage::Done:      return "Done";
    case Stage::Failed:    return "Stopped";
    }
    return "";
}

// ---- bodies ---------------------------------------------------------------

void encode(const Offer& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putI16(i16(value.protocol));
    writer.putString(value.worldName);
    writer.putI32(i32(value.files));
    writer.putI64(i64(value.bytes));
}

bool decode(const u8* body, usize size, Offer* out)
{
    ByteReader reader(body, size);
    i16 protocol = 0;
    std::string name;
    bool malformed = false;
    i32 files = 0;
    i64 bytes = 0;
    if (!reader.getI16(&protocol) || !reader.getString(&name, &malformed) || malformed
        || !reader.getI32(&files) || !reader.getI64(&bytes)) {
        return false;
    }
    // Negative counts are what a field written by something else looks like;
    // they are refused here rather than turned into enormous unsigned ones.
    if (files < 0 || bytes < 0) {
        return false;
    }
    out->protocol = u16(protocol);
    out->worldName = name;
    out->files = u32(files);
    out->bytes = u64(bytes);
    return true;
}

void encode(const Answer& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putU8(value.accepted ? 1 : 0);
    putReason(writer, value.reason);
}

bool decode(const u8* body, usize size, Answer* out)
{
    ByteReader reader(body, size);
    u8 accepted = 0;
    std::string reason;
    if (!reader.getU8(&accepted) || !getReason(reader, &reason)) {
        return false;
    }
    out->accepted = accepted != 0;
    out->reason = reason;
    return true;
}

void encode(const FileHeader& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putString(value.path);
    writer.putI64(i64(value.size));
}

bool decode(const u8* body, usize size, FileHeader* out)
{
    ByteReader reader(body, size);
    std::string path;
    bool malformed = false;
    i64 bytes = 0;
    if (!reader.getString(&path, &malformed) || malformed || !reader.getI64(&bytes)) {
        return false;
    }
    if (bytes < 0) {
        return false;
    }
    out->path = path;
    out->size = u64(bytes);
    return true;
}

void encode(const Result& value, std::vector<u8>* out)
{
    ByteWriter writer(out);
    writer.putU8(value.ok ? 1 : 0);
    putReason(writer, value.reason);
}

bool decode(const u8* body, usize size, Result* out)
{
    ByteReader reader(body, size);
    u8 ok = 0;
    std::string reason;
    if (!reader.getU8(&ok) || !getReason(reader, &reason)) {
        return false;
    }
    out->ok = ok != 0;
    out->reason = reason;
    return true;
}

// ---- sender ---------------------------------------------------------------

Sender::Sender(io::FileSystem& fs, std::string worldDir, std::string worldName)
    : fs_(fs), worldDir_(std::move(worldDir)), worldName_(std::move(worldName))
{
    buffer_.resize(kDataBytes);
    body_.reserve(link::kMaxMessage);
}

Sender::~Sender() = default;

bool Sender::begin(std::string* error)
{
    if (!world::buildTransferManifest(fs_, worldDir_, &manifest_)) {
        *error = "that world could not be read off the card";
        stage_ = Stage::Failed;
        error_ = *error;
        return false;
    }
    stage_ = Stage::Offering;
    return true;
}

void Sender::fail(const char* reason)
{
    if (stage_ == Stage::Done || stage_ == Stage::Failed) {
        return;
    }
    stage_ = Stage::Failed;
    error_ = reason;
    file_.reset();
    // Told rather than left to time out: the console at the other end is
    // watching a progress bar, and ten seconds of nothing is the worst way to
    // learn that a transfer has stopped.
    sendFailure_ = true;
}

void Sender::abandon(const char* reason)
{
    fail(reason);
}

// **The current file only.** Stepping to the next one is `pump`'s job, because
// the next one needs its header on the wire before any of its bytes, and a
// reader that crossed the boundary on its own would send the second file's
// contents under the first file's name.
bool Sender::nextSlice(usize* size)
{
    const world::TransferFile& entry = manifest_.files[fileIndex_];
    if (file_ == nullptr) {
        const std::string full = worldDir_ + "/" + entry.path;
        file_ = fs_.openRandomAccess(full.c_str(), false);
        if (file_ == nullptr) {
            return false;
        }
    }
    const u64 left = entry.size - fileOffset_;
    const usize take = usize(std::min<u64>(left, u64(kDataBytes)));
    if (!file_->readAt(fileOffset_, ByteSpan(buffer_.data(), take))) {
        return false;
    }
    fileOffset_ += u64(take);
    *size = take;
    return true;
}

void Sender::pump(link::Peer& peer)
{
    if (sendFailure_) {
        Result stopped;
        stopped.ok = false;
        stopped.reason = error_;
        body_.clear();
        encode(stopped, &body_);
        if (peer.queue(link::Msg::WorldResult, body_, true)) {
            sendFailure_ = false;
        }
        return;
    }
    if (stage_ == Stage::Done || stage_ == Stage::Failed) {
        return;
    }

    if (stage_ == Stage::Offering) {
        Offer offer;
        offer.worldName = worldName_;
        offer.files = u32(manifest_.files.size());
        offer.bytes = manifest_.totalBytes;
        body_.clear();
        encode(offer, &body_);
        if (!peer.queue(link::Msg::WorldOffer, body_, true)) {
            return;  // the window is busy; try again next frame
        }
        stage_ = Stage::Waiting;
        return;
    }

    if (stage_ != Stage::Copying) {
        return;
    }

    // **Stop at the window rather than at the end of the world.** Everything
    // here is reliable, so queueing the whole world would mean holding all of
    // it in the retransmit buffer; the link is the flow control and this is
    // where it is read.
    while (peer.inFlight() < kInFlightLimit && !peer.stalled()) {
        if (fileIndex_ >= manifest_.files.size()) {
            Result done;
            done.ok = true;
            body_.clear();
            encode(done, &body_);
            if (!peer.queue(link::Msg::WorldResult, body_, true)) {
                return;
            }
            stage_ = Stage::Finishing;
            return;
        }

        const world::TransferFile& entry = manifest_.files[fileIndex_];

        if (!headerSent_) {
            FileHeader header;
            header.path = entry.path;
            header.size = entry.size;
            body_.clear();
            encode(header, &body_);
            if (!peer.queue(link::Msg::WorldFile, body_, true)) {
                return;
            }
            headerSent_ = true;
            // A zero-byte file is whole the moment its header is out, and the
            // next turn of the loop is what steps past it.
            continue;
        }

        if (fileOffset_ >= entry.size) {
            file_.reset();
            ++fileIndex_;
            fileOffset_ = 0;
            headerSent_ = false;
            continue;
        }

        usize size = 0;
        if (!nextSlice(&size)) {
            fail("a file went missing while the world was being sent");
            return;
        }
        if (!peer.queue(link::Msg::WorldData, buffer_.data(), size, true)) {
            // The window filled between the check and the queue. Put the bytes
            // back rather than dropping them.
            fileOffset_ -= u64(size);
            return;
        }
        bytesSent_ += u64(size);
    }
}

void Sender::onMessage(link::Msg kind, const u8* body, usize size)
{
    if (stage_ == Stage::Done || stage_ == Stage::Failed) {
        return;
    }

    if (kind == link::Msg::WorldAnswer) {
        Answer answer;
        if (!decode(body, size, &answer)) {
            fail("the other console sent something this build cannot read");
            return;
        }
        if (!answer.accepted) {
            fail(answer.reason.empty() ? "the other console refused the world"
                                       : answer.reason.c_str());
            return;
        }
        if (stage_ == Stage::Waiting) {
            stage_ = Stage::Copying;
        }
        return;
    }

    if (kind == link::Msg::WorldResult) {
        Result result;
        if (!decode(body, size, &result)) {
            fail("the other console sent something this build cannot read");
            return;
        }
        if (!result.ok) {
            fail(result.reason.empty() ? "the other console could not save the world"
                                       : result.reason.c_str());
            return;
        }
        // "Saved" before "that was all" is the other console answering a
        // question nobody asked, and is not a reason to call a half-sent world
        // sent.
        if (stage_ != Stage::Finishing) {
            fail("the other console answered before the world was sent");
            return;
        }
        stage_ = Stage::Done;
        file_.reset();
    }
}

Progress Sender::progress() const
{
    Progress out;
    out.stage = stage_;
    out.bytesDone = bytesSent_;
    out.bytesTotal = manifest_.totalBytes;
    out.filesDone = u32(fileIndex_);
    out.filesTotal = u32(manifest_.files.size());
    return out;
}

// ---- receiver -------------------------------------------------------------

Receiver::Receiver(io::FileSystem& fs, std::string savesDir, std::string targetName)
    : fs_(fs), savesDir_(std::move(savesDir)), targetName_(std::move(targetName)),
      staging_(fs, savesDir_)
{
    body_.reserve(link::kMaxMessage);
}

Receiver::~Receiver()
{
    // A receiver that goes away mid-transfer takes its half-world with it. The
    // original is on the other console and was never touched.
    if (staged_ && stage_ != Stage::Done) {
        staging_.discard();
    }
}

void Receiver::refuse(const char* reason)
{
    answer_.accepted = false;
    answer_.reason = reason;
    sendAnswer_ = true;
    fail(reason);
}

void Receiver::fail(const char* reason)
{
    if (stage_ == Stage::Done || stage_ == Stage::Failed) {
        return;
    }
    // A refusal before the world was accepted travels in the Answer, which is
    // already queued; anything after it needs a Result of its own.
    if (!sendAnswer_) {
        result_.ok = false;
        result_.reason = reason;
        sendResult_ = true;
    }
    stage_ = Stage::Failed;
    error_ = reason;
    if (staged_) {
        staging_.discard();
        staged_ = false;
    }
    fileOpen_ = false;
}

void Receiver::abandon(const char* reason)
{
    fail(reason);
}

void Receiver::pump(link::Peer& peer)
{
    // Answers and verdicts are queued here rather than in `onMessage`, because
    // the window can be full at the moment a message arrives and a refusal that
    // was dropped is a console left staring at a progress bar.
    if (sendAnswer_) {
        body_.clear();
        encode(answer_, &body_);
        if (peer.queue(link::Msg::WorldAnswer, body_, true)) {
            sendAnswer_ = false;
        }
    }
    if (sendResult_ && !sendAnswer_) {
        body_.clear();
        encode(result_, &body_);
        if (peer.queue(link::Msg::WorldResult, body_, true)) {
            sendResult_ = false;
        }
    }
}

void Receiver::onMessage(link::Msg kind, const u8* body, usize size)
{
    if (stage_ == Stage::Done || stage_ == Stage::Failed) {
        return;
    }

    switch (kind) {
    case link::Msg::WorldOffer: {
        if (stage_ != Stage::Offering) {
            fail("the other console offered a second world");
            return;
        }
        Offer offer;
        if (!decode(body, size, &offer)) {
            refuse("that console sent something this build cannot read");
            return;
        }
        if (offer.protocol != kProtocol) {
            refuse("the other console is on a different build of 3DAlpha");
            return;
        }
        if (offer.files == 0 || offer.files > world::kMaxTransferFiles
            || offer.bytes > world::kMaxTransferBytes) {
            refuse("that world is not a shape this build can take");
            return;
        }
        if (fs_.exists(world::worldPath(savesDir_, targetName_).c_str())) {
            refuse("a world of that name is already on this card");
            return;
        }
        if (!staging_.begin()) {
            refuse("this card would not make room for the world");
            return;
        }
        staged_ = true;
        sourceName_ = offer.worldName;
        filesTotal_ = offer.files;
        bytesTotal_ = offer.bytes;
        answer_.accepted = true;
        answer_.reason.clear();
        sendAnswer_ = true;
        stage_ = Stage::Copying;
        return;
    }

    case link::Msg::WorldFile: {
        if (stage_ != Stage::Copying) {
            fail("a file arrived before the world was offered");
            return;
        }
        if (fileOpen_ && !staging_.endFile()) {
            fail("a file arrived shorter than it said it would be");
            return;
        }
        fileOpen_ = false;

        FileHeader header;
        if (!decode(body, size, &header)) {
            fail("that console sent something this build cannot read");
            return;
        }
        // `beginFile` re-checks the path itself; it is the layer that owns the
        // rule, and the check that matters is the one next to the write.
        if (!staging_.beginFile(header.path, header.size)) {
            fail("that world names a file this console will not write");
            return;
        }
        fileOpen_ = true;
        return;
    }

    case link::Msg::WorldData: {
        if (!fileOpen_) {
            fail("file data arrived with no file open");
            return;
        }
        if (!staging_.writeChunk(body, size)) {
            fail("the world would not fit on this card");
            return;
        }
        return;
    }

    case link::Msg::WorldResult: {
        Result sent;
        if (!decode(body, size, &sent)) {
            fail("that console sent something this build cannot read");
            return;
        }
        if (!sent.ok) {
            fail(sent.reason.empty() ? "the other console stopped sending"
                                     : sent.reason.c_str());
            return;
        }
        if (stage_ != Stage::Copying) {
            fail("the transfer ended before it started");
            return;
        }
        if (fileOpen_ && !staging_.endFile()) {
            fail("the last file arrived shorter than it said it would be");
            return;
        }
        fileOpen_ = false;
        if (staging_.filesWritten() != filesTotal_) {
            fail("the world arrived with files missing");
            return;
        }
        if (!staging_.commit(targetName_)) {
            fail("what arrived was not a world this build can open");
            return;
        }
        staged_ = false;
        stage_ = Stage::Done;
        result_.ok = true;
        result_.reason.clear();
        sendResult_ = true;
        return;
    }

    default:
        return;
    }
}

Progress Receiver::progress() const
{
    Progress out;
    out.stage = stage_;
    out.bytesDone = staging_.bytesWritten();
    out.bytesTotal = bytesTotal_;
    out.filesDone = staging_.filesWritten();
    out.filesTotal = filesTotal_;
    return out;
}

}  // namespace mc::net::copy
