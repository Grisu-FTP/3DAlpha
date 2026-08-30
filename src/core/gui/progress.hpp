#pragma once

// The two pictures the game draws while the player is waiting: a bar that
// fills, and the square that shows a world being made one chunk at a time.
//
// **Both exist because the alternative is a still screen.** Saving a world on
// the way out writes a file per dirty column and making a new one is tens of
// milliseconds of ARM11 per chunk; either is long enough that a frozen frame
// reads as a crash rather than as work. The original says "Saving level.." and
// nothing else, and that was already the weakest thing about leaving a world --
// so this is a deliberate deviation, in the direction the working habits in
// CLAUDE.md point: faithful to the game, not to its limits.
//
// **Software painting, not the GPU**, for the same reason everything else in
// core/gui is: the bottom screen is libctru's text console and pixels beside
// that text are written straight into the RGB565 framebuffer by the CPU. The
// top screen's copy of the bar is drawn with citro2d by the platform layer --
// see platform/ctr/progress_screen.hpp -- and it takes its geometry from
// `barFillWidth` here so the two cannot disagree about where "half way" is.
//
// Nothing here knows what a chunk is beyond the five states below, and nothing
// here allocates: a caller hands in a buffer of states and a rectangle to fit
// them into.

#include "core/gui/paint.hpp"
#include "core/util/types.hpp"

namespace mc::gui {

// **What one square in the grid is doing.** Five states, in the order they
// happen, because that is also the order of the colour ramp: a chunk goes black
// -> red -> orange -> yellow -> green and never backwards, so the square reads
// as a heat map cooling into place rather than as a legend to be memorised.
//
//   * `Unstarted` -- the streamer has not even been told whether the world
//     already has this column. Black: nothing has happened here.
//   * `Owed`      -- it is known to need generating and nothing has begun.
//   * `Working`   -- the generation worker has this column in hand, or the card
//     is reading it. The one state that is a *now*.
//   * `Ready`     -- block data is in memory, but the column has no geometry:
//     it is waiting on its eight neighbours before it can be meshed.
//   * `Done`      -- published to the renderer. It can be drawn.
//
// `Absent` is deliberately not a state of its own. A finite world's edge is a
// column that will never arrive, and drawing it as a sixth colour would mean
// explaining to a player that part of the square is never going to fill. It is
// `Done` instead -- there is nothing owed there, which is what the square is
// actually reporting.
enum class ChunkState : u8 {
    Unstarted = 0,
    Owed,
    Working,
    Ready,
    Done,
};

inline constexpr int kChunkStateCount = 5;

// The ramp, indexed by `ChunkState`. Written down as 0x00RRGGBB and packed
// once, because every colour in this project is written that way.
//
// The greens are the XP bar's own -- a1.1.2 draws it at RGB (128, 255, 32),
// read out of the sprite sheet rather than guessed -- so the square and the bar
// finish in the same colour the game already uses for "full".
const Pixel* chunkStatePalette();

// **The bar.** A black frame, a dark track, and a fill with a bright top edge
// and a darker bottom one, which is what makes it read as a bar of light rather
// than as a coloured rectangle -- the same two-tone the XP bar has.
struct BarStyle {
    Pixel frame = rgb565(0x000000u);
    Pixel track = rgb565(0x373737u);
    Pixel fill = rgb565(0x80FF20u);
    Pixel gloss = rgb565(0xC6FF96u);
    Pixel shade = rgb565(0x4C9E10u);
};

// How many pixels of a `w`-wide track are filled at `done` of `total`.
//
// **Its own function because two screens draw the same bar** -- this one into
// the framebuffer, the console's top screen through citro2d -- and a bar that
// rounded differently on each would be visibly out of step at the ends. `total`
// of 0 is full: nothing owed is nothing left to do, which is the answer a world
// that was saved a moment ago has to give.
int barFillWidth(int w, u32 done, u32 total);

// `x`, `y`, `w`, `h` are the outside of the frame, so a caller sizes the whole
// thing and not the track inside it. Clipped like everything else in paint.hpp.
void progressBar(const Surface& surface, int x, int y, int w, int h, u32 done, u32 total,
                 const BarStyle& style = BarStyle{});

// Where a square of `edge` x `edge` chunks lands inside a box.
//
// **The scaling is the whole point.** The render distance is a live setting and
// the square is drawn at the distance the world is actually being made to, so
// at distance 2 it is seven fat cells and at distance 12 it is twenty-seven
// small ones -- and neither may spill off a 320 x 240 screen. The pitch is
// therefore derived from the box rather than fixed, floored at one pixel, and
// the result is centred in whatever is left over.
struct GridLayout {
    int x = 0;     // top-left of the drawn square, inside the box
    int y = 0;
    int pitch = 0; // centre-to-centre of two neighbouring chunks
    int gap = 0;   // of that pitch, how much is left blank
    int edge = 0;  // chunks across

    int size() const { return pitch * edge; }
    bool valid() const { return edge > 0 && pitch > 0; }
};

// The largest `edge` that fits in a box at one pixel per chunk. A caller clamps
// its radius against this rather than letting the square run off the screen.
int maxGridEdge(int boxW, int boxH);

// `maxPitch` stops a small render distance from turning into a handful of
// enormous blocks; 0 means no ceiling.
GridLayout fitChunkGrid(int boxX, int boxY, int boxW, int boxH, int edge, int maxPitch);

// `cells` is `edge * edge` states, row-major from the north-west corner --
// the same order and the same orientation the map uses, so north is up.
void drawChunkGrid(const Surface& surface, const GridLayout& layout, const ChunkState* cells);

}  // namespace mc::gui
