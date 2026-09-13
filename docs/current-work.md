# Current work

Last verified: 2026-09-13. A compact handoff, not a substitute for inspecting the current diff.
Replace superseded facts here; keep detailed history in `status.md`.

## Checkout context

The working tree already contained extensive tracked and untracked gameplay/rendering changes
before the entity persistence fix. These are user work, not disposable build output. Do not
infer ownership from whether a file is tracked. No commit or deployment was made in this session.
Older narrative sections and source comments may describe gaps that later changes closed;
check the implementation and tests before treating those gaps as current.

**A second streamer case was timing-dependent and is not any more.**
`tests/streamer_revisit_test.cpp` --
`a_block_change_moves_the_columns_serial_and_a_reload_does_not_reuse_one` asserted that the frame
straight after `settle()` writes no blocks. How many frames `settle` takes is decided by a worker
thread, so which tick a freshly generated world's outstanding updates land on is not fixed, and the
case passed or failed on how busy the machine was -- adding two unrelated cases ahead of it was
enough to flip it, and it *aborts the process*, taking the rest of the suite with it. It now looks
for a quiet frame instead of demanding that one named frame be quiet, which is the same claim
without the race.

**One pre-existing flake blocks a clean full-suite run.**
`tests/streamer_map_work_test.cpp` --
`an_offer_made_while_the_world_is_still_being_generated_never_holds_generation_up` (`offers > 0`,
line 381) fails on roughly two runs in three and *aborts the process*, so every test registered
after it is skipped. Run those files' cases separately until it is fixed; it is a timing
assumption about the generation worker, not a regression in what it is testing.

## Fences need ground again, and sign boards stopped sliding (2026-09-13)

**Fences follow `fh.a(Lcn;III)Z`** (read off the jar): refused on another fence and over any
material that is not solid (`def.solid`), then the base cell test; `fh` has no `canBlockStay`.
`tick::canPlaceAt` checks it ahead of the behaviour switch. The old free placement is the Extra
Setting **Improved Fence Placement** (`improved_fence_placement` in 3dalpha.ini, default off),
on both Extra Settings and Create World; `WorldStreamer::open` hands it to
`TickWorld::setImprovedFencePlacement`, and `main.cpp` re-applies it after the pause menu.
`tests/placement_test.cpp` -> `a_fence_needs_solid_ground_...`; the settings round-trip covers the key.

