#include "core/gui/progress.hpp"

namespace mc::gui {
namespace {

// The ramp. Black is the console's own backdrop rather than 0x000000, so an
// untouched square sits *in* the panel instead of punching a hole through it;
// everything else is saturated on purpose, because these are six-pixel squares
// and a muted ramp at that size is one grey blur.
constexpr Pixel kPalette[kChunkStateCount] = {
    rgb565(0x1A1A1Au),  // Unstarted
    rgb565(0xC03028u),  // Owed
    rgb565(0xE07818u),  // Working
    rgb565(0xF0D020u),  // Ready
    rgb565(0x80FF20u),  // Done -- the XP bar's green
};

}  // namespace

const Pixel* chunkStatePalette()
{
    return kPalette;
}

int barFillWidth(int w, u32 done, u32 total)
{
    if (w <= 0) {
        return 0;
    }
    // Nothing owed is finished, not empty. It is the answer a world that the
    // pause menu already saved has to give on the way out.
    if (total == 0 || done >= total) {
        return w;
    }
    // 64-bit because `done * w` overflows a u32 at 4 million columns times a
    // 400-pixel bar, and a column count is a u32 that comes off the cache.
    return int((u64(done) * u64(w)) / u64(total));
}

void progressBar(const Surface& surface, int x, int y, int w, int h, u32 done, u32 total,
                 const BarStyle& style)
{
    if (w < 4 || h < 4) {
        return;
    }
    // Frame, then track, then fill: three overlapping rectangles rather than
    // four edges and an interior, because fillRect already clips and this is
    // two lines instead of six.
    fillRect(surface, x, y, w, h, style.frame);

    const int trackX = x + 1;
    const int trackY = y + 1;
    const int trackW = w - 2;
    const int trackH = h - 2;
    fillRect(surface, trackX, trackY, trackW, trackH, style.track);

    const int filled = barFillWidth(trackW, done, total);
    if (filled <= 0) {
        return;
    }
    fillRect(surface, trackX, trackY, filled, trackH, style.fill);
    // The two-tone. Only when there is room for it: at three pixels of track a
    // gloss line and a shade line would leave one pixel of the fill itself.
    if (trackH >= 4) {
        hLine(surface, trackX, trackY, filled, style.gloss);
        hLine(surface, trackX, trackY + trackH - 1, filled, style.shade);
    }
}

int maxGridEdge(int boxW, int boxH)
{
    const int edge = boxW < boxH ? boxW : boxH;
    return edge < 1 ? 0 : edge;
}

GridLayout fitChunkGrid(int boxX, int boxY, int boxW, int boxH, int edge, int maxPitch)
{
    GridLayout layout;
    if (edge <= 0 || boxW <= 0 || boxH <= 0) {
        return layout;
    }
    const int box = boxW < boxH ? boxW : boxH;
    int pitch = box / edge;
    if (pitch < 1) {
        // The square is wider than the box. One pixel per chunk is the floor
        // and the caller was supposed to have clamped its radius against
        // maxGridEdge; drawing is clipped either way, so this cannot write off
        // the end of a framebuffer.
        pitch = 1;
    }
    if (maxPitch > 0 && pitch > maxPitch) {
        pitch = maxPitch;
    }
    layout.pitch = pitch;
    // **The gap is a pixel of the pitch, not an addition to it**, so turning it
    // on cannot make the square outgrow the box it was just fitted to. Below
    // five pixels there is nothing left to spend on it: a 4-pixel pitch with a
    // gap is a 3-pixel chunk, which at distance 12 is the difference between
    // reading the square and squinting at it.
    layout.gap = pitch >= 5 ? 1 : 0;
    layout.edge = edge;

    const int size = layout.size();
    layout.x = boxX + (boxW - size) / 2;
    layout.y = boxY + (boxH - size) / 2;
    return layout;
}

void drawChunkGrid(const Surface& surface, const GridLayout& layout, const ChunkState* cells)
{
    if (!layout.valid() || cells == nullptr) {
        return;
    }
    const int side = layout.pitch - layout.gap;
    if (side <= 0) {
        return;
    }
    for (int row = 0; row < layout.edge; ++row) {
        const ChunkState* line = cells + usize(row) * usize(layout.edge);
        const int y = layout.y + row * layout.pitch;
        for (int column = 0; column < layout.edge; ++column) {
            const int index = int(line[column]);
            // A state this build does not know is drawn as untouched rather
            // than as whatever happens to follow the table in memory.
            const Pixel colour = index >= 0 && index < kChunkStateCount ? kPalette[index]
                                                                        : kPalette[0];
            fillRect(surface, layout.x + column * layout.pitch, y, side, side, colour);
        }
    }
}

}  // namespace mc::gui
