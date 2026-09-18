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
- `Entities` round-trips as opaque preserved NBT (`src/core/nbt/preserved.hpp`,
  `src/impl/storage/alpha_chunkfiles/chunk_nbt.hpp:26`). Nothing parses it. That is deliberate
  and it still holds. **`TileEntities` no longer does** -- it is modelled, decoded and
  re-encoded; see step 5 and status.md 39.
- `src/impl/items/b1_2/` — **filled.** It holds the slot numbering (36 main, 4 armour at +100); the
  stack encoding was already in the Alpha level format. The inventory is saved. See `docs/status.md`
  §*4. The item table, the inventory, and five things the first Creative pass got wrong*.
- `src/core/entity/` has the body, the ray trace and the collision sweep; `src/core/item/` has the
  item table, the Creative palette and the 40-slot inventory; `src/core/item/` also has the break
  loop, the tool rules, the cursor-stack container and the crafting matcher, and
  `src/core/entity/player_vitals.*` the health. Spectator, Survival and Creative all parse from
  `<world>/3dalpha.ini` and **all three are selectable** — nothing is drawn disabled any more.
- **Nothing from steps 1 to 4 has been run on hardware.** Every one of them builds for the 3DS and
  passes on the host. That is the single largest open item in this file.

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
      **Y toggles the crouch rather than holding it**, and a crouching camera sits 0.08 lower —
      1.8.9's `getEyeHeight` drop, because a1.1.2 has no sneaking to read and the Betas lower only
      the model. `eyeY()` and `Pos[1]` do not move; see `kSneakEyeDrop` and `status.md` §M3.
      A crouching player also moves at three tenths, and **that half is a1.1.2's own** and had been
      missed: `MovementInputFromOptions` scales `moveStrafe`/`moveForward` by `0.3D` off the sneak
      key without consulting `isSneaking`. See `applySneakSlowdown` and `docs/physics-a1.1.2.md`
      § *Sneaking scales the stick*.

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
      *Nothing* in a1.1.2 consults the heading when a block is placed — there is no
      `Block.onBlockPlacedBy` in this version at all. This line used to say stairs and furnaces
      "face the struck face"; **they do not**. Neither has an `onBlockPlaced`: both turn from their
      neighbours in `onBlockAdded` (`ku.h`, `km.h`), and the sweep's single stone cube made that
      look face-driven. Only six blocks are in the table now; the furnace and stairs are
      `tick::blockAdded`, and the furnace's mouth is drawn where its metadata says
      (`block::worldFaces`). See docs/physics-a1.1.2.md, *Which way a placed block faces*. The table said otherwise until the sweep
      itself was fixed: it was punching every block after placing it, which flipped levers, pressed
      buttons, opened doors and lit redstone ore. See docs/physics-a1.1.2.md, *The lever's "heading
      rule", which was neither*.
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

~~**Also not done: the hotbar is not saved.**~~ **Done**, along with four other things this first
pass got wrong — half a door in the hand, the burning furnace in the palette, a camera pinned to the
tick, and a gamemode change that teleported you into the ground. The whole of it is written up in
`docs/status.md` §*4. The item table, the inventory, and five things the first Creative pass got
wrong*; the short version is that there is a generated item table now, measured out of a running
jar, and the hotbar was never the right shape for the save file because a door is item 324 and the
block it leaves is 64.

## 4. Survival — **done, except on hardware**

Rules on top of the same body, and there is no hunger among them: `hasHunger` is `false` in the
manifest because a1.1.2 has no food bar — eating heals directly (`oj`), which is why the HUD row
below is hearts, armour and bubbles and nothing else.

