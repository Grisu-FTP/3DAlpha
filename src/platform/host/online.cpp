// `--online <host[:port]> [identity] [seconds]`: the console's half of
// AlphaComputer, against a real one.
//
// **The tests below this are against bytes; this is against a server.**
// tests/ac_wire_test.cpp proves the codec agrees with the Rust encoder and
// tests/ac_client_test.cpp proves the state machine answers those bytes
// correctly, but neither of them has ever sent a datagram. This has: it
// resolves a name, opens a socket, signs a real nonce with a real key and finds
// out whether the server accepts it -- which is the one question a vector
// cannot answer, because the signature is verified by code that is not here.
//
// It is a harness and not a test: it needs a server, so it is run by hand.
//
//     ./build-host/3dalpha --online <server> profile [principal]
//     ./build-host/3dalpha --online <server> unlink  [principal]
//     ./build-host/3dalpha --online <server> host    [principal]
//     ./build-host/3dalpha --online <server> join <code> [principal]
//     ./build-host/3dalpha --online <server> export <world dir> [principal]
//     ./build-host/3dalpha --online <server> import <code> <saves dir> <name> [principal]
//
// `AC_TYPING_MS=<ms>` makes `join` wait the way a console does before it sends
// the code: the menu pumping, then the keyboard applet with nothing pumped.
//
// **`AC_FORCE_RELAY=1` in the environment makes the punch fail**, by dropping
// every probe this end would send straight to the other console. Set on both
// ends it is the relay path end to end -- the one a PC on a home network never
// takes by itself, because its punch always lands -- and it is how the relay's
// registration was checked against the real Rust relay rather than a fake.
//
// **The last two are the whole stack.** `host` opens a session and prints its
// join code; `join`, in another terminal, types that code back. The server
// introduces them, they punch at each other, and then a real
// `net::link::HostSession` and `net::link::GuestSession` shake hands over the
// result -- the same two objects a pair of consoles in one room run, over a
// socket instead of a radio. If the player list prints on both ends, everything
// from the signature to the session is working together.
//
// **`export` and `import` are world sharing**, on the server's TCP port rather
// than through a session. `export` uploads the world, prints `SHARECODE` the
// moment the server hands one out -- before the upload is done -- and keeps its
// login alive for `AC_SHARE_SECONDS` (default 120) after, because the share
// lives exactly as long as the login; then it withdraws it. `import`, in another
// terminal, downloads with that code into `<saves dir>/<name>`. Diff the two
// trees afterwards; that is the test.
//
// The identity defaults to a friend code derived from a fixed principal ID, so
// two runs are the same player and the second one exercises the *returning*
// path rather than the first claim. `host` and `join` use different ones,
// because the server refuses a console that tries to join itself.

#include "platform/host/online.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/net/ac_client.hpp"
#include "core/net/ac_identity.hpp"
#include "core/net/ac_link.hpp"
#include "core/net/session.hpp"
#include "core/net/terrain_share.hpp"
#include "core/net/tcp_socket.hpp"
#include "core/net/udp_socket.hpp"
#include "core/net/world_share.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace mc;

