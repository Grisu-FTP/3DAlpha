// The entity sheet -- five small a1.1.2 textures packed into one 256 x 64.
//
// What can go wrong here is silent: a page whose offset overlaps another puts
// a minecart's iron on a boat, and a pack that carries four of the five files
// must not leave the fifth black. Both are checkable without a GPU.
//
// **The player's page is the one that is black on purpose**, and it is the
// exception to two of the rules below: no grid and no corner mark. See
// core/texture/entity_skins.hpp for why an arm gets a silhouette where a boat
// gets a placeholder.

#include "core/io/file_system.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/texture/entity_skins.hpp"
#include "framework.hpp"

#include <string>
#include <vector>

using namespace mc;
using mc::texture::EntitySkin;
using mc::texture::kEntitySheetBytes;
using mc::texture::kEntitySheetHeight;
using mc::texture::kEntitySheetWidth;

namespace {

const u8* texelAt(const std::vector<u8>& sheet, int x, int y)
{
    return sheet.data() + (usize(y) * usize(kEntitySheetWidth) + usize(x)) * 4;
}

}  // namespace

TEST(the_stand_in_sheet_is_the_right_size_and_wholly_opaque)
{
    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    CHECK_EQ(sheet.size(), kEntitySheetBytes);

    // Every page is filled. A transparent texel inside a page would be a hole
    // in a boat, and the placeholder exists precisely so that nothing is a
    // hole.
    //
    // **The spider's eye page is the exception and has to be**, because the
    // file it stands in for is an *overlay*: `mob/spider_eyes.png` is clear
    // everywhere but the eyes, and the detail pass's alpha test is what makes
    // the rest of the head show through. A filled eye page would draw a solid
    // box over the spider's face.
    for (int i = 0; i < texture::kEntitySkinCount; ++i) {
        if (EntitySkin(i) == EntitySkin::SpiderEyes) {
            continue;
        }
        int ox = 0, oy = 0;
        texture::skinOrigin(EntitySkin(i), &ox, &oy);
        CHECK_EQ(int(texelAt(sheet, ox + 1, oy + 1)[3]), 255);
    }
}

TEST(the_spider_eye_page_is_clear_except_for_two_dots)
{
    // The pair of the exemption above: the page is not merely allowed to be
    // transparent, it is **required** to be -- and to have something on it, or
    // a spider in the dark has no eyes at all.
    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    int ox = 0, oy = 0;
    texture::skinOrigin(EntitySkin::SpiderEyes, &ox, &oy);

    int opaque = 0;
    for (int y = 0; y < texture::kSkinPageHeight; ++y) {
        for (int x = 0; x < texture::kSkinPageWidth; ++x) {
            if (texelAt(sheet, ox + x, oy + y)[3] != 0) {
                ++opaque;
            }
        }
    }
    // Two 2 x 2 dots and nothing else.
    CHECK_EQ(opaque, 8);
}

TEST(each_page_has_its_own_colour)
{
    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);

    // Sampled away from the grid lines and away from the corner mark, so this
    // is the page's body colour and not its decoration.
    // **The eye page has no body colour to be its own** -- it is an overlay and
    // is clear except for two dots -- so it is left out of the comparison
    // rather than given a colour it must not have.
    u8 red[texture::kEntitySkinCount];
    for (int i = 0; i < texture::kEntitySkinCount; ++i) {
        int ox = 0, oy = 0;
        texture::skinOrigin(EntitySkin(i), &ox, &oy);
        red[i] = texelAt(sheet, ox + 5, oy + 5)[0];
    }
    for (int a = 0; a < texture::kEntitySkinCount; ++a) {
        if (EntitySkin(a) == EntitySkin::SpiderEyes) {
            continue;
        }
        for (int b = a + 1; b < texture::kEntitySkinCount; ++b) {
            if (EntitySkin(b) == EntitySkin::SpiderEyes) {
                continue;
            }
            CHECK(red[a] != red[b]);
        }
    }
}

TEST(every_placeholder_page_carries_a_corner_mark_so_up_is_visible)
{
    // The grid says whether the UVs are transposed; the corner says which way
    // up. Both are the point of a placeholder and neither is decoration.
    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    for (int i = 0; i < texture::kEntitySkinCount; ++i) {
        if (EntitySkin(i) == EntitySkin::Player) {
            continue;  // A silhouette, not a placeholder. See below.
        }
        if (EntitySkin(i) == EntitySkin::SpiderEyes) {
            continue;  // An overlay, and a corner mark on one would be a dot
                       // floating beside the spider's head.
        }
        int ox = 0, oy = 0;
        texture::skinOrigin(EntitySkin(i), &ox, &oy);
        const u8* corner = texelAt(sheet, ox, oy);
        CHECK_EQ(int(corner[0]), 255);
        CHECK_EQ(int(corner[1]), 255);
        CHECK_EQ(int(corner[2]), 255);
    }
}