**Sign boards drifted opposite to the player.** The 1/512 change below moved the text to 1/512
but the board and post still went through `buildBox` at 1/1024, so the 2x draw scale put them at
twice their offset from the eye's block. `buildSignBoards` now shrinks the placement by
`kSignUnitScale`. `a_sign_is_drawn_out_to_sixty_four_blocks_and_not_past_them` had been failing
since that change (its buffer was smaller than one sign's budget charge); it is sized right now
and pins the board's position.

## Six reports from play: an invisible join, far signs, stacked torches, buckets, X (2026-09-13)

**Joining could draw no blocks until a chunk was crossed.** The console keeps one static
`WorldStreamer` and builds a `ctr::Renderer` per world; `close()` left `centreSet_` and the centre
behind, so reopening in the chunk the last session ended in skipped `update()`'s centre-moved branch
-- the only thing that calls `renderer.setCentre` -- and the new field stayed centred on (0, 0),
refusing every column. `open()` and `close()` clear it now. Reproduced on the host first (far from
the origin; near it the two fields overlap and hide it):
`tests/streamer_revisit_test.cpp` -> `a_world_reopened_in_the_chunk_it_was_left_in_draws`.

**Signs vanished past 31.25 blocks.** The detail short at 1/1024 of a block ran out there; a1.1.2's
`fz.a(Lic;F)V` draws tile entities under a squared distance of `4096.0D`. Sign vertices are 1/512
now (`render::kSignUnitsPerBlock`), `drawSigns` scales the matrix back, and the builders apply the
64-block test. `tests/sign_test.cpp` -> `a_sign_is_drawn_out_to_sixty_four_blocks_and_not_past_them`.
**Not found: signs staying invisible after "leaving them for a while".** The store survives walking
40 chunks out and back, cache eviction and re-reads from disk, and autosave (host probes, deleted);
the light is raw stored light, and a pack change re-uploads the font. If it still happens within 60
blocks, it is not the store.

**A torch clicked onto a torch stood on it.** `placementMetadata` is `onBlockPlaced` measured against
stone; the jar only takes the struck face when that neighbour is an opaque cube and otherwise keeps
`onBlockAdded`'s first wall. `tick::attachedPlacementMetadata` applies that to torches, levers,
buttons and ladders (ladder order +z, -z, +x, -x; a lever's fallback goes through `leverPlaced`'s
roll). `tests/placement_test.cpp` -> the two `..._clicked_onto_a_..._hangs_on_the_wall_...` cases.

**Creative buckets keep their contents** (ours): the fluid still goes down and comes up, the held
item does not change -- pour, fill, milk and the cow. `main.cpp`, not host-tested.

**X on the focused bottom screen no longer releases it**; B does. On a container screen (inventory,
workbench, furnace, chest) X is a shift-click, `item::ContainerSession::quickMove` (ours; a1.1.2
has no modifier click): screen slots to the player (chest and furnace output fill the hand from
the right), a result crafted as many times as fits whole, player slots into the chest, a furnace's
input or fuel, or the matching armour slot, otherwise backpack <-> hand. A carried stack is still
thrown first. Five cases in `tests/container_session_test.cpp` (`a_quick_move_...`).
Creative has no session, so X there goes through `item::Inventory::quickMove` (hotbar row or
Items grid: hand <-> backpack, armour on and off) and `giveStack` (palette: a full stack into the
hand, then the backpack); wired in `Overlay::handleInput`'s X branch. Three cases in
`tests/creative_test.cpp` (`a_creative_quick_move_...`, `a_palette_quick_move_...`).

**The hoe was not changed.** `fu.a` refuses grass whose cell above is `Material.isSolid()` (the
`gb.a()` the `solid` column is extracted from) and tills dirt regardless -- a1.1.2's rule, which the
port already matches. Grass under a block dies to dirt on its own, and dirt tills.

## The chest connects, a third one goes in through water, and a seed breaks bedrock (2026-09-13)

Four things, three of them out of `b` (BlockChest) and one of them ours. Details: status.md 50.

**The chest's faces are a rule now, not a row.** `b.a(Lnm;IIII)I` reads up to eight cells: a lone
chest turns its front away from an opaque neighbour (`opaqueCubeLookup`, so glass does not count),
and a pair draws one picture across two cells with the halves mirrored on the far side. New
generated column `worldTexture` in blocks.json (maintainer-assigned, as `tick` is; the extractor
can only *name* the blocks that need a world), the rules in `src/core/block/world_texture.hpp`,
the chest off the mesher's fast path, and `tickBlockChanged` now invalidates the fourth corner
section -- the first thing in the project that reads a diagonal neighbour.

**`BlockChest.canPlaceBlockAt` is ported and the triple chest still works.** One chest may join one
unpaired chest, and no more -- *except* through `World.canBlockBePlacedAt`'s early return for a cell
holding water, lava, fire or a snow layer, which never asks the block. Dry land refuses the third
chest; a puddle does not. `chestInventoryParts` already joined up to five.

**Item 295 has its own path again.** `ItemSeeds.onItemUse` asks nothing -- no air test and no
placement test -- so a crop is written over whatever sits above the farmland, and farmland's 15/16
height keeps its top face clickable under a block. A seed removes bedrock; nothing else in a1.1.2
does. Routing it through `ItemBlock` had silently fixed that.

**A chest taller than the band scrolls.** Six rows shown, `chestFirstRow` clamped in
`buildContainerLayout`, and slots outside the window carry a zero-sized rectangle so no index
moves. L/R, the two arrows in the gutter, or the d-pad off the edge of the window.

**Four things the first console look found, all fixed.** A double chest wore *grass* on its long
sides: the cube pass samples a 512x512 atlas of repeat slots handed out per tile a cube face can
show, that list was built from `faces` rows alone, and a tile with no slot falls back to tile 0 --
grass. `buildCubeAtlasLayout` now asks `block::worldTextureTiles` as well (58 of 64 slots used, up
from 54). The "Chest"/"Large chest" titles are gone. The scroll thumb was the page's own colour and
is a raised bevel in a wider track now. And the world list kept a *row* where it should keep a
*world* -- the list is sorted by last played, so coming out of a world moved it under the cursor's
feet; `worldCursorName_` remembers the name, and creating a world now leaves the menu on the list
with the new world selected instead of on the Create screen.

New `tests/chest_test.cpp`; cases added to `use_test.cpp`, `container_layout_test.cpp` and
`greedy_test.cpp`. Host suite 1621/1621, 3DSX build links, `extract_blocks.py --verify` still
agrees on all 70 blocks. No hardware run of my own.

## The near plane, the spawn lift's order, and a death screen that outlived its world (2026-09-13)

Four reports off a Survival session; three of them turned out to be one number.

**`kNearPlane` is 0.05 now, a1.1.2's own, down from 0.2.** The view cutting through a ceiling when
you jump, into a wall you are standing along, and into the corners when you pitch down are all the
near *rectangle*, which on this console is not even in front of the eye: `Mtx_PerspStereoTilt` is
off-axis, so each eye is displaced sideways by `t*A*iod/2` = 0.19 of a block before its rectangle
has any width. At 0.2 it reached 0.420 sideways and 0.244 up, against 0.30 to a flush wall and 0.18
to a ceiling the head is against — both lost. At 0.05 it reaches 0.248 and 0.061. 0.1 would *not*
have fixed the wall: the sideways figure is mostly the stereo displacement, which does not shrink
with the near plane. The 0.2 was traded for depth precision a 16-bit buffer needed; both eyes have
been 24-bit since the shimmer fix, so a depth unit at the far plane is 0.037 of a block. Two other
comments quoting 0.2 were stale and are corrected, including the held item's depth-window
arithmetic — that window gets *safer* as the near plane falls. **Arithmetic, not hardware.**

**The spawn lift now runs before the player's `y()`, not after it.** A fresh Survival world opened
at 19 health: the body stands at `spawnY + 1` (64, whatever the ground is doing), and the frame loop
ran `ge.y()` — which takes a point for an eye inside an opaque block — a tick before the owed lift
took it out of the hillside. a1.1.2 calls `kh.q()` from the constructor, so nothing ticks a buried
body there. The wait/lift decision is hoisted above the `y()` block, and `y()` is skipped entirely
while the body is held for a column that has not arrived. Host coverage:
`tests/player_vitals_test.cpp` → `an_unlifted_spawn_point_suffocates_and_the_lift_is_what_stops_it`.

**`Overlay::begin` clears the death state.** Dying, choosing *Title menu* and opening another world
left the game-over screen up over a living player with no way out of it: only that screen clears
`dead_`, and the loop only raises it on `!alive() && !dead()`. The four fields are cleared directly
rather than through `setDead(false)`, which would close a container session `begin()` is about to
rebuild.

1,610/1,610 host cases pass and the 3DSX cross-build succeeds. No hardware run.

## The two screens: a square hotbar slot, Spectator's forty pixels, and a wear bar (2026-09-13)

**What changed.** Five things, all of them what the player sees:

- **Top screen.** `render/hud_mesh` scales the icon sheet by `kHudTexelUnits = 30` sixteenths of a
  pixel instead of a whole `kHudScale = 2`, so hearts, armour and bubbles are 6.25 % smaller. The
  scale is expressed in the units the vertices already use, so nothing rounds.
- **The hotbar band is 40 pixels**, which makes its slot 36 tall against 35 or 36 wide -- square.
  The eight pixels came from the focus banner, not from the page, so `kBandedPageTop` is still 48.
- **The focus banner is one character row** and no longer fades. The destructive gradient is what
  forced the reserved band to be painted exactly once per clear; the row is still reserved so a
  page redraw cannot erase it, but that is the whole of the rule now.
- **Spectator lays its pages out forty pixels higher.** `hud::pageTop()` is 48 with a hotbar and 8
  without, and every page is a `constexpr` function of it -- `lookLayout`, `paletteLayout`,
  `itemsLayout`, `columnFor`, `bandFrom` -- static-asserted at both values, so neither shape of the
  screen is the untested one. `buildContainerLayout` takes the page top as an argument.
  `Overlay::setGamemode` sets the flag before its early return and from its argument, because
  `gamemode_` starts as Spectator.
- **Margins went to the pages.** The map is 212 x 162 (212 x 202 in Spectator), the inventory panel
  is inset one pixel and its slots are 31, and the palette and look pad stop four pixels short of
  the tab strip rather than eight.

**The map's cost is an estimate, not a measurement.** Scaled off the one number there is -- 36,864
pixels in about 700 microseconds on a New 3DS -- the banded window is roughly 650 and the bare one
roughly 815. `3ds-performance.md` §11 owes this page a hardware figure. The map's chunk store went
to 1,024 / 2,048 entries because the bare window touches 756 patches at the widest zoom against the
banded window's 588, and a store that cannot hold the window thrashes rather than degrading.

**The durability bar is drawn now** -- `ab.b(kd,ey,ev,II)V`, the half of RenderItem's GUI overlay
that had never been ported, integer arithmetic and all three quads. `drawWearBar` scales texel
edges rather than texel counts, so the two bars stay in proportion on a 24-pixel icon.

**ZL/ZR move the focused cursor to the slot they select**, through `Overlay::cursorToHand`: a
container session's hand is its last nine slots, every other page drops out of the grid to the
band, and the map page is left alone.

Validation: 1,609/1,609 host tests pass; the 3DSX cross-build succeeds. Nothing here has been seen
on hardware. Detail in `status.md` 48.

## Survival is on (M3 step 4)

**What changed (2026-09-13).** `settings::gamemodeImplemented(Survival)` is true and the menus
offer it. Everything the flag was waiting for is in, derived from the jar and host-tested:

- **Health** — `core/entity/player_vitals.{hpp,cpp}`: the hurt window, difficulty scaling for
  monsters and arrows only, armour in twenty-fifths with wear, and the per-tick fall, fire, lava,
  void, suffocation, drowning and Peaceful regeneration. `PlayerBody` reports its landing through a
  new `landedFall` field and otherwise stays pure physics. The six counters load and save.
- **Death** — the inventory scattered as `dm.b` does it, and `au` (GuiGameOver) as a bottom-screen
  panel; the world keeps ticking under it. A respawn waits for its column before the body is let go.
- **Breaking** — `core/item/block_breaking.{hpp,cpp}` (`nj`'s loop) over
  `core/item/tool_rules.{hpp,cpp}` (relative hardness), with `core/render/break_overlay.{hpp,cpp}`
  drawing the stages. The harvest question is asked **after** the wear, as the jar asks it.
- **Wear and spend** — hits, breaks, placements, hoes, flint and steel, food and the bow's arrow.
  Creative spends nothing, which is the mode's rule now rather than the build's.
- **The HUD** — `core/render/hud_mesh.{hpp,cpp}`, `lu`'s own layout, **on the top screen**. That is
  a deliberate deviation from `3ds-performance.md` §11 and is written up there; it owes a hardware
  fill number.
- **Containers** — `core/item/container_session.{hpp,cpp}` is the open screen (`lo`, `hx`, `id`,
  `ea` as one slot list), `container.{hpp,cpp}` the cursor-stack click, `crafting.{hpp,cpp}` the
  shaped matcher, `tick/furnace.{hpp,cpp}` the `ke` tick. `core/gui/container_layout.{hpp,cpp}`
  places the slots and `platform/ctr/hud.cpp` draws them; the hand is the hotbar band itself.
- **Generated** — `data/a1.1.2/harvest.json` and `recipes.json`, from new `genref.java --harvest`
  and `--recipes` emitters; both byte-identical on a rerun.

**Two things scoped into the step and deliberately not done**, both recorded in `todo-m3.md` §4:
placement into water, lava and snow is still air-only, and there is no `--survival` JVM vector file
— the rules were transcribed from the class files and are checked end to end by `--survive`
instead.

`./build-host/3dalpha --survive <world-copy>` is the harness: it fails if a fall of more than three
blocks costs no health, if water never drowns, or if stone broken by hand drops cobblestone. It
writes blocks (a clear column to fall down, water over the head, one cell of stone) and puts every
one of them back, but run it on a **copy** like everything else here. On World1 it reports a
10.81-block fall for 8 damage, 2 from drowning after 320 ticks, 152 ticks to break stone by hand
with no drop, and 13 ticks with a stone pickaxe for one cobblestone and a point of wear.

Full write-up: `status.md` §46. Derivation: `physics-a1.1.2.md` *Survival*.

## Five things the first Survival playthrough found (2026-09-13)

Reported from a real session on hardware, all five now closed or pinned down:

1. **A new Survival world handed out the Creative opening hand.**
   `Overlay::setInventory` filled an empty inventory from the palette whatever the mode was. It is
   Creative's convenience and it is now Creative's only; Spectator is left out too, since a hand
   filled there would follow the player into Survival the next time the pause menu changed mode.
2. **The player spawned inside the ground and suffocated.** Two missing pieces, both in the jar:
   `kh.q()`'s lift out of the ground was only ever run on a respawn, and `cn`'s constructor walk —
   the one that looks for a column with **sand** on top, which is why Alpha worlds start on a beach
   — was not run at all, so every world began at x = 0, z = 0 with `spawnY` at 64. Both are in:
   `core/world/spawn_point.{hpp,cpp}` is the search (run once, at world creation, in
   `Menu::createWorld`), and the lift is now *owed* rather than made — paid on the first tick the
   body's own column is resident, because a lift against columns that have not streamed in yet
   finds air and does nothing. See `physics-a1.1.2.md`, *Where a world starts you*.
3. **Dying looked like it dropped nothing, and then the A that respawned dropped an item.** Two
   bugs that add up to one story. `dm.j()` guards its whole nearby-entity sweep with
   `if (health > 0)` and this port did not, so the corpse picked its own death drops back up while
   the game-over screen was still open; and `Overlay::takeDeathChoice` clears `dead()` in the middle
   of the frame, so every later `!overlay.dead()` guard let the same frame's A press through to the
   world as a drop. The frame now runs against `wasDead`, read before the choice is taken.
4. **The HUD row was too small and in the wrong place.** Hearts to the top left, armour to the top
   right, bubbles below the hearts, everything at twice its texel size -- 1.875 times it since,
   see below. The arithmetic inside a row is untouched. `3ds-performance.md` §11 and `physics-a1.1.2.md` *The HUD* both say why.
5. **A blob of world full of water from bedrock to ground level** -- ground climbing steeply to
   build height with water, iced over, filling everything under it. **Open, but narrowed a long
   way.** Four measurements, all clean: the terrain scan for an inverted density field (two seeds
   x 97 x 97 chunks, plus 96k, 960k and 6.4M blocks out) finds nothing; **the same scan driven
   through the jar's own generator agrees with ours**; population's only water is the fifty
   single-block springs a chunk, tallest column twelve; and a waterfall built in the port and in a
   **real a1.1.2 World** settles to identical profiles, source for source. Neither Extra Setting can
   move a block of terrain shape. Also pinned on the way past: still water has no random tick, so a
   generated ocean does not drain into the caves under it until something disturbs it. What is left
   is storage, the streaming worker, the mesher, or a bucket. **The seed and roughly where it was
   would settle it** -- those chunks can be regenerated and diffed against the card. `status.md` §47.

## Create World is a screen now, not two keyboards

**What changed (2026-09-13).** `+ Create New World` used to open `askWorldName` and then
`askSeed` back to back and make the world the moment the second closed. `Screen::CreateWorld`
replaced both, laid out like World Settings: **Name**, **Seed** (`Random` until one is typed),
**Gamemode**, **Difficulty**, **Format** (Packed/Folder), **Secret World** (Roll 1-in-4 / Yes /
No), **Fix Ore Generation**, **Fix Bedrock Hole**, **Create**, **Back**.

- **Nothing touches the card until Create.** Every row edits `Menu::NewWorld`; B leaves having
  made nothing, and a failed Create leaves the screen as the player filled it in. The name is
  sanitised and checked against the world list before `makeDirectories`, not left to the
  keyboard's filter having run last.
- **Format is offered rather than assumed.** Packed is still the default and still right for the
  hardware; Folder is there so a save bound for a PC does not have to be made and then converted.
- **The two generation fixes are here because this is the moment they cost nothing** -- they only
  affect chunks that have not been generated, and at Create none have. Secret World is here
  because a1.1.2 rolls SnowCovered exactly here.
- `askSeed(bool* chosen, i64* out)` no longer rolls for a blank box -- it reports `chosen=false`
  and `createWorld` rolls -- so the row can show `Random` instead of a number nobody chose. It
  also opens on whatever the row holds, as `askWorldName(current, out)` now does.
- Both clock-seeded draws (the seed and the SnowCovered flip) come off **one** `JavaRandom`: two
  built from `clockSeed()` in the same call would agree with each other.

`createWorld` takes no arguments now; it reads `newWorld_`. Write-up is `status.md` 40. The screen
is 3DS-only and **has not been seen on hardware**; what it stands on is covered by
`seed_text_test` (`a_blank_seed_means_roll_one`), `world_list_test`, `packed_storage_test` and
`streamer_generate_test`.

## Extra Settings: a per-world screen that is deliberately not a1.1.2

**What changed (2026-09-13).** World Settings has a seventh row, **Extra Settings...**, between
Delete and Back, offered **outside a game only** -- two of its rows rewrite `level.dat` and two
want the world diorama. Its bottom-screen tooltip is the screen's own warning: options that may
not be fully vanilla, applying only to this world. Six rows, in this order:

- **Set Seed** -- swkbd, pre-filled with the world's seed, writing `level.dat` `RandomSeed`
  through `AnyStorage` open/`saveLevel`/close (a read-modify-write, so unmodelled tags survive).
  Chunks on the card keep their shape; the tooltip says so.
- **Fix Ore Generation Bug** -- `3dalpha.ini` `fix_ore_generation`.
- **Secret World** -- `level.dat` `SnowCovered`, Yes/No. Genuinely retroactive for free:
  `TickWorld::snowAndIce` reads the live flag, so ice and snow spread over existing ground as the
  world ticks. It does not take them away again.
- **World Texture Pack** -- sub-list with **Default** pinned on top. Default (the empty string)
  means "follow the console"; Dev Art has a token of its own, `settings::kWorldPackDevArt`,
  because empty is already taken here. `Menu::applyWorldPack` swaps the live atlas at launch and
  sets `packOverridden_`; `packName_` and `3ds.ini` are untouched, so `ensureAtlas` rebuilds the
  console's pack on the next visit to the menu.
- **Move Panorama** -- sub-screen with the world's diorama on the bottom screen
  (`PreviewScreen::Worlds`, same world). The d-pad moves it one 128-block map tile -- the red grid
  the bottom map draws -- A or START saves, B puts it back. Each step writes
  `panorama_tile_x/z` and calls the new `MenuPreview::forgetWorld`, which quiesces the worker,
  drops the grid and its tile meshes and lets them stream in from the new corner;
  `preview::dioramaOrigin` gained a tile offset whose zero is where the table has always stood,
  and `runWorldJob` reads the offset off the world's own settings file on the worker thread.
- **Fix Bedrock Hole Bug** -- `3dalpha.ini` `fix_bedrock_hole`. Generation only; it does not fill
  in a hole already on the card.

**Both generation fixes are carried in `worldgen::GeneratorOptions`, and `WorldStreamer::open`
fills them by reading `<world>/3dalpha.ini` itself** rather than being handed them, so the menu,
the host harness and a test cannot disagree about what a world generates. **Neither changes a
random draw** -- each only widens what the already-drawn shape is written into.

The ore bug was measured, not assumed: mirroring one vein into all four quadrants over 400 seeds,
negative x costs 14.5%/3.5%/1.9% of the ore at vein sizes 8/16/32, negative z costs
14.8%/5.2%/3.1%, and both together 24.8%/12.6%/5.5%. Flooring the box makes all four quadrants
place exactly the same count. The bedrock hole is about one column in six at `y = 0`, over 4,096
columns. Derivations in [worldgen-a1.1.2.md](worldgen-a1.1.2.md); the write-up is `status.md` 39.

**Two bugs found by playing it.** *Secret World read `No` on every world*: it asked
`AnyStorage::peekLevel`, and a **packed** peek answers out of the manifest's metadata block, which
carries `lastPlayed` and `randomSeed` and nothing else -- every other field came back at its
default, and every world this port makes is packed. New `AnyStorage::readLevel` /
`PackedStorage::readLevel` inflate and decode the level blob without claiming the world; the
folder backend's peek already did the whole file, which is why nothing noticed sooner.

*A winter world thawed and refroze forever*, which is a `tick/behaviour.cpp` bug that predates
this screen. `iceTick` and `snowTick` read `skyLightAt`; the jar's `he.a`, `p.a` and `fd.a` all
read `cn.a(by.b, ...)` and **`by.b` is `EnumSkyBlock.Block`** (`by.a` is Sky, defaulting 15;
`by.b` is Block, defaulting 0 -- checked in the bytecode). Sky light is 15 on anything the sky can
see, above all three thresholds, so every exposed ice and snow block melted on its first random
tick and `snowAndIce` -- correctly reading block light -- put it straight back. Both now read
`blockLightAt`. See [tick-a1.1.2.md](tick-a1.1.2.md) *Ice and snow read block light*.

Tests: `world_settings_test`, `packed_storage_test`
(`a_packed_peek_is_the_header_only_and_read_level_is_the_whole_thing`), `tick_test`
(`ice_and_snow_under_an_open_sky_never_melt`, `both_snows_melt_above_block_light_eleven`,
`ice_melts_into_water_under_a_light_source`), `ore_test` (`ore_veins_lose_blocks_in_the_negative_quadrants`,
`ore_bounds_fix_changes_nothing_at_positive_coordinates`), `terrain_test`
(`bedrock_has_holes_in_the_floor_and_the_fix_closes_them`,
`the_bedrock_fix_changes_nothing_above_the_floor`), `diorama_test`
(`a_moved_diorama_steps_by_whole_map_tiles`), `streamer_generate_test`
(`the_generator_reads_the_worlds_own_extra_settings`). Host and 3DS builds are clean.
**The screen has not been seen on hardware.**

## World Settings measures a world on a thread, so a big world no longer freezes the menu

**What changed (2026-09-13).** `Menu::openWorldSettings` used to call `world::worldSize` inline
after drawing one "Measuring" frame. On a folder world that walk is a stat per chunk file across
up to 4,096 leaf directories, so on a big world the menu stopped for seconds and looked hung.

- `core/world/size_scan.{hpp,cpp}` (`world::SizeScan`) runs the same walk on a thread of its own
  -- `WorkerRole::Io`, so core 0 just below the main thread -- and holds the result until it is
  polled. `std::thread` on the host; if a console refuses a thread the walk runs inline, which is
  the old behaviour rather than no size at all.
- `world::worldSize` takes an optional `SizeScanContinue` callback, asked **between directory
  entries**, so a cancel lands within one stat rather than one world. A stopped walk reports
  failure; the partial total is never published.
- `Menu::measureSelectedWorld` now only starts the scan; `Menu::pollWorldSize` (from `present`)
  takes the answer and sets `consoleDirty_`. World Info shows `Size: loading...` until then.
  `setScreen` cancels on the way out of World Settings, and `beginConvert` and
  `copySelectedWorld` cancel alongside the diorama's `quiesce`.
- Tests: `tests/size_scan_test.cpp` (six cases: the total matches `worldSize`, nothing readable
  before the walk ends, cancel publishes nothing, restart replaces the subject, a missing world
  fails, the destructor joins). Clean under TSan. **Not run on hardware.**

## Main-menu bottom-screen previews: a row of skins, a pack scene, a world diorama

**What changed (2026-09-13).** On the **main menu only**, the Skins, Texture Pack and World screens
draw the bottom screen on the GPU. `platform/ctr/menu_preview` links a 320x240 render target
while one of them is up; leaving any of them deletes it and calls `consoleInit` to give the
framebuffer back to the console. `Menu::init` makes the preview and `initOverlay` does not, so
the pause menu's Skins and Texture Pack screens are unchanged. Hooks in `menu.cpp`:
`syncPreview` (from `setScreen`), `updatePreview` (from `present`), `drawPreviewScreen` (inside
the top screen's frame) and `quiesce` before delete, copy and convert.

- **Skins:** a row of players, drawn head-on with an orthographic camera (a flat picture of the
  model). The selected one is the same model at the same place; it walks into its stride over
  0.35 s and turns, so the swap is not visible.
  - The pose is `cr.a(FFFFFF)` for a player, read from the jar: `render/player_model`, where
    `bipedModel` now lives, shared with `mob_mesh`.
  - The stride amount is derived from the ground acceleration and friction in `player_body`.
  - Skins are 64x32 pages in one 256x256 sheet (32 pages, ±8 rows kept).
  - They are decoded by `texture::decodePlayerSkinPage`; Default comes from the active atlas.
- **Texture packs:** a fixed 6x5x6 scene (`preview/pack_scene`), meshed once.
  - Packs ±2 rows around the cursor are decoded by `texture::buildTerrainAtlas` (the terrain
    half of `buildAtlas`), ±3 on a New 3DS with over 24 MB of linear memory free.
  - Scrolling rebinds the texture. A pack that isn't ready yet shows the previous scene.
- **Worlds:** `preview/diorama` covers the 3x3 map tiles (128 blocks each) centred on the tile
  holding block 0, 0, for every world (moving it is a separate, planned system).
  - Cells are 4 blocks across and one block tall, so overhangs, arches and cave mouths show.
    A cell is solid where any of its 16 columns is and shows the block most of them have;
    `map::showsOnMap` decides what counts, so a torch or a rail is air here as on the map.
  - Only air the sky reaches is meshed against (`openDioramaAir`): `Sky` on every tile read,
    `Full` once when the table is read. The table's rim and the top of the world are walled
    separately, since neither has an air cell in the grid in front of it.
  - Faces carry the world's stored light (`max(sky, block)` of the cell's four corners) in the
    vertex light byte, and the preview binds the renderer's `Lightmap` at noon on unit 1.
  - The coloured stream is laid down grouped by which way its quads face
    (`kDioramaFaceOrder`, counted in `DioramaMesh::run`), and the draw submits only the groups
    `dioramaFaceVisible` keeps for the angle the table is at. The camera never looks up, so the
    undersides of overhangs go every frame and two of the four wall directions go with them --
    62% of the table's quads reach the GPU instead of all of it. The table's own two far sides
    go the same way, which matters out of proportion to their count: they are the biggest quads
    in the scene. Nothing else would drop them, since the diorama draws with `GPU_CULL_NONE`.
  - Host figures over 576 generated chunks: fold 607 ms, Sky 8 ms, Full 105 ms, nine tiles
    meshed 242 ms, 1.34 MB of vertices and a 1.83 MB grid a world.
  - **Culling what the terrain hides is not worth doing, measured.** Over a generated 3x3 table
    (seed 12345, caves): 21,434 quads, of which 5,454 tops, 15,904 walls and 76 undersides. Of
    those, 260 -- 1.2% -- are quads the camera faces at some angle but can never see past the
    terrain in front of them, traced at 32 yaws. `openDioramaAir` has already thrown away
    everything sealed inside the ground, and a cell four blocks across swallows most of what is
    left of a cave, so what survives is the table's outer skin, which an orbiting camera sees.
    Greedy 2D merging of the same cells was measured too: 20,387 quads to 16,949, 17% off the
    vertex memory, for a second axis of work in the mesher. Neither is in.
  - The table is the double slab as one block: a cube 384 blocks on an edge, its top at
    `WorldGen::kSeaLevel` (made public), drawn at 0.55 px/block about the middle of its top,
    which sits at y=100 -- so the cube runs off the bottom of the screen.
  - A missing or unread chunk shows its share of the double slab's top tile stretched over the
    table, so an unexplored tile is its ninth of that texture. Each side is the slab's side
    tile once, not tiled.
  - Reads go through `world/world_peek`: no lock, no writes. A host test shows both formats are
    byte-identical after a full read.
  - **Reads are batched (2026-09-13).** `world_peek.loadChunks` takes many chunks at once,
    groups them by region and hands each group to `format::RegionFile::readMany`, which sorts by
    sector offset and merges runs within 16 sectors into one `readAt` while they fit a 64 KB
    scratch. The worker reads the centre tile alone, then the other eight as one batch. Measured
    over a real packed world's 576 chunks: **588 card reads before, 98 after** (36 of them for the
    centre tile), 1.58 MB to 3.02 MB transferred -- at the 4 ms an operation this codebase models,
    2.35 s of card down to 0.39 s. A folder world reads one file each as before; there is nothing
    there to coalesce. See `3ds-performance.md` §7 for the grouping table and what was rejected.
  - **The batch does not hold the screen.** `readDioramaTiles` hands each tile over the moment its
    last chunk lands, so meshing and posting stay per tile. **The first console run without this
    stalled**: the centre tile appeared, the screen sat for several seconds, then the other eight
    filled in back to back. `world_peek.loadChunks` now reports every chunk asked for -- a null
    column for one that did not read, absent ones first because they cost no I/O -- which is what
    lets a tile with unexplored chunks in it count down to zero. Host: the nine tiles come back at
    9, 24, 43, 45, 58, 62, 78, 80 and 81 ms into an 81 ms read, for the same 98 reads.
  - Read-ahead stays at
    ±2 worlds (±3 when there is room): a world is 576 chunk reads, so it is the card and not the
    RAM that a scroll outruns. The room goes into keeping what has already been read instead --
    16 mesh slots, and a grid cache sized from `heapFreeBytes()` rather than a fixed 3 or 5
    (`kDioramaGridBytes` is 1.83 MB, derived from the constants and pinned by a test). A slot or
    grid outside the window is kept until something needs the space, so passing back over a world
    the cursor has already crossed costs nothing. The turn angle is never reset.
  - This is safe because `Menu::shutdown` frees the preview and joins its worker **before** the
    renderer sizes its VBO pool, and the pool is a cap rather than a reservation -- it holds
    nothing until sections mesh. Nothing the menu keeps comes out of the render distance.
- **Worker:** `preview/preview_worker`, one thread in the `Generation` role (core 2 on a New
  3DS, lowest priority on an Old one). It is joined in `Menu::shutdown` before a world opens.
- **Deviation from the plan:** the worker does not write straight into pre-allocated tile
  buffers. The main thread copies finished tile meshes into linear memory, at most 600 KB a frame.

**Verified:**
- Host build, and the full host suite (the streamer flake did not trip this run).
- New tests: `preview_window_*`, `preview_worker_*`, `player_pose_*`, `player_preview_*`,
  `pack_scene_*`, `world_peek_*`, `diorama_*`.
- For the batch reads: `a_batch_read_*` in `region_file_test` (a counting `RandomAccessFile`
  asserts the operation count, not the clock), `world_peek_batches_*`,
  `diorama_reads_a_packed_tile_as_one_batch_and_leaves_a_stopped_one_unread` and
  `diorama_hands_each_tile_over_as_it_finishes_rather_than_at_the_end_of_the_batch`.
- `make` for 3DS, and TSan on the worker, peek and diorama tests.

**First hardware report (2026-09-13): garbled bottom screen, bright blue noise over it.**
- Cause: the bottom target's transfer flags carried no `GX_TRANSFER_OUT_FORMAT`, and citro3d
  does not add one. It wrote RGBA8 into the console's RGB565 framebuffer, and past its end into
  linear memory, where the preview's own textures and buffers live.
- Evidence: `C2D_CreateScreenTarget` passes `0x1000`, which is `OUT_FORMAT(RGB8)`, and nothing in
  libcitro3d or libcitro2d calls `gfxGetScreenFormat`.
- Fixed with `OUT_FORMAT(gfxGetScreenFormat(GFX_BOTTOM))`.
- The fix is built but not yet re-checked on the console.

**Not yet confirmed on hardware.** Check on a console:
- The render target's output over the console's single-buffered framebuffer: tearing, and that
  the console text comes back on leaving.
- The depth orientation of the ortho views.
- That the characters face the camera.
- Frame time with the diorama turning.
- How long a folder world's 576 chunk reads take on an Old 3DS, and what the packed world's 98
  batched reads take beside it. The operation counts are measured; the wall clock is owed.

## Options and World Settings: grouped scrolling lists, tooltips on a dirt bottom screen

Both screens are lists of rows in groups (a few pixels between groups) that scroll to keep
the cursor in view. Under them, **the bottom screen is painted, not printed**. It shows the
pack's darkened dirt tile, the selected row's name, a short tooltip in a dark box drawn in the
pack's `default.png`, and the controls. A pack with no font (Dev Art) uses libctru's own 8x8
console font, converted by `gui::fontFromBitmap` (bit 7 is the leftmost column, checked in
`default_font_bin`). A tooltip longer than 15 lines turns into pages: the heading and the
selected top-screen button get `<` `>`. L/R turn pages on every row, Left/Right on rows with no
value, and A turns World Info forward. Every other menu screen still prints to the console,
which clears the paint when it next prints.

- **Options**: Render distance, Texture Pack, Skin | Audio, Music, Sound | Autosave | Back.
  The Sound sub-screen is gone; a silent console gets one red line (`Menu::soundProblem`).
- **World Settings**: World Info | Gamemode, Difficulty | Format, Copy..., Delete... | Back
  (in game: World Info | Gamemode, Difficulty | Back). World Info's tooltip is name, seed,
  last played (from the world list entry, so no extra level.dat read), format, size (measured on
  a thread; `loading...` until it lands), gamemode and difficulty.
- Core: `gui/settings_list` (group offsets, scroll, visible count, page count), `gui/text`
  (software pack-font text with colour codes and shadow, and the console font converted), and
  `texture::wrapText` (pixel-width word wrap, colour carried across a break). Tests:
  `settings_list_test.cpp`, `text_paint_test.cpp`, and wrap cases in `font_test.cpp`.
  Screen code: `Menu::paintSettingInfo`, `buildOptionsInfo`, `buildWorldSettingsInfo`,
  `drawSettingsRows`, `turnInfoPage`.

**Not run on hardware.** Still unconfirmed on a console: layout, tooltip fit, and whether the
paint survives the frames between repaints (the bottom screen is single-buffered, as the HUD
already relies on).

## Explosions destroy dropped items

"Explosions don't destroy items." `je`'s middle phase calls `kh.a(Lkh;I)Z` and adds the impulse on
*every* entity in its box; `dx`'s override subtracts from its 5 health, so anything near a blast is
gone. `applyBlast` only reached the player and mobs. Now `MobSurroundings::items` (set in
`ctr/main.cpp`) hands the pool to `ItemEntitySystem::takeBlast`, which hurts, pushes and sweeps.
Creeper and TNT both reach it through `around`. Test:
`a_blast_destroys_the_stacks_beside_it_and_throws_the_ones_further_out`. Boats, minecarts and
other non-living entities still ignore blasts; check each class's `a(Lkh;I)Z` before porting.

## Water and lava are generated textures now; fire's standing still is not yet explained

"Lava and fire don't animate and lava is too red." a1.1.2 registers six `TextureFX`; two (the
flames) were ported. `core/texture/fluid_fx.{hpp,cpp}` transcribes the other four (`ml`, `ht`,
`at`, `eg`) -- still/flowing water and lava, including `tileSize = 2`'s 2 x 2 flowing block and the
row scroll. `applyAnimatedTiles` bakes them at pack load; `main.cpp` keeps a `FluidAnimation`
beside the flames and pushes six runs a tick through `Atlas::updateTile(tile, texels, across)`.
The red was the pack's static tile 237 (mean G 65) against the generated ramp (mean G 129).

- **Staging race fixed** in `Atlas::writeTile`: each push takes fresh staging from a 16-tile pool,
  retired in `Renderer::drawFrame` after `C3D_FrameBegin`. Disassembly and reasoning in
  `status.md` 44.
- **Fire animation on hardware is still unexplained.** Source path, tile arithmetic and the
  simulation all check out on the host. Next console run: orange *moving* lava means tile pushes
  work; orange *still* lava means the bake works and the per-frame VRAM copy does not (look at
  `Atlas::writeTile`'s VRAM branch); red lava means `applyAnimatedTiles` is not reached.
- Tests: `tests/fluid_fx_test.cpp` (new), two run tests in `tiled_test.cpp`, and
  `texture_fx_test.cpp` / `pack_test.cpp` updated for the twelve generated tiles.

## The open inventory keeps up, the carried stack is visible, and X throws it

Three faults in the bottom screen's Items page, all in `platform/ctr`.

- **The page went stale while it was open.** Every write to the forty slots marked only the
  hotbar band dirty, so walking over a dropped item filled a backpack slot the open grid above
  did not redraw. All of them now go through `Overlay::inventoryWritten`, which marks the band
  and -- on the two pages that are a view of the inventory rather than of the world -- the body
  as well. `Overlay::selectionMoved` is the same for a change of *which* slot is in hand, which
  the Blocks page's caption and selected-cell outline both read.
- **A picked-up stack was invisible.** Its source cell was drawn hollow on the argument that it
  was "following the cursor", and nothing anywhere drew it following anything. It is now drawn
  by `hud::drawCarried`, lifted a few pixels off the cell it hovers over, with that cell's own
  count suppressed so only one number is in the corner. `Overlay::carriedPosition` decides where:
  the grid cursor, else the hotbar cursor, else -- with the focus off, where the pointing device
  is a finger that is not on the screen between presses -- the slot it came out of.
- **X throws the whole carried stack**, and is the focus toggle only when the hands are empty.
  This is `GuiContainer.mouseClicked`'s slot -999 spilling a full cursor, not `dropOneItem`;
  `Overlay::throwRequest` / `finishThrow` / `cancelThrow` split it so the entity is spawned before
  the stack is spent, exactly as `dropHeldItem` does, and a pool that refuses the spawn leaves the
  stack in hand. Handled in `main.cpp` outside the `uiFocused()` guard, since a stack is picked up
  *with* the screen focused. The hint under the grid says "A puts it down, X throws it".

Builds clean for host and 3DS. **Not run on hardware**; nothing here is a timing or memory claim.

## The sky: colours from the clock, two flat planes, the sun, the moon and 780 stars

There was no sky at all -- one daylit blue as the clear colour and a second, unrelated one as the
fog constant, neither a function of the time of day. Both are now computed per frame from `cn`'s own
three base colours and a1.1.2's own daylight fraction, and the geometry renderSky draws is built.

- **`core/world/daylight`** gained `skyColour` (`cn.b`), `fogColour` (`cn.e`), `starBrightness`
  (`cn.f`) and `viewFogColour` (`iq.h`, the fog lerped towards the sky by the render distance).
  a1.1.2 has **no biome tint in the sky** -- `getSkyColor` reads one world field, `0x88BBFF`, set
  in the constructor and never written again. The sky reaches pure black; the fog has a floor.
- **`core/render/sky`** is the geometry, **built once at start-up** into 70 KB and never rebuilt:
  two 13 x 13 grids of 64-block cells at +-16 blocks (the fog fade across the upper one *is* the
  horizon -- there is no dome in this version), the sun and moon quads at +-100, and 780 star
  billboards from `new Random(10842L)`. Everything that changes with the day is a uniform, a
  combiner constant or a matrix.
- **`Renderer::drawSky`** runs first in each eye: the detail pipeline with the chat's scale trick
  (the sky is written at 1/64 of a block, because 448 blocks does not fit 1/1024), two matrices --
  one flat, one turned about X by the celestial angle -- and a far plane of its own at 512, because
  the world's would clip the sun out of the sky below six chunks.
- **`terrain/sun.png` and `terrain/moon.png` are two new pages of the entity sheet**, 32 x 32 at
  (128, 32) and (192, 32); `kEntitySkinCount` is 19.
- **The clear colour and the fog constant must stay the same colour.** That is what hides the far
  plane cutting through the sky plane, in this port as in the original.
- Derivation is `tools/genref.java --jar <client.jar> --sky <scratch>`, which asks a **real World**
  for all three colours at fourteen times of day; `tests/sky_vectors.hpp` is checked in and
  `tests/sky_test.cpp` holds the transcription to it. `status.md` 43 is the write-up.
- **The first hardware run found one bug and it was citro3d bookkeeping, not geometry.**
  `C3D_GetBufInfo()` is what marks the buffer config dirty; writing through the returned pointer
  afterwards does not. `drawSky` fetched it once and re-based it per range, so only the sky plane's
  base reached the GPU and the other four ranges drew the sky plane's vertices -- no sun, a pale
  stripe where the sun and moon should be, the void colour painted over the plane *above* the
  camera, nothing below. Fixed with drawChat's pattern: a local `C3D_BufInfo` and `C3D_SetBufInfo`
  per draw. **Any pass that changes the vertex base between draws needs this**; the one-draw passes
  that fetch and mutate the shared config are fine. `status.md` 43 has the diagnosis.
- **Clouds are not ported** and were not asked for. **The rest is still unmeasured on hardware**:
  the fill cost of two full-screen planes and 1,560 star quads a frame is the number to take.

## Walking on a block: farmland is trampled, and redstone ore lights up

`moveEntity`'s footstep block ends with `Block.onEntityWalking(world, i, j, k, this)`, which this
port had named and skipped. **Two classes override it in a1.1.2** -- the earlier note that it was
only redstone ore was short by one: `mi` (farmland) is
`if (world.rand.nextInt(4) == 0) setBlockWithNotify(i, j, k, dirt)`, with **no fall-distance test,
no crop test and no entity test**; the jump-ruins-a-field version is Beta's. `km` (the staircase)
forwards to the block it is modelled on and so does nothing.

It is one call per footstep, which is one per whole block of ground covered, so running and
jumping trample no faster than walking and a jump on the spot does not trample at all.

- **`tick::entityWalkedOnBlock`** (`core/tick/behaviour`), beside `entityCollidedWithBlocks` and
  called the same way: `PlayerBody::move` takes a `const TickWorld&` on purpose, so it records the
  cell (`steppedOn`, `stepBlock{X,Y,Z}` -- underfoot, and not the snow/liquid substitution that
  decides `stepSoundDue`) and the owner of the tick runs the callback right after the move and
  before the collision scan, which is the jar's order. Dispatch is on the tick behaviour, as
  `entityCollidedWithBlock`'s is, so no new block column. Redstone ore reuses
  `redstoneOreActivated` and its "only the unlit one" test.
- **Both callers**: the player in `src/platform/ctr/main.cpp` and every mob in `MobPool::tick` --
  `moveEntity` is `Entity`'s, so a cow ruins a field exactly as a player does.
- **The crop dies with the ground in the same call** -- `setBlockWithNotify` notifies the cell
  above and checkFlowerChange drops the wheat -- but nothing redrew it. `TickWorld`'s change
  callback only invalidates sections while the streamer holds a renderer, and only `stepTicks` and
  the four player-edit entry points ever held one; the entity pools run out of the frame loop, so
  a trampled furrow kept drawing its crop. **`WorldStreamer::RenderBracket`** is that bracket,
  scoped and nest-safe (the five existing holders are routed through it), and `main.cpp` holds one
  around the body loop and one around the entity-pool block. A dropped stack pressing a plate and
  a falling block landing were invisible for the same reason and are fixed with it.
- **Named deviation**: the walk counters are `PlayerBody`'s, so only the player and the mobs earn
  footsteps here. In the jar they are `Entity`'s, and an item or a cart sliding over a field
  tramples it as well.
- `tests/trample_test.cpp`, and section 42 of `status.md` carries the bytecode.

## You can stand on a minecart, and a minecart stops at what is standing on it

`World.getCollidingBoundingBoxes` (`cn.a(Lkh;Lcf;)Ljava/util/List;`) is a block loop and then two
questions per nearby entity, and this port had only the block loop. **Every** `moveEntity` in the
game clips against the list all three build.

    AABB b = e.getBoundingBox();          // kh.f_()   -- the neighbour's
    if (b != null && b.intersectsWith(box)) list.add(b);
    AABB c = entity.getCollisionBox(e);   // kh.b_(kh) -- the MOVER's
    if (c != null && c.intersectsWith(box)) list.add(c);

All 402 classes in the jar were disassembled for both overrides. `f_()`: exactly `dc` (EntityBoat)
and `oc` (EntityMinecart) return their own `boundingBox`, and `kh`'s own returns null, which every
mob, item, arrow, painting and particle inherits -- so a cow is walked through and a cart is not,
and a parked cart was scenery here. `b_(kh)`: the same two classes, each `return e.boundingBox` with
no liveness or `canBeCollidedWith` test in front of it, asked of the thing **doing the moving** --
so a rolling cart or boat is stopped by the cow on the track, the item lying on it and the player
waiting at the station, while nothing else in the game collides with an entity at all.

- **`TickWorld::forEachSolidBox` / `anySolidBoxIn`** (`core/tick/tick_world`). A fold rather than a
  list, for the reason `anyEntityIn` is a predicate: the sweep wants one number per axis, the pools
  belong to the frame loop, and nothing allocates or caps to answer. The sink is handed every solid
  box intersecting the query box -- the original's final `intersectsWith` gate, which is what makes
  its 0.25 candidate expansion invisible from here. Unset means nothing is solid, which is what the
  headless tools get.
- **`entity::EntityBoxes` / `bindEntityBoxes`** (`core/entity/entity_boxes`). a1.1.2's world entity
  list, bound to the world the way `bindParticles` binds the particles: the two solid pools first,
  then the mobs, items, arrows, paintings, TNT, falling blocks and the player, which only a boat or
  a cart ever asks for. The particle pool is deliberately absent -- `bq.a(nq)` puts an `EntityFX`
  in the effect renderer's own `List[]` and never in the world's, so nothing collides with smoke.
  `main.cpp` holds the struct beside the pools; the tests bind their own.
- **`clipAxis` takes a `Mover`** (`core/entity/sweep`). `BlockRange` now carries the double box it
  was built from, because the entity half is asked by box and not by block, and `clipAxis` folds
  the entity boxes in after the block loop with the same `calculateOffset`. `Mover` is
  `getCollidingBoundingBoxes`'s first argument, as much of it as the sweep needs, and both halves
  are facts about the mover rather than about what it might hit: `self` is the identity left out of
  its own list -- a cart handed its own box finds it in the way of every move it tries and never
  goes anywhere -- and `collidesWithEntities` is `kh.b_`, false everywhere but the boat and the
  cart. Every other caller takes the default and is unchanged.
- **The sneak walk-back and `isOffsetPositionInLiquid`** (`core/entity/player_body`) are
  `getCollidingBoundingBoxes(...).isEmpty()` in the original, so both consult the solid boxes too:
  sneaking stops at the edge of a cart parked below a ledge.
- **Getting off puts you on the roof.** `kh.g(Lkh;)V` -- mountEntity, called with the vehicle you
  are on, which is how a1.1.2 dismounts -- ends in
  `setLocationAndAngles(posX, boundingBox.minY + height, posZ, ...)`, and `setLocationAndAngles`
  puts `posY` at `y + yOffset`: the rider's feet land on the vehicle's roof, not in its seat. The
  three `dismount()` methods now return that point as a `RiderSeat` and the frame loop puts the
  body there. For a pig nothing holds it, which is what getting off a pig looks like.

Everything that moves through the shared sweep gets this, as it does in the jar: a dropped item
lands on a cart, a cart is stopped by the cart in front of it, a body stands on a boat.

**A vehicle's own rider is in its list and costs it nothing**, which is worth writing down because
it looks like a bug waiting to happen. `getEntitiesWithinAABBExcludingEntity` leaves out the mover
and nothing else, and `ga.a(kh,cf,List)` filters on `e != excluded` and an intersection test alone.
But `oc.h()` and `dc.h()` -- getMountedYOffset -- are both `height * 0.0 - 0.3`, so a rider's feet
sit 0.3 *below* the vehicle's centre and its box overlaps the vehicle's on all three axes, and
`calculateOffset` clips only against a box that is ahead and clear. A test holds that.

Covered by `entity_boxes_test` -- a body dropped on a cart lands on its roof, the same drop with
the query unbound falls through it (the bug, kept as the contrast), a cart is a wall to walk into
rather than a kerb to step over, a cart still rolls with the query bound, a cart stops behind
another cart, dismounting leaves the player standing on the roof, a boat holds a body up too, a pig
and a dropped item each stop a rolling cart, a pig stops a moving boat, the same pig does **not**
stop a walking player, and a ridden cart still rolls. No hardware run; the 3DS build passes.

**Two streamer cases are intermittently flaky, and it is not this change.** The full suite passed
in one process after this work -- 1407/1407 -- but before it, three runs in a row each stopped at
one of two assertions:
`streamer_map_work_test`'s `an_offer_made_while_the_world_is_still_being_generated_never_holds_generation_up`
(`offers > 0`) and `streamer_revisit_test`'s `a_block_change_moves_the_columns_serial_...`
(`blockChangeSerial` moving in a frame that writes nothing). A different one of the two in each
run, one of them on an otherwise idle machine, and each passes on its own; the runner terminates
on the first failure, so nothing after it ran. Both assertions are timed or counted against a
microsecond budget. It reproduced with this change's test file removed, i.e. with no entity-box
query bound anywhere in the binary, which makes every line added here a null check. Treat a failure
in either as a machine-load artefact until someone has made it fail deterministically.

## The bottom map follows the world, and the game never waits for it

The map sampled a chunk once, ever. Everything built, broken, decayed, burnt, flowed or dropped
after that was invisible on it until the world was reopened. The fix is a change signal, a queue
and a clock, and none of the three is allowed to cost the frame.

- **`WorldStreamer::blockChangeSerial` / `columnBlockSerial`** (`core/render/world_streamer`).
  One session-wide counter; `Cell::mapSerial` takes a fresh value from it in `tickBlockChanged`
  and again in `adoptColumn`. A serial rather than a dirty flag because the map holds samples of
  ground the streamer has already handed back to the card, so no single reader can own and clear
  a flag; and the adoption stamp is what stops a column that left the grid and came back from
  comparing equal to a sample taken before it left. `columnBlockSerial` is 0 for a column that is
  not resident, which no live column's serial is.
- **`ChunkQueue`** (`core/util/chunk_queue`). A fixed-capacity FIFO of chunk coordinates that
  holds each coordinate once: a ring whose slots never move, plus a linear-probe membership table
  that tombstones on pop and rehashes when the dead fill it. Allocates in `setCapacity` and never
  again; a push onto a full one is refused and sets `overflowed()`, which is the consumer's cue to
  recover once rather than once per lost coordinate.
- **`WorldStreamer::takeChangedColumn`** — the columns the world wrote into, deduped at the
  `tickBlockChanged` choke point. This is what replaced "walk the whole window asking every chunk
  in it whenever the world serial moves": a lake draining for a minute used to open that gate on
  every frame of the minute to find the same two chunks.
- **`WorldStreamer::offerColumnWork` / `takeColumnWork`** — read-only work over resident columns,
  run on the generation worker (core 2 on a New 3DS) when it has nothing to generate. Generation
  keeps priority in `workerMain` itself: the slate is looked at first, and a batch is cut short
  the moment a column comes onto it. Safety is the lifetime rule, not a lock — the borrow lives
  from the offer to the next `update()`, which withdraws it (cancelling if the worker never
  started, waiting out one chunk if it did) before touching a cell, and no block is written in
  that interval.
- **`MapStore::store` takes the serial and returns whether the picture moved**
  (`core/map/map_store`). An identical re-sample keeps the patch, the south neighbour's patch and
  `stats().stored` — which is what `MapScreen`'s redraw signature hangs off — so a player mining
  under a roof does not re-shade the window every frame. `sampleSerial` reads the stored token back.
- **`MapScreen::update` queues and then spends a slice of the frame**
  (`platform/ctr/map_screen`). It collects what the worker did, drains the streamer's change list,
  walks the window only when the window moved (to find ground it has never had), offers a batch to
  core 2, and samples on its own thread until its microsecond allowance is gone — 400 us on an old
  3DS, 800 on a New one, 3.2 ms while the map is still catching up at world entry. The clock is
  read after each chunk, so a frame always does at least one and the queue can fall behind but
  cannot stall. A stale chunk whose column is no longer resident keeps the last sample anyone
  could take; the map never asks the card for anything.

Covered by `chunk_queue_test` (dedupe, FIFO order, overflow, and the drain-and-refill churn that
finds a membership table filling with tombstones), `streamer_map_work_test` (two hundred blocks in
one chunk put it on the change list once; the offered batch runs on the worker with the right
indices and columns; a non-resident coordinate is dropped and the rest compacted; offers made
while the frontier is filling never stop the world settling), and the existing
`a_resample_that_changes_nothing_costs_nothing_but_still_takes_the_serial` (map_test) and
`a_block_change_moves_the_columns_serial_and_a_reload_does_not_reuse_one` (streamer_revisit_test).
Host suite and the 3DS build both pass; **no hardware timing was taken** — see status.md 40 for the
figure to take.

## `TileEntities` is modelled, so signs, spawners and dungeon loot all persist

`core/world/tile_entity.{hpp,cpp}` is `ic` and its four subclasses; the codec is in
`src/impl/storage/alpha_chunkfiles/chunk_nbt.cpp`. The list used to be an opaque preserved tag.
See status.md 39 for the derivation.

- **All four or none.** Writing `TileEntities` without modelling the chest would empty every
  dungeon chest in the world, so the spawner's mob could not be persisted alone. `ic`'s static
  initialiser has exactly four entries and they are all modelled; a fifth id is `Unknown` and is
  round-tripped whole.
- **`id` is not guaranteed first** -- `hm` is a `HashMap` and writes in bucket order -- so each
  element is read twice over the same bytes, once for the id and once against it.
- **`reconcileTileEntities` is `ga.d`'s lazy heal, done in bulk at the save**: a container block
  with no entry gains a default one, a known entry whose block is gone is dropped, and an unknown
  entry is never touched.
- **Sign text survives a reload.** `readSigns`/`writeSigns` on the column, plus
  `WorldStreamer::markColumnModified` -- `ga.f()`, which `nv` calls as the keyboard closes,
  because text is not a block and nothing else marks the column.
- **A dungeon this build generates keeps its loot.** `PopulationSideEffects` was passed as null
  on the generation worker; the records go onto the generator's `Entry` now, because a pass
  writes into a 2x2 quadrant and a dungeon rolled for one chunk lands in the next.
- **`setColumnSinks` gained a `saving` callback**, and `dropCell` now fires it *before*
  `dropped` -- the old order would have erased the entries the save was about to read.
- **Measured**: `--rewrite` over a copy of the real World1 read and wrote 1,119 chunks, 66 tile
  entities (39 Chest, 27 MobSpawner), 195 item stacks, and `tools/nbtdiff.py difftree` reports
  1,119/1,120 semantically identical -- the one difference being `level.dat`'s `LastPlayed`.
  `--rewrite <copy> reconcile` runs the heal-and-drop over the same world and reports **0**
  entries added or dropped, diff still clean. A world **this build generated** (510 columns)
  carries 17 Chests with 89 stacks and 11 MobSpawners reading 5 Zombie / 4 Skeleton / 2 Spider --
  `cg.b`'s weighting, and not one `"Pig"`.

`tests/tile_entity_test.cpp` is new (16 cases, NBT written by hand rather than by our own
encoder). `--rewrite <world-dir>` is new in the host harness.

## The mob spawner works, and the mob was in the world file all along

`bd`, `bj` and `r` -- `core/entity/mob_spawner.{hpp,cpp}` and
`core/render/spawner_mesh.{hpp,cpp}`. **Not `az`/`k`**, which is the world's own spawner in
`core/entity/mob_spawn.hpp`; this is the block at the centre of every dungeon, which the
generator has been placing for months and which did nothing. See status.md 38 for the derivation.

- **The first tile entity that ticks.** `ic.b()` is empty and only `ke` (furnace, not ported) and
  `bd` override it, so `tickMobSpawners` is the whole of a1.1.2's tile-entity tick list. Every
  draw is `World.rand` -- `TickWorld::random()` -- not a generator of the tile entity's own.
- **A spawner nobody is within sixteen blocks of does nothing at all**: `anyPlayerInRange` returns
  before the particles, the spin and the countdown alike, so walking away freezes it.
- **The spin rate is the tell.** `c += 1000F / (delay + 200F)` in float, times ten in the
  renderer: 12.5 degrees a tick on a fresh delay, fifty on the tick before it fires.
- **`readMobSpawners` claims the mob out of the column's `TileEntities`** as it joins the
  resident grid, through `WorldStreamer::setColumnSinks`. Measured on a copy of the real World1:
  13 spawners in the loaded neighbourhood, 5 Zombie / 6 Skeleton / 2 Spider; standing on a real
  skeleton dungeon (`--spawns <copy> 2000 at=219.5,65,218.5`) it fired 5 times, placed 7
  skeletons and was then stopped three times by the six-of-a-kind check.
- **It is written back now**, which it was not when this section was first written:
  `writeMobSpawners` is the mirror, and the list it writes into is modelled rather than
  preserved. See the section above.
- **`ge.z()` was wrong and nothing had pinned it.** `spawnExplosionParticle` draws its three
  Gaussians first and subtracts each, times ten, from the puff's position. Now
  `MobSystem::explosionPuff`, shared by the death puff and the spawner.
- The miniature costs **no extra draw call**: it appends to the mob pass's buffer and shares its
  bind. `placeSpawnerMob` composes onto `placeMob` rather than restating `RenderLiving`'s
  flip-and-lift.

`tests/mob_spawner_test.cpp` is new (26 cases). `--spawns` grew a `spawner blocks` section, the
nearest three cages, and an `at=x,y,z` argument; the Info page grew a `cage` row.

## A sword hits for what it is worth, and the hoe works

Two generated columns, one engine change each, and no new machinery. Both facts came out of
`tools/genref.java --items` against the client jar; `data/a1.1.2/items.json` was regenerated and
nothing else in it moved.

- **`damageVsEntity`** -- `di.a(Lkh;)I`, called with null on every constructed item because no
  override in this version reads the entity. 1 for everything but `bs` (ItemTool, `material +
  kind`) and `iu` (ItemSword, `4 + material * 2`): wood 4, stone 6, iron 8, diamond 10, and
  **gold beside wood at 4**. `item::attackEntity` now takes the held item and passes that number;
  the unknown row's 1 is `InventoryPlayer`'s empty-slot answer, so the bare hand needs no branch.
  Durability is still not spent.
- **`tills`** -- `instanceof fu` (ItemHoe). It is a column for the same reason `spawns` is one: a
  hoe writes farmland into the cell that was *struck*, so `places` measures 0 for all five and
  cannot tell a hoe from an item that does nothing. `use.cpp`'s `useHoe` is the method, routed
  from `rightClick` on that column.

The hoe's four non-obvious parts, all of them visible: the face parameter is never read, so it
tills from the side and from below; the `isSolid` cover test guards grass and not dirt, so dirt
under stone still tills; the `nextInt(8)` seed roll is drawn on dirt too and spends the world's
random either way; and the sound is `ItemBlock.onItemUse`'s row of the block-sound table exactly,
so it goes through `audio::placeCue`. The seed rises by a constant `1.2F` rather than a third
draw. See status.md §37.

Eight cases added to `tests/use_test.cpp`. Host suite 1338/1338; the 3DSX build passed. No
hardware run.

## Arrows are not eaten by the archer any more, and a flying player can shoot a painting down

Reported as "an arrow should hit paintings and cause them to pop off". The painting was never the
problem -- `ArrowSystem::tick` has swept the painting pool since it existed and the wiring in
`main.cpp` was never absent. What the arrow did not survive was its own first tick.

`kg.e_()`'s `entity != shootingEntity || ticksInAir >= 5` was ported as a **by-place** proxy: any
candidate still standing over `(shooterX, shooterZ)` is skipped while the five-tick grace lasts.
That holds only while the shooter stays inside its own box. `entity::kFlightSpeed` is 0.6 blocks a
tick against a player half-width of 0.3, so **one tick of Creative flight carries the player clear
of the recorded place**; the exclusion then missed, and the arrow -- still inside the box it was
born in, muzzle 0.16 back and targets grown by 0.3 -- struck its own archer at a distance of zero
and was spent. Every shot fired while flying died on the tick it was loosed.

The exclusion now splits on who fired, which is what the jar's reference comparison actually says:

- a **player's** arrow skips the player *by identity* -- `ArrowTargets::playerPresent` is a stable
  handle, so no proxy is needed -- and skips nothing else, so a cart, a boat or a mob the player is
  standing inside is a target from the first tick;
- a **skeleton's** arrow keeps the footprint proxy for mobs, which still has to be one (the mob pool
  swap-removes), and owes the player no grace at all.

`tests/arrow_test.cpp`: the shot fired while flying forward that must not land on the archer, and
the whole chain end to end -- a painting on a wall, `item::useItem` with the bow, `runGame`'s own
tick order, the painting on the floor as an item afterwards. Both fail if the split is reverted.
`a_players_arrow_still_hits_someone_standing_elsewhere_at_once` was a test of the proxy rather than
of the game -- a player's arrow is always fired from the player -- and is replaced by
`a_skeletons_arrow_hits_the_player_at_once_and_owes_no_grace`.

## The stone button is a button in the hand and in the slot, not a stone block

Reported as "the stone button has the wrong texture in the inventory and hand (normal stone block)".
It was the shape, not the texture: a button is drawn in the stone tile, which is `new hu(77, 1)`'s
own second argument.

Three shapes, not two. The item paths were drawing `selectionBox(id, 0)` -- the block's *world*
bounds -- and `hu.a(Lnm;III)V` writes a button's bounds only for metadata 1..4, so metadata 0 is the
constructor's full cube. a1.1.2 does not draw that: `RenderBlocks.renderBlockAsItem` calls
`Block.setBlockBoundsForItemRender` first. It is empty on `ly` and overridden by exactly two classes
in this version -- `hu`, the button, and `al`, both pressure plates -- which are precisely the blocks
whose world bounds are leftovers at metadata 0.

- `tools/genref.java --collision` now sweeps it as well, so the numbers are measured: `kItemRenderBoxes`
  in `tests/collision_box_vectors.hpp`, 256 rows and `kItemRenderOverrides == 3`. The rest of that
  fixture regenerated byte for byte.
- `gen_selection.py` folds the column into the existing shape list (45 shapes -> 47) and
  `configure.py` emits `kItemRenderIndex`; `block::itemRenderBox` reads it and
  `block::itemRenderBoxes` is what the icon, the hand and a dropped stack now ask instead of
  `renderBoxes`. The world mesher is untouched -- it never went through `renderBoxes` for a cube.
- A button in a slot is 6x4x4 texels centred in the cell; a plate is full width and 4 texels thick
  in the middle of the cell rather than the wafer it is on the floor.

**Placement was checked and is not broken.** `hu.a(Lcn;III)Z` is four `isBlockNormalCube` calls on
the horizontal neighbours and a1.1.2's `ItemBlock.onItemUse` calls `World.canBlockBePlacedAt` with
**no side argument**, so a button goes on any of the four sides of a wall and nowhere else -- which
is what `tick::canPlaceAt` already does, end to end through `item::rightClick`. Clicking a top or
bottom face does nothing, as in the jar. One difference remains and is deliberate: a button that
lands at metadata 0 (placed on a top face next to a wall, which a1.1.2 allows) is drawn as a full
cube here, where the jar draws whatever bounds the singleton was last left holding.

## An arrow no longer shoots the player who fired it

Reported from play. `kg.e_()`'s `entity != shootingEntity || ticksInAir >= 5` was ported for the
skeleton and never for the player, in two places at once:

- `ArrowSystem::shoot` left `shooterX/shooterZ/shooterGrace` at their defaults, so a player's arrow
  carried no self-grace at all. It now records the player's `posX/posZ` (the eye argument moves only
  y) and the five-tick grace, exactly as `shootFrom` does for a skeleton.
