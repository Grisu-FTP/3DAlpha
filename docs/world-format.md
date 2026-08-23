# World format

Two representations, one conversion layer:

- **On disk** — the Alpha level format, byte-for-byte compatible with real a1.1.2 saves.
- **In memory** — palette-compressed 16³ sections, chosen to fit a 64 MB device.

The `storage` slot is the boundary. `alpha_chunkfiles` implements it for this version; a future
`mcregion` implementation is selected by another version's manifest and never compiled into this
build. See [build-versions.md](build-versions.md).

## On disk: the Alpha level format

```
<world>/level.dat                                          gzipped NBT
<world>/session.lock                                       8 bytes
<world>/<b36(x&63)>/<b36(z&63)>/c.<b36(x)>.<b36(z)>.dat    gzipped NBT, one per chunk column
```

Up to 64 top-level folders, each with up to 64 subfolders.

### Directory naming

The folder names are `base36(x & 63)` and `base36(z & 63)` — apply two's complement for negative
coordinates *before* masking. The file name uses the signed base36 of the full coordinate, where a
negative number is `-` followed by base36 of its absolute value.

Worked example — chunk (-13, 44):

```
-13 & 63 = 51  -> base36 -> "1f"
 44 & 63 = 44  -> base36 -> "18"
base36(-13) = "-d"        base36(44) = "18"

  <world>/1f/18/c.-d.18.dat
```

Get this wrong and you silently create a parallel set of chunks that the real game cannot see. It is
the first thing to unit-test, with negative coordinates on both axes.

Implemented in `src/impl/storage/alpha_chunkfiles/chunk_path.{hpp,cpp}` and covered by
`tests/chunk_path_test.cpp`, which pins the encoder against `Integer.toString(i, 36)` — including
`Integer.MIN_VALUE` (`"-zik0zk"`), where computing the magnitude in 32 bits would overflow. Paths
are built into a caller-supplied buffer rather than a `std::string`: loading a render-distance-8
area touches 289 of them and none should allocate.

Parsing is deliberately more permissive than writing. A world directory on an SD card holds whatever
the user put there, so `parseChunkFileName` rejects anything that is not exactly `c.<b36>.<b36>.dat`,
refuses digit runs too wide for an `i32` rather than letting them wrap into a plausible coordinate,
and accepts uppercase — FAT is case-insensitive, and a world that has been round-tripped through a
PC can come back with its case changed.

### session.lock

Eight bytes: one big-endian signed 64-bit integer, milliseconds since the Unix epoch. Written on
open and refreshed periodically. The original game uses it to detect a second process editing the
same world; we do the same, and refuse to open a world whose lock has moved under us.

### level.dat

```
Compound ""
  Compound "Data"
    Long   LastPlayed          ms since epoch
    Long   SizeOnDisk          estimated bytes
    Long   RandomSeed          terrain seed
    Int    SpawnX, SpawnY, SpawnZ
    Long   Time                ticks, 24000 per day
    Byte   SnowCovered         modelled -- the terrain generator reads it; see worldgen-a1.1.2.md
    Compound "Player"
      List<Double> Pos          [x, y, z]
      List<Float>  Rotation     [yaw, pitch]
      List<Double> Motion       [x, y, z]
      Byte         OnGround
      Float        FallDistance
      Short        Health, AttackTime, HurtTime, DeathTime, Air, Fire
      Int          Score
      List<Compound> Inventory  [{ Byte Slot, Short id, Byte Count, Short Damage }]
```

Implemented in `impl/storage/alpha_chunkfiles/level_dat.{hpp,cpp}` against
`core/world/level_data.hpp`. Four behaviours are worth knowing, two of which were corrected by
reading a real a1.1.2 save rather than reasoning about the format:

