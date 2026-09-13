#include "framework.hpp"
#include "texture_support.hpp"

#include "core/block/block_def.hpp"
#include "core/block/registry.hpp"
#include "core/io/posix_file_system.hpp"
#include "core/mesh/vertex.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/texture/dev_art.hpp"
#include "core/texture/fluid_fx.hpp"
#include "core/texture/pack_list.hpp"

#include "version_slots.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mc;
using mc::test::makeTerrainPng;
using mc::test::TestZip;
using texture::AtlasImage;
using texture::kAtlasEdge;
using texture::PackEntry;
using texture::PackError;

namespace {

// Real files, for the reason world_list_test gives: the bugs this layer has --
// a path built wrong, a listing that depends on readdir order, a directory pack
// mistaken for a zip -- do not exist against a stub.
struct TempDir {
    char path[64] = {};

    TempDir()
    {
        std::snprintf(path, sizeof(path), "/tmp/3dalpha_pack_XXXXXX");
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

void writeFile(io::FileSystem& fs, const std::string& path, const std::vector<u8>& bytes)
{
    fs.writeFileAtomic(path.c_str(), bytes);
}

// A pack holding nothing but a terrain.png of the given edge.
std::vector<u8> packWithTerrain(int edge)
{
    TestZip zip;
    zip.add("terrain.png", makeTerrainPng(edge), /*deflate=*/true, /*dataDescriptor=*/true);
    return zip.finish();
}

// The colour makeTerrainPng puts in tile (tx, ty), which any correct scaling
// reproduces exactly because it is flat within the tile.
void expectedTile(int tx, int ty, u8* rgba)
{
    rgba[0] = u8(tx * 16);
    rgba[1] = u8(ty * 16);
    rgba[2] = 200;
    rgba[3] = 255;
}

const u8* atlasTexel(const AtlasImage& atlas, int x, int y)
{
    return atlas.rgba.data() + (usize(y) * kAtlasEdge + usize(x)) * 4;
}

// ---------------------------------------------------------------------------
// Dev Art
// ---------------------------------------------------------------------------

TEST(dev_art_fills_the_whole_atlas_and_is_deterministic)
{
    std::vector<u8> first;
    std::vector<u8> second;
    texture::buildDevArt(&first);
    texture::buildDevArt(&second);

    CHECK_EQ(first.size(), texture::kAtlasBytes);
    CHECK(first == second);
}

// The fluid group is derived from the block table rather than written down, and
// it has to stay derived: a flowing fluid's top face samples the 2x2 that starts
// at its flowing tile, so four unrelated colours there would read as a bug in
// the mesher. See core/mesh/fluid.cpp.
TEST(dev_art_gives_a_fluid_group_one_surface)
{
    std::vector<u8> rgba;
    texture::buildDevArt(&rgba);

    int checked = 0;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (!def.known || def.render != block::RenderType::Fluid) {
            continue;
        }
        const int still = def.faces[mesh::kFacePosY];
        const int flowing = def.faces[mesh::kFaceNegZ];
        const int group[5] = {still, flowing, flowing + 1, flowing + 16, flowing + 17};

        // The jitter is +-16 around a shared base, so every texel of every tile
        // in the group has to sit inside one narrow band. A tile that fell back
        // to the hashed palette would be nowhere near it.
        u8 base[4];
        const int leaderX = (still % 16) * 16;
        const int leaderY = (still / 16) * 16;
        std::memcpy(base, rgba.data() + (usize(leaderY) * kAtlasEdge + usize(leaderX)) * 4, 4);

        for (const int tile : group) {
            if (tile < 0 || tile >= 256) {
                continue;
            }
            const int x0 = (tile % 16) * 16;
            const int y0 = (tile / 16) * 16;
            for (int y = 0; y < 16; ++y) {
                for (int x = 0; x < 16; ++x) {
                    const u8* p = rgba.data() + (usize(y0 + y) * kAtlasEdge + usize(x0 + x)) * 4;
                    for (int c = 0; c < 3; ++c) {
                        const int delta = int(p[c]) - int(base[c]);
                        CHECK(delta >= -40 && delta <= 40);
                    }
                    // Whatever the fluid's alpha is, the whole group shares it.
                    CHECK_EQ(int(p[3]), int(base[3]));
                }
            }
        }
        ++checked;
    }
    // a1.1.2 has water and lava, each still and flowing: four fluid blocks.
    CHECK(checked >= 2);
}

// A torch is four full-block quads with the stick carved out of the tile's own
// transparency. A solid tile there draws a torch as a slab, which reads as a bug
// in the emitter and is not one -- so the carve is part of the generated art.
TEST(dev_art_carves_the_torch_tiles)
{
    std::vector<u8> rgba;
    texture::buildDevArt(&rgba);

    int checked = 0;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (!def.known || def.render != block::RenderType::Torch) {
            continue;
        }
        const int tile = def.faces[mesh::kFaceNegY];
        CHECK(tile >= 0 && tile < 256);

        const int x0 = (tile % 16) * 16;
        const int y0 = (tile / 16) * 16;

        // Outside the stick: transparent. Inside it, below the flame: opaque.
        // The columns and rows are core/mesh/torch.cpp's geometry, not a taste.
        CHECK_EQ(int(rgba[(usize(y0 + 8) * kAtlasEdge + usize(x0 + 0)) * 4 + 3]), 0);
        CHECK_EQ(int(rgba[(usize(y0 + 0) * kAtlasEdge + usize(x0 + 7)) * 4 + 3]), 0);
        CHECK_EQ(int(rgba[(usize(y0 + 8) * kAtlasEdge + usize(x0 + 7)) * 4 + 3]), 255);
        CHECK_EQ(int(rgba[(usize(y0 + 15) * kAtlasEdge + usize(x0 + 8)) * 4 + 3]), 255);
        // And the cap quad's texels, which are the top of every torch.
        CHECK_EQ(int(rgba[(usize(y0 + 6) * kAtlasEdge + usize(x0 + 7)) * 4 + 3]), 255);
        ++checked;
    }
    CHECK(checked >= 1);
}

// ---------------------------------------------------------------------------
// Scaling
// ---------------------------------------------------------------------------

namespace {

// **Twelve tiles of a loaded pack are not the pack's**, and the scaling tests
// below have to skip them: the client generates fire, water and lava rather
// than reading them, and `buildAtlas` does the same -- see
// core/texture/texture_fx.hpp and core/texture/fluid_fx.hpp. That they really
// are overwritten is asserted there; here they are simply not the pack's
// colours any more.
//
// Found through the render type rather than named, which is how
// applyAnimatedTiles finds them too.
bool tileIsGenerated(int tx, int ty)
{
    const int tile = ty * 16 + tx;
    for (int id = 0; id < mcver::kBlockTableSize; ++id) {
        const block::BlockDef& def = mcver::kBlocks[id];
        if (def.known && def.render == block::RenderType::Fire
            && (tile == int(def.texture) || tile == int(def.texture) + 16)) {
            return true;
        }
    }
    for (const bool hot : {false, true}) {
        const texture::FluidTiles fluid = texture::fluidTiles(hot);
        if (tile == fluid.still) {
            return true;
        }
        if (fluid.flowing >= 0
            && (tile == fluid.flowing || tile == fluid.flowing + 1 || tile == fluid.flowing + 16
                || tile == fluid.flowing + 17)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(a_16x_pack_is_copied_unchanged)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string path = dir.at("pack.zip");
    writeFile(fs, path, packWithTerrain(256));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, path, &atlas)), int(PackError::Ok));
    CHECK_EQ(atlas.sourceEdge, 256);
    CHECK(!atlas.empty());