- The by-place exclusion was written inside the mob loop, so `targets.playerPresent` was considered
  unconditionally. It became an `isShooter` lambda inside `consider`, asked of every candidate --
  **and by place, which the section above has since replaced for the player**: the footprint proxy
  does not survive Creative flight.

The arrow leaves the muzzle 0.16 back along the heading against a half-width of 0.3 and the sweep
grows a target by another 0.3, so the first tick's segment began inside the shooter and struck them
for 4 -- reproduced by the new test before the fix.

Nothing tested `ArrowTargets::playerPresent` at all, which is why it shipped. The cases live in
`tests/arrow_test.cpp`: the shot that must not land on the archer and one fired straight up that
comes back down and does hit them once the grace is spent. Reverting either half of the fix fails
the first, and reverting the lambda also fails
`a_skeletons_arrow_does_not_shoot_the_skeleton_that_fired_it`.

## Dropped items do not stack on their own any more, and Beta 1.8 was the wrong date

Ground merging was added on request in status.md 10 and is now removed: two stacks lying in the same
block stay two stacks, which is a1.1.2's own behaviour. `combine()`, `kItemMergeInterval`,
`kItemMergeReach` and the scan in `ItemEntitySystem::tick` are gone; the end-of-tick sweep stays,
because a death still empties a stack rather than removing it while the walk holds a reference.