- **There is no `Dimension` tag.** It arrived with the Nether in a1.2.0. A real a1.1.2 `Player` has
  exactly thirteen tags and `Dimension` is not among them, so `PlayerData::hasDimension` records
  whether the file had one and it is written back only if it did. Inventing it would add a tag to
  every world this client touches.
- **An empty list is written with element type `Byte`**, not `TAG_End`. Java's `NBTTagList.write()`
  takes the type from its first element and falls back to `1` when there is none, so every empty
  list the original ever wrote carries a `1`. Our writer normalises this in `endList()`, which makes
  it true for every empty list we emit rather than only the ones someone remembered. Note that a
  semantic diff *cannot* catch this — a zero-count list compares equal whatever type byte precedes
  it — so it is pinned by a byte-level assertion in `tests/level_dat_test.cpp`.
- **An absent `Player` stays absent.** Server-created worlds have no `Player` compound, and writing
  an empty one back would move the player to 0,0,0 the next time the world is opened on a PC.
- `SizeOnDisk` is round-tripped, not maintained. Nothing reads it, and the original recomputes it.

### Decoding rule: preserve the unknown, reject the mistyped

Two rules that look similar and are not:

- A tag we **do not model** is captured verbatim and written back untouched
  (`core/nbt/preserved.hpp`).
- A tag we **do model**, appearing with a type we do not expect, is a hard error
  (`nbt::expectType`).

The second is not pedantry. A decoder that models a name writes that name back unconditionally, so
carrying a wrongly typed copy through as an unknown tag would save two tags with the same name.
It is also the right answer on the merits: `Health` as an Int is not an a1.1.2 `level.dat`, and
loading it as one would write a mangled file back over somebody's world. Both codecs also build
their result aside and move it in only on success, so a file truncated by a pulled SD card leaves
the caller's data untouched rather than half-loaded.

### Chunk files

```
Compound ""
  Compound "Level"
    Int       xPos, zPos
    Byte      TerrainPopulated
    Long      LastUpdate
    ByteArray Blocks       32768   one id per block
    ByteArray Data         16384   nibbles
    ByteArray BlockLight   16384   nibbles
    ByteArray SkyLight     16384   nibbles
    ByteArray HeightMap      256   one byte per XZ column
    List<Compound> Entities
    List<Compound> TileEntities
```

Indexing — **YZX order**, with Y the fastest-varying axis:

```c
size_t i = y + z * 128 + x * 128 * 16;          // Blocks
uint8_t nibble = (arr[i >> 1] >> ((i & 1) * 4)) & 0xF;   // Data / BlockLight / SkyLight
```

`HeightMap` is ZX order: `heightMap[z * 16 + x]`.

Alpha's axis convention as documented: X increases south, Z increases west, Y up. Our engine uses
the conventional Minecraft axes internally; the conversion is identity for array indexing but
matters when interpreting entity rotations. Verify against a real world before trusting it.

Both `level.dat` and chunk files are **gzip** (not raw zlib). Network chunk payloads are raw zlib —
different wrappers, same deflate stream underneath. Do not mix the code paths up.

## In memory: palette-compressed sections

A raw column costs **80 KB** (32768 + 3 × 16384). At render distance 8 that is 17 × 17 × 80 KB ≈
**23 MB** of block data alone, on a device with 64 MB total. Unacceptable.

Instead a column (`core/world/chunk.hpp`) is a stack of 16³ sections
(`core/world/section.hpp`), each compressed on two independent axes.

**Block ids** take the narrowest of four encodings, chosen by how many distinct blocks the section
actually holds:

| Encoding | Holds | Index array |
|---|---|---|
| `Uniform` | one id | none at all |
| `Palette4` | ≤ 16 distinct | 2048 B (4 bits/block) |
| `Palette8` | ≤ 256 distinct | 4096 B (8 bits/block) |
| `Direct16` | anything | 8192 B (one `u16`/block) |

