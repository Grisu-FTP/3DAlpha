#pragma once

// The three things the world shader samples: the block atlas, the lightmap, and
// the fog LUT.
//
// All three are small and none of them is per-frame work. The lightmap is 256
// texels rewritten when the sun moves, which is the entire mechanism that makes
// day/night free -- see docs/3ds-performance.md section 1b. The fog LUT is 128
// entries built once per render distance.

#include "core/texture/atlas_image.hpp"
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

    usize bytes() const { return usize(kAtlasEdge) * kAtlasEdge * 4; }
    bool inVram() const { return inVram_; }

private:
    C3D_Tex tex_{};
    C3D_Tex wire_{};
    bool ready_ = false;
    bool wireReady_ = false;
    bool inVram_ = false;
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
// and with a 0.2-block near plane against a 176-block far plane the whole
// 40-to-160-block ramp falls inside the first of the 128 entries. See
// docs/3ds-performance.md section 5. Fog is now a line in the vertex shader
// and an INTERPOLATE combiner stage; renderer.cpp holds both halves.

}  // namespace mc::ctr