Every number came out of the jar. Two new `genref.java` emitters carry the parts that are tables
rather than code, and both are byte-identical on a rerun:
`data/a1.1.2/harvest.json` (96 entries of `strVsBlock`, `canHarvestBlock`, armour points, food
heals) and `data/a1.1.2/recipes.json` (96 recipes in match order, 7 smelting rules, 14 fuels).
`tools/configure.py` turns them into `harvest.hpp` and `recipes.hpp`. The derivation is
`docs/physics-a1.1.2.md` *Survival*; the write-up is `status.md` §46.

- [x] **Health, damage and death** — `core/entity/player_vitals.{hpp,cpp}`: the hurt window checked
      in `dm` before `ge` ever sees the hit, difficulty scaling for monsters and arrows only,
      armour in twenty-fifths with its carry and its wear, and the per-tick fall, fire, lava, void,
      suffocation, drowning and Peaceful regeneration. `PlayerBody` stays pure physics and reports
      the landing through `landedFall`, the same shape `steppedOn` already had.
- [x] **Death and respawn** — the inventory scattered as `dm.b` scatters it, `deathTime`, and `au`
      (GuiGameOver) as a bottom-screen panel with Respawn and Title menu. The world keeps ticking
      under it, because `au.b()` is false. A respawn far from where the player died waits for its
      column to arrive before the body is let go.
- [x] **Persistence** — `Health`, `HurtTime`, `DeathTime`, `AttackTime`, `Fire` and `Air` load and
      save. A file with no `Health` reads as 10, which is the field's own initialiser and not a
      guess.
- [x] **Break progress, harvest and the crack overlay** — `core/item/block_breaking.{hpp,cpp}` is
      `nj`'s state machine, `core/item/tool_rules.{hpp,cpp}` the relative hardness, and
      `core/render/break_overlay.{hpp,cpp}` the destroy stages. **The harvest question is asked
      after the wear**, as the jar asks it, so a pickaxe that breaks on its last block drops
      nothing. There is no polygon offset on a PICA200, so the overlay is expanded 0.002 instead.
- [x] **Durability and spent stacks** — a hit costs 2 on a tool and 1 on a sword, a break 1 and 2;
      placements, doors, reeds, seeds, redstone, signs, paintings, boats, minecarts and saddles
      spend one; hoes and flint and steel take damage; food spends one and heals; the bow takes an
      arrow from the first slot that has one. Creative spends nothing, which is now the mode's rule
      rather than the build's.
- [x] **The HUD** — `core/render/hud_mesh.{hpp,cpp}` is `lu`'s own arithmetic, hearts, armour and
      bubbles, flashing and jittering as it does. **On the top screen**, which is the one departure
      from `docs/3ds-performance.md` §11 and is written up there, and **in the two top corners at
      twice size** rather than centred on a hotbar that is on the other screen: hearts left, armour
      right, bubbles under the hearts. `gui/icons.png` reaches the GPU
      through `Atlas::initIcons`, called from both `init` and `setAtlas` — the item-sheet bug
      again — and a pack without one gets a generated stand-in.
- [x] **Containers** — `core/item/container.{hpp,cpp}` is `ee`'s cursor-stack click,
      `core/item/crafting.{hpp,cpp}` the shaped matcher with its mirrored-first order,
      `core/tick/furnace.{hpp,cpp}` the `ke` tick with the lit/unlit swap that keeps its tile
      entity, and `core/item/container_session.{hpp,cpp}` the open screen: `lo`'s 2x2,
      `hx`'s 3x3, `id` and `ea`, all one slot list. `tick::blockActivated` opens them, which
      reverses the deviation `behaviour.hpp` used to carry.
- [x] **The bottom-screen pages** — `core/gui/container_layout.{hpp,cpp}` places the slots (host
      tested, so the hit test cannot drift from the drawing) and `hud.cpp` draws them. The hand is
      the hotbar band itself rather than a second copy of it; touch or the d-pad walks the
      rectangles by position, A is the left button, Y the right, X throws the cursor or, with the cursor empty,
      shift-clicks the slot under it (ours; `ContainerSession::quickMove`), B closes.
      Closing drops the cursor and the grid, through the same spawn-then-spend path a throw uses.
