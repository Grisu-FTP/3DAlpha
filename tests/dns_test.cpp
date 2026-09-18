// The A query and its answer, against bytes rather than against a resolver.
//
// **There is no way to test this end to end where it matters.** The host build
// answers at `getaddrinfo` and never reaches our resolver at all; the console
// reaches it and has no test harness. So what is checked here is the one part
// that is the same on both: the wire format, and a parser that is reading a
// packet from a machine on the network and must not be led off the end of it.

#include "core/net/dns.hpp"
#include "framework.hpp"

#include <cstring>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

// The header, then the name as length-prefixed labels, then QTYPE/QCLASS.
std::vector<u8> query(const char* host, u16 id)
{
    u8 buffer[kMaxDnsMessage];
    usize size = 0;
    if (!buildDnsQuery(host, id, buffer, &size)) {
        return {};
    }
    return std::vector<u8>(buffer, buffer + size);
}

}  // namespace

TEST(a_query_is_a_header_a_name_in_labels_and_a_type)
{
    const std::vector<u8> q = query("mc.example.org", 0x1234);
    // 12 header + (1+2) + (1+7) + (1+3) + 1 terminator + 4.
    CHECK_EQ(q.size(), usize(12 + 3 + 8 + 4 + 1 + 4));

    CHECK_EQ(int(q[0]), 0x12);
    CHECK_EQ(int(q[1]), 0x34);
    // Recursion desired, and one question.
    CHECK_EQ(int(q[2]), 0x01);
    CHECK_EQ(int(q[3]), 0x00);
    CHECK_EQ(int(q[5]), 1);
    CHECK_EQ(int(q[6]), 0);
    CHECK_EQ(int(q[7]), 0);

    CHECK_EQ(int(q[12]), 2);
    CHECK_EQ(int(q[13]), 'm');
    CHECK_EQ(int(q[15]), 7);
    CHECK_EQ(int(q[16]), 'e');
    CHECK_EQ(int(q[23]), 3);
    CHECK_EQ(int(q[27]), 0);  // the root label

    // QTYPE 1 (A), QCLASS 1 (IN).
    CHECK_EQ(int(q[q.size() - 4]), 0);
    CHECK_EQ(int(q[q.size() - 3]), 1);
    CHECK_EQ(int(q[q.size() - 2]), 0);
    CHECK_EQ(int(q[q.size() - 1]), 1);

    // A trailing dot is the same name.
    CHECK(query("mc.example.org.", 1) == query("mc.example.org", 1));
}

TEST(a_name_the_format_cannot_hold_is_refused_rather_than_truncated)
{
    u8 buffer[kMaxDnsMessage];
    usize size = 0;
    CHECK(!buildDnsQuery("", 1, buffer, &size));
    CHECK(!buildDnsQuery("mc..example.org", 1, buffer, &size));
    CHECK(!buildDnsQuery(std::string(64, 'a').c_str(), 1, buffer, &size));
    // 255 bytes is the ceiling on the whole name.
    CHECK(!buildDnsQuery(std::string(300, 'a').c_str(), 1, buffer, &size));
}

namespace {

// A reply: the question echoed, then `answers` records. The name in each answer
// is a compression pointer at the question, which is what every real resolver
// sends and is the case a parser gets wrong.
std::vector<u8> reply(const char* host, u16 id, u16 flags,
                      const std::vector<std::vector<u8>>& records)
{
    std::vector<u8> q = query(host, id);
    q[2] = u8(flags >> 8);
    q[3] = u8(flags & 0xFF);
    q[6] = u8(records.size() >> 8);
    q[7] = u8(records.size() & 0xFF);
    for (const std::vector<u8>& record : records) {
        q.insert(q.end(), record.begin(), record.end());
    }
    return q;
}

std::vector<u8> record(u16 type, const std::vector<u8>& rdata)
{
    std::vector<u8> out = {0xC0, 0x0C};  // a pointer at offset 12, the question
    out.push_back(u8(type >> 8));
    out.push_back(u8(type & 0xFF));
    out.push_back(0);
    out.push_back(1);  // IN
    for (int i = 0; i < 4; ++i) {
        out.push_back(0);  // TTL
    }
    out.push_back(u8(rdata.size() >> 8));
    out.push_back(u8(rdata.size() & 0xFF));
    out.insert(out.end(), rdata.begin(), rdata.end());
    return out;
}

}  // namespace

