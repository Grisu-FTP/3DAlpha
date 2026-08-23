#include "framework.hpp"
#include "texture_support.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/jar_import.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/zip_archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mc;
using mc::test::makeTerrainPng;
using mc::test::TestZip;
using texture::ImportResult;
using texture::PackError;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_jar_XXXXXX");
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

std::vector<u8> classBytes(int n)
{
    std::vector<u8> bytes;
    for (int i = 0; i < n; ++i) {
        bytes.push_back(u8(0xCA + i));
    }
    return bytes;
}

// A jar shaped like the real one: a texture tree, a lot of class files, a
// META-INF, and both compression methods with and without data descriptors.
std::vector<u8> syntheticJar()
{
    TestZip jar;
    jar.add("META-INF/MANIFEST.MF", classBytes(60), /*deflate=*/true, /*dataDescriptor=*/true);
    jar.add("META-INF/thing.png", classBytes(40), /*deflate=*/false, /*dataDescriptor=*/false);
    jar.add("net/minecraft/client/Minecraft.class", classBytes(400), true, true);
    jar.add("a.class", classBytes(120), true, true);
    jar.add("terrain.png", makeTerrainPng(256), /*deflate=*/true, /*dataDescriptor=*/true);
    jar.add("char.png", classBytes(80), /*deflate=*/false, /*dataDescriptor=*/false);
    jar.add("gui/items.png", classBytes(200), /*deflate=*/true, /*dataDescriptor=*/true);
    jar.add("mob/cow.png", classBytes(90), /*deflate=*/false, /*dataDescriptor=*/false);
    jar.add("misc/gear.png", classBytes(50), true, true);
    jar.add("sound/step/grass1.ogg", classBytes(300), true, true);
    return jar.finish();
}

TEST(imports_only_the_textures)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string jarPath = dir.at("minecraft.jar");
    fs.writeFileAtomic(jarPath.c_str(), syntheticJar());

    const std::string packsDir = dir.at("packs");
    const ImportResult result = texture::importJar(fs, jarPath, packsDir);

    CHECK_EQ(int(result.error), int(PackError::Ok));
    CHECK(result.ok());
    // Six .png entries, minus the one under META-INF.
    CHECK_EQ(result.copied, 5);
    CHECK_EQ(result.skipped, 5);
    CHECK_EQ(result.outPath, packsDir + "/minecraft.zip");

    std::vector<u8> written;
    CHECK(fs.readFile(result.outPath.c_str(), &written, 16u << 20));

    texture::ZipArchive pack;
    CHECK_EQ(int(pack.open(written)), int(texture::ZipError::Ok));
    CHECK_EQ(pack.entries().size(), usize(5));

    CHECK(pack.find("terrain.png") != nullptr);
    CHECK(pack.find("gui/items.png") != nullptr);
    CHECK(pack.find("mob/cow.png") != nullptr);
    // No code, and nothing out of META-INF -- including a png that was in there.
    CHECK(pack.find("a.class") == nullptr);
    CHECK(pack.find("META-INF/MANIFEST.MF") == nullptr);
    CHECK(pack.find("META-INF/thing.png") == nullptr);
    CHECK(pack.find("sound/step/grass1.ogg") == nullptr);
}

// Verbatim, both ways: an entry that was deflated in the jar stays deflated in
// the pack, an entry that was stored stays stored, and both decompress to the
// bytes the jar held. That is what makes the import one pass with no codec in
// it at all.
TEST(entries_are_copied_without_being_recompressed)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::vector<u8> jarBytes = syntheticJar();
    const std::string jarPath = dir.at("src.jar");
    fs.writeFileAtomic(jarPath.c_str(), jarBytes);

    const std::string packsDir = dir.at("packs");
    const ImportResult result = texture::importJar(fs, jarPath, packsDir);
    CHECK(result.ok());

    std::vector<u8> written;
    CHECK(fs.readFile(result.outPath.c_str(), &written, 16u << 20));

    texture::ZipArchive source;
    texture::ZipArchive pack;
    CHECK_EQ(int(source.open(jarBytes)), int(texture::ZipError::Ok));
    CHECK_EQ(int(pack.open(written)), int(texture::ZipError::Ok));

    for (const texture::ZipEntry& out : pack.entries()) {
        const texture::ZipEntry* in = source.find(out.name);
        CHECK(in != nullptr);
        CHECK_EQ(int(out.method), int(in->method));
        CHECK_EQ(out.crc, in->crc);
        CHECK_EQ(out.compressedSize, in->compressedSize);
        CHECK_EQ(out.uncompressedSize, in->uncompressedSize);

        std::vector<u8> a;
        std::vector<u8> b;
        CHECK_EQ(int(source.read(*in, &a)), int(texture::ZipError::Ok));
        CHECK_EQ(int(pack.read(out, &b)), int(texture::ZipError::Ok));
        CHECK(a == b);
    }
    // Both framings really were present, or this proved nothing.
    bool sawStored = false;
    bool sawDeflated = false;
    for (const texture::ZipEntry& out : pack.entries()) {
        sawStored = sawStored || out.method == 0;
        sawDeflated = sawDeflated || out.method == 8;
    }
    CHECK(sawStored);
    CHECK(sawDeflated);
}