- [x] **Turned on** — `settings::gamemodeImplemented(Survival)` is true and the menus offer it.
      Creative to Survival keeps the health that was saved; Survival to Creative makes the player
      invulnerable, resets the breaker and closes any screen.
- [x] **Where a world starts you** — `core/world/spawn_point.{hpp,cpp}`: `cn.g` (climb from 63
      while the block above is not air), `cn.f` (the top block has to be **sand**, which is why an
      Alpha world begins on a beach), the constructor's unbounded walk from (0, 0) — bounded here,
      see the header — and `cn.a()`'s weaker ±8 nudge. The search runs once, at world creation.
      `kh.q()`'s lift out of the ground runs at world entry as well as on respawn, and is **owed**
      until the body's own column is resident, because a lift against columns that have not
      streamed in yet finds air and does nothing. A saved player is never lifted: `q()` runs before
      `cn.a(dm)` reads `Player` out of `level.dat`, so the file's position is used as written.
- [x] **Found on the first playthrough and fixed** — the Creative opening hand was being handed out
      in every mode; a corpse picked its own death drops back up (`dm.j()` guards the whole
      nearby-entity sweep with `if (health > 0)` and this did not); and the A that answered the
      game-over screen reached the world on the same frame as a drop, because `takeDeathChoice`
      clears `dead()` mid-frame. See `status.md` §47.
- [ ] **Not done: none of it has been seen on hardware**, which is the same debt steps 1, 2 and 3
      carry. What is specifically owed here is a fill-rate number for the top-screen HUD pass and a
      stereo check on it, the crack overlay's pass, and a playthrough of the four container
      screens on a real touch screen.
- [ ] **Not done: the replaceable-material test.** Placing into water, lava and snow is still
      air-only. It was scoped into this step and is a placement rule rather than a Survival one;
      nothing above it depends on the answer. `physics-a1.1.2.md` *What is not derived yet* keeps
      it.
- [ ] **Not done: `--survival` JVM oracle vectors.** The rules were transcribed from the class
      files and checked by hand against them; there is no `tests/survival_vectors.hpp` generated
      from a running player the way `--player` generates the body's. `--survive` on a real world
      is the end-to-end check that exists instead: it fails if a fall of more than three blocks
      costs no health, if water never drowns, or if stone broken by hand leaves cobblestone.
- [ ] **Open: a blob of world reported full of water from bedrock to ground level**, iced over,
      with the ground climbing steeply to build height beside it. Ruled out by measurement, not by
      argument: the terrain scan for an inverted density field finds nothing near the origin or
      millions of blocks out, **the same scan driven through the jar agrees with ours**, population
      has no lake generator, and a waterfall settles to the same profile in the port and in a real
      a1.1.2 World. Both Extra Settings are stream-neutral and neither writes air or water. Storage,
      the streaming worker and the mesher are what is left. It needs the world's seed and roughly
      where the patch was. `status.md` §47.

## 5. Entities — last, and split

The expensive part is not the logic, it is that **nothing in the renderer can draw one**. Chunk
meshes go through `VboPool` with a 12-byte vertex that has no room even for a face-flip bit
(`status.md`, the `BlockDoor` note). Entity models are a different vertex format, a different draw
cadence and a different budget on a 268 MHz ARM11. That is a design decision, not an increment.

- [x] **Dropped items first.** Landed in status.md §9. `core/entity/item_entity.{hpp,cpp}` is
      `EntityItem` -- the throw, the fall through the shared sweep, the bounce, the five-minute
      lifetime and the pickup -- and `core/render/item_entity_mesh.{hpp,cpp}` draws it, as a
      spinning quarter-size block or as a yaw-billboarded sprite. The renderer needed no new vertex
      format after all: the 16-byte `DetailVertex` the torches and particles already use takes
      arbitrary corners, so a rotated cube and a billboard both go down the existing detail pass.
      **gui/items.png had to reach the GPU**, which it had not before.
