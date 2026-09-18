# Multi-version builds

One source tree, **one binary per Minecraft version**, selected at configure time. An a1.1.2 build
contains zero b1.7.3 code — not "the linker garbage-collected it", but *never compiled*.

```sh
make VERSION=a1.1.2          # -> build/a1.1.2/3DAlpha-a1.1.2.{3dsx,cia}
make VERSION=b1.7.3          # -> build/b1.7.3/3DAlpha-b1.7.3.{3dsx,cia}
make all-versions            # every version in versions/
```

Each output has its own title, icon and 3DS title ID, so several can be installed side by side.

## Why not `#if MC_VERSION >= ...`

The instinct — mark each diff with the versions it applies to, compile only the relevant branches —
is right about the *goal*. The literal preprocessor form is what decays:

- **Versions aren't a line.** a1.2.0 adds the nether and the login seed; b1.2 changes string
  encoding; b1.3 changes the world format. These are independent axes. A range test over one ordinal
  encodes a model that isn't true, and you end up with
  `#if MC >= A120 && MC < B130 || MC == B12` conditions nobody can read.
- **You can't compile-test what you don't build.** `#if`-disabled code isn't parsed, so it rots
  silently until someone builds that version months later.
- **Tooling gives up.** clangd, refactors, and coverage all get confused by preprocessor forks.

The scheme below gives the same "only relevant code compiles" guarantee, but the conditionals live
in **one manifest file per version** instead of scattered through the tree, and shared code stays
fully type-checked for every version at once.

## Three tiers, matched to the size of the delta

### Tier 1 — Data (the large majority of differences)

Blocks, items, recipes, packet tables, atlas mapping. These are pure data, so version selection is
just picking which generated file compiles.

```
data/a1.1.2/{blocks,items,recipes}.json
        │
        ├── tools/gen_tables.py
        ▼
build/a1.1.2/gen/{tables.cpp, block_ids.hpp, packets.cpp}
```

Generated tables are `constexpr` arrays in `.rodata`. **This also removes the JSON parser from the
shipping binary entirely** — nothing is parsed at boot, which is a straight win on a 268 MHz CPU and
a change from the original plan, where the profile was read at runtime.

No conditionals in code at all for this tier.

### Tier 2 — Feature constants and `if constexpr`

Generated `version_config.hpp`:

```cpp
namespace mcver {
    inline constexpr int  kProtocol    = 2;
    inline constexpr int  kWorldHeight = 128;
    inline constexpr bool kHasHealth   = false;
    inline constexpr bool kHasWindows  = false;
    inline constexpr bool kHasNether   = false;
    inline constexpr bool kHasBiomes   = false;
}
```

Small behavioural deltas inside otherwise-shared functions:

```cpp
if constexpr (mcver::kHasHealth) {
    applyServerHealth(pkt);
}
```

The discarded branch generates no code. Crucially, unlike `#if`, **the discarded branch is still
parsed and name-checked**, so renaming something used by another version's branch fails the build
immediately instead of a month later.

The honest limit: in a non-template context `if constexpr` discards codegen but still requires the
discarded branch to be *well-formed* — every name in it must exist. So this tier handles behavioural
differences, not "this entire subsystem doesn't exist in this version." That's Tier 3.

### Tier 3 — Swappable modules

When a whole subsystem differs, it becomes a **slot** with competing implementations:

```
src/impl/storage/alpha_chunkfiles/     src/impl/storage/mcregion/
src/impl/strings/modified_utf8/        src/impl/strings/ucs2/
src/impl/items/b1_2/                   src/impl/items/with_nbt/
src/impl/worldgen/alpha_nobiome/       src/impl/worldgen/beta_biome/
src/impl/containers/none/              src/impl/containers/windows_b1_0/
```

The manifest names which implementation fills each slot; the generator emits aliases:

```cpp
namespace mcver {
    using Storage     = impl::AlphaChunkFileStorage;
    using StringCodec = impl::ModifiedUtf8Codec;
    using WorldGen    = impl::AlphaNoBiomeGen;
    using Containers  = impl::NoContainers;   // empty stub; compiles to nothing
}
```

**Static binding, not virtual.** There is exactly one implementation per slot in the binary, so the
compiler inlines straight through the alias — no vtable, no indirect call, and no dead
implementations anchored by a vtable that `--gc-sections` cannot remove.

Unselected implementation directories are never added to the source list, so their code never
reaches the compiler. That is the literal zero-bloat guarantee.

Each slot has a documented compile-time concept (the set of functions an implementation must
provide). A missing function is a compile error naming the slot, not a link error.

## The version manifest

Everything version-specific, in one file. `versions/a1.1.2.json`:

