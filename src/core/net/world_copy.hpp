#pragma once

// One world crossing a link, from the console that has it to one that wants a
// copy of it.
//
// **This is not a session.** Nothing ticks, nobody joins, and neither console
// is playing: one end opens a link, offers a world, streams its files and hangs
// up. That is why it is a module of its own rather than a message inside
// core/net/session.hpp -- a transfer has no player, no world rules and no
// entity list, and the only thing it shares with a session is the link it runs
// over.
//
// **The payload carries no sequence numbers**, and that is not an oversight.
// `link::Peer` already delivers the reliable half in the order it was queued,
// so `WorldFile` followed by n `WorldData` messages followed by the next
// `WorldFile` arrives in exactly that order or not at all. Adding offsets on
// top of it would be re-implementing the thing underneath.
//
// **The exchange.**
//
//   sender                               receiver
//   -- WorldOffer (name, files, bytes) ->
//                                        <- WorldAnswer (yes / why not)
//   -- WorldFile (path, size) ---------->
//   -- WorldData (bytes) --------------->   (repeated until the file is whole)
//   -- ... every file, sorted by path ...
//   -- WorldResult (done) -------------->
//                                        <- WorldResult (ok / why not)
//
// Either end may send `WorldResult` with a reason at any point, which is how a
// refusal, a full card and a cancelled transfer all reach the other console as
// something it can put on screen rather than as a silence.
//
// **The receiver trusts nothing.** Every path is checked before it is opened
// (world::safeRelativePath), every size is checked against the limits in
// core/world/world_transfer.hpp, and everything lands in a staging directory
// that is not a world until the last file has arrived.

#include "core/io/file_system.hpp"
#include "core/net/link.hpp"
#include "core/util/types.hpp"
#include "core/world/world_transfer.hpp"

#include <memory>
#include <string>
#include <vector>

namespace mc::net::copy {

// Bumped when a body below changes shape. Separate from `link::kProtocol`
// because the two are versioned by different things: a transfer between two
// builds that agree about *this* is fine even if their game messages have
// moved on.
inline constexpr u16 kProtocol = 1;

// How much of one file rides in one message. `link::kMaxMessage` is the whole
// datagram's worth; the margin leaves room for the message header the peer puts
// in front of it and keeps the arithmetic in one place.
inline constexpr usize kDataBytes = link::kMaxMessage - 16;

// **What one end may queue ahead of the other's acknowledgements.** The window
// is 32 datagrams, so filling it and stopping is what turns a burst into a
// stream -- see `Peer::stalled`. Kept below the window so a keep-alive and an
// answer always have a slot.
inline constexpr int kInFlightLimit = link::kWindow - 4;

// The sender's opening message.
struct Offer {
    u16 protocol = kProtocol;
    std::string worldName;  // what the world is called on the sending console
    u32 files = 0;
    u64 bytes = 0;
};

// The receiver's answer. A refusal carries a reason because the player on the
// far console is the one who has to act on it.
struct Answer {
    bool accepted = false;
    std::string reason;
};

// A file is starting. Everything after it, until the next one, belongs to it.
struct FileHeader {
    std::string path;  // relative, '/' separated, world::safeRelativePath
    u64 size = 0;
};

// Both ends use it: the sender to say "that was all", the receiver to say
// whether what arrived was a world. `ok` false always carries a reason.
struct Result {
    bool ok = false;
    std::string reason;
};

void encode(const Offer& value, std::vector<u8>* out);
void encode(const Answer& value, std::vector<u8>* out);
void encode(const FileHeader& value, std::vector<u8>* out);
void encode(const Result& value, std::vector<u8>* out);

bool decode(const u8* body, usize size, Offer* out);
bool decode(const u8* body, usize size, Answer* out);
bool decode(const u8* body, usize size, FileHeader* out);
bool decode(const u8* body, usize size, Result* out);

// Where both ends are, for the screen that is drawn over them.
enum class Stage {
    Offering,   // sender: the offer is out; receiver: waiting for one
    Waiting,    // sender: the offer is out and unanswered
    Copying,    // files are crossing
    Finishing,  // everything is sent; waiting for the far end's verdict
    Done,
    Failed,
};

const char* describeStage(Stage stage);

// What a progress bar needs, from either end.
struct Progress {
    Stage stage = Stage::Offering;
    u64 bytesDone = 0;
    u64 bytesTotal = 0;
    u32 filesDone = 0;
    u32 filesTotal = 0;
};

// ---------------------------------------------------------------------------

// **The console that has the world.** Reads it off the card a message at a time
// and hands it to the peer as fast as the window will take it.
class Sender {
public:
    Sender(io::FileSystem& fs, std::string worldDir, std::string worldName);
    ~Sender();

    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    // Reads the world's shape off the card. False with `*error` in words a
    // player can act on -- which for a world that cannot be listed is that it
    // could not be read, not a path.
    bool begin(std::string* error);