u32 nowMs()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    return u32(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

// The seed is a fixed pattern rather than real entropy: this is a harness, the
// identity it claims is a test one, and a run that produced a different key
// every time could never exercise the returning-login path.
bool fixedSeed(void* context, u8* out, usize size)
{
    (void)context;
    for (usize i = 0; i < size; ++i) {
        out[i] = u8(0xA0 + i);
    }
    return true;
}

// Prints what a session reports, which on this harness is the whole point: a
// player list on both ends is the proof that the link works.
class Printer : public net::link::SessionListener {
public:
    void onPlayerJoined(u8 playerId, const std::string& name) override
    {
        std::printf("joined   %u %s\n", unsigned(playerId), name.c_str());
    }

    void onPlayerLeft(u8 playerId, const std::string& reason) override
    {
        std::printf("left     %u %s\n", unsigned(playerId), reason.c_str());
    }

    void onChat(u8 playerId, const std::string& text) override
    {
        std::printf("chat     %u %s\n", unsigned(playerId), text.c_str());
    }
};

// The socket, less every direct probe. A probe sent straight to a peer is a
// datagram that starts with the probe magic; one sent through the relay starts
// with the relay token instead, so this leaves the relay's traffic alone.
class NoPunchTransport : public net::ac::UdpTransport {
public:
    explicit NoPunchTransport(net::ac::UdpTransport* inner) : inner_(inner) {}

    bool send(const net::ac::Endpoint& to, const u8* data, usize size) override
    {
        if (size >= sizeof(net::ac::kProbeMagic)
            && std::memcmp(data, net::ac::kProbeMagic, sizeof(net::ac::kProbeMagic)) == 0) {
            return true;  // "sent", and lost -- which is what a sealed NAT does
        }
        return inner_->send(to, data, size);
    }

    bool receive(u8* buffer, usize capacity, usize* size, net::ac::Endpoint* from) override
    {
        return inner_->receive(buffer, capacity, size, from);
    }

    net::ac::Endpoint localEndpoint() const override { return inner_->localEndpoint(); }

private:
    net::ac::UdpTransport* inner_;
};

const char* stageName(net::ac::State state)
{
    switch (state) {
    case net::ac::State::Idle:       return "idle";
    case net::ac::State::Connecting: return "connecting";
    case net::ac::State::Ready:      return "ready";
    case net::ac::State::Failed:     return "failed";
    }
    return "?";
}

}  // namespace

