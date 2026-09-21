#!/usr/bin/env python3
"""Set the SMDH flags smdhtool leaves at zero.

    tools/smdh_flags.py <file.smdh>

devkitPro's `smdhtool --create` writes `flags = 0` and offers no way to change
it. For a 3DSX that is harmless -- the Homebrew Launcher reads the icon and the
name and nothing else -- but an SMDH inside a **CIA** is read by HOME Menu,
which is told by these bits whether the title may be shown at all. Shipping
zero there means shipping a title that claims to be invisible, uses no 3D and
records no play time, which is not what this is.

The three that apply, from 3dbrew's SMDH flags:

    0x001 Visible       -- may appear on HOME Menu. The important one.
    0x004 Allow3D       -- the renderer is stereoscopic; see docs/3ds-performance.md
    0x100 RecordUsage   -- show up in the Activity Log, like any other title

Left alone on purpose: AutoBoot, RequireEULA, AutoSaveOnExit (the game saves on
its own schedule, not on exit), UsesSaveData (worlds are plain files on the SD
card, not title save data -- see docs/save-data.md), and New3DS (it runs on an
Old 3DS).

Region is already 0xFFFFFFFF, region-free, which is what smdhtool writes and
what this wants.
"""

import struct
import sys
from pathlib import Path

FLAGS_OFFSET = 0x202C
REGION_OFFSET = 0x2018

VISIBLE = 0x001
ALLOW_3D = 0x004
RECORD_USAGE = 0x100

WANTED = VISIBLE | ALLOW_3D | RECORD_USAGE
REGION_FREE = 0xFFFFFFFF


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: smdh_flags.py <file.smdh>", file=sys.stderr)
        raise SystemExit(2)

    path = Path(sys.argv[1])
    data = bytearray(path.read_bytes())
    if data[:4] != b"SMDH":
        raise SystemExit(f"{path}: not an SMDH (magic {bytes(data[:4])!r})")
    if len(data) < FLAGS_OFFSET + 4:
        raise SystemExit(f"{path}: too short to hold a flags field")

    before = struct.unpack_from("<I", data, FLAGS_OFFSET)[0]
    struct.pack_into("<I", data, FLAGS_OFFSET, before | WANTED)
    struct.pack_into("<I", data, REGION_OFFSET, REGION_FREE)
    path.write_bytes(bytes(data))

    after = before | WANTED
    print(f"{path.name}: flags 0x{before:08x} -> 0x{after:08x} "
          f"(visible, allow3d, recordusage), region-free")


if __name__ == "__main__":
    main()
