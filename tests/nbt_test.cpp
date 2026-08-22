#include "framework.hpp"

#include "core/nbt/nbt.hpp"
#include "core/nbt/tree.hpp"
#include "core/nbt/writer.hpp"
#include "core/util/compress.hpp"

#include <string>
#include <vector>

using namespace mc;
using namespace mc::nbt;

namespace {

ConstByteSpan span(const std::vector<u8>& v)
{
    return ConstByteSpan(v.data(), v.size());
}

// A document exercising every tag type Alpha uses, plus the shapes that trip
// naive parsers: an empty list, a list of compounds, nested compounds, and a
// byte array whose contents could be mistaken for tag headers.
std::vector<u8> sampleDocument()
{
    std::vector<u8> out;
    Writer w(out);
    w.beginRoot("");
    w.beginCompound("Level");
    w.writeInt("xPos", -13);
    w.writeInt("zPos", 44);
    w.writeByte("TerrainPopulated", 1);
    w.writeLong("LastUpdate", 1234567890123LL);

    std::vector<u8> blocks(64);
    for (usize i = 0; i < blocks.size(); ++i) {
        blocks[i] = static_cast<u8>(i);
    }
    w.writeByteArray("Blocks", span(blocks));

    w.beginList("Entities", TagType::Compound);
    w.beginListElementCompound();
    w.writeString("id", "Pig");
    w.beginList("Pos", TagType::Double);
    w.listDouble(-13.5);
    w.listDouble(64.0);
    w.listDouble(44.25);
    w.endList();
    w.endCompound();
    w.endList();

    w.beginList("TileEntities", TagType::Compound);
    w.endList();

    w.beginCompound("Nested");
    w.writeFloat("value", 0.5f);
    w.writeShort("small", -300);
    w.endCompound();

    w.endCompound();
    w.endRoot();
    return out;
}

}  // namespace

TEST(writer_produces_readable_document)
{
    const std::vector<u8> doc = sampleDocument();
    Reader r(span(doc));
    std::string_view rootName;
    CHECK(r.enterRoot(&rootName));
    CHECK(rootName.empty());

    TagType type;
    std::string_view name;
    CHECK(r.nextField(&type, &name));
    CHECK(type == TagType::Compound);
    CHECK(name == "Level");
    CHECK(r.skipValue(TagType::Compound));
    CHECK(!r.nextField(&type, &name));  // root's TAG_End
    CHECK(r.ok());
}

TEST(reader_extracts_values)
{
    const std::vector<u8> doc = sampleDocument();
    Reader r(span(doc));
    CHECK(r.enterRoot());

    TagType type;
    std::string_view name;
    CHECK(r.nextField(&type, &name));
    CHECK(name == "Level");

    i32 xPos = 0;
    i32 zPos = 0;
    i64 lastUpdate = 0;
    usize blockCount = 0;
    bool sawEmptyList = false;

    while (r.nextField(&type, &name)) {
        if (name == "xPos") {
            xPos = r.intValue();
        } else if (name == "zPos") {
            zPos = r.intValue();
        } else if (name == "LastUpdate") {
            lastUpdate = r.longValue();
        } else if (name == "Blocks") {
            blockCount = r.byteArray().size();
        } else if (name == "TileEntities") {
            TagType elem;
            i32 count;
            CHECK(r.enterList(&elem, &count));
            CHECK_EQ(count, 0);
            sawEmptyList = true;
        } else {
            r.skipValue(type);
        }
    }

    CHECK(r.ok());
    CHECK_EQ(xPos, -13);
    CHECK_EQ(zPos, 44);
    CHECK_EQ(lastUpdate, 1234567890123LL);
    CHECK_EQ(blockCount, 64u);
    CHECK(sawEmptyList);
}

TEST(round_trip_is_semantically_identical)
{
    const std::vector<u8> original = sampleDocument();

    std::vector<u8> rewritten;
    CHECK(copyDocument(span(original), rewritten));

    std::string before;
    std::string after;
    CHECK(dump(span(original), before));
    CHECK(dump(span(rewritten), after));
    CHECK_EQ(after, before);

    // Our writer is deterministic and the copier preserves member order, so
    // this particular round trip is byte-exact too. If that ever stops being
    // true the dump comparison above is the one that matters.
    CHECK_EQ(rewritten.size(), original.size());
    CHECK(rewritten == original);
}

TEST(truncation_fails_cleanly_at_every_length)
{
    // Every prefix of a valid document must either parse as far as it goes and
    // report failure, or stop early -- never read out of bounds. Run under
    // sanitizers this is the parser's main defence against hostile files.
    const std::vector<u8> doc = sampleDocument();
    for (usize length = 0; length < doc.size(); ++length) {
        Reader r(ConstByteSpan(doc.data(), length));
        if (!r.enterRoot()) {
            continue;
        }
        TagType type;
        std::string_view name;
        while (r.nextField(&type, &name)) {
            r.skipValue(type);
        }
        // A strict prefix can never be a complete document.
        CHECK(!r.ok());
    }
}

