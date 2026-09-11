# Current work

Last verified: 2026-09-11. A compact handoff, not a substitute for inspecting the current diff.
Replace superseded facts here; keep detailed history in `status.md`.

## Checkout context

The working tree already contained extensive tracked and untracked gameplay/rendering changes
before the entity persistence fix. These are user work, not disposable build output. Do not
infer ownership from whether a file is tracked. No commit or deployment was made in this session.
Older narrative sections and source comments may describe gaps that later changes closed;
check the implementation and tests before treating those gaps as current.

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