Block ids are `u16`, not the generated registry enum and not `u8`. A world file or a server can
contain an id this build does not know about, and storage has to round-trip it rather than reject
it; 16 bits is what later versions need, and widening it afterwards would touch every array.
`Direct16` exists so that correctness never depends on a palette fitting — it cannot occur in a
version with 256 ids, but the type says it could, so the code handles it.

**Metadata and the two light levels** are each a `NibbleArray`, which is either a single uniform
value costing nothing or a materialised 2048-byte plane in Minecraft's packing (even index → low
nibble). That is not an optimisation for rare inputs: above the terrain every section is air with
sky light 15 and metadata 0, and below it solid rock is 0/0/0. Three planes at 2 KB each is 6 KB per
section, so collapsing them is worth more than the block palette.

Sections use the same Y-fastest index order as the Alpha column arrays (`index = y + z*16 + x*256`).
A section's slice of a column is therefore 256 contiguous runs — 16 bytes for `Blocks`, 8 for a
nibble plane — and conversion in both directions is `memcpy`, not 4096 shifts per plane per section.
Changing that order would add a transpose to every chunk load.

### Measured on a real world

660 chunks of a real a1.1.2 save, loaded through the storage layer:

| | Bytes per column |
|---|---|
| smallest | 9,472 |
| median | 17,718 |
| mean | **18,013** |
| largest | 32,342 |
| raw Alpha column | 81,920 |

**4.55× on average**, and the whole 660-chunk world is 11.6 MB in memory. At render distance 8 that
is 289 columns ≈ **5.1 MB of block data**, comfortably inside the budget in
[architecture.md](architecture.md) — which was written expecting ~8 MB.

Where the saving comes from, counted over the same 5,280 sections:

| | Share |
|---|---|
| Sections that are `Uniform` (no index array at all) | 1,942 / 5,280 — **37 %** |
| Nibble planes that are uniform (no 2 KB array) | 13,838 / 15,840 — **87 %** |
| Sections needing more than 4-bit indices | **1** (`Palette8`); zero `Direct16` |

That last row is the useful one. Exactly one section in an entire explored world holds more than 16
distinct block ids, which says the 4-bit index is the right default and the wider encodings are
correctness insurance rather than working parts. And the 87 % figure confirms the design bet: the
nibble planes, not the block palette, are where the memory goes.

### compact()

`setBlock` only ever grows a palette, exactly like the original game — a section that reached
`Palette8` stays there after being mined back to stone, and a light plane written one block at a
time stays materialised even when every nibble ends up identical. `Section::compact()` re-derives
both and drops to the smallest representation that fits.

It is deliberately not on the edit path: collapsing a plane means rescanning 2048 bytes, which is
absurd per block placement and trivial after a bulk edit. Call it after worldgen, after lighting a
chunk, and before a save.

It matters on the *build* path, not the load path, and the measurements show both halves. On a
column assembled block by block — what worldgen and the lighting engine do — it is the difference
between 21,606 and 15,462 bytes, entirely from sky-light planes that had become uniform without
anything noticing. On a column loaded from disk it saves **nothing**, because `NibbleArray::assign`
already collapses each plane as it arrives and `assignBlocks` already picks the tightest encoding.
Calling it after a load is wasted work; calling it after worldgen is not.

The mesher reads sections through an accessor that hides the encoding, so meshing code is unaware of
palettes. Profile it: if the accessor shows up hot, unpack the section plus its six neighbour faces
into a flat scratch buffer once per mesh job instead.

### Conversion

`impl/storage/alpha_chunkfiles/chunk_nbt.{hpp,cpp}` converts between the chunk NBT above and a
`ChunkColumn`, on decompressed bytes — chunk files are gzip and Map Chunk payloads are raw zlib, and
keeping the wrapper out of the codec is what lets both share it.

