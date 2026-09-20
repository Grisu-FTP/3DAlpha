#!/usr/bin/env python3
"""Draw the SMDH icon and the CIA banner into a directory.

    tools/make_packaging_art.py <out-dir> [title] [version-badge]

3DAlpha ships no Mojang content, and that has to hold for its HOME Menu art as
much as for its textures: this draws the lot from nothing. The palette, the
isometric cube and the text placement are AlphaU's `tools/make_packaging_art.py`
unchanged -- the two projects are the same game on two consoles and should not
look like two different ones on a shelf.

Outputs, at the sizes the 3DS formats require:

    icon.png        48x48   the SMDH icon HOME Menu draws for the title
    smallicon.png   24x24   the one the grid on the bottom screen draws
    banner.png      256x128 the CIA banner, the top screen of HOME Menu
    banner.wav      the banner's jingle; bannertool will not build one without

The build calls this into its own directory, so nothing here is checked in --
the badge is per-version (`MCVER_ICON_BADGE`), and a hand-edited PNG per version
drifts the moment the base art changes. See docs/build-versions.md.

Needs Pillow. The build treats its absence as "no custom art" rather than as an
error, so a fresh clone without it still produces a runnable 3DSX.
"""

import math
import struct
import sys
import wave
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# --- AlphaU's palette, verbatim -------------------------------------------
BG_TOP = (18, 32, 44)
BG_BOTTOM = (8, 14, 20)
FACE_TOP = (86, 196, 186)
FACE_LEFT = (40, 128, 124)
FACE_RIGHT = (26, 92, 96)
TEXT = (232, 240, 238)
MUTED = (140, 170, 168)

# AlphaU draws its icon at 128x128 and its splashes at 720p, where a polygon
# edge lands on a pixel boundary closely enough. The 3DS asks for 48x48, and a
# cube drawn straight into that is a staircase -- so everything is composed at
# SS times the final size and resampled down. The composition is unchanged;
# only the rasteriser gets more to work with.
SS = 8

# The size AlphaU's icon layout is written in terms of. Every number in `icon`
# below is one of its numbers, scaled from this base, so the two stay the same
# picture at whatever size a console happens to want.
ALPHAU_ICON_BASE = 128

# Under this many pixels the version badge stops being text and starts being
# noise, so it is left off. Measured by decoding the 24x24 back out of a built
# SMDH and looking at it, not guessed.
BADGE_LEGIBLE_AT = 32


# **Both of the 3DS's formats for this art are 5 bits a channel** -- the SMDH
# icon is RGB565 and the banner texture is RGBA5551 -- and AlphaU's background is
# a slow dark ramp from (18,32,44) to (8,14,20). Quantised flat, that ramp is
# eight steps, and because red, green and blue cross their thresholds at
# different rows the steps differ in *hue*: it draws as a handful of coloured
# bars, which is exactly what it looked like on a console.
#
# Ordered dithering is the fix, and it keeps the art rather than changing it:
# offsetting each pixel by less than one quantisation step before the tools
# round makes the rounding alternate across the threshold, and the eye puts the
# ramp back. The matrix has to be applied at *final* size -- dithering before a
# resize is just blurred noise -- so it is the last thing done to every image.
BAYER8 = [
    [0, 32, 8, 40, 2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
]

# Five, not six: the green channel gets six bits in RGB565 and five in RGBA5551,
# and dithering for the coarser of the two is right for both. Over-dithering a
# channel by one bit is invisible; under-dithering it is the bar it was drawn to
# remove.
QUANTISED_BITS = 5


def dithered(img: Image.Image) -> Image.Image:
    """AlphaU's colours, nudged so a 5-bit quantiser cannot band them."""
    step = 1 << (8 - QUANTISED_BITS)
    out = img.convert("RGB")
    px = out.load()
    w, h = out.size
    for y in range(h):
        row = BAYER8[y % 8]
        for x in range(w):
            offset = (row[x % 8] / 64.0 - 0.5) * step
            r, g, b = px[x, y]
            px[x, y] = (
                min(255, max(0, int(round(r + offset)))),
                min(255, max(0, int(round(g + offset)))),
                min(255, max(0, int(round(b + offset)))),
            )
    return out


# Where DejaVu Sans Bold sits on the machines this is built on. AlphaU draws
# with it, so finding it is what keeps the two consoles' art identical.
FONT_PATHS = (
    "DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
    "/usr/local/share/fonts/DejaVuSans-Bold.ttf",
    "/Library/Fonts/DejaVuSans-Bold.ttf",
)

_warned = False


def font(size: int) -> ImageFont.ImageFont:
    """DejaVu Sans Bold at `size`, or the best scalable substitute.

    **The fallback takes the size**, which is the whole point of this function
    existing. `ImageFont.load_default()` with no argument returns a fixed ~11px
    face whatever is asked of it; asked for 272px on the supersampled canvas it
    drew a title forty pixels wide, which came out of the resize as a dash and
    shipped to a console that way. A container with no fonts installed -- which
    is every slim build image, including the one CI uses -- is the ordinary case
    here, not an exotic one.
    """
    global _warned
    for name in FONT_PATHS:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    # Anything scalable the system does have, before giving up on the face.
    for name in ("DejaVuSans-Bold", "LiberationSans-Bold", "NimbusSans-Bold",
                 "FreeSansBold", "Arial Bold"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    if not _warned:
        _warned = True
        print("make_packaging_art: no DejaVu Sans Bold found; falling back to "
              "Pillow's built-in face. The art will not match AlphaU's exactly. "
              "Install fonts-dejavu-core (Debian) or ttf-dejavu (Arch).",
              file=sys.stderr)
    try:
        return ImageFont.load_default(size)
    except TypeError:
        # Pillow older than 10.1 has no scalable default at all. Say so rather
        # than quietly drawing a dash.
        raise SystemExit("make_packaging_art: Pillow >= 10.1 or an installed "
                         "font is required; this Pillow cannot scale its "
                         "default face and the text would ship as a smudge.")


def background(w: int, h: int) -> Image.Image:
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOTTOM))
        for x in range(w):
            px[x, y] = c
    return img


