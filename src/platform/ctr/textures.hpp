#pragma once

// The three things the world shader samples: the block atlas, the lightmap, and
// the fog LUT.
//
// All three are small and none of them is per-frame work. The lightmap is 256
// texels rewritten when the sun moves, which is the entire mechanism that makes
// day/night free -- see docs/3ds-performance.md section 1b. The fog LUT is 128
// entries built once per render distance.

#include "core/texture/atlas_image.hpp"
#include "core/texture/entity_skins.hpp"
#include "core/texture/font.hpp"
#include "core/texture/particle_sheet.hpp"
#include "core/util/types.hpp"

#include <citro3d.h>

namespace mc::ctr {

// 16x16 tiles of 16 px, which is the pre-1.5 terrain.png layout the mesher's
// UVs assume. Aliases of core/texture/atlas_image.hpp rather than a second
// definition: an atlas assembled in core and an atlas uploaded here have to
// agree on its size or the display transfer reads past the buffer.
inline constexpr int kAtlasTilesPerEdge = texture::kAtlasTilesPerEdge;
inline constexpr int kAtlasTilePixels = texture::kAtlasTilePixels;
inline constexpr int kAtlasEdge = texture::kAtlasEdge;

// The widest run of side-by-side tiles `updateTile` will push from one picture.
// Two, because that is what `tileSize = 2` asks for: a fluid's flowing texture
// covers a 2 x 2 block of the atlas, one row of which is one copy.
inline constexpr int kMaxTileRun = 2;

// **How many tiles' worth of staging one frame's pushes may use**, in tiles.
// Two flames, a still and a flowing block for each fluid, and the compass is
// thirteen; sixteen is that with a little room. See `tileStagingUsed_` for why
// a push cannot simply reuse the last one's buffer.
inline constexpr int kTileStagingTiles = 16;

class Atlas {
public:
    // Uploads an assembled atlas -- a pack's terrain.png, or the generated Dev
    // Art, both built by core/texture/. False when there is no VRAM for it,
    // which is fatal for rendering and worth saying so rather than drawing
    // untextured.
    //
    // **The image is R,G,B,A in memory and the GPU wants A,B,G,R.** The
    // reversal happens here, in the one loop that fills the linear staging
    // buffer, and nowhere else. See the note in the implementation.
    bool init(const texture::AtlasImage& image);
    void shutdown();

    // **The item sheet, as a second texture.** `gui/items.png` is the other
    // half of a1.1.2's icon rule -- ids below 256 come off terrain.png and ids
    // at or above it off this one -- and a dropped item on the ground needs it
    // for exactly the same reason a slot on the bottom screen does. It went to
    // the GPU when items became entities; before that only the software
    // rasteriser read it, straight out of `AtlasImage`.
    //
    // **Ordinary linear memory, not VRAM.** The block atlas earns VRAM because
    // every fragment of every chunk samples it; this one is sampled by a
    // handful of quads a frame at most, and VRAM is 6 MB with render targets
    // already in it. A pack with no items.png leaves this unmade, and
    // `hasItems()` says so rather than a bind finding nothing.
    bool initItems(const texture::AtlasImage& image);

    bool hasItems() const { return itemsReady_; }

    // **`gui/icons.png`**, 256 x 256 in ordinary linear memory for the reason
    // the item sheet is: fifty quads a frame at most. Always present in a built
    // atlas -- see core/texture/icon_sheet.hpp -- so `hasIcons()` false means no
    // memory rather than no pack.
    bool initIcons(const texture::AtlasImage& image);
    bool hasIcons() const { return iconsReady_; }
    void bindIcons(int unit) const { C3D_TexBind(unit, const_cast<C3D_Tex*>(&icons_)); }

