// What this console calls itself, where its key lives, and how a URL a player
// typed becomes an address.
//
// The friend codes here came from AlphaComputer's own `friend_code_for`, not
// from this implementation: the server validates the checksum at the door, so a
// console that computes it differently is a console that cannot log in at all.

#include "core/io/posix_file_system.hpp"
#include "core/net/ac_identity.hpp"
#include "core/net/ac_wire.hpp"
#include "core/util/ed25519.hpp"
#include "framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mc;
using namespace mc::net::ac;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_ident_XXXXXX");
        if (::mkdtemp(path) == nullptr) {
            path[0] = '\0';
        }
    }

    ~TempDir()
    {
        if (path[0] != '\0') {
            char command[128];
            std::snprintf(command, sizeof(command), "rm -rf '%s'", path);
            if (std::system(command) != 0) {
                std::fprintf(stderr, "warning: could not clean up %s\n", path);
            }
        }
    }

    std::string at(const char* name) const { return std::string(path) + "/" + name; }
};

// Entropy a test can predict. A real console asks `PS_GenerateRandomBytes`,
// which is the one part of this that cannot be checked against a known answer.
bool countingRandom(void* context, u8* out, usize size)
{
    u8* seed = static_cast<u8*>(context);
    for (usize i = 0; i < size; ++i) {
        out[i] = u8(*seed + i);
    }
    return true;
}

bool noRandom(void* context, u8* out, usize size)
{
    (void)context;
    (void)out;
    (void)size;
    return false;
}

}  // namespace

// The check byte is `sha1(principal as little-endian u32)[0] >> 1`. It rejects
// a typo and nothing else -- anyone can compute a valid-looking code for any
// principal ID, which is precisely why identity is staked with a key instead.
TEST(a_friend_code_is_the_one_the_server_would_have_built)
{
    struct Case {
        u32 principal;
        const char* code;
    };
    const Case cases[] = {
        {1, "128849018881"},
        {2, "021474836482"},
        {1000, "180388627432"},
        {0x12345678, "412622280312"},
        {0xFFFFFFFF, "468151435263"},
        {1799999999, "070519476735"},
        {0x11111111, "378243453201"},
    };

    for (const Case& one : cases) {
        char printed[24];
        std::snprintf(printed, sizeof(printed), "%012llu",
                      static_cast<unsigned long long>(friendCodeFor(one.principal)));
        CHECK_EQ(std::string(printed), std::string(one.code));
        CHECK_EQ(identityFor(one.principal), std::string("3ds:") + one.code);
    }
}

// Principal 0 is not an account, and the server refuses the identity built from
// it. Refusing here means a console that could not answer is told so rather
// than being handed something to argue about.
TEST(principal_zero_is_not_an_identity)
{
    CHECK(identityFor(0).empty());

    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("identity.key");
    u8 seed = 1;
    Identity identity;
    std::string error;
    CHECK(!loadOrCreateIdentity(fs, path.c_str(), 0, countingRandom, &seed, &identity, &error));
    CHECK(!error.empty());
    CHECK(!fs.exists(path.c_str()));
}

TEST(a_key_is_made_once_and_read_back_ever_after)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("identity.key");

    u8 seed = 7;
    Identity first;
    std::string error;
    CHECK(loadOrCreateIdentity(fs, path.c_str(), 0x11111111, countingRandom, &seed, &first,
                               &error));
    CHECK(first.created);
    CHECK_EQ(first.name, std::string("3ds:378243453201"));

    // The file is the secret and nothing else: 32 bytes, no header to migrate.
    std::vector<u8> onCard;
    CHECK(fs.readFile(path.c_str(), &onCard, 1024));
    CHECK_EQ(onCard.size(), util::kEd25519SeedSize);
    CHECK(std::memcmp(onCard.data(), first.seed, onCard.size()) == 0);

    // The public key is derived, not stored; the second load must produce the
    // same one or the server refuses the login as somebody else's key.
    u8 other = 99;
    Identity second;
    CHECK(loadOrCreateIdentity(fs, path.c_str(), 0x11111111, countingRandom, &other, &second,
                               &error));
    CHECK(!second.created);
    CHECK(std::memcmp(first.publicKey, second.publicKey, sizeof(first.publicKey)) == 0);
    CHECK(std::memcmp(first.seed, second.seed, sizeof(first.seed)) == 0);
}