- **The version that change was dated to was wrong.** It said Beta 1.8. Bisected over the jars here
  plus 1.2.5/1.3.1/1.3.2/1.4.7: `EntityItem.combineItems` is absent in 1.2.5 and present in
  **1.3.1** (a release bound -- the 2012 snapshots are not in the manifest used), and the
  `age % 25` throttle that was copied with it is later still (present by 1.8.9). b1.8.1's `onUpdate`
  has no merge step at all, and no `EntityItem` up to 1.2.5 iterates a list anywhere in the class.
  The table and the method names are in `status.md` 35.
- Five merge cases in `tests/item_entity_test.cpp` are replaced by three pinning the absence: two
  like stacks after 120 ticks, eight stacks still eight, and an old stack whose despawn clock a
  fresh one no longer resets.
- The item tick is linear again; the `status.md` 18 note about an O(n²) merge scan is struck.

## A cactus hurts what touches it, and an item cactus is the cell and not the inset box

Two play-session reports about the cactus, and they turn out to be the two halves of the same
block being read from the wrong table.

- **`ly.b(Lcn;IIILkh;)V` -- onEntityCollidedWithBlock -- is ported**, in `core/entity/
  block_contact.hpp`: the loop at the head of `moveEntity`'s tail, over every cell the box
  overlaps, floors of the bounds and **inclusive**, with none of the thousandth-of-a-block inset
  later versions add. Two classes override it in a1.1.2 and the new `Contact` column in the block
  table carries both. `hy`, the cactus, is `attackEntityFrom(null, 1)` per cell per tick; `al`, the
  pressure plate, is listed and dispatched by nothing, because the plate is armed from its own
  sense pass in `core/tick/redstone.cpp` and doing it twice would press it twice.