```json
{
  "id": "a1.1.2",
  "display": "Alpha 1.1.2",
  "appTitle": "3DAlpha a1.1.2",
  "releaseDate": "2010-09-18",

  "packaging": {
    "uniqueId": "0xFF3A0",
    "productCode": "CTR-P-3DAA",
    "iconBadge": "a1.1.2"
  },

  "constants": { "protocol": 2, "worldHeight": 128, "sectionSize": 16 },

  "slots": {
    "storage":    "alpha_chunkfiles",
    "strings":    "modified_utf8",
    "items":      "b1_2",
    "worldgen":   "alpha_nobiome",
    "containers": "none",
    "entitydata": "none"
  },

  "features": {
    "health": false, "respawn": false, "useEntity": false, "entityVelocity": false,
    "explosion": false, "windows": false, "entityMetadata": false,
    "loginSeedAndDimension": false, "nether": false, "biomes": false, "hunger": false
  },

  "data": "data/a1.1.2"
}
```

Adding a version = one manifest + the data directory + any new slot implementations. Nothing else in
the tree changes.

## How the build wires up

`tools/configure.py <version>` reads the manifest and emits, into `build/<version>/gen/`:

| Output | Status | Contents |
|---|---|---|
| `version_config.hpp` | **done** | constants and feature `constexpr bool`s |
| `version_slots.hpp` | **done** | includes each selected slot's `slot.hpp`, and names the slots not yet implemented |
| `version.cmake` | **done** | `MCVER_SLOT_DIRS` plus packaging metadata |
| `<version>.rsf` | **done** | RSF template with `Title`, `ProductCode`, `UniqueId` substituted |
| `block_ids.hpp` | M1 | generated `enum class BlockId` |
| `tables.cpp` | M1 | block/item/recipe tables as `constexpr` arrays |
| `packets.cpp` | M5 | packet descriptor tables, both directions |
| `icon.png` | M6 | base icon with the version badge composited in |

The build is **CMake**, using devkitPro's `3ds-cmake` toolchain (`arm-none-eabi-cmake`, which
provides `ctr_generate_smdh`, `ctr_create_3dsx`, `ctr_add_shader_library`). One `CMakeLists.txt`
serves both the 3DS and host targets, which is what the core/platform split in
[architecture.md](architecture.md) needs.

`configure.py` runs at configure time, before any source list is evaluated:

```cmake
set(MCVER "a1.1.2" CACHE STRING "Minecraft version to build")
execute_process(COMMAND ${Python3_EXECUTABLE} tools/configure.py ${MCVER} ${GEN_DIR})
include("${GEN_DIR}/version.cmake")

foreach(dir IN LISTS MCVER_SLOT_DIRS)          # only the selected slots
	file(GLOB_RECURSE found "${dir}/*.cpp")
	list(APPEND SLOT_SOURCES ${found})
endforeach()
```

`CMAKE_CONFIGURE_DEPENDS` is set on the manifest and on `configure.py`, so editing a slot or a
feature bit re-runs generation automatically instead of leaving a stale build.

A thin `Makefile` wraps this so the short commands still work (`make`, `make VERSION=…`,
`make all-versions`, `make cia`, `make host`, `make run`).

Generating the icon badge at build time (M6) matters: a hand-edited per-version PNG drifts the
moment someone changes the base art.

### Slot binding convention

Each implementation ends its `slot.hpp` by binding itself:

```cpp
namespace mcver::impl { class AlphaChunkFileStorage { /* ... */ }; }
namespace mcver { using Storage = impl::AlphaChunkFileStorage; }
```

The generator only emits the `#include`. Since exactly one implementation per slot is ever compiled,
there is no collision and no dispatch — the compiler inlines straight through the alias. A slot with
no `slot.hpp` yet is reported by name at configure time rather than failing obscurely later.

## Installing several versions at once

### Title IDs

A 3DS title ID is `0004 0000 <20-bit UniqueId> <variation>`. **Two titles with the same UniqueId
overwrite each other on install**, so every version needs its own.

Unique ID ranges: system `0x0–0x2FF`, retail applications `0x300–0xF7FFF`, evaluation
`0xF8000–0xFEFFF`, prototype `0xFF000–0xFF3FF`, developer `0xFF400–0xFF7FF`. Homebrew belongs in the
evaluation or prototype ranges.

`makerom`'s **default is `0xFF3FF`** — never ship that. It is the single most collided ID in
homebrew.

This project reserves `0xFF3A0–0xFF3AF` (prototype range, 16 slots):

| Version | UniqueId | ProductCode | HOME menu title |
|---|---|---|---|
| a1.1.2 | `0xFF3A0` | `CTR-P-3DAA` | 3DAlpha a1.1.2 |
| a1.2.6 | `0xFF3A1` | `CTR-P-3DAB` | 3DAlpha a1.2.6 |
| b1.7.3 | `0xFF3A2` | `CTR-P-3DAC` | 3DAlpha b1.7.3 |
| *reserved* | `0xFF3A3–0xFF3AF` | | future versions |

