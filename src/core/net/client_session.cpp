// The session thread: connect, log in, parse, inflate, hand over. See
// client_session.hpp.

#include "core/net/client_session.hpp"

#include "core/util/worker.hpp"
#include "version_config.hpp"

#include <chrono>

namespace mc::net {

namespace {

constexpr int kConnectTimeoutMs = 10000;
constexpr int kPollMs = 50;
constexpr u64 kKeepAliveMs = 1000;
// `ii`: 1200 ticks without a packet read is "Timed out". That is the right
// bound once play has started, and much too patient before it: a server that
// takes the connection and then says nothing is answered in a minute, when the
// thing that is wrong -- it is not a protocol-2 server -- could be said at once.
constexpr u64 kReadTimeoutMs = 60000;
constexpr u64 kLoginTimeoutMs = 20000;
constexpr u64 kFlushOnStopMs = 500;
constexpr usize kReadChunk = 16u << 10;

u64 steadyMillis()
{
    return u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

}  // namespace

bool ClientSession::start(const std::string& host, u16 port, const std::string& username)
{
    host_ = host;
    port_ = port;
    username_ = username;
    stopRequested_ = false;
    closedReported_ = false;
    state_ = State::Connecting;

    if (workerSpawn() != nullptr && workerJoin() != nullptr) {
        platformThread_ = workerSpawn()(&ClientSession::threadEntry, this, WorkerRole::Net);
        if (platformThread_ == nullptr) {
            state_ = State::Closed;
            return false;
        }
        return true;
    }
    thread_ = std::thread([this] { run(); });
    return true;
}

void ClientSession::threadEntry(void* self)
{
    static_cast<ClientSession*>(self)->run();
}

void ClientSession::send(const Packet& packet)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (stopRequested_) {
        return;
    }
    encodePacket(packet, &outbox_);
}

bool ClientSession::poll(Event* out)
{
    std::lock_guard<std::mutex> guard(lock_);
    if (events_.empty()) {
        return false;
    }
    *out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void ClientSession::stop(const char* reason)
{
    {
        std::lock_guard<std::mutex> guard(lock_);
        if (!stopRequested_) {
            stopRequested_ = true;
            stopReason_ = reason != nullptr ? reason : "Quitting";
        }
    }
    if (platformThread_ != nullptr) {
        workerJoin()(platformThread_);
        platformThread_ = nullptr;
    } else if (thread_.joinable()) {
        thread_.join();
    }
}

void ClientSession::push(Event&& event)
{
    std::lock_guard<std::mutex> guard(lock_);
    events_.push_back(std::move(event));
}

void ClientSession::fail(const char* title, const std::string& detail)
{
    if (!closedReported_) {
        closedReported_ = true;
        Event event;
        event.kind = Event::Kind::Closed;
        event.title = title;
        event.detail = detail;
        push(std::move(event));
    }
    socket_.close();
    state_ = State::Closed;
}

void ClientSession::run()
{
    std::string error;
    if (!socket_.connect(host_.c_str(), port_, kConnectTimeoutMs, &error)) {
        fail("Failed to connect to the server", error);
        return;
    }

    state_ = State::LoggingIn;
    send(makeHandshake(username_));

    const u64 startedMs = steadyMillis();
    u64 lastReceive = startedMs;
    u64 lastSend = startedMs;

    while (socket_.isOpen()) {
        bool stopping = false;
        std::string reason;
        {
            std::lock_guard<std::mutex> guard(lock_);
            stopping = stopRequested_;
            reason = stopReason_;
        }
        if (stopping) {
            // `gs.k()` sends the Disconnect; the flush is bounded so a server
            // that has stopped reading cannot hold the game's exit up.
            {
                std::lock_guard<std::mutex> guard(lock_);
                if (state_ == State::Playing || state_ == State::LoggingIn) {
                    encodePacket(makeDisconnect(reason), &outbox_);
                }
            }
            const u64 until = steadyMillis() + kFlushOnStopMs;
            bool sent = false;
            while (steadyMillis() < until) {
                if (!pumpSend(&sent)) break;
                std::lock_guard<std::mutex> guard(lock_);
                if (outbox_.empty() && writeOffset_ >= writing_.size()) break;
            }
            socket_.close();
            state_ = State::Closed;
            return;
        }

        bool wantWrite = writeOffset_ < writing_.size();
        if (!wantWrite) {
            std::lock_guard<std::mutex> guard(lock_);
            wantWrite = !outbox_.empty();
        }
        // **`poll` is how this waits, not how it decides to read.** It is asked
        // for a slice of sleep so the thread is not spinning; then the socket is
        // read whatever it said. A non-blocking `recv` with nothing waiting
        // costs one syscall and answers `WouldBlock`, which is the same answer
        // `poll` would have given -- and a `poll` that fails to report readable
        // on a socket that *is* readable would otherwise look exactly like a
        // server gone quiet, which is not a mistake worth being able to make on
        // a platform whose socket service this code cannot test against.
        bool readable = false;
        bool writable = false;
        socket_.wait(wantWrite, kPollMs, &readable, &writable);

        const u64 now = steadyMillis();
        usize received = 0;
        if (!pumpReceive(&received)) {
            return;
        }
        if (received > 0) {
            lastReceive = now;
        } else if (state_ == State::Playing) {
            if (now - lastReceive > kReadTimeoutMs) {
                fail("Connection lost",
                     "The server sent nothing for a minute. It may have stopped, or the "
                     "console may have lost the network.");
                return;
            }
        } else if (now - startedMs > kLoginTimeoutMs) {
            // **Which half of the login never happened**, because the two mean
            // very different things to whoever is running the server.
            fail("Failed to login",
                 bytesIn_.load() == 0
                     ? "The server took the connection and then said nothing. That is what a "
                       "server speaking a later protocol does -- this needs Alpha 1.1.2's, "
                       "which is server 0.2.1."
                     : "The server answered the handshake but never sent a login.");
            return;
        }

        if (state_ == State::Playing && now - lastSend >= kKeepAliveMs) {
            std::lock_guard<std::mutex> guard(lock_);
            if (outbox_.empty()) {
                encodePacket(makeKeepAlive(), &outbox_);
            }
        }

        bool sentAnything = false;
        if (!pumpSend(&sentAnything)) {
            return;
        }
        if (sentAnything) {
            lastSend = now;
        }
    }
}

bool ClientSession::pumpReceive(usize* received)
{
    *received = 0;
    // Compact once the consumed prefix is most of the buffer, so a long
    // session does not grow it by what it has already parsed.
    if (inStart_ > 0 && inStart_ * 2 >= in_.size()) {
        in_.erase(in_.begin(), in_.begin() + std::ptrdiff_t(inStart_));
        inStart_ = 0;
    }

    const usize before = in_.size();
    in_.resize(before + kReadChunk);
    const TcpSocket::Status status = socket_.receive(in_.data() + before, kReadChunk, received);
    in_.resize(before + *received);

    if (status == TcpSocket::Status::Closed) {
        fail("Connection lost", "End of stream");
        return false;
    }
    if (status == TcpSocket::Status::Error) {
        fail("Connection lost", socket_.lastError("recv"));
        return false;
    }
    bytesIn_ += *received;
    if (*received == 0) {
        return true;  // nothing new; whatever is buffered was parsed last time
    }

    for (;;) {
        usize consumed = 0;
        const ParseResult result =
            parsePacket(in_.data() + inStart_, in_.size() - inStart_, &parsed_, &consumed);
        if (result == ParseResult::NeedMore) {
            break;
        }
        if (result == ParseResult::UnknownId) {
            char detail[48];
            std::snprintf(detail, sizeof(detail), "Bad packet id %u", unsigned(in_[inStart_]));
            fail("Connection lost", detail);
            return false;
        }
        if (result == ParseResult::Malformed) {
            fail("Connection lost", "Internal exception: malformed packet");
            return false;
        }
        inStart_ += consumed;
        ++packetsIn_;
        if (!handle(parsed_)) {
            return false;
        }
    }
    return true;
}

bool ClientSession::pumpSend(bool* sentAnything)
{
    *sentAnything = false;
    if (writeOffset_ >= writing_.size()) {
        writing_.clear();
        writeOffset_ = 0;
        std::lock_guard<std::mutex> guard(lock_);
        writing_.swap(outbox_);
    }
    while (writeOffset_ < writing_.size()) {
        usize sent = 0;
        const TcpSocket::Status status =
            socket_.send(writing_.data() + writeOffset_, writing_.size() - writeOffset_, &sent);
        if (status == TcpSocket::Status::WouldBlock) {
            break;
        }
        if (status != TcpSocket::Status::Ok) {
            fail("Connection lost", socket_.lastError("send"));
            return false;
        }
        writeOffset_ += sent;
        bytesOut_ += sent;
        *sentAnything = *sentAnything || sent > 0;
    }
    return true;
}

bool ClientSession::handle(const Packet& packet)
{
    switch (packet.id) {
    case packet::KeepAlive:
        return true;

    case packet::Handshake:
        // `gy.a(gt)`: "-" is an offline server. Anything else is a connection
        // hash to be verified with minecraft.net, whose session service for
        // this era no longer exists.
        if (packet.text(0) == "-") {
            send(makeLogin(username_, mcver::kProtocol));
            return true;
        }
        fail("Failed to login", "The server is in online mode, which needs Mojang's "
                                "long-gone session servers. Set online-mode=false.");
        return false;

    case packet::Login: {
        state_ = State::Playing;
        Event event;
        event.kind = Event::Kind::LoggedIn;
        event.entityId = i32(packet.integer(0));
        push(std::move(event));
        return true;
    }

    case packet::KickDisconnect:
        fail("Disconnected by server", packet.text(0));
        return false;

    case packet::MapChunk: {
        Event event;
        if (!inflateMapChunk(packet, &event.region)) {
            fail("Connection lost", "Internal exception: Bad compressed data format");
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