    for (int ty = 0; ty < 16; ++ty) {
        for (int tx = 0; tx < 16; ++tx) {
            if (tileIsGenerated(tx, ty)) {
                continue;
            }
            u8 want[4];
            expectedTile(tx, ty, want);
            const u8* got = atlasTexel(atlas, tx * 16 + 3, ty * 16 + 9);
            CHECK_EQ(int(got[0]), int(want[0]));
            CHECK_EQ(int(got[1]), int(want[1]));
            CHECK_EQ(int(got[2]), int(want[2]));
            CHECK_EQ(int(got[3]), int(want[3]));
        }
    }
}

// A 64x pack: 1024x1024, which is 4 MB at RGBA8 against a 6 MB VRAM budget that
// render targets already take 0.8-1.5 MB of. It comes down to 256 rather than
// being refused, and every tile is flat, so a correct box filter reproduces the
// source exactly.
TEST(an_hd_pack_is_box_filtered_down)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string path = dir.at("hd.zip");
    writeFile(fs, path, packWithTerrain(1024));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, path, &atlas)), int(PackError::Ok));
    CHECK_EQ(atlas.sourceEdge, 1024);
    CHECK_EQ(atlas.rgba.size(), texture::kAtlasBytes);

    for (int ty = 0; ty < 16; ++ty) {
        for (int tx = 0; tx < 16; ++tx) {
            if (tileIsGenerated(tx, ty)) {
                continue;
            }
            u8 want[4];
            expectedTile(tx, ty, want);
            const u8* got = atlasTexel(atlas, tx * 16 + 5, ty * 16 + 2);
            CHECK_EQ(int(got[0]), int(want[0]));
            CHECK_EQ(int(got[1]), int(want[1]));
            CHECK_EQ(int(got[2]), int(want[2]));
        }
    }
}

