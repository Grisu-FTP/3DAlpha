# M3 to-do: the player body, then Creative, then entities

This is a work list, not a design document. It exists because the order of M3 is the whole
argument: **entities and Creative share a prerequisite, and it is the player body.** Building the
body first means Creative is nearly free afterwards and entities lose their hardest third. Building
entities first means building the body anyway, under a deadline, alongside a renderer that does not
exist yet.

Written 2026-09-02. When an item lands, move its result into `docs/status.md` §*Next steps, in
order* — that file is the handoff, this one is scaffolding and should shrink to nothing.

## Where things actually stand

*Rewritten as steps 0–3 landed. The first three bullets are what is still true; the rest of this
paragraph was the state before any of it and is gone.*

- `src/impl/entitydata/none/` — empty; the slot is still `"none"` in `versions/a1.1.2.json`.
- `Entities` and `TileEntities` round-trip as opaque preserved NBT
  (`src/core/nbt/preserved.hpp`, `src/impl/storage/alpha_chunkfiles/chunk_nbt.hpp:26`). Nothing
  parses them. That is deliberate and it still holds.
- `src/impl/items/b1_2/` — still empty. Nothing encodes an `ItemStack` yet, which is why the hotbar
  is not saved.
- `src/core/entity/` has the body, the ray trace and the collision sweep; `src/core/item/` has the
  Creative palette and the hotbar. Spectator, Survival and Creative all parse from
  `<world>/3dalpha.ini`, and **Spectator and Creative are both selectable** — Survival is the one
  drawn disabled now.
- **Nothing from steps 1, 2 or 3 has been run on hardware.** Every one of them builds for the 3DS
  and passes on the host. That is the single largest open item in this file.

## 0. Block collision shapes — **done**

- [x] `tools/genref.java --collision` — a new emitter asking a running jar for the collision boxes
      of every block crossed with every metadata value, through `addCollisionBoxesToList`, the same
      entry point the physics reaches. Byte-identical across runs.
- [x] `tests/collision_box_vectors.hpp` — the whole truth table, 1,120 rows, plus per-block
      slipperiness.
- [x] `block::Shape` beside `RenderType` and `TickBehaviour`, a `shape` column in `blocks.json`
      derived by classifying the fixture, and `slipperiness` alongside it.
- [x] `src/core/util/aabb.hpp`, `src/core/block/collision.{hpp,cpp}`,
      `tests/collision_box_test.cpp`. Host and 3DS both build; suite green.

Three measured results, written up in `docs/physics-a1.1.2.md`:

- **A collision box is a pure function of `(id, metadata)`** — nothing reads a neighbour, not even
  the top half of a door. That is why `collisionBoxes()` takes no world.
- Stairs are the only shape with more than one box, and have exactly two.
- A ladder with metadata outside 2..5 sets no bounds at all and the original answers with the
  previous query's leftovers. Ours answers with the constructor default.

## 1. The player body — the actual unlock

Pure `src/core/`, no GPU work, host-testable, and derivable from the jar rather than invented.

- [x] Derive from `Entity.moveEntity` / `EntityPlayer` in the a1.1.2 jar: the 0.6 × 1.8 AABB, the
      0.5 step-up, gravity, the drag and ground-friction constants, the jump impulse, eye height,
      and the sneak edge-check. **All of it is transcribed in `docs/physics-a1.1.2.md`** — read
      that before writing any of the code below; several constants are not the round numbers they
      look like (`0.98` is really `0.9800000190734863`, `0.42` is `0.41999998688697815`).
- [x] `src/core/util/aabb.hpp` — the box and the geometry that needs no world. It went to `util`
      rather than `entity` because `core/block/collision.hpp` answers with boxes and `core/entity`
      moves one through them, and neither should include the other.
- [x] **Moved `MathHelper` out of the worldgen slot** into `src/core/util/`, with `sqrtFloat` and
      `sqrtDouble` added. `moveFlying` uses the same quantised 65,536-entry sine table the cave
      carver does, and `src/core/entity/` may not include `src/impl/worldgen/`.
- [x] `src/core/entity/player_body.{hpp,cpp}` — `moveEntity`, `moveEntityWithHeading`, `moveFlying`
      and `jump`. Land branch only; water and lava are transcribed in the doc and not written.