- **What a touch costs is the entity's own `attackEntityFrom`**, exactly as fire's is: a dropped
  stack has five health and no invulnerability window, so a cactus destroys it in a quarter of a
  second where before it lay on the spikes for the full five minutes; an animal's ten-tick window
  makes it about a heart a second; a boat or a cart takes ten of its forty points. Wired into the
  item, mob, boat and cart pools. An arrow, a falling block and a block of TNT take nothing --
  `kh.a(Lkh;I)Z` is `return false` for all three -- and the player has no health to lose yet.
- **A stack resting on a cactus is inside the cactus's own cell**: the collision top is 0.9375, so
  the box bottom floors to the cactus's y. So is one pushed against its side, because the box is
  inset a sixteenth on x and z -- which is why walking into a cactus hurts at all.
- **The item cactus is `bc.a(Lly;)V`'s shape, not the collision box.** Render type 13 resets the
  bounds to the whole cell, draws the caps there, and draws each side as a full-cell face under
  `addTranslation(0.0625)` -- the same construction `bc.b(ly,IIIFFF)` uses in the world. Handing
  back the inset box instead mapped the tile's transparent outer columns, where the spikes live,
  onto a narrowed face: **that was the ring of daylight round a dropped cactus.**
- `block::renderBoxes` therefore answers with a **face mask per box** (`faceMask`, defaulted to
  every face), and the cactus is the one shape that uses it: three boxes of one cell, two faces
  each. The dropped item, the held item, the slot icon and `mesh::addCactus` all read it, so the
  four now share one definition of the shape rather than three copies and a special case.
- **The icon sorts faces, not boxes.** Three boxes in one cell have no useful order between them;
  the painter's key is now the face centre's `x + y - z`, which is the projection's own view
  direction. Sorting boxes put a sixteenth-wide strip of the side tile over the front of the top.
- Suite 1324/1324, 3DSX built, no hardware run.

## Fire burns entities now: `kh.c(DDD)V`'s tail, and a finding that was wrong

"Fire doesn't burn entities like dropped items or animals" -- and the port had that written down as
a derived fact, with a test pinning it. The evidence was true and the conclusion was not: `og` has
no entity callback and no class outside `kh` writes `kh.aT`, but **`kh` writes its own counter in
`moveEntity`, four times**, from a box test against the world. `docs/status.md` 33 is the write-up.

- **New: `core/entity/fire_entry.hpp`** -- `isBoundingBoxBurning` (fire or lava, `floor(max + 1)`
  on each axis, no inset) and the tail's counter arithmetic, shared by every pool.
- **The counter is a fuse.** It rests at `-fireResistance` -- 1 for everything, 20 for the player --
  and catching fire is the tick it reaches *zero*, which becomes 300. `fire <= 0` is "not burning";
  `fire == 0` is not, and two tests had to be corrected for that.
- **A counter at exactly zero never catches**, because `fire++` runs before the test: an entity
  dropped straight into a flame burns at 1 rather than 300. It still takes the damage. Pinned.
- **`dealFireDamage(1)` every tick, and what it costs is the entity's own `attackEntityFrom`**: a
  mob loses about a heart a second, **a dropped stack is gone in five ticks**, a boat or a cart
  breaks after four seconds, and an arrow, a falling block and a block of TNT take nothing.
- **What burns is what moves.** `kg` and `jc` -- the arrow and the painting -- never call
  `moveEntity` and never catch fire. Derived from the jar, not assumed.
- **The renderer follows**: `buildEntityFires` walks all six pools into one buffer and one draw.
- **Not done: the player**, which has the fuse and no health to spend it on until Survival.
- New `tests/fire_entry_test.cpp` (six cases) and three item cases; three existing tests corrected.

## A burning mob now shows it: `ak.a(Lkh;DDDF)V`

The tick half has been right since the mobs landed -- a zombie caught fire at dawn, took a heart
every twenty ticks, sizzled when it hit water -- and the renderer drew nothing at all, so the only
evidence of a burning mob was that it died. `doRenderShadowAndFire`'s fire half had never been
ported; its shadow half still has not been.

- **New: `core/render/entity_fire_mesh.{hpp,cpp}`**, transcribed from the private
  `ak.a(Lkh;DDDF)V`: `ceil(height / width)` camera-facing sheets, each 1.4 units tall and stepped
  one unit up, in units of `width * 1.4`. Full bright and white, because the original disables
  lighting for them.
- **One tile, not two.** a1.1.2 reads `Block.fire.blockIndexInTexture` above the loop; later
  versions alternate two tiles down the stack. Block fire uses both (`core/mesh/shapes.cpp`) and
  entity fire uses the first, asked of the block that *renders as fire* through
  `texture::flameTile` -- which is the tile `FlameAnimation` already rewrites 20 times a second,
  so the sheets animate for nothing.
- **The stack leans toward the camera**, `-0.4` plus `-0.04` a sheet along the local +z that
  `glRotatef(-playerViewY, 0, 1, 0)` points down the view direction. Fire drawn inside a mob would
  be invisible for a second reason.
- **`(int)(1.8F / 0.6F)` is 2.** The depth term truncates the ratio where the loop takes its
  ceiling, and the float quotient is 2.99999976, so a zombie gets three sheets placed as though it
  had two. Java divides the same two floats. Pinned by test so it is never "fixed".
- **A pass of its own**, after `drawMobs`: the sheets come off the block atlas and the mob off the
  entity sheet, so they could never have been one draw. `drawMobs` rebinds the block atlas on its
  way out, so the pass costs no bind -- 12 KB of vertex buffer and one draw call.
- **Mobs only**, because `Mob::fire` is the only fire counter in the tree; a player body has none
  until Survival does.
- `tests/entity_fire_mesh_test.cpp`, six cases. Host suite 1309/1309; 3DS build links. Not seen
  on hardware.

## TNT is complete: `jd`, and all four ways a1.1.2 lights it

`docs/status.md` §31 is the write-up. The block, the drop table, the flammability, the explosion
and both sound keys were already here; **the entity was not**, and every ignition path in the tree
ended in a comment saying so.