- [x] **Session entity persistence.** Dropped items, falling blocks, paintings, arrows,
      boats and minecarts save through a versioned `3DAlphaEntities` compound in level.dat
      (also inside packed manifests). Immutable snapshots go through the existing I/O queue
      on autosave, pause and close. This is a port extension, not native chunk entity support.
- [x] **The mob spawner block** -- `core/entity/mob_spawner.{hpp,cpp}` is `bd`, and
      `core/render/spawner_mesh.{hpp,cpp}` is `r`. Landed in status.md 38; the derivation is in
      the header. **The first tile entity that ticks**: `ic.b()` is a no-op and only `ke`
      (furnace, not ported) and `bd` override it, so this is the whole of a1.1.2's tile-entity
      tick list.
      - **The mob was in the world file already.** `readMobSpawners` claims `MobSpawner`
        compounds out of a column's *preserved* `TileEntities` as it joins the resident grid --
        a new pair of `WorldStreamer` column sinks, which is `cn.b(Lcu;)V` and `cn.c(Lcu;)V`.
        The tag is not rewritten. Measured on a copy of the real World1: 13 spawners, 5 Zombie /
        6 Skeleton / 2 Spider, and a real skeleton dungeon placed 7 skeletons in 2,000 ticks.
      - **Nothing is written back**, which is the line below and the rest of this box.
      - `ge.z()` was transcribed wrong and is fixed: the three Gaussians are drawn first and each
        is subtracted from the position, times ten. `MobSystem::explosionPuff` is the one copy.
- [x] **Tile entity persistence** -- **done**, and it had to be all four tenants at once.
      `core/world/tile_entity.{hpp,cpp}` is `ic` and its four subclasses; the codec is in
      `src/impl/storage/alpha_chunkfiles/chunk_nbt.cpp`. Landed in status.md 39.
      - **Sign text survives a reload**, which was this box's original tenant, and so does a
        spawner's mob -- the `TileEntities` list is *encoded* now rather than preserved. Writing
        it without modelling the chest would have emptied every dungeon chest in the world, so
        the chest's 27 stacks and the furnace's three landed with them.
      - **`id` is not guaranteed to come first.** `hm` is a `HashMap` and writes in bucket
        order, so each element is read twice over the same bytes.
      - **`reconcileTileEntities` is `ga.d`'s lazy heal done in bulk at the save.** Unknown
        entries are never judged: this build cannot tell which block a modded tile entity
        belongs to.
      - `PopulationSideEffects` has a consumer now. It goes onto the generator's `Entry` rather
        than onto a column, because a pass writes into a 2x2 quadrant and a dungeon rolled for
        one chunk routinely lands in the next.
      - `WorldStreamer` gained a `saving` column sink and `markColumnModified` (`ga.f()`), and
        `dropCell` fires `saving` **before** `dropped` -- the old order would have erased the
        entries the save was about to read.
- [ ] The rest of what that box was for: the **`entitydata` slot** end to end -- `none` → a real
      implementation, `Entities` parsed instead of preserved, ticked at 20 Hz, saved, reloaded,
      byte-compared against a real client's world. The session pool snapshot does not yet
      import or export Java's chunk `Entities` list.
- [x] **Block drops**, which turned out not to be an entity question at all: `idDropped` and
      `quantityDropped` off every block, generated the way the placement table is --
      `tools/genref.java --drops` -> `data/<ver>/drops.json` -> `core/tick/drop.{hpp,cpp}`. There is
      **no `damageDropped`**: `dropBlockAsItemWithChance` builds `new ItemStack(id)` in this
      version, so nothing keeps its metadata. Wired to the nine tick paths that call it; the hand's
      own break is Survival's and is step 4. See status.md §11.