TEST(corruption_fails_cleanly)
{
    // Flip one byte at a time and require that parsing either succeeds or
    // fails, but never crashes or hangs. Sanitizers turn this into a real
    // memory-safety test; without them it still catches infinite loops.
    std::vector<u8> doc = sampleDocument();
    for (usize i = 0; i < doc.size(); ++i) {
        const u8 saved = doc[i];
        doc[i] = static_cast<u8>(saved ^ 0xFF);

        Reader r(span(doc));
        if (r.enterRoot()) {
            TagType type;
            std::string_view name;
            while (r.nextField(&type, &name)) {
                r.skipValue(type);
            }
        }

        doc[i] = saved;
    }
    CHECK(true);
}

TEST(deeply_nested_input_is_rejected_not_crashed)
{
    // A hostile file can nest compounds far deeper than any real one. The
    // depth guard must turn that into a parse failure rather than a stack
    // overflow -- on the 3DS the main thread's stack is small.
    std::vector<u8> doc;
    doc.push_back(static_cast<u8>(TagType::Compound));
    doc.push_back(0);
    doc.push_back(0);  // root name ""
    for (int i = 0; i < 4096; ++i) {
        doc.push_back(static_cast<u8>(TagType::Compound));
        doc.push_back(0);
        doc.push_back(0);
    }

    Reader r(span(doc));
    CHECK(r.enterRoot());
    TagType type;
    CHECK(r.nextField(&type, nullptr));
    CHECK(!r.skipValue(type));
    CHECK(!r.ok());
}

TEST(list_of_end_type_with_nonzero_count_is_rejected)
{
    // TAG_End as an element type only makes sense for an empty list. Paired
    // with a count it describes elements of unknowable size, which a reader
    // cannot skip; accepting it would desynchronise the whole document.
    std::vector<u8> doc;
    doc.push_back(static_cast<u8>(TagType::Compound));
    doc.push_back(0);
    doc.push_back(0);
    doc.push_back(static_cast<u8>(TagType::List));
    doc.push_back(0);
    doc.push_back(1);
    doc.push_back('x');
    doc.push_back(static_cast<u8>(TagType::End));
    doc.push_back(0);
    doc.push_back(0);
    doc.push_back(0);
    doc.push_back(5);

    Reader r(span(doc));
    CHECK(r.enterRoot());
    TagType type;
    std::string_view name;
    CHECK(r.nextField(&type, &name));
    CHECK(type == TagType::List);
    CHECK(!r.enterList(nullptr, nullptr));
    CHECK(!r.ok());
}

TEST(gzip_round_trip)
{
    const std::vector<u8> doc = sampleDocument();

    std::vector<u8> compressed;
    CHECK(zip::compress(span(doc), compressed, zip::Wrapper::Gzip));
    CHECK(compressed.size() > 0);

    std::vector<u8> restored;
    CHECK(zip::decompress(span(compressed), restored, zip::Wrapper::Gzip));
    CHECK(restored == doc);
}

TEST(zlib_round_trip)
{
    const std::vector<u8> doc = sampleDocument();

    std::vector<u8> compressed;
    CHECK(zip::compress(span(doc), compressed, zip::Wrapper::Zlib));

    std::vector<u8> restored;
    CHECK(zip::decompress(span(compressed), restored, zip::Wrapper::Zlib));
    CHECK(restored == doc);
}

TEST(decompress_rejects_truncated_and_corrupt_streams)
{
    const std::vector<u8> doc = sampleDocument();
    std::vector<u8> compressed;
    CHECK(zip::compress(span(doc), compressed, zip::Wrapper::Gzip));

    std::vector<u8> restored;
    CHECK(!zip::decompress(ConstByteSpan(compressed.data(), compressed.size() / 2), restored,
                           zip::Wrapper::Gzip));
    CHECK(restored.empty());

    // Wrong wrapper: gzip bytes fed to the zlib decoder.
    restored.clear();
    CHECK(!zip::decompress(span(compressed), restored, zip::Wrapper::Zlib));
    CHECK(restored.empty());
}

TEST(decompress_honours_its_output_ceiling)
{
    // A zip bomb is the failure mode that matters here: a few hundred bytes on
    // an SD card must not be able to exhaust a 40 MB heap.
    std::vector<u8> zeros(1u << 20, 0);
    std::vector<u8> compressed;
    CHECK(zip::compress(span(zeros), compressed, zip::Wrapper::Gzip));
    CHECK(compressed.size() < 8192);

    std::vector<u8> restored;
    CHECK(!zip::decompress(span(compressed), restored, zip::Wrapper::Gzip, 64 * 1024));
    CHECK(restored.empty());

    CHECK(zip::decompress(span(compressed), restored, zip::Wrapper::Gzip, 2u << 20));
    CHECK_EQ(restored.size(), zeros.size());
}

TEST(decompress_appends_rather_than_replacing)
{
    // Packed-world sectors are decoded one after another into a single buffer.
    std::vector<u8> first{1, 2, 3};
    std::vector<u8> second{4, 5, 6};

    std::vector<u8> a;
    std::vector<u8> b;
    CHECK(zip::compress(span(first), a, zip::Wrapper::Raw));
    CHECK(zip::compress(span(second), b, zip::Wrapper::Raw));

    std::vector<u8> out;
    CHECK(zip::decompress(span(a), out, zip::Wrapper::Raw));
    CHECK(zip::decompress(span(b), out, zip::Wrapper::Raw));

    const std::vector<u8> expected{1, 2, 3, 4, 5, 6};
    CHECK(out == expected);
}
