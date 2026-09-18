// SOC and FRD for multiplayer. See network.hpp.

#include "platform/ctr/network.hpp"

#include "core/net/dns.hpp"
#include "core/net/server_list.hpp"

#include <3ds.h>

#include <malloc.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace mc::ctr {

namespace {

// SOC's own working memory. A megabyte is what libctru's examples use, and it
// comes out of the newlib heap once for the life of the process rather than
// per session.
constexpr u32 kSocBufferBytes = 0x100000;

u32* gSocBuffer = nullptr;
bool gSocUp = false;

// **The name servers DHCP gave this console**, which on a home network is the
// router -- and which is the whole of the answer to "can it not just use the
// 3DS's or the router's DNS". `SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_DNS_TABLE)`
// hands back an array of `SOCU_DNSTableEntry`; libctru's header notes that the
// buffer comes back 336 bytes with only the first two entries set, so the count
// is taken from the length the call reports and then bounded by what is
// actually addressed.
//
// **The default gateway is appended as a last resort.** A router that hands out
// its own address as the DNS server is the usual case and this adds nothing
// then -- the duplicate is skipped -- but a lease that names a DNS server which
// has since gone away still leaves the router itself answering on port 53,
// which is one more thing to try before telling the player to type an IP.
int consoleNameServers(void*, mc::u32* out, int max)
{
    if (!gSocUp || out == nullptr || max <= 0) {
        return 0;
    }

    int count = 0;
    const auto add = [&](mc::u32 address) {
        if (address == 0 || count >= max) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            if (out[i] == address) {
                return;
            }
        }
        out[count++] = address;
    };

    SOCU_DNSTableEntry table[8] = {};
    socklen_t length = sizeof(table);
    if (SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_DNS_TABLE, table, &length) == 0) {
        const usize entries = usize(length) / sizeof(SOCU_DNSTableEntry);
        const usize cap = entries < 8 ? entries : 8;
        for (usize i = 0; i < cap; ++i) {
            add(ntohl(table[i].ip.s_addr));
        }
    }

    SOCU_RoutingTableEntry routes[8] = {};
    socklen_t routeLength = sizeof(routes);
    if (SOCU_GetNetworkOpt(SOL_CONFIG, NETOPT_ROUTING_TABLE, routes, &routeLength) == 0) {
        const usize entries = usize(routeLength) / sizeof(SOCU_RoutingTableEntry);
        const usize cap = entries < 8 ? entries : 8;
        for (usize i = 0; i < cap; ++i) {
            if ((routes[i].flags & ROUTING_FLAG_G) != 0) {
                add(ntohl(routes[i].gateway.s_addr));
            }
        }
    }
    return count;
}

}  // namespace

bool startNetwork(std::string* error)
{
    if (!gSocUp) {
        if (gSocBuffer == nullptr) {
            gSocBuffer = static_cast<u32*>(memalign(0x1000, kSocBufferBytes));
        }
        if (gSocBuffer == nullptr) {
            *error = "There is no memory left for the network service.";
            return false;
        }
        const Result rc = socInit(gSocBuffer, kSocBufferBytes);
        if (R_FAILED(rc)) {
            char text[96];
            std::snprintf(text, sizeof(text), "The network service would not start (0x%08lX).",
                          static_cast<unsigned long>(rc));
            *error = text;
            std::free(gSocBuffer);
            gSocBuffer = nullptr;
            return false;
        }
        gSocUp = true;
        // **Installed once SOC is up, because it is SOC that answers it.** Core
        // resolves through this when neither of the console's own resolvers
        // does; see core/net/dns.hpp.
        net::setNameServerSource(&consoleNameServers, nullptr);
    }

    return true;
}

bool haveAddress()
{
    // **Asked after a failure, never before one.** This used to refuse the
    // connection outright when the console had no address yet, which is a
    // guess: the address arrives with the association and DHCP, and a check run
    // the instant SOC comes up can be reading the moment before rather than a
    // console with its Wi-Fi off. Turning that guess into a refusal means a
    // player who *could* have connected is told they cannot. So the connection
    // is attempted whatever this says, and this only explains a failure.
    return gethostid() != 0;
}

u32 localAddress()
{
    // `gethostid` reports it in network byte order, like an `in_addr`; every
    // caller here wants a number it can compare and print.
    return ntohl(u32(gethostid()));
}

void stopNetwork()
{
    if (gSocUp) {
        net::setNameServerSource(nullptr, nullptr);
        socExit();
        gSocUp = false;
    }
    if (gSocBuffer != nullptr) {
        std::free(gSocBuffer);
        gSocBuffer = nullptr;
    }
}

std::string loginName()
{
    return net::usernameFrom(friendScreenName());
}

std::string friendScreenName()
{
    std::string screenName;
    // frd:u, the user service: the one an application holds, and the one
    // GetMyScreenName is on.
    if (R_SUCCEEDED(frdInit(true))) {
        MiiScreenName name = {};
        if (R_SUCCEEDED(FRD_GetMyScreenName(&name))) {
            screenName = net::utf16ToUtf8(name, MII_NAME_LEN);
        }
        frdExit();
    }
    return screenName;
}

u32 principalId()
{
    u32 principal = 0;
    if (R_SUCCEEDED(frdInit(true))) {
        FriendKey key = {};
        if (R_SUCCEEDED(FRD_GetMyFriendKey(&key))) {
            principal = key.principalId;
        }
        frdExit();
    }
    if (principal != 0) {
        return principal;
    }

    // **The fallback, and what it costs.** A console whose friend account was
    // never set up has no principal ID, and refusing to let it online at all
    // would be the wrong trade for a service whose identities are staked rather
    // than proved anyway. The device ID is a different number about the same
    // console: stable, console-unique, and readable without the friend service.
    //
    // What it risks is a collision with a real friend code's principal, which
    // would be one console finding its identity already claimed. That is the
    // same failure the design already has a story for -- it is refused, it is
    // logged, and an operator clears it -- and it is rarer than the number of
    // consoles that have no friend account at all.
    if (R_SUCCEEDED(psInit())) {
        u32 device = 0;
        if (R_SUCCEEDED(PS_GetDeviceId(&device))) {
            principal = device;
        }
        psExit();
    }
    return principal;
}

bool randomBytes(void* context, u8* out, usize size)
{
    (void)context;
    if (R_FAILED(psInit())) {
        return false;
    }
    const Result rc = PS_GenerateRandomBytes(out, size);
    psExit();
    return R_SUCCEEDED(rc);
}

}  // namespace mc::ctr
