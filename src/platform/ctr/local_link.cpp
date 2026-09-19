#include "platform/ctr/local_link.hpp"

#include "core/net/server_list.hpp"
#include "core/net/session.hpp"

#include <cstdio>
#include <cstring>

namespace mc::ctr {

namespace {

// **The network's identity on the air.** `wlancommID` is what separates this
// game's beacons from every other console in range; it is an application's own
// number rather than one issued to us, so it is simply an unlikely constant
// with the port's name in it. `id8` distinguishes kinds of session within the
// game, and there is one kind so far.
constexpr u32 kWlanCommId = 0x3DA11200;
constexpr u8 kSessionId8 = 1;

// The data channel every frame is filtered by. Non-zero is a service
// requirement.
constexpr u8 kDataChannel = 1;

// Not a secret -- a passphrase on a local network is what stops a build with a
// different frame layout from connecting, and this one changes with the link
// protocol for exactly that reason.
constexpr char kPassphrase[] = "3dalpha-local-2";

// The host plus `kMaxGuests`. UDS allows sixteen; the limit is the world, and
// telling the service the real number is what makes a full session refuse a
// fifth console on the radio rather than in our code.
constexpr u8 kMaxNodes = u8(1 + net::link::kMaxGuests);

// Shared memory for the service, and the buffer inside it that holds frames
// until they are pulled.
//
// **Bigger than the default on purpose.** `UDS_DEFAULT_RECVBUFSIZE` is 0x2E30,
// which is about eight of this link's datagrams: eight frames is what the
// radio can hold for an application that has stopped pulling, and a console
// running a library applet has stopped pulling for as long as somebody is
// typing. Everything past the eighth is dropped by the service, and the link
// then has to retransmit a whole world's worth of ground -- which it does, and
// slowly.
//
// Forty kilobytes is roughly thirty datagrams. It does not make a suspension
// free, and nothing can; it makes coming back from one cost a stutter instead
// of a visible reload. The shared block is sized to hold the buffer with the
// service's own bookkeeping beside it rather than sitting against its end.
constexpr usize kSharedMemSize = 0x10000;
constexpr u32 kRecvBufferSize = 0xA000;

// The beacon's payload is 200 bytes, so the two names in it are cut rather
// than allowed to push each other out.
constexpr usize kMaxAppData = 0xC8;
constexpr usize kMaxAdvertisedName = 32;

constexpr u8 kAppDataMagic[4] = {'3', 'D', 'A', 'L'};

bool serviceUp = false;

// **Links that are hosting or joined right now**, which is what decides whether
// the service may be let go. See `releaseLocalWireless`.
int openLinks = 0;

// When `udsExit` last ran, in `osGetTime` milliseconds; 0 for never.
u64 releasedAtMs = 0;

// See `setLinkPauseHook`. Null when no session is open, which is every
// single-player keyboard in the build.
LinkPauseFn pauseHook = nullptr;
void* pauseCtx = nullptr;

// What every failure here has to answer: a player who sees "could not host"
// wants to know whether to flip the wireless switch or move closer.
std::string wirelessError(const char* what, Result code)
{
    char text[128];
    std::snprintf(text, sizeof(text), "%s (0x%08lX)", what, static_cast<unsigned long>(code));
    return std::string(text);
}

void putShortString(std::vector<u8>* out, const std::string& text, usize limit)
{
    usize size = text.size() < limit ? text.size() : limit;
    // Never cut a UTF-8 character in half: the name goes on another console's
    // screen, and half a character there is a glyph nobody chose.
    while (size > 0 && (text[size] & 0xC0) == 0x80) {
        --size;
    }
    out->push_back(u8(size));
    out->insert(out->end(), text.begin(), text.begin() + std::ptrdiff_t(size));
}

bool takeShortString(const u8* data, usize size, usize* pos, std::string* out)
{
    if (*pos >= size) {
        return false;
    }
    const usize length = data[*pos];
    ++*pos;
    if (size - *pos < length) {
        return false;
    }
    out->assign(reinterpret_cast<const char*>(data + *pos), length);
    *pos += length;
    return true;
}

// The beacon's payload: who is hosting what, and on which protocol.
std::vector<u8> buildAppData(const std::string& worldName, const std::string& hostName,
                             int players, LocalKind kind)
{
    std::vector<u8> out;
    out.reserve(kMaxAppData);
    out.insert(out.end(), kAppDataMagic, kAppDataMagic + sizeof(kAppDataMagic));
    out.push_back(u8(net::link::kProtocol >> 8));
    out.push_back(u8(net::link::kProtocol));
    out.push_back(u8(players < 0 ? 0 : players));
    out.push_back(kMaxNodes);
    // The kind rides between the counts and the names, which is why adding it
    // bumped `net::link::kProtocol` and the passphrase with it: a build that
    // read it as the first byte of a name would put a control character on
    // somebody's screen.
    out.push_back(u8(kind));
    putShortString(&out, worldName, kMaxAdvertisedName);
    putShortString(&out, hostName, kMaxAdvertisedName);
    if (out.size() > kMaxAppData) {
        out.resize(kMaxAppData);
    }
    return out;
}

// False when the beacon is not one of ours at all, which is how a scan tells
// another game's network from a 3DAlpha session running a protocol this build
// does not speak.
bool readAppData(const u8* data, usize size, LocalSession* out)
{
    if (size < sizeof(kAppDataMagic) + 5) {
        return false;
    }
    if (std::memcmp(data, kAppDataMagic, sizeof(kAppDataMagic)) != 0) {
        return false;
    }
    usize pos = sizeof(kAppDataMagic);
    const u16 protocol = u16(u16(data[pos]) << 8 | data[pos + 1]);
    pos += 2;
    out->players = data[pos++];
    out->maxPlayers = data[pos++];
    const u8 kind = data[pos++];
    out->kind = kind == u8(LocalKind::WorldOffer) ? LocalKind::WorldOffer : LocalKind::Session;
    out->compatible = protocol == net::link::kProtocol;
    if (!takeShortString(data, size, &pos, &out->worldName)
        || !takeShortString(data, size, &pos, &out->hostName)) {
        // A beacon of ours whose names did not survive the air is still a
        // session; it just has nothing to put in the row.
        out->worldName.clear();
        out->hostName.clear();
    }
    return true;
}

}  // namespace

bool startLocalWireless(std::string* error)
{
    if (serviceUp) {
        return true;
    }
    // The console's own name goes on the air as this node's username, which is
    // what another console's system-level list shows. The session's player name
    // is sent separately in the handshake -- see core/net/session.hpp -- so the
    // two can differ without either being wrong.
    const Result result = udsInit(kSharedMemSize, nullptr);
    if (R_FAILED(result)) {
        *error = wirelessError("local wireless could not start -- check the wireless switch",
                               result);
        return false;
    }
    serviceUp = true;
    return true;
}

void stopLocalWireless()
{
    if (!serviceUp) {
        return;
    }
    udsExit();
    serviceUp = false;
    releasedAtMs = osGetTime();
}

void releaseLocalWireless()
{
    if (openLinks == 0) {
        stopLocalWireless();
    }
}

bool localWirelessReleasedWithin(u32 ms)
{
    return !serviceUp && releasedAtMs != 0 && osGetTime() - releasedAtMs < ms;
}

bool localWirelessReady()
{
    return serviceUp;
}

void setLinkPauseHook(LinkPauseFn hook, void* ctx)
{
    pauseHook = hook;
    pauseCtx = ctx;
}

void linkPausing(u32 expectedMs)
{
    if (pauseHook != nullptr) {
        pauseHook(pauseCtx, expectedMs);
    }
}

bool scanLocalSessions(std::vector<LocalSession>* out, std::string* error)
{
    out->clear();
    if (!serviceUp && !startLocalWireless(error)) {
        return false;
    }
    // **The radio is only borrowed for the scan.** A list of sessions is a
    // picture of the room, not a connection to it, and the Java servers and the
    // internet rows sit on the same screen: holding UDS up while the player
    // reads the list would keep the console off its access point for as long
    // as they look. A join brings the service back.
    struct Release {
        ~Release() { releaseLocalWireless(); }
    } release;

    // The service parses beacons into this buffer and hands back pointers into
    // it, so it has to outlive the walk below. Heap rather than stack: the
    // main thread's is 32 KB.
    std::vector<u8> buffer(0x4000);
    udsNetworkScanInfo* networks = nullptr;
    usize total = 0;
    const Result result = udsScanBeacons(buffer.data(), buffer.size(), &networks, &total,
                                         kWlanCommId, kSessionId8, nullptr, false);
    if (R_FAILED(result)) {
        *error = wirelessError("could not search for sessions", result);
        return false;
    }

    for (usize i = 0; i < total; ++i) {
        LocalSession session;
        session.network = networks[i].network;

        u8 appdata[kMaxAppData];
        usize actual = 0;
        const Result read = udsGetNetworkStructApplicationData(&session.network, appdata,
                                                               sizeof(appdata), &actual);
        if (R_FAILED(read) || !readAppData(appdata, actual, &session)) {
            continue;  // somebody else's network on the same channel
        }
        if (session.worldName.empty()) {
            session.worldName = "a world";
        }
        if (session.hostName.empty()) {
            session.hostName = net::kFallbackUsername;
        }
        // The beacon's own node count is the authority on how full it is; the
        // appdata's copy is one refresh behind when somebody has just joined.
        if (session.network.total_nodes != 0) {
            session.players = session.network.total_nodes;
            session.maxPlayers = session.network.max_nodes;
        }
        out->push_back(std::move(session));
    }
    return true;
}

LocalLink::~LocalLink()
{
    leave();
}

bool LocalLink::host(const std::string& worldName, const std::string& hostName,
                     LocalKind kind, std::string* error)
{
    // Leave first: leaving the last link lets the service go, and this one is
    // about to need it.
    leave();
    if (!serviceUp && !startLocalWireless(error)) {
        return false;
    }

    worldName_ = worldName;
    hostName_ = hostName;
    kind_ = kind;
    maxNodes_ = kMaxNodes;

    udsGenerateDefaultNetworkStruct(&network_, kWlanCommId, kSessionId8, kMaxNodes);
    const Result created = udsCreateNetwork(&network_, kPassphrase, sizeof(kPassphrase),
                                            &bind_, kDataChannel, kRecvBufferSize);
    if (R_FAILED(created)) {
        *error = wirelessError("could not open a local session", created);
        releaseLocalWireless();
        return false;
    }
    bound_ = true;
    active_ = true;
    hosting_ = true;
    ++openLinks;
    node_ = net::link::kHostNode;
    advertise(0);
    return true;
}

void LocalLink::advertise(int players)
{
    if (!active_ || !hosting_) {
        return;
    }
    const std::vector<u8> appdata = buildAppData(worldName_, hostName_, players + 1, kind_);
    udsSetApplicationData(appdata.data(), appdata.size());
}

bool LocalLink::join(const LocalSession& session, std::string* error)
{
    leave();
    if (!serviceUp && !startLocalWireless(error)) {
        return false;
    }

    network_ = session.network;
    const Result connected =
        udsConnectNetwork(&network_, kPassphrase, sizeof(kPassphrase), &bind_,
                          UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, kDataChannel,
                          kRecvBufferSize);
    if (R_FAILED(connected)) {
        *error = wirelessError("could not reach that session", connected);
        releaseLocalWireless();
        return false;
    }
    bound_ = true;
    active_ = true;
    hosting_ = false;
    ++openLinks;

    // Which node this console became. Only used for the debug page: everything
    // a guest sends goes to the host, which knows where it came from.
    udsConnectionStatus status;
    node_ = R_SUCCEEDED(udsGetConnectionStatus(&status)) ? status.cur_NetworkNodeID : 0;
    return true;
}

void LocalLink::leave()
{
    if (!active_) {
        return;
    }
    if (hosting_) {
        udsDestroyNetwork();
    } else {
        udsDisconnectNetwork();
    }
    if (bound_) {
        udsUnbind(&bind_);
        bound_ = false;
    }
    active_ = false;
    hosting_ = false;
    node_ = 0;

    // **The last link out hands the radio back.** UDS holds the console's
    // wireless in NDM's local-communication state from `udsInit` to `udsExit`
    // -- libctru's own pair enters and leaves it -- and while it does, the
    // console is off its access point: no internet, no Online row, and the HOME
    // Menu's own services offline too, until the game is closed. Nothing keeps
    // the service up between sessions for a reason; the next host, join or
    // scan starts it again.
    --openLinks;
    releaseLocalWireless();
}

bool LocalLink::send(u16 node, const u8* data, usize size)
{
    if (!active_) {
        return false;
    }
    const Result result = udsSendTo(node, kDataChannel, UDS_SENDFLAG_Default, data, size);
    if (R_SUCCEEDED(result)) {
        return true;
    }
    // **A full send buffer is not a broken link.** The radio is behind, this
    // frame never left, and the link layer notices the missing acknowledgement
    // and sends it again -- which is exactly what it does for a frame lost in
    // the air. Only the other failures mean the network itself is gone.
    if (!UDS_CHECK_SENDTO_FATALERROR(result)) {
        ++overflows_;
        return true;
    }
    return false;
}

bool LocalLink::receive(u8* buffer, usize capacity, usize* size, u16* node)
{
    if (!active_) {
        return false;
    }
    usize actual = 0;
    u16 from = 0;
    const Result result = udsPullPacket(&bind_, buffer, capacity, &actual, &from);
    if (R_FAILED(result) || actual == 0) {
        return false;
    }
    *size = actual;
    *node = from;
    return true;
}

}  // namespace mc::ctr
