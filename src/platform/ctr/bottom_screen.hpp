#pragma once

// **The bottom screen is drawn off-screen and copied on whole.**
//
// `consoleInit` turns double buffering off, so the bottom framebuffer is the
// buffer the LCD is scanning out *while* the CPU writes it. Every repaint in
// this port clears first and draws second -- `\x1b[2J` blanks the console to
// black, then the backdrop, the panels and the text go over it -- and a scanout
// that lands between the two shows the black. The data cache makes it worse
// rather than better: the writes reach memory in whatever order lines are
// evicted, so what the LCD catches is a scatter of cleared and painted blocks
// rather than a clean wipe. That was the "black pixels" on both the menus and
// the in-game pages.
//
// So nothing writes the framebuffer any more except `present`. The console
// draws into `pixels()` -- `PrintConsole::frameBuffer` is repointed after every
// `consoleInit` -- and so does everything that paints through
// `hud::bottomSurface`. `present` copies the finished picture across in one go
// and flushes it, so the LCD only ever sees a whole old frame or a whole new
// one. The copy can still tear between those two, which is two complete images
// and not a black one.
//
// **When the copy happens.** `flush` is what used to be `gfxFlushBuffers` at
// the end of a paint: copy now. Console text needs no call -- a tap on stdout
// marks the picture changed, and `presentIfChanged` at every frame end puts it
// across. The diagnostic breadcrumbs (`geoTrace`, the out-of-memory line) call
// `flush` too, which is still synchronous, so they are on the glass before the
// next instruction as they were.
//
// **While the menu preview renders the bottom screen on the GPU**, the screen
// is the GPU's: nothing is copied over it, and whatever the console printed
// meanwhile waits in `pixels()`. That also ends a second way to get black
// blocks: a CPU write into the framebuffer the GPU is writing, evicted from the
// cache after the GPU's frame landed.

#include <3ds.h>

namespace mc::ctr::bottom {

// `consoleInit(GFX_BOTTOM, nullptr)`, then the console repointed at the
// off-screen picture, which is cleared the way the console just cleared the
// screen. Call this instead of `consoleInit` everywhere -- an applet
// invalidates the console, and every re-init has to repoint it again.
PrintConsole* initConsole();

// The off-screen picture, in the framebuffer's own layout: RGB565, 240 pixels
// down each column, 320 columns.
u16* pixels();

// Something was drawn into `pixels()`; copy it at the next `presentIfChanged`.
void changed();

// Copy `pixels()` to the framebuffer now, and flush it. The end of a paint.
void flush();

// `flush` if anything changed since the last copy. Called after every frame.
void presentIfChanged();

// True while a GPU render target outputs to the bottom screen.
void setGpuOwned(bool owned);

// How long the last copy and its flush took, in microseconds. **The price of
// the fix**, on the debug page beside the map's redraw: one copy a frame the
// bottom screen changes on, and the map page changes on most.
u32 lastCopyMicros();

}  // namespace mc::ctr::bottom