    // **The two entity sheets**, which are not square and so do not go through
    // `initItems`.
    //
    //   * the entity skin sheet is 256 x 64, holding `item/boat.png`,
    //     `item/cart.png`, `item/sign.png`, `item/arrows.png` and `char.png`
    //     in five fixed pages -- see core/texture/entity_skins.hpp;
    //   * the art sheet is `art/kz.png`, 256 x 256, indexed in absolute texels
    //     by the generated painting table.
    //
    // Both in ordinary linear memory for the same reason `items_` is: a few
    // hundred quads a frame do not earn VRAM. 64 KB and 256 KB.
    //
    // **Neither can fail for want of a pack.** `buildEntitySkins` lays down
    // generated stand-ins before it reads anything, so these are always whole
    // and `hasEntities()` false means "no memory", not "no pack".
    bool initEntitySheets(const texture::AtlasImage& image);

    // **The pack's bitmap font as a world texture.** The menu already uploads
    // one for citro2d; this is the same 128 x 128 sheet bound to the detail
    // pipeline instead, because sign text is geometry in the world and not a
    // 2D overlay. 64 KB. False when the pack has no `default.png`, which is a
    // sign with a blank board rather than an error.
    bool initFont(const texture::FontImage& font);
    bool hasFont() const { return fontReady_; }
    void bindFont(int unit) const { C3D_TexBind(unit, const_cast<C3D_Tex*>(&font_)); }

    // **`particles.png`, the third sheet the particle pass samples.** 128 x 128
    // and 64 KB, in ordinary linear memory for the reason the font is: it is
    // read by a few dozen quads a frame at most, and VRAM is 6 MB with render
    // targets already in it.
    //
    // **This one cannot fail for want of a pack.** `buildParticleSheet` lays
    // down a generated stand-in before it reads anything, so `hasParticles()`
    // false means "no memory", not "no pack" -- and a console that answers
    // false simply does not draw the eight sprite kinds, where the two cut out
    // of terrain.png and gui/items.png carry on.
    bool initParticles(const std::vector<u8>& sheet);
    bool hasParticles() const { return particlesReady_; }
    void bindParticles(int unit) const
    {
        C3D_TexBind(unit, const_cast<C3D_Tex*>(&particles_));
    }

    bool hasEntities() const { return entityReady_; }
    bool hasArt() const { return artReady_; }

    void bindEntity(int unit) const
    {
        C3D_TexBind(unit, const_cast<C3D_Tex*>(&entity_));
    }
    void bindArt(int unit) const { C3D_TexBind(unit, const_cast<C3D_Tex*>(&art_)); }

    void bindItems(int unit) const
    {
        C3D_TexBind(unit, const_cast<C3D_Tex*>(&items_));
    }

    // The debug wireframe, built on first use and kept afterwards.
    //
    // **The PICA cannot rasterise lines.** Its primitive list is triangles,
    // strip, fan and geometry-shader, and there is no polygon mode, so the
    // usual meaning of "wireframe" is not available at all. What is available
    // is that every quad in this renderer maps to exactly one atlas tile, so a
    // second atlas whose every tile is a one-texel outline over transparency
    // draws each quad's border and nothing else. With the alpha test on, the
    // interior is discarded before it writes depth, so the mesh behind shows
    // through -- which is the thing a wireframe is wanted for.
    //
    // It lives in ordinary linear memory rather than VRAM: it is 256 KB, it is
    // a debug view, and the bandwidth that made VRAM worth it for the real
    // atlas does not matter here. False if there was no room, in which case the
    // setting simply refuses to turn on.
    bool ensureWireframe();

    void bind(int unit, bool wireframe = false) const
    {
        C3D_TexBind(unit, const_cast<C3D_Tex*>(wireframe && wireReady_ ? &wire_ : &tex_));
    }

    // **The cube atlas, which is what the cube pass samples** -- and only the
    // cube pass. Every tile a cube face can show, stored as a 3x3 repeat of
    // itself inside an 8-texel gutter of its own edge texels, in a 64x64 slot,
    // so a greedy-merged quad can repeat its tile across the blocks it covers;
    // the PICA's wrap mode belongs to a whole texture and cannot repeat one tile
    // of a shared atlas. 512x512, 1 MB, built by `init`
    // from the same image as the ordinary atlas and laid out by
    // core/mesh/cube_atlas.hpp. **VRAM first, before the ordinary atlas**:
    // this is the texture nearly every fragment of the world samples now.
    //
    // Its wireframe is a second texture of the same layout, made with the other
    // one by `ensureWireframe`: the ordinary outline atlas sampled with cube-
    // atlas UVs would be sixteen tiles of noise.
    void bindCube(int unit, bool wireframe = false) const
    {
        C3D_TexBind(unit,
                    const_cast<C3D_Tex*>(wireframe && cubeWireReady_ ? &cubeWire_ : &cube_));
    }