- [x] **The falling block**, which is the second entity: `core/entity/falling_block.{hpp,cpp}` is
      `ff` and `core/render/falling_block_mesh.{hpp,cpp}` draws it. `BlockSand.fallInstantly` is a
      real flag and the instant path stayed as what it is rather than as a fallback.
- [x] Entity model/renderer design note in `docs/` **before** any mob code, with a measured budget.
      **Done: `docs/entity-render-a1.1.2.md`.** Its finding is that the box overstated the problem.
      The pipeline question was already answered by the dropped item -- `DetailVertex` takes
      arbitrary corners and rides the world shader -- and what was actually left was texture
      management: the detail pass samples one texture per draw and a1.1.2 draws these entities out
      of five more files. Two pieces came out of it, both landed and both tested:
      `core/texture/entity_skins.{hpp,cpp}` (one 128 x 64 sheet holding boat, cart, sign and arrow
      pages, plus `art/kz.png` as its own plane, with generated stand-ins under both) and
      `core/render/box_model.{hpp,cpp}` (`ip`/ModelRenderer, which holds exactly one box per part
      and whose texture space is a hard-coded 64 x 32). See status.md §13.
- [x] **Paintings**, the first of the four and the cheapest: no physics, no motion, no collision
      sweep. `core/entity/painting.{hpp,cpp}` is `jc` -- `setDirection`, `onValidSurface`, and the
      constructor's art draw over every `er` that fits -- and `core/render/painting_mesh.{hpp,cpp}`
      is `bw`, which does *not* use the box model because it lights each 16 x 16 cell separately.
      The art table is generated: `tools/genref.java --art` -> `data/<ver>/paintings.json`.
      Wired through a new **`spawns`** column on `ItemDef`, which is what tells "places no block"
      apart from "does nothing" -- the distinction all six entity-spawning items needed.
- [x] **The bow** -- **done.** `core/entity/arrow.{hpp,cpp}` is `kg` and
      `core/render/arrow_mesh.{hpp,cpp}` is `gk`. It does not use `moveEntity`: an arrow
      ray-traces from where it is to where it would be. **Gravity is 0.03, not 0.05**, and there
      is **no charge in this version**. One stated deviation: it costs no arrow, on the same
      no-depletion rule Creative's block placement already takes.
- [x] **The boat and the minecart** -- **done**, and **riding with them.** The riding question
      turned out to be small: `EntityLiving` never checks whether it is riding, so the movement keys
      keep becoming motion exactly as they do on foot and only the position is overwritten.
      `PlayerBody::tickRiding` is that and `core/entity/rider.hpp` is the seam. Y dismounts.
      The minecart's rail physics is transcribed in full -- snapped to the centreline, speed
      re-pointed rather than re-computed, height looked up rather than integrated -- and the
      connection matrix `oc.j` is exactly the ten shapes `core/tick/rail.hpp` already derives.
      **A chest or furnace cart is placeable and drawable and opens nothing**, which is the one
      piece of those two that is not ported.
      **Both are solid now**, which they were not at first: `cn.a(Lkh;Lcf;)` adds every nearby
      entity's `getBoundingBox()` to the list `moveEntity` clips against, and `dc` and `oc` are the
      only two classes in a1.1.2 that answer it with a box -- so a cart is something to walk into
      and stand on. The same method's other branch, `getCollisionBox` on the *mover*, is ported
      too, and the same two classes are the only ones that answer that: a rolling cart or boat is
      stopped by the mob, item, arrow, painting, TNT, falling block or player in front of it.
      `core/entity/entity_boxes.{hpp,cpp}` is that half of the list and
      `tests/entity_boxes_test.cpp` checks it; getting off now puts the player on the roof, which
      is `mountEntity`'s own tail.
