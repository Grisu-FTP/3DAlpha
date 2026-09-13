#pragma once

// **The four small entity textures, packed into one sheet.**
//
// The world is drawn out of `terrain.png` and a slot out of `gui/items.png`,
// and until now those were the only two images anything sampled. a1.1.2 draws
// its entities out of five *more* files, and the detail pass samples exactly
// one texture per draw -- so five files would be five binds and five draws for
// a handful of quads each.
//
// Five of the six are tiny and go here:
//
//   | a1.1.2 file        | size    | drawn by                       |
//   |--------------------|---------|--------------------------------|
//   | `item/boat.png`    | 64 x 32 | `cp` RenderBoat                |
//   | `item/cart.png`    | 64 x 32 | `kt` RenderMinecart            |
//   | `item/sign.png`    | 64 x 32 | `in` TileEntitySignRenderer    |
//   | `item/arrows.png`  | 32 x 32 | `gk` RenderArrow               |
//   | `char.png`         | 64 x 32 | `bu` RenderPlayer -- the arm   |
//
// The sixth, `art/kz.png`, is already 256 x 256 and stays a sheet of its own --
// see `artRgba`. See docs/entity-render-a1.1.2.md for the argument.
//
// **`char.png` is the player skin and it is a pack file like any other.** It
// sits at the root of the jar rather than under `item/`, which is the only
// thing unusual about it, and the one thing that reads from it here is the
// **right arm of the empty hand** -- `bu.b()`, `drawFirstPersonHand`. Nothing
// else in this build draws a biped.
//
// **256 x 128, and it has grown twice.** It was 128 x 64 for the four small
// item textures, then 256 x 64 when `char.png` became a fifth page (96 is not a
// power of two and a PICA texture dimension has to be), and it is 256 x 128 now
// that the four animals arrived with six pages between them:
//
//   | a1.1.2 file           | drawn by                                  |
//   |-----------------------|-------------------------------------------|
//   | `mob/pig.png`         | `gm` RenderPig                            |
//   | `mob/saddle.png`      | `gm`'s second pass, on a saddled pig only |
//   | `mob/cow.png`         | `mc` RenderCow                            |
//   | `mob/sheep.png`       | `ns` RenderSheep -- the shorn body        |
//   | `mob/sheep_fur.png`   | `ns`'s second pass, on an unshorn one     |
//   | `mob/chicken.png`     | `eq` RenderChicken                        |
//
// Sixteen 64 x 32 slots, eleven used, 128 KB in linear memory rather than VRAM
// for the reason the header gives below. **Every existing page keeps the origin
// it had** -- the sheet grew downwards -- so no model's UVs moved, which is the
// same promise the last growth made. The block atlas is square and 256 because
// the mesher's tile arithmetic says so; nothing here is tiled, so the sheet is
// only as big as what it holds.
//
// **The slots are fixed, not packed.** A rectangle packer whose output can move
// is a table that has to be regenerated to stay true, and these offsets are
// compiled into every model's UVs. Four constants are cheaper than a
// dependency.
//
// **64 x 32 is not our choice either.** a1.1.2's `ll` -- TexturedQuad -- divides
// every UV by a hard-coded 64.0f and 32.0f, so a box model's texture space *is*
// 64 x 32 in this version, with no `textureWidth` field anywhere to say
// otherwise. A model's UVs are therefore written in those units and this file's
// only job is to say where in the sheet each 64 x 32 page begins.
//
// **A pack's HD version is scaled down to the canonical size**, exactly as an
// HD terrain.png is scaled to 256. The reason is the same one: the offsets
// above are compiled in, and a sheet whose page size was a runtime value would
// make every model's UV a runtime multiply.
//
// **Nothing here is required.** A pack with none of these files gets the
// generated stand-ins, the same way a pack with no `gui/items.png` gets its
// icons from terrain tiles. Dev Art gets them always.
//
// **The player's page is the exception, and its stand-in is black.** Every
// other page's stand-in is a coloured grid, which is right for them: a boat
// with a stand-in still reads as a boat. An arm does not work that way -- a
// forearm in Dev Art orange reads as a bug rather than as a placeholder -- so a
// pack with no `char.png` gets an arm that is a **solid black silhouette**,
// which is honest about being a shape with no skin on it. It is also the one
// page whose texels are all covered by the model, so a grid would tell a reader
// nothing a silhouette does not.

#include "core/io/file_system.hpp"
#include "core/texture/atlas_image.hpp"
#include "core/util/types.hpp"

#include <string_view>
#include <vector>