    bool cubeInVram() const { return cubeInVram_; }

    // **One 16x16 tile, replaced in place.** What makes fire animate.
    //
    // `RenderEngine.updateDynamicTextures` re-uploads a `TextureFX`'s 256
    // texels every frame; on this console the atlas is 256 KB and lives in
    // VRAM, which the CPU cannot store into at all, so "re-upload the tile"
    // has to become a DMA of exactly the tile. That is possible because of how
    // the PICA stores a texture: a 16x16 region is four of its 8x8 tiles, and
    // they sit in memory as **two runs of 512 bytes**. See
    // core/texture/tiled.hpp's `tileRunsFlipped`, which is where the
    // arithmetic lives and where the host suite pins it.
    //
    // So this is two 512-byte copies rather than a quarter-megabyte one.
    // `texels` is 16 x 16 x 4 bytes, R,G,B,A in memory order, top row first,
    // which is what `texture::FlameAnimation` produces. False if the atlas is
    // not up or the staging buffer could not be had, in which case the tile
    // simply keeps whatever `applyAnimatedTiles` baked into it.
    //
    // **`across` writes a run of tiles side by side from the one picture**, and
    // that is what a fluid's flowing tile wants: `tileSize = 2` in the original
    // means the same 256 texels land in a 2 x 2 block of the atlas, and two
    // tiles in the same row are **adjacent in memory** -- their runs meet
    // exactly -- so a row of the block is one pair of copies rather than two.
    // Six fluid tiles a tick would otherwise be twelve of them.
    //
    // **Between frames, not during one.** The GPU samples this texture for
    // every fragment of every chunk; the copy has to land while nothing is
    // reading it, which is why the caller runs it beside the lightmap update
    // and not inside the draw.
    bool updateTile(int tile, const u8* texels, int across = 1);

    // **Every tile copy queued so far has finished**, so the staging they read
    // can be handed out again. Only a caller that has just watched
    // `C3D_FrameBegin` drain the GX queue may say so. See `tileStagingUsed_`.
    void tileCopiesRetired() { tileStagingUsed_ = 0; }

    // The same thing on the **items** sheet, which is what the compass needs.
    //
    // `gui/items.png` is tiled and flipped exactly as the block atlas is -- it
    // has to be, because the mesher's UV convention does not know which sheet
    // it is sampling -- so a tile in it is the same two runs of 512 bytes and
    // the same arithmetic. The only difference is that this one is in ordinary
    // linear memory, so the two copies are `memcpy` rather than a GPU texture
    // copy. See core/texture/compass_fx.hpp.
    bool updateItemsTile(int tile, const u8* texels);

    usize bytes() const { return usize(kAtlasEdge) * kAtlasEdge * 4; }
    bool inVram() const { return inVram_; }

private:
    // The body of both `updateTile` and `updateItemsTile`: one run of 16 x 16
    // tiles into one tiled, vertically flipped RGBA8 texture.
    bool updateTileOf(C3D_Tex* target, bool live, bool vram, int tile, const u8* texels,
                      int across);

    // A run of `across` 16 x 16 blocks of `edge`-wide texture, starting at
    // 16-texel column and row -- atlas tiles, or 16 x 16 blocks of a slot in
    // the cube atlas. Every tile in the run gets the same picture.
    bool writeTile(C3D_Tex* target, bool vram, u32 column, u32 row, u32 edge, const u8* texels,
                   u32 across = 1);

