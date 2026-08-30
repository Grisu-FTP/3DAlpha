#pragma once

// The screen the player watches while the game is busy: a green bar on the top
// screen, and -- while a world is being made -- a square on the bottom one that
// shows the chunks arriving.
//
// **It replaces two still screens.** Leaving a world used to freeze the last
// frame of it on the top screen and count columns in text on the bottom, and
// creating one used to sit on empty sky printing four lines of streamer
// counters. Both are work rather than a hang, and both are long enough that
// nothing on the screen saying so reads as a crash: a folder world writes a
// file per dirty column on the way out, and a fresh world is tens of
// milliseconds of ARM11 per chunk with a whole render distance to fill.
//
// Three decisions about its shape:
//
//   * **It draws into the renderer's own frame**, exactly as the pause menu
//     does -- `Renderer::drawFrame(camera, this, entry)` -- rather than taking
//     a render target of its own. So the world is still there behind it, under
//     the same scrim the pause menu uses, and both eyes get the bar. See
//     ctr::PauseBackdrop for the argument that settled this.
//   * **The bar's geometry comes from core.** `gui::barFillWidth` decides how
//     much of a track is filled, and both this and the bottom screen's copy of
//     the bar ask it, so the two cannot round differently and be visibly out of
//     step at the ends.
//   * **The square scales with the render distance.** It is drawn at the radius
//     the world is actually being filled to, which is the render distance plus
//     the ring the mesher needs, so at distance 2 it is seven fat cells and at
//     distance 12 twenty-seven small ones -- and `gui::fitChunkGrid` derives
//     the pitch from the box rather than from a constant, so a high distance
//     shrinks the cells instead of running off the screen.
//
// **The bottom screen is the console's**, like every other bottom screen in
// this shell: panels and the square are written into the RGB565 framebuffer by
// the CPU through core/gui/, and the labels are printed on top of them with
// libctru's own glyphs. See hud.hpp.
//
// The system font, not the pack's. The pack's font is two textures of linear
// memory and this is on screen while the renderer holds its pool and its
// atlas; the labels here are six short strings and the system font draws them
// without asking for anything.

#include "core/gui/progress.hpp"
#include "core/util/types.hpp"

#include "platform/ctr/renderer.hpp"

#include <citro2d.h>

namespace mc::ctr {

class ProgressScreen {
public:
    // Which of the two waits this is. It picks the wording and whether there is
    // a square: saving has no chunks arriving to draw.
    enum class Kind {
        Generating,
        Saving,
    };

    // citro2d and a text buffer, and nothing else -- no render target, no card
    // access. C3D_Init must have run. **The Menu must not be up**: both would
    // own citro2d, and the shell already gives the menu back before it starts a
    // world and before it closes one.
    bool init();

    // Parks citro3d's shader pointer before freeing citro2d's program, for the
    // reason spelled out in renderer.hpp's parkShaderProgram.
    void shutdown();
    bool ready() const { return textBuf_ != nullptr; }

    void begin(Kind kind, const char* worldName);

    // What the bar reads. `total` of 0 means "nothing to do", which fills the
    // bar rather than emptying it -- a world the pause menu saved a moment ago
    // owes nothing and has not failed to save.
    void setCounts(u32 done, u32 total);

    // The line under the bar. Null clears it. Borrowed for the length of the
    // next present() only, so a caller may point it at a stack buffer.
    void setNote(const char* note);

    // The square, as `edge * edge` states row-major from the north-west corner
    // -- what WorldStreamer::progressGrid fills. Borrowed, not copied: it is
    // read inside present() and never held. Null turns the square off.
    void setGrid(const gui::ChunkState* cells, int edge);

    // One frame: the world through the renderer with the bar over every eye,
    // and the bottom screen redrawn. Blocks on vsync exactly once, inside
    // Renderer::drawFrame.
    void present(Renderer& renderer, const Camera& camera);

    // The largest radius whose square still fits the box on the bottom screen
    // at one pixel per chunk. A caller clamps against it rather than letting
    // the square run off the edge.
    static int maxGridRadius();

private:
    static void overlayEntry(void* context, C3D_RenderTarget* target);
    void drawTop(C3D_RenderTarget* target);
    void drawBottom();
    void drawBottomChrome(const gui::Surface& surface);

    void label(const char* text, float x, float y, float scale, u32 colour, u32 flags);

    C2D_TextBuf textBuf_ = nullptr;

    Kind kind_ = Kind::Generating;

    // **Copied and truncated, not borrowed.** A world name comes off an SD card
    // and can be anything a PC let somebody type; citro2d does not clip, so a
    // long one drawn centred on the top screen runs off both edges. The menu
    // clips every name it draws for the same reason. Forty characters is what
    // the bottom screen's console row holds, and the top screen's copy is drawn
    // at half scale and fits comfortably.
    char worldName_[41] = {};

    // Borrowed, unlike the name: the only strings passed here are literals in
    // the shell, and it is read inside present() and never held.
    const char* note_ = nullptr;
    u32 done_ = 0;
    u32 total_ = 0;

    const gui::ChunkState* cells_ = nullptr;
    int edge_ = 0;

    // The bottom screen is redrawn from scratch on the first frame and then
    // only where something moved -- the square, the counts and the bar. The
    // panels and the legend do not move, and redrawing them sixty times a
    // second is CPU taken off the thread that is writing or generating.
    bool chromeDrawn_ = false;
    int lastPercent_ = -1;
    u32 lastDone_ = 0xFFFFFFFFu;
};

}  // namespace mc::ctr