namespace mc::texture {

// The sheet, and the page every model's UVs are relative to.
//
// **256 x 256 since the monsters landed**, up from 256 x 128. Four rows of four
// 64 x 32 pages held sixteen and the five monsters plus the spider's eye
// overlay needed seventeen; the GPU wants both dimensions a power of two, so
// the next size up is the only size up. It costs 128 KB of the sheet and the
// same again of texture memory, and it leaves fifteen pages free -- which is
// the whole of what is left in `ew`'s table plus room to spare.
//
// **Every page above the monsters kept its origin**, so nothing that was
// already drawing had to move.
inline constexpr int kEntitySheetWidth = 256;
inline constexpr int kEntitySheetHeight = 256;
inline constexpr usize kEntitySheetBytes =
    usize(kEntitySheetWidth) * kEntitySheetHeight * 4;

// `ll`'s two divisors. See the header: they are the class file's, not ours.
inline constexpr int kSkinPageWidth = 64;
inline constexpr int kSkinPageHeight = 32;

// **The player skin's name, defined once.** a1.1.2 keeps it at the root of the
// pack rather than under `item/`, which is the one thing unusual about it, and
// three unrelated files ask about it -- the page below, the pack list's
// `hasSkin`, and the skin screen. It lives here because this is where the page
// it fills is defined.
inline constexpr char kSkinFile[] = "char.png";

// Which page of the sheet a model draws from.
//
// The order is the order they are laid out in, and the values are indices into
// `skinOrigin` rather than anything a save file or a packet has ever seen -- so
// adding one is free.
enum class EntitySkin : u8 {
    Boat,
    Minecart,
    Sign,
    Arrow,
    Player,
    // The four animals and the two overlays. `Saddle` and `SheepFur` are second
    // passes over the same model rather than models of their own -- see
    // core/render/mob_mesh.hpp.
    Pig,
    Saddle,
    Cow,
    Sheep,
    SheepFur,
    Chicken,
    // The five monsters, and the spider's eyes. `mob/spider_eyes.png` is a
    // second pass over the spider's own model in the original (`ok`, which
    // blends it at `(1 - brightness) * 0.5`); it is a page here because it is a
    // file in the pack and because one part of the model is redrawn from it --
    // see core/render/mob_mesh.hpp.
    Zombie,
    Skeleton,
    Creeper,
    Spider,
    SpiderEyes,
    Slime,
    // **The sky's two, and they are 32 x 32 rather than 64 x 32.** `terrain/
    // sun.png` and `terrain/moon.png` are the only two files RenderGlobal binds
    // that are not a block atlas, they are a handful of texels each, and a
    // texture of their own would be a third bind for two quads a frame -- so
    // they come off this sheet like everything else small. Each still takes a
    // whole 64 x 32 slot: the pages are addressed by origin and half of one is
    // cheaper than a second page size for the sheet's arithmetic to carry.
    Sun,
    Moon,
};
inline constexpr int kEntitySkinCount = 19;

// The sun and the moon are square and 32 texels, where every other page is
// 64 x 32. core/render/sky.cpp reads this to build their UVs.
inline constexpr int kCelestialPagePixels = 32;

// Where this page starts in the sheet, in texels from the top left.
void skinOrigin(EntitySkin skin, int* x, int* y);

// The pack file each page comes from, for the reader that wants to say which
// file was missing.
const char* skinFileName(EntitySkin skin);

// Builds the whole sheet.
//
// `packPath` empty means Dev Art. Each of the four is read, decoded and scaled
// into its page independently: **a page that fails for any reason gets the
// generated stand-in and nothing else is affected**, which is why this returns
// nothing to check. A boat with no texture would be a black boat; a boat with a
// stand-in is a boat.
void buildEntitySkins(io::FileSystem& fs, std::string_view packPath, std::vector<u8>* out);

// **Replaces the player's page with a skin from somewhere else**, on an
// already-built sheet.
//
// `buildEntitySkins` reads the *active pack's* `char.png`, which is what the
// Skins screen calls Default. This is every other row on that screen: a skin
// out of a different pack (`fromPack`, so `char.png` is taken out of the
// archive at `path`) or a loose file in `skins/` (`path` is the PNG itself).
//
// **A 64 x 64 skin keeps its top half and loses the rest.** That format and the
// narrow body are both 1.8's; its upper 64 x 32 is exactly the layout this
// version already knows, and the second layer below it is not something
// a1.1.2's `ModelBiped` has anywhere to put. See core/texture/skin_list.hpp.
//
// False leaves the page exactly as it was -- which is the Default skin, so a
// file that vanished between being listed and being chosen costs a fallback and
// not a hole.
bool applyPlayerSkin(io::FileSystem& fs, std::string_view path, bool fromPack,
                     std::vector<u8>* sheet);

// **The same skin as a page of its own**, 64 x 32 RGBA: exactly what
// `applyPlayerSkin` would put in the sheet, without a sheet. For the main
// menu's row of characters on the Skins screen, which shows every skin in the
// list at once and must not disturb the one the game is using.
bool decodePlayerSkinPage(io::FileSystem& fs, std::string_view path, bool fromPack,
                          std::vector<u8>* page);

// `art/kz.png` into its own 256 x 256 plane, or the generated stand-in.
//
// The art sheet is not scaled to a canonical size the way the four pages are,
// because it already has one: `er` -- EnumArt -- indexes it in absolute texels
// and every painting's rectangle is compiled from that table. A pack whose
// kz.png is not 256 x 256 is rescaled to it rather than refused.
void buildArtSheet(io::FileSystem& fs, std::string_view packPath, std::vector<u8>* out);

// The stand-ins, exposed for the same reason `buildDevArt` is: they are what
// Dev Art draws with, and the host suite checks them.
//
// **A different shape rather than a different palette**, following
// buildDevArtItems: each page is a flat body with its own hue and a one-texel
// grid over it, so a model textured from the wrong page is obvious and a model
// whose UVs are transposed shows it in the grid.
void buildDevArtSkins(std::vector<u8>* out);
void buildDevArtArt(std::vector<u8>* out);

}  // namespace mc::texture