// **The bug this exists to stop.** A cutout edge -- a torch, a sapling, a leaf
// rim -- sits next to texels whose alpha is 0 and whose RGB is usually black. A
// plain mean across them drags the visible half toward black and every HD pack
// grows dark halos. Averaging premultiplied and dividing back out keeps the
// colour of whatever was actually visible.
TEST(downscaling_does_not_darken_a_cutout_edge)
{
    constexpr int kEdge = 512;
    std::vector<u8> rgba(usize(kEdge) * kEdge * 4, 0);

    // A 2x2 source block per destination texel. In each, one texel is opaque
    // white and three are transparent black.
    for (int y = 0; y < kEdge; ++y) {
        for (int x = 0; x < kEdge; ++x) {
            u8* p = rgba.data() + (usize(y) * kEdge + usize(x)) * 4;
            const bool opaque = (x % 2) == 0 && (y % 2) == 0;
            p[0] = opaque ? 255 : 0;
            p[1] = opaque ? 255 : 0;
            p[2] = opaque ? 255 : 0;
            p[3] = opaque ? 255 : 0;
        }
    }

    texture::Image source;
    source.width = kEdge;
    source.height = kEdge;
    source.rgba = std::move(rgba);

    std::vector<u8> out;
    texture::scaleToAtlas(source, &out);

    // Colour stays white -- it is the only colour anything visible had. Alpha
    // becomes a quarter, which is the coverage. A plain mean would give 63 for
    // the colour as well, which is what the halo looks like.
    CHECK_EQ(int(out[0]), 255);
    CHECK_EQ(int(out[1]), 255);
    CHECK_EQ(int(out[2]), 255);
    CHECK_EQ(int(out[3]), 63);
}

// A fully transparent region has no colour to preserve, and dividing by its
// zero alpha would be the obvious way to crash here.
TEST(downscaling_a_fully_transparent_region_does_not_divide_by_zero)
{
    constexpr int kEdge = 512;
    texture::Image source;
    source.width = kEdge;
    source.height = kEdge;
    source.rgba.assign(usize(kEdge) * kEdge * 4, 0);
    for (usize i = 0; i < source.rgba.size(); i += 4) {
        source.rgba[i + 0] = 120;  // a colour hidden under zero alpha
    }

    std::vector<u8> out;
    texture::scaleToAtlas(source, &out);
    CHECK_EQ(int(out[3]), 0);
    CHECK_EQ(int(out[0]), 120);
}

