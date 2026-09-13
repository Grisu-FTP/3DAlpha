#pragma once

// **The sky: two flat planes, a sun, a moon, and 780 stars.** `e.a(F)V` --
// RenderGlobal.renderSky -- built once into a buffer that never changes again.
//
// a1.1.2's sky has no dome in it and no gradient baked into anything. It is:
//
//   * a **flat plane sixteen blocks above the camera**, drawn in the sky
//     colour with fog on, so the distance fade *is* the horizon gradient;
//   * a **second plane sixteen blocks below**, in a darker colour of its own,
//     which is what you see when you look down off the edge of the world;
//   * a sun quad thirty blocks across at a hundred blocks up, a moon quad
//     twenty across at a hundred blocks down, both added rather than blended;
//   * a star field of 780 billboards on a sphere of radius 100, drawn at
//     `world::starBrightness` and added the same way.
//
// **All of it is camera-relative.** renderSky translates by nothing at all --
// the one `glTranslatef` in it is (0, 0, 0) -- so the planes follow the player
// up and down and the sun is always a hundred blocks away. That is why the sky
// in this version looks like a ceiling rather than a dome, and reproducing the
// dome later versions grew would be a different game's sky.
//
// **Only the celestial half turns.** `glRotatef(celestialAngle * 360, 1, 0, 0)`
// is pushed before the sun and popped after the stars, so sun, moon and stars
// share one rotation about the world's X axis and the two planes do not move at
// all. That is the whole of the day's motion, and it is why this file hands the
// caller two ranges to draw with two different matrices rather than baking an
// angle into anything.
//
// **Nothing here is rebuilt per frame.** The colours are uniforms at the draw
// (see world::skyColour and the renderer's texenv constants) and the rotation is
// a matrix, so the vertices are written once at start-up and read for the life
// of the process. 1,120 quads and 70 KB, of which the stars are 780 and 49 KB.
//
// Recovered from `e.class` (RenderGlobal) in minecraft-a1.1.2_01-client.jar:
//
//   renderSky     e.a(F)V        -- the pass, in the order drawn
//   the planes    e.<init>       -- display lists `z` (sky) and `A` (void)
//   the stars     e.f()V         -- renderStars, seeded with 10842
//
// Obfuscated names are version-specific; these are for a1.1.2_01 only.

#include "core/mesh/vertex.hpp"
#include "core/util/types.hpp"
#include "core/world/daylight.hpp"

namespace mc::render {

// **One vertex unit is 1/64 of a block, and the sky is the only thing in this
// renderer measured that way.**
//
// `mesh::DetailVertex` holds position as a signed short at 1/1024 of a block,
// which reaches 32 blocks -- and the sky plane is 448 blocks across. Sixteen
// units of every 1024 is the smallest scale that spans it (511 blocks), and it
// is the same trick the chat uses: the vertices are written in these units and
// the renderer multiplies the matrix by 1024/64 before it draws them. A
// sixty-fourth of a block is three orders of magnitude finer than anything the
// sky needs -- the smallest feature in it is a quarter-block star at a hundred
// blocks' distance, which is sixteen units across.
inline constexpr int kSkyUnitsPerBlock = 64;

// The grid both planes are made of: **64-block cells from -384 to +448**, which
// is `for (int i = -64 * 6; i <= 64 * 6; i += 64)` twice over, with 6 being
// `256 / 64 + 2` for the original's 256-block far plane.
//
// The cells exist for the fog, which OpenGL computes per vertex: one enormous
// quad would fade linearly across its diagonal instead of radially about the
// camera. 13 x 13 is what the original settled on and the arithmetic that
// produced it is the far plane's, not the horizon's -- so this port keeps the
// grid rather than sizing it to its own shorter view.
inline constexpr int kSkyPlaneCells = 13;
inline constexpr int kSkyPlaneCellBlocks = 64;
inline constexpr int kSkyPlaneQuads = kSkyPlaneCells * kSkyPlaneCells;

// Sixteen blocks above the camera, and sixteen below.
inline constexpr int kSkyPlaneHeightBlocks = 16;

// The sun is 30 blocks to a side at 100 blocks up; the moon 20 at 100 down.
// `f = 30.0F` and `f = 20.0F` in renderSky, each used as a *half*-width -- the
// quad runs -f to +f -- with the distance a literal 100.0D.
inline constexpr float kSunHalfWidthBlocks = 30.0f;
inline constexpr float kMoonHalfWidthBlocks = 20.0f;
inline constexpr float kCelestialDistanceBlocks = 100.0f;

// 1500 candidates, of which 780 land inside the unit sphere and outside its
// 0.01 core. Not a guess: the rejection rule is `d < 1 && d > 0.01` over three
// `nextFloat` draws from `new Random(10842L)`, and the count comes out of that
// stream. tests/sky_vectors.hpp carries it from a run of the original's own
// loop, and tests/sky_test.cpp fails if this number and that one disagree.
inline constexpr int kStarQuads = 780;
inline constexpr float kStarRadiusBlocks = 100.0f;

// **The five ranges, in the order buildSky writes them.** One buffer, because
// none of it ever changes and the renderer draws each range with its own state:
// the planes fogged and opaque, the three celestial ranges added with fog off.
inline constexpr int kSkyPlaneFirst = 0;
inline constexpr int kSkyPlaneVertices = kSkyPlaneQuads * 4;
inline constexpr int kVoidPlaneFirst = kSkyPlaneFirst + kSkyPlaneVertices;
inline constexpr int kVoidPlaneVertices = kSkyPlaneQuads * 4;
inline constexpr int kSunFirst = kVoidPlaneFirst + kVoidPlaneVertices;
inline constexpr int kSunVertices = 4;
inline constexpr int kMoonFirst = kSunFirst + kSunVertices;
inline constexpr int kMoonVertices = 4;
inline constexpr int kStarsFirst = kMoonFirst + kMoonVertices;
inline constexpr int kStarVertices = kStarQuads * 4;
inline constexpr int kSkyVertexCount = kStarsFirst + kStarVertices;

// **The void plane's colour, which is the sky's and is not a colour anyone can
// name.** `glColor3f(r * 0.2F + 0.04F, g * 0.2F + 0.04F, b * 0.6F + 0.1F)` over
// the sky colour of the moment, so it is a fifth of the sky with a floor under
// it and rather more blue than the rest -- dark slate by day, near-black at
// night. Kept here rather than beside the other three because it is the
// *renderer's* line, not the world's: `cn` never computes it, `e.a(F)V` does.
// It takes and returns the world's own colour type rather than one of its own,
// since it is the same three floats in the same units.
world::SkyColour voidPlaneColour(const world::SkyColour& sky);

// **The sky pass has fog of its own.** `iq.a(-1)` -- setupFog with a negative
// pass -- sets the start to 0 and the end to `farPlaneDistance * 0.8`, where
// the world's own fog runs from a quarter of the far plane to all of it. So the
// sky fades sooner and from directly overhead, which is what puts a gradient in
// a flat ceiling.
inline constexpr float kSkyFogStartScale = 0.0f;
inline constexpr float kSkyFogEndScale = 0.8f;

// Fills `out` with exactly kSkyVertexCount vertices and returns how many it
// wrote, or 0 if `max` is too small.
//
// Positions are in kSkyUnitsPerBlock units about the camera; UVs are into the
// entity sheet and mean nothing outside the sun and moon ranges; the colour
// bytes are white and the light byte full, because no range of this pass reads
// either -- the planes and the stars take their colour from a combiner constant
// and the sun and moon from the texture.
int buildSky(mesh::DetailVertex* out, int max);

}  // namespace mc::render