- [x] **The `tileentity` half**, which signs need -- **done as a session store, not as a slot.**
      `core/world/sign_store.{hpp,cpp}` is `ob`, `core/render/sign_mesh.{hpp,cpp}` is `jk` through
      `in`, and `Overlay::editSignViaKeyboard` is `GuiEditSign` as one multi-line system keyboard
      rather than four. **The text survives the world closing** -- `readSigns`/`writeSigns` on the
      column, through the modelled `TileEntities` list; see the box below and status.md 39.
      **There is no generated font**, so a sign on Dev Art shows a blank board -- the one place in
      this project where Dev Art is less than a real pack.
- [x] **The compass** -- **done.** It is not an entity and not an item behaviour: item 345 is a
      plain `di`, and the whole of a compass in a1.1.2 is `aa` (TextureCompassFX) rewriting its
      16 x 16 icon every tick with a needle aimed at `spawnX/spawnZ`.
      `core/texture/compass_fx.{hpp,cpp}` is that, on the world's 20 Hz clock rather than the
      frame's for the reason the flames already are. The items sheet has an animated-tile path now
      (`Atlas::updateItemsTile`), and the bottom screen takes an **override** rather than a write
      into the pack's pixels. Which tile is read off the FX class by the generator -- nothing names
      item 345. The same sweep settles that **a1.1.2 has no clock**: six `TextureFX` and only one
      of them on the items sheet.
- [x] **The held item in the corner of the top screen** -- **done.**
      `core/render/held_item.{hpp,cpp}` is `ItemRenderer` -- the equip animation and the arm swing
      on the 20 Hz clock, and `renderItemInFirstPerson` plus `renderItem` as camera-space quads --
      and `Renderer::drawHeldItem` is one extra pass at the end of each eye. Self-contained as
      estimated: `block::renderBoxes` gave the block branch, and the flat branch is the original's
      own **66-quad extrusion** of the icon rather than the item entity's single billboard.
      An **empty hand draws the arm**: `char.png` is a pack file like `terrain.png`, so it gets a
      fifth page of the entity sheet (which grew 128 x 64 -> 256 x 64; the four existing offsets did
      not move) and a pack without one gets a **black silhouette** rather than a placeholder grid.
      Spectator gets no hand at all, which is a state of its own.
      **Options -> Skin** picks between Default, any pack's `char.png` and any `.png` in
      `sdmc:/3dalpha/skins` (`core/texture/skin_list.{hpp,cpp}`). A slim skin is detected, marked as
      such and still drawn on the wide arm -- `hasSlimSkins` is false for this version and
      `held_item.cpp` fails the build if it is turned on without deriving that version's box.
      Three of its numbers are the screen's rather than the game's -- a 0.14-block sideways shift
      for a 5:3 window, its own 0.05 near plane, and `C3D_DepthMap` standing in for the
      `glClear(GL_DEPTH_BUFFER_BIT)` citro3d cannot do mid-frame. See `docs/status.md` §15.
      **It still owes a hardware fill number and a look at its stereo** (CONTRIBUTING.md): the
      separation is a sixteenth of the world's, and that sixteenth is arithmetic, not a measurement.
      The first hardware run found it drawing at zero alpha -- see `docs/status.md` §15 -- so the
      run that confirms it is drawn at all is still owed too.
- [x] **Hitting entities.** The break button never looked for one, so a boat, a cart and a
      painting could not be broken and never dropped anything. `item::attackEntity` is the
      entity half of `Minecraft.clickMouse`; each pool has an `attack` transcribed from its own
      `attackEntityFrom`. The `damageVsEntity` column now exists and `attackEntity` takes the
      held item, so a sword hits for 4 to 10 and a tool for its own number; the unknown row's 1
      is `InventoryPlayer`'s empty-slot answer, so the bare hand needs no branch. Durability is
      still not spent -- `stack.hitEntity` has nowhere to go. See status.md §16.
