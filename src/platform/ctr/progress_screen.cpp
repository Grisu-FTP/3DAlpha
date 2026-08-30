#include "platform/ctr/progress_screen.hpp"

#include "platform/ctr/hud.hpp"

#include <3ds.h>

#include <cstdio>

namespace mc::ctr {
namespace {

constexpr float kTopWidth = 400.0f;

// The top screen's bar. 280 of the 400 pixels, which leaves the ends clear of
// the curve of the screen and is wide enough that one column of a 729-column
// world moves it by a visible amount.
constexpr float kBarX = 60.0f;
constexpr float kBarY = 132.0f;
constexpr float kBarW = 280.0f;
constexpr float kBarH = 20.0f;

// The same colours the software bar uses, as citro2d wants them. Written out
// rather than converted from gui::BarStyle, because that one is packed RGB565
// and unpacking it back to eight bits per channel would be a lossy round trip
// to arrive where these already are.
constexpr u32 kBarFrame = 0xFF000000u;   // ABGR, which is C2D_Color32's order
constexpr u32 kInk = 0xFFFFFFFFu;
constexpr u32 kInkDim = 0xFFB4B4B4u;
constexpr u32 kShadow = 0xC0000000u;

u32 colour(u8 r, u8 g, u8 b, u8 a = 0xFF)
{
    return C2D_Color32(r, g, b, a);
}

// The bottom screen, in two columns: the square on the left and a panel of
// legend and numbers on the right. Both are inside the body band, so the
// heading rows above them are the console's own text.
constexpr int kGridBoxX = 8;
constexpr int kGridBoxY = 26;
constexpr int kGridBoxW = 184;
constexpr int kGridBoxH = 184;
constexpr int kGridInset = 3;

constexpr int kSideX = 200;
constexpr int kSideY = 26;
constexpr int kSideW = 112;
constexpr int kSideH = 184;

// The wide panel the save screen gets instead, since it has no square.
constexpr int kWidePanelX = 24;
constexpr int kWidePanelY = 72;
constexpr int kWidePanelW = 272;
constexpr int kWidePanelH = 96;

// **Every y below is a multiple of eight, and that is not tidiness.** The
// console's glyphs can only start on a character cell, so a label whose panel
// row is not a multiple of eight is a label that does not line up with the
// swatch or the bar beside it. The rows are named in the console's own 1-based
// addressing and the pixel is derived from them, rather than the other way
// round.
constexpr int kRowY(int row) { return (row - 1) * hud::kCell; }

// A ceiling on the pitch, because the low render distances would otherwise fill
// the box with a handful of enormous blocks -- distance 2 is five cells across,
// which without this is 35 pixels each and reads as a mistake rather than as a
// small world. The square shrinks away from the box edges instead.
constexpr int kMaxPitch = 16;

constexpr int kLegendRows = 5;

// The side panel, in console rows. Legend from row 5 down every other row, the
// counts under it, and the bar between the two numbers -- 26 to 210 in pixels,
// which is rows 5 to 26 with a row of panel above and below.
constexpr int kSideColumns = kSideW / hud::kCell;               // 14
constexpr int kLegendTopRow = 5;
constexpr int kLegendTextColumn = (kSideX + 24) / hud::kCell + 1;
constexpr int kSideCountRow = 17;
constexpr int kSideBarY = 144;
constexpr int kSidePercentRow = 21;

// The same three, on the save screen's wide panel.
constexpr int kWideColumns = kWidePanelW / hud::kCell;          // 34
constexpr int kWideCountRow = 11;
constexpr int kWideBarY = 96;
constexpr int kWidePercentRow = 16;

// **The layout, checked at compile time rather than on a console.**
//
// Every number above was chosen by hand against a 320 x 240 screen, and the two
// ways a hand-chosen layout goes wrong -- something off the edge, or two things
// on top of each other -- are both arithmetic. Checking them here means a later
// edit to any one constant is a build failure on the target rather than a
// column of text drawn through the bar on a console nobody has in hand.
static_assert(kGridBoxX + kGridBoxW <= kSideX, "the square overlaps the side panel");
static_assert(kSideX + kSideW <= hud::kScreenWidth, "the side panel runs off the screen");
static_assert(kGridBoxY + kGridBoxH <= hud::kScreenHeight, "the square runs off the screen");
static_assert(kSideY + kSideH <= hud::kScreenHeight, "the side panel runs off the screen");
static_assert(kWidePanelX + kWidePanelW <= hud::kScreenWidth, "the wide panel runs off");
static_assert(kWidePanelY + kWidePanelH <= hud::kScreenHeight, "the wide panel runs off");

// Legend, counts, bar and percentage, in that order down the side panel, with
// nothing touching what is above or below it.
static_assert(kRowY(kLegendTopRow) >= kSideY, "the legend starts above its panel");
static_assert(kRowY(kLegendTopRow + (kLegendRows - 1) * 2) + hud::kCell <= kRowY(kSideCountRow),
              "the legend runs into the counts");
static_assert(kRowY(kSideCountRow) + hud::kCell <= kSideBarY, "the counts run into the bar");
static_assert(kSideBarY + 14 <= kRowY(kSidePercentRow), "the bar runs into the percentage");
static_assert(kRowY(kSidePercentRow) + hud::kCell <= kSideY + kSideH,
              "the percentage runs off the panel");

static_assert(kRowY(kWideCountRow) >= kWidePanelY, "the counts start above the wide panel");
static_assert(kRowY(kWideCountRow) + hud::kCell <= kWideBarY, "the counts run into the bar");
static_assert(kWideBarY + 14 <= kRowY(kWidePercentRow), "the bar runs into the percentage");
static_assert(kRowY(kWidePercentRow) + hud::kCell <= kWidePanelY + kWidePanelH,
              "the percentage runs off the wide panel");

// The footer, which is the one row of text outside either panel.
static_assert(kRowY(hud::kRows - 1) >= kGridBoxY + kGridBoxH, "the hint overlaps the square");
static_assert(kRowY(hud::kRows - 1) >= kWidePanelY + kWidePanelH, "the hint overlaps the panel");

// Top to bottom, which is finished to untouched: a player looks for the green
// first, and a legend that reads downwards from it is the one that answers
// "what is the red" without being read from the bottom up.
constexpr gui::ChunkState kLegendOrder[kLegendRows] = {
    gui::ChunkState::Done, gui::ChunkState::Ready, gui::ChunkState::Working,
    gui::ChunkState::Owed, gui::ChunkState::Unstarted,
};

constexpr const char* kLegendText[kLegendRows] = {
    "drawn", "lit", "making", "owed", "empty",
};

}  // namespace

bool ProgressScreen::init()
{
    // **The buffer is per frame, not per scene**, and this screen draws on both
    // eyes -- so everything below is spent twice at any slider setting above
    // zero. The busiest frame is five labels of about a hundred glyphs between
    // them, each drawn twice for its shadow, plus a dozen rectangles: a little
    // over four hundred objects an eye. 1024 leaves room for a world name at
    // the length the console row allows; 512 is the fallback, because a screen
    // that will not open is worse than one that is thin on room, and at slider
    // zero there is only one eye and the question does not arise. The same
    // reasoning, and the same fallback, as Menu::initOverlay.
    if (!C2D_Init(1024) && !C2D_Init(512)) {
        return false;
    }
    C2D_Prepare();

    textBuf_ = C2D_TextBufNew(512);
    if (textBuf_ == nullptr) {
        shutdown();
        return false;
    }
    return true;
}

void ProgressScreen::shutdown()
{
    // **Before C2D_Fini**, which frees citro2d's shader program while citro3d
    // is still holding the pointer to it -- see parkShaderProgram in
    // renderer.hpp. This screen sits between the renderer's frames on both
    // sides, so it is exactly the case that crash came out of.
    parkShaderProgram();

    if (textBuf_ != nullptr) {
        C2D_TextBufDelete(textBuf_);
        textBuf_ = nullptr;
    }
    C2D_Fini();
}

int ProgressScreen::maxGridRadius()
{
    const int edge = gui::maxGridEdge(kGridBoxW - kGridInset * 2, kGridBoxH - kGridInset * 2);
    return edge <= 0 ? 0 : (edge - 1) / 2;
}

void ProgressScreen::begin(Kind kind, const char* worldName)
{
    kind_ = kind;
    std::snprintf(worldName_, sizeof(worldName_), "%s", worldName != nullptr ? worldName : "");
    note_ = nullptr;
    done_ = 0;
    total_ = 0;
    cells_ = nullptr;
    edge_ = 0;
    chromeDrawn_ = false;
    lastPercent_ = -1;
    lastDone_ = 0xFFFFFFFFu;
}

void ProgressScreen::setCounts(u32 done, u32 total)
{
    done_ = done;
    total_ = total;
}

void ProgressScreen::setNote(const char* note)
{
    note_ = note;
}

void ProgressScreen::setGrid(const gui::ChunkState* cells, int edge)
{
    cells_ = edge > 0 ? cells : nullptr;
    edge_ = cells_ != nullptr ? edge : 0;
}

void ProgressScreen::overlayEntry(void* context, C3D_RenderTarget* target)
{
    static_cast<ProgressScreen*>(context)->drawTop(target);
}

void ProgressScreen::present(Renderer& renderer, const Camera& camera)
{
    // The bottom screen first, so its CPU work happens before drawFrame parks
    // the thread on the next VBlank rather than after it.
    drawBottom();
    renderer.drawFrame(camera, this, ready() ? &ProgressScreen::overlayEntry : nullptr);
}

void ProgressScreen::label(const char* text, float x, float y, float scale, u32 ink, u32 flags)
{
    C2D_Text parsed;
    C2D_TextParse(&parsed, textBuf_, text);
    C2D_TextOptimize(&parsed);
    C2D_DrawText(&parsed, C2D_WithColor | flags, x + 1.0f, y + 1.0f, 0.4f, scale, scale,
                 kShadow);
    C2D_DrawText(&parsed, C2D_WithColor | flags, x, y, 0.5f, scale, scale, ink);
}

void ProgressScreen::drawTop(C3D_RenderTarget* target)
{
    // citro2d's whole state over the renderer's, and the depth test off. Both
    // are the pause menu's reasoning verbatim -- see Menu::drawOverlay: the
    // buffer under this holds the world's depths, written by a pass whose test
    // is the other way round, so a bar that respected them would have terrain
    // through it.
    C2D_Prepare();
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
    C3D_AlphaTest(false, GPU_ALWAYS, 0x00);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

    C2D_SceneBegin(target);
    C2D_TextBufClear(textBuf_);

    // The original's own scrim over an open world: a gradient from 0xC0101010
    // to 0xD0101010, the same two alphas GuiScreen uses and the same ones the
    // pause menu draws.
    C2D_DrawRectangle(0.0f, 0.0f, 0.05f, kTopWidth, 240.0f, colour(0x10, 0x10, 0x10, 0xC0),
                      colour(0x10, 0x10, 0x10, 0xC0), colour(0x10, 0x10, 0x10, 0xD0),
                      colour(0x10, 0x10, 0x10, 0xD0));

    const bool saving = kind_ == Kind::Saving;
    // "Saving level.." is the original's wording, from the screen reached by
    // Save and quit to title. The other is ours: a1.1.2 says "Building terrain"
    // there, which is a lie on a console that is generating rather than
    // building anything.
    label(saving ? "Saving level.." : "Generating world", kTopWidth * 0.5f, 78.0f, 0.8f, kInk,
          C2D_AlignCenter);
    if (worldName_[0] != '\0') {
        label(worldName_, kTopWidth * 0.5f, 104.0f, 0.5f, kInkDim, C2D_AlignCenter);
    }

    // The bar. Frame, track, fill, and the two-tone that makes the fill read as
    // light -- the same four rectangles core/gui/progress.cpp paints, in the
    // same order, with the fill width from the same function so the two screens
    // cannot disagree about where half way is.
    C2D_DrawRectSolid(kBarX, kBarY, 0.1f, kBarW, kBarH, kBarFrame);
    const float trackX = kBarX + 2.0f;
    const float trackY = kBarY + 2.0f;
    const float trackW = kBarW - 4.0f;
    const float trackH = kBarH - 4.0f;
    C2D_DrawRectSolid(trackX, trackY, 0.2f, trackW, trackH, colour(0x37, 0x37, 0x37));

    const float filled = float(gui::barFillWidth(int(trackW), done_, total_));
    if (filled > 0.0f) {
        C2D_DrawRectSolid(trackX, trackY, 0.3f, filled, trackH, colour(0x80, 0xFF, 0x20));
        C2D_DrawRectSolid(trackX, trackY, 0.4f, filled, 3.0f, colour(0xC6, 0xFF, 0x96));
        C2D_DrawRectSolid(trackX, trackY + trackH - 3.0f, 0.4f, filled, 3.0f,
                          colour(0x4C, 0x9E, 0x10));
    }

    char text[64];
    const int percent =
        total_ == 0 ? 100 : int((u64(done_ > total_ ? total_ : done_) * 100u) / u64(total_));
    std::snprintf(text, sizeof(text), "%d%%", percent);
    label(text, kTopWidth * 0.5f, kBarY + kBarH + 8.0f, 0.6f, kInk, C2D_AlignCenter);

    if (total_ == 0) {
        // The answer to "did pressing START a moment ago already do this", and
        // it is yes. Without it a world the pause menu saved shows a full bar
        // and no reason for it.
        std::snprintf(text, sizeof(text), "%s", saving ? "already saved" : "nothing to make");
    } else {
        std::snprintf(text, sizeof(text), "%lu of %lu chunks",
                      (unsigned long)(done_ > total_ ? total_ : done_), (unsigned long)total_);
    }
    label(text, kTopWidth * 0.5f, kBarY + kBarH + 32.0f, 0.5f, kInkDim, C2D_AlignCenter);

    if (note_ != nullptr) {
        label(note_, kTopWidth * 0.5f, 202.0f, 0.5f, kInkDim, C2D_AlignCenter);
    }

    // Handed over before the caller ends the frame or draws the next eye over
    // it; citro2d otherwise holds the batch until its own next flush.
    C2D_Flush();
}

void ProgressScreen::drawBottomChrome(const gui::Surface& surface)
{
    // The console first: `\x1b[2J` blanks every cell, and everything below is
    // drawn over the top of it. The other way round would erase the panels.
    std::printf("\x1b[2J\x1b[1;1H");

    hud::drawBackdrop(surface, nullptr);

    const bool saving = kind_ == Kind::Saving;
    hud::text(1, 1, hud::kColumns, 0x80FF20u, hud::kBackdrop, "%s",
              saving ? "Saving level.." : "Generating world");
    hud::text(2, 1, hud::kColumns, 0x9A9A9Au, hud::kBackdrop, "%s", worldName_);

    if (cells_ == nullptr) {
        hud::panel(surface, kWidePanelX, kWidePanelY, kWidePanelW, kWidePanelH);
        return;
    }

    // The square sits in a dark readout rather than on a panel: the untouched
    // state is nearly black, and a black square on a light grey face reads as a
    // hole punched through the screen.
    hud::readout(surface, kGridBoxX, kGridBoxY, kGridBoxW, kGridBoxH);
    hud::panel(surface, kSideX, kSideY, kSideW, kSideH);

    const gui::Pixel* palette = gui::chunkStatePalette();
    for (int i = 0; i < kLegendRows; ++i) {
        const int row = kLegendTopRow + i * 2;
        const int y = kRowY(row);
        // A one-pixel black surround, so a swatch on the panel's light grey
        // face has an edge whatever colour it is -- the yellow and the grey are
        // otherwise close enough at eight pixels to run together.
        gui::fillRect(surface, kSideX + 8, y, 8, 8, gui::rgb565(0x000000u));
        gui::fillRect(surface, kSideX + 9, y + 1, 6, 6, palette[int(kLegendOrder[i])]);
        hud::text(row, kLegendTextColumn, kSideColumns - 4, hud::kPanelText, hud::kPanelFace,
                  "%s", kLegendText[i]);
    }
}

void ProgressScreen::drawBottom()
{
    gui::Surface surface;
    // False when libctru is not handing back the framebuffer this expects,
    // which is the cue to draw nothing at all rather than to write pixels at
    // computed offsets into whatever is there.
    if (!hud::bottomSurface(&surface)) {
        chromeDrawn_ = false;
        return;
    }

    if (!chromeDrawn_) {
        drawBottomChrome(surface);
        chromeDrawn_ = true;
        lastPercent_ = -1;
        lastDone_ = 0xFFFFFFFFu;
    }

    // **The square is redrawn on every frame it is asked for**, because a
    // chunk's state changes without the counts moving -- red to orange is the
    // worker picking it up, and that is the part of this screen that is alive.
    // It is at most 184 x 184 pixels of fillRect and it is the whole reason the
    // panels around it are not redrawn with it.
    if (cells_ != nullptr) {
        const gui::GridLayout layout =
            gui::fitChunkGrid(kGridBoxX + kGridInset, kGridBoxY + kGridInset,
                              kGridBoxW - kGridInset * 2, kGridBoxH - kGridInset * 2, edge_,
                              kMaxPitch);
        gui::drawChunkGrid(surface, layout, cells_);
    }

    const int percent =
        total_ == 0 ? 100 : int((u64(done_ > total_ ? total_ : done_) * 100u) / u64(total_));
    if (percent != lastPercent_ || done_ != lastDone_) {
        lastPercent_ = percent;
        lastDone_ = done_;

        // Where the numbers go depends on whether the square took the left half
        // of the screen.
        const bool wide = cells_ == nullptr;
        const int panelX = wide ? kWidePanelX : kSideX;
        const int panelW = wide ? kWidePanelW : kSideW;
        const int barY = wide ? kWideBarY : kSideBarY;
        const int countRow = wide ? kWideCountRow : kSideCountRow;
        const int percentRow = wide ? kWidePercentRow : kSidePercentRow;
        const int column = panelX / hud::kCell + 2;
        const int columns = (wide ? kWideColumns : kSideColumns) - 4;

        hud::text(countRow, column, columns, hud::kPanelText, hud::kPanelFace, "%lu / %lu",
                  (unsigned long)(done_ > total_ ? total_ : done_), (unsigned long)total_);
        gui::progressBar(surface, panelX + 8, barY, panelW - 16, 14, done_, total_);
        hud::text(percentRow, column, columns, hud::kPanelText, hud::kPanelFace, "%d%%",
                  percent);
    }

    if (note_ != nullptr) {
        hud::text(hud::kRows - 1, 1, hud::kColumns, 0x9A9A9Au, hud::kBackdrop, "%s", note_);
    }

    // The CPU has just written a buffer the LCD reads by DMA.
    gfxFlushBuffers();
}

}  // namespace mc::ctr
