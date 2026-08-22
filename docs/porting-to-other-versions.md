# Adding another game version

The engine targets a1.1.2, but nothing version-specific is hardcoded. Supporting a1.2.6 or b1.7.3
should mean **new data and at most a new slot implementation, not new engine code**.

Each version compiles to its **own separate binary** with its own 3DS title ID, so several can be
installed at once and no version carries another's code. The build mechanics — manifests, slots,
codegen, title IDs, SD layout — are in [build-versions.md](build-versions.md). This document is the
per-version content checklist.

## What a version needs

Everything lives in one build-time manifest, `versions/<id>.json`, plus a data directory.

Checklist for a new version:

1. **Protocol table** — packet IDs and field descriptors for both directions. Derive from the real
   client (OrnitheMC decompile) and confirm with a capture. Never from memory.
2. **String codec** — modified UTF-8 through the alpha era; UCS-2 later. Getting this wrong produces
   a client that connects and then desyncs on the first chat message.
3. **Item format** — `B1_1` (id, count, *byte* damage), `B1_2` (id, count, short damage), or the
   later NBT-carrying form.
4. **World format** — Alpha chunk files, McRegion (`r.x.z.mcr`, Beta 1.3+), or Anvil. A new format
   is one new `storage` slot implementation; nothing above the slot changes.
5. **Block, item and recipe tables** — `data/<id>/blocks.json` etc., code-generated into enums and
   `constexpr` tables so lookups stay compile-time cheap.
6. **Feature bits** — `hasNether`, `hasHealthPacket`, `hasWindows`, `hasBiomes`, `hasHunger`, …
   Gameplay systems query features; they never compare version numbers.
7. **World generator** — a new `worldgen` slot implementation if terrain generation changed.
8. **Packaging** — a new `uniqueId` from the project's reserved block and a distinct `productCode`,
   so the build installs alongside the others instead of overwriting one. See
   [build-versions.md](build-versions.md).

## The rules that keep this true

These are review-blocking. See [CONTRIBUTING.md](../CONTRIBUTING.md).

- No packet ID literal outside a generated packet table.
- No block ID literal outside the block registry.
- World height, section size and inventory layout come from `mcver::`, never from `#define`.
- The renderer knows *render types*, never block IDs.
- Gameplay branches on **feature constants**, never on `mcver::kProtocol == 2`.
- **No `#ifdef` on version.** Behavioural deltas use `if constexpr`; whole differing subsystems
  become slots.

A version comparison outside the generated config is a bug.

## Worked example: a1.2.6 (protocol 6)

Roughly the smallest interesting jump, and a good test of whether the abstraction holds.

| Area | Change | Where |
|---|---|---|
| Protocol | 2 → 6 | manifest `constants` |
| Login packet | gains `i64 mapSeed`, `i8 dimension` | `packets.json` |
| New packets | 0x07 Use Entity, 0x08 Update Health, 0x09 Respawn, 0x1C Entity Velocity, 0x26 Entity Status, 0x27 Attach Entity, 0x3C Explosion | `packets.json` |
| Features | `hasHealthPacket`, `hasNether`, `hasServerSideDamage` on | manifest `features` |
| Blocks | sandstone, cactus, dispenser, note block, … | `data/a1.2.6/blocks.json` |
| World format | still Alpha chunk files | slot unchanged |
| Strings | still modified UTF-8 | slot unchanged |
| Worldgen | biomes introduced | new `worldgen` slot impl |
| Packaging | `uniqueId 0xFF3A1`, `CTR-P-3DAB` | manifest `packaging` |

Note what is *not* on that list: the renderer, the mesher, the storage layer, the threading model,
the memory manager. If a version bump makes you edit those, the abstraction leaked — fix the leak
rather than adding a special case.

## Worked example: b1.7.3 (protocol 14) — the bigger jump

| Area | Change | Slot |
|---|---|---|
| Strings | **UCS-2** (`u16 charCount` + UTF-16BE) | `strings: ucs2` |
| World format | **McRegion** — the first that isn't file-per-chunk | `storage: mcregion` |
| Windows | 0x64–0x6B container family — a real container UI, not `0x05` resync | `containers: windows_b1_0` |
| Entity metadata | typed metadata streams appear | `entitydata: typed_metadata` |
| Items | stacks gain NBT | `items: with_nbt` |
| Blocks/items | large table growth, pistons, repeaters | data only |

Every one of those is a slot swap or a data file — which is the test of whether the design held.
The a1.1.2 build gains nothing from any of it, because none of those implementations are in its
source list.

McRegion is also where the SD-card pain in [world-format.md](world-format.md) largely goes away:
one file per 32×32 chunks instead of one per chunk.

## Worlds across versions

Builds share `sdmc:/3dalpha/saves/`, but a build only lists worlds in a format it owns — the a1.1.2
build shows base36 chunk-file worlds and hides `region/` ones. Opening a foreign world is an
explicit **"convert a copy"** action that never touches the original. Silent in-place upgrades are
how people lose worlds.

Keep block and item tables per version rather than merged. A merged table is how "just one small
version check" gets into the renderer.