def cube(draw: ImageDraw.ImageDraw, cx: float, cy: float, s: float) -> None:
    """An isometric cube of edge `s`, centred on (cx, cy)."""
    dx, dy = s * 0.866, s * 0.5
    top = (cx, cy - s)
    left = (cx - dx, cy - s + dy)
    right = (cx + dx, cy - s + dy)
    mid = (cx, cy - s + 2 * dy)
    left_b = (left[0], left[1] + s)
    right_b = (right[0], right[1] + s)
    mid_b = (mid[0], mid[1] + s)
    draw.polygon([top, right, mid, left], fill=FACE_TOP)
    draw.polygon([left, mid, mid_b, left_b], fill=FACE_LEFT)
    draw.polygon([mid, right, right_b, mid_b], fill=FACE_RIGHT)


def centred_text(draw, y, text, f, fill, w):
    box = draw.textbbox((0, 0), text, font=f)
    draw.text(((w - (box[2] - box[0])) / 2 - box[0], y), text, font=f, fill=fill)


def fitted(draw, text: str, size: int, max_width: float) -> ImageFont.ImageFont:
    """The largest font at or below `size` that draws `text` inside `max_width`.

    The banner's title sits in a fixed panel beside the cube, and the longest
    string it has to hold is not known here -- `MCVER_APP_TITLE` is per version.
    Shrinking to fit is the difference between a longer name looking smaller and
    a longer name running off the edge of the top screen.
    """
    while size > 1:
        f = font(size)
        box = draw.textbbox((0, 0), text, font=f)
        if box[2] - box[0] <= max_width:
            return f
        size -= SS
    return font(size)


def icon(badge: str, size: int) -> Image.Image:
    """AlphaU's icon at whatever size the console asks for.

    The four numbers are its own -- cube centred at (64, 62) with edge 34, badge
    at y = 98 in 18pt -- scaled from its 128x128 base and rasterised large.

    **The badge is dropped below `BADGE_LEGIBLE_AT`.** At 24x24 -- the size the
    HOME Menu grid draws -- "a1.1.2" is four pixels tall and resolves to a grey
    smear under the cube, which reads as a broken icon rather than as a small
    one. A cube alone at that size is a cube; the version is on the 48x48 and in
    the title beside it either way.
    """
    k = size * SS / ALPHAU_ICON_BASE
    img = background(size * SS, size * SS)
    d = ImageDraw.Draw(img)
    if size >= BADGE_LEGIBLE_AT:
        cube(d, 64 * k, 62 * k, 34 * k)
        centred_text(d, 98 * k, badge, font(round(18 * k)), TEXT, size * SS)
    else:
        # Centred, and a little larger, because it is now the whole picture.
        cube(d, 64 * k, 72 * k, 40 * k)
    return dithered(img.resize((size, size), Image.LANCZOS))


