#include "framework.hpp"
#include "texture_support.hpp"

#include "core/texture/zip_archive.hpp"
#include "core/texture/zip_builder.hpp"

#include <string>

using namespace mc;
using mc::test::TestZip;
using texture::ZipArchive;
using texture::ZipBuilder;
using texture::ZipEntry;
using texture::ZipError;

namespace {

std::vector<u8> textBytes(const char* text)
{
    return std::vector<u8>(reinterpret_cast<const u8*>(text),
                           reinterpret_cast<const u8*>(text) + std::string(text).size());
}

// Long enough to actually compress, so the deflate path is exercised rather
// than degenerating into a stored block.
std::vector<u8> compressible()
{
    std::vector<u8> data;
    for (int i = 0; i < 4000; ++i) {
        data.push_back(u8('a' + (i % 7)));
    }
    return data;
}

TEST(reads_stored_and_deflated_entries)
{
    TestZip zip;
    zip.add("terrain.png", textBytes("stored bytes"), /*deflate=*/false,
            /*dataDescriptor=*/false);
    zip.add("gui/gui.png", compressible(), /*deflate=*/true, /*dataDescriptor=*/false);
    const std::vector<u8> bytes = zip.finish();

    ZipArchive archive;
    CHECK_EQ(int(archive.open(bytes)), int(ZipError::Ok));
    CHECK_EQ(archive.entries().size(), usize(2));

    const ZipEntry* stored = archive.find("terrain.png");
    CHECK(stored != nullptr);
    CHECK_EQ(int(stored->method), 0);

    std::vector<u8> out;
    CHECK_EQ(int(archive.read(*stored, &out)), int(ZipError::Ok));
    CHECK(out == textBytes("stored bytes"));

    const ZipEntry* deflated = archive.find("gui/gui.png");
    CHECK(deflated != nullptr);
    CHECK_EQ(int(deflated->method), 8);
    CHECK(deflated->compressedSize < deflated->uncompressedSize);
    CHECK_EQ(int(archive.read(*deflated, &out)), int(ZipError::Ok));
    CHECK(out == compressible());

    CHECK(archive.find("nothing.png") == nullptr);
}

// **The measurement this whole reader is built around.** 497 of the 538 entries
// in a real a1.1.2 client jar set general-purpose flag bit 3, which zeroes the
// CRC and both sizes in the local header. A reader that trusts local headers
// gets zero-length entries for 92 % of that jar; this one reads the central
// directory and only walks the local header far enough to skip its name and
// extra fields.
TEST(reads_an_entry_whose_local_header_lies)
{
    const std::vector<u8> content = compressible();

    TestZip zip;
    zip.add("terrain.png", content, /*deflate=*/true, /*dataDescriptor=*/true);
    const std::vector<u8> bytes = zip.finish();

    ZipArchive archive;
    CHECK_EQ(int(archive.open(bytes)), int(ZipError::Ok));

    const ZipEntry* entry = archive.find("terrain.png");
    CHECK(entry != nullptr);
    CHECK_EQ(entry->uncompressedSize, u32(content.size()));

    std::vector<u8> out;
    CHECK_EQ(int(archive.read(*entry, &out)), int(ZipError::Ok));
    CHECK(out == content);
}

// The bytes the jar importer copies: still compressed, exactly as stored, and
// the same length the directory promised.
TEST(raw_bytes_are_the_stored_form)
{
    const std::vector<u8> content = compressible();

    TestZip zip;
    zip.add("a.png", content, /*deflate=*/true, /*dataDescriptor=*/true);
    const std::vector<u8> bytes = zip.finish();

    ZipArchive archive;
    CHECK_EQ(int(archive.open(bytes)), int(ZipError::Ok));
    const ZipEntry* entry = archive.find("a.png");
    CHECK(entry != nullptr);

    const ConstByteSpan raw = archive.rawBytes(*entry);
    CHECK_EQ(raw.size(), usize(entry->compressedSize));
    CHECK(raw.size() < content.size());
}

TEST(a_directory_entry_is_not_a_file)
{
    TestZip zip;
    zip.add("gui/", {}, /*deflate=*/false, /*dataDescriptor=*/false);
    zip.add("gui/gui.png", textBytes("x"), /*deflate=*/false, /*dataDescriptor=*/false);
    const std::vector<u8> bytes = zip.finish();

    ZipArchive archive;
    CHECK_EQ(int(archive.open(bytes)), int(ZipError::Ok));
    CHECK_EQ(archive.entries().size(), usize(1));
    CHECK_EQ(archive.entries()[0].name, std::string("gui/gui.png"));
}

TEST(refuses_what_is_not_a_zip)
{
    ZipArchive archive;
    CHECK_EQ(int(archive.open(std::vector<u8>(100, 0x5A))), int(ZipError::NoEndRecord));
    CHECK_EQ(int(archive.open(std::vector<u8>())), int(ZipError::NoEndRecord));
}

// A pack is a file somebody downloaded. An entry named "../../boot.firm" must
// never reach the card, so the check is at the point of reading rather than at
// the point of writing -- there is only one reader and there could be several
// writers.
TEST(refuses_names_that_escape)
{
    CHECK(texture::isSafeZipName("terrain.png"));
    CHECK(texture::isSafeZipName("gui/items.png"));
    CHECK(texture::isSafeZipName("a..b.png"));   // not a component
    CHECK(texture::isSafeZipName("..png"));      // not a component either

    CHECK(!texture::isSafeZipName("../terrain.png"));
    CHECK(!texture::isSafeZipName("gui/../../x.png"));
    CHECK(!texture::isSafeZipName(".."));
    CHECK(!texture::isSafeZipName("/terrain.png"));
    CHECK(!texture::isSafeZipName("gui\\items.png"));
    CHECK(!texture::isSafeZipName("C:/terrain.png"));
    CHECK(!texture::isSafeZipName(""));

    TestZip zip;
    zip.add("../escape.png", textBytes("x"), /*deflate=*/false, /*dataDescriptor=*/false);
    ZipArchive archive;
    CHECK_EQ(int(archive.open(zip.finish())), int(ZipError::UnsafeName));
}

// What the importer does, end to end: read entries out of one archive and
// re-emit them into another without decompressing anything.
TEST(builder_round_trips_verbatim_entries)
{
    const std::vector<u8> big = compressible();

    TestZip source;
    source.add("terrain.png", big, /*deflate=*/true, /*dataDescriptor=*/true);
    source.add("char.png", textBytes("skin"), /*deflate=*/false, /*dataDescriptor=*/false);
    const std::vector<u8> sourceBytes = source.finish();

    ZipArchive in;
    CHECK_EQ(int(in.open(sourceBytes)), int(ZipError::Ok));

    ZipBuilder builder;
    for (const ZipEntry& entry : in.entries()) {
        CHECK(builder.addRaw(entry.name, entry.method, entry.crc, entry.uncompressedSize,
                             in.rawBytes(entry)));
    }
    builder.finish();
    CHECK_EQ(builder.entryCount(), usize(2));

    ZipArchive out;
    CHECK_EQ(int(out.open(builder.bytes())), int(ZipError::Ok));
    CHECK_EQ(out.entries().size(), usize(2));

    std::vector<u8> content;
    const ZipEntry* terrain = out.find("terrain.png");
    CHECK(terrain != nullptr);
    // The flag bit was dropped on the way out, so the rewritten local header
    // has to carry the real sizes instead.
    CHECK_EQ(int(out.read(*terrain, &content)), int(ZipError::Ok));
    CHECK(content == big);

    const ZipEntry* skin = out.find("char.png");
    CHECK(skin != nullptr);
    CHECK_EQ(int(out.read(*skin, &content)), int(ZipError::Ok));
    CHECK(content == textBytes("skin"));
}

TEST(builder_refuses_an_unsafe_name)
{
    ZipBuilder builder;
    const std::vector<u8> data = textBytes("x");
    CHECK(!builder.addStored("../x.png", data));
    CHECK(!builder.addStored("", data));
    CHECK(builder.addStored("ok.png", data));
    CHECK_EQ(builder.entryCount(), usize(1));
}

// A builder that computes its own CRC has to agree with a reader that checks
// one; the empty case is the one every hand-rolled CRC gets wrong.
TEST(builder_stores_an_empty_entry)
{
    ZipBuilder builder;
    CHECK(builder.addStored("empty.png", ConstByteSpan()));
    builder.finish();

    ZipArchive archive;
    CHECK_EQ(int(archive.open(builder.bytes())), int(ZipError::Ok));
    const ZipEntry* entry = archive.find("empty.png");
    CHECK(entry != nullptr);
    CHECK_EQ(entry->uncompressedSize, u32(0));

    std::vector<u8> out;
    CHECK_EQ(int(archive.read(*entry, &out)), int(ZipError::Ok));
    CHECK(out.empty());
}

}  // namespace