- [x] **The four peaceful animals** -- pig, sheep, cow and chicken -- model, animation, AI,
      pathfinding and the spawn algorithm. Landed in status.md §23; the derivation is
      `docs/mobs-a1.1.2.md`.
      - The 15-animal cap is kept, and it is a cap on *spawning* rather than on the list: a
        chicken's egg clock can push the count past it, as it does in the original.
      - **No breeding, no babies and no taming**, because this version has none of them. An
        earlier pass ported 1.1's breeding and it was removed for parity -- see status.md §23.
      - **Per-frame model building was not the cost it was expected to be.** The estimate in §18
        was 200 bipeds; the reality is fifteen quadrupeds of six to twelve boxes, which is 4,608
        vertices in the worst case -- one draw call, one bind, and the same detail pass the boat
        already rides. The static-buffer-and-part-matrices plan is written down in §18 and is
        still the right answer *if monsters land*, which is where 200 of anything comes from.
      - Pathfinding has its per-tick budget: one search per tick across all animals, and 1,024
        nodes per search. Neither binds in ordinary play.
        *(The per-tick budget was removed on 2026-09-17: it bound hard once the monsters landed,
        and it sat in front of the wander branch's random draws. Only the node ceiling remains --
        see docs/mobs-a1.1.2.md, "The pathfinder".)*
      - **Monsters are not ported**, and with them `ek`'s attack branch and the 200-cap spawner.
        *(Closed below.)*