Assignments are permanent — reusing a retired ID makes an old install silently upgrade into a
different game. Register the block in the community
[Homebrew CIAs UniqueID Collection](https://gbatemp.net/threads/homebrew-cias-uniqueid-collection.379362/)
so other homebrew doesn't land on it.

### RSF fields that differ per version

```
BasicInfo:
  Title       : 3DAlpha a1.1.2      # generated
  ProductCode : CTR-P-3DAA          # generated
TitleInfo:
  Category    : Application
  UniqueId    : 0xFF3A0             # generated
```

Everything else in the RSF (the New 3DS block — `SystemModeExt`, `CpuSpeed`, `EnableL2Cache`,
`CanAccessCore2` — and `FileSystemAccess: DirectSdmcWrite`, which we need for SD access) is shared
and lives in an RSF template.

### 3DSX needs none of this

`.3dsx` files in `sd:/3ds/` coexist by filename alone, and hbmenu reads the title from the embedded
SMDH. Multi-version side-by-side is free there. Title IDs only matter for CIA installs onto the HOME
menu.

## SD card layout

Heavy shared assets are shared; anything version-specific is namespaced:

```
sdmc:/3dalpha/
  packs/                  texture packs      — SHARED (they're large; don't duplicate per install)
  resources/              sounds             — SHARED
  saves/<world>/          worlds             — SHARED root
  versions/a1.1.2/        options.txt, 3ds.ini, cache/, crash.txt   — per version
  versions/b1.7.3/        ...
```

Worlds are shared deliberately — the on-disk format *is* the compatibility contract. But a build
only lists worlds in a format it owns: the a1.1.2 build shows base36 chunk-file worlds and hides
`region/` ones. Opening a foreign world is an explicit **"convert a copy"** action that never
touches the original. Silent in-place upgrades are how people lose worlds.

Texture-pack conversion caches are keyed by pack **and** by converter version, so two installs
sharing `packs/` don't fight over a stale `.3dtex`.

## CI

Test and build **every** version on every change. The whole point of Tier 2 and Tier 3 over
`#ifdef` is that shared code stays type-checked across versions, and that only holds if CI actually
compiles them — and compiling only proves the code parses. Running the host suite per version is
what proves a change made for one version did not quietly alter shared entity, world, net or tick
behaviour for another.

`.github/workflows/build.yml` does both on every push. The matrix is not written down: it is
`versions/*.json`, read at the start of the run, so a new manifest is a new build with no workflow
edit. Each version produces a `.3dsx` and a `.cia`, both uploaded as one artifact named
**`3DAlphaR<n><version>`** — `3DAlphaR1a1.1.2` — where `<n>` is the run number and therefore goes up
with every push. The number is in the filename and nowhere else: title, ProductCode and UniqueId
stay per-version, so a newer CIA installs *over* an older one instead of beside it.

Per run it also prints the section sizes from `arm-none-eabi-size`, so cross-version bloat shows up
as a number in the build log rather than as a surprise on hardware, and runs
[`tools/check3dsx.py`](../tools/check3dsx.py) over the 3DSX — a file Luma's loader would reject is
indistinguishable from a crash once it is on a console, so the check belongs before it gets there.

Two things the workflow has to work around, both of them permanent:

- **`makerom` is built from source, at a pinned tag.** devkitPro does not ship it, and the prebuilt
  release binary from Project_CTR is linked against glibc 2.38 while the `devkitpro/devkitarm` image
  is Debian 12 (2.36). Building it takes about two seconds, so this is not a cost worth caching.
- **CI does not use `make`.** The Makefile builds into `build/<version>/`, which is committed to
  this repo with a `CMakeCache.txt` full of one developer's absolute paths; CMake refuses to reuse
  it from anywhere else. CI configures into `build-ci/<version>` instead.

The `test` job runs first and the 3DS builds `need:` it, so a red suite for any version produces no
artifacts at all — an artifact that boots but silently broke shared code for another version is
worse than no artifact. It builds host-side only (`src/core/` is platform-independent, so no
devkitARM), Debug with `SANITIZE=ON`, matching the `build-host` configuration in
[working-guide.md](working-guide.md) so a CI failure reproduces locally with the same command. The
suite binary is invoked directly rather than through `ctest`, because `add_test` wraps all of it as
a single test and would report one line either way, where the binary names the failing case.

Still to add: a Debug configuration for the *3DS* target alongside Release.

## Rules

- **No `#ifdef`/`#if` on version, anywhere.** Version deltas go in the manifest.
- **No runtime version comparison.** Branch on feature constants; the version number appears only in
  the generated config and the about screen.
- A new slot implementation lives entirely in its own directory and is added to no source list by
  hand.
- Title ID assignments are append-only. Never reuse.