`decodeChunk` builds the column aside and moves it in only on success, so a file truncated by a
pulled SD card leaves the caller's chunk untouched instead of half-loaded. `Blocks`, `Data`,
`BlockLight` and `SkyLight` are required and length-checked; a chunk missing one is not a chunk, and
defaulting it to zero would write that guess back over the real world on the next save.
`encodeChunk` refuses outright if the column holds an id above 255, because Alpha's `Blocks` array
has one byte per block and a truncating write would produce a plausible-looking wrong world.

## Storage layer behaviour on the 3DS

The Alpha format is hostile to FAT on an SD card: thousands of small files and slow directory
enumeration. Compatibility is a hard requirement, so we adapt around it rather than changing it.

- **Chunk index — built, and lazily rather than up front.** An earlier version of this document
  asked for a walk of the whole 64×64 tree at world open, cached to `sdmc:/3dalpha/cache/<world>.idx`
  and invalidated against `level.dat`. That is not what was built, and the incremental form is
  strictly cheaper.

  The layout puts a chunk in `<x & 63>/<z & 63>/`, so **one leaf directory holds only chunks spaced
  64 apart**: walking one chunk lands in a different directory every step and returns to a given one
  only after 64. So a single `listDirectory` settles up to a thousand `hasChunk` answers for the
  rest of the session, an unvisited region costs nothing at all, and there is no cache file and
  nothing to invalidate. `AlphaChunkFileStorage::listChunkGroup` is the primitive;
  `core/world/chunk_cache.hpp` holds the index and keeps it ahead of the player by listing the ring
  one chunk beyond the streamer's grid whenever the centre moves. A group asked about before its
  listing arrives falls back to one `stat`, which is what it always cost.

  This is what removed the per-boundary stat storm: crossing a chunk boundary used to re-classify a
  whole row of cells, one IPC round trip each, on the render thread.
- **All chunk I/O on the I/O thread**, with a write-back queue — built, in `core/world/chunk_cache.hpp`.
  Reads, writes, existence and directory listings all happen there; the render thread's half of the
  API returns "not yet" rather than blocking. Dirty columns are coalesced and flushed on the autosave
  timer, when the pause menu opens, and on world exit — **and at no other time**, which is the shape
  a1.1.2 has. The one exception is the dirty budget: a dirty column cannot be evicted because it is
  the only copy of that part of the world, so past the cap whoever dirtied it writes one itself,
  which is back-pressure paid by the generation worker rather than by the frame.

  **The invariant it holds:** every read returns byte-identical content to what the card would
  return, and `hasChunk` answers true from the moment a save is accepted rather than from the moment
  bytes land. Deferring a write therefore changes no answer that cell classification or a generator
  sweep can observe — which matters because classification feeds the generation queue and population
  order *is* the world. The third arm of
  `a_worker_thread_produces_the_same_world_as_inline_while_it_keeps_up` compares whole world trees to prove
  it.
- **POSIX `open`/`read`/`write`, not `fopen`/`fread`.** An earlier version of this document said to
  prefer raw `FSFILE` handles because "the devoptab adds real per-call overhead". Disassembling the
  installed libctru shows that is wrong: `archive_read` calls `FSFILE_Read` directly, `fsync` calls
  `FSFILE_Flush`, and `archive_dirnext` reads **32 directory entries per `FSDIR_Read`** and serves
  the rest from a cache. The devoptab is a thin wrapper. What does cost is the newlib `FILE` layer
  stacked on top of it — its own buffering, `_reent` lookup and per-call locking — and the file
  descriptor path skips all of that. See [3ds-performance.md §Storage I/O](3ds-performance.md).
- One consequence of that batching: an open `DIR` caches 32 × 552 bytes ≈ **17.7 KB**. The index
  scan should hold one directory open at a time, not recurse with several.
- Directories are created lazily on first write, exactly as the original does. Chunks save in bursts
  that share a folder, so the last-created directory is remembered and the two `mkdir` calls happen
  once per folder rather than once per chunk.
