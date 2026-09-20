// Which player skins the card has, and what each one turns into.
//
// Three groups of claim, and they fail in different ways:
//
//   * **The list.** Default is always row 0 and always present; a pack with no
//     `char.png` is not offered; a file that is not a skin -- wrong shape,
//     wrong extension, not a PNG at all -- is left off rather than offered and
//     then failing at the moment it is picked.
//   * **The key**, which is what `3ds.ini` carries. It has to survive a
//     round trip through the settings file and it has to name a file without
//     listing anything, because that is what `ensureAtlas` does at boot.
//   * **The image.** A 64 x 64 keeps its top half; a narrow skin is recognised
//     as one and is still drawn on the wide arm, because the narrow body is
//     1.8's and this is a1.1.2.
//
// Real files, for the reason `pack_test.cpp` gives: the bugs at this layer are
// paths built wrong and listings that depend on readdir order, and neither
// exists against a stub.

#include "framework.hpp"
#include "texture_support.hpp"

#include "core/io/posix_file_system.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/skin_list.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mc;
using mc::test::makeRgbaPng;
using mc::test::makeTerrainPng;
using mc::test::TestZip;
using texture::SkinEntry;
using texture::SkinModel;
using texture::SkinSource;

namespace {

struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_skin_XXXXXX");
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

    std::string at(const char* leaf) const { return std::string(path) + "/" + leaf; }
};

// A skin of `width` x `height`, opaque everywhere unless `slim`, in which case
// the two columns a narrow arm never fills are transparent.
std::vector<u8> makeSkinPng(int width, int height, bool slim)
{
    std::vector<u8> rgba(usize(width) * usize(height) * 4, 0xC0);
    for (usize i = 3; i < rgba.size(); i += 4) {
        rgba[i] = 255;
    }
    if (slim) {
        const int scale = width / 64;
        for (int y = 20 * scale; y < 32 * scale && y < height; ++y) {
            for (int x = 54 * scale; x < 56 * scale && x < width; ++x) {
                rgba[(usize(y) * usize(width) + usize(x)) * 4 + 3] = 0;
            }
        }
    }
    return makeRgbaPng(width, height, rgba);
}

bool writeFile(io::FileSystem& fs, const std::string& path, const std::vector<u8>& bytes)
{
    return fs.writeFileAtomic(path.c_str(), ConstByteSpan(bytes.data(), bytes.size()));
}

// A zip pack with a terrain.png and, optionally, a char.png.
std::vector<u8> makePackZip(bool withSkin, int skinWidth = 64, int skinHeight = 32,
                            bool slim = false)
{
    TestZip zip;
    zip.add("terrain.png", makeTerrainPng(256), true, true);
    if (withSkin) {
        zip.add("char.png", makeSkinPng(skinWidth, skinHeight, slim), true, true);
    }
    return zip.finish();
}

}  // namespace

TEST(default_is_always_the_first_row_and_needs_no_card)
{
    io::PosixFileSystem fs;
    std::vector<texture::SkinEntry> skins;
    // No packs and no skins folder: the state a console is in before anything
    // has been put on the card.
    texture::listSkins(fs, {}, "/nonexistent/skins/for/this/test", &skins);

    CHECK_EQ(int(skins.size()), 1);
    CHECK(skins[0].source == SkinSource::Default);
    CHECK(skins[0].key.empty());
    CHECK_EQ(skins[0].name, std::string("Default"));
}

TEST(a_pack_is_listed_only_when_it_carries_a_skin)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;

    CHECK(writeFile(fs, dir.at("with.zip"), makePackZip(true)));
    CHECK(writeFile(fs, dir.at("without.zip"), makePackZip(false)));

    std::vector<texture::PackEntry> packs;
    texture::listPacks(fs, dir.path, &packs);
    // Dev Art plus the two on the card, and `hasSkin` read off the central
    // directory rather than by opening either of them a second time.
    CHECK_EQ(int(packs.size()), 3);

    std::vector<texture::SkinEntry> skins;
    texture::listSkins(fs, packs, "/nonexistent/skins", &skins);
    CHECK_EQ(int(skins.size()), 2);
    CHECK(skins[1].source == SkinSource::Pack);
    CHECK_EQ(skins[1].name, std::string("with.zip"));
    CHECK_EQ(skins[1].key, std::string("pack:with.zip"));
}