TEST(the_player_page_without_a_skin_is_opaque_black_all_over)
{
    // **The whole page, not a sample.** The arm covers texels from several
    // corners of its page, so one grid line left over from an earlier
    // stand-in would show as a coloured stripe down a silhouette.
    std::vector<u8> sheet;
    texture::buildDevArtSkins(&sheet);
    int ox = 0, oy = 0;
    texture::skinOrigin(EntitySkin::Player, &ox, &oy);
    for (int y = 0; y < texture::kSkinPageHeight; ++y) {
        for (int x = 0; x < texture::kSkinPageWidth; ++x) {
            const u8* p = texelAt(sheet, ox + x, oy + y);
            CHECK_EQ(int(p[0]), 0);
            CHECK_EQ(int(p[1]), 0);
            CHECK_EQ(int(p[2]), 0);
            // Opaque: the alpha test would otherwise cut the arm away.
            CHECK_EQ(int(p[3]), 255);
        }
    }
}

TEST(the_pages_do_not_overlap_each_other)
{
    // Five pages in an eight-slot sheet, and the sheet grew rightwards so the
    // original four kept their offsets. An overlap would put a boat's planks on
    // an arm and would be invisible until somebody looked at one.
    for (int a = 0; a < texture::kEntitySkinCount; ++a) {
        int ax = 0, ay = 0;
        texture::skinOrigin(EntitySkin(a), &ax, &ay);
        CHECK(ax >= 0);
        CHECK(ay >= 0);
        CHECK(ax + texture::kSkinPageWidth <= kEntitySheetWidth);
        CHECK(ay + texture::kSkinPageHeight <= kEntitySheetHeight);
        for (int b = a + 1; b < texture::kEntitySkinCount; ++b) {
            int bx = 0, by = 0;
            texture::skinOrigin(EntitySkin(b), &bx, &by);
            const bool apart = ax + texture::kSkinPageWidth <= bx
                               || bx + texture::kSkinPageWidth <= ax
                               || ay + texture::kSkinPageHeight <= by
                               || by + texture::kSkinPageHeight <= ay;
            CHECK(apart);
        }
    }
}

TEST(a_pack_with_none_of_the_files_still_gets_a_whole_sheet)
{
    // The rule the header states: the stand-ins go down first and the pack is
    // painted over them, so a missing file costs its page nothing. This is the
    // "no pack at all" end of it -- an empty path is Dev Art.
    io::PosixFileSystem fs;
    std::vector<u8> sheet;
    texture::buildEntitySkins(fs, "", &sheet);
    CHECK_EQ(sheet.size(), kEntitySheetBytes);

    std::vector<u8> devArt;
    texture::buildDevArtSkins(&devArt);
    CHECK(sheet == devArt);
}

TEST(a_pack_path_that_does_not_exist_leaves_the_stand_ins_alone)
{
    // A path with nothing at it is not an error here -- it is a pack carrying
    // none of the five files, which is the same answer.
    io::PosixFileSystem fs;
    std::vector<u8> sheet;
    texture::buildEntitySkins(fs, "/nonexistent/pack/for/this/test.zip", &sheet);
    CHECK_EQ(sheet.size(), kEntitySheetBytes);

    std::vector<u8> devArt;
    texture::buildDevArtSkins(&devArt);
    CHECK(sheet == devArt);
}

TEST(the_art_stand_in_is_a_grid_of_framed_cells)
{
    std::vector<u8> art;
    texture::buildDevArtArt(&art);
    CHECK_EQ(art.size(), usize(256) * 256 * 4);

    // Frames on the cell edges, body inside. Every art rectangle in the
    // generated table is a multiple of sixteen, so a picture placed with a
    // stand-in reads as a framed painting of the right size rather than as a
    // missing texture.
    const u8* edge = art.data() + (usize(0) * 256 + 0) * 4;
    const u8* body = art.data() + (usize(8) * 256 + 8) * 4;
    CHECK(edge[0] != body[0] || edge[1] != body[1] || edge[2] != body[2]);
    CHECK_EQ(int(body[3]), 255);
}

TEST(named_pages_report_the_files_they_come_from)
{
    // The five names are what a reader checks against a pack, and a page whose
    // name did not match its offset would be found here and nowhere else.
    CHECK(std::string(texture::skinFileName(EntitySkin::Boat)).find("boat")
          != std::string::npos);
    CHECK(std::string(texture::skinFileName(EntitySkin::Minecart)).find("cart")
          != std::string::npos);
    CHECK(std::string(texture::skinFileName(EntitySkin::Sign)).find("sign")
          != std::string::npos);
    CHECK(std::string(texture::skinFileName(EntitySkin::Arrow)).find("arrow")
          != std::string::npos);
    // **At the root and not under `item/`**, which is where a1.1.2 keeps the
    // player skin and is the one thing unusual about this page.
    CHECK_EQ(std::string(texture::skinFileName(EntitySkin::Player)), std::string("char.png"));
}