// The import is only a success if the *written file* re-opens and its
// terrain.png decodes. Nothing else makes it honest to then offer to delete the
// jar the player gave us.
TEST(a_successful_import_is_a_pack_that_loads)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string jarPath = dir.at("ok.jar");
    fs.writeFileAtomic(jarPath.c_str(), syntheticJar());

    const std::string packsDir = dir.at("packs");
    const ImportResult result = texture::importJar(fs, jarPath, packsDir);
    CHECK(result.ok());

    texture::AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, result.outPath, &atlas)), int(PackError::Ok));
    CHECK_EQ(atlas.sourceEdge, 256);

    // And it shows up as a pack in the folder it was written to.
    std::vector<texture::PackEntry> packs;
    texture::listPacks(fs, packsDir, &packs);
    CHECK_EQ(packs.size(), usize(2));
    CHECK_EQ(packs[1].name, std::string("ok.zip"));
    CHECK(packs[1].hasTerrain);
}

// A jar whose terrain.png will not decode must not be reported as a success,
// because the caller is about to offer to delete the source on the strength of
// that word.
TEST(a_jar_with_a_broken_terrain_is_not_a_success)
{
    TempDir dir;
    io::PosixFileSystem fs;

    TestZip jar;
    jar.add("terrain.png", std::vector<u8>(500, 0x7E), /*deflate=*/true,
            /*dataDescriptor=*/true);
    jar.add("char.png", std::vector<u8>(40, 0x10), false, false);

    const std::string jarPath = dir.at("broken.jar");
    fs.writeFileAtomic(jarPath.c_str(), jar.finish());

    const ImportResult result = texture::importJar(fs, jarPath, dir.at("packs"));
    CHECK(!result.ok());
    CHECK_EQ(int(result.error), int(PackError::BadPng));
    // The file was still written -- reporting the reason beats leaving nothing
    // behind for a maintainer to look at -- but the caller must not treat this
    // as a pack, and must not offer to delete the jar.
}

TEST(a_jar_with_no_textures_is_refused)
{
    TempDir dir;
    io::PosixFileSystem fs;

    TestZip jar;
    jar.add("a.class", classBytes(50), true, true);
    jar.add("META-INF/MANIFEST.MF", classBytes(20), false, false);

    const std::string jarPath = dir.at("empty.jar");
    fs.writeFileAtomic(jarPath.c_str(), jar.finish());

    const ImportResult result = texture::importJar(fs, jarPath, dir.at("packs"));
    CHECK(!result.ok());
    CHECK_EQ(result.copied, 0);
    CHECK(result.outPath.empty());
}

TEST(what_is_not_a_jar_is_refused_without_writing_anything)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string jarPath = dir.at("junk.jar");
    fs.writeFileAtomic(jarPath.c_str(), std::vector<u8>(400, 0x5A));

    const std::string packsDir = dir.at("packs");
    const ImportResult junk = texture::importJar(fs, jarPath, packsDir);
    CHECK_EQ(int(junk.error), int(PackError::NotAPack));
    CHECK(!fs.exists(packsDir.c_str()));

    const ImportResult missing = texture::importJar(fs, dir.at("nope.jar"), packsDir);
    CHECK_EQ(int(missing.error), int(PackError::NotFound));
}

// The pack file's name comes from the jar's, through the same FAT rule a world
// name goes through, so a download with a colon or a trailing dot in its name
// still lands on the card.
TEST(the_pack_name_comes_from_the_jar_name)
{
    CHECK_EQ(texture::packNameForJar("sdmc:/3dalpha/packs/minecraft.jar"),
             std::string("minecraft.zip"));
    CHECK_EQ(texture::packNameForJar("/a/b/minecraft-a1.1.2_01-client.jar"),
             std::string("minecraft-a1.1.2_01-client.zip"));
    CHECK_EQ(texture::packNameForJar("weird:name.jar"), std::string("weirdname.zip"));
    CHECK_EQ(texture::packNameForJar("no-extension"), std::string("no-extension.zip"));
    CHECK(texture::packNameForJar("...").empty());
}

}  // namespace