TEST(skins_folder_rows_lose_their_extension_and_keep_it_in_the_key)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;
    CHECK(writeFile(fs, dir.at("Steve.png"), makeSkinPng(64, 32, false)));

    std::vector<texture::SkinEntry> skins;
    texture::listSkins(fs, {}, dir.path, &skins);
    CHECK_EQ(int(skins.size()), 2);
    CHECK(skins[1].source == SkinSource::File);
    // The button gets the readable half and the settings file the exact one:
    // the key has to rebuild a path without listing the folder again.
    CHECK_EQ(skins[1].name, std::string("Steve"));
    CHECK_EQ(skins[1].key, std::string("file:Steve.png"));
}

TEST(a_file_that_is_not_a_usable_skin_is_never_offered)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;

    CHECK(writeFile(fs, dir.at("good.png"), makeSkinPng(64, 32, false)));
    // Not a PNG at all.
    CHECK(writeFile(fs, dir.at("broken.png"), std::vector<u8>{1, 2, 3, 4}));
    // A PNG of the wrong shape: 100 does not divide 64.
    CHECK(writeFile(fs, dir.at("oblong.png"),
                    makeRgbaPng(100, 50, std::vector<u8>(100 * 50 * 4, 0x40))));
    // A width that divides but a height that is neither of the two layouts.
    CHECK(writeFile(fs, dir.at("tall.png"),
                    makeRgbaPng(64, 48, std::vector<u8>(64 * 48 * 4, 0x40))));
    // Not a PNG by name. Nothing opens it.
    CHECK(writeFile(fs, dir.at("notes.txt"), std::vector<u8>{'h', 'i'}));

    std::vector<texture::SkinEntry> skins;
    texture::listSkins(fs, {}, dir.path, &skins);
    // **Offered and then failing is the thing being avoided**: a row a player
    // can land on has already been decoded once.
    CHECK_EQ(int(skins.size()), 2);
    CHECK_EQ(skins[1].name, std::string("good"));
}

TEST(a_saved_key_names_its_file_without_listing_anything)
{
    // This is what `ensureAtlas` does at boot, and the whole reason the key is
    // a name: listing the packs folder is a full read of every zip on the card.
    std::string path;
    bool fromPack = false;

    CHECK(texture::skinPathForKey("pack:Faithful.zip", "sdmc:/alpha/packs",
                                  "sdmc:/alpha/skins", &path, &fromPack));
    CHECK(fromPack);
    CHECK_EQ(path, std::string("sdmc:/alpha/packs/Faithful.zip"));

    CHECK(texture::skinPathForKey("file:Steve.png", "sdmc:/alpha/packs",
                                  "sdmc:/alpha/skins", &path, &fromPack));
    CHECK(!fromPack);
    CHECK_EQ(path, std::string("sdmc:/alpha/skins/Steve.png"));

    // Default names no file, and neither does a key from a build that knows a
    // source this one does not.
    CHECK(!texture::skinPathForKey("", "p", "s", &path, &fromPack));
    CHECK(!texture::skinPathForKey("url:https://example/skin.png", "p", "s", &path, &fromPack));
}

TEST(a_key_the_card_no_longer_has_falls_back_to_default)
{
    std::vector<texture::SkinEntry> skins;
    io::PosixFileSystem fs;
    texture::listSkins(fs, {}, "/nonexistent/skins", &skins);

    CHECK_EQ(texture::findSkin(skins, ""), 0);
    // Deleted, renamed, or a different card entirely. Row 0 rather than a
    // screen pointing at nothing.
    CHECK_EQ(texture::findSkin(skins, "file:Gone.png"), 0);
}