TEST(the_first_a_record_is_the_answer_even_behind_a_cname)
{
    u32 address = 0;
    const std::vector<u8> plain =
        reply("mc.example.org", 0x1234, 0x8180, {record(1, {192, 168, 1, 20})});
    CHECK(parseDnsAnswer(plain.data(), plain.size(), 0x1234, &address));
    CHECK_EQ(address, u32(0xC0A80114));

    // **A CNAME in front of it**, which is what a hosted server answers with
    // and is the reason the walk skips records rather than parsing names.
    const std::vector<u8> chained = reply(
        "mc.example.org", 0x1234, 0x8180,
        {record(5, {3, 'w', 'w', 'w', 0}), record(1, {10, 0, 0, 7})});
    CHECK(parseDnsAnswer(chained.data(), chained.size(), 0x1234, &address));
    CHECK_EQ(address, u32(0x0A000007));
}

TEST(an_answer_that_is_not_an_answer_is_refused)
{
    u32 address = 0;
    const std::vector<u8> good =
        reply("mc.example.org", 0x1234, 0x8180, {record(1, {1, 2, 3, 4})});

    // Somebody else's reply: the id does not match.
    CHECK(!parseDnsAnswer(good.data(), good.size(), 0x9999, &address));

    // A query rather than a response.
    const std::vector<u8> notReply =
        reply("mc.example.org", 0x1234, 0x0100, {record(1, {1, 2, 3, 4})});
    CHECK(!parseDnsAnswer(notReply.data(), notReply.size(), 0x1234, &address));

    // NXDOMAIN -- an answer, and a no.
    const std::vector<u8> missing = reply("mc.example.org", 0x1234, 0x8183, {});
    CHECK(!parseDnsAnswer(missing.data(), missing.size(), 0x1234, &address));

    // A CNAME the server did not follow: answers, but nothing this can use.
    const std::vector<u8> onlyCname =
        reply("mc.example.org", 0x1234, 0x8180, {record(5, {3, 'w', 'w', 'w', 0})});
    CHECK(!parseDnsAnswer(onlyCname.data(), onlyCname.size(), 0x1234, &address));

    // **Every prefix of a good reply**, which is the shape a hostile or simply
    // broken packet arrives in. None of them may read past the end, and the
    // sanitisers are what make this assertion worth writing.
    for (usize cut = 0; cut < good.size(); ++cut) {
        parseDnsAnswer(good.data(), cut, 0x1234, &address);
    }
}

TEST(with_no_name_server_source_there_are_no_name_servers)
{
    // The host build installs none: its libc resolves long before the
    // cascade reaches ours. See core/net/dns.hpp.
    setNameServerSource(nullptr, nullptr);
    u32 servers[kMaxNameServers];
    CHECK_EQ(nameServers(servers, kMaxNameServers), 0);

    u32 address = 0;
    CHECK(!resolveViaDns("mc.example.org", 10, &address));

    // And one that is installed is asked, and its answer bounded by `max`.
    static const u32 table[] = {0x08080808, 0x01010101, 0xC0A80101};
    setNameServerSource(
        [](void*, u32* out, int max) {
            const int count = max < 3 ? max : 3;
            for (int i = 0; i < count; ++i) {
                out[i] = table[i];
            }
            return count;
        },
        nullptr);
    CHECK_EQ(nameServers(servers, kMaxNameServers), 3);
    CHECK_EQ(servers[0], u32(0x08080808));
    CHECK_EQ(nameServers(servers, 1), 1);
    setNameServerSource(nullptr, nullptr);
}