- **New: `core/entity/primed_tnt.{hpp,cpp}`** -- `jd`, EntityTNTPrimed. `ff`'s physics with one
  difference (the landing factors apply on every grounded tick, not once), one smoke particle a
  tick, and `createExplosion` at strength 4 when the fuse runs out.
- **Four ignition paths, all four ported.** A break and a fire go through `q.b` --
  `tick::tntDestroyedByPlayer` -- at fuse 80 with `random.fuse`; a blast goes through `q.c` --
  `tick::tntDestroyedByExplosion` -- at `nextInt(20) + 10` and **silently**; a neighbour going live
  goes through `q.a` -- `tick::tntNeighbourChanged` in `core/tick/redstone.cpp`, gated on
  `canProvidePower` exactly as a door is. The fourth was the one waiting on a subsystem and the
  subsystem was already finished.
- **Breaking TNT lights it and drops nothing**, which is a1.1.2's and not a bug here:
  `quantityDropped` is a hard 0 and the override has no metadata guard in this version. Crafting is
  the only way to obtain a block of it.
- **`if (fuse-- <= 0)` reads the old value**, so 80 is **81 ticks** -- eighty that smoke and one
  that explodes.
- **A blast re-primes rather than detonating**, which is what makes a chain ripple. `je`'s phase
  three calls the hook after the cell is already air, so nothing is ever standing inside the block
  it came from; the fuse draw is on the world's generator and is made with or without a pool.
- **An entity spawned mid-tick is ticked that tick** -- a1.1.2 walks `loadedEntityList` by index
  and appends to it -- so a chain's second blast lands one tick sooner than a naive count suggests.
  Pinned by test rather than left to be rediscovered.
- **New: `core/render/primed_tnt_mesh.{hpp,cpp}`**, and the white flash is a **second draw through
  the outline pipeline**, not a vertex colour: the detail pipeline modulates, and modulation cannot
  make a textured cube white. One draw call per flashing entity, capped at sixteen. The swell is a
  fourth power and both passes read the same `primedTntScale`.
- **Fixed after a report that lit TNT tinted the whole ocean white.** The flash pass restored three
  pieces of GPU state by hand and left the world's three-stage texture combiner where the outline
  pipeline had put it -- one stage, primary colour, REPLACE. It is **not** the last pass in the
  eye, so the four entity passes and the translucent terrain pass after it all drew untextured, and
  the sea is the biggest surface in the frame. It now calls `applyWorldState()` and re-clears the
  cull mode. **Rule worth keeping: a mid-eye pass that changes GPU state restates all of it through
  `applyWorldState` rather than undoing its own edits** -- `drawSelection` and `drawCrosshair`
  restore nothing only because they are the last two passes in the eye. `drawPrimedTnt` was the
  only pass clobbering the combiner ahead of the translucent terrain, so nothing else carried the
  same fault. See status.md §31.
- **`MobSystem::detonate`'s middle phase became `entity::applyBlast`**, so a creeper and TNT share
  `je`'s "hurt everything in reach while the blocks are still standing" rather than having two
  copies of it. A creeper excludes itself; TNT excludes nobody, its exploder argument being null.
- **Saved.** `PrimedTnt` joins the level.dat pools, added without a `Version` bump on the same
  argument the animals were. `Fuse` is a byte, as `jd.a(Lhm;)V` writes it.
- **No new sound samples.** `random.fuse` and `random.explode` were already preloaded for the
  creeper, so `ctr::kMaxSamples` is unchanged.
- New: `tests/primed_tnt_test.cpp` (twelve cases), plus a fuse through `entity_persistence`. 3DSX
  built. **No hardware run**, and it owes one for the same reason the creeper does: a 1,352-ray
  blast now has a second caller and TNT comes in walls.

## Sneaking is a toggle, it lowers the camera, and it slows you down

Y no longer has to be held. The state lives in the world loop in
`platform/ctr/main.cpp` (`sneaking`, beside `flying`), flips on a Y press, and is handed
to `readBodyInput` instead of being read off the button — the physics is untouched,
`PlayerInput::sneak` still means what it meant.

- **Y keeps its other three jobs.** Spectator's descent, Creative flight's descent and a
  rider's dismount all still own the button; the toggle is suppressed there and the same
  three also stand the player back up, so a crouch cannot outlive the button that turns
  it off. `SELECT + Y` is the page cycle, so a held SELECT skips the press too.
- **The camera drops 0.08 and the save file does not.** New `PlayerBody::cameraEyeY`,
  `kSneakEyeHeight` / `kSneakEyeDrop`. `eyeY()` — the physics' `posY`, and `Pos[1]` — is
  deliberately unchanged, so a world saved while crouching does not reload 0.08 lower.
  `placeBodyAtEye` adds the drop back before converting a camera to feet, for the same
  reason.
- **The number is 1.8.9's, not a1.1.2's and not the Betas'.** a1.1.2's `isSneaking` is a
  hardcoded false, and b1.6.2/b1.8.1 leave `yOffset` at 1.62 while sneaking and lower
  only the *model*, by 0.125, in `RenderPlayer`. The eye drop is release-era:
  `EntityPlayer.getEyeHeight` does `f = 1.62F; if (isSneaking()) f -= 0.08F`, so the
  crouched eye is `1.62f - 0.08f`, which is exactly `1.54f`. Derivation and the dead ends
  are in the note on `kSneakEyeDrop`.
- **The slowdown, unlike the camera, is a1.1.2's own** — it was simply missing here.
  `MovementInputFromOptions.updatePlayerMoveState` (`gd.a(dm)`) scales `moveStrafe` and
  `moveForward` by `0.3D` the moment the sneak key reads true, and it never consults
  `isSneaking`, which is why that half of the crouch works in the jar while the stance,
  the walk-back and the silent step do not. New `applySneakSlowdown` +
  `kSneakMoveScale`, called from `readBodyInput` because that is the layer the jar puts
  it in — `PlayerBody` scaling its own input would disagree with the generated vectors,
  which drive `moveEntityWithHeading` directly. Transcript in
  `docs/physics-a1.1.2.md` § *Sneaking scales the stick*.
- Covered by `sneaking_lowers_the_camera_and_not_the_saved_position` and
  `sneaking_scales_the_stick_and_slows_the_walk` in `tests/player_body_test.cpp`. The
  second checks the exact float the double round trip produces and that twenty ticks of
  held stick cover three tenths the ground. Not run on hardware.

## All twelve particles

`docs/status.md` §29 is the write-up. The pool held one kind (`iw`, the broken-block
fleck); it now holds all twelve `nq` subclasses a1.1.2 constructs, and most of them are
thrown by a loop that did not exist here at all.

- **`cn.m(III)V` is where the particles come from.** A thousand darts a tick at a
  33-block cube round the player, each asking the block it hits for a display tick --
  new `src/core/tick/display.{hpp,cpp}`, seven blocks answering. Its offsets come off
  the *world's* generator, as the jar's do, so the block-tick stream shifts with it.
- **`World.spawnParticle` is a seam on `TickWorld`** (`setParticleSink` /
  `spawnParticle`, kind as an int so `core/tick` need not include the pool). Grep
  `spawnParticle` to find every thrower. A harness that installs no sink runs the full
  logic and draws nothing, which is what the host tests and `--spawns` get.
- **Three sheets and three draws**, as `EffectRenderer` has three lists:
  `particles.png` (new 128 x 128 texture, 64 KB, `texture/particle_sheet` + a generated
  stand-in), `terrain.png`, `gui/items.png`. All three spans are built before any is
  drawn -- the `drawItemEntities` trap.
- **Two kinds are spawned by nothing and both say why in the header**: `Rain`, whose
  gate `Minecraft.J` is assigned nowhere in the jar, and `addBlockHit`, which needs
  break progress this build does not have.
- **Fixed while rewriting the mesh:** `buildParticles` mirrored every sprite
  horizontally (high u on the `-right` corners). Invisible on a chip of stone, not on a
  flame.
- **Two jar faults copied deliberately**: the flame's six dead `nextFloat` draws, and
  the glowing ore glittering on five faces rather than six. Both named in the code.
- **Two sounds joined `preloadEffects`** -- `liquid.water` and `fire.fire`, both reached
  only through the display tick. `--audio-list` now decodes **110 samples / 7,017.8 KB**
  against `kMaxSamples` 128.

## A Creative player is not something a monster hunts

`docs/status.md` §28 is the write-up. New `entity::MobPlayer::targetable` and
`item::Attacker::provokes`; both default to the old behaviour.

- **The rule is "invulnerable is not prey", not "Creative is special".** a1.1.2
  has no gamemode to ask about, so this copies the later versions' derivation:
  targeting goes through `TargetingConditions.forCombat`, which refuses anybody
  who `isInvulnerable()`.
- **Four refusals, not one.** `findTarget` covers zombie, skeleton, creeper and
  spider (a creeper that cannot acquire cannot light). The target-maintenance
  `else` covers a mode switch mid-chase. `hopAbout` and the slime's
  `onCollideWithPlayer` had to be told separately -- `ma` is not an `ek` and
  never reaches `findPlayerToAttack`. `MobSystem::attack`'s `provokes` covers
  `dq.a(Lkh;I)Z`, so a Creative punch lands in full and leaves no grudge.
- **`present` stays true.** Despawn by distance, the head that turns to watch,
  and the shove all still see the player. Spectator is left targetable: it has
  no body, so a mob that reaches it reaches nothing.

## Where the monsters actually go, and the chunk order that decided it

`docs/status.md` §27 is the write-up. Reported as "I still did not see any enemy spawn at night
on the surface" after §26.

- **`az` iterates a `HashSet<ol>` and the order is observable.** The pass `return`s on the first
  drawn point that is not air, and `k`'s `rand(rand(120) + 8)` lands inside rock better than
  nineteen times in twenty -- measured, **17,986 of 18,000 passes ended on their first or second
  chunk**. So whichever chunks the set hands over first are the only part of the 9 x 9 that ever
  spawns anything. Iterating row-major pinned every monster north of the player: 52 spawns in a
  measured run, **none at all in the southern third**.
- **Fixed by deriving the order.** `ol.hashCode()` is `(x << 8) | z` -- a hash so bad that for
  negative z the sign bits swallow the x half whole -- and `HashMap` buckets on
  `(h ^ (h >>> 16)) & 127`, 81 entries settling at a table of 128 that `clear()` keeps. New
  `entity::eligibleOrder`, pinned by tests against **real JVM output**. Bucket partition and
  bucket order are exact; Java hoists one entry per treeified (8+) bucket via `moveRootToFront`
  and this does not, which is stated rather than emulated.
- **The surface is quiet because `az` is, not because anything is broken.** Measured over 30,000
  held-at-midnight ticks of a real generated world: 806 monsters, **14 of them above y 64**.
  98.9% of drawn positions have no floor -- `gy` never moves, so a surface spawn needs the y draw
  to land on the local ground exactly, where a cave draw is already standing in one.
- **New `--spawns` host harness and a spawner row on the Info page**, because a fixture world is
  a floor and some air and reports a spawn rate no real world has.

## Three fixes on top of the hostile mobs

`docs/status.md` §26 is the write-up.

- **No monster ever spawned on the console, and every host test passed.**
  `getCanSpawnHere` runs after the entity is built (it must -- `ma.a()Z` reads
  the size its constructor drew), so the candidate was in the pool when
  `ge.a()Z` asked the world for colliding boxes, and **found its own**.
  `TickWorld::anyEntityIn` is a predicate with no identity, and the frame
  loop's implementation of it walks the same mob pool. The suite missed it
  because **a bare `SceneWorld` sets no entity query at all**, so the predicate
  answered false for everything -- an unset seam is an untested seam. Fixed
  with `NotYetInTheWorld`, a guard *inside* `canMonsterSpawnAt`, because a
  contract a caller can forget is one that gets forgotten. New fixture hook
  `Cave::watchEntities()`; verified the test fails with the guard removed.
- **The two music discs were outside the item table** -- ids 2256/2257, 1,910
  past the end of the item run -- so §25 shipped a drop with no icon, no name
  and no palette row. They get a two-entry side table (`kOutsideItems`,
  generated from the `outside` list `configure.py` already had) and `item::def`
  falls back to it; the palette is **149 items, not 147**. `genref` names them
  `record_13`/`record_cat` rather than `item_2256` -- `lg`'s second argument is
  the *record* name, not an item name -- and the data matches what it would
  emit.
- **The Creative tab is "Items" and the inventory is "Inventory".** The palette
  was blocks-only when it was named "Blocks" and has been the whole item table
  since M3 step 3. The `PlayerPage` members are unchanged; these are labels.

## The hostile mobs are in: zombie, skeleton, creeper, spider, slime

`docs/mobs-a1.1.2.md` §*The five hostiles* is the derivation; `docs/status.md` §25 is the write-up.
The short version:

- **Nine rows, not five plus four.** `MobDef` gained six columns (`ai`, `hostile`,
  `attackStrength`, `moveSpeed`, `burnsInSunlight`, `talkInterval`) and the monsters are five more
  rows of the same table. `hostile` is a column and not `type >= Zombie` because `co` is an
  *interface*: the 200-cap spawner counts the slime, which is a `ge` and not an `ek`.
- **`ek`'s attack branch was dead and is now live.** It was left out when the animals landed with
  the reason written down, and it is the half of `updatePlayerActionState` only monsters use: the
  target and wander branches are exclusive, and `hasAttacked` turns the walk into a sidestep about
  the target's heading -- which is why a skeleton circles rather than marching in.
- **`getBlockPathWeight` is one method with the sign reversed.** `0.5 - brightness` for a monster
  against grass-or-brightness for an animal, and through `ek.a()`'s `weight >= 0` it is also the
  spawn refusal. That is the whole of "monsters keep to the dark".
- **New: `core/entity/explosion.{hpp,cpp}`** -- `je`, all three phases, with the cell record as a
  616-byte bitset on the caller's stack. It is in `core/entity/` because it needs `dropBlockAsItem`
  from `tick` and `rayTraceBlocks` from `entity`, and the dependency already runs that way.
  `getExplosionResistance` is `max(resistance * 3, hardness * 5) / 5`, derived and checked against
  all seventy block rows.
- **New: a difficulty**, in `<world>/3dalpha.ini` beside the gamemode with a World Settings row.
  a1.1.2's own setting (`cn.l`). **Peaceful is a removal, not a suppression**: `dq.e_()` kills the
  monster at the end of the tick it notices, so the spawner still runs and still costs what it
  costs.
- **The player has no health**, so a fist leaves through `MobSurroundings::hurtPlayer` and
  `platform/ctr/main.cpp` takes the two parts that exist -- knockback and `random.hurt` -- behind
  `ge`'s ten-tick window, and counts the damage. Survival is M3 step 4.
- **Arrows reach mobs and the player now**, which is the other half of the bow landing. An arrow
  carries who fired it, because `dd.b(Lkh;)V` drops a **music disc** when a *skeleton* kills a
  creeper -- the only source of a record in this version.
- **Three things only the bytecode says.** `World.getBlockDensity` decompiles as integer division
  and is not (`i2f` on both operands). A slime splits at `health == 0` *exactly*, so one overshot
  past zero does not. `cu.a(J)`'s slime-chunk hash wraps at 32 bits three products out of four.
- **The entity sheet grew 256 x 128 -> 256 x 256** (seventeen pages needed, sixteen fit; both
  dimensions must be powers of two). Every existing page kept its origin; still one draw call.
  `Placement` gained a per-axis scale for the creeper's swell and the slime's size.