TEST(only_a_64x64_skin_can_be_narrow)
{
    texture::Image image;
    // The 64 x 32 layout predates the narrow body by four years, so its arm's
    // last two columns are ordinary texels -- transparent there means nothing.
    CHECK_EQ(int(decodePng(makeSkinPng(64, 32, /*slim=*/true), &image)),
             int(texture::PngError::Ok));
    CHECK(texture::detectSkinModel(image) == SkinModel::Classic);

    CHECK_EQ(int(decodePng(makeSkinPng(64, 64, /*slim=*/true), &image)),
             int(texture::PngError::Ok));
    CHECK(texture::detectSkinModel(image) == SkinModel::Slim);

    CHECK_EQ(int(decodePng(makeSkinPng(64, 64, /*slim=*/false), &image)),
             int(texture::PngError::Ok));
    CHECK(texture::detectSkinModel(image) == SkinModel::Classic);

    // An HD skin is a whole multiple of 64 across and the test scales with it.
    CHECK_EQ(int(decodePng(makeSkinPng(128, 128, /*slim=*/true), &image)),
             int(texture::PngError::Ok));
    CHECK(texture::detectSkinModel(image) == SkinModel::Slim);
}

TEST(a_chosen_skin_replaces_the_player_page_and_leaves_the_others_alone)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;
    const std::string file = dir.at("Steve.png");
    CHECK(writeFile(fs, file, makeSkinPng(64, 32, false)));

    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    std::vector<u8> before = sheet;

    CHECK(texture::applyPlayerSkin(fs, file, /*fromPack=*/false, &sheet));
    CHECK_EQ(sheet.size(), texture::kEntitySheetBytes);

    int ox = 0, oy = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &ox, &oy);
    const auto texel = [&](const std::vector<u8>& s, int x, int y) {
        return s.data() + (usize(y) * usize(texture::kEntitySheetWidth) + usize(x)) * 4;
    };
    // The page changed: the stand-in is black and the skin is not.
    CHECK(texel(sheet, ox + 5, oy + 5)[0] != texel(before, ox + 5, oy + 5)[0]);

    // **And nothing else did.** The pages share one texture, so a blit that
    // walked off the end of the player's page would put a skin on a boat.
    for (int i = 0; i < texture::kEntitySkinCount; ++i) {
        const auto skin = texture::EntitySkin(i);
        if (skin == texture::EntitySkin::Player) {
            continue;
        }
        int px = 0, py = 0;
        texture::skinOrigin(skin, &px, &py);
        for (int y = 0; y < texture::kSkinPageHeight; ++y) {
            for (int x = 0; x < texture::kSkinPageWidth; ++x) {
                CHECK_EQ(int(texel(sheet, px + x, py + y)[0]),
                         int(texel(before, px + x, py + y)[0]));
            }
        }
    }
}

TEST(a_64x64_skin_keeps_its_top_half_rather_than_being_squashed)
{
    TempDir dir;
    CHECK(dir.path[0] != '\0');
    io::PosixFileSystem fs;

    // Top half white, bottom half red. A build that scaled the whole 64 x 64
    // into the 64 x 32 page would land red in the lower rows; a build that
    // crops keeps white everywhere.
    std::vector<u8> rgba(usize(64) * 64 * 4, 0);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            u8* p = rgba.data() + (usize(y) * 64 + usize(x)) * 4;
            p[0] = y < 32 ? 255 : 255;
            p[1] = y < 32 ? 255 : 0;
            p[2] = y < 32 ? 255 : 0;
            p[3] = 255;
        }
    }
    const std::string file = dir.at("Alex.png");
    CHECK(writeFile(fs, file, makeRgbaPng(64, 64, rgba)));

    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    CHECK(texture::applyPlayerSkin(fs, file, /*fromPack=*/false, &sheet));

    int ox = 0, oy = 0;
    texture::skinOrigin(texture::EntitySkin::Player, &ox, &oy);
    for (int y = 0; y < texture::kSkinPageHeight; ++y) {
        const u8* p =
            sheet.data() + (usize(oy + y) * usize(texture::kEntitySheetWidth) + usize(ox + 10)) * 4;
        CHECK_EQ(int(p[1]), 255);  // green survives only in the top half
    }
}

TEST(a_skin_that_went_away_leaves_the_page_as_it_was)
{
    io::PosixFileSystem fs;
    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    const std::vector<u8> before = sheet;

    // Deleted between being listed and being chosen. The page keeps Default,
    // which is a skin rather than a hole.
    CHECK(!texture::applyPlayerSkin(fs, "/nonexistent/skins/Gone.png", false, &sheet));
    CHECK(sheet == before);
}
