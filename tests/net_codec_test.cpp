// Protocol 2's wire codec: Java's modified UTF-8, the packet table, and a
// parser that has to cope with a packet arriving a byte at a time.
//
// The byte strings below are written out by hand from the classes' own
// `a(DataOutputStream)` bodies in the a1.1.2 client and the 0.2.1 server, not
// produced by this encoder, so a round trip cannot pass by agreeing with itself.

#include "framework.hpp"

#include "core/net/packets.hpp"
#include "core/net/wire.hpp"

#include <string>
#include <vector>

using namespace mc;
using namespace mc::net;

namespace {

std::vector<u8> bytesOf(std::initializer_list<int> values)
{
    std::vector<u8> out;
    for (const int v : values) {
        out.push_back(u8(v));
    }
    return out;
}

}  // namespace

TEST(modified_utf8_writes_nul_as_two_bytes_and_astral_characters_as_surrogates)
{
    std::vector<u8> out;
    encodeModifiedUtf8(std::string("a\0b", 3), &out);
    CHECK(out == bytesOf({'a', 0xC0, 0x80, 'b'}));

    // U+1F600 is D83D DE00 in UTF-16, and writeUTF writes each half as three
    // bytes -- six in all, where real UTF-8 would use four.
    out.clear();
    encodeModifiedUtf8("\xF0\x9F\x98\x80", &out);
    CHECK(out == bytesOf({0xED, 0xA0, 0xBD, 0xED, 0xB8, 0x80}));

    std::string back;
    CHECK(decodeModifiedUtf8(out.data(), out.size(), &back));
    CHECK(back == "\xF0\x9F\x98\x80");

    // The section sign every server colour code starts with.
    out.clear();
    encodeModifiedUtf8("\xC2\xA7" "7hi", &out);
    CHECK(out == bytesOf({0xC2, 0xA7, '7', 'h', 'i'}));
}

TEST(modified_utf8_refuses_what_readUTF_refuses)
{
    std::string text;
    const std::vector<u8> fourByte = bytesOf({0xF0, 0x9F, 0x98, 0x80});
    CHECK(!decodeModifiedUtf8(fourByte.data(), fourByte.size(), &text));
    const std::vector<u8> truncated = bytesOf({'a', 0xE2, 0x82});
    CHECK(!decodeModifiedUtf8(truncated.data(), truncated.size(), &text));
    // A lone surrogate is legal Java and becomes the replacement character.
    const std::vector<u8> lone = bytesOf({0xED, 0xA0, 0xBD, 'x'});
    CHECK(decodeModifiedUtf8(lone.data(), lone.size(), &text));
    CHECK(text == "\xEF\xBF\xBDx");
}

TEST(a_login_is_the_protocol_version_the_name_and_the_word_Password)
{
    std::vector<u8> out;
    CHECK(encodePacket(makeLogin("Grisu", 2), &out));
    CHECK(out
          == bytesOf({0x01, 0, 0, 0, 2, 0, 5, 'G', 'r', 'i', 's', 'u', 0, 8, 'P', 'a', 's', 's',
                      'w', 'o', 'r', 'd'}));
}

TEST(position_towards_the_server_is_x_feet_eye_z)
{
    std::vector<u8> out;
    CHECK(encodePacket(makePosition(1.0, 64.0, 65.62, -2.0, true), &out));
    CHECK_EQ(out.size(), usize(1 + 8 * 4 + 1));

    Packet back;
    usize consumed = 0;
    CHECK(parsePacket(out.data(), out.size(), &back, &consumed) == ParseResult::Ok);
    CHECK_EQ(consumed, out.size());
    CHECK_EQ(back.realCount, u8(4));
    CHECK_EQ(back.real(0), 1.0);
    CHECK_EQ(back.real(1), 64.0);
    CHECK_EQ(back.real(2), 65.62);
    CHECK_EQ(back.real(3), -2.0);
    CHECK_EQ(back.integer(0), i64(1));
}

TEST(every_packet_parses_back_to_what_was_encoded_even_a_byte_at_a_time)
{
    std::vector<Packet> packets;
    packets.push_back(makeKeepAlive());
    packets.push_back(makeHandshake("-"));
    packets.push_back(makeChat("\xC2\xA7" "e<Grisu> hello"));
    packets.push_back(makePositionLook(8.5, 70.0, 71.62, -8.5, 90.0f, -12.5f, false));
    packets.push_back(makeDig(1, -100, 63, 2000000, 5));
    packets.push_back(makePlace(-1, 3, 255, -4, 0));
    packets.push_back(makeHoldingChange(276));
    WireStack stacks[3];
    stacks[0] = WireStack{1, 64, 0};
    stacks[2] = WireStack{261, 1, 17};
    packets.push_back(makeInventory(-1, stacks, 3));
    packets.push_back(makePickupSpawn(7, 4, 3, -320, 2048, 96, 10, -20, 30));
    packets.push_back(makeDisconnect("Quitting"));

    Packet chunk;
    chunk.reset(packet::MapChunk);
    for (int v : {160, 0, -32, 15, 127, 15}) chunk.pushInt(v);
    chunk.bytes = bytesOf({0x78, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01});
    packets.push_back(chunk);

    Packet multi;
    multi.reset(packet::MultiBlockChange);
    multi.pushInt(-1);
    multi.pushInt(3);
    multi.changeCoords = {i16((15 << 12) | (0 << 8) | 64), i16((1 << 12) | (14 << 8) | 127)};
    multi.changeIds = {0, 20};
    multi.changeData = {0, 0};
    packets.push_back(multi);

    std::vector<u8> stream;
    for (const Packet& p : packets) {
        CHECK(encodePacket(p, &stream));
    }

    // Fed one byte at a time, which is the worst a socket can do.
    std::vector<u8> buffer;
    std::vector<Packet> parsed;
    for (const u8 b : stream) {
        buffer.push_back(b);
        for (;;) {
            Packet p;
            usize consumed = 0;
            const ParseResult r = parsePacket(buffer.data(), buffer.size(), &p, &consumed);
            if (r == ParseResult::NeedMore) {
                break;
            }
            CHECK(r == ParseResult::Ok);
            buffer.erase(buffer.begin(), buffer.begin() + std::ptrdiff_t(consumed));
            parsed.push_back(std::move(p));
        }
    }
    CHECK(buffer.empty());
    CHECK_EQ(parsed.size(), packets.size());

    for (usize i = 0; i < packets.size(); ++i) {
        std::vector<u8> a;
        std::vector<u8> b;
        CHECK(encodePacket(packets[i], &a));
        CHECK(encodePacket(parsed[i], &b));
        CHECK(a == b);
    }
    CHECK(parsed[2].text(0) == "\xC2\xA7" "e<Grisu> hello");
    CHECK_EQ(parsed[7].stacks.size(), usize(3));
    CHECK(parsed[7].stacks[1].empty());
    CHECK_EQ(parsed[7].stacks[2].damage, i16(17));
}