// An 8x pack goes up by replication rather than interpolation: the whole look
// depends on nearest filtering, and a blurred atlas would be visible everywhere.
TEST(a_small_pack_is_replicated_not_interpolated)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string path = dir.at("small.zip");
    writeFile(fs, path, packWithTerrain(128));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, path, &atlas)), int(PackError::Ok));
    CHECK_EQ(atlas.sourceEdge, 128);

    // Every texel is one of the source's exact colours -- no blend appears.
    for (int ty = 0; ty < 16; ++ty) {
        for (int tx = 0; tx < 16; ++tx) {
            if (tileIsGenerated(tx, ty)) {
                continue;
            }
            u8 want[4];
            expectedTile(tx, ty, want);
            for (int i = 0; i < 4; ++i) {
                const u8* got = atlasTexel(atlas, tx * 16 + i, ty * 16 + i);
                CHECK_EQ(int(got[0]), int(want[0]));
                CHECK_EQ(int(got[1]), int(want[1]));
            }
        }
    }
}

TEST(refuses_a_terrain_png_that_is_not_a_tile_grid)
{
    TempDir dir;
    io::PosixFileSystem fs;

    // Not square.
    TestZip oblong;
    oblong.add("terrain.png", mc::test::makeRgbaPng(32, 16, std::vector<u8>(32 * 16 * 4, 0x40)),
               /*deflate=*/true, /*dataDescriptor=*/false);
    const std::string oblongPath = dir.at("oblong.zip");
    writeFile(fs, oblongPath, oblong.finish());

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, oblongPath, &atlas)), int(PackError::NotSquare));

    // Square, but not 16 tiles across -- so every tile boundary would land a
    // fraction of a texel off where the mesher's UVs put it.
    TestZip odd;
    odd.add("terrain.png", mc::test::makeRgbaPng(100, 100, std::vector<u8>(100 * 100 * 4, 0x40)),
            /*deflate=*/true, /*dataDescriptor=*/false);
    const std::string oddPath = dir.at("odd.zip");
    writeFile(fs, oddPath, odd.finish());

    CHECK_EQ(int(texture::buildAtlas(fs, oddPath, &atlas)), int(PackError::NotTileGrid));

    // A zip with no terrain.png at all.
    TestZip bare;
    bare.add("gui/gui.png", std::vector<u8>(16, 0x11), /*deflate=*/false,
             /*dataDescriptor=*/false);
    const std::string barePath = dir.at("bare.zip");
    writeFile(fs, barePath, bare.finish());

    CHECK_EQ(int(texture::buildAtlas(fs, barePath, &atlas)), int(PackError::NoTerrain));

    // A file that is not an archive at all.
    const std::string junkPath = dir.at("junk.zip");
    writeFile(fs, junkPath, std::vector<u8>(200, 0x5A));
    CHECK_EQ(int(texture::buildAtlas(fs, junkPath, &atlas)), int(PackError::NotAPack));

    CHECK_EQ(int(texture::buildAtlas(fs, dir.at("absent.zip"), &atlas)),
             int(PackError::NotFound));
}

// A card is mounted on a PC as often as on a console, and a player who unzipped
// a pack in place has not done anything wrong.
TEST(a_directory_is_a_pack_too)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string pack = dir.at("Loose Pack");
    CHECK(fs.makeDirectories((pack + "/gui").c_str()));
    writeFile(fs, pack + "/terrain.png", makeTerrainPng(256));
    writeFile(fs, pack + "/gui/gui.png", std::vector<u8>(8, 0x22));

    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, pack, &atlas)), int(PackError::Ok));
    CHECK_EQ(atlas.sourceEdge, 256);

    std::vector<PackEntry> packs;
    texture::listPacks(fs, dir.path, &packs);
    CHECK_EQ(packs.size(), usize(2));
    CHECK_EQ(packs[1].name, std::string("Loose Pack"));
    CHECK(packs[1].hasTerrain);
    CHECK_EQ(packs[1].textureCount, 2);  // terrain.png and gui/gui.png
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

