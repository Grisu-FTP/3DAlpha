// The console's end of AlphaComputer. See online.hpp.

#include "platform/ctr/online.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/net/tcp_socket.hpp"
#include "core/util/worker.hpp"
#include "platform/ctr/network.hpp"

#include <3ds.h>

#include <atomic>
#include <cstring>

namespace mc::ctr {

// **The only blocking call in this file, and it is on a thread of its own.**
// `resolveHostAddress` walks four resolvers, the last of which waits three
// seconds on the console's own name servers. On the menu thread that would be a
// screen that has stopped; here it is a line that says "Looking up".
struct Online::Lookup {
    std::string host;
    std::atomic<int> done{0};  // 0 running, 1 answered, 2 failed
    u32 address = 0;
    std::string error;
};

// **How long to wait for the console to have an address.** An association and
// a DHCP lease take a second or two from cold; ten is generous and still short
// enough that a player with the wireless switched off is told so rather than
// left watching a screen that never changes.
constexpr u32 kAddressWaitMs = 10000;

constexpr const char* kNoAddress =
    "This console has no network address. Check the wireless switch.";

Online::Online() = default;

Online::~Online()
{
    stop();
}

void Online::resolveEntry(void* arg)
{
    auto* lookup = static_cast<Lookup*>(arg);
    u32 address = 0;
    std::string error;
    if (net::resolveHostAddress(lookup->host.c_str(), &address, &error)) {
        lookup->address = address;
        lookup->done.store(1, std::memory_order_release);
    } else {
        lookup->error = error;
        lookup->done.store(2, std::memory_order_release);
    }
}

void Online::fail(const std::string& reason)
{
    stage_ = Stage::Failed;
    message_ = reason;
}

void Online::setHosting(bool hosting)
{
    hosting_ = hosting;
}

void Online::start(const std::string& serverUrl, bool hosting)
{
    hosting_ = hosting;
    if (stage_ != Stage::Off && serverUrl == serverUrl_ && stage_ != Stage::Failed) {
        return;
    }
    stop();

    serverUrl_ = serverUrl;
    message_.clear();

    net::ac::ServerAddress address;
    if (!net::ac::parseServerUrl(serverUrl, &address)) {
        fail("That server address cannot be read.");
        return;
    }
    host_ = address.host;
    port_ = address.port;

    std::string error;
    if (!startNetwork(&error)) {
        fail(error);
        return;
    }

    // The identity key: read from the card, or made once and written there.
    // **Before the socket**, because a console with no identity has nothing to
    // say and there is no point opening a port for it.
    io::PosixFileSystem fs;
    if (!net::ac::loadOrCreateIdentity(fs, net::ac::kIdentityKeyPath, principalId(),
                                       &randomBytes, nullptr, &identity_, &error)) {
        fail(error);
        return;
    }

    // **No address, no lookup.** Every resolver needs the console to be on a
    // network, and the usual reason it is not is that local wireless has just
    // handed the radio back and the access point is a few seconds from taking
    // it again. A lookup started now would fail and blame the name.
    if (localAddress() == 0) {
        lookupPending_ = true;
        stage_ = Stage::WaitingForAddress;
        waitingSinceMs_ = u32(osGetTime());
        message_ = "Waiting for the network...";
        return;
    }
    beginLookup();
}

void Online::beginLookup()
{
    lookupPending_ = false;
    lookup_ = std::make_unique<Lookup>();
    lookup_->host = host_;
    stage_ = Stage::Resolving;
    message_ = "Looking up " + host_ + "...";

    const WorkerSpawn spawn = workerSpawn();
    if (spawn != nullptr) {
        lookupThread_ = spawn(&Online::resolveEntry, lookup_.get(), WorkerRole::Net);
    }
    if (lookupThread_ == nullptr) {
        // No thread to be had. Doing it inline is slow rather than broken, and
        // is what the host build and the tests run on.
        resolveEntry(lookup_.get());
    }
}

void Online::finishLookup()
{
    const int done = lookup_->done.load(std::memory_order_acquire);
    if (done == 0) {
        return;
    }
    if (lookupThread_ != nullptr) {
        const WorkerJoin join = workerJoin();
        if (join != nullptr) {
            join(lookupThread_);
        }
        lookupThread_ = nullptr;
    }

    const bool answered = done == 1;
    serverAddress_ = lookup_->address;
    const std::string error = lookup_->error;
    lookup_.reset();

    if (!answered) {
        fail(error);
        return;
    }

    stage_ = Stage::WaitingForAddress;
    waitingSinceMs_ = u32(osGetTime());
    message_ = "Waiting for the network...";
}

bool Online::openSocket(u32 nowMs)
{
    // **A datagram socket has to bind this console's own address**, because
    // `SOCU:Bind` refuses the wildcard -- libctru's own sockets example binds
    // `gethostid()` and that is not decoration. Zero means the console has not
    // joined a network yet, which is a reason to wait rather than to refuse:
    // the address arrives with the association and with DHCP.
    const u32 bindAddress = localAddress();
    if (bindAddress == 0) {
        if (nowMs - waitingSinceMs_ > kAddressWaitMs) {
            fail(kNoAddress);
        }
        return false;
    }

    const net::ac::Endpoint server = net::ac::endpointV4(serverAddress_, port_);
    std::string socketError;
    if (!socket_.open(server, bindAddress, &socketError)) {
        fail(socketError);
        return false;
    }

    net::ac::Login login;
    login.identity = identity_.name;
    std::memcpy(login.seed, identity_.seed, sizeof(login.seed));
    std::memcpy(login.publicKey, identity_.publicKey, sizeof(login.publicKey));
    // The friend-list name, which is what the server calls this console until
    // it is linked to an account. Absent is ordinary and the server has its own
    // answer for it, so nothing is invented here.
    login.platformName = friendScreenName();
    login.hasPlatformName = !login.platformName.empty();

    stage_ = Stage::Connecting;
    message_ = "Connecting to " + host_ + "...";
    connection_.begin(server, login, &socket_, hosting_, nowMs);
    return true;
}

void Online::pump(u32 nowMs)
{
    switch (stage_) {
    case Stage::Off:
    case Stage::Failed:
        return;
    case Stage::Resolving:
        finishLookup();
        return;
    case Stage::WaitingForAddress:
        if (!lookupPending_) {
            openSocket(nowMs);
        } else if (localAddress() != 0) {
            beginLookup();
        } else if (nowMs - waitingSinceMs_ > kAddressWaitMs) {
            fail(kNoAddress);
        }
        return;
    case Stage::Connecting:
    case Stage::Ready:
        break;
    }

    connection_.pump(nowMs);

    switch (connection_.client().state()) {
    case net::ac::State::Ready:
        if (stage_ != Stage::Ready) {
            stage_ = Stage::Ready;
            message_.clear();
        }
        break;
    case net::ac::State::Failed:
        fail(connection_.client().message());
        break;
    default:
        break;
    }
}

net::ac::Event Online::takeEvent()
{
    const net::ac::Event event = connection_.takeEvent();
    if (event == net::ac::Event::Refused && !connection_.client().message().empty()) {
        message_ = connection_.client().message();
    }
    if (!connection_.message().empty()) {
        message_ = connection_.message();
    }
    return event;
}

void Online::leave()
{
    stop();
}

// **Logged in, not "has a peer".** A host whose world is open and whose guests
// have not arrived yet still has a session to close and a directory entry to
// withdraw; answering false there would mean nobody ever told the server.
bool Online::active() const
{
    return stage_ == Stage::Ready;
}

// The game loop's one frame of network. Everything the menu's `pump` does, and
// the events with it -- a punch landing mid-game is how a second guest arrives
// at a world that is already running.
void Online::service(u32 nowMs)
{
    pump(nowMs);
    while (takeEvent() != net::ac::Event::None) {
    }
}

u32 Online::sendOverflows() const
{
    return connection_.overflows();
}

bool Online::send(u16 node, const u8* data, usize size)
{
    return connection_.send(node, data, size);
}

bool Online::receive(u8* buffer, usize capacity, usize* size, u16* node)
{
    return connection_.receive(buffer, capacity, size, node);
}

void Online::stop()
{
    if (lookupThread_ != nullptr) {
        // The worker is blocked in a resolver that cannot be interrupted, so
        // this waits for it. It is bounded -- the DNS step gives up after three
        // seconds -- and it only happens if a player leaves the screen inside
        // that window.
        const WorkerJoin join = workerJoin();
        if (join != nullptr) {
            join(lookupThread_);
        }
        lookupThread_ = nullptr;
    }
    lookup_.reset();
    lookupPending_ = false;

    if (stage_ == Stage::Connecting || stage_ == Stage::Ready) {
        connection_.end();
    }
    serverAddress_ = 0;
    socket_.close();
    stage_ = Stage::Off;
    message_.clear();
}

}  // namespace mc::ctr