def banner(title: str, badge: str) -> Image.Image:
    """The CIA banner: 256x128, which is the one shape AlphaU has no art for.

    Its splash is 16:9 and stacks cube over name over badge. Squeezed into 2:1
    that leaves both ends empty and shrinks the badge to a smudge, so the same
    three pieces are laid out along the banner instead -- cube on the left, name
    and badge beside it. Same palette, same cube, same font; only the placement
    answers to the shape, which is what the splash's own numbers do at 720p.
    """
    w, h = 256 * SS, 128 * SS
    img = background(w, h)
    d = ImageDraw.Draw(img)

    cube(d, 62 * SS, 74 * SS, 32 * SS)

    # **The right margin is insurance, not decoration.** The banner is drawn as
    # a 3D model and the outermost pixels of the texture are the first thing a
    # console's framing eats; text that ends six pixels from the edge is text
    # that can go missing. The working reference this was checked against leaves
    # about a sixth of the width clear, so this does too.
    panel = 104 * SS
    room = w - panel - 26 * SS

    name = fitted(d, title, 34 * SS, room)
    box = d.textbbox((0, 0), title, font=name)
    d.text((panel - box[0], 46 * SS - box[1]), title, font=name, fill=TEXT)

    # The badge sits under the name rather than centred on the panel, so a
    # longer version string grows to the right into space that is empty anyway
    # instead of pushing the name around.
    small = fitted(d, badge, 16 * SS, room)
    box = d.textbbox((0, 0), badge, font=small)
    d.text((panel + 2 * SS - box[0], 84 * SS - box[1]), badge, font=small, fill=MUTED)

    return dithered(img.resize((256, 128), Image.LANCZOS))


def banner_audio(path: Path) -> None:
    """The jingle HOME Menu loops under the banner.

    bannertool will not build a banner without one, and silence is a worse
    answer than four notes: a banner with no audio at all is one of the ways a
    CIA comes out looking unfinished next to a retail title.
    """
    rate = 32728  # what retail banners use
    seconds = 3.0
    # An A minor arpeggio, which is the chord a1.1.2's own menu music opens on.
    notes = [(0.00, 0.30, 440.00), (0.18, 0.30, 523.25),
             (0.36, 0.30, 659.25), (0.54, 0.70, 880.00)]
    n = int(rate * seconds)
    samples = [0.0] * n

    for start, dur, freq in notes:
        i0 = int(start * rate)
        length = int(dur * rate)
        for i in range(length):
            if i0 + i >= n:
                break
            t = i / rate
            env = math.exp(-t * 5.0) * (1.0 - math.exp(-t * 400.0))
            samples[i0 + i] += math.sin(2 * math.pi * freq * t) * env * 0.20

    frames = bytearray()
    for v in samples:
        s16 = int(max(-1.0, min(1.0, v)) * 32767)
        frames += struct.pack("<hh", s16, s16)

    with wave.open(str(path), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(bytes(frames))


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        raise SystemExit(2)

    out = Path(sys.argv[1])
    title = sys.argv[2] if len(sys.argv) > 2 else "3DAlpha"
    badge = sys.argv[3] if len(sys.argv) > 3 else "a1.1.2"

    # The SMDH title is "3DAlpha a1.1.2" and the badge is "a1.1.2", so handing
    # both straight to the banner prints the version twice. The name is what is
    # left when the badge is taken off the end.
    if title.endswith(badge):
        title = title[: -len(badge)].rstrip() or title

    out.mkdir(parents=True, exist_ok=True)

    # **Both sizes now, where before only the 48x48 was drawn.** smdhtool's own
    # downscale of it is a faithful downscale -- that much was measured -- but a
    # faithful downscale of text too small to read is still unreadable, and the
    # 24x24 is the one the HOME Menu grid puts on the bottom screen.
    icon(badge, 48).save(out / "icon.png")
    icon(badge, 24).save(out / "smallicon.png")
    banner(title, badge).save(out / "banner.png")
    banner_audio(out / "banner.wav")
    print(f"wrote {out}/icon.png, smallicon.png, banner.png, banner.wav")


if __name__ == "__main__":
    main()