- [x] `tools/genref.java --player` → `tests/player_body_vectors.hpp`, driving a **real
      `EntityPlayer`** (`dm` turns out to be concrete and to override neither movement method)
      against a real `World`. 22 cases, 880 ticks, compared as raw bit patterns: falling, walking,
      a wall, a slab step, a block too tall to step, a fence that cannot be jumped, a ledge, ice,
      a corner, and every one of them again across the negative axis. **All pass.** Three real bugs
      it caught are written up in `docs/physics-a1.1.2.md`.
- [x] Wired into `main.cpp` behind the gamemode. `moveCamera` is now `flyCamera` and serves
      Spectator only; every other mode ticks the body at 20 Hz alongside the world tick. The 1.62
      bug below is fixed with it: the body owns the feet, and both the camera and `Pos[1]` come off
      it. Controls moved with it — B and Y are jump and sneak, the shoulders are free for break and
      place, and the 3D tuner went from Y + d-pad to SELECT + d-pad because Y is sneak now.

### The 1.62 bug this closed

**`posY` in a1.1.2 is the eye, not the feet** — `yOffset` is 1.62 and `setPosition` puts the box at
`posY - yOffset`. So `level.dat`'s `Pos[1]` is an eye height, and the reference world's
`70.62000000476837` proves it. But `WorldStreamer::spawnPosition` returns `Pos` verbatim and
`main.cpp:561` then adds 1.62 to it, while `setPlayerState` writes the camera's `y` straight back
out — so **a saved player rises 1.62 blocks on every save-and-reload cycle**. The fresh-world path
is correct, because its fallback returns `spawnY`, which really is a block coordinate. A body that
owns the feet and derives both the eye and `Pos[1]` from them fixes the two paths together.

- [x] `--walk <world-dir> [distance] [ticks] [gen]` in `src/platform/host/main.cpp`. Walks a real
      streamed world under the sanitizers and **exits non-zero** when the body falls through loaded
      ground, goes NaN, leaves the world, or never moves — a check, not a measurement, so none of
      its numbers belong in `status.md`'s measured table. It turns away after twenty ticks of no
      progress, because the first version fell into a ravine and spent the remaining 1,700 ticks
      pressed against one wall still reporting "ok".

**Done.** 4,000 ticks over a copy of the real 660-chunk world: 398 blocks of path, 73 % of ticks on
the ground, down into the caves to y=13, no fall-through and no NaN. The vector tests pass as well
(22 cases, 880 ticks, bit-exact).

Two things `--walk` turned up that the fixtures could not:

- **A fresh world spawns the player inside the ground.** `level.dat`'s `spawnY` is a block
  coordinate with no promise attached, and putting the feet at it can bury the body to the waist —
  which cannot walk in any direction and reads as broken physics. `PlayerBody::liftOutOfGround`
  raises it clear, the way the original does when a player joins a world, and is a no-op for a world
  with a saved player.
- **Walking off the edge of the generated world is not a bug.** An absent column contributes no
  collision boxes, so the body falls — which is exactly what the original does, and is why
  `clipAxis` checks `chunkResident` at all. The harness tells the two apart by asking whether the
  column under the body was loaded, and only calls the loaded case a failure.

## 2. Reach, break and place

- [x] `src/core/entity/ray_trace.{hpp,cpp}` — a1.1.2's `rayTraceBlocks` and `collisionRayTrace`,
      returning block, face and hit point. **Reach is 4.0**: the base controller says 5.0 and both
      concrete ones override it. `tools/genref.java --raytrace` →
      `tests/ray_trace_vectors.hpp`: a floor with a slab, two stairs, a fence, a floor torch, a wall
      torch, a ladder, a sapling, snow, glass, a door and water, swept by **3,744 rays** from six
      eyes. Bit-exact, including the hit point.
- [x] The **selection** table, which is not the collision table: `block::selectionBox` and
      `block::isTargetable`, generated by `tools/gen_selection.py` into
      `data/a1.1.2/selection.json` and compiled to ~5 KB of `.rodata`. A torch is targetable and has
      no collision box; water is solid and targetable by nothing.
- [x] `WorldStreamer::setBlock`, bracketing `tickRenderer_` the way `stepTicks` does. **This was the
      trap**: an edit written straight through `worldTick()` marks its column dirty and queues its
      lighting but never redraws. `tests/streamer_revisit_test.cpp` now asserts both halves — the
      raw write leaves the section stale, `setBlock` marks it.