TEST(dev_art_is_pinned_first_and_always_there)
{
    io::PosixFileSystem fs;

    std::vector<PackEntry> packs;
    texture::listPacks(fs, "/tmp/3dalpha_no_such_packs_dir", &packs);

    CHECK_EQ(packs.size(), usize(1));
    CHECK(packs[0].builtIn);
    CHECK(packs[0].path.empty());
    CHECK(packs[0].hasTerrain);

    // The built-in pack needs nothing off the card, which is what makes this a
    // selector rather than something that can leave the game untextured.
    AtlasImage atlas;
    CHECK_EQ(int(texture::buildAtlas(fs, packs[0].path, &atlas)), int(PackError::Ok));
    CHECK(!atlas.empty());
}

TEST(lists_packs_by_name_and_skips_what_is_not_one)
{
    TempDir dir;
    io::PosixFileSystem fs;

    writeFile(fs, dir.at("zebra.zip"), packWithTerrain(256));
    writeFile(fs, dir.at("alpha.zip"), packWithTerrain(256));
    // A zip with no terrain.png: not a pack, skipped in silence.
    TestZip bare;
    bare.add("readme.txt", std::vector<u8>(4, 'x'), false, false);
    writeFile(fs, dir.at("notapack.zip"), bare.finish());
    // Things that are simply other files on the card.
    writeFile(fs, dir.at("minecraft.jar"), std::vector<u8>(64, 0x30));
    writeFile(fs, dir.at("notes.txt"), std::vector<u8>(8, 'n'));
    writeFile(fs, dir.at("corrupt.zip"), std::vector<u8>(300, 0x5A));

    std::vector<PackEntry> packs;
    texture::listPacks(fs, dir.path, &packs);

    CHECK_EQ(packs.size(), usize(3));
    CHECK(packs[0].builtIn);
    CHECK_EQ(packs[1].name, std::string("alpha.zip"));
    CHECK_EQ(packs[2].name, std::string("zebra.zip"));
    CHECK_EQ(packs[1].path, dir.at("alpha.zip"));
}

TEST(lists_jars_across_both_folders_with_their_sizes)
{
    TempDir dir;
    io::PosixFileSystem fs;

    const std::string packs = dir.at("packs");
    CHECK(fs.makeDirectories(packs.c_str()));

    writeFile(fs, packs + "/minecraft.jar", std::vector<u8>(1234, 0x11));
    writeFile(fs, dir.at("minecraft-a1.1.2_01-client.jar"), std::vector<u8>(4321, 0x22));
    writeFile(fs, packs + "/pack.zip", packWithTerrain(256));

    const std::string dirs[2] = {packs, std::string(dir.path)};
    std::vector<texture::JarEntry> jars;
    texture::listJars(fs, dirs, 2, &jars);

    CHECK_EQ(jars.size(), usize(2));
    CHECK_EQ(jars[0].name, std::string("minecraft-a1.1.2_01-client.jar"));
    CHECK_EQ(jars[0].bytes, usize(4321));
    CHECK_EQ(jars[1].name, std::string("minecraft.jar"));
    CHECK_EQ(jars[1].bytes, usize(1234));
}

// The table the pack screen counts against was read out of a real jar rather
// than written from memory, and its shape is the evidence for three corrections
// to docs/assets.md. If any of these three stops holding, the table has been
// edited by hand and the doc is wrong again.
TEST(the_a112_file_table_matches_what_the_jar_holds)
{
    CHECK_EQ(texture::kA112FileCount, 58);

    int misc = 0;
    bool sawRootWater = false;
    bool sawColourMap = false;
    bool sawTerrain = false;
    for (int i = 0; i < texture::kA112FileCount; ++i) {
        const std::string name = texture::kA112Files[i];
        if (name.compare(0, 5, "misc/") == 0) {
            ++misc;
        }
        if (name == "water.png") {
            sawRootWater = true;
        }
        if (name == "misc/grasscolor.png" || name == "misc/foliagecolor.png") {
            sawColourMap = true;
        }
        if (name == "terrain.png") {
            sawTerrain = true;
        }
    }

    CHECK(sawTerrain);
    CHECK_EQ(misc, 3);       // gear, gearmiddle, vignette -- and nothing else
    CHECK(sawRootWater);     // water.png is at the root, not under misc/
    CHECK(!sawColourMap);    // a1.1.2 tints nothing; there are no colour maps
}

}  // namespace
