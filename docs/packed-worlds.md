# Packed worlds

A per-world storage mode, toggled from the world list. Both modes are fully playable; conversion
either way is lossless.

| Mode | On disk | Good for |
|---|---|---|
| **Folder** (default) | A real Alpha world — `level.dat`, base36 chunk tree | Dropping in from a PC, copying back out, interchange |
| **Packed** | Sector-allocated containers, one per 32×32 chunk region | Playing on the console: 2–5× smaller, and faster to load |

Drop a folder into `sdmc:/3dalpha/saves/` and play it as-is. Pack it when you want the space back.
Unpack it and the original folder returns byte-for-byte.

## What "compression" actually buys here

Chunk files are already gzip streams, so re-compressing them gains close to nothing. The waste is
**cluster slack**: the 3DS is FAT32-only with 32 KB clusters (64 KB at ≥128 GB), and a 12 KB chunk
file still consumes a whole cluster. See [save-data.md](save-data.md).

Packing removes that by putting many chunks inside one file, allocated in **4 KB sectors**. Average
waste falls from ~half a cluster per chunk (16–32 KB) to half a sector (2 KB) — roughly a **10×
reduction in slack**, which lands as the 2–5× total saving from the size table.

So the payloads are stored **verbatim**, exactly the bytes the folder held. That costs no CPU on a
268 MHz ARM11, and it is what makes byte-exact restoration provable rather than hopeful.

An optional second level — recompressing each chunk at a higher gzip level — would gain maybe
10–15% more and give up byte-exact restore. Not worth it by default; noted so the decision is on the
record rather than forgotten.

### The secondary win is speed

Opening a file on the 3DS FS is expensive, and the Alpha layout means one open per chunk plus a walk
through 4,096 directories. A packed region is **one open handle, then seeks** — chunk streaming
should get materially faster, which directly buys render distance and flight speed. Measure it at
M1/M2 and record the number in [3ds-performance.md](3ds-performance.md).

It also makes the chunk-index cache redundant: for a packed world the region header **is** the
index, so `cache/<world>.idx` isn't built at all.

## The state is the disk, not a setting

There is no stored preference to keep in sync. The mode is observable from the folder's shape:

- `level.dat` + base36 directories ⇒ **Folder**
- `world.3dm` present ⇒ **Packed**

That means a folder copied in from a PC is immediately recognised with no registration step, and the
mode can never desync from reality. The world list shows a badge and the true on-disk size for each.

## Layout

### Folder mode

Untouched, canonical Alpha level format — see [world-format.md](world-format.md).

### Packed mode

```
sdmc:/3dalpha/saves/<world>/
  world.3dm            manifest + verbatim store for level.dat and any non-chunk files
  r.0.0.3dr            region container, 32×32 chunks
  r.-1.0.3dr
  README.txt           plain text: this is a packed 3DAlpha world, and how to unpack it
```

**A packed world is deliberately not a valid Minecraft world.** `level.dat` moves inside the
manifest. If it stayed on disk, copying a packed world to a PC and opening it in Minecraft would
find a world with no chunks and cheerfully generate new terrain over it. Making the folder
unopenable by Minecraft is the safer failure: the `README.txt` explains what it is.

The manifest carries a small metadata block — world name, last played, seed, chunk count, packed and
unpacked sizes — so the world list renders without opening any region.

## Region container format

Modelled on McRegion's sector scheme, for the same reason Mojang chose it.

```
r.<rx>.<rz>.3dr        one region = 32 × 32 chunks, 4096-byte sectors

sector 0   header A  ─┐ two copies, each { magic "3DR1", version, sectorSize,
sector 1   header B  ─┘   generation u64, dirSlot, freeMapSector, crc32 }
sector 2+  directory slot A   1024 entries × 16 bytes
           directory slot B   (double-buffered)
           free-sector bitmap
           data sectors

directory entry:
    u32 sectorOffset      0 = chunk absent
    u16 sectorCount
    u16 flags             payload encoding (verbatim gzip / recompressed)
    u32 byteLength
    u32 crc32             of the payload
```

**Commit protocol** — this is what makes it crash-safe:

1. Write payload sectors (into free space, never over the live copy).
2. Write the updated directory into the **inactive** slot.
3. Write the header with `generation + 1` pointing at that slot.

On open, take the header copy with the highest `generation` that passes its CRC. A torn header
leaves the previous generation intact, so the worst case of a power cut mid-save is losing the last
save, never the region. Chunks are verified against their `crc32` on read; a bad chunk is reported
and skipped rather than fed to the mesher.

Sectors freed by a chunk that grew are returned to the bitmap. Fragmentation is bounded by a
**Compact** action in the world list; it is not needed for correctness.

### Shared with McRegion later

When the b1.7.3 build needs real McRegion, the sector allocator and the double-buffered commit are
the same machinery. Build it as a reusable utility under the `storage` slot rather than inline in
one implementation — see [build-versions.md](build-versions.md).

## Lossless round-trip

The promise is that unpacking restores the *entire* original folder, so the manifest records every
file the folder had:

```
per file:  path, byteLength, crc32, mtime, location
           location = (region, slot)  for chunk files
                    | (blob offset)   for level.dat, session.lock and anything unrecognised
```

Stray files matter. Worlds pick up data from third-party tools and older servers; a "packer" that
silently drops what it doesn't recognise is a data-loss bug waiting to happen. Anything not
identified as a chunk goes into the manifest blob verbatim.

Unpack recreates the tree from the manifest, restores mtimes, and **verifies every CRC as it
writes**. A mismatch aborts the conversion with the original still intact.

## Conversion, safely

Both directions follow the same rule: **never destroy the source until the destination is complete
and verified.**

1. Check free space with the real cluster size from `FSUSER_GetSdmcArchiveResource()`. Refuse up
   front, with the actual numbers, if it won't fit.
2. Build the new representation into `<world>.converting/`.
3. Verify every CRC.
4. Write the commit marker, rename into place, then delete the old representation.

Consequences to surface in the UI:

- **Packing needs the packed size free** — roughly a third of the original.
- **Unpacking needs the unpacked size free, including slack**, which can be 3× what the packed world
  occupies. A 200 MB packed world may need ~600 MB. Say so *before* starting, not at 80%.
- Converting 16,000 chunks is not instant. Show progress and allow cancel; cancelling just deletes
  `<world>.converting/`.
- A power cut mid-conversion leaves an unfinished `<world>.converting/` with no commit marker, which
  is discarded on next scan. The original is untouched either way.

Conversion runs on the I/O thread with the game loop still responsive, same as any other bulk I/O.

## Why this doesn't compromise faithfulness

The Alpha level format stays the canonical interchange form and the default. Packed mode is an
explicit, reversible, verified user choice about how bytes sit on one SD card — it changes nothing
about what the world *is*. Every packed world can be returned to a pristine folder that a real
a1.1.2 client will open.

## Test requirements

- **Round-trip on the host**: real alpha world → pack → unpack → compare the whole tree
  byte-for-byte, including files the packer doesn't understand and a world with negative
  coordinates on both axes.
- **Torn-write simulation**: truncate or corrupt a region at each stage of the commit protocol and
  assert the previous generation still loads.
- **Free-space refusal**: assert conversion refuses rather than half-fills a card.
- **Cancellation and power-loss**: assert `<world>.converting/` is discarded and the original opens
  normally.