- [x] **The five hostile mobs** -- zombie, skeleton, creeper, spider and slime -- with `ek`'s
      attack branch, `dq`, the 200-cap spawner, the explosion, the spider jockey and a difficulty.
      Landed in status.md 25; the derivation is docs/mobs-a1.1.2.md, *The five hostiles*.
      - **Nine rows of one table**, not five new classes: `MobDef` gained six columns and `MobAi`
        is the jar's five overrides of `ek.a(Lkh;F)V`. `hostile` is a column because `co` is an
        interface and the slime is not an `ek`.
      - **`core/entity/explosion.{hpp,cpp}`** is `je`, all three phases, with the cell record as a
        bitset on the caller's stack. `dropBlockAsItem` took the `chance` the jar's method always
        had.
      - **A difficulty** in `<world>/3dalpha.ini` with a World Settings row. Peaceful is a removal
        and not a suppression, which is a1.1.2's and is why the spawner still costs what it costs.
      - **The player has no health yet**, so a monster's fist leaves through a seam and
        `main.cpp` takes the knockback and the sound and counts the damage. That is step 4's, and
        it is the one thing these mobs are still short of.
        *(Superseded: `PlayerVitals` landed with step 4 and `PlayerHarm::hurt` is wired in
        `main.cpp`, so a monster's fist lands. Checked 2026-09-17.)*
      - `ctr::kMaxSamples` 80 -> 128 off a second host measurement (108 samples / 6.6 MB).
- [x] **The entity sounds** -- `random.splash`, the two `random.fizz` sites, and the arrow's
      `random.drr` moved off the player's ears and onto the arrow. Landed in status.md 24; the
      derivation is docs/audio-a1.1.2.md, *What an entity plays*.
      - **Which entity splashes is derived**: `kh.e_()` is one line, `y()`, so it is whoever calls
        `super.onUpdate()` -- the player, the four animals, dropped items, arrows and boats. The
        falling block, the painting and the minecart do not, and enter water silently.
      - **The real fault was the boot list.** A key that was never preloaded is silence, and the
        list lived in `platform/ctr/main.cpp`, so `random.bow` and all eight `mob.*` keys were
        played correctly and heard by nobody. `audio::preloadEffects` is the one list now, half of
        it derived from the block table and `MobDef`.
      - `ctr::kMaxSamples` 48 -> 80, off a host measurement (72 samples / 3.4 MB) rather than an
        estimate. `--audio-list` prints it.
- [x] **TNT** -- `core/entity/primed_tnt.{hpp,cpp}` is `jd`, and with it all four of a1.1.2's
      ignition paths. Landed in status.md 30; the derivation is in the header.
      - **Four paths, and three of them were already written as comments naming the gap**: a break
        (`q.b`, through `tick/drop`), a fire (`og.a`, through `tick/fire`), a blast (`q.c`, through
        `je`'s third phase) and a neighbour going live (`q.a`, through `tick/redstone` -- the one
        that needed a subsystem, and the subsystem was already there).
      - **Breaking TNT lights it and drops nothing.** `quantityDropped` is a hard 0 and `q.b` has
        no metadata guard in this version, so crafting is the only way to obtain a block of it.
      - **The fuse test is off by one on purpose**: `if (fuse-- <= 0)` reads the old value, so 80
        is eighty smoking ticks and an eighty-first that explodes.
      - **A blast re-primes rather than detonating**, on `nextInt(20) + 10` and with no
        `random.fuse`, which is what makes a chain ripple and sound like one.
      - **The white flash is a second draw and not a colour.** The detail pipeline modulates, and
        modulation cannot make a textured cube white; the flash goes through the outline pipeline
        -- untextured, blended, uniform-tinted -- which is what `hw` does with the fixed-function
        state. One draw call per flashing entity, capped at sixteen.
      - Both sound keys were already preloaded for the monsters, so no change to `kMaxSamples`.
- [x] **The flames on a burning mob** -- `core/render/entity_fire_mesh.{hpp,cpp}` is
      `ak.a(Lkh;DDDF)V`, the fire half of `doRenderShadowAndFire`. Landed in status.md 32; the
      derivation is in the header.
      - **The tick was already right and the frame drew nothing**, so a burning mob was a mob that
        died for no visible reason.
      - **`ceil(height / width)` camera-facing sheets** off **one** tile -- a1.1.2 reads
        `Block.fire.blockIndexInTexture` above the loop where later versions alternate two -- full
        bright, leaning toward the camera, narrowing as they rise.
      - **`(int)(1.8F / 0.6F)` is 2**, because the float quotient is 2.99999976: the depth term
        truncates where the loop takes the ceiling, so a zombie's three sheets are placed as
        though it had two. Java gets the same quotient. Pinned by test.
      - **Still not ported: the shadow**, which is the other half of the same method.
- [x] **Fire burns entities** -- `core/entity/fire_entry.hpp` is the tail of `kh.c(DDD)V`, wired
      into every pool that calls `moveEntity`. Landed in status.md 33.
      - **The ignition is a box test in `moveEntity`, not a block callback**, which is why looking
        for an entity hook on `og` found nothing and this was written down as "a1.1.2 does not do
        it". The old note and the test that pinned it are corrected.
      - **`fire <= 0` is "not burning"**: the counter rests at `-fireResistance`, catching fire is
        the tick it reaches zero, and a counter left at exactly zero can never catch.
      - **A dropped stack is gone in five ticks**; a mob loses about a heart a second; a boat and a
        cart break after four; an arrow, a falling block and TNT char and take nothing.
      - **The arrow and the painting never burn** -- neither calls `moveEntity`.
      - **Still open: the player**, which needs the health Survival brings. The first-person fire
        overlay is the same gap.
- [x] `nbtdiff.py` a copied real world before and after to prove nothing preserved got dropped.
      **Done, and it is a harness mode rather than a one-off**: `--rewrite <world-dir>` loads
      every chunk and writes it straight back, changing nothing on purpose, so every difference
      `tools/nbtdiff.py difftree` reports afterwards is a bug in the codec. Over a copy of the
      real World1: 1,119 chunks, 66 tile entities (39 Chest, 27 MobSpawner), 195 item stacks,
      **1,119/1,120 files semantically identical** -- the one difference being `level.dat`'s
      `LastPlayed`, which is a clock. `--rewrite <copy> reconcile` additionally runs the
      heal-and-drop pass over all 1,119 columns and reports **0** entries added or dropped.

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