    // Queues as much as `peer` will take without overrunning its window. Called
    // once a frame, before the peer is flushed.
    void pump(link::Peer& peer);

    // One message off the link.
    void onMessage(link::Msg kind, const u8* body, usize size);

    // The link died, or the player pressed B. Neither is an error the far end
    // will be told about -- there is nothing left to tell it through.
    void abandon(const char* reason);

    Progress progress() const;
    Stage stage() const { return stage_; }
    bool finished() const { return stage_ == Stage::Done || stage_ == Stage::Failed; }
    const std::string& error() const { return error_; }

private:
    void fail(const char* reason);
    // Fills `buffer_` with the next slice of the current file, opening the next
    // file when this one runs out. False when there is nothing left to send.
    bool nextSlice(usize* size);

    io::FileSystem& fs_;
    std::string worldDir_;
    std::string worldName_;

    world::TransferManifest manifest_;
    usize fileIndex_ = 0;
    u64 fileOffset_ = 0;
    bool headerSent_ = false;
    std::unique_ptr<io::RandomAccessFile> file_;

    std::vector<u8> buffer_;
    std::vector<u8> body_;

    Stage stage_ = Stage::Offering;
    u64 bytesSent_ = 0;
    // A failure the far end has not been told about yet. See `Sender::fail`.
    bool sendFailure_ = false;
    std::string error_;
};

// **The console that wants the copy.** Answers an offer, writes what arrives
// into a staging directory, and commits it under the name the player chose.
class Receiver {
public:
    // `targetName` is already sanitised and already known not to be taken --
    // the menu asks for it on a keyboard before the radio is touched, because
    // a keyboard suspends the console and a suspended console is one the link
    // has to be told about in advance.
    Receiver(io::FileSystem& fs, std::string savesDir, std::string targetName);
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    void pump(link::Peer& peer);
    void onMessage(link::Msg kind, const u8* body, usize size);

    // Throws away whatever arrived. The original on the other console is
    // untouched by definition -- nothing here ever writes to it.
    void abandon(const char* reason);

    Progress progress() const;
    Stage stage() const { return stage_; }
    bool finished() const { return stage_ == Stage::Done || stage_ == Stage::Failed; }
    const std::string& error() const { return error_; }

    // What the sending console calls the world, once the offer has arrived.
    const std::string& sourceName() const { return sourceName_; }

private:
    void refuse(const char* reason);
    void fail(const char* reason);

    io::FileSystem& fs_;
    std::string savesDir_;
    std::string targetName_;
    std::string sourceName_;

    world::ImportStaging staging_;
    bool staged_ = false;
    bool fileOpen_ = false;

    std::vector<u8> body_;
    std::vector<u8> pendingOut_;
    bool sendAnswer_ = false;
    Answer answer_;
    bool sendResult_ = false;
    Result result_;

    Stage stage_ = Stage::Offering;
    u64 bytesTotal_ = 0;
    u32 filesTotal_ = 0;
    std::string error_;
};

}  // namespace mc::net::copy