TEST(a_block_change_reads_its_y_and_its_id_unsigned)
{
    // et/li: int x, read() y, int z, read() id, read() metadata.
    const std::vector<u8> wire = bytesOf({0x35, 0xFF, 0xFF, 0xFF, 0xF0, 200, 0, 0, 0, 5, 0xFE, 3});
    Packet p;
    usize consumed = 0;
    CHECK(parsePacket(wire.data(), wire.size(), &p, &consumed) == ParseResult::Ok);
    CHECK_EQ(p.integer(0), i64(-16));
    CHECK_EQ(p.integer(1), i64(200));
    CHECK_EQ(p.integer(2), i64(5));
    CHECK_EQ(p.integer(3), i64(254));
    CHECK_EQ(p.integer(4), i64(3));
}

TEST(an_unknown_id_and_a_negative_length_are_not_waited_for)
{
    Packet p;
    usize consumed = 0;
    // 0x08 is Update Health, which arrives at protocol 5 and is in no table
    // here. (0x07 used to stand in for this and no longer can: a 3DAlpha host
    // sends it -- see `packet::UseEntity` and protocol-a1.1.2.md.)
    const std::vector<u8> unknown = bytesOf({0x08, 0, 0, 0, 0});
    CHECK(parsePacket(unknown.data(), unknown.size(), &p, &consumed) == ParseResult::UnknownId);

    const std::vector<u8> negative =
        bytesOf({0x33, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 15, 127, 15, 0xFF, 0xFF, 0xFF, 0xFF});
    CHECK(parsePacket(negative.data(), negative.size(), &p, &consumed) == ParseResult::Malformed);

    const std::vector<u8> badString = bytesOf({0xFF, 0, 2, 0xF0, 0x80});
    CHECK(parsePacket(badString.data(), badString.size(), &p, &consumed) == ParseResult::Malformed);

    // A truncated one is simply not here yet.
    const std::vector<u8> partial = bytesOf({0x04, 0, 0, 0});
    CHECK(parsePacket(partial.data(), partial.size(), &p, &consumed) == ParseResult::NeedMore);
}

TEST(the_table_holds_the_jars_thirty_three_packets_and_the_four_additions)
{
    int registered = 0;
    for (int id = 0; id < 256; ++id) {
        if (shapeOf(u8(id)) != nullptr) {
            ++registered;
        }
    }
    // **Thirty-three is the jar's number** -- `fn` in the client and `hp` in the
    // server both register exactly that many -- and the four past it are ours,
    // listed here so the table can never grow quietly. A 3DAlpha host sends
    // `0x07` so a guest can hit an animal, `0x26` so a guest sees the animal
    // was hit or died, and `0x13` and `0x09` so a player's crouch, death and
    // respawn are seen by the others -- protocol 2 has no way to say any of it,
    // and none of it is ever sent to a Java server. See protocol-a1.1.2.md,
    // "What a 3DAlpha host adds to protocol 2".
    CHECK_EQ(registered, 37);

    const PacketShape* status = shapeOf(packet::EntityStatus);
    CHECK(status != nullptr);
    CHECK_EQ(int(status->fieldCount), 2);
    CHECK(status->fields[0] == FieldType::Int);
    CHECK(status->fields[1] == FieldType::Byte);

    // b1.2's `on` and `kr`, at their own ids and shapes.
    const PacketShape* action = shapeOf(packet::EntityAction);
    CHECK(action != nullptr);
    CHECK_EQ(int(action->fieldCount), 2);
    CHECK(action->fields[0] == FieldType::Int);
    CHECK(action->fields[1] == FieldType::Byte);
    const PacketShape* respawn = shapeOf(packet::Respawn);
    CHECK(respawn != nullptr);
    CHECK_EQ(int(respawn->fieldCount), 0);

    const PacketShape* ours = shapeOf(packet::UseEntity);
    CHECK(ours != nullptr);
    CHECK_EQ(int(ours->fieldCount), 3);
    CHECK(ours->fields[0] == FieldType::Int);
    CHECK(ours->fields[1] == FieldType::Int);
    CHECK(ours->fields[2] == FieldType::Bool);

    // Added in later protocols; a server for this one never sends them, and
    // nothing here has a reason to add them.
    CHECK(shapeOf(0x08) == nullptr);
    CHECK(shapeOf(0x1C) == nullptr);
    CHECK(shapeOf(0x3C) == nullptr);
    CHECK(shapeOf(0x64) == nullptr);
}
