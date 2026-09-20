#!/usr/bin/env python3
"""Assert that an SMDH carries the icon we drew, and not a fallback.

    tools/check_smdh.py <file.smdh> [--default <devkitpro default_icon.png>]

**This exists because the obvious check is the wrong one.** A build step that
confirms `art/icon.png` was drawn proves only that Pillow ran: the icon still
has to survive smdhtool and land in the SMDH, and when it does not, what ships
is devkitPro's default -- a picture of a red 3DS -- which installs and runs and
looks deliberate. That went out in a release before anyone noticed, because
nothing between the PNG and the console ever looked at the bytes.

So this reads the icons back out of the SMDH the way the console does, and
fails if either one is the devkitPro default or a flat colour.

The SMDH layout, from 3dbrew: a 0x2040 header, then the 24x24 icon (0x480
bytes) and the 48x48 icon (0x1200 bytes), both RGB565 little-endian in 8x8
Morton-order tiles.
"""

import struct
import sys
from pathlib import Path

from PIL import Image

SMALL_OFFSET = 0x2040
SMALL_SIZE = 24
LARGE_OFFSET = 0x24C0
LARGE_SIZE = 48


def morton(i: int) -> tuple:
    x = y = 0
    for bit in range(3):
        x |= ((i >> (2 * bit)) & 1) << bit
        y |= ((i >> (2 * bit + 1)) & 1) << bit
    return x, y


def decode(data: bytes, size: int) -> Image.Image:
    img = Image.new("RGB", (size, size))
    px = img.load()
    off = 0
    for ty in range(size // 8):
        for tx in range(size // 8):
            for i in range(64):
                x, y = morton(i)
                v = struct.unpack_from("<H", data, off)[0]
                off += 2
                px[tx * 8 + x, ty * 8 + y] = (
                    ((v >> 11) & 0x1F) * 255 // 31,
                    ((v >> 5) & 0x3F) * 255 // 63,
                    (v & 0x1F) * 255 // 31,
                )
    return img


def quantise565(img: Image.Image) -> Image.Image:
    """The same rounding the SMDH format applies, so the two are comparable."""
    out = img.convert("RGB")
    px = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b = px[x, y]
            px[x, y] = ((r >> 3) * 255 // 31, (g >> 2) * 255 // 63, (b >> 3) * 255 // 31)
    return out


def main() -> None:
    args = sys.argv[1:]
    if not args:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        raise SystemExit(2)

    path = Path(args[0])
    default = None
    if "--default" in args:
        default = Path(args[args.index("--default") + 1])

    data = path.read_bytes()
    if data[:4] != b"SMDH":
        raise SystemExit(f"{path}: not an SMDH (magic {data[:4]!r})")
    if len(data) < LARGE_OFFSET + LARGE_SIZE * LARGE_SIZE * 2:
        raise SystemExit(f"{path}: too short to hold both icons ({len(data)} bytes)")

    icons = {
        "24x24": decode(data[SMALL_OFFSET:], SMALL_SIZE),
        "48x48": decode(data[LARGE_OFFSET:], LARGE_SIZE),
    }

    want = None
    if default is not None and default.exists():
        base = Image.open(default).convert("RGB")

    problems = []
    for name, img in icons.items():
        colours = len(img.getcolors(img.width * img.height) or ())
        if colours < 4:
            problems.append(f"{name} is effectively blank ({colours} colours)")
        elif default is not None and default.exists():
            want = quantise565(base.resize(img.size, Image.LANCZOS))
            a = img.tobytes()
            b = want.tobytes()
            same = sum(1 for i in range(0, len(a), 3) if a[i:i + 3] == b[i:i + 3])
            share = same / float(img.width * img.height)
            # Not an exact compare: this resizes the default itself, where
            # smdhtool would have done its own, and the two differ in the last
            # bit. Anything this close is the default icon whatever drew it.
            if share > 0.90:
                problems.append(
                    f"{name} is devkitPro's default icon ({share:.0%} identical)")
        if not problems or not problems[-1].startswith(name):
            print(f"{path.name}: {name} ok, {colours} colours")

    if problems:
        for problem in problems:
            print(f"{path.name}: {problem}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