// **A key file of the wrong length is not replaced.** Overwriting it would take
// this console's identity away for good, with no way back short of an operator
// clearing the binding by hand -- so it stops and says so instead.
TEST(a_key_file_that_is_the_wrong_size_is_refused_rather_than_overwritten)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("identity.key");

    const u8 stub[] = {1, 2, 3};
    CHECK(fs.writeFileAtomic(path.c_str(), ConstByteSpan(stub, sizeof(stub))));

    u8 seed = 3;
    Identity identity;
    std::string error;
    CHECK(!loadOrCreateIdentity(fs, path.c_str(), 1000, countingRandom, &seed, &identity,
                                &error));
    CHECK(!error.empty());

    std::vector<u8> after;
    CHECK(fs.readFile(path.c_str(), &after, 1024));
    CHECK_EQ(after.size(), usize(3));
}

// A key that exists only in memory would claim an identity on the first login
// and fail to prove it on the second, which is the one failure in this design
// that needs a human to undo.
TEST(a_key_that_cannot_be_written_is_not_played_with)
{
    TempDir dir;
    io::PosixFileSystem fs;
    const std::string path = dir.at("no-such-directory/identity.key");

    u8 seed = 5;
    Identity identity;
    std::string error;
    CHECK(!loadOrCreateIdentity(fs, path.c_str(), 1000, countingRandom, &seed, &identity,
                                &error));
    CHECK(!error.empty());

    CHECK(!loadOrCreateIdentity(fs, dir.at("k.key").c_str(), 1000, noRandom, nullptr, &identity,
                                &error));
    CHECK(!error.empty());
}

// The player is given the website, because that is what they will be typing a
// link code into. The control plane is a UDP port on the same host that appears
// in no URL anybody publishes, so the host name is what is taken and the port
// is what this build knows.
TEST(a_website_url_yields_the_host_and_the_control_port)
{
    struct Case {
        const char* url;
        const char* host;
        u16 port;
    };
    const Case cases[] = {
        {kDefaultServerUrl, "ac.grisu-ftp.de", kDefaultPort},
        {"https://ac.grisu-ftp.de/", "ac.grisu-ftp.de", kDefaultPort},
        {"http://ac.grisu-ftp.de/stats?x=1#y", "ac.grisu-ftp.de", kDefaultPort},
        {"ac.grisu-ftp.de", "ac.grisu-ftp.de", kDefaultPort},
        {"  ac.grisu-ftp.de  ", "ac.grisu-ftp.de", kDefaultPort},
        // A port the player named wins over the default, which is how a test
        // server on a laptop is reached.
        {"ac.grisu-ftp.de:9000", "ac.grisu-ftp.de", 9000},
        {"https://ac.grisu-ftp.de:9000/path", "ac.grisu-ftp.de", 9000},
        {"192.168.1.20:7000", "192.168.1.20", 7000},
        {"[2001:db8::5]:7000", "2001:db8::5", 7000},
        {"[2001:db8::5]", "2001:db8::5", kDefaultPort},
        // An unbracketed v6 literal keeps all of its colons rather than losing
        // its last group to a port that was never there.
        {"2001:db8::5", "2001:db8::5", kDefaultPort},
        {"user:pass@ac.grisu-ftp.de", "ac.grisu-ftp.de", kDefaultPort},
    };

    for (const Case& one : cases) {
        ServerAddress address;
        CHECK(parseServerUrl(one.url, &address));
        CHECK_EQ(address.host, std::string(one.host));
        CHECK_EQ(int(address.port), int(one.port));
    }
}

TEST(a_url_with_no_host_in_it_is_refused)
{
    ServerAddress address;
    CHECK(!parseServerUrl("", &address));
    CHECK(!parseServerUrl("   ", &address));
    CHECK(!parseServerUrl("https://", &address));
    CHECK(!parseServerUrl("https:///path", &address));
    CHECK(!parseServerUrl("[2001:db8::5", &address));
    // Port 0 is not a port, and 70000 is not one either -- both leave the
    // colon in the host, which then fails to resolve rather than silently
    // talking to the wrong place.
    CHECK(parseServerUrl("host:0", &address));
    CHECK_EQ(address.host, std::string("host:0"));
}