- **Atomic writes**: write `*.tmp`, `fsync`, close, then rename over the target. FAT32 has no journal
  and the console can be switched off mid-write. The `fsync` is not optional — without it the rename
  can land before the data does, producing an empty file under a valid name, which is worse than
  either outcome the rename was supposed to guarantee. POSIX `rename` replaces atomically; FAT does
  not, so there is a remove-then-rename fallback on the platform that never had an atomic replace.

### session.lock, and one deliberate deviation

Eight bytes of big-endian milliseconds, written on open and re-read to confirm nothing else claimed
the world in between. The original game re-checks the lock on *every chunk save*. We do not: a
console cannot run two copies of the game at once, and the check would double the file operations on
the hottest I/O path there is, on a device where each one is an IPC round trip. `lockStillOurs()` is
exposed for the caller to run on a timer instead. This is not observable in the save format.

There is a second, larger cost that no amount of I/O tuning removes: the 3DS is FAT32-only with
32–64 KB clusters, and one file per chunk means **on-disk size ≈ chunks × cluster size**, regardless
of how well the data compresses. Sizes, limits, backup and transfer are in
[save-data.md](save-data.md).

That cost is opt-out per world. **Packed mode** stores chunks in sector-allocated region containers
instead of one file each, cutting slack ~10× and replacing per-chunk file opens with seeks; the
conversion is lossless both ways and the format above stays the canonical interchange form. The
storage layer therefore implements two backends behind the same interface, and the chunk-index cache
applies only to folder mode — a packed region's header is its own index. See
[packed-worlds.md](packed-worlds.md).

## Round-trip guarantee

The stated requirement is that real a1.1.2 worlds work, in both directions. The test that proves it:

1. Copy a real alpha world to the host.
2. Load it, save it, without editing.
3. `tools/nbtdiff.py diff <before> <after> --ignore SizeOnDisk` and require **semantic equality** —
   same tags, same values, ignoring only key order within compounds (the original stores them in a
   `HashMap`) and `SizeOnDisk`, which the original recomputes too.

Run it against a world containing negative coordinates on both axes, tile entities (signs, chests,
furnaces), a full inventory, and at least one unpopulated chunk.

`tools/nbtdiff.py` is written from the NBT specification rather than from `core/nbt`, deliberately.
Round-tripping a file through our own reader and writer only proves the two agree with each other;
checking the bytes with a separate implementation is what proves they agree with the format.

### Result on a real a1.1.2 world

Run against a 660-chunk world produced by the real game (PrismLauncher, a1.1.2_01), on a copy:
opened, scanned, every chunk loaded and written back, then compared file by file.

**661 / 661 files semantically identical. 0 load failures, 0 save failures.**

**0 / 661 byte-identical**, which is the expected and correct outcome: the original stores compounds
in a `HashMap` and writes them in hash order, so byte equality is not achievable by anyone, us
included. Our files come out 2.7 % smaller in total. Semantic equality is the only meaningful bar.

The run found two real bugs that no amount of self-round-tripping would have:

1. The encoder wrote a `Dimension` tag into `Player`. a1.1.2 does not have one.
2. The encoder wrote `TAG_End` as the element type of an empty list. The original writes `Byte`.

Both are fixed and pinned by tests. The second is worth dwelling on: a semantic diff could not see
it, because a zero-count list compares equal regardless of the type byte in front of it. It was
found by reading the raw bytes of the real file and comparing them with ours. **A passing semantic
diff is necessary, not sufficient** — for anything where the original's exact encoding is
observable, check the bytes.

Still untested: worlds with negative coordinates on *both* axes at scale (this one has a handful),
and chests/furnaces/signs with contents (the richest chunk here has 3 tile entities and 2 entities,
which round-tripped, but they travel as preserved blobs and are not yet parsed).

Unknown NBT tags in chunks and `level.dat` must be **preserved verbatim** through a load/save cycle.
Servers and third-party tools of the era wrote extra data; dropping it silently corrupts worlds.
