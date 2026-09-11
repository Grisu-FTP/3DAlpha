#pragma once

// **Which player skins the card has**, for the Skins screen.
//
// The only thing in this build that draws a player skin is the arm of an empty
// hand -- `bu.b()`, and see core/render/held_item.hpp -- so this is a small
// list and it is worth saying what is on it and where each row comes from:
//
//   * **Default**, which is whatever `buildEntitySkins` already put in the
//     player's page: the *active* texture pack's `char.png` if it carries one,
//     and the black silhouette if it does not. Always first, always present,
//     and the one row that reads nothing off the card.
//   * **Every texture pack that carries a `char.png`**, whether or not it is
//     the pack in use. A player who wants one pack's blocks and another's skin
//     can have both.
//   * **Every `.png` in `sdmc:/3dalpha/skins`**, which is the normal skin
//     format and the normal thing to have a folder of.
//
// **It is core rather than platform code** for the reason `pack_list.hpp`
// gives: the console cannot run a unit test, and what counts as a skin, what
// order the rows come in and what a row is called are all rules worth testing.
//
// **The pack list is passed in rather than re-read.** `listPacks` already opens
// every zip on the card once, and `PackEntry::hasSkin` is filled from the same
// name list it counts textures from -- so a pack with no skin costs this
// nothing at all, and a pack with one is opened a second time only to read the
// file. Listing a folder of skins costs one decode each; they are one or two
// kilobytes and there is no other way to know whether a skin is slim.

#include "core/io/file_system.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/texture/pack_list.hpp"
#include "core/texture/png.hpp"
#include "core/util/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace mc::texture {

// Where a player drops skins. Beside `packs/`, and named for what is in it.
inline constexpr char kSkinsDir[] = "sdmc:/3dalpha/skins";

// Where a row's image comes from.
enum class SkinSource : u8 {
    // The page the atlas already holds. No file, no read.
    Default,
    // `char.png` inside a pack in `packs/`.
    Pack,
    // A `.png` in `skins/`.
    File,
};

// **Which arm the skin was drawn for**, and it is not a thing a1.1.2 has.
//
// The narrow body arrived with the 64 x 64 skin format, and both are 1.8's --
// which is why this is reported rather than acted on here, and why acting on it
// is behind `mcver::kHasSlimSkins`. A skin drawn for the narrow arm still
// *works* in this version; its sleeve is simply a texel wider than the artist
// drew, exactly as it was for everyone who used one before 1.8.
enum class SkinModel : u8 {
    Classic,
    Slim,
};

struct SkinEntry {
    // What the player sees: "Default", the pack's name, or the file's name
    // without its extension.
    std::string name;

    // **What goes in `3ds.ini`**, and a name rather than a path so that moving
    // the card's `3dalpha` folder does not orphan the setting -- the same rule
    // `texturePack` follows:
    //
    //     Default          ""
    //     a pack           "pack:<pack name>"
    //     a skins/ file    "file:<file name.png>"
    //
    // The file keeps its extension here and loses it in `name`, because this
    // half has to reconstruct a path and that half has to fit on a button.
    std::string key;

    // The pack directory/zip, or the skin file. Empty for Default.
    std::string path;

    SkinSource source = SkinSource::Default;
    SkinModel model = SkinModel::Classic;

    // The decoded size, which is what says whether a skin is the 64 x 32 this
    // version knows or a 64 x 64 whose lower half belongs to a later one. Zero
    // for Default, which is not read.
    int width = 0;
    int height = 0;
};

// Default first, then every pack carrying a skin in the order `packs` is in,
// then every `.png` in `skinsDir` sorted by name.
//
// A file that will not decode is left off the list rather than offered and then
// failing at the moment it is picked -- the same rule `listPacks` follows for a
// zip that will not open.
void listSkins(io::FileSystem& fs, const std::vector<PackEntry>& packs,
               std::string_view skinsDir, std::vector<SkinEntry>* out);

// The row `key` names, or 0 -- Default -- when the card no longer has it. A
// skin deleted off the card falls back rather than leaving the screen pointing
// at nothing.
int findSkin(const std::vector<SkinEntry>& skins, std::string_view key);

// **The file a saved key names, without listing anything.**
//
// This is the half `ensureAtlas` needs and the reason the key carries a name
// rather than an index: applying the saved skin at boot must not cost a walk of
// the packs folder, which is a full read of every zip on the card. False for
// Default, which has no file.
//
// `*fromPack` says which kind of read `*path` wants: a pack is opened and
// `char.png` taken out of it, a file is read whole.
bool skinPathForKey(std::string_view key, std::string_view packsDir, std::string_view skinsDir,
                    std::string* path, bool* fromPack);

// **Is this image drawn for the narrow arm?**
//
// Only a 64 x 64 skin can be: the narrow body and that format arrived together.
// The test is the one the format itself implies -- a narrow arm's unwrap is
// 2 * (3 + 4) = 14 texels wide where a wide one is 16, so the last two columns
// of the right arm's page are texels no narrow skin ever fills. Transparent
// there means narrow.
//
// Scaled for an HD skin, which is a whole multiple of 64 across.
SkinModel detectSkinModel(const Image& image);

}  // namespace mc::texture
