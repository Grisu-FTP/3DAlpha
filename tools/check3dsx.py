#!/usr/bin/env python3
"""Answer the only question a 3DSX file gets asked: will Luma's loader take it?

    tools/check3dsx.py build/a1.1.2/3DAlpha-a1.1.2.3dsx [more.3dsx ...]

Nothing on the console tells you why a launch failed. Luma's `loader` does not
print, does not return an error to the Homebrew Launcher, and does not fall
back: `LoadProcess` wraps `hbldrLoadProcess` in `assertSuccess`, and
`assertSuccess` calls `panic`, which is one `__builtin_trap()` -- an undefined
instruction. A rejected 3DSX is therefore indistinguishable, from the outside,
from a crash: black screen, red text, a dump belonging to `loader`.

So the checks live here instead. Every rule below is transcribed from
`sysmodules/loader/source/3dsx.c` and `hbldr.c` in Luma3DS, and the reason each
one is worth checking is that failing it produces that same silent trap.

The one that actually bites is **length**. `Ldr_Get3dsxSize` reads the 32-byte
header with `IFile_Read2`, which returns the byte count and `0` on error, and
compares it to 32. A file that is short -- an interrupted copy to the SD card, a
netload onto a full card, a card pulled before the write flushed -- opens fine
and reads short, and short is the -1 that ends in the trap. The header is
believed without a length check, so a truncated file whose *header* survived is
read past its own end later, in `Ldr_CodesetFrom3dsx`, with the same ending.

Exit status is 0 only if every file would load.
"""

import argparse
import hashlib
import struct
import sys

MAGIC = b"3DSX"
HEADER_READ = 32          # sizeof(_3DSX_Header) in Luma; the extended fields are extra
PAGE = 0x1000


def pages(n):
    return (n + PAGE - 1) & ~(PAGE - 1)


class Bad(Exception):
    pass


def check(path):
    data = open(path, "rb").read()
    size = len(data)
    print(f"=== {path} ===")
    print(f"  {size} bytes, md5 {hashlib.md5(data).hexdigest()}")

    # Ldr_Get3dsxSize, in order.
    if size < HEADER_READ:
        raise Bad(f"file is {size} bytes; loader reads {HEADER_READ} for the header "
                  f"and rejects a short read. This is a truncated file.")
    if data[:4] != MAGIC:
        raise Bad(f"magic is {data[:4]!r}, not {MAGIC!r}")

    (hdr_size, reloc_hdr_size, fmt_ver, flags,
     code, rodata, dseg, bss) = struct.unpack_from("<HHIIIIII", data, 4)

    seg = [pages(code), pages(rodata), pages(dseg)]
    for name, raw, rounded in zip(("code", "rodata", "data"), (code, rodata, dseg), seg):
        # SEC_ASSERT(segSizes[i] >= hdr.xSegSize) -- this is an overflow check,
        # and the only way to fail it is a size within 0xFFF of 4 GB.
        if rounded < raw:
            raise Bad(f"{name} segment size {raw} overflows when page-rounded")

    total = sum(seg) + PAGE     # the extra page loader reserves for argv/_prm
    print(f"  header {hdr_size}, relocHdr {reloc_hdr_size}, formatVer {fmt_ver}, flags 0x{flags:X}")
    print(f"  code   {code:>8}  -> {seg[0]:>8} ({seg[0] // PAGE} pages)")
    print(f"  rodata {rodata:>8}  -> {seg[1]:>8} ({seg[1] // PAGE} pages)")
    print(f"  data   {dseg:>8}  -> {seg[2]:>8} ({seg[2] // PAGE} pages), of which bss {bss}")
    print(f"  loader allocates {total} bytes ({total // PAGE} pages) at 0x10000000")

    if bss > dseg:
        raise Bad(f"bssSize {bss} exceeds dataSegSize {dseg}; the non-bss read length underflows")

    n_tables = reloc_hdr_size // 4
    if 3 * 4 * n_tables > PAGE:
        raise Bad(f"relocHdrSize {reloc_hdr_size} needs more than the one extra page")

    # Ldr_CodesetFrom3dsx reads, in this order, and every read is checked
    # against the length it asked for. Walk the same offsets and require the
    # file to be long enough for each -- that is what a truncated file fails.
    off = hdr_size
    reloc_counts = []
    for i in range(3):
        need(size, off, reloc_hdr_size, f"relocation header {i}")
        reloc_counts.append(struct.unpack_from(f"<{n_tables}I", data, off))
        off += reloc_hdr_size
    for name, length in (("code segment", code), ("rodata segment", rodata),
                         ("data segment", dseg - bss)):
        need(size, off, length, name)
        off += length
    for i, counts in enumerate(reloc_counts):
        for j, n in enumerate(counts):
            if j >= 2:              # not an absolute/relative table; loader skips it
                off += n
                continue
            need(size, off, n * 4, f"segment {i} reloc table {j} ({n} entries)")
            off += n * 4
    print(f"  loader reads {off} of {size} bytes; relocs "
          + ", ".join(f"seg{i}({c[0]}abs,{c[1]}rel)" for i, c in enumerate(reloc_counts)))

    if hdr_size >= 44:
        smdh_off, smdh_size, fs_off = struct.unpack_from("<III", data, 32)
        if smdh_size:
            need(size, smdh_off, smdh_size, "SMDH")
            print(f"  SMDH {smdh_size} bytes at {smdh_off}")
        if fs_off:
            print(f"  RomFS at {fs_off} ({size - fs_off} bytes)")

    print("  OK -- loader would accept this image")


def need(size, off, length, what):
    if off + length > size:
        raise Bad(f"{what} wants bytes [{off},{off + length}) but the file ends at {size}. "
                  f"This file is truncated by at least {off + length - size} bytes.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()

    ok = True
    for path in args.files:
        try:
            check(path)
        except Bad as e:
            ok = False
            print(f"  REJECTED: {e}")
            print("  -> loader panics on this: assertSuccess(hbldrLoadProcess(...)) "
                  "traps, and the dump belongs to `loader`, not to us.")
        except OSError as e:
            ok = False
            print(f"  {e}")
        print()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
