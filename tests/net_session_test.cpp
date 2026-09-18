// The session thread against a fake server on a loopback socket: the handshake
// and offline login, a column inflated off the main thread, keep-alives while
// the game says nothing, and a kick. The real 0.2.1 server is the host
// harness's job (`--join`); this is the part that has to run under TSan.

#include "framework.hpp"

#include "core/net/chunk_payload.hpp"
#include "core/net/client_session.hpp"
#include "core/net/packets.hpp"
#include "core/world/chunk.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

struct FakeServer {
    int listener = -1;
    int client = -1;
    u16 port = 0;
    std::vector<u8> in;

    bool listen()
    {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listener, 1) != 0) return false;
        socklen_t length = sizeof(addr);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &length);
        port = ntohs(addr.sin_port);
        return true;
    }

    bool accept()
    {
        client = ::accept(listener, nullptr, nullptr);
        return client >= 0;
    }

    void send(const Packet& p)
    {
        std::vector<u8> bytes;
        encodePacket(p, &bytes);
        ::send(client, bytes.data(), bytes.size(), 0);
    }

    // Blocks until one whole packet has arrived.
    bool receive(Packet* out)
    {
        for (;;) {
            usize consumed = 0;
            if (parsePacket(in.data(), in.size(), out, &consumed) == ParseResult::Ok) {
                in.erase(in.begin(), in.begin() + std::ptrdiff_t(consumed));
                return true;
            }
            u8 buffer[4096];
            const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
            if (n <= 0) return false;
            in.insert(in.end(), buffer, buffer + n);
        }
    }

    ~FakeServer()
    {
        if (client >= 0) ::close(client);
        if (listener >= 0) ::close(listener);
    }
};

const world::ChunkColumn* stoneColumn(void*, i32 cx, i32 cz)
{
    static world::ChunkColumn column;
    column.x = cx;
    column.z = cz;
    for (int x = 0; x < 16; ++x)
        for (int z = 0; z < 16; ++z)
            for (int y = 0; y < 40; ++y) column.setBlock(x, y, z, 1);
    return &column;
}

bool waitFor(ClientSession& session, ClientSession::Event::Kind kind,
             ClientSession::Event* out)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        while (session.poll(out)) {
            if (out->kind == kind) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

}  // namespace

TEST(a_session_logs_in_offline_hands_over_a_column_and_reports_a_kick)
{
    FakeServer server;
    CHECK(server.listen());

    // **By name, not by number**, so the resolver's `gethostbyname` path is the
    // one under test here; the other cases go through `inet_aton`.
    ClientSession session;
    CHECK(session.start("localhost", server.port, "Grisu"));
    CHECK(server.accept());

    Packet p;
    CHECK(server.receive(&p));
    CHECK_EQ(p.id, packet::Handshake);
    CHECK(p.text(0) == "Grisu");
    server.send(makeHandshake("-"));

    CHECK(server.receive(&p));
    CHECK_EQ(p.id, packet::Login);
    CHECK_EQ(p.integer(0), i64(2));
    CHECK(p.text(0) == "Grisu");

    Packet login;
    login.reset(packet::Login);
    login.pushInt(42);
    login.pushString("");
    login.pushString("");
    server.send(login);

    Packet chunk;
    CHECK(makeMapChunk(&stoneColumn, nullptr, 32, 0, -16, 16, world::ChunkColumn::kHeight, 16,
                       &chunk));
    server.send(chunk);

    ClientSession::Event event;
    CHECK(waitFor(session, ClientSession::Event::Kind::LoggedIn, &event));
    CHECK_EQ(event.entityId, i32(42));
    CHECK(waitFor(session, ClientSession::Event::Kind::Column, &event));
    CHECK(event.column != nullptr);
    CHECK_EQ(event.column->x, i32(2));
    CHECK_EQ(event.column->z, i32(-1));
    CHECK_EQ(event.column->block(3, 39, 3), u16(1));
    CHECK_EQ(event.column->block(3, 40, 3), u16(0));

    // The game sends nothing for a while -- the pause menu -- and the thread
    // keeps the server's read timeout from firing.
    session.send(makeChat("hello"));
    CHECK(server.receive(&p));
    CHECK_EQ(p.id, packet::Chat);
    CHECK(server.receive(&p));
    CHECK_EQ(p.id, packet::KeepAlive);

    server.send(makeDisconnect("The server is full!"));
    CHECK(waitFor(session, ClientSession::Event::Kind::Closed, &event));
    CHECK(event.title == "Disconnected by server");
    CHECK(event.detail == "The server is full!");
    CHECK(session.state() == ClientSession::State::Closed);
    session.stop("Quitting");
}

TEST(a_session_to_nowhere_says_it_could_not_connect)
{
    FakeServer server;
    CHECK(server.listen());
    const u16 port = server.port;
    ::close(server.listener);  // nothing listens there now
    server.listener = -1;

    ClientSession session;
    CHECK(session.start("127.0.0.1", port, "Grisu"));
    ClientSession::Event event;
    CHECK(waitFor(session, ClientSession::Event::Kind::Closed, &event));
    CHECK(event.title == "Failed to connect to the server");
    CHECK(!event.detail.empty());
    session.stop("Quitting");
}

TEST(an_online_mode_server_is_refused_with_a_reason)
{
    FakeServer server;
    CHECK(server.listen());
    ClientSession session;
    CHECK(session.start("127.0.0.1", server.port, "Grisu"));
    CHECK(server.accept());
    Packet p;
    CHECK(server.receive(&p));
    server.send(makeHandshake("5f3a9c21d07e44b1"));

    ClientSession::Event event;
    CHECK(waitFor(session, ClientSession::Event::Kind::Closed, &event));
    CHECK(event.title == "Failed to login");
    CHECK(event.detail.find("online-mode=false") != std::string::npos);
    session.stop("Quitting");
}
