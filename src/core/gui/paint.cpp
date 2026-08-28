#include "core/gui/paint.hpp"

#include <cmath>

namespace mc::gui {

namespace {

// Clips a run to 0..limit and reports whether anything is left. Both edges move,
// which is why it takes the start by pointer: a rectangle that begins off the
// left of the screen keeps its right-hand part.
bool clipRun(int* start, int* length, int limit)
{
    if (*length <= 0) {
        return false;
    }
    if (*start < 0) {
        *length += *start;
        *start = 0;
    }
    if (*start >= limit) {
        return false;
    }
    if (*start + *length > limit) {
        *length = limit - *start;
    }
    return *length > 0;
}

}  // namespace

void fillRect(const Surface& surface, int x, int y, int w, int h, Pixel colour)
{
    if (!surface.valid()) {
        return;
    }
    if (!clipRun(&x, &w, surface.width) || !clipRun(&y, &h, surface.height)) {
        return;
    }
    Pixel* row = surface.pixels + x * surface.strideX + y * surface.strideY;
    for (int iy = 0; iy < h; ++iy) {
        Pixel* out = row;
        for (int ix = 0; ix < w; ++ix) {
            *out = colour;
            out += surface.strideX;
        }
        row += surface.strideY;
    }
}

void hLine(const Surface& surface, int x, int y, int w, Pixel colour)
{
    fillRect(surface, x, y, w, 1, colour);
}

void vLine(const Surface& surface, int x, int y, int h, Pixel colour)
{
    fillRect(surface, x, y, 1, h, colour);
}

void frameRect(const Surface& surface, int x, int y, int w, int h, Pixel colour)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    hLine(surface, x, y, w, colour);
    hLine(surface, x, y + h - 1, w, colour);
    vLine(surface, x, y + 1, h - 2, colour);
    vLine(surface, x + w - 1, y + 1, h - 2, colour);
}

void bevelBox(const Surface& surface, int x, int y, int w, int h, Pixel face, Pixel light,
              Pixel dark, bool raised)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fillRect(surface, x, y, w, h, face);

    const Pixel topLeft = raised ? light : dark;
    const Pixel bottomRight = raised ? dark : light;
    hLine(surface, x, y, w, topLeft);
    vLine(surface, x, y, h, topLeft);
    // One short at the near end of each, so the two bevels meet on the diagonal
    // instead of one painting over the other's corner.
    hLine(surface, x + 1, y + h - 1, w - 1, bottomRight);
    vLine(surface, x + w - 1, y + 1, h - 1, bottomRight);
}

void tilePattern(const Surface& surface, int x, int y, int w, int h, const Pixel* tile,
                 int tileEdge)
{
    if (!surface.valid() || tile == nullptr || tileEdge <= 0) {
        return;
    }
    if (!clipRun(&x, &w, surface.width) || !clipRun(&y, &h, surface.height)) {
        return;
    }
    // The phase follows the surface rather than the rectangle, so a rectangle
    // drawn in two halves is still one continuous pattern.
    Pixel* row = surface.pixels + x * surface.strideX + y * surface.strideY;
    for (int iy = 0; iy < h; ++iy) {
        const int ty = ((y + iy) % tileEdge + tileEdge) % tileEdge;
        const Pixel* source = tile + ty * tileEdge;
        Pixel* out = row;
        for (int ix = 0; ix < w; ++ix) {
            const int tx = ((x + ix) % tileEdge + tileEdge) % tileEdge;
            *out = source[tx];
            out += surface.strideX;
        }
        row += surface.strideY;
    }
}

