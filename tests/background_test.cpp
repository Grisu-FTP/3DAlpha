#include "framework.hpp"
#include "texture_support.hpp"

#include "core/block/registry.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/background.hpp"

#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mc;
using mc::test::makeRgbaPng;
using mc::test::makeTerrainPng;
using mc::test::TestZip;
using texture::AtlasImage;
using texture::kBackgroundEdge;
using texture::kBackgroundShade;
using texture::PackError;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_bg_XXXXXX");
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

// What 0x404040 does to one channel.
u8 darkened(int value)
{
    return u8((u32(value) * kBackgroundShade) / 0xFF);
}

std::vector<u8> flatPng(int edge, u8 r, u8 g, u8 b)
{
    std::vector<u8> rgba(usize(edge) * usize(edge) * 4);
    for (usize i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    return makeRgbaPng(edge, edge, rgba);
}

const u8* texel(const std::vector<u8>& tile, int x, int y)
{
    return tile.data() + (usize(y) * kBackgroundEdge + usize(x)) * 4;
}

}  // namespace

TEST(background_uses_the_packs_dirt_darkened)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    TestZip zip;
    zip.add("terrain.png", makeTerrainPng(256), true, false);
    zip.add("dirt.png", flatPng(16, 120, 90, 60), true, false);
    const std::string path = dir.at("pack.zip");

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(path.c_str(), zip.finish()));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, path, &atlas)), int(PackError::Ok));

    std::vector<u8> tile;
    CHECK_EQ(int(texture::buildBackground(fs, path, atlas, &tile)), int(PackError::Ok));
    CHECK_EQ(int(tile.size()), int(texture::kBackgroundBytes));

    // Replicated to 32, so every texel is the same one darkened.
    for (int y = 0; y < kBackgroundEdge; y += 7) {
        for (int x = 0; x < kBackgroundEdge; x += 7) {
            CHECK_EQ(int(texel(tile, x, y)[0]), int(darkened(120)));
            CHECK_EQ(int(texel(tile, x, y)[1]), int(darkened(90)));
            CHECK_EQ(int(texel(tile, x, y)[2]), int(darkened(60)));
            CHECK_EQ(int(texel(tile, x, y)[3]), 255);
        }
    }
}

TEST(background_falls_back_to_the_atlas_dirt_tile)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    // A pack with terrain.png and nothing else, which is all the pack screen
    // demands of one.
    TestZip zip;
    zip.add("terrain.png", makeTerrainPng(256), true, false);
    const std::string path = dir.at("bare.zip");

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(path.c_str(), zip.finish()));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, path, &atlas)), int(PackError::Ok));

    std::vector<u8> tile;
    CHECK_EQ(int(texture::buildBackground(fs, path, atlas, &tile)), int(PackError::Ok));

    // makeTerrainPng colours tile (tx, ty) with (tx * 16, ty * 16, 200), and
    // the tile taken is whichever one the block table gives dirt.
    const int dirt = int(block::def(block::BlockId(mcver::Block::Dirt)).texture);
    CHECK_EQ(int(texel(tile, 3, 3)[0]), int(darkened((dirt % 16) * 16)));
    CHECK_EQ(int(texel(tile, 3, 3)[1]), int(darkened((dirt / 16) * 16)));
    CHECK_EQ(int(texel(tile, 3, 3)[2]), int(darkened(200)));
}

TEST(background_dev_art_comes_from_the_generated_atlas)
{
    io::PosixFileSystem fs;

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, "", &atlas)), int(PackError::Ok));

    // Dev Art has no dirt.png and no pack path at all; the tile still exists.
    std::vector<u8> tile;
    CHECK_EQ(int(texture::buildBackground(fs, "", atlas, &tile)), int(PackError::Ok));
    CHECK_EQ(int(tile.size()), int(texture::kBackgroundBytes));

    // Darkened, so nothing in it can be brighter than the shade allows.
    for (usize i = 0; i + 3 < tile.size(); i += 4) {
        CHECK(tile[i] <= kBackgroundShade);
        CHECK(tile[i + 1] <= kBackgroundShade);
        CHECK(tile[i + 2] <= kBackgroundShade);
    }
}

TEST(background_needs_something_to_draw)
{
    io::PosixFileSystem fs;
    AtlasImage empty;
    std::vector<u8> tile;
    CHECK_EQ(int(texture::buildBackground(fs, "", empty, &tile)), int(PackError::NotFound));
    CHECK(tile.empty());
}

TEST(background_ignores_a_dirt_png_it_cannot_use)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');

    // Not square, so it is not a tile. The atlas answers instead rather than
    // the menu losing its backdrop over it.
    std::vector<u8> oblong(usize(32) * 16 * 4, 0xFF);
    TestZip zip;
    zip.add("terrain.png", makeTerrainPng(256), true, false);
    zip.add("dirt.png", makeRgbaPng(32, 16, oblong), true, false);
    const std::string path = dir.at("odd.zip");

    io::PosixFileSystem fs;
    CHECK(fs.writeFileAtomic(path.c_str(), zip.finish()));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, path, &atlas)), int(PackError::Ok));

    std::vector<u8> tile;
    CHECK_EQ(int(texture::buildBackground(fs, path, atlas, &tile)), int(PackError::Ok));

    const int dirt = int(block::def(block::BlockId(mcver::Block::Dirt)).texture);
    CHECK_EQ(int(texel(tile, 0, 0)[2]), int(darkened(200)));
    CHECK_EQ(int(texel(tile, 0, 0)[0]), int(darkened((dirt % 16) * 16)));
}