- [x] Break and place on L and R, routed through that seam, so neighbour updates, falling sand and
      fluid flow come for free from `src/core/tick/`. Placement refuses anything but air and
      anything that would intersect the player's own box.
- [x] The column is marked dirty by the existing `tickBlockChanged` path, so the autosave picks the
      edit up with no new code — the seam `status.md:181` and `:1305` reserved.
- [x] **`onBlockPlaced`**, as a generated table: `tools/genref.java --place` sweeps every block
      against every face at sixteen player headings, `tools/gen_selection.py` reduces it to
      `data/a1.1.2/placement.json`, and `block::placementMetadata` reads it.
      **Measured and surprising: a1.1.2 stairs and furnaces face the struck face, not the player.**
      Only the lever consults the heading, and only on its top face — that one rule came out
      non-quadrant-shaped under this harness and is a documented gap rather than a guess.
- [x] Repeat while the button is held, every **five ticks** — `Minecraft.runTick` gates both mouse
      buttons on `ticksRan - lastClickTick >= Timer.ticksPerSecond / 4`, and the timer is built with
      20.0f. **This is not hardness-paced**, and an earlier note here saying it was is wrong:
      hardness governs the *progress* of a break, accumulated per tick through
      `Block.getPlayerRelativeBlockHardness`, which is a separate mechanism and is Survival's.
- [x] Selection outline. `src/core/render/outline.{hpp,cpp}` builds the geometry — host-tested, so
      the part that can be wrong in an interesting way is checked — and `shaders/outline.v.pica`
      plus `Renderer::drawSelection` put it on screen after the translucent pass, writing colour and
      no depth. **The PICA200 has no line primitive**, so a1.1.2's one-pixel `GL_LINE_STRIP` becomes
      twelve thin boxes; the 0.002 expansion is the original's.
      **⚠ No hardware number has been taken for this pass**, which CONTRIBUTING requires of anything
      touching the renderer. It builds and it is 432 vertices a frame rebuilt only when the
      crosshair changes block, but it has not been seen or measured on a console.

**Done when** a block placed on the console survives a pause-save and a reload. — *The host side is
proved; the console run has not happened.*

## 3. Creative — **done, except on hardware**

Small once 1 and 2 are in — mostly UI against a hotbar that is already drawn.

- [x] Note in the file header, and in `status.md`, that **a1.1.2 has no Creative mode** — it is
      Beta 1.8's. Ours is invented, which is already why gamemode lives in `3dalpha.ini` and not in
      `level.dat`. There is no oracle for its rules and there should not pretend to be one.
      `core/item/creative_palette.hpp` carries the argument, `tests/creative_test.cpp` repeats it
      so a suite that looks like the oracle suites beside it is not mistaken for one, and
      `status.md` §*0s* is the write-up.
- [x] **Hotbar on the ~~top~~ bottom screen**, as a band under every player page; block palette in
      a **Blocks** tab of its own; touch to pick, shoulder to page while focused and touch arrows
      when not.

      **The bullet said "top screen" and that was wrong on two counts.**
      `docs/3ds-performance.md` §11 has said since before any of this that the hotbar goes on the
      bottom screen so the top renders nothing but the world — no HUD overdraw at all on a
      fill-bound device — and the top screen has no 2D pass in game at all: no citro2d, no
      crosshair, only the outline pipeline. A hotbar over the world would have meant a new textured
      screen-space GPU pass and a hardware fill-rate number to go with it. The bottom screen is also
      the only one that can be touched.

      **And the palette is its own page rather than the Items tab.** They are different things: a
      catalogue held by nobody against what a player is carrying. Creative's strip is Map, Items,
      Blocks, Look, which is exactly `hud::kMaxTabs`.

      The bottom screen is four bands now — tabs 0–24, the focus banner's band 24–40, page 40–208,
      hotbar 208–240 — all reserved in every gamemode so there is one layout rather than two. **The
      map went from 208×200 to 208×158 and got cheaper for it**: 32,864 pixels against the 36,864
      that the measured 700 µs was taken on, so an estimated 625 rather than the 790 the bigger
      window cost.