int runOnline(int argc, char** argv)
{
    // Line-buffered: two of these are run side by side and one of them is
    // waiting for a code the other prints, so output held back until the
    // process exits would make the harness unusable.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const char* target = argc > 2 ? argv[2] : "127.0.0.1:7717";
    const std::string mode = argc > 3 ? argv[3] : "profile";
    const bool hosting = mode == "host";
    const bool joining = mode == "join";
    const bool unlinking = mode == "unlink";
    const bool exporting = mode == "export";
    const bool importing = mode == "import";
    const bool sharing = exporting || importing;
    const char* joinCode = (joining || importing) && argc > 4 ? argv[4] : "";
    const char* exportDir = exporting && argc > 4 ? argv[4] : "";
    const char* importSaves = importing && argc > 5 ? argv[5] : "";
    const char* importName = importing && argc > 6 ? argv[6] : "";
    const int principalArg = joining ? 5 : (exporting ? 5 : (importing ? 7 : 4));
    // The downloading end is a different console: one identity logged in twice
    // is one login, and the second would end the first one's share.
    const u32 principal = argc > principalArg
                              ? u32(std::strtoul(argv[principalArg], nullptr, 10))
                              : (joining || importing ? 0x22222222u : 0x11111111u);

    if (joining && joinCode[0] == '\0') {
        std::printf("--online <server> join <code>\n");
        return 1;
    }
    if (exporting && exportDir[0] == '\0') {
        std::printf("--online <server> export <world dir>\n");
        return 1;
    }
    if (importing && (joinCode[0] == '\0' || importSaves[0] == '\0' || importName[0] == '\0')) {
        std::printf("--online <server> import <code> <saves dir> <name>\n");
        return 1;
    }

    mc::net::ac::ServerAddress address;
    if (!mc::net::ac::parseServerUrl(target, &address)) {
        std::printf("`%s` is not an address\n", target);
        return 1;
    }

    u32 resolved = 0;
    std::string error;
    if (!mc::net::resolveHostAddress(address.host.c_str(), &resolved, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    const mc::net::ac::Endpoint server = mc::net::ac::endpointV4(resolved, address.port);
    std::printf("server   %s\n", mc::net::ac::endpointText(server).c_str());

    // A key beside the harness rather than on a card, one per identity.
    // Deleting it makes the next run a first claim again.
    char keyPath[64];
    std::snprintf(keyPath, sizeof(keyPath), "/tmp/3dalpha-online-%08x.key", principal);
    mc::io::PosixFileSystem fs;
    mc::net::ac::Identity identity;
    if (!mc::net::ac::loadOrCreateIdentity(fs, keyPath, principal, &fixedSeed, nullptr,
                                           &identity, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("identity %s%s\n", identity.name.c_str(), identity.created ? "  (new key)" : "");

    mc::net::UdpSocket socket;
    // INADDR_ANY, which is what it means on a PC. A console passes its own
    // address instead -- see platform/ctr/online.cpp.
    if (!socket.open(server, 0, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }
    std::printf("local    %s\n", mc::net::ac::endpointText(socket.localEndpoint()).c_str());

    mc::net::ac::Login login;
    login.identity = identity.name;
    std::memcpy(login.seed, identity.seed, sizeof(login.seed));
    std::memcpy(login.publicKey, identity.publicKey, sizeof(login.publicKey));
    login.hasPlatformName = true;
    login.platformName = hosting || exporting ? "HarnessHost"
                         : (joining || importing ? "HarnessGuest" : "HostHarness");

    // **`AC_TYPING_MS=<ms>` joins the way a console does**: two seconds of menu,
    // then that long with the loop stopped for the keyboard, then the code.
    const char* typing = std::getenv("AC_TYPING_MS");
    const u32 typingMs = typing != nullptr ? u32(std::strtoul(typing, nullptr, 10)) : 0;

    NoPunchTransport noPunch(&socket);
    const char* forceRelay = std::getenv("AC_FORCE_RELAY");
    const bool relayOnly = forceRelay != nullptr && forceRelay[0] == '1';
    mc::net::ac::UdpTransport* transport = &socket;
    if (relayOnly) {
        transport = &noPunch;
        std::printf("relay    forced: direct probes are dropped\n");
    }

    mc::net::ac::Connection connection;
    connection.begin(server, login, transport, hosting, nowMs());

    Printer printer;
    mc::net::link::HostSession host;
    mc::net::link::GuestSession guest;
    mc::net::link::GeneratorId world;
    world.seed = 1234;
    world.version = mc::net::link::generatorVersion();

    bool asked = false;
    bool requested = false;
    bool sessionStarted = false;
    bool saidHello = false;
    int listedPlayers = -1;
    mc::net::ac::State last = mc::net::ac::State::Idle;

    // An errand is answered in a round trip; a session is watched for a while.
    // Longer by the typing, on both ends: a host that is to be joined after a
    // minute in the keyboard has to still be there.
    // World sharing: the job runs on a thread of its own, as it does on the
    // console, and this loop goes on pumping the login under it -- a share
    // whose owner stops sending keep-alives is deleted.
    const char* shareSecondsText = std::getenv("AC_SHARE_SECONDS");
    const u32 shareMs =
        (shareSecondsText != nullptr ? u32(std::strtoul(shareSecondsText, nullptr, 10)) : 120u)
        * 1000u;
    std::unique_ptr<mc::net::share::Upload> upload;
    std::unique_ptr<mc::net::share::Download> download;
    mc::net::share::Job* job = nullptr;
    mc::net::TcpSocket transferSocket;
    std::thread worker;
    int shareResult = -1;
    u32 sharedAtMs = 0;
    mc::net::share::Stage lastStage = mc::net::share::Stage::Connecting;
    u64 lastPrintedBytes = 0;
    bool codeShown = false;

    const u32 deadline = (hosting || joining) ? 45000 + typingMs : (sharing ? 0xFFFFFFFFu : 8000);
    while (nowMs() < deadline) {
        connection.pump(nowMs());

        const mc::net::ac::State state = connection.client().state();
        if (state != last) {
            std::printf("state    %s\n", stageName(state));
            last = state;
            if (state == mc::net::ac::State::Failed) {
                std::printf("         %s\n", connection.client().message().c_str());
                return 1;
            }
        }

        mc::net::ac::Event event = connection.takeEvent();
        while (event != mc::net::ac::Event::None) {
            switch (event) {
            case mc::net::ac::Event::LoggedIn:
                std::printf("name     %s\n", connection.client().displayName().c_str());
                std::printf("account  %s\n",
                            connection.client().accountHandle().empty()
                                ? "(not linked)"
                                : connection.client().accountHandle().c_str());
                std::printf("public   %s\n",
                            mc::net::ac::endpointText(connection.client().reflexive()).c_str());
                if (connection.client().firstClaim()) {
                    std::printf("claimed  %s\n", connection.client().keyFingerprint().c_str());
                }
                break;
            case mc::net::ac::Event::LinkCodeIssued:
                std::printf("code     %s  (%u s)\n", connection.client().linkCode().c_str(),
                            unsigned(connection.client().linkCodeSeconds()));
                break;
            case mc::net::ac::Event::Unlinked:
                std::printf("unlinked, now %s\n", connection.client().displayName().c_str());
                break;
            case mc::net::ac::Event::SessionOpened:
                std::printf("session  %llu\n",
                            static_cast<unsigned long long>(connection.client().sessionId()));
                std::printf("JOINCODE %s\n", connection.client().joinCode().c_str());
                break;
            case mc::net::ac::Event::SessionsListed:
                std::printf("listed   %d session(s)\n",
                            int(connection.client().sessions().size()));
                for (const mc::net::ac::SessionInfo& info : connection.client().sessions()) {
                    std::printf("         %s  %s  %u/%u  %s\n", info.joinCode.c_str(),
                                info.worldName.c_str(), unsigned(info.players),
                                unsigned(info.maxPlayers), info.hostName.c_str());
                }
                break;
            case mc::net::ac::Event::PunchNow:
                std::printf("punch    towards %s\n",
                            connection.client().introduction().peerName.c_str());
                break;
            case mc::net::ac::Event::RelayAllocated:
                std::printf("relay    %s\n",
                            mc::net::ac::endpointText(connection.client().introduction().relay)
                                .c_str());
                break;
            case mc::net::ac::Event::Refused:
                std::printf("refused  %s\n", connection.client().message().c_str());
                break;
            default:
                break;
            }
            event = connection.takeEvent();
        }

        if (connection.client().ready()) {
            if (hosting && !requested) {
                requested = true;
                connection.client().hostSession("Harness World", 4, true);
            } else if (joining && !requested) {
                requested = true;
                if (typingMs > 0) {
                    // The console's join: the menu goes on pumping while the
                    // player finds the row, then the keyboard applet stops the
                    // loop outright for as long as the code takes to type.
                    const u32 until = nowMs() + 2000;
                    while (nowMs() < until) {
                        connection.pump(nowMs());
                        while (connection.takeEvent() != mc::net::ac::Event::None) {
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(16));
                    }
                    std::printf("typing   %u ms with nothing pumped\n", unsigned(typingMs));
                    std::this_thread::sleep_for(std::chrono::milliseconds(typingMs));
                }
                connection.client().joinByCode(joinCode);
            } else if (sharing && job == nullptr) {
                const u16 port = connection.client().transferPort();
                std::printf("transfer port %u\n", unsigned(port));
                if (!mc::net::share::connectTransfer(transferSocket, resolved, port, &error)) {
                    std::printf("failed   %s\n", error.c_str());
                    return 1;
                }
                if (exporting) {
                    const char* slash = std::strrchr(exportDir, '/');
                    const std::string name = slash != nullptr && slash[1] != '\0' ? slash + 1
                                                                                  : exportDir;
                    upload = std::make_unique<mc::net::share::Upload>(fs, exportDir, name,
                                                                      connection.client().token());
                    if (!upload->prepare()) {
                        std::printf("failed   %s\n", upload->error().c_str());
                        return 1;
                    }
                    job = upload.get();
                    std::printf("world    %s, %u files, %llu bytes\n", name.c_str(),
                                unsigned(upload->progress().filesTotal),
                                static_cast<unsigned long long>(upload->progress().bytesTotal));
                    worker = std::thread([&] {
                        mc::net::share::TcpStream stream(transferSocket);
                        upload->run(stream);
                        transferSocket.close();
                    });
                } else {
                    download = std::make_unique<mc::net::share::Download>(
                        fs, importSaves, importName, joinCode, connection.client().token());
                    job = download.get();
                    worker = std::thread([&] {
                        mc::net::share::TcpStream stream(transferSocket);
                        download->run(stream);
                        transferSocket.close();
                    });
                }
            } else if (unlinking && !asked) {
                asked = true;
                connection.client().unlink();
            } else if (!hosting && !joining && !unlinking && !sharing && !asked) {
                asked = true;
                connection.client().requestLinkCode();
            }
        }

        // **The session, the moment there is somebody to have one with.** This
        // is the same pair of objects two consoles in a room run; what changed
        // underneath them is only what the frames travel on.
        if ((hosting || joining) && !sessionStarted && connection.connected()) {
            sessionStarted = true;
            std::printf("linked   %s\n",
                        connection.relayed(joining ? mc::net::link::kHostNode : u16(2))
                            ? "through the relay"
                            : "directly");
            if (hosting) {
                host.open("Harness World", "HarnessHost", world, nowMs(), &printer);
            } else {
                guest.join("HarnessGuest", world, nowMs(), &printer);
            }
        }

        if (sessionStarted && hosting) {
            host.pump(nowMs(), connection);
            if (host.guestCount() != listedPlayers) {
                listedPlayers = host.guestCount();
                std::printf("guests   %d\n", listedPlayers);
                if (listedPlayers > 0 && !saidHello) {
                    saidHello = true;
                    host.say("hello from the host");
                }
            }
        } else if (sessionStarted && joining) {
            guest.pump(nowMs(), connection);
            if (int(guest.players().size()) != listedPlayers) {
                listedPlayers = int(guest.players().size());
                std::printf("players  %d, in %s hosted by %s\n", listedPlayers,
                            guest.worldName().c_str(), guest.hostName().c_str());
                for (const mc::net::link::Player& player : guest.players()) {
                    std::printf("         %u %s\n", unsigned(player.playerId), player.name.c_str());
                }
                if (listedPlayers > 0 && !saidHello) {
                    saidHello = true;
                    guest.say("hello from the guest");
                }
            }
            if (guest.state() == mc::net::link::GuestSession::State::Finished) {
                std::printf("ended    %s\n", guest.reason().c_str());
                break;
            }
        }

        if (job != nullptr && shareResult < 0) {
            const mc::net::share::Progress progress = job->progress();
            if (progress.stage != lastStage) {
                lastStage = progress.stage;
                std::printf("stage    %s\n", mc::net::share::describeStage(progress.stage));
            }
            if (exporting && !codeShown && (progress.stage == mc::net::share::Stage::Sending
                                            || progress.stage == mc::net::share::Stage::Finishing
                                            || progress.stage == mc::net::share::Stage::Shared)) {
                codeShown = true;
                std::printf("SHARECODE %s\n", job->code().c_str());
            }
            if (progress.wireBytes >= lastPrintedBytes + (1u << 20)) {
                lastPrintedBytes = progress.wireBytes;
                std::printf("progress %llu / %llu bytes, %u / %u files, %llu on the wire\n",
                            static_cast<unsigned long long>(progress.bytesDone),
                            static_cast<unsigned long long>(progress.bytesTotal),
                            unsigned(progress.filesDone), unsigned(progress.filesTotal),
                            static_cast<unsigned long long>(progress.wireBytes));
            }
            if (job->finished()) {
                worker.join();
                std::printf("progress %llu / %llu bytes, %u / %u files, %llu on the wire\n",
                            static_cast<unsigned long long>(progress.bytesDone),
                            static_cast<unsigned long long>(progress.bytesTotal),
                            unsigned(progress.filesDone), unsigned(progress.filesTotal),
                            static_cast<unsigned long long>(progress.wireBytes));
                if (progress.stage == mc::net::share::Stage::Failed) {
                    std::printf("failed   %s\n", job->error().c_str());
                    return 1;
                }
                if (importing) {
                    std::printf("imported \"%s\" as %s/%s\n", job->worldName().c_str(),
                                importSaves, importName);
                    shareResult = 0;
                    break;
                }
                std::printf("shared   for %u s while this login lasts\n", unsigned(shareMs / 1000));
                sharedAtMs = nowMs();
                shareResult = 0;
            }
        }
        if (exporting && shareResult == 0 && nowMs() - sharedAtMs >= shareMs) {
            mc::net::TcpSocket stopSocket;
            if (mc::net::share::connectTransfer(stopSocket, resolved,
                                                connection.client().transferPort(), &error)) {
                mc::net::share::TcpStream stream(stopSocket);
                if (mc::net::share::stopSharing(stream, connection.client().token(), &error)) {
                    std::printf("stopped  the share is withdrawn\n");
                } else {
                    std::printf("stop     %s\n", error.c_str());
                }
            } else {
                std::printf("stop     %s\n", error.c_str());
            }
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (worker.joinable()) {
        if (job != nullptr) {
            job->cancel();
        }
        worker.join();
    }

    if (sessionStarted && hosting) {
        host.close("the harness stopped", nowMs(), connection);
    } else if (sessionStarted && joining) {
        guest.leave("the harness stopped", nowMs(), connection);
    }
    connection.client().closeSession();
    connection.pump(nowMs());
    std::printf("done\n");
    return 0;
}
