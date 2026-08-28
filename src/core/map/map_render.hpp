#pragma once

// Turning stored samples into pixels.
//
// **The shading is `MapData.updateVisitedBlocks`, at scale 0.** a1.1.2 has
// nothing to copy here, so the specification is the later game's, and following
// it is what makes this recognisably a Minecraft map rather than a
// height-coloured blob:
//
//   * A pixel is one of three brightnesses of its block's colour.
//   * On land the brightness comes from the step to the block **one to the
//     north**: higher is bright, lower is dark, level is the middle. At scale 0
//     the later code's dither term cannot change the answer -- the height
//     difference is a whole number and the dither is +-0.2 -- so this is
//     exactly that comparison and not an approximation of it.
//   * On water the brightness comes from **depth**, not from the step, on a
//     checkerboard: `depth * 0.1 + ((x + z) & 1) * 0.2`, bright under 0.5, dark
//     over 0.9. That is what makes a shoreline read as a shoreline.
//
// The one place a rule had to be invented: later versions branch on "is this
// block's map colour the water colour", and there is no map-colour table here
// to ask. A **fluid that emits no light** is the water case and a fluid that
// does is the lava case, which is derived from the block table rather than
// written down per version.
//
// **Nothing here allocates or knows what it is drawing into.** The surface is a
// pointer and two strides, so the console passes a pointer into the bottom
// screen's framebuffer -- which is stored in columns, bottom-up -- and a test
// passes an ordinary row-major buffer, and the same code serves both.
//
// ---------------------------------------------------------------------------
// **Drawing and copying are two jobs, and this is where they were split.**
//
// A New 3DS measured one 192 x 192 window at **5,000 microseconds** when every
// pixel was shaded on the way to the screen -- a third of a frame, on every
// block the player crosses. Nothing in the shading was wrong or wasteful; there
// were simply 36,864 of it.
//
// Everything a pixel depends on is either inside its chunk or knowable from the
// chunk's coordinates: its own heights, the row of heights immediately north of
// it, the palette and the grid style. So `renderMapChunk` draws a chunk's
// sixteen-by-sixteen patch once, `refreshMapWindow` keeps the window's patches
// current, and `renderMapWindow` is left with a copy.
// ---------------------------------------------------------------------------

#include "core/map/map_palette.hpp"
#include "core/map/map_store.hpp"
#include "core/util/types.hpp"

namespace mc::map {

// Where the pixels go. `strideX` is the step from a block to the one east of
// it and `strideZ` the step to the one south of it, both in pixels and both
// signed, so a framebuffer that runs up the screen is a negative stride rather
// than a second code path.
struct MapSurface {
    MapPixel* pixels = nullptr;
    int strideX = 1;
    int strideZ = 1;
};

// The patch of world being drawn. The origin is its north-west block, and one
// block is one pixel, so a chunk is always sixteen pixels and a chunk edge is
// always a pixel edge -- which is the whole of "aligned with the chunks".
struct MapWindow {
    i32 originBlockX = 0;
    i32 originBlockZ = 0;
    int width = 0;
    int height = 0;
};

struct MapStyle {
    // Ground that has never been sampled, and ground with nothing in the
    // column at all. Later versions give the second one the air colour, whose
    // colour value is zero, so the two really are the same pixel.
    MapPixel unexplored = 0;

    // Darkens the first row and column of every chunk, using the same 180/255
    // the shading's dark step uses. It is the alignment made visible: chunk
    // edges are where they are whatever the map is doing.
    bool chunkGrid = false;

