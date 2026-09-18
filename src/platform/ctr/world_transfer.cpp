// One console handing a world to another. See world_transfer.hpp.

#include "platform/ctr/world_transfer.hpp"

#include <3ds.h>

namespace mc::ctr {

namespace {

// The guest's node id on a network with one guest. The host is
// `net::link::kHostNode`; this is the only other console a transfer ever has.
constexpr u16 kOtherNode = 2;

}  // namespace

WorldTransfer::WorldTransfer(io::FileSystem& fs) : fs_(fs) {}

WorldTransfer::~WorldTransfer()
{
    stop("the transfer was left");
}

bool WorldTransfer::offer(const std::string& worldDir, const std::string& worldName,
                          const std::string& hostName, std::string* error)
{
    // **The card before the radio.** A world that cannot be read is a refusal
    // the player should get in front of them, not a beacon somebody else walks
    // over to and finds empty.
    auto sender = std::make_unique<net::copy::Sender>(fs_, worldDir, worldName);
    if (!sender->begin(error)) {
        return false;
    }
    if (!startLocalWireless(error)) {
        return false;
    }
    if (!link_.host(worldName, hostName, LocalKind::WorldOffer, error)) {
        return false;
    }

    sender_ = std::move(sender);
    worldName_ = worldName;
    peerNode_ = kOtherNode;
    lastPumpMs_ = u32(osGetTime());
    peer_.reset(lastPumpMs_);
    return true;
}

bool WorldTransfer::accept(const LocalSession& session, const std::string& savesDir,
                           const std::string& targetName, std::string* error)
{
    if (!startLocalWireless(error)) {
        return false;
    }
    if (!link_.join(session, error)) {
        return false;
    }

    receiver_ = std::make_unique<net::copy::Receiver>(fs_, savesDir, targetName);
    worldName_ = session.worldName;
    peerNode_ = net::link::kHostNode;
    lastPumpMs_ = u32(osGetTime());
    peer_.reset(lastPumpMs_);
    return true;
}

void WorldTransfer::onMessage(void* context, net::link::Msg kind, const u8* body, usize size)
{
    auto* self = static_cast<WorldTransfer*>(context);
    if (kind == net::link::Msg::Away) {
        // The other console is about to be suspended and said so. The same
        // grace a session gives, for the same reason: a library applet stops a
        // 3DS outright and that is indistinguishable from a walk out of range.
        if (size >= 2) {
            const u32 ms = u32(body[0]) << 8 | u32(body[1]);
            self->peer_.grantAway(u32(osGetTime()), ms);
        }
        return;
    }
    if (self->sender_ != nullptr) {
        self->sender_->onMessage(kind, body, size);
    } else if (self->receiver_ != nullptr) {
        self->receiver_->onMessage(kind, body, size);
    }
}

bool WorldTransfer::pump()
{
    if (!link_.active() || (sender_ == nullptr && receiver_ == nullptr)) {
        return false;
    }

    const u32 nowMs = u32(osGetTime());
    // **Time this console never saw.** The home menu and the system's own
    // applets stop the application; a gap that long is not the other console
    // having gone quiet and must not be counted against it.
    const u32 gap = nowMs - lastPumpMs_;
    if (gap > net::link::kStallMs) {
        peer_.forgive(gap);
    }
    lastPumpMs_ = nowMs;

    const net::copy::Progress before = progress();

    u8 datagram[net::link::kMaxDatagram];
    usize size = 0;
    u16 node = 0;
    while (link_.receive(datagram, sizeof(datagram), &size, &node)) {
        heard_ = true;
        // The node a transfer talks to is fixed the moment the link comes up:
        // a host has one guest and a guest has one host, so a frame from
        // anywhere else is not part of this conversation.
        if (node != peerNode_) {
            continue;
        }
        if (!peer_.receive(datagram, size, nowMs, &WorldTransfer::onMessage, this)) {
            stop("the other console sent a frame this build could not read");
            return true;
        }
    }

    if (sender_ != nullptr) {
        sender_->pump(peer_);
    } else {
        receiver_->pump(peer_);
    }
    // A send that fails is a console that is not there yet -- an export sits on
    // the air before anybody has connected -- so it is not an error here. The
    // silence timeout below is what notices a link that has really gone.
    peer_.flush(nowMs, link_, peerNode_);

    if (heard_ && !finished() && peer_.timedOut(nowMs)) {
        stop("the other console stopped answering");
        return true;
    }

    // A transfer that stopped on its own -- a refusal, a full card, a file that
    // would not read -- carries its reason in the half that noticed. Picked up
    // here so the screen has one whether the end came from this console or the
    // other.
    if (finished() && !succeeded() && reason_.empty()) {
        reason_ = sender_ != nullptr ? sender_->error() : receiver_->error();
    }

    // A finished transfer still has a last message to get out -- the receiver's
    // verdict, or a refusal -- so the link is left up for the screen to close.
    const net::copy::Progress after = progress();
    return after.stage != before.stage || after.bytesDone != before.bytesDone
           || after.filesDone != before.filesDone;
}

bool WorldTransfer::finished() const
{
    if (sender_ != nullptr) {
        return sender_->finished();
    }
    if (receiver_ != nullptr) {
        return receiver_->finished();
    }
    return true;
}

bool WorldTransfer::succeeded() const
{
    if (sender_ != nullptr) {
        return sender_->stage() == net::copy::Stage::Done;
    }
    if (receiver_ != nullptr) {
        return receiver_->stage() == net::copy::Stage::Done;
    }
    return false;
}

net::copy::Progress WorldTransfer::progress() const
{
    if (sender_ != nullptr) {
        return sender_->progress();
    }
    if (receiver_ != nullptr) {
        return receiver_->progress();
    }
    return net::copy::Progress();
}

void WorldTransfer::stop(const char* why)
{
    if (stopped_) {
        return;
    }
    stopped_ = true;

    // **One last pump and flush either way**, and not only when this end is
    // cutting the transfer short: a verdict queued on the frame the player
    // pressed A -- the receiver's "saved", most of all -- is still in the
    // window, and leaving without it makes the far console sit through a
    // ten-second silence for a transfer that worked. Best effort: if the link
    // has already gone there is nothing to send it through.
    if (sender_ != nullptr) {
        if (!sender_->finished()) {
            sender_->abandon(why);
        }
        sender_->pump(peer_);
        peer_.flush(u32(osGetTime()), link_, peerNode_);
        if (sender_->stage() != net::copy::Stage::Done) {
            reason_ = sender_->error();
        }
    } else if (receiver_ != nullptr) {
        if (!receiver_->finished()) {
            receiver_->abandon(why);
        }
        receiver_->pump(peer_);
        peer_.flush(u32(osGetTime()), link_, peerNode_);
        if (receiver_->stage() != net::copy::Stage::Done) {
            reason_ = receiver_->error();
        }
    }
    if (reason_.empty() && !succeeded()) {
        reason_ = why;
    }
    link_.leave();
}

}  // namespace mc::ctr