- [x] Instant break, no stack depletion, flight toggle reusing Spectator's mover but with collision.

      **Instant break was already true and nothing had to be added.** Hardness governs the
      *progress* of a break through `Block.getPlayerRelativeBlockHardness`, which is Survival's and
      does not exist — so one press has always been one broken block, and step 4 has to take that
      away rather than step 3 add it. Depletion is the same shape: the hotbar holds a stack of one
      and the place path never touches the count.

      Flight is `PlayerBody::tickFlying` — Spectator's `flyCamera` with `moveEntity` under it, so it
      collides, which is the whole difference between the two. Speeds are Spectator's own 12 and 40
      blocks a second converted to ticks (0.6 and 2.0), and are the only two invented numbers in
      `player_body.hpp`. Toggled by a double tap on B, timed in frames because it is a gesture.
- [x] Flip Creative from greyed to selectable in World Settings. The row was reordered while it was
      open — Spectator, Creative, Survival — so the two live modes are adjacent: stepping it is one
      button, and a disabled mode between them would make every switch pass through a state that
      refuses.

**X focuses the bottom screen**, which was not in this list and is the thing that makes the palette
usable: the screen is resistive, a player walking has no stylus out, and focused the d-pad drives a
cursor over the palette and the hotbar while the world keeps running. ZL and ZR change the held slot
from anywhere — and do not exist on an old 3DS, which is the other half of why the focused d-pad is
not a convenience.

Three things came out of using it, all in `status.md` §*0s*:

- **A banner says the screen is focused**, and it needed a band of its own. The fade darkens the
  pixels under it, which cannot be applied twice — so it sits in 24–40, which nothing but the
  backdrop paints, and every page below it starts at `hud::kPageTop`. That is the 16 pixels the map
  gave up.
- **L and R change tab rather than paging the palette**, and the focus survives the change. A
  focused screen with no button route between Map, Items and Blocks could only be navigated by
  touching it, which is the one thing the focus exists to avoid. The palette pages by running the
  cursor off either end of its grid, and by the arrows on its title row.
- **The focused stick scrolls the map**, half a window a second at every zoom, with the panel's `x`
  and `z` naming the middle of the picture in amber and a cross on it. Letting the focus go puts the
  map back on the player. The map keeps its own d-pad — zoom and grids — while focused, since the
  stick is doing the moving.

**Not done: none of it has been seen on hardware.** It builds for the 3DS and the host suite is
green. That is the same debt step 2's selection outline carries.

**Also not done: the hotbar is not saved.** a1.1.2 writes the inventory into `level.dat`'s `Player`
compound and this project preserves that compound verbatim rather than parsing it; writing one back
means going through the `items` slot, which is empty for this version. The contents live for the
session and start from the palette's first nine each time.

## 4. Survival

Rules on top of the same body — fall damage, block hardness and break progress, drops, stack
management. Health and hunger are `false` in the manifest's features for a reason; check each one
before assuming it exists in a1.1.2.

## 5. Entities — last, and split

The expensive part is not the logic, it is that **nothing in the renderer can draw one**. Chunk
meshes go through `VboPool` with a 12-byte vertex that has no room even for a face-flip bit
(`status.md`, the `BlockDoor` note). Entity models are a different vertex format, a different draw
cadence and a different budget on a 268 MHz ARM11. That is a design decision, not an increment.

- [ ] **Dropped items first.** No AI, one quad, and it exercises the whole `entitydata` slot
      end to end: `none` → a real implementation, `Entities` parsed instead of preserved, ticked at
      20 Hz, saved, reloaded, byte-compared against a real client's world.
- [ ] Entity model/renderer design note in `docs/` **before** any mob code, with a measured budget.
- [ ] Mobs after that: model, animation, AI, pathfinding, then the spawn algorithm.
- [ ] `nbtdiff.py` a copied real world before and after to prove nothing preserved got dropped.

## Standing rules that will bite here

- Collision dispatches on block **shape**, ticking on **behaviour**, rendering on **render type**.
  No block ids anywhere in any of the three.
- No allocation in the per-frame path; the body and the raycast allocate nothing.
- Copy the real world before opening it, every time — `storage.open()` writes `session.lock`.
- Constants come from `docs/physics-a1.1.2.md`, which took them out of the class file. Do not
  retype a remembered value; several of them are a float widened to double and differ in the
  eleventh digit, which is exactly the size of error that makes a vector test fail mysteriously.
- Run `make index` after adding files; delete this file when its last box is ticked. Steps 4
  and 5 are what is left, plus the hardware run that all three landed steps owe.
