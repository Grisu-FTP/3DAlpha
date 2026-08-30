# Contributing

## Review checklist

Each item below is review-blocking. They exist because the two things that kill a project like this
are hardcoded version assumptions and performance debt, and both are cheap to prevent and expensive
to remove.

### No hardcoding

- [ ] No packet ID literal outside a `PacketTable`.
- [ ] No block or item ID literal outside the registry. Use the generated `BlockId` / `ItemId` enums.
- [ ] No world height, section size or inventory dimension as a `#define` — they come from the
      generated `mcver::` config.
- [ ] No version comparison (`mcver::kProtocol == 2`) outside the generated config. Branch on
      **feature constants**.
- [ ] **No `#ifdef` / `#if` on game version, anywhere.** Behavioural deltas use
      `if constexpr (mcver::kHasX)`; whole differing subsystems become slot implementations under
      `src/impl/<slot>/<impl>/`. See [docs/build-versions.md](docs/build-versions.md).
- [ ] A new slot implementation lives entirely in its own directory and is added to no source list
      by hand — the manifest selects it.
- [ ] A new version's `uniqueId` comes from the project's reserved block and is never a reuse of a
      retired one.
- [ ] Changes to shared code build for **every** version, not just the one you're working on.
- [ ] The renderer references *render types*, never block IDs.
- [ ] New version-specific data goes in `data/`, not in a `switch`.

### Platform separation

- [ ] Nothing in `src/core/` includes `<3ds.h>`, `<citro3d.h>`, or any devkitPro header.
- [ ] Nothing in `src/core/` has an `#ifdef _3DS`.
- [ ] New platform functionality is added to the interface *and* to both `platform/ctr/` and
      `platform/host/`.
- [ ] New core logic has a host-side test.

### Performance

- [ ] No allocation in the per-frame path. Pools and arenas, not `malloc`.
- [ ] No filesystem access or decompression on core0.
- [ ] Nothing blocks the main thread waiting on a worker; results are drained with a try-lock.
- [ ] Worker results carry a revision stamp and are discarded when stale.
- [ ] `GSPGPU_FlushDataCache` before the GPU reads anything the CPU just wrote.
- [ ] New render passes are behind a config option, and turning that option off actually skips the
      pass (see the options contract in [docs/3ds-performance.md](docs/3ds-performance.md)).
- [ ] Changes that touch the renderer or mesher report before/after numbers from **hardware**,
      not from Azahar.

### Documentation that others navigate by

- [ ] Every new file opens with a header comment saying what it is and **why it is that way**. Its
      first sentence is a real summary — `docs/code-map.md` is generated from it.
- [ ] Adding, removing or renaming a file, or editing a heading under `docs/`, is followed by
      `make index`. CI-style check: `python3 tools/gen_index.py --check`.
- [ ] A milestone that moves updates `docs/status.md`, which is the handoff.

### Correctness of formats

- [ ] World writes preserve unknown NBT tags verbatim.
- [ ] Chunk path computation is tested with negative coordinates on both axes.
- [ ] Protocol changes come with a replay-harness case.

## Style

- C++17, `-fno-exceptions -fno-rtti`. No `throw`, no `dynamic_cast`, no `typeid`.
- No exceptions means errors are return values. Use a small `Result`/`expected`-like type; do not
  signal failure by convention.
- No STL containers in hot paths where a pool or fixed array will do. `std::vector` is fine for
  setup and tooling.
- Prefer plain functions and data over class hierarchies. The only virtual interfaces are the
  platform boundary and the storage/protocol/worldgen abstractions.
- Match the surrounding file's naming and comment density.

## Licensing discipline

- **Never** copy code from ViaLegacy or any other GPL source into this tree. It is documentation.
  Protocol IDs and wire formats are facts about a 2010 protocol; the code implementing them is not.
- craftus_reloaded is MIT: reusable **with attribution** in `romfs/licenses.txt`.
- Never commit Mojang assets — no textures, sounds, fonts, or jar contents.
- Every vendored dependency gets its licence reproduced in `romfs/licenses.txt`.

## The player supplies nothing

The game must be complete and playable on a fresh install, with no dumping, extracting or
converting. Textures and sounds are the only things a player may optionally supply.

- [ ] No game data is read from a user-supplied jar **at build or run time**. Block, item, recipe
      and worldgen tables are checked in as `data/<version>/*.json` and compiled into `.rodata`.
- [ ] Maintainer tools that read an original jar (`tools/extract_blocks.py`, and `tools/javap.py`
      which it interprets bytecode with) are run by hand, are never invoked by the build, and say so
      in their docstring.
- [ ] No feature is gated behind having a jar. Missing textures fall back to the bundled pack;
      missing sounds and missing DSP firmware degrade to silence with an explanation.

Deriving a table from the real jar is good practice and stays — it is how the numbers get to be
correct. The rule is about *who* does it and *when*: once, by a maintainer, with the result checked
in. It never becomes a step in the player's way.

## Commit and PR expectations

- One logical change per commit.
- A commit touching a hot path states what it measured.
- Milestone-gating work does not merge until the milestone's performance gate passes on hardware.
