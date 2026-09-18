// The parse thread of a local session. See local_channel.hpp.

#include "core/net/local_channel.hpp"

#include "core/util/worker.hpp"

#include <chrono>
#include <cstdio>

namespace mc::net {

namespace {

// How long the thread waits for bytes before looking at `stopRequested_`
// again. Nothing depends on it being short -- `feed` and `stop` both notify --
// so it is only the backstop against a lost wake-up.
constexpr int kIdleWaitMs = 100;

}  // namespace

LocalChannel::~LocalChannel()
{
    stop();
}

bool LocalChannel::start()
{
    stopRequested_ = false;
    closedReported_ = false;
    state_ = State::LoggingIn;

    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformThread_ = workerSpawn()(&LocalChannel::threadEntry, this, WorkerRole::Net);
        if (platformThread_ == nullptr) {
            state_ = State::Closed;
            return false;
        }
        return true;
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void LocalChannel::threadEntry(void* self)
{
    static_cast<LocalChannel*>(self)->run();
}

void LocalChannel::feed(const u8* data, usize size)
{
    if (size == 0) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        inbox_.insert(inbox_.end(), data, data + size);
    }
    bytesIn_ += size;
    wake_.notify_one();
}

void LocalChannel::takeOutbound(std::vector<u8>* out)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (outbox_.empty()) {
        return;
    }
    out->insert(out->end(), outbox_.begin(), outbox_.end());
    bytesOut_ += outbox_.size();
    outbox_.clear();
}

void LocalChannel::send(const Packet& packet)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (stopRequested_) {
        return;
    }
    encodePacket(packet, &outbox_);
}

bool LocalChannel::poll(Event* out)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (events_.empty()) {
        return false;
    }
    *out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void LocalChannel::push(Event&& event)
{
    std::lock_guard<std::mutex> guard(lock_);
    events_.push_back(std::move(event));
}

void LocalChannel::closeWith(const std::string& title, const std::string& detail)
{
    bool report = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        report = !closedReported_;
        closedReported_ = true;
    }
    if (report) {
        Event event;
        event.kind = Event::Kind::Closed;
        event.title = title;
        event.detail = detail;
        push(std::move(event));
    }
    state_ = State::Closed;
    stop();
}

void LocalChannel::stop()
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        stopRequested_ = true;
    }
    wake_.notify_all();
    if (platformThread_ != nullptr) {
        workerJoin()(platformThread_);
        platformThread_ = nullptr;
    } else if (thread_.joinable()) {
        thread_.join();
    }
}

void LocalChannel::run()
{
    for (;;) {
        {
            std::unique_lock<std::mutex> guard(lock_);
            wake_.wait_for(guard, std::chrono::milliseconds(kIdleWaitMs),
                           [this] { return stopRequested_ || !inbox_.empty(); });
            if (stopRequested_) {
                return;
            }
            if (inbox_.empty()) {
                continue;
            }
            // Compact the consumed prefix before the next piece lands on the
            // end, so a session's buffer stays the size of the largest packet
            // in it rather than the size of everything it has ever carried.
            if (inStart_ > 0) {
                in_.erase(in_.begin(), in_.begin() + std::ptrdiff_t(inStart_));
                inStart_ = 0;
            }
            in_.insert(in_.end(), inbox_.begin(), inbox_.end());
            inbox_.clear();
        }

        for (;;) {
            usize consumed = 0;
            const ParseResult result =
                parsePacket(in_.data() + inStart_, in_.size() - inStart_, &parsed_, &consumed);
            if (result == ParseResult::NeedMore) {
                break;
            }
            if (result == ParseResult::UnknownId) {
                char detail[64];
                std::snprintf(detail, sizeof(detail), "Bad packet id %u from the host",
                              unsigned(in_[inStart_]));
                fail("Connection lost", detail);
                return;
            }
            if (result == ParseResult::Malformed) {
                fail("Connection lost", "The other console sent a packet this cannot read.");
                return;
            }
            inStart_ += consumed;
            ++packetsIn_;
            if (!handle(parsed_)) {
                return;
            }
        }
    }
}

void LocalChannel::fail(const char* title, const std::string& detail)
{
    bool report = false;
    {
        std::lock_guard<std::mutex> guard(lock_);
        report = !closedReported_;
        closedReported_ = true;
    }
    if (report) {
        Event event;
        event.kind = Event::Kind::Closed;
        event.title = title;
        event.detail = detail;
        push(std::move(event));
    }
    state_ = State::Closed;
}

bool LocalChannel::handle(const Packet& packet)
{
    switch (packet.id) {
    case packet::KeepAlive:
        // The link has its own keep-alive and its own timeout; this one is
        // only here because the encoder has it.
        return true;

    case packet::Login: {
        state_ = State::Playing;
        Event event;
        event.kind = Event::Kind::LoggedIn;
        event.entityId = i32(packet.integer(0));
        push(std::move(event));
        return true;
    }

    case packet::KickDisconnect:
        fail("Disconnected by the host", packet.text(0));
        return false;

    case packet::MapChunk: {
        Event event;
        if (!inflateMapChunk(packet, &event.region)) {
            fail("Connection lost", "A column from the host could not be read.");
            return false;
        }
        if (event.region.wholeColumn()) {
            event.kind = Event::Kind::Column;
            event.column = buildColumn(event.region);
            event.region.data = std::vector<u8>();
        } else {
            event.kind = Event::Kind::Region;
        }
        push(std::move(event));
        return true;
    }

    default: {
        Event event;
        event.kind = Event::Kind::Packet;
        event.packet = packet;
        push(std::move(event));
        return true;
    }
    }
}

}  // namespace mc::net
