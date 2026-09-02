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
//
// **...and then most of what was left turned out not to be the copy either.**
// Zoom is what exposed it: a hardware redraw at zoom -1 came back at 3,283
// microseconds, four times what the 1:1 blit was believed to cost, which was
// too much to be explained by moving the same 41,600 pixels a different way. It
// was not the pixels. The walk asked `MapStore::patch` for a pointer every time it crossed
// a chunk edge going south -- 208 columns times thirteen chunk rows, **2,704
// hash lookups to obtain 182 distinct answers**, each one a probe into an index
// in front of 2.25 MB of entries that no 3DS cache can hold. Sixteen output
// columns share a chunk column, so the pointers are gathered once per chunk
// column now and the walk indexes an array of 64 of them on the stack.
//
// Measured on the host over the 1,119-chunk reference world, one 208 x 200
// window, sanitised build, before and after:
//
//     zoom          -1        0       +1       +2
//     before      1052 us   389 us   280 us   130 us
//     after        397       86      187       78
//
// The 1:1 copy is **4.5x faster** and it was never the copy that was slow. The
// same change is most of what shrinking cost, and it is why magnifying measures
// *cheaper* than 1:1 -- it reads a quarter of the source columns and block-moves
// three quarters of what it writes.
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

// How far the map may be zoomed, as a power of two, and what a level means.
//
// **Powers of two, so a chunk edge is still a pixel edge at every level.** A
// chunk is sixteen blocks; magnified it is 32 or 64 pixels and shrunk it is
// eight, and all of those are whole numbers. Any other ratio would put the
// grids -- and the alignment claim they exist to show -- half way through a
// pixel at some zoom levels and not others.
//
// **The range is asymmetric because memory is.** Magnifying costs nothing at
// all: the patches stay one pixel per block and the blit repeats them, so +2 is
// free. Shrinking costs the store, because the window covers four times the
// ground per level -- at -1 the window touches about 700 chunks, which is what
// `MapScreen::configure` now sizes the store for, and at -2 it would touch
// 2,600 and 4 MB of patches on a console whose newlib heap has already been
// measured running out (crashlogs/008). One level out is what fits.
inline constexpr int kZoomMin = -1;
inline constexpr int kZoomMax = 2;

// Pixels one block covers, and blocks one pixel covers. Exactly one of the two
// is ever greater than one, which is what lets the render walk use both at once
// without a branch on the sign.
inline int mapPixelsPerBlock(int zoom)
{
    return zoom > 0 ? 1 << zoom : 1;
}

inline int mapBlocksPerPixel(int zoom)
{
    return zoom < 0 ? 1 << -zoom : 1;
}

// How much ground a run of pixels covers at this zoom.
inline int mapWindowBlocks(int pixels, int zoom)
{
    return zoom < 0 ? pixels << -zoom : pixels >> zoom;
}

// The patch of world being drawn. The origin is its north-west block, and at
// `zoom == 0` one block is one pixel, so a chunk is always sixteen pixels and a
// chunk edge is always a pixel edge -- which is the whole of "aligned with the
// chunks".
//
// **Zoom is a property of the window and not of the stored pixels**, and that
// is the decision the whole feature rests on. A chunk's patch is always drawn
// at one pixel per block, so changing zoom invalidates nothing: the store keeps
// every patch it had, `stamp` does not move, and the cost of a zoom step is one
// ordinary redraw rather than the whole window re-shaded. It also means
// magnifying costs no memory, since there is no larger patch to hold.
//
// `width` and `height` are always pixels. The ground covered is
// `mapWindowBlocks(width, zoom)`, and the origin is expected to be a multiple
// of `mapBlocksPerPixel(zoom)` so that the sampled lattice does not shift under
// the player as they walk.
struct MapWindow {
    i32 originBlockX = 0;
    i32 originBlockZ = 0;
    int width = 0;
    int height = 0;
    int zoom = 0;
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
//
// **`zoom == 0` is a different function to the rest, and on purpose.** At 1:1 a
// run down a chunk is contiguous and ascending in both the patch and the
// framebuffer, so the whole redraw is one `memcpy` per chunk column -- the path
// the 700-microsecond figure above was measured on, and the one the console
// takes whenever the player has not zoomed. Zoomed, the source lattice no
// longer matches the destination and there is nothing to `memcpy`; magnifying
// gets most of it back by building one output column and copying it to the
// `pixelsPerBlock - 1` identical columns beside it, and shrinking is an honest
// per-pixel walk.
//
// Shrinking **point-samples**: a pixel is the block that lands on it, not an
// average of the blocks around it. Later versions average, and averaging four
// or sixteen already-shaded RGB565 pixels per output pixel is arithmetic this
// console cannot afford 41,600 times on a redraw. What it costs is that a
// one-block feature has an even chance of falling between samples; what it buys
// is that the grids, which sit on chunk and tile origins, always land on the
// lattice and so survive every zoom level intact.
void renderMapWindow(const MapStore& store, const MapWindow& window, const MapStyle& style,
                     const MapSurface& surface);

// The chunks a window touches, inclusive on both ends. What the caller iterates
// to decide which chunks are worth sampling and which to keep alive in the
// store. Reads `window.zoom`, so a shrunk window reports the wider ground it
// actually shows.
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
//
// `length` is in **pixels**, not blocks, so the marker is the same size at
// every zoom level. It is an indicator of where you are and not a thing on the
// ground, and one that grew four times over when the map was magnified would be
// covering the detail the player zoomed in to see.
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