    // ...and paints the first row and column of every later-version map tile,
    // the 128-block grid `MapData` centres on. Off by default; the spectator
    // screen turns it on.
    bool tileGrid = false;
    MapPixel tileGridColour = 0;
};

// Draws one chunk's patch: 256 pixels in `MapChunkPatch`'s x-major, z-reversed
// order (see map_store.hpp for why that order).
//
// `north` is the chunk immediately north, or null when it has not been sampled.
// It is read for one thing only -- the heights of its last row -- because the
// patch's first row shades against them. Null means that row shades level
// rather than against a height of zero: zero is not a neutral seed, every
// surface is above it, so an unseeded row would come out at the bright step and
// the top edge of the map would flash as the player walked north into ground
// whose northern neighbour had not arrived yet.
void renderMapChunk(const MapChunkSample& sample, const MapChunkSample* north, i32 chunkX,
                    i32 chunkZ, const MapPalette& palette, const MapStyle& style, MapPixel* patch);

// Draws every patch the window touches that is not already drawn with `stamp`,
// and returns how many it drew.
//
// **Not budgeted, deliberately.** In steady play the answer is the one or two
// chunks that were sampled this frame, plus the neighbour to the north of each;
// the only time it is the whole window is when the stamp itself changed, which
// is a texture pack or the grid being toggled, and both are already a moment
// where the screen is expected to stop and think. A budget here would mean the
// map showing the old pack for a few frames, which is worse than the hitch.
int refreshMapWindow(MapStore& store, const MapPalette& palette, const MapWindow& window,
                     const MapStyle& style, u32 stamp);

// Copies the window onto the surface. Every pixel is written -- ground with no
// patch gets `style.unexplored` -- so the caller never has to clear first.
//
// Call `refreshMapWindow` first, or this draws whatever the patches last held.
void renderMapWindow(const MapStore& store, const MapWindow& window, const MapStyle& style,
                     const MapSurface& surface);

// The chunks a window touches, inclusive on both ends. What the caller iterates
// to decide which chunks are worth sampling and which to keep alive in the
// store.
void windowChunkRange(const MapWindow& window, i32* minChunkX, i32* minChunkZ, i32* maxChunkX,
                      i32* maxChunkZ);

// Which of eight compass directions a yaw points at, as an index into
// kFacingStep below. Minecraft's own yaw: 0 looks along +Z, which is south and
// therefore *down* the map, and it increases towards -X, which is west and
// therefore left.
int facingFromYaw(float yawDegrees);

// The step, in blocks, for each of those eight directions. Index 0 is south.
struct MapStep {
    int x;
    int z;
};
extern const MapStep kFacingStep[8];

// ...and what each of them is called, in the same order, so a compass point can
// be printed without a second table beside the one that steps it.
extern const char* const kCompass[8];

// One player on the map: an arrowhead at their position, pointing where they
// are looking.
//
// **It takes a yaw and not a compass point, and that is the whole of the
// change.** What was here before was a diamond with a tick made of whole
// blocks, stepped along one of `kFacingStep`'s eight directions -- so it could
// only ever point eight ways, and because a diagonal block step is the square
// root of two longer than a straight one, the tick grew and shrank as the
// player turned. `gui::drawArrow` rotates a shape instead of stepping one, so
// the marker is the same length at every angle and points at the angle it is
// given. See core/gui/paint.hpp.
//
// **Written for more than one of them from the start.** Multiplayer adds
// callers, not parameters: every other player is this function with their own
// position, yaw and colour.
void drawMarker(const MapSurface& surface, const MapWindow& window, double blockX, double blockZ,
                float yawDegrees, float length, MapPixel fill, MapPixel outline);

// How many steps a yaw is rounded to before the marker is drawn, and the reason
// there are any.
//
// A redraw is a copy of the whole window, so redrawing on every yaw a player's
// thumb can produce would be a copy every frame they are turning. The angle is
// quantised instead, and the same quantised angle is what the marker is drawn
// with, so what is on the screen and what the screen thinks is on it can never
// disagree. 64 steps is 5.6 degrees, which moves the tip of an eight-pixel
// arrow by less than a pixel -- so the rounding is below what the marker can
// draw, and it reads as continuous.
inline constexpr int kYawSteps = 64;

// The step a yaw falls in, 0..kYawSteps-1, and the angle at the middle of that
// step. Negative and multi-turn yaws wrap.
int yawStep(float yawDegrees);
float yawFromStep(int step);

}  // namespace mc::map