    // Builds and uploads the cube atlas from the image, a slot row at a time
    // through `staging`, which has to hold at least kCubeBandBytes.
    // Two staging buffers, alternated band by band, and that is load-bearing
    // rather than tidy: see the note in `initCube`. `spare` is the buffer the
    // ordinary atlas never touched; `shared` is the one it was uploaded from.
    bool initCube(const texture::AtlasImage& image, u32* spare, u32* shared);

    // One RGBA8 texture of any power-of-two size, tiled and flipped the way
    // every other sheet here is. The three `init*` above would otherwise be
    // three copies of the same twenty lines.
    static bool uploadSheet(C3D_Tex* tex, const u8* pixels, int width, int height);

    C3D_Tex tex_{};
    C3D_Tex items_{};
    C3D_Tex icons_{};
    bool iconsReady_ = false;
    C3D_Tex entity_{};
    C3D_Tex art_{};
    C3D_Tex font_{};
    C3D_Tex particles_{};
    C3D_Tex wire_{};
    C3D_Tex cube_{};
    C3D_Tex cubeWire_{};
    // Linear memory, because it is what the GPU reads when the destination is
    // VRAM. One allocation, made the first time a tile is pushed and kept for
    // the life of the atlas; 16 KB, `kTileStagingTiles` tiles' worth.
    u32* tileStaging_ = nullptr;

    // **How much of `tileStaging_` this frame's pushes have taken**, in tiles,
    // and the reason a push may not just reuse the start of the buffer.
    //
    // Read out of `libcitro3d.a`: outside a frame `C3D_SyncTextureCopy` waits
    // for the GX queue to empty, *then* queues its copy, starts it, and waits
    // on `GSPGPU_EVENT_PPF` with `nextEvent = false` -- which returns at once
    // when the last frame's display transfer left a PPF unconsumed. So the
    // wait that makes a copy safe to follow happens at the start of the *next*
    // copy, and `writeTile` fills its staging before it gets there. A push that
    // reused the buffer overwrote the words the previous push's second copy was
    // still reading. With two flames that was half of one tile; with six fluid
    // runs straight behind them it would put water on the fire.
    //
    // So each push takes fresh words, and they are handed back only by
    // `tileCopiesRetired`, after `C3D_FrameBegin` has drained the queue. A push
    // that finds the pool spent is refused and the tile keeps its last picture.
    int tileStagingUsed_ = 0;
    bool ready_ = false;
    bool itemsReady_ = false;
    bool entityReady_ = false;
    bool artReady_ = false;
    bool fontReady_ = false;
    bool particlesReady_ = false;
    bool wireReady_ = false;
    bool inVram_ = false;
    bool cubeReady_ = false;
    bool cubeWireReady_ = false;
    bool cubeInVram_ = false;
};

// u is block light 0..15, v is sky light 0..15 -- exactly the two nibbles the
// vertex carries. 1 KB, so it stays in the texture cache whatever else the frame
// is doing.
class Lightmap {
public:
    bool init();
    void shutdown();

    // Rewrites the 256 texels for Alpha's sky-light subtraction, an integer
    // 0..11 from mc::world::skyLightSubtracted. Skipped when it has not
    // changed, which is all but twelve frames a day.
    void setSkyDarken(int subtracted);

    void bind(int unit) const { C3D_TexBind(unit, const_cast<C3D_Tex*>(&tex_)); }

private:
    C3D_Tex tex_{};
    bool ready_ = false;
    int lastSubtracted_ = -1;
};

// There is no FogLut here any more, and that is a reversal worth keeping.
//
// a1.1.2's terrain fog is linear from renderDistance * 0.25 to renderDistance
// (iq.class), and the PICA has no fog equation -- only a 128-entry LUT. The
// natural move is to sample the line into that LUT, which is what this class
// did. It cannot work: the LUT is indexed by **window depth**, which is 1/d,
// and with a 0.05-block near plane against a 176-block far plane the whole
// 40-to-160-block ramp falls inside the first of the 128 entries. See
// docs/3ds-performance.md section 5. Fog is now a line in the vertex shader
// and an INTERPOLATE combiner stage; renderer.cpp holds both halves.

}  // namespace mc::ctr