- **`ctr::kMaxSamples` 80 -> 128, re-measured**: `--audio-list` gives **108 samples / 3,386,798
  frames / 6,638.9 KB** against the real folder, up from 72 / 3,500.8 KB. The monsters cost half as
  much again as everything before them; the cap is now half the 16 MB non-mesh reserve rather than
  a quarter.
- New: `tests/monster_test.cpp` (24 cases), plus three monsters through `entity_persistence`.
  Suite **1248/1248**; new cases plus `entity_persistence`, `housekeeping` and `sound` clean under
  TSan; 3DSX built. **No hardware run**, and this owes one more than anything before it: two
  hundred models and a 1,352-ray explosion on a 268 MHz ARM11 is a real question.

## The entity sounds, and the boot list that was swallowing half of them

`docs/audio-a1.1.2.md` §*What an entity plays* is the derivation; `docs/status.md` §24 is the
write-up. Two separate faults, and the second is the one that was invisible:

- **Three entity sounds had no emitter.** `random.splash` (`kh.y()`'s water entry),
  `random.fizz` for a stack landing in lava and for a burning animal reaching water. All now play
  through `TickWorld::playSoundAt`, the seam the pressure plate's click already used.
- **Two more had an emitter that could never be heard.** A key that was never preloaded is
  silence, deliberately -- and the boot list lived in `platform/ctr/main.cpp` as six literals, so
  **`random.bow` has been mute since the bow landed and so has every one of the animals' eight
  `mob.*` keys.** The list is `audio::preloadEffects` now
  (`core/audio/effect_preload.{hpp,cpp}`), half of it derived from the block table and `MobDef`,
  and shared with the host harness. `a_key_that_is_played_is_a_key_that_is_preloaded` compares the
  two lists, which is the comparison nobody was making.
- **`ctr::kMaxSamples` 48 → 80, measured not guessed.** `--audio-list <resources>` decodes the
  whole boot set on the host as the console does at boot: **72 samples, 1,780,061 frames,
  3,500.8 KB** against the real folder on this machine. 48 was chosen off the block table alone
  and against an a1.1.2-era resources tree, which no player has -- a modern one carries eight
  `step.grass` variants where the old one carried six.
- **`random.drr` was playing at the player, not the arrow.** It was a count the frame loop read
  back through `ArrowSystem::struckLastTick()`, so an arrow landing forty blocks away sounded like
  one at your feet. It plays from `core/entity/arrow.cpp` now and that accessor is gone.
- **Who splashes is derived, not chosen.** `kh.e_()` is one line, `y()`, so an entity splashes
  exactly when its own `onUpdate` calls `super.onUpdate()`: the player, the four animals, dropped
  items, arrows and boats do. **The falling block, the painting and the minecart do not** and
  enter water in silence -- a1.1.2's, not a gap. `core/entity/water_entry.hpp` is the shared edge
  detector (`inWater` and `firstUpdate`, both needed).
- Measured and kept: a floating boat flickers in and out of `g_()`'s inverted probe, **3 splashes
  in 240 ticks at volume ~0.03**. Faithful and inaudible, because the volume is the motion.
- New: `tests/entity_sound_test.cpp` (8 cases) and `tests/sound_catcher.hpp`, the mirror of
  `drop_catcher.hpp`. Suite **1219/1219**; new cases plus `entity_persistence`, `housekeeping` and
  `sound` clean under TSan; 3DSX built. **No hardware run**, which this owes more than most.

## The peaceful animals are in: pig, sheep, cow, chicken

They spawn (and only by a1.1.2's own spawner -- this version generates none with the world),
wander along a real path, take hits, drop, are milked, saddled, ridden, sheared, lay eggs,
despawn, save and reload. `docs/mobs-a1.1.2.md` is the derivation; `docs/status.md` 23 is the
write-up. The short version:

- **One struct and a table**, not four classes: `core/entity/mob.{hpp,cpp}` is `ge`/`ek`/`ag` once
  with a four-row `MobDef`. `core/entity/mob_spawn.{hpp,cpp}` is `az`, `core/entity/path_finder.
  {hpp,cpp}` is `cz`, `core/render/mob_mesh.{hpp,cpp}` is the five models and `dn`'s transform.
- **`PlayerBody` is `EntityLiving`'s body now.** `width`, `height`, `yOffset` and `stepHeight`
  became fields with the player's values as defaults, and `LivingBody` is an alias. The 22-case
  bit-exact player oracle passes unchanged. A mob's `yOffset` is 0, so its `posY` is its feet.
- **One draw call and one bind** for every animal: the entity sheet grew 256 x 64 -> 256 x 128 and
  the six new pages sit below the five that were there, which kept every existing UV. A fleeced
  sheep and a saddled pig are two models each.
- **No breeding, no babies and no way to keep one**, because this version has none of the three.
  An earlier pass in this session had ported 1.1's rules (wheat, love ticks, a half-size calf) plus
  an invented `persistent` flag; **that has all been removed** on the user's instruction to hold
  parity. What is left is a1.1.2's own behaviour: wheat is refused, every animal is full size, and
  **every animal despawns** with no exception a player can buy.
- **Two a1.1.2 bugs, treated differently:** the pathfinder's dead size loop is kept (it is what the
  search explores), its colliding node hash is fixed (it is a hash written as an identity).
- **Saved in `Data/3DAlphaEntities`**, a new `mobs` list beside the other six pools with no version
  bump -- an older build skips it and a newer one reads a file without it as "no animals". Native
  chunk `Entities` are still preserved and unread, so animals do not survive a trip back to the
  real client.
- Measured: `sizeof(Mob)` 680 bytes, fifteen animals 10.0 KB, path arena 32 KB taken once, at most
  4,608 vertices (72 KB) a frame.
- **Two bugs the tests caught:** knockback pushed animals *towards* the fist (the jar's vector
  runs mob-to-attacker and is subtracted), and a death by lava, drowning, suffocation or the void
  dropped nothing, because those are `attackEntityFrom(null, n)` in the jar and were subtracting
  health directly here.
- **Not done:** monsters, the death puff (no smoke particle exists in this build), and the
  `entitydata` slot. Suite 1210/1210 (ASan/UBSan), `entity_persistence`/`housekeeping`/animals
  clean under TSan, 3DSX built, **no hardware run**.

## A door's texture now shows which side its hinge is on

The second door of a pair used to look hinged on the wrong side. `ItemDoor` writes it as the facing a
quarter back with bit 2 set. That is the same box as a plain closed door, so the hinge shows only in
`BlockDoor.getBlockTexture` (`fw.a(II)I`) answering a negative tile, which means "reverse u".
`addDoor` (core/mesh/shapes) ported the tile but dropped the sign. It now ports the whole method per
face, and `addBox` takes a `mirrorMask` that swaps a face's u ends, as `bc.c`–`bc.f` do with
`flipTexture`. That also puts the lower-half tile on the top, bottom and thin edges of the upper
half, as the jar does.
- `a_door_draws_each_face_with_the_tile_and_mirror_the_jar_answers` holds the jar's answer for both
  doors, every face, all 16 metadata values. It came from a scratch reflection harness over the
  real jar.
- **Open, not changed:** `renderBlockDoor` shades faces 0.5/1.0/0.8/0.6. `addDoor` still draws them
  unshaded (`kFlat`).
- Suite 1185/1185, 3DSX built, no hardware run.

## Item-sheet sprites were never uploaded at launch

Swords, paintings, signs, doors, minecarts, and in fact every `gui/items.png` sprite, drew nothing
in the hand. `Renderer::init` uploaded the block atlas and the entity sheets but not the item sheet.
Only `setAtlas`, which runs on a texture-pack change, called `Atlas::initItems`. So
`atlas_.hasItems()` stayed false and `drawHeldItem` (and `drawItemEntities`, and the compass tile)
returned early. `init` now calls `initItems` too. Found by ruling things out: the core mesh
(`buildHeldItem`) is identical for failing and working items (264 vertices, same sheet, same
place), and the texels are ordinary 0/255 alpha. 3DSX built, no hardware run.

**Open, not changed:** the hand's 5:3 shift (`kOriginalAspect = 4/3`, held_item.hpp) assumes
a1.1.2 framed the hand for 4:3. `Minecraft.class` opens at **854 × 480** (16:9). Against that
reference the shift should be 0.035 of a block inward, not 0.14 outward. As built, a software
render shows most of each sprite past the right edge; a held sword keeps little more than its handle.

## Teleport has no height range; cacti have spikes

- **Teleport Y** is bounded only by ±32,000,000 (`kCameraYLimit`, core/util/coord_text), which
  exists so the `int(camera.y)` readouts stay defined. Spectator's flight clamps to the same
  value, not 1..254. Block lookups and the visibility walk already handle heights outside the
  column.
- **Teleport now works with a body.** Survival/Creative overwrite the camera from the body at the
  end of every frame, so a teleport used to be undone before it was drawn. main.cpp now spots the
  camera moving inside `overlay.handleInput`, dismounts any vehicle, and calls `placeBodyAtEye`.
  That helper is shared with the Spectator→body hand-over.
- **Cactus spikes** (`addCactus`, core/mesh/shapes): `bc.b(ly,IIIFFF)` draws each side as the
  full-cell face under `setTranslation(±0.0625)`. The spikes are the tile's alpha-tested edge
  columns. We used to narrow the sides to the inset box, which cropped those columns off. Still
  six quads, so there is nothing to distance-limit. The icon, dropped and held cactus took the
  inset box from `block::renderBoxes` until the section above gave them `bc.a(ly)`'s shape too.
- Suite 1184/1184, 3DSX built, no hardware run.

## Greedy meshing is in, through a cube atlas and a seam

Equal neighbouring cube faces are one quad, with runs of up to 3×3. That is the same plane, face,
tile and light. The toggle is `MeshBuilder::setGreedy`, on by default, and there is a debug-page
row.
- **Cube atlas** (`core/mesh/cube_atlas.hpp`): the cube pass samples a 512×512 texture (1 MB,
  VRAM first). Each of the 54 cube tiles has a 64×64 slot: an 8-texel gutter of its own edge
  texels, three copies, another gutter. The PICA cannot repeat one tile of a shared atlas. Other
  passes keep terrain.png. `Atlas::bindCube` is bound around the cube pass only.
- **Seam**: merged quads grow ⅛ px in the vertex shaders to cover T-junction cracks from the
  rasteriser's 1/16-px snapping.
  - `WorldVertex::face` is now `seam` (face + 6·corner code); use `mesh::faceOf`.
  - `QuadVertex` carries `slotX/slotY` and `extent` (`w + 16h`) where `tileX/tileY/ao` were.
- **Hardware, first layout (runs of 4, no gutter):** 22 ms GPU / 25 ms CPU with greedy on, against
  34 / 44 off. It also bled neighbouring-slot texels (red TNT on grass corners), which is what the
  gutter fixes. The gutter layout has not been on a console yet.
- Real world on the host: 2,135,496 → 1,057,821 cube quads; 12-byte mesh memory 107 → 58 MB; mesh
  time +2 %.
- Details: status.md §22. Suite 1182/1182 (ASan/UBSan), 3DSX built. Shaders checked with a scratch
  PICA interpreter on the assembled `.shbin`s.

## The crosshair's entity pick is `getMouseOver`'s now

Entity hitboxes felt too big, and a rail could not go back under a cart. The crosshair borrowed
the arrow's 0.3 border and ignored distance. `item::pickEntity` (core/item/use) now follows
`iq.a(F)`:
- a 0.1 border;
- three blocks at most, and nearer than the block hit;
- the nearest intercept across paintings, boats and carts.

It returns an `EntityTarget`, and `attackEntity` / `interactWithEntity` act on it. The outline
clears while an entity takes the crosshair. L on a refusing entity no longer clicks the block
behind it. Details: status.md §21, physics-a1.1.2.md. Suite 1169/1169, 3DSX built, no hardware
run.

## Furnaces and stairs face their neighbours, and a furnace draws its mouth where it faces

a1.1.2 has no `onBlockPlacedBy`: nothing faces the player, and logs have no orientation. The user
chose faithful behaviour over a heading rule. Fixed underneath it:
- The mesher drew every furnace's mouth on +Z. It now uses `metadataFaces` via
  `block::worldFaces`, and furnaces are not `unitCube`. The lit mouth is tile 61.
- Furnace and stairs orient in `tick::blockAdded` (`ku.h`, `km.h`), not the face table.
  Only the six `onBlockPlaced` classes have table rows now, listed by the `--place` sweep.
- Stairs re-shape their flight, and turn into their `model` block under anything solid.

Not done: the chest (neighbour-dependent texture). Details: status.md §20. Suite 1163/1163,
3DSX built, no hardware run.

## A sign whose support is broken now goes

`signNeighbourChanged` already turned the block to air, but only the player's break path erased
the `SignStore` entry, and a sign is drawn from the store. The fix is a1.1.2's `jt.b`
(BlockContainer.onBlockRemoval → `World.removeBlockTileEntity`): `blockRemoved` calls
`TickWorld::removeTileEntity` for both sign behaviours, and `runGame` wires that seam to
`SignStore::erase`. The break path's own `erase` is gone. Covered in `tests/sign_test.cpp`.
Suite 1155/1155, 3DSX built, no hardware run.

## A minecart stack no longer overflows to NaN; a NaN entity no longer bricks a save

Carts in contact compound motion by 1.2 per collision (a1.1.2's own `oc.f(kh)`), overflowing to
NaN in about 16 s of a ridden stack. That broke the cart, its rider, the map marker and the save.
- `collideCarts` clamps each axis to `kMinecartMotionLimit` (10, the port's bound; see minecart.hpp).
- Entity readers now **drop** an entity with an impossible value instead of failing `level.dat`.
- `decodeData` puts a player with a non-finite `Pos` back at spawn and keeps the inventory.

Details: status.md §19. Suite 1151/1151 (ASan/UBSan), 3DSX built, no hardware run.

## Entity pools have no cap

Every entity pool (arrows, carts, boats, paintings, items, falling blocks, particles, signs) is a
`SegmentedPool` (`core/util/segmented_pool.hpp`), as a1.1.2 caps none of them.
- **The old number is `kInitialCapacity`**, taken at construction, so ordinary play never allocates.
- **Growth** uses `malloc` a segment at a time, and only while `poolGrowthAllowed`
  (`core/util/memory.hpp`) leaves 8 MB plus three save copies of every pool byte.
- **A refusal takes the old full-array path.** Elements never move; `trim` runs at the end of each tick.
- **Saves:** `SavedPool` is unbounded, and a capture that fails keeps the previous snapshot.
- **Drawing:** the vertex buffers are still fixed, but past them the **nearest** are drawn
  (`core/render/draw_budget.hpp`). Items and signs share one cutoff across their two passes.
- **Still capped (not entities):** the tick scheduler (4096), the light queue (2048), and the
  torch/notify tables.

**A refusal is now said on the top screen.** "The maximum number of Minecarts in a world has been
reached." appears in a1.1.2's own chat overlay (`lu`), which is the look LCE kept.
- `core/gui/chat_log`, `core/render/chat_mesh` and `Renderer::drawChat` draw it; it needs the pack font.
- `item::markRefusals`/`refusedSince` detect the refusal.
- `useSign` no longer leaves an invisible block when the store refuses.
- Sign text is drawn upright on the board's front (it was upside down, under and behind the board).
  `in` pops the model's `(f, -f, -f)` scale before the text, and `buildSignText` had treated
  the text as if it were still inside that scale. Covered by `text_is_upright_on_the_front_of_the_board`.
- **Mob memory:** entities are 80–176 bytes with nothing per-type duplicated, and a1.1.2 caps mob
  spawning at 200 monsters and 15 animals. Per-frame model rebuilding is the mob cost to plan for.
  See status.md §18.

Details: status.md §18. Suite 1149/1149 (ASan/UBSan), 3DSX built, no hardware run: the chat pass
has never been seen on a console.


`ArrowSystem::tick(world, ArrowTargets{paintings, boats, minecarts})` runs one nearest-target
sweep over all three pools. Each is collidable per its `c_()` in the jar. Every hit is
`attackEntityFrom(4)`: a painting dies at once, and a boat or cart breaks to a second arrow.
`ItemEntitySystem` now **replaces the oldest item when full** instead of refusing the newest.
Only `dropFromPlayer` still refuses, since the item stays in the hand. The old refusal is the
inferred cause of "carts drop nothing when killed by arrows": in Creative, block drops fill the
64 slots. Core was shown to drop the cart correctly. Details: status.md §17. Suite 1117/1117,
3DSX built, no hardware run.

## Breaking entities, and the crop's seeds

A play-session list of nine "won't drop / won't break" items was two faults. **R now attacks the
entity under the crosshair before the block behind it** (`item::attackEntity`, core/item/use.hpp):
boats and carts take `attackEntityFrom` damage (x10, break past 40, five bare-handed hits) and drop
3 planks + 2 sticks / item 328 plus chest or furnace; a painting dies in one hit. A painting whose
wall is mined now drops item 321 too. And `item::destroyBlock` now calls **`onBlockDestroyedByPlayer`**
(`tick::blockDestroyedByPlayer`, core/tick/drop.hpp), whose one visible override is the crop's
three seed rolls. Pools drop through `TickWorld::spawnItem`; the boat's `Wreck` list and `runGame`'s
drain loop are gone. Door, iron door, redstone torch, pressure plate and sign were **measured
correct** already. Hand damage is fixed at 1 (no `damageVsEntity` column yet). Details: status.md
§16. Suite 1112/1112, 3DSX built, no hardware run.

## Entity persistence

Six pools now save: paintings, arrows, boats, minecarts, dropped items, falling blocks.
Implementation: `src/core/entity/persistence.{hpp,cpp}`. Integration:
`WorldStreamer::bindEntities` / `snapshotEntities` → `ChunkCache::PlayerState::entities`
→ `ChunkCache::applyPlayerState` → `alpha::encodeLevelDat` / `decodeLevelDat`.
3DS `runGame` binds the pools before ticking them. Snapshot ownership is immutable across I/O;
pools must outlive `WorldStreamer::close`. Autosave, pause and close capture state, including
empty pools. Entity-only changes do not dirty block columns. Falling blocks now wait for
resident chunks after load. Reopening a streamer resets its pending state.

**Compatibility limit:** `Data/3DAlphaEntities`, version 1, is a port-specific level.dat
compound, also carried in packed manifests. Native Java chunk `Entities` remain opaque and
preserved; they are neither imported into the pools nor updated from them. Sign text persistence
and restoring vehicle rider attachment remain open. This is not completion of the entitydata slot.

## The held item

`core/render/held_item.{hpp,cpp}` is `ItemRenderer` (`jh`): `HeldItemState` is the per-tick equip
animation and arm swing, `buildHeldItem` is `renderItemInFirstPerson` + `renderItem` as camera-space
`DetailVertex` quads. `Renderer::drawHeldItem` draws it last in each eye with the **projection
alone** -- no view matrix, because the original resets the modelview to identity for `renderHand`.
`main.cpp` ticks `hand` at the top of the per-tick loop, swings it where `Minecraft.clickMouse`
does (break always; place only when the click was taken; `onItemRightClick` re-equips instead), and
calls `setHeldItem` once a frame -- **after the tick loop**, unlike every other `renderer.set*`,
because a five-tick animation cannot afford the frame of latency the entity pools do not notice.
Spectator calls `clearHeldItem()`, which is **not** the same as item 0: item 0
is an empty hand and draws the player's right arm (`bu.b()`, one `ModelBiped` box through the
existing `buildBox`).

`char.png` is a pack file like any other, so it is now a **fifth page of the entity sheet**, which
grew 128 x 64 -> 256 x 64 (a fifth 64 x 32 page does not fit, and 96 is not a power of two). The
four existing pages kept their offsets, so no model's UVs moved. **A pack with no skin gets an
opaque black arm**, not a Dev Art grid -- a silhouette is honest, an orange forearm reads as a bug.

**The hand was invisible on hardware and it was a GPU-state bug, now fixed.** `drawSelection` and
`drawCrosshair` deliberately restore nothing, on the argument that `applyWorldState` restates
everything before each eye -- true only while they are *last* in the eye. `drawHeldItem` runs after
them and inherited a combiner that replaces alpha with the vertex's own, the alpha test off, and
src-alpha blending; the detail shader puts the **fog amount** in that alpha, which this close to the
eye is zero. `drawHeldItem` now calls `applyWorldState()` itself. **Any future pass added after the
crosshair has to do the same.**

## Skins

**Options → Skin**: Default (the active pack's `char.png`, or black), every pack carrying one, and
every `.png` in `sdmc:/3dalpha/skins` -- created when the screen is first opened.
`core/texture/skin_list.{hpp,cpp}` is the list; `texture::applyPlayerSkin` overrides the sheet's
player page; `GameSettings::skin` persists it as **a name, not a path or an index**
(`pack:<name>` / `file:<name.png>`), so `ensureAtlas` applies it at boot without listing the packs
folder. Picking a skin **rebuilds the atlas and then overrides** -- patching alone cannot return to
Default, which only `buildEntitySkins` can produce. 64 x 64 skins keep their top half.

**Slim is detected and deliberately not drawn.** `versions/<id>.json` gained `hasSlimSkins` (false
here); `held_item.cpp` carries a `static_assert` on it so that turning it on **fails the build**
until the narrow arm's `ModelBiped` box is derived from the jar of the version that added it. Do not
replace that assert with a remembered box.

Three constants are the console's, not a1.1.2's, and each is argued in place: a 0.14-block sideways
shift so a 5:3 screen frames it like the 4:3 window it was designed for; its own 0.05 near plane
(the world's 0.2 clips a corner that reaches 0.193); and `C3D_DepthMap(true, -0.05, 0.95)` standing
in for the `glClear(GL_DEPTH_BUFFER_BIT)` citro3d has no mid-frame equivalent of -- **restore it to
(-1, 0) after any pass that changes it**. Stereo gets its own focal (0.72) and a sixteenth of the
world's separation, because the item is nearer than the eyes are apart; that sixteenth is arithmetic
and **has not been seen on hardware**. `tests/held_item_test.cpp` covers it; suite 1081/1081, 3DSX
built.

## Hardware save crash — cause found, previous verdict corrected

Dumps 11 and 12 are archived as `crashlogs/009-save-with-a-minecart/` with the
full reading in its `NOTES.md`. **They are a use-after-free of
`tick::TickWorld`, not a command-buffer overrun.** `pc == r0 == r3` points into
newlib's `__malloc_av_` — provably, from the self-referential word pairs Luma
dumped, since neither dump has its ELF — which is the `fd`/`bk` of a freed
chunk read as `TickAccess`'s context and function pointer. `r1`/`r2` are chunk
(-10, -4) and `r5`/`r6`/`r7` the block (-152, 70, -56) inside it.

`WorldStreamer::close` destroyed `tick_` before the drain loop, and that loop's
progress callback draws a whole frame per pumped write; `Renderer::drawMinecarts`
borrows the tick world (a cart leans along the track). So every frame of the
"Saving level.." screen read a freed object, which is why it only ever fired
while saving and only with a cart in the world.

Fixed by releasing `tick_`/`light_` **after** the drain, plus a
`renderer.setMinecarts(nullptr, nullptr)` hand-back in `runGame` once `close()`
returns. `tests/streamer_progress_test.cpp` →
`the_save_progress_callback_can_still_read_the_world` reproduces it: against the
old ordering, 49 of 49 progress callbacks see no world.

The earlier session's `.bss`/command-buffer reading was wrong. Its 64K-word late
-pass reserve and the crosshair's own linear buffer were kept (harmless, and
tidier). Its **white outline shader was reverted**: it had made a1.1.2's
`glColor4f(0, 0, 0, 0.4)` selection box white in order to make the crosshair
visible. Both colours now come off a `tint` uniform in `shaders/outline.v.pica`,
so the outline is the original's black again and the crosshair is opaque white.

Host suite 1066/1066; the 3DSX build passed. **No hardware run** — the save
retry is still owed.

## Dungeons after the null-side-effects tweak

`ChunkGenerator::populate` now passes `nullptr` for `PopulationSideEffects`
instead of building records it discarded, and drops the `droppedChests` /
`droppedSpawners` counters. **Behaviour-identical and worth having** -- those
records were already thrown away (chunk tile entities round-trip as an opaque
blob), so this is an allocation removed from the generation worker.

The risk in it is not the dropped data, it is the RNG stream: population reads
one stream and clay, seven ore passes, lakes and trees all follow the dungeons
in it, so a single draw skipped under a null check would shift every later
feature and nothing would look wrong enough to notice. `generateDungeon`'s
`out != nullptr` guards wrap only the recording -- every position roll, loot
roll, slot draw and mob draw is outside them.

Checked rather than read: `tests/dungeon_test.cpp` →
`a_dungeon_generated_with_no_output_draws_and_writes_the_same` runs the whole
jar fixture a second time with no output struct and asserts identical blocks
plus a post-generation draw equal to both the recorded run's and the fixture's
own `afterDraw`. Verified to bite -- making one mob draw conditional fails it.

## Minecart placement and the crosshair, checked rather than assumed

Both were reported as missing on hardware. **Neither is missing from this
working tree, and both are absent from `HEAD`** — `src/core/item/use.cpp`, which
holds the minecart dispatch, is untracked, and `drawCrosshair` does not exist in
the last commit. A card still carrying a build from before those sessions shows
exactly the reported symptoms, including the save crash.

Placement is now covered end to end rather than at the item seam only.
`tests/minecart_test.cpp` →
`the_crosshair_finds_a_rail_and_the_cart_goes_on_it` traces a ray at a rail from
an ordinary standing eye and feeds the resulting `RayHit` to `item::rightClick`.
That closes the one link the older test skipped: a rail's selection box is an
eighth of a block tall, the thinnest shape in the selection table, and nothing
had checked that the crosshair's ray finds it. It does.

The crosshair is drawn unconditionally in `Renderer::drawEye`, after the
translucent pass, in every gamemode — nothing gates it on Creative. The `tint`
uniform (above) was one defect; it was **not** the reason the mark was invisible
on hardware. See the next section.

## The crosshair was being back-face culled

Reported again as "the crosshair doesn't show up" after the `tint` uniform
landed, and the uniform was not the cause. `drawCrosshair` builds its two quads
from `rx, rz = cos(yaw), sin(yaw)` and `up = forward x right`. That `rx, rz` is
perpendicular to the forward vector, so it is *a* horizontal basis vector — but
it is the wrong one of the two: through `Mtx_LookAt`'s basis (`s = forward x
up`, so at yaw 0 screen right is world **-X**) it is screen **left**.

The cross is symmetric about both axes, so the shape is identical either way and
nothing looked wrong — but the triangles come out clockwise in screen space,
which is back-facing. `drawEye` turns culling back on (`GPU_CULL_BACK_CCW`)
before the translucent pass and `drawCrosshair` never restated it, so the whole
pass was discarded. The selection outline shares the pipeline and survived
because a box shell presents both windings.

The convention is derivable rather than assumed: `kFaceCorner[kFaceNegZ]` is
(1,0,0), (0,0,0), (0,1,0) and `indices_` winds a quad (0,1,2),(0,2,3); mapped
through the basis above those come out counter-clockwise, which is why the world
is visible at all under `GPU_CULL_BACK_CCW`.

Fixed by stating `GPU_CULL_NONE` in the crosshair pass — a mark that always
faces the camera has no back to cull, and stating it does not depend on which
perpendicular the basis picked. Safe to leave set: `applyWorldState` restates
the cull mode before every eye. **No hardware run.**

Host suite 1069/1069 and the 3DSX build passed with this and the compass change
below in place.

## The compass no longer repaints a screen that has not changed

Reported as the compass update causing garbage pixels on the hotbar or the
inventory, depending on which the compass was in.

`CompassTexture::tick` was pushed to both consumers on every one of the 20 ticks
a second, unconditionally. One consumer is a GPU tile upload; the other is
`Overlay::setAnimatedItemsTile`, which marks the band dirty and makes
`drawPlayerPage` repaint the whole 320 x 32 hotbar — or the inventory panel —
with the CPU, into the framebuffer the LCD is scanning out of. That went on
forever: the spring converges in the eleventh digit long after the needle has
stopped moving a texel, so a settled compass in a hotbar slot was costing a full
band repaint twenty times a second for a picture already on the screen.

`tick` now returns whether the 256 texels differ from the ones it last reported,
and `runGame` pushes only then. The test is the pixels rather than a threshold
on the angle, because the needle is plotted through a truncation to int.
`setBase` forces the next tick to report, so a pack change still reaches both
consumers. Host coverage: `tests/compass_test.cpp` →
`a_settled_compass_stops_asking_to_be_pushed` and
`a_turn_asks_to_be_pushed_and_a_new_pack_does_too`.

This removes the repaint in the steady state entirely. It does **not** prove
what the garbage pixels were — a repaint while the player is actually turning
still writes a live scanout buffer — so if they survive a turn, that is still
open and wants a description of what they look like. **No hardware run.**

## Arrow and minecart follow-up

Arrow and minecart pools have no cap any more (see the first section). Arrows use Alpha's nearest expanded-AABB sweep against
minecarts, clipped by the first block hit; a hit deals the original four
damage, removes the arrow, and two immediate hits break the cart. The Alpha
arrow expiry rules remain: no air-time expiry, void removal below -64, and
exactly 1200 ticks while embedded. The mined-block release continues from the
same interpolated previous position, so it renders smoothly as it falls.
Focused host coverage: `arrow` 16/16 and `minecart` 7/7 (outside the sandbox
because its LeakSanitizer cannot run under tracing).

Minecarts now apply Alpha-style nearby-entity impulses: a player body (including
Creative flight) shoves an unmounted cart, and a moving cart transfers momentum
to the cart immediately ahead on a rail. The world view now has a centered,
untextured top-screen crosshair; the bottom Look-pad marker remains a touch
control affordance. L places and R breaks, restoring the established
controls that the last input change reversed. Minecart dispatch now precedes
generic block activation, so a rail click cannot be consumed before the item
path. Focused host coverage: `cart` 15/15 outside the sandbox and `minecart`
7/7; the 3DSX build passed. No hardware run.

## Verification evidence

After the persistence change: **1059/1059** host cases passed under ASan/UBSan;
`entity_persistence` (4 cases) and `housekeeping` (2) passed under TSan. The 3DSX cross-build
succeeded. No hardware run. Tests: `tests/entity_persistence_test.cpp`.

LeakSanitizer failed under the session sandbox's tracing; the approved outside-sandbox run
passed. Do not disable sanitizers to turn an environment failure into a reported pass.
Logs from that run, if still present: `/tmp/entity-tests.log`, `/tmp/entity-build.log`,
`/tmp/entity-tsan-build.log`, `/tmp/entity-ctr-build.log`. Temporary logs are not durable evidence.

Documentation-only follow-up: root `AGENTS.md` now routes agents to `task-map.md`, this handoff,
and bounded sections of the generated indexes; the former CLAUDE guide is in `working-guide.md`.