void drawArrow(const Surface& surface, float centreX, float centreY, float angle,
               const ArrowShape& shape, Pixel fill, Pixel outline)
{
    if (!surface.valid()) {
        return;
    }

    float length = shape.length;
    if (length < 1.0f) {
        length = 1.0f;
    }
    if (length > kMaxArrowLength) {
        length = kMaxArrowLength;
    }
    const float scale = length / shape.length;
    const float halfWidth = shape.halfWidth * scale;
    const float tail = shape.tail * scale;
    const float notch = shape.notch * scale;

    // The four corners, before rotation, with -y forward. The notch is what
    // makes it read as an arrowhead rather than as a triangle at eight pixels
    // across, and it is what the outline pass then traces into the back.
    const float localX[4] = {0.0f, halfWidth, 0.0f, -halfWidth};
    const float localY[4] = {-length, tail, tail - notch, tail};

    // Clockwise from up: (0, -1) has to come out as (sin a, -cos a).
    const float cosA = std::cos(angle);
    const float sinA = std::sin(angle);
    float pointX[4];
    float pointY[4];
    for (int i = 0; i < 4; ++i) {
        pointX[i] = localX[i] * cosA - localY[i] * sinA;
        pointY[i] = localX[i] * sinA + localY[i] * cosA;
    }

    // The coverage buffer, in whole pixels around the pixel the centre falls
    // in. `reach` covers the tip at any angle and `edge` adds the outline.
    const int reach = int(std::ceil(length)) + 1;
    const int edge = reach + 1;
    const int span = edge * 2 + 1;
    constexpr int kMaxEdge = int(kMaxArrowLength) + 2;
    constexpr int kMaxSpan = kMaxEdge * 2 + 1;
    bool body[kMaxSpan * kMaxSpan] = {};

    const int baseX = int(std::floor(centreX));
    const int baseY = int(std::floor(centreY));
    // The sub-pixel part of where the arrow actually is, so a marker at x.5
    // does not jump a whole pixel as the player walks.
    const float offsetX = centreX - float(baseX);
    const float offsetY = centreY - float(baseY);

    // **Sampled four by four inside each pixel, not once at its centre.**
    // A ten-pixel arrow tested at pixel centres is a different shape at every
    // angle -- a corner that falls a tenth of a pixel the wrong side of a
    // centre takes a whole pixel with it, and the marker comes out lopsided in
    // a way that changes as the player turns. Sixteen samples and a half-cover
    // rule is the standard answer, it keeps the edges hard, which is what a map
    // drawn one pixel to the block wants, and it costs 16 tests on about 200
    // pixels once per redraw.
    constexpr int kSubSamples = 4;
    constexpr float kSubStep = 1.0f / float(kSubSamples);
    constexpr int kHalfCover = kSubSamples * kSubSamples / 2;

    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            int covered = 0;
            for (int sy = 0; sy < kSubSamples; ++sy) {
                const float py = float(dy) + (float(sy) + 0.5f) * kSubStep - offsetY;
                for (int sx = 0; sx < kSubSamples; ++sx) {
                    const float px = float(dx) + (float(sx) + 0.5f) * kSubStep - offsetX;

                    // Even-odd crossing, which is the test the notch needs: the
                    // polygon is concave, so a convex half-plane test would
                    // fill the bite back in.
                    bool inside = false;
                    for (int i = 0, j = 3; i < 4; j = i++) {
                        const bool straddles = (pointY[i] > py) != (pointY[j] > py);
                        if (!straddles) {
                            continue;
                        }
                        const float t = (py - pointY[i]) / (pointY[j] - pointY[i]);
                        if (px < pointX[i] + t * (pointX[j] - pointX[i])) {
                            inside = !inside;
                        }
                    }
                    covered += inside ? 1 : 0;
                }
            }
            if (covered >= kHalfCover) {
                body[(dy + edge) * span + (dx + edge)] = true;
            }
        }
    }

    for (int dy = -edge; dy <= edge; ++dy) {
        for (int dx = -edge; dx <= edge; ++dx) {
            const bool inside = body[(dy + edge) * span + (dx + edge)];

            bool ring = false;
            if (!inside) {
                for (int ey = -1; ey <= 1 && !ring; ++ey) {
                    for (int ex = -1; ex <= 1 && !ring; ++ex) {
                        const int ny = dy + ey + edge;
                        const int nx = dx + ex + edge;
                        if (ny >= 0 && ny < span && nx >= 0 && nx < span) {
                            ring = body[ny * span + nx];
                        }
                    }
                }
            }
            if (!inside && !ring) {
                continue;
            }

            Pixel* out = surface.at(baseX + dx, baseY + dy);
            if (out != nullptr) {
                *out = inside ? fill : outline;
            }
        }
    }
}

}  // namespace mc::gui
